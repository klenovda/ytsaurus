#include "file_writer.h"
#include "private.h"
#include "client.h"
#include "config.h"
#include "transaction.h"
#include "connection.h"
#include "private.h"

#include <yt/client/api/file_writer.h>

#include <yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/cypress_client/cypress_ypath_proxy.h>

#include <yt/ytlib/file_client/file_chunk_writer.h>
#include <yt/ytlib/file_client/file_ypath_proxy.h>

#include <yt/ytlib/object_client/object_service_proxy.h>
#include <yt/client/object_client/helpers.h>

#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/transaction_listener.h>
#include <yt/ytlib/transaction_client/config.h>

#include <yt/ytlib/hive/helpers.h>

#include <yt/client/api/transaction.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/crypto/crypto.h>

#include <yt/core/logging/log.h>

#include <yt/core/rpc/helpers.h>

#include <yt/core/ytree/convert.h>

namespace NYT::NApi::NNative {

using namespace NCrypto;
using namespace NConcurrency;
using namespace NRpc;
using namespace NYTree;
using namespace NYPath;
using namespace NYson;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NObjectClient::NProto;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NApi;
using namespace NTransactionClient;
using namespace NFileClient;
using namespace NHiveClient;

////////////////////////////////////////////////////////////////////////////////

class TFileWriter
    : public TTransactionListener
    , public IFileWriter
{
public:
    TFileWriter(
        IClientPtr client,
        const TRichYPath& path,
        const TFileWriterOptions& options)
        : Client_(client)
        , Path_(path)
        , Options_(options)
        , Config_(options.Config ? options.Config : New<TFileWriterConfig>())
        , UploadSynchronizer_(Client_->GetNativeConnection())
        , Logger(NLogging::TLogger(ApiLogger)
            .AddTag("Path: %v, TransactionId: %v",
                Path_.GetPath(),
                Options_.TransactionId))
    { }

    virtual TFuture<void> Open() override
    {
        return BIND(&TFileWriter::DoOpen, MakeStrong(this))
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetWriterInvoker())
            .Run();
    }

    virtual TFuture<void> Write(const TSharedRef& data) override
    {
        try {
            ValidateAborted();

            if (Options_.ComputeMD5 && MD5Hasher_) {
                MD5Hasher_->Append(data);
            }

            if (Writer_->Write(data)) {
                return VoidFuture;
            }

            return Writer_->GetReadyEvent();
        } catch (const std::exception& ex) {
            return MakeFuture<void>(ex);
        }
    }

    virtual TFuture<void> Close() override
    {
        return BIND(&TFileWriter::DoClose, MakeStrong(this))
            .AsyncVia(NChunkClient::TDispatcher::Get()->GetWriterInvoker())
            .Run();
    }

private:
    const IClientPtr Client_;
    const TRichYPath Path_;
    const TFileWriterOptions Options_;
    const TFileWriterConfigPtr Config_;

    TChunkUploadSynchronizer UploadSynchronizer_;

    NApi::ITransactionPtr Transaction_;
    NApi::ITransactionPtr UploadTransaction_;

    std::optional<TMD5Hasher> MD5Hasher_;

    IFileMultiChunkWriterPtr Writer_;

    TCellTag NativeCellTag_ = InvalidCellTag;
    TCellTag ExternalCellTag_ = InvalidCellTag;
    TObjectId ObjectId_;

    const NLogging::TLogger Logger;


