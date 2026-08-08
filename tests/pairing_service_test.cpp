#include "engine/security/pairing_service.h"
#include "tests/test_framework.h"

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
struct SharedResponses {
  std::vector<veritassync::signaling::TrackerHttpResponse> values;
};

class ScriptedHttp final : public veritassync::signaling::TrackerHttpTransport {
 public:
  explicit ScriptedHttp(std::shared_ptr<SharedResponses> responses)
      : responses_(std::move(responses)) {}
  veritassync::signaling::TrackerHttpResponse Post(std::string_view, std::string_view,
                                                   const std::map<std::string, std::string>&,
                                                   std::string_view) override {
    if (responses_->values.empty()) throw std::runtime_error("unexpected pairing request");
    auto response = responses_->values.front();
    responses_->values.erase(responses_->values.begin());
    return response;
  }

 private:
  std::shared_ptr<SharedResponses> responses_;
};

std::string Enrollment(
    const std::string& room, const std::string& authorization, const std::string& session,
    const std::vector<std::tuple<std::string, std::string, std::string>>& members) {
  std::string response =
      "OK\t" + room + "\t" + authorization + "\t" + session + "\t9999999999999\tMEMBERS\n";
  for (const auto& [device, fingerprint, role] : members) {
    response += "MEMBER\t" + device + "\t" + fingerprint + "\t" + role + "\n";
  }
  return response + "END\n";
}
}  // namespace

VSYNC_TEST(PairingServiceCreatesAndConsumesBoundInvitation) {
  const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const auto source_db_path =
      std::filesystem::temp_directory_path() / ("veritassync-pair-source-" + stamp + ".db");
  const auto target_db_path =
      std::filesystem::temp_directory_path() / ("veritassync-pair-target-" + stamp + ".db");
  const auto target_root =
      std::filesystem::temp_directory_path() / ("veritassync-pair-root-" + stamp);
  std::filesystem::create_directories(target_root);
  {
    veritassync::storage::Database source_db(source_db_path);
    veritassync::storage::Database target_db(target_db_path);
    source_db.ApplyMigrations();
    target_db.ApplyMigrations();
    source_db.CreateTask({"photos", "one_way", "source", "C:/photos"});
    auto source_identity = veritassync::security::DeviceIdentity::Generate();
    auto target_identity = veritassync::security::DeviceIdentity::Generate();
    const auto source_device = source_identity.DeviceId();
    const auto source_fingerprint = source_identity.Fingerprint();
    const auto target_device = target_identity.DeviceId();
    const auto target_fingerprint = target_identity.Fingerprint();
    const auto authorization = std::string(64, 'a');
    auto responses = std::make_shared<SharedResponses>();
    responses->values.push_back(
        {200, "INVITE\tABCD-EFGH\n" + Enrollment("room-1", authorization, "source-session",
                                                 {{source_device, source_fingerprint, "source"}})});
    responses->values.push_back({200, Enrollment("room-1", authorization, "renewed-source-session",
                                                 {{source_device, source_fingerprint, "source"}})});
    responses->values.push_back({200, "INVITE\tIJKL-MNOP\n"});
    responses->values.push_back({200, Enrollment("room-1", authorization, "target-session",
                                                 {{source_device, source_fingerprint, "source"},
                                                  {target_device, target_fingerprint, "target"}})});
    auto factory = [responses] { return std::make_unique<ScriptedHttp>(responses); };
    veritassync::security::PairingService source_pairing(source_db, std::move(source_identity),
                                                         factory);
    const auto invitation = source_pairing.CreateInvitation("photos", "https://tracker.example");
    VSYNC_CHECK(invitation.token.starts_with("VSINVITE1|"));
    VSYNC_CHECK(source_db.FindTaskConnection("photos").has_value());
    VSYNC_CHECK(source_db.ListTaskMembers("photos").size() == 1);
    const auto additional = source_pairing.CreateInvitation("photos", "https://tracker.example");
    VSYNC_CHECK(additional.code == "IJKL-MNOP");
    VSYNC_CHECK(additional.room_id == invitation.room_id);

    veritassync::security::PairingService target_pairing(target_db, std::move(target_identity),
                                                         factory);
    const auto connection = target_pairing.JoinInvitation(invitation.token, target_root.string());
    VSYNC_CHECK(connection.room_id == "room-1");
    const auto target_task = target_db.FindTask("photos");
    VSYNC_CHECK(target_task.has_value() && target_task->role == "target");
    VSYNC_CHECK(target_db.ListTaskMembers("photos").size() == 2);
  }
  for (const auto& path : {source_db_path, target_db_path}) {
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-shm");
    std::filesystem::remove(path.string() + "-wal");
  }
  std::filesystem::remove_all(target_root);
}
