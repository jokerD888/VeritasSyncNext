#pragma once

#include "engine/security/device_identity.h"
#include "engine/signaling/tracker_contract.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veritassync::signaling {

struct TrackerHttpResponse {
  std::uint16_t status = 0;
  std::string body;
};

class TrackerHttpTransport {
 public:
  virtual ~TrackerHttpTransport() = default;
  [[nodiscard]] virtual TrackerHttpResponse Post(
      std::string_view base_url, std::string_view path,
      const std::map<std::string, std::string>& headers, std::string_view body) = 0;
};

class WinHttpTrackerTransport final : public TrackerHttpTransport {
 public:
  [[nodiscard]] TrackerHttpResponse Post(
      std::string_view base_url, std::string_view path,
      const std::map<std::string, std::string>& headers, std::string_view body) override;
};

struct TrackerMember {
  std::string device_id;
  std::string fingerprint;
  protocol::Role role = protocol::Role::kPeer;
};

struct TrackerEnrollment {
  std::string room_id;
  std::string authorization_digest;
  std::string session_token;
  std::int64_t session_expires_at_ms = 0;
  std::vector<TrackerMember> members;
};

struct CreatedInvitation : TrackerEnrollment {
  std::string invitation_code;
};

// Signed HTTPS client for the Tracker's narrow room, invitation and signaling
// API. It also implements SignalingRelay so SignalingSession remains unaware of
// HTTP, authentication and polling details.
class TrackerClient final : public SignalingRelay {
 public:
  TrackerClient(std::string base_url, const security::DeviceIdentity& identity,
                std::unique_ptr<TrackerHttpTransport> transport =
                    std::make_unique<WinHttpTrackerTransport>());

  [[nodiscard]] CreatedInvitation CreateRoom(std::string_view task_id,
                                              Topology topology,
                                              protocol::Role local_role,
                                              protocol::Role invited_role);
  [[nodiscard]] TrackerEnrollment RedeemInvitation(std::string_view invitation_code,
                                                    std::string_view task_id,
                                                    protocol::Role requested_role);
  [[nodiscard]] TrackerEnrollment JoinRoom(std::string_view room_id,
                                           std::string_view task_id,
                                           protocol::Role role);
  void UseEnrollment(TrackerEnrollment enrollment);
  [[nodiscard]] const std::optional<TrackerEnrollment>& Enrollment() const {
    return enrollment_;
  }

  void Forward(const RelayMessage& message) override;
  [[nodiscard]] std::vector<RelayMessage> DrainInbox(
      const std::string& device_id) override;

  [[nodiscard]] static std::string EncodeField(std::string_view value);
  [[nodiscard]] static std::string DecodeField(std::string_view value);

 private:
  [[nodiscard]] TrackerHttpResponse SignedPost(std::string_view path,
                                               std::string body,
                                               bool include_session);
  [[nodiscard]] TrackerEnrollment ParseEnrollment(std::string_view response) const;

  std::string base_url_;
  const security::DeviceIdentity& identity_;
  std::unique_ptr<TrackerHttpTransport> transport_;
  std::optional<TrackerEnrollment> enrollment_;
};

}  // namespace veritassync::signaling
