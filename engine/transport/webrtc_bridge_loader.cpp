#include "engine/transport/webrtc_bridge_loader.h"

#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>

namespace veritassync::transport {
namespace {
using AbiVersionFunction = std::uint32_t(__cdecl*)();
using MaxQueuedBytesFunction = std::uint64_t(__cdecl*)();
using CreateFactoryFunction = void*(__cdecl*)();
using DestroyFactoryFunction = void(__cdecl*)(void*);
using CreateProtocolChannelsFunction = std::uint32_t(__cdecl*)(void*);
using OfferCallback = void(__cdecl*)(void*, const char*, std::uint32_t);
using SetOfferCallbackFunction = void(__cdecl*)(void*, OfferCallback, void*);
using SetAnswerCallbackFunction = void(__cdecl*)(void*, OfferCallback, void*);
using CreatePeerConnectionFunction = std::uint32_t(__cdecl*)(void*);
using CreateOfferFunction = std::uint32_t(__cdecl*)(void*);
using ApplyRemoteDescriptionFunction = std::uint32_t(__cdecl*)(void*, const char*, std::uint32_t);

struct OfferCapture {
  std::mutex mutex;
  std::condition_variable ready;
  std::string sdp;
};

void __cdecl CaptureOffer(void* context, const char* sdp, std::uint32_t length) {
  auto& capture = *static_cast<OfferCapture*>(context);
  {
    std::scoped_lock lock(capture.mutex);
    capture.sdp.assign(sdp, length);
  }
  capture.ready.notify_one();
}

template <typename Function>
Function Lookup(HMODULE module, const char* name) {
  const auto address = GetProcAddress(module, name);
  if (address == nullptr) throw std::runtime_error(std::string("WebRTC bridge is missing ") + name);
  return reinterpret_cast<Function>(address);
}
}  // namespace

std::uint64_t WebRtcBridgeLoader::VerifyAndReadMaxQueuedBytes(
    const std::filesystem::path& library_path) {
  const HMODULE module = LoadLibraryW(library_path.c_str());
  if (module == nullptr) throw std::runtime_error("cannot load WebRTC bridge: " + library_path.string());
  try {
    const auto abi_version = Lookup<AbiVersionFunction>(module, "VeritasSyncWebRtcBridgeAbiVersion")();
    if (abi_version != kExpectedAbiVersion) throw std::runtime_error("WebRTC bridge ABI version mismatch");
    const auto max_queued_bytes = Lookup<MaxQueuedBytesFunction>(module, "VeritasSyncWebRtcBridgeMaxQueuedBytes")();
    FreeLibrary(module);
    return max_queued_bytes;
  } catch (...) {
    FreeLibrary(module);
    throw;
  }
}

void WebRtcBridgeLoader::VerifyFactoryLifecycle(const std::filesystem::path& library_path) {
  const HMODULE module = LoadLibraryW(library_path.c_str());
  if (module == nullptr) throw std::runtime_error("cannot load WebRTC bridge: " + library_path.string());
  try {
    const auto create_factory = Lookup<CreateFactoryFunction>(module, "VeritasSyncWebRtcBridgeCreateFactory");
    const auto destroy_factory = Lookup<DestroyFactoryFunction>(module, "VeritasSyncWebRtcBridgeDestroyFactory");
    const auto create_protocol_channels = Lookup<CreateProtocolChannelsFunction>(module, "VeritasSyncWebRtcBridgeCreateProtocolChannels");
    const auto set_offer_callback = Lookup<SetOfferCallbackFunction>(module, "VeritasSyncWebRtcBridgeSetOfferCallback");
    const auto set_answer_callback = Lookup<SetAnswerCallbackFunction>(module, "VeritasSyncWebRtcBridgeSetAnswerCallback");
    const auto create_peer_connection = Lookup<CreatePeerConnectionFunction>(module, "VeritasSyncWebRtcBridgeCreatePeerConnection");
    const auto create_offer = Lookup<CreateOfferFunction>(module, "VeritasSyncWebRtcBridgeCreateOffer");
    const auto apply_remote_offer = Lookup<ApplyRemoteDescriptionFunction>(module, "VeritasSyncWebRtcBridgeApplyRemoteOffer");
    const auto apply_remote_answer = Lookup<ApplyRemoteDescriptionFunction>(module, "VeritasSyncWebRtcBridgeApplyRemoteAnswer");
    void* const factory = create_factory();
    if (factory == nullptr) throw std::runtime_error("WebRTC bridge could not create a PeerConnectionFactory");
    if (create_protocol_channels(factory) != 1U) {
      destroy_factory(factory);
      throw std::runtime_error("WebRTC bridge could not create control-v1 and bulk-v1 DataChannels");
    }
    OfferCapture capture;
    set_offer_callback(factory, CaptureOffer, &capture);
    if (create_offer(factory) != 1U) {
      destroy_factory(factory);
      throw std::runtime_error("WebRTC bridge could not start an SDP offer");
    }
    {
      std::unique_lock lock(capture.mutex);
      if (!capture.ready.wait_for(lock, std::chrono::seconds(5), [&capture] { return !capture.sdp.empty(); })) {
        destroy_factory(factory);
        throw std::runtime_error("WebRTC bridge did not produce an SDP offer");
      }
      if (capture.sdp.find("m=application") == std::string::npos) {
        destroy_factory(factory);
        throw std::runtime_error("WebRTC SDP offer does not describe DataChannels");
      }
    }
    void* const remote_factory = create_factory();
    if (remote_factory == nullptr || create_peer_connection(remote_factory) != 1U) {
      if (remote_factory != nullptr) destroy_factory(remote_factory);
      destroy_factory(factory);
      throw std::runtime_error("WebRTC bridge could not create the remote PeerConnection");
    }
    OfferCapture answer_capture;
    set_answer_callback(remote_factory, CaptureOffer, &answer_capture);
    if (apply_remote_offer(remote_factory, capture.sdp.data(), static_cast<std::uint32_t>(capture.sdp.size())) != 1U) {
      destroy_factory(remote_factory);
      destroy_factory(factory);
      throw std::runtime_error("WebRTC bridge rejected its own SDP offer");
    }
    {
      std::unique_lock lock(answer_capture.mutex);
      if (!answer_capture.ready.wait_for(lock, std::chrono::seconds(5), [&answer_capture] { return !answer_capture.sdp.empty(); })) {
        destroy_factory(remote_factory);
        destroy_factory(factory);
        throw std::runtime_error("WebRTC bridge did not produce an SDP answer");
      }
    }
    if (apply_remote_answer(factory, answer_capture.sdp.data(), static_cast<std::uint32_t>(answer_capture.sdp.size())) != 1U) {
      destroy_factory(remote_factory);
      destroy_factory(factory);
      throw std::runtime_error("WebRTC bridge rejected its own SDP answer");
    }
    destroy_factory(remote_factory);
    destroy_factory(factory);
    FreeLibrary(module);
  } catch (...) {
    FreeLibrary(module);
    throw;
  }
}
}  // namespace veritassync::transport
