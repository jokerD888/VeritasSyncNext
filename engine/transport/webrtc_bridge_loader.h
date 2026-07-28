#pragma once

#include <cstdint>
#include <filesystem>

namespace veritassync::transport {

class WebRtcBridgeLoader {
 public:
  static constexpr std::uint32_t kExpectedAbiVersion = 1;

  [[nodiscard]] static std::uint64_t VerifyAndReadMaxQueuedBytes(
      const std::filesystem::path& library_path);
};

}  // namespace veritassync::transport
