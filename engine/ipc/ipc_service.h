#pragma once

#include "engine/storage/database.h"

#include <string>
#include <string_view>
#include <functional>
#include "engine/storage/ignore_policy.h"

namespace veritassync::security {
class PairingService;
}

namespace veritassync::runtime {
class NetworkSessionManager;
class TaskRuntimeManager;
}  // namespace veritassync::runtime

namespace veritassync::ipc {

inline constexpr std::string_view kIpcProtocol = "VSYNC_IPC/1";

// Stateless command router for the desktop shell. Requests and responses are
// UTF-8 tab-separated records with percent escaping; every request is bounded
// to one named-pipe message and carries kIpcProtocol as its first field.
class IpcService {
 public:
  using PeerPolicyProposer = std::function<void(const std::string&, std::string, std::string, std::string)>;
  explicit IpcService(storage::Database& database, security::PairingService* pairing = nullptr,
                      runtime::TaskRuntimeManager* runtime = nullptr,
                      runtime::NetworkSessionManager* network = nullptr,
                      PeerPolicyProposer policy_proposer = {});
  [[nodiscard]] std::string Handle(std::string_view request, bool* should_shutdown = nullptr);

 private:
  storage::Database& database_;
  security::PairingService* pairing_;
  runtime::TaskRuntimeManager* runtime_;
  runtime::NetworkSessionManager* network_;
  PeerPolicyProposer policy_proposer_;
  [[nodiscard]] storage::IgnorePolicyState NegotiatePolicy(const storage::TaskDefinition& task,
      const std::string& expected_hash, const std::string& rules, const std::string& source);
};

}  // namespace veritassync::ipc
