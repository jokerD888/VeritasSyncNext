#include "engine/transport/send_scheduler.h"

#include <stdexcept>
#include <utility>

namespace veritassync::transport {

SendScheduler::SendScheduler(const std::size_t max_pending_bytes, const std::size_t resume_below_bytes)
    : max_pending_bytes_(max_pending_bytes), resume_below_bytes_(resume_below_bytes) {
  if (max_pending_bytes_ == 0 || resume_below_bytes_ >= max_pending_bytes_) {
    throw std::invalid_argument("send scheduler byte limits are invalid");
  }
}

void SendScheduler::Enqueue(PendingSend pending) {
  if (pending.wire.empty()) throw std::invalid_argument("cannot schedule an empty frame");
  if (pending.wire.size() > max_pending_bytes_ - pending_bytes_) {
    throw std::length_error("per-peer send budget exceeded");
  }
  pending_bytes_ += pending.wire.size();
  queues_[static_cast<std::size_t>(pending.priority)].push_back(std::move(pending));
}

std::optional<PendingSend> SendScheduler::Next(const std::size_t transport_buffered_bytes) {
  if (!CanResume(transport_buffered_bytes)) return std::nullopt;
  for (auto& queue : queues_) {
    if (!queue.empty()) {
      PendingSend next = std::move(queue.front());
      queue.erase(queue.begin());
      pending_bytes_ -= next.wire.size();
      return next;
    }
  }
  return std::nullopt;
}

std::size_t SendScheduler::PendingBytes() const { return pending_bytes_; }

bool SendScheduler::CanResume(const std::size_t transport_buffered_bytes) const {
  return transport_buffered_bytes < resume_below_bytes_;
}

}  // namespace veritassync::transport
