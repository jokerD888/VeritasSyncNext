#pragma once

#include "engine/sync/chunk_source.h"
#include "engine/transport/send_scheduler.h"

#include <cstdint>
#include <deque>
#include <optional>

namespace veritassync::sync {

// Converts validated resume requests into byte-budgeted bulk frames. The actual
// transport owns the returned frame and reports its buffered amount on each poll.
class UploadSession {
 public:
  UploadSession(ChunkSource source, std::size_t max_pending_bytes, std::size_t resume_below_bytes);
  void QueueRequested(const protocol::FileRequest& request);
  [[nodiscard]] std::optional<transport::PendingSend> NextForTransport(std::size_t buffered_bytes);
  [[nodiscard]] std::size_t PendingBytes() const;
  [[nodiscard]] bool HasPending() const;

 private:
  ChunkSource source_;
  transport::SendScheduler scheduler_;
  std::deque<protocol::ChunkRange> requested_ranges_;
  std::uint64_t next_request_id_ = 1;
};

}  // namespace veritassync::sync
