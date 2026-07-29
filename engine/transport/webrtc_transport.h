#pragma once

#include "engine/transport/transport.h"

#include <filesystem>

namespace veritassync::transport {
class WebRtcTransport final : public Transport {
 public:
  explicit WebRtcTransport(const std::filesystem::path& bridge_path);
  ~WebRtcTransport() override;
  WebRtcTransport(const WebRtcTransport&) = delete;
  void Send(protocol::Channel channel, std::vector<std::uint8_t> wire) override;
  void SetReceiveCallback(ReceiveCallback callback) override;
 private:
  static void __cdecl Receive(void* context, std::uint32_t channel, const std::uint8_t* bytes, std::uint32_t length);
  void* module_ = nullptr; void* factory_ = nullptr; void* send_control_ = nullptr; void* send_bulk_ = nullptr; void* destroy_ = nullptr; ReceiveCallback callback_;
};
}  // namespace veritassync::transport
