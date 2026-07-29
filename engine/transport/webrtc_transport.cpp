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
    send_control_ = reinterpret_cast<void*>(Lookup<SendFunction>(module, "VeritasSyncWebRtcBridgeSendControl"));
    send_bulk_ = reinterpret_cast<void*>(Lookup<SendFunction>(module, "VeritasSyncWebRtcBridgeSendBulk"));
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
void WebRtcTransport::SetReceiveCallback(ReceiveCallback callback) { callback_ = std::move(callback); }
void __cdecl WebRtcTransport::Receive(void* context, const std::uint32_t channel, const std::uint8_t* bytes, const std::uint32_t length) {
  auto* const self = static_cast<WebRtcTransport*>(context);
  if (self->callback_ != nullptr && bytes != nullptr && length > 0 && (channel == 1U || channel == 2U)) {
    self->callback_(channel == 1U ? protocol::Channel::kControl : protocol::Channel::kBulk, {bytes, bytes + length});
  }
}
}  // namespace veritassync::transport
