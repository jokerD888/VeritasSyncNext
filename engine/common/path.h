#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace veritassync::common {
inline std::filesystem::path Utf8Path(std::string_view text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
inline std::string PathUtf8(const std::filesystem::path& path) {
  const auto text = path.generic_u8string();
  return std::string(text.begin(), text.end());
}
}  // namespace veritassync::common
