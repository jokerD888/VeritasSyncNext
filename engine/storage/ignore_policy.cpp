#include "engine/storage/ignore_policy.h"

#include "engine/storage/ignore_rules.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace veritassync::storage {
namespace {

struct InventoryEntry {
  std::string path;
  bool is_file = false;
};

struct Inventory {
  std::vector<InventoryEntry> entries;
  std::size_t file_count = 0;
  bool truncated = false;
};

[[nodiscard]] std::string ToUtf8(const std::filesystem::path& path) {
  const auto value = path.generic_u8string();
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::string LowerAscii(std::string value) {
  for (auto& character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte >= 'A' && byte <= 'Z') character = static_cast<char>(byte + ('a' - 'A'));
  }
  return value;
}

[[nodiscard]] std::string Extension(std::string_view path) {
  const auto slash = path.find_last_of('/');
  const auto name = path.substr(slash == std::string_view::npos ? 0 : slash + 1);
  const auto dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) return "(none)";
  return LowerAscii(std::string{name.substr(dot)});
}

[[nodiscard]] Inventory BuildInventory(const std::filesystem::path& task_root,
                                       const std::size_t maximum_files) {
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(task_root, error);
  if (error || !std::filesystem::is_directory(root)) {
    throw std::invalid_argument("ignore policy root must be an existing directory");
  }
  IgnoreRules built_in_rules;
  Inventory inventory;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  if (error) throw std::runtime_error("cannot enumerate ignore policy root");
  const std::filesystem::recursive_directory_iterator end;
  while (iterator != end) {
    const auto entry = *iterator;
    const auto status = entry.symlink_status(error);
    if (error) {
      error.clear();
      iterator.increment(error);
      error.clear();
      continue;
    }
    if (std::filesystem::is_symlink(status)) {
      if (entry.is_directory(error)) iterator.disable_recursion_pending();
      error.clear();
      iterator.increment(error);
      error.clear();
      continue;
    }
    const auto relative = ToUtf8(entry.path().lexically_relative(root));
    if (built_in_rules.IsIgnored(relative)) {
      if (std::filesystem::is_directory(status)) iterator.disable_recursion_pending();
      iterator.increment(error);
      error.clear();
      continue;
    }
    const bool is_file = std::filesystem::is_regular_file(status);
    if (is_file || std::filesystem::is_directory(status)) {
      inventory.entries.push_back({relative, is_file});
      if (is_file) {
        ++inventory.file_count;
        if (inventory.file_count >= maximum_files) {
          inventory.truncated = true;
          break;
        }
      }
    }
    iterator.increment(error);
    error.clear();
  }
  std::ranges::sort(inventory.entries, {}, &InventoryEntry::path);
  return inventory;
}

using ExtensionCounts = std::map<std::string, std::size_t>;

void AppendSummaryLine(std::string& output, const std::string& label, const std::size_t count,
                       const ExtensionCounts& extensions, const std::size_t maximum_bytes) {
  std::vector<std::pair<std::size_t, std::string>> ordered;
  ordered.reserve(extensions.size());
  for (const auto& [extension, extension_count] : extensions) {
    ordered.emplace_back(extension_count, extension);
  }
  std::ranges::sort(ordered, [](const auto& left, const auto& right) {
    return left.first != right.first ? left.first > right.first : left.second < right.second;
  });
  std::ostringstream line;
  line << label << ": " << count << " files";
  if (!ordered.empty()) {
    line << " (";
    for (std::size_t index = 0; index < ordered.size() && index < 6; ++index) {
      if (index > 0) line << ", ";
      line << ordered[index].second << " x" << ordered[index].first;
    }
    if (ordered.size() > 6) line << ", ...";
    line << ')';
  }
  line << '\n';
  const auto value = line.str();
  if (output.size() + value.size() <= maximum_bytes) output += value;
}

[[nodiscard]] std::vector<std::string> SearchTerms(std::string_view description) {
  const auto lower = LowerAscii(std::string{description});
  static const std::vector<std::pair<std::string, std::vector<std::string>>> mappings{
      {"test", {"test", "spec", "mock", "测试"}},
      {"build", {"build", "dist", "output", "编译", "构建", "产物"}},
      {"log", {"log", "日志"}}, {"cache", {"cache", "缓存"}},
      {"temp", {"temp", "tmp", "临时"}}, {"doc", {"doc", "readme", "文档"}},
      {"config", {"config", "conf", "setting", "配置"}},
      {"image", {"image", "img", "图片", "照片"}}, {"video", {"video", "视频"}},
  };
  std::vector<std::string> terms;
  for (const auto& [term, triggers] : mappings) {
    if (std::ranges::any_of(triggers, [&](const auto& trigger) { return lower.find(trigger) != std::string::npos; })) {
      terms.push_back(term);
    }
  }
  std::string word;
  for (const auto character : lower) {
    const auto byte = static_cast<unsigned char>(character);
    if (std::isalnum(byte) != 0 || character == '_' || character == '.') {
      word.push_back(character);
    } else {
      if (word.size() >= 3) terms.push_back(std::exchange(word, {}));
      else word.clear();
    }
  }
  if (word.size() >= 3) terms.push_back(std::move(word));
  std::ranges::sort(terms);
  terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
  return terms;
}

}  // namespace

std::string IgnorePolicy::ReadRules(const std::filesystem::path& task_root) {
  const auto path = task_root / ".veritasignore";
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) && !error) return {};
    throw std::runtime_error("cannot read .veritasignore");
  }
  const std::string rules{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  if (stream.bad()) throw std::runtime_error("cannot read .veritasignore");
  IgnoreRules::Validate(rules);
  return rules;
}

