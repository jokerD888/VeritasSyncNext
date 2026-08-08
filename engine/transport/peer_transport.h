#pragma once

#include "engine/transport/transport.h"

#include <cstdint>
#include <functional>
#include <string>

namespace veritassync::transport {

// PeerConnection signaling surface used by the Engine session manager. Keeping
// it abstract permits deterministic session tests without loading libwebrtc.
class PeerTransport : public Transport {
 public:
  using SdpCallback = std::function<void(std::string)>;
  using RemoteDescriptionCallback = std::function<void(bool)>;
  struct IceCandidate {
    std::string mid;
    std::int32_t mline_index;
    std::string candidate;
  };
  using IceCallback = std::function<void(IceCandidate)>;

  virtual void SetOfferCallback(SdpCallback callback) = 0;
  virtual void SetAnswerCallback(SdpCallback callback) = 0;
  virtual void SetIceCallback(IceCallback callback) = 0;
  virtual void SetRemoteDescriptionCallback(RemoteDescriptionCallback callback) = 0;
  virtual void CreateOffer() = 0;
  virtual void ApplyRemoteOffer(std::string sdp) = 0;
  virtual void ApplyRemoteAnswer(std::string sdp) = 0;
  virtual void ApplyRemoteIceCandidate(const IceCandidate& candidate) = 0;
  [[nodiscard]] virtual bool IsReady() const = 0;
};

}  // namespace veritassync::transport
