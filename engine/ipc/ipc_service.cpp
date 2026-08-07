#include "engine/ipc/ipc_service.h"

#include "engine/common/uuid.h"
#include "engine/storage/ignore_rules.h"
#include "engine/storage/ignore_policy.h"
#include "engine/storage/manifest_scanner.h"
#include "engine/sync/snapshot_reconciler.h"
#include "engine/sync/task_policy.h"

#include <chrono>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace veritassync::ipc {
namespace {

[[nodiscard]] std::int64_t NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] std::string Escape(const std::string_view value) {
  std::ostringstream output;
  output << std::uppercase << std::hex;
  for (const unsigned char character : value) {
    if (character == '%' || character == '\t' || character == '\r' || character == '\n') {
      output << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(character);
    } else {
      output << static_cast<char>(character);
    }
  }
  return output.str();
}

[[nodiscard]] std::string Unescape(const std::string_view value) {
  std::string output;
  output.reserve(value.size());
  const auto hex = [](const char character) -> int {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
  };
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] != '%') { output.push_back(value[index]); continue; }
    if (index + 2U >= value.size()) throw std::invalid_argument("invalid IPC escape");
    const auto high = hex(value[index + 1U]); const auto low = hex(value[index + 2U]);
    if (high < 0 || low < 0) throw std::invalid_argument("invalid IPC escape");
    output.push_back(static_cast<char>((high << 4U) | low)); index += 2U;
  }
  return output;
}

[[nodiscard]] std::vector<std::string> Fields(const std::string_view request) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  while (begin <= request.size()) {
    const auto end = request.find('\t', begin);
    fields.push_back(Unescape(request.substr(begin, end == std::string_view::npos ? request.size() - begin : end - begin)));
    if (end == std::string_view::npos) break;
    begin = end + 1U;
  }
  return fields;
}

[[nodiscard]] std::string Ok(std::string_view message = {}) {
  return message.empty() ? "OK\n" : "OK\t" + Escape(message) + "\n";
}
[[nodiscard]] std::string Error(const std::string_view message) { return "ERR\t" + Escape(message) + "\n"; }
void RequireCount(const std::vector<std::string>& fields, const std::size_t expected) {
  if (fields.size() != expected) throw std::invalid_argument("invalid IPC command arguments");
}
[[nodiscard]] std::size_t ParseLimit(const std::string& value) {
  std::size_t limit = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), limit);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) throw std::invalid_argument("invalid event limit");
  return limit;
}
void Record(storage::Database& database, std::optional<std::string> task_id, std::string message) {
  database.RecordEngineEvent({0, std::move(task_id), "info", std::move(message), NowMilliseconds()});
}

[[nodiscard]] storage::TaskDefinition RequireTask(storage::Database& database,
                                                  const std::string& task_id) {
  const auto task = database.FindTask(task_id);
  if (!task.has_value()) throw std::invalid_argument("task does not exist");
  return *task;
}

void RequireIgnorePolicyEditor(const storage::TaskDefinition& task) {
  if (task.mode == "one_way" && task.role != "source") {
    throw std::invalid_argument("one-way target ignore policy is read-only");
  }
  if (task.mode == "bidirectional") {
    throw std::invalid_argument("bidirectional ignore policy requires peer negotiation");
  }
}

}  // namespace

IpcService::IpcService(storage::Database& database) : database_(database) {}