IgnoreContext IgnorePolicy::BuildContext(const std::filesystem::path& task_root,
                                         const std::string_view description,
                                         const IgnoreContextMode mode,
                                         const std::size_t maximum_files,
                                         const std::size_t maximum_summary_bytes) {
  if (description.empty() || description.size() > 4096U) {
    throw std::invalid_argument("ignore description must contain 1 to 4096 bytes");
  }
  if (maximum_files == 0 || maximum_summary_bytes < 256U) {
    throw std::invalid_argument("ignore context limits are invalid");
  }
  const auto inventory = BuildInventory(task_root, maximum_files);
  IgnoreContext context;
  context.scanned_files = inventory.file_count;
  context.truncated = inventory.truncated;
  context.directory_summary = "Total files: " + std::to_string(inventory.file_count) +
      (inventory.truncated ? "+ (truncated)\n" : "\n");

  if (mode == IgnoreContextMode::kPrivate) {
    std::map<std::size_t, std::pair<std::size_t, ExtensionCounts>> depths;
    for (const auto& entry : inventory.entries) {
      if (!entry.is_file) continue;
      const auto depth = static_cast<std::size_t>(std::count(entry.path.begin(), entry.path.end(), '/'));
      ++depths[depth].first;
      ++depths[depth].second[Extension(entry.path)];
    }
    for (const auto& [depth, values] : depths) {
      AppendSummaryLine(context.directory_summary, "depth " + std::to_string(depth), values.first,
                        values.second, maximum_summary_bytes);
    }
    return context;
  }

  std::map<std::string, std::pair<std::size_t, ExtensionCounts>> directories;
  for (const auto& entry : inventory.entries) {
    if (!entry.is_file) continue;
    const auto slash = entry.path.find_last_of('/');
    const auto directory = slash == std::string::npos ? std::string{"(root)"} : entry.path.substr(0, slash);
    ++directories[directory].first;
    ++directories[directory].second[Extension(entry.path)];
  }
  std::vector<std::pair<std::string, std::pair<std::size_t, ExtensionCounts>>> ordered(
      directories.begin(), directories.end());
  std::ranges::sort(ordered, [](const auto& left, const auto& right) {
    const auto left_depth = std::count(left.first.begin(), left.first.end(), '/');
    const auto right_depth = std::count(right.first.begin(), right.first.end(), '/');
    return left_depth != right_depth ? left_depth < right_depth : left.first < right.first;
  });
  for (const auto& [directory, values] : ordered) {
    AppendSummaryLine(context.directory_summary, directory, values.first, values.second,
                      maximum_summary_bytes);
  }

  const auto terms = SearchTerms(description);
  std::set<std::string> matched_directories;
  for (const auto& entry : inventory.entries) {
    if (!entry.is_file || context.relevant_paths.size() >= 20U) continue;
    const auto lower_path = LowerAscii(entry.path);
    if (std::ranges::any_of(terms, [&](const auto& term) { return lower_path.find(term) != std::string::npos; })) {
      context.relevant_paths.push_back(entry.path);
      const auto slash = entry.path.find_last_of('/');
      matched_directories.insert(slash == std::string::npos ? std::string{} : entry.path.substr(0, slash));
    }
  }
  for (const auto& entry : inventory.entries) {
    if (!entry.is_file || context.comparison_paths.size() >= 8U ||
        std::ranges::find(context.relevant_paths, entry.path) != context.relevant_paths.end()) continue;
    const auto slash = entry.path.find_last_of('/');
    const auto directory = slash == std::string::npos ? std::string{} : entry.path.substr(0, slash);
    if (matched_directories.contains(directory)) context.comparison_paths.push_back(entry.path);
  }
  return context;
}

IgnorePreview IgnorePolicy::Preview(const std::filesystem::path& task_root,
                                    const std::string_view current_rules,
                                    const std::string_view proposed_rules,
                                    const std::vector<std::string>& tracked_paths,
                                    const std::size_t maximum_files,
                                    const std::size_t maximum_samples) {
  IgnoreRules current;
  current.Load(current_rules);
  IgnoreRules proposed;
  proposed.Load(proposed_rules);
  const auto inventory = BuildInventory(task_root, maximum_files);
  const std::unordered_set<std::string> tracked(tracked_paths.begin(), tracked_paths.end());
  IgnorePreview preview;
  preview.scanned_files = inventory.file_count;
  preview.truncated = inventory.truncated;
  for (const auto& entry : inventory.entries) {
    if (!entry.is_file) continue;
    const bool was_ignored = current.IsIgnored(entry.path);
    const bool will_be_ignored = proposed.IsIgnored(entry.path);
    if (was_ignored) ++preview.currently_ignored;
    if (will_be_ignored) ++preview.proposed_ignored;
    if (!was_ignored && will_be_ignored) {
      ++preview.newly_ignored;
      if (preview.newly_ignored_samples.size() < maximum_samples) {
        preview.newly_ignored_samples.push_back(entry.path);
      }
      if (tracked.contains(entry.path)) {
        ++preview.tracked_newly_ignored;
        if (preview.tracked_deletion_samples.size() < maximum_samples) {
          preview.tracked_deletion_samples.push_back(entry.path);
        }
      }
    } else if (was_ignored && !will_be_ignored) {
      ++preview.newly_included;
      if (preview.newly_included_samples.size() < maximum_samples) {
        preview.newly_included_samples.push_back(entry.path);
      }
    }
  }
  return preview;
}

}  // namespace veritassync::storage
