#include "engine/storage/ignore_rules.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

namespace veritassync::storage {
namespace {

[[nodiscard]] bool MatchCharacterClass(const std::string_view pattern, const std::size_t begin,
                                       const char value, std::size_t* const end) {
  const auto close = pattern.find(']', begin + 1);
  if (close == std::string_view::npos) {
    return false;
  }
  bool negated = false;
  std::size_t position = begin + 1;
  if (position < close && pattern[position] == '!') {
    negated = true;
    ++position;
  }
  bool matched = false;
  while (position < close) {
    if (position + 2 < close && pattern[position + 1] == '-') {
      matched = matched || (value >= pattern[position] && value <= pattern[position + 2]);
      position += 3;
    } else {
      matched = matched || value == pattern[position];
      ++position;
    }
  }
  *end = close + 1;
  return negated ? !matched : matched;
}

[[nodiscard]] bool GlobMatches(const std::string_view pattern, const std::string_view path,
                               const std::size_t pattern_position = 0,
                               const std::size_t path_position = 0) {
  if (pattern_position == pattern.size()) {
    return path_position == path.size();
  }
  if (pattern[pattern_position] == '*' && pattern_position + 2 < pattern.size() &&
      pattern[pattern_position + 1] == '*' && pattern[pattern_position + 2] == '/') {
    if (GlobMatches(pattern, path, pattern_position + 3, path_position)) {
      return true;
    }
    for (std::size_t position = path_position; position < path.size(); ++position) {
      if (path[position] == '/' && GlobMatches(pattern, path, pattern_position + 3, position + 1)) {
        return true;
      }
    }
    return false;
  }
  if (pattern[pattern_position] == '*' && pattern_position + 1 < pattern.size() &&
      pattern[pattern_position + 1] == '*') {
    for (std::size_t position = path_position; position <= path.size(); ++position) {
      if (GlobMatches(pattern, path, pattern_position + 2, position)) {
        return true;
      }
    }
    return false;
  }
  if (path_position == path.size()) {
    return false;
  }
  if (pattern[pattern_position] == '*') {
    for (std::size_t position = path_position; position <= path.size() &&
                                               (position == path_position || path[position - 1] != '/'); ++position) {
      if (GlobMatches(pattern, path, pattern_position + 1, position)) {
        return true;
      }
    }
    return false;
  }
  if (pattern[pattern_position] == '?') {
    return path[path_position] != '/' && GlobMatches(pattern, path, pattern_position + 1, path_position + 1);
  }
  if (pattern[pattern_position] == '[') {
    std::size_t next = pattern_position;
    if (MatchCharacterClass(pattern, pattern_position, path[path_position], &next)) {
      return path[path_position] != '/' && GlobMatches(pattern, path, next, path_position + 1);
    }
    return false;
  }
  return pattern[pattern_position] == path[path_position] &&
         GlobMatches(pattern, path, pattern_position + 1, path_position + 1);
}

[[nodiscard]] bool HasSlash(const std::string_view value) {
  return value.find('/') != std::string_view::npos;
}

[[nodiscard]] bool MatchesAnyComponent(const std::string_view pattern, const std::string_view path) {
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const auto end = path.find('/', begin);
    const auto component = path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
    if (GlobMatches(pattern, component)) {
      return true;
    }
    if (end == std::string_view::npos) {
      return false;
    }
    begin = end + 1;
  }
  return false;
}

}  // namespace

IgnoreRules::IgnoreRules() {
  defaults_ = {
      {"*.part", false, false, false},
      {".git", false, false, true},
      {".veritasignore", false, false, false},
  };
}

void IgnoreRules::Validate(const std::string_view text) {
  constexpr std::size_t kMaximumBytes = 16U * 1024U;
  constexpr std::size_t kMaximumLineBytes = 512U;
  constexpr std::size_t kMaximumRules = 128U;
  if (text.size() > kMaximumBytes) {
    throw std::invalid_argument("ignore rules exceed 16 KiB");
  }
  if (text.find('\0') != std::string_view::npos) {
    throw std::invalid_argument("ignore rules contain a null byte");
  }

  std::size_t rule_count = 0;
  std::size_t line_number = 0;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    ++line_number;
    const auto end = text.find('\n', begin);
    std::string line{text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin)};
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() > kMaximumLineBytes) {
      throw std::invalid_argument("ignore rule line " + std::to_string(line_number) + " exceeds 512 bytes");
    }
    const auto first = line.find_first_not_of(" \t");
    if (first != std::string::npos) {
      line.erase(0, first);
      const auto last = line.find_last_not_of(" \t");
      line.erase(last + 1);
      if (!line.empty() && line.front() != '#') {
        const bool escaped_prefix = line.starts_with("\\!") || line.starts_with("\\#");
        if (escaped_prefix) line.erase(0, 1);
        if (!line.empty() && line.front() == '!') line.erase(0, 1);
        if (!line.empty() && line.front() == '/') line.erase(0, 1);
        if (!line.empty() && line.back() == '/') line.pop_back();
        if (line.empty()) {
          throw std::invalid_argument("ignore rule line " + std::to_string(line_number) + " has no pattern");
        }
        if (line.find('\\') != std::string::npos) {
          throw std::invalid_argument("ignore rule line " + std::to_string(line_number) + " contains an unsupported escape");
        }
        std::size_t component_begin = 0;
        while (component_begin <= line.size()) {
          const auto component_end = line.find('/', component_begin);
          const auto component = line.substr(component_begin, component_end == std::string::npos
              ? line.size() - component_begin : component_end - component_begin);
          if (component.empty() || component == "." || component == "..") {
            throw std::invalid_argument("ignore rule line " + std::to_string(line_number) + " contains an invalid path component");
          }
          if (component_end == std::string::npos) break;
          component_begin = component_end + 1;
        }
        for (std::size_t position = 0; position < line.size(); ++position) {
          const auto byte = static_cast<unsigned char>(line[position]);
          if (byte < 0x20U) {
            throw std::invalid_argument("ignore rule line " + std::to_string(line_number) + " contains a control character");
          }
          if (line[position] == '[') {
            const auto close = line.find(']', position + 1);
            if (close == std::string::npos || close == position + 1 ||
                (close == position + 2 && line[position + 1] == '!')) {
              throw std::invalid_argument("ignore rule line " + std::to_string(line_number) + " has an invalid character class");
            }
            position = close;
          } else if (line[position] == ']') {
            throw std::invalid_argument("ignore rule line " + std::to_string(line_number) + " has an unmatched bracket");
          }
        }
        ++rule_count;
        if (rule_count > kMaximumRules) {
          throw std::invalid_argument("ignore rules exceed 128 patterns");
        }
      }
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  if (rule_count == 0 && !text.empty()) {
    throw std::invalid_argument("ignore rules do not contain a pattern");
  }
}

