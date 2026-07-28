#include "engine/transport/send_scheduler.h"
#include "tests/test_framework.h"

namespace {

veritassync::transport::PendingSend Frame(const veritassync::transport::SendPriority priority,
                                          const std::uint8_t marker, const std::size_t size = 1) {
  return {priority == veritassync::transport::SendPriority::kBulk ? veritassync::protocol::Channel::kBulk
                                                                   : veritassync::protocol::Channel::kControl,
          priority, std::vector<std::uint8_t>(size, marker)};
}

}  // namespace

VSYNC_TEST(SendSchedulerPrioritizesControlAndHonorsBufferedAmount) {
  veritassync::transport::SendScheduler scheduler(32, 8);
  scheduler.Enqueue(Frame(veritassync::transport::SendPriority::kBulk, 4, 4));
  scheduler.Enqueue(Frame(veritassync::transport::SendPriority::kManifest, 3, 3));
  scheduler.Enqueue(Frame(veritassync::transport::SendPriority::kControl, 1, 1));
  VSYNC_CHECK(!scheduler.Next(8).has_value());
  const auto control = scheduler.Next(7);
  const auto manifest = scheduler.Next(0);
  const auto bulk = scheduler.Next(0);
  VSYNC_CHECK(control->wire.front() == 1);
  VSYNC_CHECK(manifest->wire.front() == 3);
  VSYNC_CHECK(bulk->wire.front() == 4);
  VSYNC_CHECK(scheduler.PendingBytes() == 0);
}

VSYNC_TEST(SendSchedulerEnforcesIndependentPeerByteBudget) {
  veritassync::transport::SendScheduler slow_peer(5, 2);
  veritassync::transport::SendScheduler other_peer(5, 2);
  slow_peer.Enqueue(Frame(veritassync::transport::SendPriority::kBulk, 1, 5));
  VSYNC_CHECK_THROWS(slow_peer.Enqueue(Frame(veritassync::transport::SendPriority::kBulk, 2, 1)));
  other_peer.Enqueue(Frame(veritassync::transport::SendPriority::kControl, 3, 1));
  VSYNC_CHECK(other_peer.Next(0)->wire.front() == 3);
}
