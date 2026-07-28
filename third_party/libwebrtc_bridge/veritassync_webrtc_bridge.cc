#include "veritassync_webrtc_bridge.h"

#include <memory>
#include <mutex>
#include <string>

#include "api/create_modular_peer_connection_factory.h"
#include "api/data_channel_interface.h"
#include "api/enable_media_with_defaults.h"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

namespace {
class PeerObserver final : public webrtc::PeerConnectionObserver {
 public:
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
  void OnIceCandidate(const webrtc::IceCandidate*) override {}
};

class FactoryHolder final {
 public:
  FactoryHolder() {
    if (!webrtc::InitializeSSL()) return;
    network_thread_ = webrtc::Thread::CreateWithSocketServer();
    worker_thread_ = webrtc::Thread::Create();
    signaling_thread_ = webrtc::Thread::Create();
    if (!network_thread_->Start() || !worker_thread_->Start() || !signaling_thread_->Start()) return;

    webrtc::PeerConnectionFactoryDependencies dependencies;
    dependencies.env = webrtc::CreateEnvironment();
    dependencies.network_thread = network_thread_.get();
    dependencies.worker_thread = worker_thread_.get();
    dependencies.signaling_thread = signaling_thread_.get();
    webrtc::EnableMediaWithDefaults(dependencies);
    factory_ = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));
  }

  [[nodiscard]] bool Ready() const { return factory_ != nullptr; }

  [[nodiscard]] bool CreateProtocolChannels() {
    if (!CreatePeerConnection() || control_channel_ != nullptr || bulk_channel_ != nullptr) return false;

    webrtc::DataChannelInit control_init;
    control_init.ordered = true;
    auto control = peer_connection_->CreateDataChannelOrError("control-v1", &control_init);
    if (!control.ok()) return false;
    control_channel_ = control.MoveValue();

    webrtc::DataChannelInit bulk_init;
    bulk_init.ordered = false;
    auto bulk = peer_connection_->CreateDataChannelOrError("bulk-v1", &bulk_init);
    if (!bulk.ok()) return false;
    bulk_channel_ = bulk.MoveValue();
    return control_channel_->label() == "control-v1" && control_channel_->ordered() &&
           bulk_channel_->label() == "bulk-v1" && !bulk_channel_->ordered();
  }

  [[nodiscard]] bool CreatePeerConnection() {
    if (!Ready()) return false;
    if (peer_connection_ != nullptr) return true;
    webrtc::PeerConnectionInterface::RTCConfiguration configuration;
    configuration.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    auto peer_connection = factory_->CreatePeerConnectionOrError(
        configuration, webrtc::PeerConnectionDependencies(&peer_observer_));
    if (!peer_connection.ok()) return false;
    peer_connection_ = peer_connection.MoveValue();
    return true;
  }

  void SetOfferCallback(VsyncWebRtcBridgeSdpCallback callback, void* context) {
    std::scoped_lock lock(callback_mutex_);
    offer_callback_ = callback;
    offer_context_ = context;
  }
  void SetAnswerCallback(VsyncWebRtcBridgeSdpCallback callback, void* context) {
    std::scoped_lock lock(callback_mutex_);
    answer_callback_ = callback;
    answer_context_ = context;
  }

  [[nodiscard]] bool CreateOffer();
  [[nodiscard]] bool ApplyRemoteOffer(const char* sdp, uint32_t length);
  [[nodiscard]] bool ApplyRemoteAnswer(const char* sdp, uint32_t length);

 private:
  class SetLocalDescriptionObserver : public webrtc::SetSessionDescriptionObserver {
   public:
    SetLocalDescriptionObserver(FactoryHolder& owner, std::string sdp, bool offer)
        : owner_(owner), sdp_(std::move(sdp)), offer_(offer) {}
    void OnSuccess() override {
      if (offer_) owner_.EmitOffer(sdp_);
      else owner_.EmitAnswer(sdp_);
    }
    void OnFailure(webrtc::RTCError) override {}
   private:
    FactoryHolder& owner_;
    std::string sdp_;
    bool offer_;
  };

  class DescriptionObserver : public webrtc::CreateSessionDescriptionObserver {
   public:
    DescriptionObserver(FactoryHolder& owner, bool offer) : owner_(owner), offer_(offer) {}
    void OnSuccess(webrtc::SessionDescriptionInterface* description) override {
      std::string sdp;
      description->ToString(&sdp);
      owner_.peer_connection_->SetLocalDescription(
          webrtc::make_ref_counted<SetLocalDescriptionObserver>(owner_, std::move(sdp), offer_).get(), description);
    }
    void OnFailure(webrtc::RTCError) override {}

   private:
    FactoryHolder& owner_;
    bool offer_;
  };

  class RemoteOfferObserver : public webrtc::SetRemoteDescriptionObserverInterface {
   public:
    explicit RemoteOfferObserver(FactoryHolder& owner) : owner_(owner) {}
    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
      if (error.ok()) owner_.CreateAnswer();
    }
   private:
    FactoryHolder& owner_;
  };

  class RemoteAnswerObserver : public webrtc::SetRemoteDescriptionObserverInterface {
   public:
    void OnSetRemoteDescriptionComplete(webrtc::RTCError) override {}
  };

  void EmitOffer(const std::string& sdp) {
    std::scoped_lock lock(callback_mutex_);
    if (offer_callback_ != nullptr) offer_callback_(offer_context_, sdp.data(), static_cast<uint32_t>(sdp.size()));
  }
  void EmitAnswer(const std::string& sdp) {
    std::scoped_lock lock(callback_mutex_);
    if (answer_callback_ != nullptr) answer_callback_(answer_context_, sdp.data(), static_cast<uint32_t>(sdp.size()));
  }
  void CreateAnswer() {
    peer_connection_->CreateAnswer(webrtc::make_ref_counted<DescriptionObserver>(*this, false).get(),
                                   webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  }

  std::unique_ptr<webrtc::Thread> network_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> signaling_thread_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  PeerObserver peer_observer_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> control_channel_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> bulk_channel_;
  std::mutex callback_mutex_;
  VsyncWebRtcBridgeSdpCallback offer_callback_ = nullptr;
  void* offer_context_ = nullptr;
  VsyncWebRtcBridgeSdpCallback answer_callback_ = nullptr;
  void* answer_context_ = nullptr;
};

