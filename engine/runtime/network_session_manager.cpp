#include "engine/runtime/network_session_manager.h"

#include "engine/signaling/signaling_session.h"
#include "engine/sync/bidirectional_sync.h"
#include "engine/sync/multi_target_source.h"
#include "engine/sync/one_way_sync.h"
#include "engine/transport/queued_peer_transport.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace veritassync::runtime {
namespace {

protocol::Role ParseRole(const std::string_view role) {
  if (role == "source") return protocol::Role::kSource;
  if (role == "target") return protocol::Role::kTarget;
  if (role == "peer") return protocol::Role::kPeer;
  throw std::invalid_argument("task has an invalid network role");
}

std::int64_t NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

class TrackerRelayHub;

class PeerRelay final : public signaling::SignalingRelay {
 public:
  PeerRelay(TrackerRelayHub& hub, std::string local_device_id, std::string remote_device_id)
      : hub_(hub),
        local_device_id_(std::move(local_device_id)),
        remote_device_id_(std::move(remote_device_id)) {}
  void Forward(const signaling::RelayMessage& message) override;
  std::vector<signaling::RelayMessage> DrainInbox(const std::string& device_id) override;

 private:
  TrackerRelayHub& hub_;
  std::string local_device_id_;
  std::string remote_device_id_;
};

class TrackerRelayHub {
 public:
  TrackerRelayHub(signaling::TrackerClient& tracker, std::string local_device_id)
      : tracker_(tracker), local_device_id_(std::move(local_device_id)) {}
  void Poll() {
    for (auto& message : tracker_.DrainInbox(local_device_id_)) {
      inbox_[message.sender_device_id].push_back(std::move(message));
    }
  }
  void Forward(const signaling::RelayMessage& message) { tracker_.Forward(message); }
  std::vector<signaling::RelayMessage> Drain(const std::string& remote_device_id) {
    auto messages = std::move(inbox_[remote_device_id]);
    inbox_.erase(remote_device_id);
    return messages;
  }

 private:
  signaling::TrackerClient& tracker_;
  std::string local_device_id_;
  std::map<std::string, std::vector<signaling::RelayMessage>> inbox_;
};

void PeerRelay::Forward(const signaling::RelayMessage& message) {
  if (message.sender_device_id != local_device_id_ ||
      message.recipient_device_id != remote_device_id_) {
    throw std::invalid_argument("peer relay identity mismatch");
  }
  hub_.Forward(message);
}

std::vector<signaling::RelayMessage> PeerRelay::DrainInbox(const std::string& device_id) {
  if (device_id != local_device_id_) throw std::invalid_argument("peer relay inbox mismatch");
  return hub_.Drain(remote_device_id_);
}

std::vector<std::string> MemberKeys(const signaling::TrackerEnrollment& enrollment,
                                    const std::string& local_device_id) {
  std::vector<std::string> keys;
  for (const auto& member : enrollment.members) {
    if (member.device_id != local_device_id) {
      keys.push_back(member.device_id + ":" + member.fingerprint + ":" +
                     std::to_string(static_cast<int>(member.role)));
    }
  }
  std::ranges::sort(keys);
  return keys;
}

}  // namespace

struct NetworkSessionManager::TaskSession {
  struct Peer {
    signaling::TrackerMember member;
    bool initiator = false;
    bool data_started = false;
    bool was_ready = false;
    std::unique_ptr<transport::QueuedPeerTransport> transport;
    std::unique_ptr<PeerRelay> relay;
    std::unique_ptr<signaling::SignalingSession> signaling;
    std::unique_ptr<sync::OneWaySyncNode> one_way;
    std::unique_ptr<sync::BidirectionalSyncNode> bidirectional;
  };

  storage::TaskDefinition task;
  storage::TaskConnection connection;
  std::string local_device_id;
  std::string local_fingerprint;
  std::unique_ptr<signaling::TrackerClient> tracker;
  std::unique_ptr<TrackerRelayHub> relay_hub;
  std::vector<std::unique_ptr<Peer>> peers;
  std::unique_ptr<sync::MultiTargetSource> multi_target;
  std::vector<std::string> member_keys;
  std::chrono::steady_clock::time_point next_poll;
  std::chrono::steady_clock::time_point next_membership_refresh;
  bool local_changed = false;
  bool online_recorded = false;

