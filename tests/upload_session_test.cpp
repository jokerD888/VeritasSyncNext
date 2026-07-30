#include "engine/sync/upload_session.h"
#include "engine/common/content_hash.h"
#include "tests/test_framework.h"

#include <filesystem>
#include <fstream>

VSYNC_TEST(UploadSessionSchedulesOnlyRequestedBulkFrames) {
  const auto path = std::filesystem::temp_directory_path() / "veritassync-upload-session.bin";
  std::vector<std::uint8_t> bytes(veritassync::protocol::kLogicalChunkSize + 1, 4);
  { std::ofstream stream(path, std::ios::binary); stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); }
  std::array<std::uint8_t, 16> id{}; id[0] = 9; const auto hash = veritassync::common::Blake3(bytes);
  veritassync::sync::UploadSession session({path, id, hash}, 1024U * 1024U, 32);
  session.QueueRequested({id, hash, {{1, 1}}});
  VSYNC_CHECK(session.HasPending());
  VSYNC_CHECK(!session.NextForTransport(32).has_value());
  const auto pending = session.NextForTransport(0);
  VSYNC_CHECK(pending.has_value() && pending->channel == veritassync::protocol::Channel::kBulk);
  const auto frame = veritassync::protocol::DecodeFrame(pending->wire);
  VSYNC_CHECK(frame.type == veritassync::protocol::FrameType::kChunk);
  VSYNC_CHECK(veritassync::protocol::DecodeChunk(frame.payload).offset == veritassync::protocol::kLogicalChunkSize);
  VSYNC_CHECK(!session.HasPending());
  std::filesystem::remove(path);
}
