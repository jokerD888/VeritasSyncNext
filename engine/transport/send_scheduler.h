#pragma once

#include "engine/common/protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace veritassync::transport {

enum class SendPriority : std::size_t { kControl = 0, kManifest = 1, kSmallFile = 2, kBulk = 3 };

struct PendingSend {
  protocol::Channel channel;
  SendPriority priority;
  std::vector<std::uint8_t> wire;
};

// Per-peer byte-budgeted scheduler. A caller sends only entries returned by Next;
// the caller remains responsible for obtaining the transport buffered amount.
class SendScheduler {
 public:
  SendScheduler(std::size_t max_pending_bytes, std::size_t resume_below_bytes);

  void Enqueue(PendingSend pending);
  [[nodiscard]] std::optional<PendingSend> Next(std::size_t transport_buffered_bytes);
  [[nodiscard]] std::size_t PendingBytes() const;
  [[nodiscard]] bool CanResume(std::size_t transport_buffered_bytes) const;

 private:
  std::array<std::deque<PendingSend>, 4> queues_;
  std::size_t max_pending_bytes_;
  std::size_t resume_below_bytes_;
  std::size_t pending_bytes_ = 0;
};

}  // namespace veritassync::transport