  [[nodiscard]] bool Pump(storage::Database& database,
                          std::chrono::milliseconds tracker_poll_interval,
                          std::chrono::milliseconds membership_refresh) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_membership_refresh) {
      const auto enrollment =
          tracker->JoinRoom(connection.room_id, task.task_id, ParseRole(task.role));
      next_membership_refresh = now + membership_refresh;
      if (MemberKeys(enrollment, local_device_id) != member_keys) return true;
    }
    if (now >= next_poll) {
      relay_hub->Poll();
      next_poll = now + tracker_poll_interval;
    }
    for (auto& peer : peers) peer->signaling->Pump();

    bool any_ready = false;
    {
      std::scoped_lock database_lock(database.AccessMutex());
      for (auto& peer : peers) {
        const bool ready = peer->transport->IsReady();
        if (peer->was_ready && !ready) return true;
        peer->was_ready = peer->was_ready || ready;
        if (ready && !peer->data_started) {
          if (task.mode == "one_way" && task.role == "source") {
            multi_target->AddTarget(
                {peer->member.device_id, peer->member.fingerprint, connection.authorization_digest},
                *peer->transport);
            if (multi_target->TargetCount() == 1) multi_target->Start();
          } else if (peer->one_way) {
            peer->one_way->Start();
          } else if (peer->bidirectional) {
            peer->bidirectional->Start();
          }
          peer->data_started = true;
        }
        if (ready) any_ready = true;
        peer->transport->PumpReceived();
        if (peer->one_way && peer->data_started) peer->one_way->Pump();
        if (peer->bidirectional && peer->data_started) peer->bidirectional->Pump();
      }
      if (multi_target && multi_target->TargetCount() > 0) multi_target->Pump();
      if (local_changed) {
        if (multi_target && multi_target->TargetCount() > 0) multi_target->RefreshSource();
        for (auto& peer : peers) {
          if (peer->bidirectional && peer->data_started) peer->bidirectional->RefreshLocal();
        }
        local_changed = false;
      }
    }
    online_recorded = online_recorded || any_ready;
    return false;
  }
};

NetworkSessionManager::NetworkSessionManager(storage::Database& database,
                                             security::PairingService& pairing,
                                             TransportFactory transport_factory,
                                             NetworkSessionOptions options)
    : database_(database),
      pairing_(pairing),
      transport_factory_(std::move(transport_factory)),
      options_(options) {
  if (!transport_factory_ || options_.pump_interval.count() <= 0 ||
      options_.tracker_poll_interval.count() <= 0 || options_.membership_refresh.count() <= 0 ||
      options_.retry_delay.count() <= 0) {
    throw std::invalid_argument("network session configuration is invalid");
  }
}

NetworkSessionManager::~NetworkSessionManager() { Stop(); }

void NetworkSessionManager::Start() {
  std::vector<std::string> paired;
  {
    std::scoped_lock database_lock(database_.AccessMutex());
    for (const auto& task : database_.ListTasks()) {
      if (database_.RuntimeState(task.task_id).enabled &&
          database_.FindTaskConnection(task.task_id).has_value())
        paired.push_back(task.task_id);
    }
  }
  {
    std::scoped_lock lock(mutex_);
    if (started_) return;
    started_ = true;
    stopping_ = false;
    rebuild_.insert(paired.begin(), paired.end());
  }
  worker_ = std::thread([this] { WorkerLoop(); });
}

void NetworkSessionManager::Stop() {
  {
    std::scoped_lock lock(mutex_);
    if (!started_) return;
    stopping_ = true;
    wake_.notify_all();
  }
  if (worker_.joinable()) worker_.join();
  std::scoped_lock lock(mutex_);
  tasks_.clear();
  rebuild_.clear();
  remove_.clear();
  local_changes_.clear();
  retry_after_.clear();
  started_ = false;
}

