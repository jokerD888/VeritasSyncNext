#include "engine/transport/webrtc_transport.h"

#include "engine/transport/webrtc_bridge_loader.h"

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
using DataCallback = void(__cdecl*)(void*, std::uint32_t, const std::uint8_t*, std::uint32_t);
using SetDataCallbackFunction = void(__cdecl*)(void*, DataCallback, void*);
using SendFunction = std::uint32_t(__cdecl*)(void*, const std::uint8_t*, std::uint32_t);
using SdpCallback = void(__cdecl*)(void*, const char*, std::uint32_t);
using SetSdpCallbackFunction = void(__cdecl*)(void*, SdpCallback, void*);
using IceCallback = void(__cdecl*)(void*, const char*, std::uint32_t, std::int32_t, const char*, std::uint32_t);
using SetIceCallbackFunction = void(__cdecl*)(void*, IceCallback, void*);
using CreateOfferFunction = std::uint32_t(__cdecl*)(void*);
using ApplySdpFunction = std::uint32_t(__cdecl*)(void*, const char*, std::uint32_t);
using ApplyIceFunction = std::uint32_t(__cdecl*)(void*, const char*, std::uint32_t, std::int32_t, const char*, std::uint32_t);
template <typename Function> Function Lookup(const HMODULE module, const char* const name) {
  const auto address = GetProcAddress(module, name);
  if (address == nullptr) throw std::runtime_error(std::string("WebRTC bridge is missing ") + name);
  return reinterpret_cast<Function>(address);
}
}

