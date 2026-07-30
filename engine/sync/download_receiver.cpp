#include "engine/sync/download_receiver.h"

#include "engine/common/content_hash.h"
#include "engine/sync/resume_request.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace veritassync::sync {
DownloadReceiver::DownloadReceiver(storage::Database& database, const storage::TransferId& transfer_id,
                                   storage::SafeFileWriter& writer, std::string relative_path,
                                   const std::uint64_t expected_size, common::ContentHash expected_hash,
                                   const std::uint64_t chunk_count)
    : database_(database), transfer_id_(transfer_id), writer_(writer), relative_path_(std::move(relative_path)),
      expected_size_(expected_size), expected_hash_(expected_hash), chunk_count_(chunk_count) {
  if (relative_path_.empty() || chunk_count_ == 0) throw std::invalid_argument("download metadata is invalid");
}
void DownloadReceiver::AcceptChunk(const std::uint64_t chunk_index, const std::uint64_t offset,
                                   const std::span<const std::uint8_t> bytes, const common::ContentHash& chunk_hash,
                                   const std::int64_t updated_at_ms, const bool persist) {
  if (cancelled_) throw std::logic_error("download is cancelled");
  const auto expected_offset = chunk_index * protocol::kLogicalChunkSize;
  const auto expected_length = expected_offset < expected_size_ ?
      std::min<std::uint64_t>(protocol::kLogicalChunkSize, expected_size_ - expected_offset) : 0;
  if (chunk_index >= chunk_count_ || offset != expected_offset || bytes.size() != expected_length || common::Blake3(bytes) != chunk_hash) {
    throw std::invalid_argument("download chunk is invalid");
  }
  writer_.WritePartialChunk(relative_path_, offset, bytes, persist);
  if (persist) database_.MarkTransferChunkCompleted(transfer_id_, chunk_index, updated_at_ms);
}
void DownloadReceiver::PersistAcceptedChunks(const std::span<const std::uint64_t> chunk_indices,
                                             const std::int64_t updated_at_ms) {
  if (cancelled_) throw std::logic_error("download is cancelled");
  writer_.FlushPartial(relative_path_);
  database_.MarkTransferChunksCompleted(transfer_id_, chunk_indices, updated_at_ms);
}
protocol::FileRequest DownloadReceiver::ResumeRequest() const {
  if (cancelled_) throw std::logic_error("download is cancelled");
  return BuildResumeRequest(transfer_id_, expected_hash_, chunk_count_, database_);
}
void DownloadReceiver::Cancel(const protocol::Cancel& cancel, const std::int64_t cancelled_at_ms) {
  if (cancel.transfer_id != transfer_id_) throw std::invalid_argument("cancel does not match transfer");
  database_.UpdateTransferState(transfer_id_, "cancelled", cancelled_at_ms, cancel.reason);
  cancelled_ = true;
}
void DownloadReceiver::Commit(const std::int64_t completed_at_ms) {
  if (cancelled_) throw std::logic_error("download is cancelled");
  const auto completed = database_.CompletedTransferChunks(transfer_id_);
  if (completed.size() != chunk_count_) throw std::logic_error("download has missing chunks");
  for (std::uint64_t index = 0; index < chunk_count_; ++index) {
    if (completed[index] != index) throw std::logic_error("download has missing chunks");
  }
  writer_.CommitPartial(relative_path_, expected_size_, expected_hash_);
  database_.UpdateTransferState(transfer_id_, "completed", completed_at_ms);
}
}  // namespace veritassync::sync
