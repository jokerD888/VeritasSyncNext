#pragma once

#include "engine/common/protocol.h"

#include <filesystem>
#include <vector>

namespace veritassync::sync {
class ChunkSource {
 public:
  ChunkSource(std::filesystem::path source_path, std::array<std::uint8_t, 16> transfer_id,
              std::array<std::uint8_t, 32> file_hash);
  [[nodiscard]] std::vector<protocol::Chunk> ReadRequested(const protocol::FileRequest& request) const;
 private:
  std::filesystem::path source_path_;
  std::array<std::uint8_t, 16> transfer_id_;
  std::array<std::uint8_t, 32> file_hash_;
};
}  // namespace veritassync::sync
