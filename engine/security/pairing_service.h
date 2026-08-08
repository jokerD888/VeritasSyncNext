#pragma once

#include "engine/security/device_identity.h"
#include "engine/signaling/tracker_client.h"
#include "engine/storage/database.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace veritassync::security {

struct PairingInvitation {
  std::string token;
  std::string code;
  std::string room_id;
  std::string task_id;
  std::string tracker_url;
  std::string invited_role;
};

struct ParsedInvitation {
  std::string tracker_url;
  std::string task_id;
  std::string topology;
  std::string invited_role;
  std::string code;
  std::string creator_device_id;
  std::string creator_fingerprint;
};

class PairingService {
 public:
  using HttpFactory = std::function<std::unique_ptr<signaling::TrackerHttpTransport>()>;

  PairingService(
      storage::Database& database, DeviceIdentity identity, HttpFactory http_factory = [] {
        return std::make_unique<signaling::WinHttpTrackerTransport>();
      });

  [[nodiscard]] const DeviceIdentity& Identity() const { return identity_; }
  [[nodiscard]] PairingInvitation CreateInvitation(const std::string& task_id,
                                                   const std::string& tracker_url);
  [[nodiscard]] storage::TaskConnection JoinInvitation(const std::string& token,
                                                       const std::string& local_root);
  [[nodiscard]] signaling::TrackerEnrollment RenewTaskEnrollment(const std::string& task_id);
  [[nodiscard]] std::unique_ptr<signaling::TrackerClient> OpenTaskSession(
      const std::string& task_id);

  [[nodiscard]] static ParsedInvitation ParseInvitation(std::string_view token);

 private:
  void PersistEnrollment(const storage::TaskDefinition& task, const std::string& tracker_url,
                         const signaling::TrackerEnrollment& enrollment);

  storage::Database& database_;
  DeviceIdentity identity_;
  HttpFactory http_factory_;
};

}  // namespace veritassync::security
