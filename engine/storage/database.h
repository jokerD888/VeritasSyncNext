#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct sqlite3;

namespace veritassync::storage {

struct TaskDefinition {
  std::string task_id;
  std::string mode;
  std::string role;
  std::string root_path;
};

enum class FileKind { kFile, kDirectory, kTombstone };

struct FileRecord {
  std::string task_id;
  std::string relative_path;
  FileKind kind;
  std::uint64_t size = 0;
  std::int64_t mtime_ns = 0;
  std::vector<std::uint8_t> content_hash;
  std::string version_id;
  std::string origin_device_id;
  std::uint64_t logical_clock = 0;
  std::optional<std::int64_t> deleted_at_ms;
};

using TransferId = std::array<std::uint8_t, 16>;

struct TransferRecord {
  TransferId transfer_id;
  std::string task_id;
  std::string peer_device_id;
  std::string direction;
  std::vector<std::uint8_t> file_hash;
  std::string state;
  std::int64_t created_at_ms = 0;
  std::int64_t updated_at_ms = 0;
  std::string relative_path;
};

struct VersionLineage {
  std::string task_id;
  std::string version_id;
  std::optional<std::string> parent_version_id;
};

struct ConflictRecord {
  std::string conflict_id;
  std::string task_id;
  std::string original_path;
  std::string winning_version_id;
  std::string conflict_path;
  std::string state;
  std::int64_t created_at_ms = 0;
};

struct EngineEvent {
  std::int64_t event_id = 0;
  std::optional<std::string> task_id;
  std::string level;
  std::string message;
  std::int64_t created_at_ms = 0;
};

class Database {
 public:
  explicit Database(const std::filesystem::path& path);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  void ApplyMigrations();
  void CreateTask(const TaskDefinition& task);
  [[nodiscard]] std::optional<TaskDefinition> FindTask(const std::string& task_id) const;
  [[nodiscard]] std::vector<TaskDefinition> ListTasks() const;
  void DeleteTask(const std::string& task_id);
  void UpsertFileRecord(const FileRecord& record);
  void RecordTombstone(std::string task_id, std::string relative_path,
                       std::string version_id, std::string origin_device_id,
                       std::uint64_t logical_clock, std::int64_t deleted_at_ms);
  [[nodiscard]] std::optional<FileRecord> FindFileRecord(
      const std::string& task_id, const std::string& relative_path) const;
  [[nodiscard]] std::vector<FileRecord> ListFileRecords(const std::string& task_id) const;
  // A version's parent is immutable once recorded. The graph permits a peer to
  // determine whether a received record is a successor or a concurrent branch.
  void RecordVersionLineage(const VersionLineage& lineage);
  [[nodiscard]] std::optional<VersionLineage> FindVersionLineage(
      const std::string& task_id, const std::string& version_id) const;
  [[nodiscard]] bool IsVersionAncestor(const std::string& task_id,
                                       const std::string& ancestor_version_id,
                                       const std::string& descendant_version_id) const;
  // Advances the durable Lamport clock to max(local, observed_remote) + 1.
  [[nodiscard]] std::uint64_t AdvanceLogicalClock(const std::string& task_id,
                                                   std::uint64_t observed_remote_clock = 0);
  void RecordConflict(const ConflictRecord& conflict);
  [[nodiscard]] std::vector<ConflictRecord> ListConflicts(const std::string& task_id) const;
  void UpdateConflictState(const std::string& conflict_id, const std::string& state);
  void RecordEngineEvent(const EngineEvent& event);
  [[nodiscard]] std::vector<EngineEvent> ListEngineEvents(std::size_t limit = 200) const;
  void InTransaction(const std::function<void()>& operation);
  void CreateTransfer(const TransferRecord& transfer);
  void MarkTransferChunkCompleted(const TransferId& transfer_id, std::uint64_t chunk_index,
                                  std::int64_t updated_at_ms);
  void MarkTransferChunksCompleted(const TransferId& transfer_id,
                                   std::span<const std::uint64_t> chunk_indices,
                                   std::int64_t updated_at_ms);
  void UpdateTransferState(const TransferId& transfer_id, std::string state,
                           std::int64_t updated_at_ms, std::optional<std::string> error_code = std::nullopt);
  [[nodiscard]] std::optional<TransferRecord> FindActiveDownloadTransfer(
      const std::string& task_id, const std::string& peer_device_id,
      const std::string& relative_path, std::span<const std::uint8_t> file_hash) const;
  [[nodiscard]] std::optional<std::string> TransferState(const TransferId& transfer_id) const;
  [[nodiscard]] std::vector<std::uint64_t> CompletedTransferChunks(const TransferId& transfer_id) const;
  [[nodiscard]] int SchemaVersion() const;
  [[nodiscard]] int CountRows(const std::string& table) const;

 private:
  void Execute(const char* sql) const;
  sqlite3* connection_ = nullptr;
};

}  // namespace veritassync::storage
