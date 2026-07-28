#include "engine/common/content_hash.h"

#include <blake3.h>

#include <array>
#include <fstream>
#include <stdexcept>

namespace veritassync::common {
namespace {

void Finalize(blake3_hasher* const hasher, ContentHash* const result) {
  blake3_hasher_finalize(hasher, result->data(), result->size());
}

}  // namespace

ContentHash Blake3(const std::span<const std::uint8_t> bytes) {
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, bytes.data(), bytes.size());
  ContentHash result{};
  Finalize(&hasher, &result);
  return result;
}

ContentHash Blake3File(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot open file for BLAKE3");
  }
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  std::array<std::uint8_t, 64U * 1024U> buffer{};
  while (stream) {
    stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto count = stream.gcount();
    if (count > 0) {
      blake3_hasher_update(&hasher, buffer.data(), static_cast<std::size_t>(count));
    }
  }
  if (!stream.eof()) {
    throw std::runtime_error("cannot read file for BLAKE3");
  }
  ContentHash result{};
  Finalize(&hasher, &result);
  return result;
}

}  // namespace veritassync::common
