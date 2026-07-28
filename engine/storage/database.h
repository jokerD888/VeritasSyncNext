#pragma once

#include <filesystem>
#include <functional>
#include <cstdint>
#include <optional>
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

class Database {
 public:
  explicit Database(const std::filesystem::path& path);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  void ApplyMigrations();
  void CreateTask(const TaskDefinition& task);
  void UpsertFileRecord(const FileRecord& record);
  void RecordTombstone(std::string task_id, std::string relative_path,
                       std::string version_id, std::string origin_device_id,
                       std::uint64_t logical_clock, std::int64_t deleted_at_ms);
  [[nodiscard]] std::optional<FileRecord> FindFileRecord(
      const std::string& task_id, const std::string& relative_path) const;
  [[nodiscard]] std::vector<FileRecord> ListFileRecords(const std::string& task_id) const;
  void InTransaction(const std::function<void()>& operation);
  [[nodiscard]] int SchemaVersion() const;
  [[nodiscard]] int CountRows(const std::string& table) const;

 private:
  void Execute(const char* sql) const;
  sqlite3* connection_ = nullptr;
};

}  // namespace veritassync::storage
