#pragma once

#include <filesystem>
#include <string>

struct sqlite3;

namespace veritassync::storage {

struct TaskDefinition {
  std::string task_id;
  std::string mode;
  std::string role;
  std::string root_path;
};

class Database {
 public:
  explicit Database(const std::filesystem::path& path);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  void ApplyMigrations();
  void CreateTask(const TaskDefinition& task);
  [[nodiscard]] int SchemaVersion() const;
  [[nodiscard]] int CountRows(const std::string& table) const;

 private:
  void Execute(const char* sql) const;
  sqlite3* connection_ = nullptr;
};

}  // namespace veritassync::storage
