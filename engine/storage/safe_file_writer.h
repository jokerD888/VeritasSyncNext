#pragma once

#include "engine/common/content_hash.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace veritassync::storage {

// Resolves a protocol-supplied relative path below an existing task root. It is
// intentionally lexical: callers must not use the result to follow a symlink.
[[nodiscard]] std::filesystem::path ResolveTaskPath(
    const std::filesystem::path& task_root, std::string_view relative_path);

// Writes a completed download beneath task_root without ever exposing a partial
// destination file. The final rename is performed within one directory.
class SafeFileWriter {
 public:
  explicit SafeFileWriter(std::filesystem::path task_root);

  void WriteAtomically(std::string_view relative_path,
                       std::span<const std::uint8_t> bytes) const;
  void WritePartialChunk(std::string_view relative_path, std::uint64_t offset,
                         std::span<const std::uint8_t> bytes) const;
  void CommitPartial(std::string_view relative_path, std::uint64_t expected_size,
                     const common::ContentHash& expected_hash) const;

 private:
  std::filesystem::path task_root_;
};

}  // namespace veritassync::storage
