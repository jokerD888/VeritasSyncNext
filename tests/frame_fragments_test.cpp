#include "engine/transport/frame_fragments.h"
#include "tests/test_framework.h"
#include <algorithm>

VSYNC_TEST(DataChannelFragmentsReassembleLargeFramesInReverseOrderAndRejectTampering) {
  using namespace veritassync::transport;
  std::vector<std::uint8_t> wire(262244, 42);
  auto fragments = FragmentFrame(1, wire);
  FrameReassembler receiver;
  VSYNC_CHECK(fragments.size() > 1);
  for (const auto& fragment : fragments) VSYNC_CHECK(fragment.size() <= 16U * 1024U);
  VSYNC_CHECK(!receiver.Accept(fragments.back()));
  VSYNC_CHECK(!receiver.Accept(fragments.back())); // exact duplicate is harmless
  auto tampered = fragments.back(); tampered.back() ^= 1;
  VSYNC_CHECK_THROWS(receiver.Accept(tampered));
  for (std::size_t index = fragments.size() - 1; index > 1; --index) VSYNC_CHECK(!receiver.Accept(fragments[index - 1]));
  VSYNC_CHECK(receiver.Accept(fragments.front()).value() == wire);
  auto invalid = fragments.front(); invalid[16] = 1;
  VSYNC_CHECK_THROWS(receiver.Accept(invalid));
}
VSYNC_TEST(DataChannelFragmentsEnforceBoundedReassemblyMemory) {
  using namespace veritassync;
  std::vector<std::uint8_t> wire(protocol::kMaxFrameSize, 1);
  transport::FrameReassembler receiver;
  auto first = transport::FragmentFrame(1, wire);
  auto second = transport::FragmentFrame(2, wire);
  VSYNC_CHECK(!receiver.Accept(first.front()));
  VSYNC_CHECK(!receiver.Accept(second.front()));
  auto third = second.front(); third[4] = 3;
  VSYNC_CHECK_THROWS(receiver.Accept(third));
}