    void DoOpen()
    {
        if (Path_.GetAppend() && Path_.GetCompressionCodec()) {
            THROW_ERROR_EXCEPTION("YPath attributes \"append\" and \"compression_codec\" are not compatible")
                << TErrorAttribute("path", Path_);
        }

        if (Path_.GetAppend() && Path_.GetErasureCodec()) {
            THROW_ERROR_EXCEPTION("YPath attributes \"append\" and \"erasure_codec\" are not compatible")
                << TErrorAttribute("path", Path_);
        }

        if (Options_.TransactionId) {
            Transaction_ = Client_->AttachTransaction(Options_.TransactionId);
            StartListenTransaction(Transaction_);
        }

        auto writerOptions = New<TMultiChunkWriterOptions>();

        TUserObject userObject(Path_);

        GetUserObjectBasicAttributes(
            Client_,
            {&userObject},
            Options_.TransactionId,
            Logger,
            EPermission::Write);

        ObjectId_ = userObject.ObjectId;
        NativeCellTag_ = CellTagFromId(ObjectId_);
        ExternalCellTag_ = userObject.ExternalCellTag;

        if (userObject.Type != EObjectType::File) {
            THROW_ERROR_EXCEPTION("Invalid type of %v: expected %Qlv, actual %Qlv",
                Path_.GetPath(),
                EObjectType::File,
                userObject.Type);
        }

        auto objectIdPath = FromObjectId(ObjectId_);

        {
            YT_LOG_INFO("Requesting extended file attributes");

            auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Follower, NativeCellTag_);
            TObjectServiceProxy proxy(channel);

            auto req = TCypressYPathProxy::Get(objectIdPath + "/@");
            SetTransactionId(req, Transaction_);
            std::vector<TString> attributeKeys{
                "account",
                "compression_codec",
                "erasure_codec",
                "primary_medium",
                "replication_factor"
            };
            ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);

            auto rspOrError = WaitFor(proxy.Execute(req));
            THROW_ERROR_EXCEPTION_IF_FAILED(
                rspOrError,
                "Error requesting extended attributes of file %v",
                Path_.GetPath());

            auto rsp = rspOrError.Value();
            auto attributes = ConvertToAttributes(TYsonString(rsp->value()));
            auto attributesCompressionCodec = attributes->Get<NCompression::ECodec>("compression_codec");
            auto attributesErasureCodec = attributes->Get<NErasure::ECodec>("erasure_codec");

            writerOptions->ReplicationFactor = attributes->Get<int>("replication_factor");
            writerOptions->MediumName = attributes->Get<TString>("primary_medium");
            writerOptions->Account = attributes->Get<TString>("account");
            writerOptions->CompressionCodec = Path_.GetCompressionCodec().value_or(attributesCompressionCodec);
            writerOptions->ErasureCodec = Path_.GetErasureCodec().value_or(attributesErasureCodec);

