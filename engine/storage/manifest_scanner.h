#pragma once

#include "engine/common/content_hash.h"
#include "engine/storage/ignore_rules.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace veritassync::storage {

enum class SnapshotKind { kFile, kDirectory };

struct SnapshotEntry {
  std::string relative_path;
  SnapshotKind kind;
  std::uint64_t size = 0;
  std::int64_t mtime_ns = 0;
  std::optional<common::ContentHash> content_hash;
};

// Builds a deterministic task-local snapshot. It never follows symlinks and only
// hashes files that survive the ignore rules.
class ManifestScanner {
 public:
  explicit ManifestScanner(IgnoreRules rules = {});

  [[nodiscard]] std::vector<SnapshotEntry> Scan(const std::filesystem::path& task_root) const;

 private:
  IgnoreRules rules_;
};

}  // namespace veritassync::storage
