#include "engine/transport/webrtc_bridge_loader.h"

#include <Windows.h>

#include <stdexcept>
#include <string>

namespace veritassync::transport {
namespace {
using AbiVersionFunction = std::uint32_t(__cdecl*)();
using MaxQueuedBytesFunction = std::uint64_t(__cdecl*)();

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
}  // namespace veritassync::transport
