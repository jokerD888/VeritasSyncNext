#pragma once
#include "engine/common/protocol.h"
#include <chrono>
#include <map>
#include <optional>

namespace veritassync::transport {
// Include the 20-byte envelope in the conservative 16 KiB DataChannel message limit.
inline constexpr std::size_t kFragmentPayloadSize = 16U * 1024U - 20U;
[[nodiscard]] std::vector<std::vector<std::uint8_t>> FragmentFrame(std::uint64_t id,
    std::span<const std::uint8_t> wire);
class FrameReassembler {
 public:
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> Accept(std::span<const std::uint8_t> fragment);
 private:
  struct Partial {
    std::vector<std::uint8_t> wire;
    std::vector<bool> received;
    std::size_t count = 0;
    std::chrono::steady_clock::time_point touched;
  };
  std::map<std::uint64_t, Partial> pending_;
  std::size_t bytes_ = 0;
};
}  // namespace veritassync::transport
