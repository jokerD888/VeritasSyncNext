#include "engine/common/content_hash.h"
#include "tests/test_framework.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>

VSYNC_TEST(ContentHashUsesBlake3KnownVectorAndStreamsFiles) {
  const std::array<std::uint8_t, 3> input{'a', 'b', 'c'};
  const std::array<std::uint8_t, 32> expected{
      0x64, 0x37, 0xb3, 0xac, 0x38, 0x46, 0x51, 0x33, 0xff, 0xb6, 0x3b, 0x75, 0x27, 0x3a, 0x8d, 0xb5,
      0x48, 0xc5, 0x58, 0x46, 0x5d, 0x79, 0xdb, 0x03, 0xfd, 0x35, 0x9c, 0x6c, 0xd5, 0xbd, 0x9d, 0x85};
  VSYNC_CHECK(veritassync::common::Blake3(input) == expected);

  const auto path = std::filesystem::temp_directory_path() / "veritassync-blake3-test.bin";
  { std::ofstream stream(path, std::ios::binary); stream.write("abc", 3); }
  VSYNC_CHECK(veritassync::common::Blake3File(path) == expected);
  std::filesystem::remove(path);
}