std::string IpcService::Handle(const std::string_view request, bool* const should_shutdown) {
  if (should_shutdown != nullptr) *should_shutdown = false;
  try {
    const auto fields = Fields(request);
    if (fields.size() < 2U || fields[0] != kIpcProtocol) throw std::invalid_argument("unsupported IPC protocol");
    const auto& command = fields[1];
    if (command == "ping") { RequireCount(fields, 2); return Ok("pong"); }
    if (command == "status") {
      RequireCount(fields, 2);
      return "OK\t" + std::to_string(database_.SchemaVersion()) + "\t" +
             std::to_string(database_.CountRows("tasks")) + "\n";
    }
    if (command == "list_tasks") {
      RequireCount(fields, 2); std::string response;
      for (const auto& task : database_.ListTasks()) response += "ROW\t" + Escape(task.task_id) + "\t" + Escape(task.mode) + "\t" + Escape(task.role) + "\t" + Escape(task.root_path) + "\n";
      return response + "END\n";
    }
    if (command == "create_task") {
      RequireCount(fields, 6); database_.CreateTask({fields[2], fields[3], fields[4], fields[5]});
      Record(database_, fields[2], "Created task " + fields[2]); return Ok();
    }
    if (command == "delete_task") {
      RequireCount(fields, 3); database_.DeleteTask(fields[2]);
      Record(database_, std::nullopt, "Deleted task " + fields[2]); return Ok();
    }
    if (command == "scan_task") {
      RequireCount(fields, 4); const auto task = database_.FindTask(fields[2]);
      if (!task.has_value()) throw std::invalid_argument("task does not exist");
      if (!sync::CanScanLocalChanges(*task)) throw std::invalid_argument("target task cannot scan local changes");
      storage::IgnoreRules rules; rules.LoadFile(task->root_path);
      const auto snapshot = storage::ManifestScanner(std::move(rules)).Scan(task->root_path);
      const auto result = sync::SnapshotReconciler(common::NewUuidV4).Apply(database_, snapshot,
          {fields[2], fields[3], 0, NowMilliseconds()});
      Record(database_, fields[2], "Scanned " + std::to_string(snapshot.size()) + " entries");
      return "OK\t" + std::to_string(result.created_or_changed) + "\t" + std::to_string(result.tombstoned) + "\n";
    }
    if (command == "ignore_get") {
      RequireCount(fields, 3); const auto task = RequireTask(database_, fields[2]);
      const auto policy = storage::IgnorePolicy::Synchronize(
          database_, task.task_id, task.root_path, NowMilliseconds());
      const bool can_undo = database_.ListIgnorePolicyRevisions(task.task_id, 2).size() > 1U;
      return "OK\t" + std::to_string(policy.revision) + "\t" + Escape(policy.content_hash) +
             "\t" + (can_undo ? "1" : "0") + "\t" + Escape(policy.rules) + "\n";
    }
    if (command == "ignore_context") {
      RequireCount(fields, 5); const auto task = RequireTask(database_, fields[2]);
      const auto mode = fields[4] == "private" ? storage::IgnoreContextMode::kPrivate :
          fields[4] == "precise" ? storage::IgnoreContextMode::kPrecise :
          throw std::invalid_argument("ignore context mode is invalid");
      const auto context = storage::IgnorePolicy::BuildContext(task.root_path, fields[3], mode);
      std::string response = "OK\t" + std::to_string(context.scanned_files) + "\t" +
          (context.truncated ? "1" : "0") + "\t" + Escape(context.directory_summary) + "\n";
      for (const auto& path : context.relevant_paths) response += "MATCH\t" + Escape(path) + "\n";
      for (const auto& path : context.comparison_paths) response += "COMPARE\t" + Escape(path) + "\n";
      return response + "END\n";
    }
    if (command == "ignore_preview") {
      RequireCount(fields, 4); const auto task = RequireTask(database_, fields[2]);
      const auto current = storage::IgnorePolicy::Synchronize(
          database_, task.task_id, task.root_path, NowMilliseconds());
      std::vector<std::string> tracked_paths;
      for (const auto& record : database_.ListFileRecords(task.task_id)) {
        if (record.kind == storage::FileKind::kFile) tracked_paths.push_back(record.relative_path);
      }
      const auto preview = storage::IgnorePolicy::Preview(
          task.root_path, current.rules, fields[3], tracked_paths);
      std::string response = "OK\t" + Escape(current.content_hash) + "\t" +
          std::to_string(preview.scanned_files) + "\t" + std::to_string(preview.currently_ignored) +
          "\t" + std::to_string(preview.proposed_ignored) + "\t" +
          std::to_string(preview.newly_ignored) + "\t" + std::to_string(preview.newly_included) +
          "\t" + std::to_string(preview.tracked_newly_ignored) + "\t" +
          (preview.truncated ? "1" : "0") + "\n";
      for (const auto& path : preview.newly_ignored_samples) response += "IGNORE\t" + Escape(path) + "\n";
      for (const auto& path : preview.newly_included_samples) response += "INCLUDE\t" + Escape(path) + "\n";
      for (const auto& path : preview.tracked_deletion_samples) response += "DELETE\t" + Escape(path) + "\n";
      return response + "END\n";
    }
    if (command == "ignore_apply") {
      RequireCount(fields, 6); const auto task = RequireTask(database_, fields[2]);
      RequireIgnorePolicyEditor(task);
      const auto policy = storage::IgnorePolicy::Apply(
          database_, task.task_id, task.root_path, fields[3], fields[4], fields[5], NowMilliseconds());
      Record(database_, task.task_id, "Applied ignore policy revision " + std::to_string(policy.revision));
      return "OK\t" + std::to_string(policy.revision) + "\t" + Escape(policy.content_hash) + "\n";
    }
    if (command == "ignore_undo") {
      RequireCount(fields, 4); const auto task = RequireTask(database_, fields[2]);
      RequireIgnorePolicyEditor(task);
      const auto policy = storage::IgnorePolicy::Undo(
          database_, task.task_id, task.root_path, fields[3], NowMilliseconds());
      Record(database_, task.task_id, "Restored ignore policy revision " + std::to_string(policy.revision));
      return "OK\t" + std::to_string(policy.revision) + "\t" + Escape(policy.content_hash) +
             "\t" + Escape(policy.rules) + "\n";
    }
    if (command == "list_conflicts") {
      if (fields.size() != 2U && fields.size() != 3U) {
        throw std::invalid_argument("invalid IPC field count");
      }
      std::string response;
      const auto conflicts = fields.size() == 3U ? database_.ListConflicts(fields[2])
                                                 : database_.ListConflicts();
      for (const auto& conflict : conflicts) response += "ROW\t" + Escape(conflict.conflict_id) + "\t" + Escape(conflict.state) + "\t" + Escape(conflict.original_path) + "\t" + Escape(conflict.conflict_path) + "\t" + Escape(conflict.winning_version_id) + "\n";
      return response + "END\n";
    }
    if (command == "resolve_conflict") {
      RequireCount(fields, 3); database_.UpdateConflictState(fields[2], "resolved");
      Record(database_, std::nullopt, "Resolved conflict " + fields[2]); return Ok();
    }
    if (command == "list_events") {
      RequireCount(fields, 3); std::string response;
      const auto limit = ParseLimit(fields[2]);
      const auto events = database_.ListEngineEvents(limit);
      for (const auto& event : events) {
        response.append("ROW\t"); response.append(std::to_string(event.event_id)); response.push_back('\t');
        response.append(Escape(event.task_id.value_or(""))); response.push_back('\t');
        response.append(Escape(event.level)); response.push_back('\t');
        response.append(Escape(event.message)); response.push_back('\t');
        response.append(std::to_string(event.created_at_ms)); response.push_back('\n');
      }
      return response + "END\n";
    }
    if (command == "shutdown") {
      RequireCount(fields, 2); if (should_shutdown != nullptr) *should_shutdown = true; return Ok();
    }
    throw std::invalid_argument("unknown IPC command");
  } catch (const std::exception& error) {
    return Error(error.what());
  }
}

}  // namespace veritassync::ipc
