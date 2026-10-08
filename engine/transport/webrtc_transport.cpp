#include "engine/transport/webrtc_transport.h"

#include "engine/transport/webrtc_bridge_loader.h"
#include "third_party/libwebrtc_bridge/veritassync_webrtc_bridge.h"

#include <Windows.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace veritassync::transport {
namespace {
using CreateFunction = void*(__cdecl*)();
using DestroyFunction = void(__cdecl*)(void*);
using CreateChannelsFunction = std::uint32_t(__cdecl*)(void*);
using CreatePeerConnectionFunction = std::uint32_t(__cdecl*)(void*);
using DataCallback = void(__cdecl*)(void*, std::uint32_t, const std::uint8_t*, std::uint32_t);
using SetDataCallbackFunction = void(__cdecl*)(void*, DataCallback, void*);
using SendFunction = std::uint32_t(__cdecl*)(void*, const std::uint8_t*, std::uint32_t);
using SdpCallback = void(__cdecl*)(void*, const char*, std::uint32_t);
using SetSdpCallbackFunction = void(__cdecl*)(void*, SdpCallback, void*);
using IceCallback = void(__cdecl*)(void*, const char*, std::uint32_t, std::int32_t, const char*,
                                   std::uint32_t);
using SetIceCallbackFunction = void(__cdecl*)(void*, IceCallback, void*);
using CompletionCallback = void(__cdecl*)(void*, std::uint32_t);
using SetCompletionCallbackFunction = void(__cdecl*)(void*, CompletionCallback, void*);
using CreateOfferFunction = std::uint32_t(__cdecl*)(void*);
using ApplySdpFunction = std::uint32_t(__cdecl*)(void*, const char*, std::uint32_t);
using ApplyIceFunction = std::uint32_t(__cdecl*)(void*, const char*, std::uint32_t, std::int32_t,
                                                 const char*, std::uint32_t);
using IsReadyFunction = std::uint32_t(__cdecl*)(void*);
using BufferedAmountFunction = std::uint64_t(__cdecl*)(void*);
template <typename Function>
Function Lookup(const HMODULE module, const char* const name) {
  const auto address = GetProcAddress(module, name);
  if (address == nullptr) throw std::runtime_error(std::string("WebRTC bridge is missing ") + name);
  return reinterpret_cast<Function>(address);
}
}  // namespace

