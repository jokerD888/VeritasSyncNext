#include "engine/sync/upload_session.h"

#include <utility>

namespace veritassync::sync {
UploadSession::UploadSession(ChunkSource source, const std::size_t max_pending_bytes,
                             const std::size_t resume_below_bytes)
    : source_(std::move(source)), scheduler_(max_pending_bytes, resume_below_bytes) {}
void UploadSession::QueueRequested(const protocol::FileRequest& request) {
  for (auto chunk : source_.ReadRequested(request)) {
    auto wire = protocol::EncodeFrame({protocol::FrameType::kChunk, next_request_id_++, protocol::EncodeChunk(chunk)});
    scheduler_.Enqueue({protocol::Channel::kBulk, transport::SendPriority::kBulk, std::move(wire)});
  }
}
std::optional<transport::PendingSend> UploadSession::NextForTransport(const std::size_t buffered_bytes) { return scheduler_.Next(buffered_bytes); }
std::size_t UploadSession::PendingBytes() const { return scheduler_.PendingBytes(); }
}  // namespace veritassync::sync