WebRtcTransport::WebRtcTransport(const std::filesystem::path& bridge_path) {
  module_ = LoadLibraryW(bridge_path.c_str());
  if (module_ == nullptr) throw std::runtime_error("cannot load WebRTC bridge");
  try {
    const auto module = static_cast<HMODULE>(module_);
    const auto abi = Lookup<std::uint32_t(__cdecl*)()>(module, "VeritasSyncWebRtcBridgeAbiVersion");
    if (abi() != WebRtcBridgeLoader::kExpectedAbiVersion) throw std::runtime_error("WebRTC bridge ABI mismatch");
    const auto create = Lookup<CreateFunction>(module, "VeritasSyncWebRtcBridgeCreateFactory");
    destroy_ = reinterpret_cast<void*>(Lookup<DestroyFunction>(module, "VeritasSyncWebRtcBridgeDestroyFactory"));
    factory_ = create();
    if (factory_ == nullptr) throw std::runtime_error("cannot create WebRTC factory");
    if (Lookup<CreateChannelsFunction>(module, "VeritasSyncWebRtcBridgeCreateProtocolChannels")(factory_) != 1U) throw std::runtime_error("cannot create protocol channels");
    Lookup<SetDataCallbackFunction>(module, "VeritasSyncWebRtcBridgeSetDataCallback")(factory_, Receive, this);
    Lookup<SetSdpCallbackFunction>(module, "VeritasSyncWebRtcBridgeSetOfferCallback")(factory_, ReceiveOffer, this);
    Lookup<SetSdpCallbackFunction>(module, "VeritasSyncWebRtcBridgeSetAnswerCallback")(factory_, ReceiveAnswer, this);
    Lookup<SetIceCallbackFunction>(module, "VeritasSyncWebRtcBridgeSetIceCallback")(factory_, ReceiveIce, this);
    send_control_ = reinterpret_cast<void*>(Lookup<SendFunction>(module, "VeritasSyncWebRtcBridgeSendControl"));
    send_bulk_ = reinterpret_cast<void*>(Lookup<SendFunction>(module, "VeritasSyncWebRtcBridgeSendBulk"));
    create_offer_ = reinterpret_cast<void*>(Lookup<CreateOfferFunction>(module, "VeritasSyncWebRtcBridgeCreateOffer"));
    apply_offer_ = reinterpret_cast<void*>(Lookup<ApplySdpFunction>(module, "VeritasSyncWebRtcBridgeApplyRemoteOffer"));
    apply_answer_ = reinterpret_cast<void*>(Lookup<ApplySdpFunction>(module, "VeritasSyncWebRtcBridgeApplyRemoteAnswer"));
    apply_ice_ = reinterpret_cast<void*>(Lookup<ApplyIceFunction>(module, "VeritasSyncWebRtcBridgeApplyRemoteIceCandidate"));
  } catch (...) {
    if (factory_ != nullptr && destroy_ != nullptr) reinterpret_cast<DestroyFunction>(destroy_)(factory_);
    FreeLibrary(static_cast<HMODULE>(module_));
    throw;
  }
}
WebRtcTransport::~WebRtcTransport() {
  if (factory_ != nullptr && destroy_ != nullptr) reinterpret_cast<DestroyFunction>(destroy_)(factory_);
  if (module_ != nullptr) FreeLibrary(static_cast<HMODULE>(module_));
}
void WebRtcTransport::Send(const protocol::Channel channel, std::vector<std::uint8_t> wire) {
  if (wire.empty() || wire.size() > (std::numeric_limits<std::uint32_t>::max)()) throw std::invalid_argument("invalid WebRTC frame");
  const auto send = reinterpret_cast<SendFunction>(channel == protocol::Channel::kControl ? send_control_ : send_bulk_);
  if (send(factory_, wire.data(), static_cast<std::uint32_t>(wire.size())) != 1U) throw std::runtime_error("WebRTC DataChannel rejected frame");
}
void WebRtcTransport::SetReceiveCallback(ReceiveCallback callback) { std::scoped_lock lock(callback_mutex_); callback_ = std::move(callback); }
void WebRtcTransport::SetOfferCallback(SdpCallback callback) { std::scoped_lock lock(callback_mutex_); offer_callback_ = std::move(callback); }
void WebRtcTransport::SetAnswerCallback(SdpCallback callback) { std::scoped_lock lock(callback_mutex_); answer_callback_ = std::move(callback); }
void WebRtcTransport::SetIceCallback(IceCallback callback) { std::scoped_lock lock(callback_mutex_); ice_callback_ = std::move(callback); }
void WebRtcTransport::CreateOffer() { if (reinterpret_cast<CreateOfferFunction>(create_offer_)(factory_) != 1U) throw std::runtime_error("WebRTC could not create offer"); }
void WebRtcTransport::ApplyRemoteOffer(std::string sdp) { if (reinterpret_cast<ApplySdpFunction>(apply_offer_)(factory_, sdp.data(), static_cast<std::uint32_t>(sdp.size())) != 1U) throw std::runtime_error("WebRTC rejected remote offer"); }
void WebRtcTransport::ApplyRemoteAnswer(std::string sdp) { if (reinterpret_cast<ApplySdpFunction>(apply_answer_)(factory_, sdp.data(), static_cast<std::uint32_t>(sdp.size())) != 1U) throw std::runtime_error("WebRTC rejected remote answer"); }
void WebRtcTransport::ApplyRemoteIceCandidate(const IceCandidate& candidate) { if (reinterpret_cast<ApplyIceFunction>(apply_ice_)(factory_, candidate.mid.data(), static_cast<std::uint32_t>(candidate.mid.size()), candidate.mline_index, candidate.candidate.data(), static_cast<std::uint32_t>(candidate.candidate.size())) != 1U) throw std::runtime_error("WebRTC rejected remote ICE candidate"); }
void __cdecl WebRtcTransport::Receive(void* context, const std::uint32_t channel, const std::uint8_t* bytes, const std::uint32_t length) {
  auto* const self = static_cast<WebRtcTransport*>(context);
  ReceiveCallback callback;
  { std::scoped_lock lock(self->callback_mutex_); callback = self->callback_; }
  if (callback != nullptr && bytes != nullptr && length > 0 && (channel == 1U || channel == 2U)) {
    callback(channel == 1U ? protocol::Channel::kControl : protocol::Channel::kBulk, {bytes, bytes + length});
  }
}
void __cdecl WebRtcTransport::ReceiveOffer(void* context, const char* sdp, const std::uint32_t length) { auto* self=static_cast<WebRtcTransport*>(context); SdpCallback callback; { std::scoped_lock lock(self->callback_mutex_); callback=self->offer_callback_; } if (callback!=nullptr) callback(std::string(sdp,length)); }
void __cdecl WebRtcTransport::ReceiveAnswer(void* context, const char* sdp, const std::uint32_t length) { auto* self=static_cast<WebRtcTransport*>(context); SdpCallback callback; { std::scoped_lock lock(self->callback_mutex_); callback=self->answer_callback_; } if (callback!=nullptr) callback(std::string(sdp,length)); }
void __cdecl WebRtcTransport::ReceiveIce(void* context, const char* mid, const std::uint32_t mid_length, const std::int32_t index, const char* candidate, const std::uint32_t candidate_length) { auto* self=static_cast<WebRtcTransport*>(context); IceCallback callback; { std::scoped_lock lock(self->callback_mutex_); callback=self->ice_callback_; } if(callback!=nullptr) callback({std::string(mid,mid_length),index,std::string(candidate,candidate_length)}); }
}  // namespace veritassync::transport