WebRtcTransport::WebRtcTransport(const std::filesystem::path& bridge_path, const bool initiator,
                                 const IceOptions& options) {
  options.Validate();
  module_ = LoadLibraryW(bridge_path.c_str());
  if (module_ == nullptr) throw std::runtime_error("cannot load WebRTC bridge");
  try {
    const auto module = static_cast<HMODULE>(module_);
    const auto abi = Lookup<std::uint32_t(__cdecl*)()>(module, "VeritasSyncWebRtcBridgeAbiVersion");
    if (abi() != WebRtcBridgeLoader::kExpectedAbiVersion)
      throw std::runtime_error("WebRTC bridge ABI mismatch");
    const auto create = Lookup<CreateFunction>(module, "VeritasSyncWebRtcBridgeCreateFactory");
    destroy_ = reinterpret_cast<void*>(
        Lookup<DestroyFunction>(module, "VeritasSyncWebRtcBridgeDestroyFactory"));
    factory_ = create();
    if (factory_ == nullptr) throw std::runtime_error("cannot create WebRTC factory");
    std::vector<VsyncWebRtcIceServer> servers;
    for (const auto& server : options.servers)
      servers.push_back({server.url.c_str(), server.username.c_str(), server.credential.c_str()});
    using ConfigureIceFunction = std::uint32_t(__cdecl*)(void*, const VsyncWebRtcIceServer*, std::uint32_t, std::uint32_t);
    if (Lookup<ConfigureIceFunction>(module, "VeritasSyncWebRtcBridgeConfigureIce")(
            factory_, servers.data(), static_cast<std::uint32_t>(servers.size()), options.relay_only ? 1U : 0U) != 1U)
      throw std::runtime_error("WebRTC rejected ICE configuration");
    const auto connection_created =
        initiator ? Lookup<CreateChannelsFunction>(
                        module, "VeritasSyncWebRtcBridgeCreateProtocolChannels")(factory_)
                  : Lookup<CreatePeerConnectionFunction>(
                        module, "VeritasSyncWebRtcBridgeCreatePeerConnection")(factory_);
    if (connection_created != 1U) throw std::runtime_error("cannot create WebRTC peer connection");
    Lookup<SetDataCallbackFunction>(module, "VeritasSyncWebRtcBridgeSetDataCallback")(
        factory_, Receive, this);
    Lookup<SetSdpCallbackFunction>(module, "VeritasSyncWebRtcBridgeSetOfferCallback")(
        factory_, ReceiveOffer, this);
    Lookup<SetSdpCallbackFunction>(module, "VeritasSyncWebRtcBridgeSetAnswerCallback")(
        factory_, ReceiveAnswer, this);
    Lookup<SetIceCallbackFunction>(module, "VeritasSyncWebRtcBridgeSetIceCallback")(
        factory_, ReceiveIce, this);
    Lookup<SetCompletionCallbackFunction>(module,
                                          "VeritasSyncWebRtcBridgeSetRemoteDescriptionCallback")(
        factory_, ReceiveRemoteDescription, this);
    send_control_ =
        reinterpret_cast<void*>(Lookup<SendFunction>(module, "VeritasSyncWebRtcBridgeSendControl"));
    send_bulk_ =
        reinterpret_cast<void*>(Lookup<SendFunction>(module, "VeritasSyncWebRtcBridgeSendBulk"));
    create_offer_ = reinterpret_cast<void*>(
        Lookup<CreateOfferFunction>(module, "VeritasSyncWebRtcBridgeCreateOffer"));
    restart_ice_ = reinterpret_cast<void*>(
        Lookup<CreateOfferFunction>(module, "VeritasSyncWebRtcBridgeRestartIce"));
    apply_offer_ = reinterpret_cast<void*>(
        Lookup<ApplySdpFunction>(module, "VeritasSyncWebRtcBridgeApplyRemoteOffer"));
    apply_answer_ = reinterpret_cast<void*>(
        Lookup<ApplySdpFunction>(module, "VeritasSyncWebRtcBridgeApplyRemoteAnswer"));
    apply_ice_ = reinterpret_cast<void*>(
        Lookup<ApplyIceFunction>(module, "VeritasSyncWebRtcBridgeApplyRemoteIceCandidate"));
    is_ready_ =
        reinterpret_cast<void*>(Lookup<IsReadyFunction>(module, "VeritasSyncWebRtcBridgeIsReady"));
    control_buffered_amount_ = reinterpret_cast<void*>(
        Lookup<BufferedAmountFunction>(module, "VeritasSyncWebRtcBridgeControlBufferedAmount"));
    bulk_buffered_amount_ = reinterpret_cast<void*>(
        Lookup<BufferedAmountFunction>(module, "VeritasSyncWebRtcBridgeBulkBufferedAmount"));
  } catch (...) {
    if (factory_ != nullptr && destroy_ != nullptr)
      reinterpret_cast<DestroyFunction>(destroy_)(factory_);
    FreeLibrary(static_cast<HMODULE>(module_));
    throw;
  }
}
WebRtcTransport::~WebRtcTransport() {
  if (factory_ != nullptr && destroy_ != nullptr)
    reinterpret_cast<DestroyFunction>(destroy_)(factory_);
  if (module_ != nullptr) FreeLibrary(static_cast<HMODULE>(module_));
}
void WebRtcTransport::Send(const protocol::Channel channel, std::vector<std::uint8_t> wire) {
  if (wire.empty() || wire.size() > protocol::kMaxFrameSize)
    throw std::invalid_argument("invalid WebRTC frame");
  const auto index = channel == protocol::Channel::kControl ? 0U : 1U;
  {
    std::scoped_lock lock(wire_mutex_);
    const auto size = wire.size() + ((wire.size() + kFragmentPayloadSize - 1) / kFragmentPayloadSize) * 20;
    if (size > 32U * 1024U * 1024U - queued_bytes_[0] - queued_bytes_[1])
      throw std::length_error("per-peer WebRTC send budget exceeded");
    for (auto& fragment : FragmentFrame(next_message_id_++, wire)) {
      queued_bytes_[index] += fragment.size();
      outgoing_[index].push_back(std::move(fragment));
    }
  }
  Pump();
}
void WebRtcTransport::Pump() {
  // Never call libwebrtc while holding wire_mutex_: Send may synchronously deliver a callback.
  for (std::size_t index = 0; index < 2; ++index) {
    const auto send = reinterpret_cast<SendFunction>(index == 0 ? send_control_ : send_bulk_);
    const auto buffered = reinterpret_cast<BufferedAmountFunction>(index == 0 ? control_buffered_amount_ : bulk_buffered_amount_);
    while (IsReady() && buffered(factory_) < 512U * 1024U) {
      std::vector<std::uint8_t> fragment;
      {
        std::scoped_lock lock(wire_mutex_);
        if (wire_error_) throw std::runtime_error(*wire_error_);
        if (outgoing_[index].empty()) break;
        fragment = std::move(outgoing_[index].front());
        outgoing_[index].pop_front();
        queued_bytes_[index] -= fragment.size();
      }
      if (send(factory_, fragment.data(), static_cast<std::uint32_t>(fragment.size())) != 1U)
        throw std::runtime_error("WebRTC DataChannel rejected fragment");
    }
  }
}
std::size_t WebRtcTransport::BufferedAmount(const protocol::Channel channel) const {
  const auto function = reinterpret_cast<BufferedAmountFunction>(
      channel == protocol::Channel::kControl ? control_buffered_amount_ : bulk_buffered_amount_);
  const auto amount = function(factory_);
  if (amount > (std::numeric_limits<std::size_t>::max)()) {
    return (std::numeric_limits<std::size_t>::max)();
  }
  std::scoped_lock lock(wire_mutex_);
  return static_cast<std::size_t>(amount) + queued_bytes_[channel == protocol::Channel::kControl ? 0 : 1];
}
void WebRtcTransport::SetReceiveCallback(ReceiveCallback callback) {
  std::scoped_lock lock(callback_mutex_);
  callback_ = std::move(callback);
}
void WebRtcTransport::SetOfferCallback(SdpCallback callback) {
  std::scoped_lock lock(callback_mutex_);
  offer_callback_ = std::move(callback);
}
void WebRtcTransport::SetAnswerCallback(SdpCallback callback) {
  std::scoped_lock lock(callback_mutex_);
  answer_callback_ = std::move(callback);
}
void WebRtcTransport::SetIceCallback(IceCallback callback) {
  std::scoped_lock lock(callback_mutex_);
  ice_callback_ = std::move(callback);
}
void WebRtcTransport::SetRemoteDescriptionCallback(RemoteDescriptionCallback callback) {
  std::scoped_lock lock(callback_mutex_);
  remote_description_callback_ = std::move(callback);
}
void WebRtcTransport::CreateOffer() {
  if (reinterpret_cast<CreateOfferFunction>(create_offer_)(factory_) != 1U)
    throw std::runtime_error("WebRTC could not create offer");
}
void WebRtcTransport::RestartIce() {
  if (reinterpret_cast<CreateOfferFunction>(restart_ice_)(factory_) != 1U)
    throw std::runtime_error("WebRTC could not restart ICE");
}
void WebRtcTransport::ApplyRemoteOffer(std::string sdp) {
  if (reinterpret_cast<ApplySdpFunction>(apply_offer_)(
          factory_, sdp.data(), static_cast<std::uint32_t>(sdp.size())) != 1U)
    throw std::runtime_error("WebRTC rejected remote offer");
}
void WebRtcTransport::ApplyRemoteAnswer(std::string sdp) {
  if (reinterpret_cast<ApplySdpFunction>(apply_answer_)(
          factory_, sdp.data(), static_cast<std::uint32_t>(sdp.size())) != 1U)
    throw std::runtime_error("WebRTC rejected remote answer");
}
void WebRtcTransport::ApplyRemoteIceCandidate(const IceCandidate& candidate) {
  if (reinterpret_cast<ApplyIceFunction>(apply_ice_)(
          factory_, candidate.mid.data(), static_cast<std::uint32_t>(candidate.mid.size()),
          candidate.mline_index, candidate.candidate.data(),
          static_cast<std::uint32_t>(candidate.candidate.size())) != 1U)
    throw std::runtime_error("WebRTC rejected remote ICE candidate");
}
bool WebRtcTransport::IsReady() const {
  return reinterpret_cast<IsReadyFunction>(is_ready_)(factory_) == 1U;
}
std::string WebRtcTransport::DiagnosticState() const {
  using StateFunction = std::uint32_t(__cdecl*)(void*);
  const auto module = static_cast<HMODULE>(module_);
  return "connection=" + std::to_string(Lookup<StateFunction>(module, "VeritasSyncWebRtcBridgeConnectionState")(factory_)) +
         ", control=" + std::to_string(Lookup<StateFunction>(module, "VeritasSyncWebRtcBridgeControlChannelState")(factory_));
}
void __cdecl WebRtcTransport::Receive(void* context, const std::uint32_t channel,
                                      const std::uint8_t* bytes, const std::uint32_t length) {
  auto* const self = static_cast<WebRtcTransport*>(context);
  ReceiveCallback callback;
  {
    std::scoped_lock lock(self->callback_mutex_);
    callback = self->callback_;
  }
  if (callback != nullptr && bytes != nullptr && length > 0 && (channel == 1U || channel == 2U)) {
    try {
      std::optional<std::vector<std::uint8_t>> frame;
      { std::scoped_lock lock(self->wire_mutex_); frame = self->reassembly_[channel - 1].Accept({bytes, length}); }
      if (frame) callback(channel == 1U ? protocol::Channel::kControl : protocol::Channel::kBulk, std::move(*frame));
    } catch (const std::exception& error) {
      std::scoped_lock lock(self->wire_mutex_);
      self->wire_error_ = error.what();
    }
  }
}
void __cdecl WebRtcTransport::ReceiveOffer(void* context, const char* sdp,
                                           const std::uint32_t length) {
  auto* self = static_cast<WebRtcTransport*>(context);
  SdpCallback callback;
  {
    std::scoped_lock lock(self->callback_mutex_);
    callback = self->offer_callback_;
  }
  if (callback != nullptr) callback(std::string(sdp, length));
}
void __cdecl WebRtcTransport::ReceiveAnswer(void* context, const char* sdp,
                                            const std::uint32_t length) {
  auto* self = static_cast<WebRtcTransport*>(context);
  SdpCallback callback;
  {
    std::scoped_lock lock(self->callback_mutex_);
    callback = self->answer_callback_;
  }
  if (callback != nullptr) callback(std::string(sdp, length));
}
void __cdecl WebRtcTransport::ReceiveIce(void* context, const char* mid,
                                         const std::uint32_t mid_length, const std::int32_t index,
                                         const char* candidate,
                                         const std::uint32_t candidate_length) {
  auto* self = static_cast<WebRtcTransport*>(context);
  IceCallback callback;
  {
    std::scoped_lock lock(self->callback_mutex_);
    callback = self->ice_callback_;
  }
  if (callback != nullptr)
    callback({std::string(mid, mid_length), index, std::string(candidate, candidate_length)});
}
void __cdecl WebRtcTransport::ReceiveRemoteDescription(void* context, const std::uint32_t success) {
  auto* self = static_cast<WebRtcTransport*>(context);
  RemoteDescriptionCallback callback;
  {
    std::scoped_lock lock(self->callback_mutex_);
    callback = self->remote_description_callback_;
  }
  if (callback != nullptr) callback(success == 1U);
}
}  // namespace veritassync::transport
