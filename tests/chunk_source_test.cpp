#include "engine/sync/chunk_source.h"
#include "engine/common/content_hash.h"
#include "tests/test_framework.h"
#include <filesystem>
#include <fstream>

VSYNC_TEST(ChunkSourceReissuesOnlyRequestedVerifiedChunks) {
  const auto path = std::filesystem::temp_directory_path() / "veritassync-chunk-source.bin";
  std::vector<std::uint8_t> bytes(veritassync::protocol::kLogicalChunkSize + 3, 1); bytes.back() = 9;
  { std::ofstream stream(path, std::ios::binary); stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); }
  std::array<std::uint8_t, 16> id{}; id[0] = 7; const auto hash = veritassync::common::Blake3(bytes);
  {
    veritassync::sync::ChunkSource source(path, id, hash);
    source.ValidateRequest({id, hash, {{1, 1}}});
    const auto chunk = source.ReadChunk(1);
    VSYNC_CHECK(chunk.offset == veritassync::protocol::kLogicalChunkSize); VSYNC_CHECK(chunk.bytes.size() == 3);
  }
  std::filesystem::remove(path);
}
