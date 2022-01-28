#include "chaos_table_replica_type_handler.h"

#include "type_handler_detail.h"
#include "client_impl.h"

#include <yt/yt/ytlib/chaos_client/chaos_node_service_proxy.h>

#include <yt/yt/client/chaos_client/helpers.h>
#include <yt/yt/client/chaos_client/replication_card_serialization.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NApi::NNative {

using namespace NYson;
using namespace NYPath;
using namespace NYTree;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NTabletClient;
using namespace NChaosClient;
using namespace NTabletClient;

////////////////////////////////////////////////////////////////////////////////

class TChaosTableReplicaTypeHandler
    : public TVirtualTypeHandler
{
public:
    explicit TChaosTableReplicaTypeHandler(TClient* client)
        : Client_(client)
    { }

    std::optional<std::monostate> AlterTableReplica(
        TTableReplicaId replicaId,
        const TAlterTableReplicaOptions& options) override
    {
        if (TypeFromId(replicaId) != EObjectType::ChaosTableReplica) {
            return {};
        }

        if (options.Atomicity) {
            THROW_ERROR_EXCEPTION("Cannot alter \"atomicity\" for chaos replica");
        }
        if (options.PreserveTimestamps) {
            THROW_ERROR_EXCEPTION("Cannot alter \"preserve_timestamps\" for chaos replica");
        }

        auto replicationCardId = ReplicationCardIdFromReplicaId(replicaId);

        auto channel = Client_->GetChaosChannelByCardId(replicationCardId);
        auto proxy = TChaosNodeServiceProxy(std::move(channel));

        auto req = proxy.AlterTableReplica();
        Client_->SetMutationId(req, options);
        ToProto(req->mutable_replication_card_id(), replicationCardId);
        ToProto(req->mutable_replica_id(), replicaId);
        if (options.Mode) {
            req->set_mode(ToProto<int>(*options.Mode));
        }
        if (options.Enabled) {
            req->set_enabled(*options.Enabled);
        }

        WaitFor(req->Invoke())
            .ThrowOnError();

        return std::monostate();
    }

private:
    TClient* const Client_;


    EObjectType GetSupportedObjectType() override
    {
        return EObjectType::ChaosTableReplica;
    }

    TYsonString GetObjectYson(TReplicaId replicaId) override
    {
        auto replicationCardId = ReplicationCardIdFromReplicaId(replicaId);

        TGetReplicationCardOptions getCardOptions;
        getCardOptions.IncludeProgress = true;
        getCardOptions.IncludeHistory = true;
        getCardOptions.BypassCache = true;
        auto card = WaitFor(Client_->GetReplicationCard(replicationCardId, getCardOptions))
            .ValueOrThrow();

        const auto* replicaInfo = card->GetReplicaOrThrow(replicaId, replicationCardId);
        return BuildYsonStringFluently()
            .BeginAttributes()
                .Item("id").Value(replicaId)
                .Item("type").Value(EObjectType::ChaosTableReplica)
                .Item("replication_card_id").Value(replicationCardId)
                .Do([&] (auto fluent) {
                    Serialize(
                        *replicaInfo,
                        fluent,
                        static_cast<TReplicationCardFetchOptions&>(getCardOptions));
                })
            .EndAttributes()
            .Entity();
    }

    std::optional<TObjectId> DoCreateObject(const TCreateObjectOptions& options) override
    {
        auto attributes = options.Attributes ? options.Attributes->Clone() : EmptyAttributes().Clone();

        auto replicationCardId = attributes->Get<TReplicationCardId>("replication_card_id");
        auto clusterName = attributes->Get<TString>("cluster_name");
        auto replicaPath = attributes->Get<TString>("replica_path");
        auto contentType = attributes->Get<ETableReplicaContentType>("content_type", ETableReplicaContentType::Data);
        auto mode = attributes->Get<ETableReplicaMode>("mode", ETableReplicaMode::Async);
        auto enabled = attributes->Get<bool>("enabled", false);

        auto channel = Client_->GetChaosChannelByCardId(replicationCardId);
        auto proxy = TChaosNodeServiceProxy(std::move(channel));

        auto req = proxy.CreateTableReplica();
        Client_->SetMutationId(req, options);
        ToProto(req->mutable_replication_card_id(), replicationCardId);
        req->set_cluster_name(clusterName);
        req->set_replica_path(replicaPath);
        req->set_content_type(ToProto<int>(contentType));
        req->set_mode(ToProto<int>(mode));
        req->set_enabled(enabled);

        auto rsp = WaitFor(req->Invoke())
            .ValueOrThrow();

        return FromProto<TReplicaId>(rsp->replica_id());
    }

    void DoRemoveObject(TReplicaId replicaId, const TRemoveNodeOptions& options) override
    {
        auto replicationCardId = ReplicationCardIdFromReplicaId(replicaId);

        auto channel = Client_->GetChaosChannelByCardId(replicationCardId);
        auto proxy = TChaosNodeServiceProxy(std::move(channel));

        auto req = proxy.RemoveTableReplica();
        Client_->SetMutationId(req, options);
        ToProto(req->mutable_replication_card_id(), replicationCardId);
        ToProto(req->mutable_replica_id(), replicaId);

        WaitFor(req->Invoke())
            .ThrowOnError();
    }
};

////////////////////////////////////////////////////////////////////////////////

ITypeHandlerPtr CreateChaosTableReplicaTypeHandler(TClient* client)
{
    return New<TChaosTableReplicaTypeHandler>(client);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