void NetworkSessionManager::TaskChanged(const std::string& task_id) {
  std::scoped_lock lock(mutex_);
  rebuild_.insert(task_id);
  retry_after_.erase(task_id);
  wake_.notify_all();
}

void NetworkSessionManager::TaskDeleting(const std::string& task_id) {
  std::unique_lock lock(mutex_);
  if (!started_) {
    tasks_.erase(task_id);
    rebuild_.erase(task_id);
    local_changes_.erase(task_id);
    retry_after_.erase(task_id);
    return;
  }
  remove_.insert(task_id);
  rebuild_.erase(task_id);
  local_changes_.erase(task_id);
  wake_.notify_all();
  wake_.wait(lock, [&] { return !tasks_.contains(task_id) && !remove_.contains(task_id); });
}

void NetworkSessionManager::PauseTask(const std::string& task_id) {
  TaskDeleting(task_id);
  RecordNetworkState(task_id, "offline");
}

void NetworkSessionManager::ResumeTask(const std::string& task_id) { TaskChanged(task_id); }

void NetworkSessionManager::LocalChanged(const std::string& task_id) {
  std::scoped_lock lock(mutex_);
  local_changes_.insert(task_id);
  wake_.notify_all();
}

std::unique_ptr<NetworkSessionManager::TaskSession> NetworkSessionManager::BuildTask(
    const std::string& task_id) {
  storage::TaskDefinition task;
  storage::TaskConnection connection;
  {
    std::scoped_lock database_lock(database_.AccessMutex());
    const auto found_task = database_.FindTask(task_id);
    const auto found_connection = database_.FindTaskConnection(task_id);
    if (!found_task.has_value() || !found_connection.has_value() ||
        !database_.RuntimeState(task_id).enabled)
      return nullptr;
    task = *found_task;
    connection = *found_connection;
    database_.UpdateTaskNetworkState(task_id, "connecting");
  }
  auto tracker = pairing_.OpenTaskSession(task_id);
  const auto enrollment = *tracker->Enrollment();
  auto session = std::make_unique<TaskSession>();
  session->task = task;
  session->connection = connection;
  session->local_device_id = pairing_.Identity().DeviceId();
  session->local_fingerprint = pairing_.Identity().Fingerprint();
  session->member_keys = MemberKeys(enrollment, session->local_device_id);
  session->tracker = std::move(tracker);
  session->relay_hub =
      std::make_unique<TrackerRelayHub>(*session->tracker, session->local_device_id);
  const auto now = std::chrono::steady_clock::now();
  session->next_poll = now;
  session->next_membership_refresh = now + options_.membership_refresh;
  if (task.mode == "one_way" && task.role == "source") {
    session->multi_target = std::make_unique<sync::MultiTargetSource>(
        sync::MultiTargetSourceConfig{task.task_id, session->local_device_id,
                                      session->local_fingerprint, task.root_path, database_});
  }
  for (const auto& member : enrollment.members) {
    if (member.device_id == session->local_device_id) continue;
    auto peer = std::make_unique<TaskSession::Peer>();
    peer->member = member;
    peer->initiator = task.role == "source" ||
                      (task.role == "peer" && session->local_device_id < member.device_id);
    peer->transport =
        std::make_unique<transport::QueuedPeerTransport>(transport_factory_(peer->initiator));
    peer->relay = std::make_unique<PeerRelay>(*session->relay_hub, session->local_device_id,
                                              member.device_id);
    peer->signaling = std::make_unique<signaling::SignalingSession>(
        *peer->relay, session->local_device_id, member.device_id, *peer->transport);
    if (task.mode == "one_way" && task.role == "target") {
      if (member.role != protocol::Role::kSource)
        throw std::runtime_error("target room has no authorized source");
      peer->one_way = std::make_unique<sync::OneWaySyncNode>(
          sync::OneWaySyncConfig{task.task_id, protocol::Role::kTarget, session->local_device_id,
                                 member.device_id, session->local_fingerprint,
                                 connection.authorization_digest, task.root_path, database_},
          *peer->transport);
    } else if (task.mode == "bidirectional") {
      if (member.role != protocol::Role::kPeer)
        throw std::runtime_error("bidirectional room has an invalid member");
      peer->bidirectional = std::make_unique<sync::BidirectionalSyncNode>(
          sync::BidirectionalSyncConfig{task.task_id, session->local_device_id, member.device_id,
                                        session->local_fingerprint, member.fingerprint,
                                        connection.authorization_digest, task.root_path, database_},
          *peer->transport);
    } else if (member.role != protocol::Role::kTarget) {
      throw std::runtime_error("source room has an invalid target");
    }
    if (peer->initiator) peer->signaling->StartOffer();
    session->peers.push_back(std::move(peer));
  }
  return session;
}

