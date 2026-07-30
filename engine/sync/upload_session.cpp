#include "engine/sync/upload_session.h"

#include <utility>

namespace veritassync::sync {
UploadSession::UploadSession(ChunkSource source, const std::size_t max_pending_bytes,
                             const std::size_t resume_below_bytes)
    : source_(std::move(source)), scheduler_(max_pending_bytes, resume_below_bytes) {}
void UploadSession::QueueRequested(const protocol::FileRequest& request) {
  source_.ValidateRequest(request);
  requested_ranges_.insert(requested_ranges_.end(), request.missing_ranges.begin(), request.missing_ranges.end());
}
std::optional<transport::PendingSend> UploadSession::NextForTransport(const std::size_t buffered_bytes) {
  if (auto pending = scheduler_.Next(buffered_bytes); pending.has_value()) return pending;
  if (requested_ranges_.empty() || !scheduler_.CanResume(buffered_bytes)) return std::nullopt;
  auto& range = requested_ranges_.front();
  const auto chunk = source_.ReadChunk(range.first_chunk);
  ++range.first_chunk;
  --range.chunk_count;
  if (range.chunk_count == 0) requested_ranges_.pop_front();
  auto wire = protocol::EncodeFrame(
      {protocol::FrameType::kChunk, next_request_id_++, protocol::EncodeChunk(chunk)});
  scheduler_.Enqueue({protocol::Channel::kBulk, transport::SendPriority::kBulk, std::move(wire)});
  return scheduler_.Next(buffered_bytes);
}
std::size_t UploadSession::PendingBytes() const { return scheduler_.PendingBytes(); }
bool UploadSession::HasPending() const { return !requested_ranges_.empty() || scheduler_.PendingBytes() != 0; }
}  // namespace veritassync::sync
