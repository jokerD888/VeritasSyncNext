#include "engine/common/path.h"
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
  void Replay(std::vector<signaling::RelayMessage> messages) { replay_ = std::move(messages); }

 private:
  TrackerRelayHub& hub_;
  std::string local_device_id_;
  std::string remote_device_id_;
  std::vector<signaling::RelayMessage> replay_;
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
  auto messages = std::exchange(replay_, {});
  for (auto& message : hub_.Drain(remote_device_id_)) messages.push_back(std::move(message));
  return messages;
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
    std::chrono::steady_clock::time_point connection_deadline;
    bool restarting = false;
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
  NetworkMetrics metrics;
  NetworkMetrics baseline;
  TransportFactory transport_factory;
  std::uint64_t retired_sent = 0, retired_received = 0;
  std::chrono::steady_clock::time_point last_sample = std::chrono::steady_clock::now();

  [[nodiscard]] std::unique_ptr<Peer> MakePeer(storage::Database& database,
      const signaling::TrackerMember& member, std::chrono::milliseconds timeout) {
    auto peer = std::make_unique<Peer>();
    peer->connection_deadline = std::chrono::steady_clock::now() + timeout;
    peer->member = member;
    peer->initiator = task.role == "source" ||
        (task.role == "peer" && local_device_id < member.device_id);
    peer->transport = std::make_unique<transport::QueuedPeerTransport>(transport_factory(peer->initiator));
    peer->relay = std::make_unique<PeerRelay>(*relay_hub, local_device_id, member.device_id);
    peer->signaling = std::make_unique<signaling::SignalingSession>(
        *peer->relay, local_device_id, member.device_id, *peer->transport);
    if (task.mode == "one_way" && task.role == "target") {
      if (member.role != protocol::Role::kSource) throw std::runtime_error("target room has no authorized source");
      peer->one_way = std::make_unique<sync::OneWaySyncNode>(sync::OneWaySyncConfig{
          task.task_id, protocol::Role::kTarget, local_device_id, member.device_id,
          local_fingerprint, connection.authorization_digest, common::Utf8Path(task.root_path), database}, *peer->transport);
    } else if (task.mode == "bidirectional") {
      if (member.role != protocol::Role::kPeer) throw std::runtime_error("bidirectional room has an invalid member");
      peer->bidirectional = std::make_unique<sync::BidirectionalSyncNode>(sync::BidirectionalSyncConfig{
          task.task_id, local_device_id, member.device_id, local_fingerprint, member.fingerprint,
          connection.authorization_digest, common::Utf8Path(task.root_path), database}, *peer->transport);
    } else if (member.role != protocol::Role::kTarget) {
      throw std::runtime_error("source room has an invalid target");
    }
    return peer;
  }

  void Retire(Peer& peer) {
    retired_sent += peer.transport->BytesSent();
    retired_received += peer.transport->BytesReceived();
    if (multi_target && peer.data_started) multi_target->RemoveTarget(peer.member.device_id);
  }

  void ResetPeer(std::unique_ptr<Peer>& peer, storage::Database& database,
      std::chrono::milliseconds timeout, std::vector<signaling::RelayMessage> messages = {}) {
    const auto member = peer->member;
    Retire(*peer);
    peer.reset(); // Quiesce native callbacks before installing the replacement.
    peer = MakePeer(database, member, timeout);
    if (peer->initiator) peer->signaling->StartOffer();
    else peer->relay->Replay(std::move(messages));
  }

  [[nodiscard]] bool Pump(storage::Database& database,
                          std::chrono::milliseconds tracker_poll_interval,
                          std::chrono::milliseconds membership_refresh,
                          std::chrono::milliseconds connection_timeout) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_membership_refresh) {
      const auto enrollment =
          tracker->JoinRoom(connection.room_id, task.task_id, ParseRole(task.role));
      next_membership_refresh = now + membership_refresh;
      if (MemberKeys(enrollment, local_device_id) != member_keys) {
        std::scoped_lock database_lock(database.AccessMutex());
        // Joining/removing a target must not destroy healthy sessions to other targets.
        for (auto peer = peers.begin(); peer != peers.end();) {
          const auto found = std::ranges::find_if(enrollment.members, [&](const auto& member) {
            return member.device_id == (*peer)->member.device_id &&
                   member.fingerprint == (*peer)->member.fingerprint && member.role == (*peer)->member.role;
          });
          if (found == enrollment.members.end()) { Retire(**peer); peer = peers.erase(peer); }
          else ++peer;
        }
        for (const auto& member : enrollment.members) {
          if (member.device_id == local_device_id ||
              (task.mode == "one_way" && task.role == "target" && member.role == protocol::Role::kTarget)) continue;
          if (std::ranges::none_of(peers, [&](const auto& peer) { return peer->member.device_id == member.device_id; })) {
            auto peer = MakePeer(database, member, connection_timeout);
            if (peer->initiator) peer->signaling->StartOffer();
            peers.push_back(std::move(peer));
          }
        }
        member_keys = MemberKeys(enrollment, local_device_id);
      }
    }
    if (now >= next_poll) {
      relay_hub->Poll();
      next_poll = now + tracker_poll_interval;
    }
    for (auto& peer : peers) {
      peer->signaling->Pump();
      auto messages = peer->signaling->TakeResetMessages();
      if (!messages.empty()) {
        std::scoped_lock database_lock(database.AccessMutex());
        ResetPeer(peer, database, connection_timeout, std::move(messages));
      }
    }

    bool any_ready = false;
    {
      std::scoped_lock database_lock(database.AccessMutex());
      for (auto& peer : peers) {
        const bool ready = peer->transport->IsReady();
        if (!ready) {
          if (multi_target && peer->data_started) multi_target->SetTargetAvailable(peer->member.device_id, false);
          if (peer->was_ready && !peer->restarting) {
            peer->restarting = true;
            peer->connection_deadline = now + connection_timeout;
            if (peer->initiator) peer->signaling->RestartIce();
            if (!multi_target) database.UpdateTaskNetworkState(task.task_id, "connecting");
          }
          if (now >= peer->connection_deadline) {
            ResetPeer(peer, database, connection_timeout);
          }
          continue;
        }
        peer->restarting = false;
        if (multi_target && peer->data_started) multi_target->SetTargetAvailable(peer->member.device_id, true);
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
        const auto error = peer->bidirectional ? peer->bidirectional->LastError() :
            peer->one_way ? peer->one_way->LastError() : multi_target ? multi_target->LastError(peer->member.device_id) : std::nullopt;
        if (error) throw std::runtime_error(*error);
      }
      const bool all_ready = std::ranges::all_of(peers, [](const auto& peer) { return peer->transport->IsReady(); });
      if (multi_target && multi_target->TargetCount() > 0) multi_target->Pump();
      if (local_changed && (multi_target || all_ready)) {
        if (multi_target && multi_target->TargetCount() > 0) multi_target->RefreshSource();
        for (auto& peer : peers) {
          if (peer->bidirectional && peer->data_started) peer->bidirectional->RefreshLocal();
        }
        local_changed = false;
      }
    }
    online_recorded = any_ready;
    NetworkMetrics sample;
    sample.bytes_sent = retired_sent;
    sample.bytes_received = retired_received;
    for (const auto& peer : peers) {
      sample.bytes_sent += peer->transport->BytesSent();
      sample.bytes_received += peer->transport->BytesReceived();
      sample.buffered_bytes += peer->transport->BufferedAmount(protocol::Channel::kControl) +
                               peer->transport->BufferedAmount(protocol::Channel::kBulk);
      sample.connected_peers += peer->transport->IsReady() ? 1U : 0U;
      if (peer->one_way) sample.pending_downloads += peer->one_way->PendingDownloadCount();
      if (peer->bidirectional) sample.pending_downloads += peer->bidirectional->PendingDownloadCount();
      const auto progress = peer->bidirectional ? peer->bidirectional->DownloadProgress() :
          peer->one_way ? peer->one_way->DownloadProgress() : std::pair<std::uint64_t, std::uint64_t>{};
      sample.download_total_bytes += progress.first;
      sample.download_received_bytes += progress.second;
    }
    const auto elapsed = std::chrono::duration<double>(now - last_sample).count();
    if (elapsed >= 1.0) {
      sample.send_bytes_per_second = static_cast<std::uint64_t>((sample.bytes_sent - baseline.bytes_sent) / elapsed);
      sample.receive_bytes_per_second = static_cast<std::uint64_t>((sample.bytes_received - baseline.bytes_received) / elapsed);
      last_sample = now;
      baseline = sample;
    } else {
      sample.send_bytes_per_second = metrics.send_bytes_per_second;
      sample.receive_bytes_per_second = metrics.receive_bytes_per_second;
    }
    metrics = sample;
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
      options_.retry_delay.count() <= 0 || options_.connection_timeout.count() <= 0) {
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
NetworkMetrics NetworkSessionManager::Metrics(const std::string& task_id) const {
  std::scoped_lock lock(metrics_mutex_);
  const auto found = metrics_.find(task_id);
  return found == metrics_.end() ? NetworkMetrics{} : found->second;
}
void NetworkSessionManager::ProposeIgnorePolicy(const std::string& task_id, std::string expected_hash,
                                                std::string rules, std::string source) {
  std::scoped_lock lock(mutex_);
  if (!started_ || stopping_) throw std::runtime_error("network session is unavailable");
  policy_requests_.push_back({task_id, std::move(expected_hash), std::move(rules), std::move(source)});
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
  session->transport_factory = transport_factory_;
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
                                      session->local_fingerprint, common::Utf8Path(task.root_path), database_});
  }
  for (const auto& member : enrollment.members) {
    if (member.device_id == session->local_device_id) continue;
    // Targets share a room for discovery, but their data sessions go only to the source.
    if (task.mode == "one_way" && task.role == "target" && member.role == protocol::Role::kTarget) continue;
    auto peer = session->MakePeer(database_, member, options_.connection_timeout);
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
        std::scoped_lock metrics_lock(metrics_mutex_);
        metrics_.erase(task_id);
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
    std::vector<PolicyRequest> proposals;
    {
      std::scoped_lock lock(mutex_);
      proposals.swap(policy_requests_);
      for (const auto& task_id : local_changes_) {
        if (const auto task = tasks_.find(task_id); task != tasks_.end())
          task->second->local_changed = true;
      }
      local_changes_.clear();
    }
    for (auto& proposal : proposals) {
      try {
        const auto task = tasks_.find(proposal.task_id);
        if (task == tasks_.end()) throw std::runtime_error("peer is not connected");
        auto& peers = task->second->peers;
        if (peers.size() != 1 || !peers[0]->bidirectional || !peers[0]->transport->IsReady())
          throw std::runtime_error("ignore policy negotiation requires one connected peer");
        std::scoped_lock lock(database_.AccessMutex());
        peers[0]->bidirectional->ProposeIgnorePolicy(proposal.expected_hash, proposal.rules, proposal.source);
      } catch (const std::exception& error) {
        std::scoped_lock lock(database_.AccessMutex());
        database_.RecordEngineEvent({0, proposal.task_id, "warning", "Ignore policy proposal failed: " + std::string(error.what()), NowMilliseconds()});
      }
    }
    for (auto& [task_id, task] : tasks_) {
      try {
        const bool was_online = task->online_recorded;
        if (task->Pump(database_, options_.tracker_poll_interval, options_.membership_refresh, options_.connection_timeout)) {
          rebuild_after_pump.push_back(task_id);
        } else if (!was_online && task->online_recorded) {
          RecordNetworkState(task_id, "online");
        }
        { std::scoped_lock metrics_lock(metrics_mutex_); metrics_[task_id] = task->metrics; }
      } catch (const std::exception& error) {
        RecordNetworkState(task_id, "error", error.what());
        rebuild_after_pump.push_back(task_id);
      }
    }
    {
      std::unique_lock lock(mutex_);
      for (const auto& task_id : rebuild_after_pump) {
        tasks_.erase(task_id);
        { std::scoped_lock metrics_lock(metrics_mutex_); metrics_.erase(task_id); }
        retry_after_[task_id] = std::chrono::steady_clock::now() + options_.retry_delay;
        rebuild_.insert(task_id);
      }
      wake_.wait_for(lock, options_.pump_interval, [this] {
        return stopping_ || !remove_.empty() || !local_changes_.empty();
      });
    }
  }
  std::scoped_lock lock(mutex_);
  tasks_.clear();
  wake_.notify_all();
}

}  // namespace veritassync::runtime
