#include "engine/storage/database.h"

#include <sqlite3.h>

#include <stdexcept>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

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

[[nodiscard]] const char* FileKindName(const FileKind kind) {
  switch (kind) {
    case FileKind::kFile: return "file";
    case FileKind::kDirectory: return "directory";
    case FileKind::kTombstone: return "tombstone";
  }
  throw std::invalid_argument("unknown file kind");
}

[[nodiscard]] FileKind ParseFileKind(const std::string_view value) {
  if (value == "file") return FileKind::kFile;
  if (value == "directory") return FileKind::kDirectory;
  if (value == "tombstone") return FileKind::kTombstone;
  throw std::runtime_error("database contains an unknown file kind");
}

void ValidateRelativePath(const std::string_view relative_path) {
  if (relative_path.empty() || relative_path.starts_with('/') || relative_path.find('\\') != std::string_view::npos ||
      relative_path.find(':') != std::string_view::npos) {
    throw std::invalid_argument("file record path must be normalized and task-relative");
  }
  std::size_t begin = 0;
  while (begin <= relative_path.size()) {
    const auto end = relative_path.find('/', begin);
    const auto component = relative_path.substr(begin, end == std::string_view::npos ? relative_path.size() - begin : end - begin);
    if (component.empty() || component == "." || component == "..") {
      throw std::invalid_argument("file record path must not traverse directories");
    }
    if (end == std::string_view::npos) return;
    begin = end + 1;
  }
}

void ValidateFileRecord(const FileRecord& record) {
  if (record.task_id.empty() || record.version_id.empty() || record.origin_device_id.empty()) {
    throw std::invalid_argument("file record identity fields are required");
  }
  ValidateRelativePath(record.relative_path);
  if (record.kind == FileKind::kFile && record.content_hash.empty()) {
    throw std::invalid_argument("file records require a content hash");
  }
  if (record.kind == FileKind::kTombstone && !record.deleted_at_ms.has_value()) {
    throw std::invalid_argument("tombstones require a deletion timestamp");
  }
  if (record.kind != FileKind::kTombstone && record.deleted_at_ms.has_value()) {
    throw std::invalid_argument("live records must not have a deletion timestamp");
  }
}

void ValidateTransfer(const TransferRecord& transfer) {
  if (transfer.task_id.empty() || transfer.peer_device_id.empty() ||
      (transfer.direction != "upload" && transfer.direction != "download") ||
      transfer.file_hash.empty() || transfer.state.empty() || transfer.created_at_ms <= 0 || transfer.updated_at_ms <= 0) {
    throw std::invalid_argument("transfer fields are invalid");
  }
}

