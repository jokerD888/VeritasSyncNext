#include "veritassync_webrtc_bridge.h"

#include <memory>

#include "api/create_modular_peer_connection_factory.h"
#include "api/data_channel_interface.h"
#include "api/enable_media_with_defaults.h"
#include "api/environment/environment_factory.h"
#include "api/peer_connection_interface.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

namespace {
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

 private:
  std::unique_ptr<webrtc::Thread> network_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> signaling_thread_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
};
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
