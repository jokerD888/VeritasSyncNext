#pragma once

#include "engine/common/content_hash.h"
#include "engine/common/protocol.h"
#include "engine/storage/database.h"

#include <cstdint>
#include <vector>

namespace veritassync::sync {

[[nodiscard]] std::vector<protocol::ChunkRange> MissingChunkRanges(
    std::uint64_t total_chunks, const std::vector<std::uint64_t>& completed_chunks);
[[nodiscard]] protocol::FileRequest BuildResumeRequest(
    const storage::TransferId& transfer_id, const common::ContentHash& file_hash,
    std::uint64_t total_chunks, const storage::Database& database);

}  // namespace veritassync::sync