void BindTransferId(sqlite3_stmt* const statement, const int index, const TransferId& transfer_id,
                    sqlite3* const connection, const char* const operation) {
  Check(sqlite3_bind_blob(statement, index, transfer_id.data(), static_cast<int>(transfer_id.size()), SQLITE_TRANSIENT),
        connection, operation);
}
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
std::optional<TaskDefinition> Database::FindTask(const std::string& task_id) const {
  if (task_id.empty()) throw std::invalid_argument("task id is required");
  constexpr const char* sql = "SELECT mode, role, root_path FROM tasks WHERE task_id=?;";
  sqlite3_stmt* statement = nullptr;
  Check(sqlite3_prepare_v2(connection_, sql, -1, &statement, nullptr), connection_, "prepare task lookup");
  const auto cleanup = [&] { sqlite3_finalize(statement); };
  try {
    Check(sqlite3_bind_text(statement, 1, task_id.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind task lookup");
    const int result = sqlite3_step(statement);
    if (result == SQLITE_DONE) { cleanup(); return std::nullopt; }
    Check(result, connection_, "read task");
    TaskDefinition task{task_id, reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)),
                        reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
                        reinterpret_cast<const char*>(sqlite3_column_text(statement, 2))};
    cleanup();
    return task;
  } catch (...) { cleanup(); throw; }
}
void Database::UpsertFileRecord(const FileRecord& record) {
  ValidateFileRecord(record);
  constexpr const char* sql = R"sql(
INSERT INTO file_records(task_id, relative_path, kind, size, mtime_ns, content_hash, version_id, origin_device_id, logical_clock, deleted_at_ms)
VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(task_id, relative_path) DO UPDATE SET
  kind=excluded.kind, size=excluded.size, mtime_ns=excluded.mtime_ns, content_hash=excluded.content_hash,
  version_id=excluded.version_id, origin_device_id=excluded.origin_device_id, logical_clock=excluded.logical_clock,
  deleted_at_ms=excluded.deleted_at_ms;
)sql";
  sqlite3_stmt* statement = nullptr;
  Check(sqlite3_prepare_v2(connection_, sql, -1, &statement, nullptr), connection_, "prepare file record upsert");
  const auto cleanup = [&] { sqlite3_finalize(statement); };
  try {
    Check(sqlite3_bind_text(statement, 1, record.task_id.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind file task");
    Check(sqlite3_bind_text(statement, 2, record.relative_path.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind file path");
    Check(sqlite3_bind_text(statement, 3, FileKindName(record.kind), -1, SQLITE_STATIC), connection_, "bind file kind");
    Check(sqlite3_bind_int64(statement, 4, static_cast<sqlite3_int64>(record.size)), connection_, "bind file size");
    Check(sqlite3_bind_int64(statement, 5, record.mtime_ns), connection_, "bind file mtime");
    if (record.content_hash.empty()) {
      Check(sqlite3_bind_null(statement, 6), connection_, "bind empty hash");
    } else {
      Check(sqlite3_bind_blob(statement, 6, record.content_hash.data(), static_cast<int>(record.content_hash.size()), SQLITE_TRANSIENT), connection_, "bind file hash");
    }
    Check(sqlite3_bind_text(statement, 7, record.version_id.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind file version");
    Check(sqlite3_bind_text(statement, 8, record.origin_device_id.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind file origin");
    Check(sqlite3_bind_int64(statement, 9, static_cast<sqlite3_int64>(record.logical_clock)), connection_, "bind file clock");
    if (record.deleted_at_ms.has_value()) {
      Check(sqlite3_bind_int64(statement, 10, *record.deleted_at_ms), connection_, "bind deleted at");
    } else {
      Check(sqlite3_bind_null(statement, 10), connection_, "bind live record");
    }
    Check(sqlite3_step(statement), connection_, "upsert file record");
    cleanup();
  } catch (...) { cleanup(); throw; }
}
void Database::RecordTombstone(std::string task_id, std::string relative_path,
                               std::string version_id, std::string origin_device_id,
                               const std::uint64_t logical_clock, const std::int64_t deleted_at_ms) {
  UpsertFileRecord({std::move(task_id), std::move(relative_path), FileKind::kTombstone, 0, 0, {},
                    std::move(version_id), std::move(origin_device_id), logical_clock, deleted_at_ms});
}
std::optional<FileRecord> Database::FindFileRecord(const std::string& task_id, const std::string& relative_path) const {
  ValidateRelativePath(relative_path);
  constexpr const char* sql = "SELECT kind, size, mtime_ns, content_hash, version_id, origin_device_id, logical_clock, deleted_at_ms FROM file_records WHERE task_id=? AND relative_path=?;";
  sqlite3_stmt* statement = nullptr;
  Check(sqlite3_prepare_v2(connection_, sql, -1, &statement, nullptr), connection_, "prepare file record lookup");
  const auto cleanup = [&] { sqlite3_finalize(statement); };
  try {
    Check(sqlite3_bind_text(statement, 1, task_id.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind lookup task");
    Check(sqlite3_bind_text(statement, 2, relative_path.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind lookup path");
    const int result = sqlite3_step(statement);
    if (result == SQLITE_DONE) { cleanup(); return std::nullopt; }
    Check(result, connection_, "read file record");
    FileRecord record;
    record.task_id = task_id;
    record.relative_path = relative_path;
    record.kind = ParseFileKind(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
    record.size = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 1));
    record.mtime_ns = sqlite3_column_int64(statement, 2);
    const auto* hash = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement, 3));
    const int hash_size = sqlite3_column_bytes(statement, 3);
    if (hash != nullptr && hash_size > 0) record.content_hash.assign(hash, hash + hash_size);
    record.version_id = reinterpret_cast<const char*>(sqlite3_column_text(statement, 4));
    record.origin_device_id = reinterpret_cast<const char*>(sqlite3_column_text(statement, 5));
    record.logical_clock = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 6));
    if (sqlite3_column_type(statement, 7) != SQLITE_NULL) record.deleted_at_ms = sqlite3_column_int64(statement, 7);
    cleanup();
    return record;
  } catch (...) { cleanup(); throw; }
}
std::vector<FileRecord> Database::ListFileRecords(const std::string& task_id) const {
  if (task_id.empty()) throw std::invalid_argument("task id is required");
  constexpr const char* sql = "SELECT relative_path, kind, size, mtime_ns, content_hash, version_id, origin_device_id, logical_clock, deleted_at_ms FROM file_records WHERE task_id=? ORDER BY relative_path;";
  sqlite3_stmt* statement = nullptr;
  Check(sqlite3_prepare_v2(connection_, sql, -1, &statement, nullptr), connection_, "prepare file record list");
  const auto cleanup = [&] { sqlite3_finalize(statement); };
  try {
    Check(sqlite3_bind_text(statement, 1, task_id.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind list task");
    std::vector<FileRecord> records;
    while (true) {
      const int result = sqlite3_step(statement);
      if (result == SQLITE_DONE) break;
      Check(result, connection_, "read file record list");
      FileRecord record;
      record.task_id = task_id;
      record.relative_path = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
      record.kind = ParseFileKind(reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)));
      record.size = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 2));
      record.mtime_ns = sqlite3_column_int64(statement, 3);
      const auto* hash = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement, 4));
      const int hash_size = sqlite3_column_bytes(statement, 4);
      if (hash != nullptr && hash_size > 0) record.content_hash.assign(hash, hash + hash_size);
      record.version_id = reinterpret_cast<const char*>(sqlite3_column_text(statement, 5));
      record.origin_device_id = reinterpret_cast<const char*>(sqlite3_column_text(statement, 6));
      record.logical_clock = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 7));
      if (sqlite3_column_type(statement, 8) != SQLITE_NULL) record.deleted_at_ms = sqlite3_column_int64(statement, 8);
      records.push_back(std::move(record));
    }
    cleanup();
    return records;
  } catch (...) { cleanup(); throw; }
}
void Database::InTransaction(const std::function<void()>& operation) {
  Execute("BEGIN IMMEDIATE;");
  try { operation(); Execute("COMMIT;"); } catch (...) { Execute("ROLLBACK;"); throw; }
}
void Database::CreateTransfer(const TransferRecord& transfer) {
  ValidateTransfer(transfer);
  constexpr const char* sql = "INSERT INTO transfers(transfer_id, task_id, peer_device_id, direction, file_hash, state, created_at_ms, updated_at_ms) VALUES(?, ?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* statement = nullptr;
  Check(sqlite3_prepare_v2(connection_, sql, -1, &statement, nullptr), connection_, "prepare transfer insert");
  const auto cleanup = [&] { sqlite3_finalize(statement); };
  try {
    BindTransferId(statement, 1, transfer.transfer_id, connection_, "bind transfer id");
    Check(sqlite3_bind_text(statement, 2, transfer.task_id.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind transfer task");
    Check(sqlite3_bind_text(statement, 3, transfer.peer_device_id.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind transfer peer");
    Check(sqlite3_bind_text(statement, 4, transfer.direction.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind transfer direction");
    Check(sqlite3_bind_blob(statement, 5, transfer.file_hash.data(), static_cast<int>(transfer.file_hash.size()), SQLITE_TRANSIENT), connection_, "bind transfer hash");
    Check(sqlite3_bind_text(statement, 6, transfer.state.c_str(), -1, SQLITE_TRANSIENT), connection_, "bind transfer state");
    Check(sqlite3_bind_int64(statement, 7, transfer.created_at_ms), connection_, "bind transfer creation time");
    Check(sqlite3_bind_int64(statement, 8, transfer.updated_at_ms), connection_, "bind transfer update time");
    Check(sqlite3_step(statement), connection_, "insert transfer");
    cleanup();
  } catch (...) { cleanup(); throw; }
}
void Database::MarkTransferChunkCompleted(const TransferId& transfer_id, const std::uint64_t chunk_index,
                                          const std::int64_t updated_at_ms) {
  if (chunk_index > static_cast<std::uint64_t>((std::numeric_limits<sqlite3_int64>::max)()) || updated_at_ms <= 0) {
    throw std::invalid_argument("transfer chunk fields are invalid");
  }
  InTransaction([&] {
    constexpr const char* chunk_sql = "INSERT INTO transfer_chunks(transfer_id, chunk_index, completed) VALUES(?, ?, 1) ON CONFLICT(transfer_id, chunk_index) DO UPDATE SET completed=1;";
    sqlite3_stmt* chunk = nullptr;
    Check(sqlite3_prepare_v2(connection_, chunk_sql, -1, &chunk, nullptr), connection_, "prepare transfer chunk update");
    const auto cleanup_chunk = [&] { sqlite3_finalize(chunk); };
    try {
      BindTransferId(chunk, 1, transfer_id, connection_, "bind chunk transfer id");
      Check(sqlite3_bind_int64(chunk, 2, static_cast<sqlite3_int64>(chunk_index)), connection_, "bind chunk index");
      Check(sqlite3_step(chunk), connection_, "mark transfer chunk complete");
      cleanup_chunk();
    } catch (...) { cleanup_chunk(); throw; }
    constexpr const char* transfer_sql = "UPDATE transfers SET updated_at_ms=? WHERE transfer_id=?;";
    sqlite3_stmt* transfer = nullptr;
    Check(sqlite3_prepare_v2(connection_, transfer_sql, -1, &transfer, nullptr), connection_, "prepare transfer timestamp update");
    const auto cleanup_transfer = [&] { sqlite3_finalize(transfer); };
    try {
      Check(sqlite3_bind_int64(transfer, 1, updated_at_ms), connection_, "bind transfer timestamp");
      BindTransferId(transfer, 2, transfer_id, connection_, "bind transfer update id");
      Check(sqlite3_step(transfer), connection_, "update transfer timestamp");
      if (sqlite3_changes(connection_) != 1) throw std::invalid_argument("transfer does not exist");
      cleanup_transfer();
    } catch (...) { cleanup_transfer(); throw; }
  });
}
std::vector<std::uint64_t> Database::CompletedTransferChunks(const TransferId& transfer_id) const {
  constexpr const char* sql = "SELECT chunk_index FROM transfer_chunks WHERE transfer_id=? AND completed=1 ORDER BY chunk_index;";
  sqlite3_stmt* statement = nullptr;
  Check(sqlite3_prepare_v2(connection_, sql, -1, &statement, nullptr), connection_, "prepare transfer chunk list");
  const auto cleanup = [&] { sqlite3_finalize(statement); };
  try {
    BindTransferId(statement, 1, transfer_id, connection_, "bind transfer chunk list id");
    std::vector<std::uint64_t> chunks;
    while (true) {
      const int result = sqlite3_step(statement);
      if (result == SQLITE_DONE) break;
      Check(result, connection_, "read transfer chunk list");
      chunks.push_back(static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0)));
    }
    cleanup();
    return chunks;
  } catch (...) { cleanup(); throw; }
}
int Database::SchemaVersion() const { sqlite3_stmt* statement = nullptr; Check(sqlite3_prepare_v2(connection_, "SELECT COALESCE(MAX(version), 0) FROM schema_migrations;", -1, &statement, nullptr), connection_, "prepare schema version"); const int rc = sqlite3_step(statement); Check(rc, connection_, "read schema version"); const int version = sqlite3_column_int(statement, 0); sqlite3_finalize(statement); return version; }
int Database::CountRows(const std::string& table) const { const std::string sql = "SELECT COUNT(*) FROM " + table + ";"; sqlite3_stmt* statement = nullptr; Check(sqlite3_prepare_v2(connection_, sql.c_str(), -1, &statement, nullptr), connection_, "prepare count"); const int rc = sqlite3_step(statement); Check(rc, connection_, "count rows"); const int count = sqlite3_column_int(statement, 0); sqlite3_finalize(statement); return count; }
}  // namespace veritassync::storage