std::string IgnoreRules::Normalize(const std::string_view value) {
  if (value.empty()) {
    throw std::invalid_argument("ignore path is required");
  }
  std::string result{value};
  std::replace(result.begin(), result.end(), '\\', '/');
  if (result.front() == '/' || result.find(":/") != std::string::npos) {
    throw std::invalid_argument("ignore path must be task-relative");
  }
  while (result.starts_with("./")) {
    result.erase(0, 2);
  }
  if (result.empty() || result.ends_with('/') || result.find("//") != std::string::npos) {
    throw std::invalid_argument("ignore path must name a file or directory");
  }
  std::size_t begin = 0;
  while (begin <= result.size()) {
    const auto end = result.find('/', begin);
    const auto component = result.substr(begin, end == std::string::npos ? result.size() - begin : end - begin);
    if (component == "." || component == "..") {
      throw std::invalid_argument("ignore path must not traverse directories");
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return result;
}

void IgnoreRules::Load(const std::string_view text) {
  Validate(text);
  rules_.clear();
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const auto end = text.find('\n', begin);
    std::string line{text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin)};
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto first = line.find_first_not_of(" \t");
    if (first != std::string::npos) {
      line.erase(0, first);
      const auto last = line.find_last_not_of(" \t");
      line.erase(last + 1);
      if (!line.empty() && line.front() != '#') {
        Rule rule;
        const bool escaped_bang = line.starts_with("\\!");
        if (escaped_bang || line.starts_with("\\#")) line.erase(0, 1);
        rule.negated = !escaped_bang && line.front() == '!';
        if (rule.negated) {
          line.erase(0, 1);
        }
        rule.rooted = !line.empty() && line.front() == '/';
        if (rule.rooted) {
          line.erase(0, 1);
        }
        rule.directory_only = !line.empty() && line.back() == '/';
        if (rule.directory_only) {
          line.pop_back();
        }
        if (!line.empty()) {
          rule.pattern = std::move(line);
          rules_.push_back(std::move(rule));
        }
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
}

void IgnoreRules::LoadFile(const std::filesystem::path& task_root) {
  const auto path = task_root / ".veritasignore";
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    std::error_code error;
    if (std::filesystem::exists(path, error) || error) {
      throw std::runtime_error("cannot read .veritasignore");
    }
    Load({});
    return;
  }
  const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  if (stream.bad()) {
    throw std::runtime_error("cannot read .veritasignore");
  }
  Load(text);
}

bool IgnoreRules::Matches(const Rule& rule, const std::string_view path) {
  const auto path_pattern = std::string_view{rule.pattern};
  const bool component_pattern = !rule.rooted && !HasSlash(path_pattern);
  if (!rule.directory_only) {
    return component_pattern ? MatchesAnyComponent(path_pattern, path) : GlobMatches(path_pattern, path);
  }

  std::size_t begin = 0;
  while (begin <= path.size()) {
    const auto end = path.find('/', begin);
    const auto prefix = path.substr(0, end == std::string_view::npos ? path.size() : end);
    if ((component_pattern && GlobMatches(path_pattern, path.substr(begin, prefix.size() - begin))) ||
        (!component_pattern && GlobMatches(path_pattern, prefix))) {
      return true;
    }
    if (end == std::string_view::npos) {
      return false;
    }
    begin = end + 1;
  }
  return false;
}

bool IgnoreRules::IsIgnored(const std::string_view relative_path) const {
  const auto path = Normalize(relative_path);
  for (const auto& rule : defaults_) {
    if (Matches(rule, path)) {
      return true;
    }
  }
  bool ignored = false;
  for (const auto& rule : rules_) {
    if (Matches(rule, path)) {
      ignored = !rule.negated;
    }
  }
  return ignored;
}

}  // namespace veritassync::storage
