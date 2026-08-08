#include "engine/security/pairing_service.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

namespace veritassync::security {
namespace {

std::int64_t NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string RoleName(const protocol::Role role) {
  switch (role) {
    case protocol::Role::kSource:
      return "source";
    case protocol::Role::kTarget:
      return "target";
    case protocol::Role::kPeer:
      return "peer";
  }
  throw std::invalid_argument("invalid pairing role");
}

protocol::Role ParseRole(const std::string_view role) {
  if (role == "source") return protocol::Role::kSource;
  if (role == "target") return protocol::Role::kTarget;
  if (role == "peer") return protocol::Role::kPeer;
  throw std::invalid_argument("invitation contains an invalid role");
}

std::vector<std::string_view> Split(const std::string_view value, const char separator) {
  std::vector<std::string_view> fields;
  std::size_t first = 0;
  while (first <= value.size()) {
    const auto next = value.find(separator, first);
    fields.push_back(
        value.substr(first, next == std::string_view::npos ? value.size() - first : next - first));
    if (next == std::string_view::npos) break;
    first = next + 1;
  }
  return fields;
}

std::string InvitationToken(const ParsedInvitation& invitation) {
  auto encode = signaling::TrackerClient::EncodeField;
  return "VSINVITE1|" + encode(invitation.tracker_url) + "|" + encode(invitation.task_id) + "|" +
         encode(invitation.topology) + "|" + encode(invitation.invited_role) + "|" +
         encode(invitation.code) + "|" + encode(invitation.creator_device_id) + "|" +
         encode(invitation.creator_fingerprint);
}

}  // namespace

PairingService::PairingService(storage::Database& database, DeviceIdentity identity,
                               HttpFactory http_factory)
    : database_(database), identity_(std::move(identity)), http_factory_(std::move(http_factory)) {
  if (!http_factory_) throw std::invalid_argument("pairing HTTP factory is required");
}

PairingInvitation PairingService::CreateInvitation(const std::string& task_id,
                                                   const std::string& tracker_url) {
  std::optional<storage::TaskDefinition> task;
  std::optional<storage::TaskConnection> connection;
  {
    std::scoped_lock database_lock(database_.AccessMutex());
    task = database_.FindTask(task_id);
    connection = database_.FindTaskConnection(task_id);
  }
  if (!task.has_value()) throw std::invalid_argument("task does not exist");
  signaling::Topology topology;
  protocol::Role local_role;
  protocol::Role invited_role;
  if (task->mode == "one_way" && task->role == "source") {
    topology = signaling::Topology::kOneWay;
    local_role = protocol::Role::kSource;
    invited_role = protocol::Role::kTarget;
  } else if (task->mode == "bidirectional" && task->role == "peer") {
    topology = signaling::Topology::kBidirectional;
    local_role = protocol::Role::kPeer;
    invited_role = protocol::Role::kPeer;
  } else {
    throw std::invalid_argument("only a source or bidirectional peer can create an invitation");
  }
  std::string code;
  std::string room_id;
  if (connection.has_value()) {
    if (connection->tracker_url != tracker_url) {
      throw std::invalid_argument("paired task must keep using its existing Tracker");
    }
    auto client = OpenTaskSession(task_id);
    room_id = connection->room_id;
    code = client->CreateInvitation(room_id, invited_role);
  } else {
    signaling::TrackerClient client(tracker_url, identity_, http_factory_());
    const auto created = client.CreateRoom(task_id, topology, local_role, invited_role);
    PersistEnrollment(*task, tracker_url, created);
    room_id = created.room_id;
    code = created.invitation_code;
  }
  ParsedInvitation parsed{tracker_url,
                          task_id,
                          task->mode,
                          RoleName(invited_role),
                          code,
                          identity_.DeviceId(),
                          identity_.Fingerprint()};
  return {InvitationToken(parsed), code, room_id, task_id, tracker_url, parsed.invited_role};
}

ParsedInvitation PairingService::ParseInvitation(const std::string_view token) {
  const auto fields = Split(token, '|');
  if (fields.size() != 8 || fields[0] != "VSINVITE1") {
    throw std::invalid_argument("invitation token is invalid");
  }
  ParsedInvitation invitation{
      signaling::TrackerClient::DecodeField(fields[1]),
      signaling::TrackerClient::DecodeField(fields[2]),
      signaling::TrackerClient::DecodeField(fields[3]),
      signaling::TrackerClient::DecodeField(fields[4]),
      signaling::TrackerClient::DecodeField(fields[5]),
      signaling::TrackerClient::DecodeField(fields[6]),
      signaling::TrackerClient::DecodeField(fields[7]),
  };
  if (invitation.tracker_url.empty() || invitation.task_id.empty() || invitation.code.empty() ||
      invitation.creator_device_id.size() != 32 || invitation.creator_fingerprint.size() != 64 ||
      ((invitation.topology == "one_way" && invitation.invited_role != "target") ||
       (invitation.topology == "bidirectional" && invitation.invited_role != "peer") ||
       (invitation.topology != "one_way" && invitation.topology != "bidirectional"))) {
    throw std::invalid_argument("invitation token fields are invalid");
  }
  return invitation;
}

storage::TaskConnection PairingService::JoinInvitation(const std::string& token,
                                                       const std::string& local_root) {
  const auto invitation = ParseInvitation(token);
  if (local_root.empty() || !std::filesystem::is_directory(local_root)) {
    throw std::invalid_argument("pairing root must be an existing directory");
  }
  const auto role = ParseRole(invitation.invited_role);
  bool created_task = false;
  std::optional<storage::TaskDefinition> task;
  {
    std::scoped_lock database_lock(database_.AccessMutex());
    task = database_.FindTask(invitation.task_id);
    if (!task.has_value()) {
      database_.CreateTask(
          {invitation.task_id, invitation.topology, invitation.invited_role, local_root});
      created_task = true;
      task = database_.FindTask(invitation.task_id);
    } else if (task->mode != invitation.topology || task->role != invitation.invited_role ||
               std::filesystem::weakly_canonical(task->root_path) !=
                   std::filesystem::weakly_canonical(local_root)) {
      throw std::invalid_argument("existing task does not match invitation and local root");
    }
  }
  try {
    signaling::TrackerClient client(invitation.tracker_url, identity_, http_factory_());
    const auto enrollment = client.RedeemInvitation(invitation.code, invitation.task_id, role);
    const auto creator = std::ranges::find_if(enrollment.members, [&](const auto& member) {
      return member.device_id == invitation.creator_device_id &&
             member.fingerprint == invitation.creator_fingerprint;
    });
    if (creator == enrollment.members.end()) {
      throw std::runtime_error("Tracker membership does not match invitation creator");
    }
    PersistEnrollment(*task, invitation.tracker_url, enrollment);
    std::scoped_lock database_lock(database_.AccessMutex());
    return *database_.FindTaskConnection(invitation.task_id);
  } catch (...) {
    if (created_task) {
      std::scoped_lock database_lock(database_.AccessMutex());
      database_.DeleteTask(invitation.task_id);
    }
    throw;
  }
}

signaling::TrackerEnrollment PairingService::RenewTaskEnrollment(const std::string& task_id) {
  auto client = OpenTaskSession(task_id);
  return *client->Enrollment();
}

std::unique_ptr<signaling::TrackerClient> PairingService::OpenTaskSession(
    const std::string& task_id) {
  std::optional<storage::TaskDefinition> task;
  std::optional<storage::TaskConnection> connection;
  {
    std::scoped_lock database_lock(database_.AccessMutex());
    task = database_.FindTask(task_id);
    connection = database_.FindTaskConnection(task_id);
  }
  if (!task.has_value() || !connection.has_value()) {
    throw std::invalid_argument("task is not paired");
  }
  auto client = std::make_unique<signaling::TrackerClient>(connection->tracker_url, identity_,
                                                           http_factory_());
  const auto enrollment = client->JoinRoom(connection->room_id, task_id, ParseRole(task->role));
  if (enrollment.authorization_digest != connection->authorization_digest) {
    throw std::runtime_error("Tracker authorization changed unexpectedly");
  }
  PersistEnrollment(*task, connection->tracker_url, enrollment);
  return client;
}

void PairingService::PersistEnrollment(const storage::TaskDefinition& task,
                                       const std::string& tracker_url,
                                       const signaling::TrackerEnrollment& enrollment) {
  const auto current = NowMilliseconds();
  std::scoped_lock database_lock(database_.AccessMutex());
  database_.InTransaction([&] {
    database_.ConfigureTaskConnection(
        {task.task_id, tracker_url, enrollment.room_id, enrollment.authorization_digest, current});
    for (const auto& member : enrollment.members) {
      database_.UpsertTaskMember({task.task_id, member.device_id, member.fingerprint,
                                  RoleName(member.role), current, false});
    }
  });
}

}  // namespace veritassync::security
