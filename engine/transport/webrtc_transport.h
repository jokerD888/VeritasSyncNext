#pragma once

#include "engine/transport/transport.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace veritassync::transport {
class WebRtcTransport final : public Transport {
 public:
  using SdpCallback = std::function<void(std::string)>;
  using RemoteDescriptionCallback = std::function<void(bool)>;
  struct IceCandidate { std::string mid; std::int32_t mline_index; std::string candidate; };
  using IceCallback = std::function<void(IceCandidate)>;
  explicit WebRtcTransport(const std::filesystem::path& bridge_path);
  ~WebRtcTransport() override;
  WebRtcTransport(const WebRtcTransport&) = delete;
  void Send(protocol::Channel channel, std::vector<std::uint8_t> wire) override;
  [[nodiscard]] std::size_t BufferedAmount(protocol::Channel channel) const override;
  void SetReceiveCallback(ReceiveCallback callback) override;
  void SetOfferCallback(SdpCallback callback);
  void SetAnswerCallback(SdpCallback callback);
  void SetIceCallback(IceCallback callback);
  void SetRemoteDescriptionCallback(RemoteDescriptionCallback callback);
  void CreateOffer();
  void ApplyRemoteOffer(std::string sdp);
  void ApplyRemoteAnswer(std::string sdp);
  void ApplyRemoteIceCandidate(const IceCandidate& candidate);
  [[nodiscard]] bool IsReady() const;
 private:
  static void __cdecl Receive(void* context, std::uint32_t channel, const std::uint8_t* bytes, std::uint32_t length);
  static void __cdecl ReceiveOffer(void* context, const char* sdp, std::uint32_t length);
  static void __cdecl ReceiveAnswer(void* context, const char* sdp, std::uint32_t length);
  static void __cdecl ReceiveIce(void* context, const char* mid, std::uint32_t mid_length, std::int32_t index, const char* candidate, std::uint32_t candidate_length);
  static void __cdecl ReceiveRemoteDescription(void* context, std::uint32_t success);

  void* module_ = nullptr;
  void* factory_ = nullptr;
  void* send_control_ = nullptr;
  void* send_bulk_ = nullptr;
  void* destroy_ = nullptr;
  void* create_offer_ = nullptr;
  void* apply_offer_ = nullptr;
  void* apply_answer_ = nullptr;
  void* apply_ice_ = nullptr;
  void* is_ready_ = nullptr;
  void* control_buffered_amount_ = nullptr;
  void* bulk_buffered_amount_ = nullptr;
  std::mutex callback_mutex_;
  ReceiveCallback callback_;
  SdpCallback offer_callback_;
  SdpCallback answer_callback_;
  IceCallback ice_callback_;
  RemoteDescriptionCallback remote_description_callback_;
};
}  // namespace veritassync::transport
