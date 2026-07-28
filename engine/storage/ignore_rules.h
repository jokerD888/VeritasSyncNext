#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace veritassync::storage {

// Matcher for the task-root .veritasignore file. Inputs are task-relative paths;
// Windows separators are accepted and normalized before matching.
class IgnoreRules {
 public:
  IgnoreRules();

  void Load(std::string_view text);
  void LoadFile(const std::filesystem::path& task_root);
  [[nodiscard]] bool IsIgnored(std::string_view relative_path) const;

 private:
  struct Rule {
    std::string pattern;
    bool negated = false;
    bool rooted = false;
    bool directory_only = false;
  };

  [[nodiscard]] static std::string Normalize(std::string_view value);
  [[nodiscard]] static bool Matches(const Rule& rule, std::string_view path);

  std::vector<Rule> defaults_;
  std::vector<Rule> rules_;
};

}  // namespace veritassync::storage
