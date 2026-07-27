#include "engine/common/protocol.h"
#include "tests/test_framework.h"

VSYNC_TEST(ProtocolRoundTripsEnvelopeAndPayload) {
  using namespace veritassync::protocol;
  const Hello expected{"task-01", Role::kSource, "source", "fingerprint", "auth-digest"};
  const auto wire = EncodeFrame({FrameType::kHello, 42, EncodeHello(expected)});
  const auto frame = DecodeFrame(wire);
  const auto actual = DecodeHello(frame.payload);
  VSYNC_CHECK(frame.type == FrameType::kHello);
  VSYNC_CHECK(frame.request_id == 42);
  VSYNC_CHECK(actual.task_id == expected.task_id);
  VSYNC_CHECK(actual.role == expected.role);
  VSYNC_CHECK(actual.device_id == expected.device_id);
  VSYNC_CHECK(IsAllowedOn(Channel::kControl, frame.type));
  VSYNC_CHECK(!IsAllowedOn(Channel::kBulk, frame.type));
}

VSYNC_TEST(ProtocolRejectsMalformedAndTamperedChunks) {
  using namespace veritassync::protocol;
  VSYNC_CHECK_THROWS(DecodeFrame(std::vector<std::uint8_t>{'V', 'S'}));
  auto malformed = EncodeFrame({FrameType::kHeartbeat, 1, {}});
  malformed[0] = 'X';
  VSYNC_CHECK_THROWS(DecodeFrame(malformed));
  Chunk chunk{};
  chunk.bytes = {1, 2, 3, 4};
  chunk.chunk_hash = TestHash(chunk.bytes);
  auto payload = EncodeChunk(chunk);
  payload.back() = 99;
  VSYNC_CHECK_THROWS(DecodeChunk(payload));
}