bool FactoryHolder::CreateOffer() {
  if (peer_connection_ == nullptr) return false;
  peer_connection_->CreateOffer(webrtc::make_ref_counted<DescriptionObserver>(*this, true).get(),
                                webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  return true;
}

bool FactoryHolder::ApplyRemoteOffer(const char* sdp, uint32_t length) {
  if (peer_connection_ == nullptr || sdp == nullptr) return false;
  auto description = webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, std::string(sdp, length));
  if (description == nullptr) return false;
  peer_connection_->SetRemoteDescription(std::move(description),
                                         webrtc::make_ref_counted<RemoteOfferObserver>(*this));
  return true;
}

bool FactoryHolder::ApplyRemoteAnswer(const char* sdp, uint32_t length) {
  if (peer_connection_ == nullptr || sdp == nullptr) return false;
  auto description = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, std::string(sdp, length));
  if (description == nullptr) return false;
  peer_connection_->SetRemoteDescription(std::move(description),
                                         webrtc::make_ref_counted<RemoteAnswerObserver>());
  return true;
}
}  // namespace

extern "C" uint32_t VeritasSyncWebRtcBridgeAbiVersion(void) {
  return VSYNC_WEBRTC_BRIDGE_ABI_VERSION;
}

extern "C" uint64_t VeritasSyncWebRtcBridgeMaxQueuedBytes(void) {
  return webrtc::DataChannelInterface::MaxSendQueueSize();
}

extern "C" void* VeritasSyncWebRtcBridgeCreateFactory(void) {
  auto holder = std::make_unique<FactoryHolder>();
  if (!holder->Ready()) return nullptr;
  return holder.release();
}

extern "C" void VeritasSyncWebRtcBridgeDestroyFactory(void* factory) {
  delete static_cast<FactoryHolder*>(factory);
}

extern "C" uint32_t VeritasSyncWebRtcBridgeCreateProtocolChannels(void* factory) {
  if (factory == nullptr) return 0;
  return static_cast<FactoryHolder*>(factory)->CreateProtocolChannels() ? 1U : 0U;
}

extern "C" void VeritasSyncWebRtcBridgeSetOfferCallback(
    void* factory, VsyncWebRtcBridgeSdpCallback callback, void* context) {
  if (factory != nullptr) static_cast<FactoryHolder*>(factory)->SetOfferCallback(callback, context);
}

extern "C" void VeritasSyncWebRtcBridgeSetAnswerCallback(
    void* factory, VsyncWebRtcBridgeSdpCallback callback, void* context) {
  if (factory != nullptr) static_cast<FactoryHolder*>(factory)->SetAnswerCallback(callback, context);
}

extern "C" uint32_t VeritasSyncWebRtcBridgeCreatePeerConnection(void* factory) {
  if (factory == nullptr) return 0;
  return static_cast<FactoryHolder*>(factory)->CreatePeerConnection() ? 1U : 0U;
}

extern "C" uint32_t VeritasSyncWebRtcBridgeCreateOffer(void* factory) {
  if (factory == nullptr) return 0;
  return static_cast<FactoryHolder*>(factory)->CreateOffer() ? 1U : 0U;
}

extern "C" uint32_t VeritasSyncWebRtcBridgeApplyRemoteOffer(void* factory, const char* sdp, uint32_t length) {
  if (factory == nullptr) return 0;
  return static_cast<FactoryHolder*>(factory)->ApplyRemoteOffer(sdp, length) ? 1U : 0U;
}

extern "C" uint32_t VeritasSyncWebRtcBridgeApplyRemoteAnswer(void* factory, const char* sdp, uint32_t length) {
  if (factory == nullptr) return 0;
  return static_cast<FactoryHolder*>(factory)->ApplyRemoteAnswer(sdp, length) ? 1U : 0U;
}