void NetworkSessionManager::RecordNetworkState(const std::string& task_id, std::string state,
                                               std::optional<std::string> error) {
  std::scoped_lock database_lock(database_.AccessMutex());
  try {
    database_.UpdateTaskNetworkState(task_id, state, error);
    if (error.has_value())
      database_.RecordEngineEvent(
          {0, task_id, "error", "Network session failed: " + *error, NowMilliseconds()});
  } catch (const std::invalid_argument&) {
    // The task may have been deleted while a failed session was unwinding.
  }
}

void NetworkSessionManager::WorkerLoop() {
  while (true) {
    std::vector<std::string> removals;
    std::vector<std::string> builds;
    {
      std::unique_lock lock(mutex_);
      if (stopping_) break;
      removals.assign(remove_.begin(), remove_.end());
      remove_.clear();
      const auto now = std::chrono::steady_clock::now();
      for (auto iterator = rebuild_.begin(); iterator != rebuild_.end();) {
        const auto retry = retry_after_.find(*iterator);
        if (retry == retry_after_.end() || retry->second <= now) {
          builds.push_back(*iterator);
          iterator = rebuild_.erase(iterator);
        } else {
          ++iterator;
        }
      }
      for (const auto& task_id : removals) {
        tasks_.erase(task_id);
        retry_after_.erase(task_id);
      }
      wake_.notify_all();
    }
    for (const auto& task_id : builds) {
      try {
        auto task = BuildTask(task_id);
        std::scoped_lock lock(mutex_);
        if (!remove_.contains(task_id)) {
          if (task)
            tasks_[task_id] = std::move(task);
          else
            tasks_.erase(task_id);
          retry_after_.erase(task_id);
        }
      } catch (const std::exception& error) {
        RecordNetworkState(task_id, "error", error.what());
        std::scoped_lock lock(mutex_);
        retry_after_[task_id] = std::chrono::steady_clock::now() + options_.retry_delay;
        rebuild_.insert(task_id);
      }
    }

    std::vector<std::string> rebuild_after_pump;
    {
      std::scoped_lock lock(mutex_);
      for (const auto& task_id : local_changes_) {
        if (const auto task = tasks_.find(task_id); task != tasks_.end())
          task->second->local_changed = true;
      }
      local_changes_.clear();
    }
    for (auto& [task_id, task] : tasks_) {
      try {
        const bool was_online = task->online_recorded;
        if (task->Pump(database_, options_.tracker_poll_interval, options_.membership_refresh)) {
          rebuild_after_pump.push_back(task_id);
        } else if (!was_online && task->online_recorded) {
          RecordNetworkState(task_id, "online");
        }
      } catch (const std::exception& error) {
        RecordNetworkState(task_id, "error", error.what());
        rebuild_after_pump.push_back(task_id);
      }
    }
    {
      std::unique_lock lock(mutex_);
      for (const auto& task_id : rebuild_after_pump) {
        tasks_.erase(task_id);
        retry_after_[task_id] = std::chrono::steady_clock::now() + options_.retry_delay;
        rebuild_.insert(task_id);
      }
      wake_.wait_for(lock, options_.pump_interval, [this] {
        return stopping_ || !remove_.empty() || !rebuild_.empty() || !local_changes_.empty();
      });
    }
  }
  std::scoped_lock lock(mutex_);
  tasks_.clear();
  wake_.notify_all();
}

}  // namespace veritassync::runtime
