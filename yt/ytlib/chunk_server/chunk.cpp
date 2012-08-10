#include "stdafx.h"
#include "chunk.h"

#include "common.h"
#include "chunk_tree_statistics.h"
#include "chunk_list.h"

#include <ytlib/cell_master/load_context.h>
#include <ytlib/chunk_holder/chunk_meta_extensions.h>

namespace NYT {
namespace NChunkServer {

using namespace NCellMaster;
using namespace NObjectServer;
using namespace NChunkHolder::NProto;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkServerLogger;

////////////////////////////////////////////////////////////////////////////////

TChunk::TChunk(const TChunkId& id)
    : TObjectWithIdBase(id)
    , ReplicationFactor_(1)
    , Movable_(true)
{
    // Initialize required proto fields, otherwise #Save would fail.
    ChunkInfo_.set_size(UnknownSize);
    
    ChunkMeta_.set_type(EChunkType::Unknown);
    ChunkMeta_.mutable_extensions();
}

TChunk::~TChunk()
{ }

TChunkTreeStatistics TChunk::GetStatistics() const
{
    TChunkTreeStatistics result;

    YASSERT(ChunkInfo().size() != TChunk::UnknownSize);
    result.CompressedSize = ChunkInfo().size();
    result.ChunkCount = 1;
    result.Rank = 0;

    auto miscExt = GetProtoExtension<NChunkHolder::NProto::TMiscExt>(ChunkMeta().extensions());
    result.UncompressedSize = miscExt.uncompressed_data_size();
    result.RowCount = miscExt.row_count();

    return result;
}

void TChunk::Save(TOutputStream* output) const
{
    TObjectWithIdBase::Save(output);
    SaveProto(output, ChunkInfo_);
    SaveProto(output, ChunkMeta_);
    ::Save(output, ReplicationFactor_);
    ::Save(output, Movable_);
    SaveObjectRefs(output, Parents_);
    ::Save(output, StoredLocations_);
    SaveNullableSet(output, CachedLocations_);
}

void TChunk::Load(const TLoadContext& context, TInputStream* input)
{
    UNUSED(context);
    TObjectWithIdBase::Load(input);
    LoadProto(input, ChunkInfo_);
    LoadProto(input, ChunkMeta_);
    ::Load(input, ReplicationFactor_);
    ::Load(input, Movable_);
    LoadObjectRefs(input, Parents_, context);
    ::Load(input, StoredLocations_);
    LoadNullableSet(input, CachedLocations_);
}

void TChunk::AddLocation(THolderId holderId, bool cached)
{
    if (cached) {
        if (!CachedLocations_) {
            CachedLocations_.Reset(new std::unordered_set<THolderId>());
        }
        YCHECK(CachedLocations_->insert(holderId).second);
    } else {
        StoredLocations_.push_back(holderId);
    }
}

void TChunk::RemoveLocation(THolderId holderId, bool cached)
{
    if (cached) {
        YASSERT(~CachedLocations_);
        YCHECK(CachedLocations_->erase(holderId) == 1);
        if (CachedLocations_->empty()) {
            CachedLocations_.Destroy();
        }
    } else {
        for (auto it = StoredLocations_.begin(); it != StoredLocations_.end(); ++it) {
            if (*it == holderId) {
                StoredLocations_.erase(it);
                return;
            }
        }
        YUNREACHABLE();
    }
}

std::vector<THolderId> TChunk::GetLocations() const
{
    std::vector<THolderId> result(StoredLocations_.begin(), StoredLocations_.end());
    if (~CachedLocations_) {
        result.insert(result.end(), CachedLocations_->begin(), CachedLocations_->end());
    }
    return result;
}

bool TChunk::IsConfirmed() const
{
    return ChunkMeta_.type() != EChunkType::Unknown;
}

bool TChunk::ValidateChunkInfo(const NChunkHolder::NProto::TChunkInfo& chunkInfo) const
{
    if (ChunkInfo_.size() == UnknownSize)
        return true;

    /*
    Switched off for now.
    if (chunkInfo.has_meta_checksum() && ChunkInfo_.has_meta_checksum() &&
        ChunkInfo_.meta_checksum() != chunkInfo.meta_checksum())
    {
        return false;
    }
    */

    return ChunkInfo_.size() == chunkInfo.size();
}

const i64 TChunk::UnknownSize = -1;

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