            YT_LOG_INFO("Extended file attributes received (Account: %v)",
                writerOptions->Account);
        }

        {
            YT_LOG_INFO("Starting file upload");

            auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader, NativeCellTag_);
            TObjectServiceProxy proxy(channel);

            auto batchReq = proxy.ExecuteBatch();

            {
                auto* prerequisitesExt = batchReq->Header().MutableExtension(TPrerequisitesExt::prerequisites_ext);
                for (const auto& id : Options_.PrerequisiteTransactionIds) {
                    auto* prerequisiteTransaction = prerequisitesExt->add_transactions();
                    ToProto(prerequisiteTransaction->mutable_transaction_id(), id);
                }
            }

            {
                bool append = Path_.GetAppend();
                auto req = TFileYPathProxy::BeginUpload(objectIdPath);
                auto updateMode = append ? EUpdateMode::Append : EUpdateMode::Overwrite;
                req->set_update_mode(static_cast<int>(updateMode));
                auto lockMode = (append && !Options_.ComputeMD5) ? ELockMode::Shared : ELockMode::Exclusive;
                req->set_lock_mode(static_cast<int>(lockMode));
                req->set_upload_transaction_title(Format("Upload to %v", Path_.GetPath()));
                req->set_upload_transaction_timeout(ToProto<i64>(Client_->GetNativeConnection()->GetConfig()->UploadTransactionTimeout));
                GenerateMutationId(req);
                SetTransactionId(req, Transaction_);
                batchReq->AddRequest(req, "begin_upload");
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(
                GetCumulativeError(batchRspOrError),
                "Error starting upload to file %v",
                Path_.GetPath());
            const auto& batchRsp = batchRspOrError.Value();

            {
                auto rsp = batchRsp->GetResponse<TFileYPathProxy::TRspBeginUpload>("begin_upload").Value();
                auto uploadTransactionId = FromProto<TTransactionId>(rsp->upload_transaction_id());

                UploadTransaction_ = Client_->AttachTransaction(uploadTransactionId, TTransactionAttachOptions{
                    .AutoAbort = true,
                    .PingAncestors = Options_.PingAncestors
                });
                StartListenTransaction(UploadTransaction_);

                UploadSynchronizer_.AfterBeginUpload(Options_.TransactionId, ObjectId_, ExternalCellTag_);

                YT_LOG_INFO("File upload started (UploadTransactionId: %v)",
                    uploadTransactionId);
            }
        }

        TChunkListId chunkListId;

        {
            YT_LOG_INFO("Requesting file upload parameters");

            auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Follower, ExternalCellTag_);
            TObjectServiceProxy proxy(channel);

            auto req = TFileYPathProxy::GetUploadParams(objectIdPath);
            SetTransactionId(req, UploadTransaction_);

            auto rspOrError = WaitFor(proxy.Execute(req));
            THROW_ERROR_EXCEPTION_IF_FAILED(
                rspOrError,
                "Error requesting upload parameters for file %v",
                Path_.GetPath());

            const auto& rsp = rspOrError.Value();
            chunkListId = FromProto<TChunkListId>(rsp->chunk_list_id());

            if (Options_.ComputeMD5) {
                if (Path_.GetAppend()) {
                    FromProto(&MD5Hasher_, rsp->md5_hasher());
                    if (!MD5Hasher_) {
                        THROW_ERROR_EXCEPTION(
                            "Non-empty file %v has no computed MD5 hash thus "
                            "cannot append data and update the hash simultaneously",
                            Path_.GetPath());
                    }
                } else {
                    MD5Hasher_.emplace();
                }
            }

            YT_LOG_INFO("File upload parameters received (ChunkListId: %v)",
                chunkListId);
        }

        Writer_ = CreateFileMultiChunkWriter(
            Config_,
            writerOptions,
            Client_,
            ExternalCellTag_,
            UploadTransaction_->GetId(),
            chunkListId);

        YT_LOG_INFO("File opened");
    }

    void DoClose()
    {
        ValidateAborted();

        YT_LOG_INFO("Closing file");

        {
            auto result = WaitFor(Writer_->Close());
            THROW_ERROR_EXCEPTION_IF_FAILED(result, "Failed to close file writer");
        }

        auto objectIdPath = FromObjectId(ObjectId_);

        auto channel = Client_->GetMasterChannelOrThrow(EMasterChannelKind::Leader, NativeCellTag_);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        {
            auto* prerequisitesExt = batchReq->Header().MutableExtension(TPrerequisitesExt::prerequisites_ext);
            for (const auto& id : Options_.PrerequisiteTransactionIds) {
                auto* prerequisiteTransaction = prerequisitesExt->add_transactions();
                ToProto(prerequisiteTransaction->mutable_transaction_id(), id);
            }
        }

        StopListenTransaction(UploadTransaction_);

        UploadSynchronizer_.BeforeEndUpload();

        {
            auto req = TFileYPathProxy::EndUpload(objectIdPath);
            *req->mutable_statistics() = Writer_->GetDataStatistics();
            ToProto(req->mutable_md5_hasher(), MD5Hasher_);

            if (auto compressionCodec = Path_.GetCompressionCodec()) {
                req->set_compression_codec(static_cast<int>(*compressionCodec));
            }

            if (auto erasureCodec = Path_.GetErasureCodec()) {
                req->set_erasure_codec(static_cast<int>(*erasureCodec));
            }

            if (auto securityTags = Path_.GetSecurityTags()) {
                ToProto(req->mutable_security_tags()->mutable_items(), *securityTags);
            }

            SetTransactionId(req, UploadTransaction_);
            GenerateMutationId(req);
            batchReq->AddRequest(req, "end_upload");
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(
            GetCumulativeError(batchRspOrError),
            "Error finishing upload to file %v",
            Path_.GetPath());

        UploadTransaction_->Detach();

        UploadSynchronizer_.AfterEndUpload();

        YT_LOG_INFO("File closed");
    }
};

IFileWriterPtr CreateFileWriter(
    IClientPtr client,
    const TRichYPath& path,
    const TFileWriterOptions& options)
{
    return New<TFileWriter>(client, path, options);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
