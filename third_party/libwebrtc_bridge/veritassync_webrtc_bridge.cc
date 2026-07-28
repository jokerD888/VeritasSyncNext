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
class FactoryHolder;

class PeerObserver final : public webrtc::PeerConnectionObserver {
 public:
  explicit PeerObserver(FactoryHolder& owner) : owner_(owner) {}
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
  void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
 private:
  FactoryHolder& owner_;
};

class FactoryHolder final {
 public:
  FactoryHolder() : peer_observer_(*this) {
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
    AttachControlChannel(control.MoveValue());

    webrtc::DataChannelInit bulk_init;
    bulk_init.ordered = false;
    auto bulk = peer_connection_->CreateDataChannelOrError("bulk-v1", &bulk_init);
    if (!bulk.ok()) return false;
    AttachBulkChannel(bulk.MoveValue());
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
  void SetIceCallback(VsyncWebRtcBridgeIceCallback callback, void* context) {
    std::scoped_lock lock(callback_mutex_);
    ice_callback_ = callback;
    ice_context_ = context;
  }
  void SetDataCallback(VsyncWebRtcBridgeDataCallback callback, void* context) {
    std::scoped_lock lock(callback_mutex_);
    data_callback_ = callback;
    data_context_ = context;
  }
  void SetRemoteDescriptionCallback(VsyncWebRtcBridgeCompletionCallback callback, void* context) {
    std::scoped_lock lock(callback_mutex_);
    remote_description_callback_ = callback;
    remote_description_context_ = context;
  }

  [[nodiscard]] bool CreateOffer();
  [[nodiscard]] bool ApplyRemoteOffer(const char* sdp, uint32_t length);
  [[nodiscard]] bool ApplyRemoteAnswer(const char* sdp, uint32_t length);
  [[nodiscard]] bool ApplyRemoteIceCandidate(const char* mid, uint32_t mid_length,
                                              int32_t mline_index, const char* candidate,
                                              uint32_t candidate_length);
  [[nodiscard]] bool SendControl(const uint8_t* bytes, uint32_t length) {
    if (control_channel_ == nullptr || bytes == nullptr || length == 0) return false;
    return control_channel_->Send(webrtc::DataBuffer(webrtc::CopyOnWriteBuffer(bytes, length), true));
  }
  [[nodiscard]] uint32_t ControlChannelState() const {
    if (control_channel_ == nullptr) return 0;
    return static_cast<uint32_t>(control_channel_->state()) + 1U;
  }
  [[nodiscard]] uint32_t ConnectionState() const {
    if (peer_connection_ == nullptr) return 0;
    return static_cast<uint32_t>(peer_connection_->peer_connection_state()) + 1U;
  }

  ~FactoryHolder() {
    if (control_channel_ != nullptr) control_channel_->UnregisterObserver();
    if (bulk_channel_ != nullptr) bulk_channel_->UnregisterObserver();
    if (peer_connection_ != nullptr) peer_connection_->Close();
  }

 private:
  friend class PeerObserver;
  enum class DataChannelKind : uint32_t { kControl = 1, kBulk = 2 };

  class DataObserver : public webrtc::DataChannelObserver {
   public:
    DataObserver(FactoryHolder& owner, DataChannelKind kind) : owner_(owner), kind_(kind) {}
    void OnStateChange() override {}
    void OnMessage(const webrtc::DataBuffer& buffer) override {
      owner_.EmitData(kind_, buffer.data.data(), static_cast<uint32_t>(buffer.data.size()));
    }
   private:
    FactoryHolder& owner_;
    DataChannelKind kind_;
  };
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
      owner_.EmitRemoteDescriptionComplete(error.ok());
      if (error.ok()) owner_.CreateAnswer();
    }
   private:
    FactoryHolder& owner_;
  };

  class RemoteAnswerObserver : public webrtc::SetRemoteDescriptionObserverInterface {
   public:
    explicit RemoteAnswerObserver(FactoryHolder& owner) : owner_(owner) {}
    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
      owner_.EmitRemoteDescriptionComplete(error.ok());
    }
   private:
    FactoryHolder& owner_;
  };

  void EmitOffer(const std::string& sdp) {
    std::scoped_lock lock(callback_mutex_);
    if (offer_callback_ != nullptr) offer_callback_(offer_context_, sdp.data(), static_cast<uint32_t>(sdp.size()));
  }
  void EmitAnswer(const std::string& sdp) {
    std::scoped_lock lock(callback_mutex_);
    if (answer_callback_ != nullptr) answer_callback_(answer_context_, sdp.data(), static_cast<uint32_t>(sdp.size()));
  }
  void EmitIceCandidate(const webrtc::IceCandidate& ice_candidate) {
    std::string candidate;
    if (!ice_candidate.ToString(&candidate)) return;
    const std::string mid = ice_candidate.sdp_mid();
    VsyncWebRtcBridgeIceCallback callback = nullptr;
    void* context = nullptr;
    {
      std::scoped_lock lock(callback_mutex_);
      callback = ice_callback_;
      context = ice_context_;
    }
    if (callback != nullptr) {
      callback(context, mid.data(), static_cast<uint32_t>(mid.size()), ice_candidate.sdp_mline_index(),
               candidate.data(), static_cast<uint32_t>(candidate.size()));
    }
  }
  void EmitData(DataChannelKind kind, const uint8_t* bytes, uint32_t length) {
    VsyncWebRtcBridgeDataCallback callback = nullptr;
    void* context = nullptr;
    {
      std::scoped_lock lock(callback_mutex_);
      callback = data_callback_;
      context = data_context_;
    }
    if (callback != nullptr) callback(context, static_cast<uint32_t>(kind), bytes, length);
  }
  void EmitRemoteDescriptionComplete(bool success) {
    VsyncWebRtcBridgeCompletionCallback callback = nullptr;
    void* context = nullptr;
    {
      std::scoped_lock lock(callback_mutex_);
      callback = remote_description_callback_;
      context = remote_description_context_;
    }
    if (callback != nullptr) callback(context, success ? 1U : 0U);
  }
  void AttachControlChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
    control_channel_ = std::move(channel);
    control_observer_ = std::make_unique<DataObserver>(*this, DataChannelKind::kControl);
    control_channel_->RegisterObserver(control_observer_.get());
  }
  void AttachBulkChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
    bulk_channel_ = std::move(channel);
    bulk_observer_ = std::make_unique<DataObserver>(*this, DataChannelKind::kBulk);
    bulk_channel_->RegisterObserver(bulk_observer_.get());
  }
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
    if (channel->label() == "control-v1" && control_channel_ == nullptr) AttachControlChannel(std::move(channel));
    else if (channel->label() == "bulk-v1" && bulk_channel_ == nullptr) AttachBulkChannel(std::move(channel));
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
  std::unique_ptr<DataObserver> control_observer_;
  std::unique_ptr<DataObserver> bulk_observer_;
  std::mutex callback_mutex_;
  VsyncWebRtcBridgeSdpCallback offer_callback_ = nullptr;
  void* offer_context_ = nullptr;
  VsyncWebRtcBridgeSdpCallback answer_callback_ = nullptr;
  void* answer_context_ = nullptr;
  VsyncWebRtcBridgeIceCallback ice_callback_ = nullptr;
  void* ice_context_ = nullptr;
  VsyncWebRtcBridgeDataCallback data_callback_ = nullptr;
  void* data_context_ = nullptr;
  VsyncWebRtcBridgeCompletionCallback remote_description_callback_ = nullptr;
  void* remote_description_context_ = nullptr;
};

