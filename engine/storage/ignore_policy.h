#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

class IgnorePolicy {
 public:
  [[nodiscard]] static std::string ReadRules(const std::filesystem::path& task_root);
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
