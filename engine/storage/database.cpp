#include "engine/storage/database.h"

#include <sqlite3.h>

#include <stdexcept>
#include <string>

namespace veritassync::storage {
namespace {
void Check(int code, sqlite3* db, const char* operation) {
  if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW) {
    throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(db));
  }
}
constexpr const char* kMigration1 = R"sql(
CREATE TABLE tasks (
  task_id TEXT PRIMARY KEY, mode TEXT NOT NULL CHECK(mode IN ('one_way', 'bidirectional')),
  role TEXT NOT NULL CHECK(role IN ('source', 'target', 'peer')), root_path TEXT NOT NULL,
  created_at_ms INTEGER NOT NULL DEFAULT (unixepoch() * 1000)
);
CREATE TABLE file_records (
  task_id TEXT NOT NULL REFERENCES tasks(task_id), relative_path TEXT NOT NULL,
  kind TEXT NOT NULL CHECK(kind IN ('file', 'directory', 'tombstone')), size INTEGER NOT NULL,
  mtime_ns INTEGER NOT NULL, content_hash BLOB, version_id TEXT NOT NULL,
  origin_device_id TEXT NOT NULL, logical_clock INTEGER NOT NULL DEFAULT 0,
  deleted_at_ms INTEGER, PRIMARY KEY(task_id, relative_path)
);
CREATE TABLE transfers (
  transfer_id BLOB PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(task_id), peer_device_id TEXT NOT NULL,
  direction TEXT NOT NULL CHECK(direction IN ('upload', 'download')), file_hash BLOB NOT NULL,
  state TEXT NOT NULL, error_code TEXT, created_at_ms INTEGER NOT NULL,
  updated_at_ms INTEGER NOT NULL
);
CREATE TABLE transfer_chunks (
  transfer_id BLOB NOT NULL REFERENCES transfers(transfer_id), chunk_index INTEGER NOT NULL,
  completed INTEGER NOT NULL CHECK(completed IN (0, 1)), PRIMARY KEY(transfer_id, chunk_index)
);
CREATE TABLE peer_state (
  task_id TEXT NOT NULL REFERENCES tasks(task_id), device_id TEXT NOT NULL,
  fingerprint TEXT NOT NULL, last_connected_at_ms INTEGER, protocol_version INTEGER,
  acknowledged_manifest_revision INTEGER, PRIMARY KEY(task_id, device_id)
);
CREATE TABLE conflicts (
  conflict_id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES tasks(task_id), original_path TEXT NOT NULL,
  winning_version_id TEXT NOT NULL, conflict_path TEXT NOT NULL, state TEXT NOT NULL,
  created_at_ms INTEGER NOT NULL
);
CREATE INDEX idx_file_records_task ON file_records(task_id);
CREATE INDEX idx_transfers_task_state ON transfers(task_id, state);
)sql";
}

Database::Database(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  Check(sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &connection_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr), connection_, "open database");
  Execute("PRAGMA journal_mode=WAL;");
  Execute("PRAGMA foreign_keys=ON;");
  Execute("PRAGMA busy_timeout=5000;");
}
Database::~Database() { if (connection_ != nullptr) sqlite3_close(connection_); }
void Database::Execute(const char* sql) const { char* message = nullptr; const int rc = sqlite3_exec(connection_, sql, nullptr, nullptr, &message); if (rc != SQLITE_OK) { std::string detail = message == nullptr ? sqlite3_errmsg(connection_) : message; sqlite3_free(message); throw std::runtime_error(detail); } }
void Database::ApplyMigrations() {
  Execute("CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at_ms INTEGER NOT NULL);");
  if (SchemaVersion() >= 1) return;
  Execute("BEGIN IMMEDIATE;");
  try { Execute(kMigration1); Execute("INSERT INTO schema_migrations(version, applied_at_ms) VALUES(1, unixepoch() * 1000);"); Execute("COMMIT;"); } catch (...) { Execute("ROLLBACK;"); throw; }
}
void Database::CreateTask(const TaskDefinition& task) {
  if (task.task_id.empty() || task.root_path.empty()) throw std::invalid_argument("task id and root path are required");
  sqlite3_stmt* statement = nullptr;
  Check(sqlite3_prepare_v2(connection_, "INSERT INTO tasks(task_id, mode, role, root_path) VALUES(?, ?, ?, ?);", -1, &statement, nullptr), connection_, "prepare task insert");
  const auto cleanup = [&] { sqlite3_finalize(statement); };
  try { Check(sqlite3_bind_text(statement, 1, task.task_id.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind task id"); Check(sqlite3_bind_text(statement, 2, task.mode.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind task mode"); Check(sqlite3_bind_text(statement, 3, task.role.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind task role"); Check(sqlite3_bind_text(statement, 4, task.root_path.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind task root"); Check(sqlite3_step(statement), connection_, "insert task"); cleanup(); } catch (...) { cleanup(); throw; }
}
int Database::SchemaVersion() const { sqlite3_stmt* statement = nullptr; Check(sqlite3_prepare_v2(connection_, "SELECT COALESCE(MAX(version), 0) FROM schema_migrations;", -1, &statement, nullptr), connection_, "prepare schema version"); const int rc = sqlite3_step(statement); Check(rc, connection_, "read schema version"); const int version = sqlite3_column_int(statement, 0); sqlite3_finalize(statement); return version; }
int Database::CountRows(const std::string& table) const { const std::string sql = "SELECT COUNT(*) FROM " + table + ";"; sqlite3_stmt* statement = nullptr; Check(sqlite3_prepare_v2(connection_, sql.c_str(), -1, &statement, nullptr), connection_, "prepare count"); const int rc = sqlite3_step(statement); Check(rc, connection_, "count rows"); const int count = sqlite3_column_int(statement, 0); sqlite3_finalize(statement); return count; }
}  // namespace veritassync::storage
