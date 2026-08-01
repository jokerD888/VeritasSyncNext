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

VSYNC_TEST(ProtocolRoundTripsOrderedMissingChunkRanges) {
  veritassync::protocol::FileRequest request{}; request.transfer_id[0] = 1; request.file_hash[0] = 2; request.missing_ranges = {{0, 2}, {5, 3}};
  const auto decoded = veritassync::protocol::DecodeFileRequest(veritassync::protocol::EncodeFileRequest(request));
  VSYNC_CHECK(decoded.transfer_id == request.transfer_id); VSYNC_CHECK(decoded.file_hash == request.file_hash); VSYNC_CHECK(decoded.missing_ranges.size() == 2); VSYNC_CHECK(decoded.missing_ranges[1].first_chunk == 5);
  VSYNC_CHECK_THROWS(veritassync::protocol::EncodeFileRequest({{}, {}, {{3, 1}, {2, 1}}}));
}

VSYNC_TEST(ProtocolRoundTripsTransferCancellation) {
  veritassync::protocol::Cancel cancel{}; cancel.transfer_id[0] = 9; cancel.reason = "source_changed";
  const auto decoded = veritassync::protocol::DecodeCancel(veritassync::protocol::EncodeCancel(cancel));
  VSYNC_CHECK(decoded.transfer_id == cancel.transfer_id); VSYNC_CHECK(decoded.reason == "source_changed");
  VSYNC_CHECK_THROWS(veritassync::protocol::EncodeCancel({{}, ""}));
}

VSYNC_TEST(ProtocolRoundTripsVersionedManifestAndRejectsInvalidTombstone) {
  using namespace veritassync::protocol;
  const VersionedManifest expected{9, {
      {"folder", VersionedEntryKind::kDirectory, 0, {}, "dir-v1", "device-a", 2, "", std::nullopt},
      {"folder/notes.txt", VersionedEntryKind::kFile, 3, "abcd", "file-v2", "device-b", 7,
       "file-v1", std::nullopt},
      {"deleted.txt", VersionedEntryKind::kTombstone, 0, {}, "delete-v3", "device-a", 8,
       "file-v2", std::optional<std::uint64_t>{1234}},
  }};
  const auto frame = DecodeFrame(EncodeFrame({FrameType::kVersionManifest, 4,
                                               EncodeVersionedManifest(expected)}));
  const auto actual = DecodeVersionedManifest(frame.payload);
  VSYNC_CHECK(frame.type == FrameType::kVersionManifest);
  VSYNC_CHECK(IsAllowedOn(Channel::kControl, frame.type));
  VSYNC_CHECK(actual.revision == 9);
  VSYNC_CHECK(actual.entries[1].parent_version_id == "file-v1");
  VSYNC_CHECK(actual.entries[2].deleted_at_ms == std::optional<std::uint64_t>{1234});
  VSYNC_CHECK_THROWS(EncodeVersionedManifest({1, {{"gone", VersionedEntryKind::kTombstone, 0, {}, "v", "d", 1, "", std::nullopt}}}));
}
