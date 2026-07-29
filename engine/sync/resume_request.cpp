#include "engine/sync/resume_request.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace veritassync::sync {
std::vector<protocol::ChunkRange> MissingChunkRanges(const std::uint64_t total_chunks,
                                                      const std::vector<std::uint64_t>& completed_chunks) {
  if (total_chunks == 0) throw std::invalid_argument("transfer must contain chunks");
  std::vector<std::uint64_t> completed = completed_chunks;
  std::ranges::sort(completed);
  completed.erase(std::unique(completed.begin(), completed.end()), completed.end());
  if (!completed.empty() && completed.back() >= total_chunks) throw std::invalid_argument("completed chunk exceeds transfer size");
  std::vector<protocol::ChunkRange> missing;
  std::uint64_t cursor = 0;
  for (const auto chunk : completed) {
    if (chunk > cursor) {
      const auto count = chunk - cursor;
      if (count > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("missing range is too large");
      missing.push_back({cursor, static_cast<std::uint32_t>(count)});
    }
    cursor = chunk + 1;
  }
  if (cursor < total_chunks) {
    const auto count = total_chunks - cursor;
    if (count > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("missing range is too large");
    missing.push_back({cursor, static_cast<std::uint32_t>(count)});
  }
  return missing;
}

protocol::FileRequest BuildResumeRequest(const storage::TransferId& transfer_id,
                                         const common::ContentHash& file_hash,
                                         const std::uint64_t total_chunks,
                                         const storage::Database& database) {
  return {transfer_id, file_hash, MissingChunkRanges(total_chunks, database.CompletedTransferChunks(transfer_id))};
}
}  // namespace veritassync::sync
