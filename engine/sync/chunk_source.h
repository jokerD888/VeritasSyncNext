#pragma once

#include "engine/common/protocol.h"

#include <filesystem>
#include <fstream>

namespace veritassync::sync {
class ChunkSource {
 public:
  ChunkSource(std::filesystem::path source_path, std::array<std::uint8_t, 16> transfer_id,
              std::array<std::uint8_t, 32> file_hash);
  void ValidateRequest(const protocol::FileRequest& request) const;
  [[nodiscard]] protocol::Chunk ReadChunk(std::uint64_t chunk_index) const;
 private:
  std::filesystem::path source_path_;
  std::array<std::uint8_t, 16> transfer_id_;
  std::array<std::uint8_t, 32> file_hash_;
  mutable std::ifstream stream_;
};
}  // namespace veritassync::sync
