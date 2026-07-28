#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

namespace veritassync::common {

using ContentHash = std::array<std::uint8_t, 32>;

[[nodiscard]] ContentHash Blake3(std::span<const std::uint8_t> bytes);
[[nodiscard]] ContentHash Blake3File(const std::filesystem::path& path);

}  // namespace veritassync::common
