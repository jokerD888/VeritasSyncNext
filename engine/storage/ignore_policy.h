#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "engine/storage/database.h"

namespace veritassync::storage {

enum class IgnoreContextMode { kPrivate, kPrecise };

struct IgnoreContext {
  std::size_t scanned_files = 0;
  bool truncated = false;
  std::string directory_summary;
  std::vector<std::string> relevant_paths;
  std::vector<std::string> comparison_paths;
};

struct IgnorePreview {
  std::size_t scanned_files = 0;
  std::size_t currently_ignored = 0;
  std::size_t proposed_ignored = 0;
  std::size_t newly_ignored = 0;
  std::size_t newly_included = 0;
  std::size_t tracked_newly_ignored = 0;
  bool truncated = false;
  std::vector<std::string> newly_ignored_samples;
  std::vector<std::string> newly_included_samples;
  std::vector<std::string> tracked_deletion_samples;
};

struct IgnorePolicyState {
  std::uint64_t revision = 0;
  std::string rules;
  std::string content_hash;
  std::string source;
  std::int64_t created_at_ms = 0;
};

class IgnorePolicy {
 public:
  [[nodiscard]] static std::string ReadRules(const std::filesystem::path& task_root);
  [[nodiscard]] static std::string HashRules(std::string_view rules);
  [[nodiscard]] static IgnorePolicyState Synchronize(
      Database& database, const std::string& task_id,
      const std::filesystem::path& task_root, std::int64_t now_ms);
  [[nodiscard]] static IgnorePolicyState Apply(
      Database& database, const std::string& task_id,
      const std::filesystem::path& task_root, std::string_view expected_hash,
      std::string_view rules, std::string source, std::int64_t now_ms);
  [[nodiscard]] static IgnorePolicyState Undo(
      Database& database, const std::string& task_id,
      const std::filesystem::path& task_root, std::string_view expected_hash,
      std::int64_t now_ms);
  [[nodiscard]] static IgnoreContext BuildContext(
      const std::filesystem::path& task_root, std::string_view description,
      IgnoreContextMode mode, std::size_t maximum_files = 50000,
      std::size_t maximum_summary_bytes = 8192);
  [[nodiscard]] static IgnorePreview Preview(
      const std::filesystem::path& task_root, std::string_view current_rules,
      std::string_view proposed_rules, const std::vector<std::string>& tracked_paths,
      std::size_t maximum_files = 50000, std::size_t maximum_samples = 20);
};

}  // namespace veritassync::storage
