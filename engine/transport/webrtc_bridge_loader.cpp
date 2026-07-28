#include "engine/transport/webrtc_bridge_loader.h"

#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

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
using IceCallback = void(__cdecl*)(void*, const char*, std::uint32_t, std::int32_t, const char*, std::uint32_t);
using SetIceCallbackFunction = void(__cdecl*)(void*, IceCallback, void*);
using ApplyRemoteIceCandidateFunction = std::uint32_t(__cdecl*)(
    void*, const char*, std::uint32_t, std::int32_t, const char*, std::uint32_t);
using CompletionCallback = void(__cdecl*)(void*, std::uint32_t);
using SetRemoteDescriptionCallbackFunction = void(__cdecl*)(void*, CompletionCallback, void*);

struct OfferCapture {
  std::mutex mutex;
  std::condition_variable ready;
  std::string sdp;
};

struct IceCandidate {
  std::string mid;
  std::int32_t mline_index;
  std::string candidate;
};

struct IceCapture {
  std::mutex mutex;
  std::condition_variable ready;
  std::vector<IceCandidate> candidates;
};

struct CompletionCapture {
  std::mutex mutex;
  std::condition_variable ready;
  bool completed = false;
  bool success = false;
};

void __cdecl CaptureOffer(void* context, const char* sdp, std::uint32_t length) {
  auto& capture = *static_cast<OfferCapture*>(context);
  {
    std::scoped_lock lock(capture.mutex);
    capture.sdp.assign(sdp, length);
  }
  capture.ready.notify_one();
}

void __cdecl CaptureIceCandidate(void* context, const char* mid, std::uint32_t mid_length,
                                 std::int32_t mline_index, const char* candidate,
                                 std::uint32_t candidate_length) {
  auto& capture = *static_cast<IceCapture*>(context);
  {
    std::scoped_lock lock(capture.mutex);
    capture.candidates.push_back({std::string(mid, mid_length), mline_index,
                                  std::string(candidate, candidate_length)});
  }
  capture.ready.notify_one();
}

void __cdecl CaptureCompletion(void* context, std::uint32_t success) {
  auto& capture = *static_cast<CompletionCapture*>(context);
  {
    std::scoped_lock lock(capture.mutex);
    capture.completed = true;
    capture.success = success == 1U;
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
    const auto set_ice_callback = Lookup<SetIceCallbackFunction>(module, "VeritasSyncWebRtcBridgeSetIceCallback");
    const auto apply_remote_ice_candidate = Lookup<ApplyRemoteIceCandidateFunction>(module, "VeritasSyncWebRtcBridgeApplyRemoteIceCandidate");
    const auto set_remote_description_callback = Lookup<SetRemoteDescriptionCallbackFunction>(
        module, "VeritasSyncWebRtcBridgeSetRemoteDescriptionCallback");
    void* const factory = create_factory();
    if (factory == nullptr) throw std::runtime_error("WebRTC bridge could not create a PeerConnectionFactory");
    if (create_protocol_channels(factory) != 1U) {
      destroy_factory(factory);
      throw std::runtime_error("WebRTC bridge could not create control-v1 and bulk-v1 DataChannels");
    }
    OfferCapture capture;
    IceCapture offer_ice_capture;
    set_offer_callback(factory, CaptureOffer, &capture);
    set_ice_callback(factory, CaptureIceCandidate, &offer_ice_capture);
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
    IceCapture answer_ice_capture;
    CompletionCapture remote_offer_completion;
    set_answer_callback(remote_factory, CaptureOffer, &answer_capture);
    set_ice_callback(remote_factory, CaptureIceCandidate, &answer_ice_capture);
    set_remote_description_callback(remote_factory, CaptureCompletion, &remote_offer_completion);
    if (apply_remote_offer(remote_factory, capture.sdp.data(), static_cast<std::uint32_t>(capture.sdp.size())) != 1U) {
      destroy_factory(remote_factory);
      destroy_factory(factory);
      throw std::runtime_error("WebRTC bridge rejected its own SDP offer");
    }
    {
      std::unique_lock lock(remote_offer_completion.mutex);
      if (!remote_offer_completion.ready.wait_for(lock, std::chrono::seconds(5), [&remote_offer_completion] {
            return remote_offer_completion.completed;
          }) || !remote_offer_completion.success) {
        destroy_factory(remote_factory);
        destroy_factory(factory);
        throw std::runtime_error("WebRTC bridge did not apply the remote SDP offer");
      }
    }
    {
      std::unique_lock lock(answer_capture.mutex);
      if (!answer_capture.ready.wait_for(lock, std::chrono::seconds(5), [&answer_capture] { return !answer_capture.sdp.empty(); })) {
        destroy_factory(remote_factory);
        destroy_factory(factory);
        throw std::runtime_error("WebRTC bridge did not produce an SDP answer");
      }
    }
    CompletionCapture remote_answer_completion;
    set_remote_description_callback(factory, CaptureCompletion, &remote_answer_completion);
    if (apply_remote_answer(factory, answer_capture.sdp.data(), static_cast<std::uint32_t>(answer_capture.sdp.size())) != 1U) {
      destroy_factory(remote_factory);
      destroy_factory(factory);
      throw std::runtime_error("WebRTC bridge rejected its own SDP answer");
    }
    {
      std::unique_lock lock(remote_answer_completion.mutex);
      if (!remote_answer_completion.ready.wait_for(lock, std::chrono::seconds(5), [&remote_answer_completion] {
            return remote_answer_completion.completed;
          }) || !remote_answer_completion.success) {
        destroy_factory(remote_factory);
        destroy_factory(factory);
        throw std::runtime_error("WebRTC bridge did not apply the remote SDP answer");
      }
    }
    auto wait_for_candidate = [](IceCapture& capture) {
      std::unique_lock lock(capture.mutex);
      return capture.ready.wait_for(lock, std::chrono::seconds(5), [&capture] { return !capture.candidates.empty(); });
    };
    if (!wait_for_candidate(offer_ice_capture) || !wait_for_candidate(answer_ice_capture)) {
      destroy_factory(remote_factory);
      destroy_factory(factory);
      throw std::runtime_error("WebRTC bridge did not gather local ICE candidates");
    }
    Sleep(1000);
    std::vector<IceCandidate> offer_candidates;
    std::vector<IceCandidate> answer_candidates;
    {
      std::scoped_lock lock(offer_ice_capture.mutex);
      offer_candidates = offer_ice_capture.candidates;
    }
    {
      std::scoped_lock lock(answer_ice_capture.mutex);
      answer_candidates = answer_ice_capture.candidates;
    }
    for (const auto& candidate : offer_candidates) {
      if (apply_remote_ice_candidate(remote_factory, candidate.mid.data(), static_cast<std::uint32_t>(candidate.mid.size()),
                                     candidate.mline_index, candidate.candidate.data(),
                                     static_cast<std::uint32_t>(candidate.candidate.size())) != 1U) {
        destroy_factory(remote_factory);
        destroy_factory(factory);
        throw std::runtime_error("WebRTC bridge rejected an offer-side ICE candidate");
      }
    }
    for (const auto& candidate : answer_candidates) {
      if (apply_remote_ice_candidate(factory, candidate.mid.data(), static_cast<std::uint32_t>(candidate.mid.size()),
                                     candidate.mline_index, candidate.candidate.data(),
                                     static_cast<std::uint32_t>(candidate.candidate.size())) != 1U) {
        destroy_factory(remote_factory);
        destroy_factory(factory);
        throw std::runtime_error("WebRTC bridge rejected an answer-side ICE candidate");
      }
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
