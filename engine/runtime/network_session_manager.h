#pragma once

#include "engine/security/pairing_service.h"
#include "engine/storage/database.h"
#include "engine/transport/peer_transport.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace veritassync::runtime {

struct NetworkSessionOptions {
  std::chrono::milliseconds pump_interval{50};
  std::chrono::milliseconds tracker_poll_interval{350};
  std::chrono::milliseconds membership_refresh{5000};
  std::chrono::milliseconds retry_delay{5000};
};

class NetworkSessionManager {
 public:
  using TransportFactory = std::function<std::unique_ptr<transport::PeerTransport>(bool initiator)>;

  NetworkSessionManager(storage::Database& database, security::PairingService& pairing,
                        TransportFactory transport_factory, NetworkSessionOptions options = {});
  ~NetworkSessionManager();
  NetworkSessionManager(const NetworkSessionManager&) = delete;
  NetworkSessionManager& operator=(const NetworkSessionManager&) = delete;

  void Start();
  void Stop();
  void TaskChanged(const std::string& task_id);
  void TaskDeleting(const std::string& task_id);
  void PauseTask(const std::string& task_id);
  void ResumeTask(const std::string& task_id);
  void LocalChanged(const std::string& task_id);

 private:
  struct TaskSession;
  [[nodiscard]] std::unique_ptr<TaskSession> BuildTask(const std::string& task_id);
  void WorkerLoop();
  void RecordNetworkState(const std::string& task_id, std::string state,
                          std::optional<std::string> error = std::nullopt);

  storage::Database& database_;
  security::PairingService& pairing_;
  TransportFactory transport_factory_;
  NetworkSessionOptions options_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool started_ = false;
  bool stopping_ = false;
  std::thread worker_;
  std::unordered_map<std::string, std::unique_ptr<TaskSession>> tasks_;
  std::unordered_set<std::string> rebuild_;
  std::unordered_set<std::string> remove_;
  std::unordered_set<std::string> local_changes_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> retry_after_;
};

}  // namespace veritassync::runtime