void PeerObserver::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  owner_.OnDataChannel(std::move(channel));
}

void PeerObserver::OnIceCandidate(const webrtc::IceCandidate* candidate) {
  if (candidate != nullptr) owner_.EmitIceCandidate(*candidate);
}

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
                                         webrtc::make_ref_counted<RemoteAnswerObserver>(*this));
  return true;
}

bool FactoryHolder::ApplyRemoteIceCandidate(const char* mid, uint32_t mid_length,
                                             int32_t mline_index, const char* candidate,
                                             uint32_t candidate_length) {
  if (peer_connection_ == nullptr || mid == nullptr || candidate == nullptr) return false;
  webrtc::SdpParseError error;
  std::unique_ptr<webrtc::IceCandidate> parsed(webrtc::CreateIceCandidate(
      std::string(mid, mid_length), mline_index, std::string(candidate, candidate_length), &error));
  return parsed != nullptr && peer_connection_->AddIceCandidate(parsed.get());
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

extern "C" void VeritasSyncWebRtcBridgeSetIceCallback(
    void* factory, VsyncWebRtcBridgeIceCallback callback, void* context) {
  if (factory != nullptr) static_cast<FactoryHolder*>(factory)->SetIceCallback(callback, context);
}

extern "C" void VeritasSyncWebRtcBridgeSetDataCallback(
    void* factory, VsyncWebRtcBridgeDataCallback callback, void* context) {
  if (factory != nullptr) static_cast<FactoryHolder*>(factory)->SetDataCallback(callback, context);
}

extern "C" void VeritasSyncWebRtcBridgeSetRemoteDescriptionCallback(
    void* factory, VsyncWebRtcBridgeCompletionCallback callback, void* context) {
  if (factory != nullptr) {
    static_cast<FactoryHolder*>(factory)->SetRemoteDescriptionCallback(callback, context);
  }
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

extern "C" uint32_t VeritasSyncWebRtcBridgeApplyRemoteIceCandidate(
    void* factory, const char* mid, uint32_t mid_length, int32_t mline_index,
    const char* candidate, uint32_t candidate_length) {
  if (factory == nullptr) return 0;
  return static_cast<FactoryHolder*>(factory)->ApplyRemoteIceCandidate(
      mid, mid_length, mline_index, candidate, candidate_length) ? 1U : 0U;
}

extern "C" uint32_t VeritasSyncWebRtcBridgeSendControl(
    void* factory, const uint8_t* bytes, uint32_t length) {
  if (factory == nullptr) return 0;
  return static_cast<FactoryHolder*>(factory)->SendControl(bytes, length) ? 1U : 0U;
}

extern "C" uint32_t VeritasSyncWebRtcBridgeControlChannelState(void* factory) {
  if (factory == nullptr) return 0;
  return static_cast<FactoryHolder*>(factory)->ControlChannelState();
}

extern "C" uint32_t VeritasSyncWebRtcBridgeConnectionState(void* factory) {
  if (factory == nullptr) return 0;
  return static_cast<FactoryHolder*>(factory)->ConnectionState();
}
