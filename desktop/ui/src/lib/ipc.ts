import { invoke } from "@tauri-apps/api/core";

export type TaskMode = "one_way" | "bidirectional";
export type TaskRole = "source" | "target" | "peer";

export interface EngineStatus {
  schemaVersion: string;
  taskCount: number;
}

export interface SyncTask {
  id: string;
  mode: TaskMode;
  role: TaskRole;
  root: string;
}

export interface EngineEvent {
  id: string;
  taskId: string | null;
  level: "info" | "warning" | "error";
  message: string;
  timestamp: number;
}

export interface Conflict {
  id: string;
  state: "open" | "resolved" | string;
  originalPath: string;
  conflictPath: string;
  winningVersionId: string;
}

export interface ScanResult {
  changed: number;
  deleted: number;
}

export interface IgnorePolicyState {
  revision: number;
  hash: string;
  canUndo: boolean;
  rules: string;
}

export interface IgnorePreview {
  expectedHash: string;
  scannedFiles: number;
  currentlyIgnored: number;
  proposedIgnored: number;
  newlyIgnored: number;
  newlyIncluded: number;
  trackedNewlyIgnored: number;
  truncated: boolean;
  newlyIgnoredSamples: string[];
  newlyIncludedSamples: string[];
  trackedDeletionSamples: string[];
}

export interface DashboardSnapshot {
  status: EngineStatus;
  tasks: SyncTask[];
  events: EngineEvent[];
  conflicts: Conflict[];
}

const decodeField = (field: string) => field.replace(/%([0-9a-f]{2})/gi, (_, hex: string) => String.fromCharCode(Number.parseInt(hex, 16)));

export function frameLines(reply: string): string[] {
  return reply.split(/\r?\n/).filter(Boolean);
}

export function frameRows(reply: string): string[][] {
  return frameLines(reply)
    .filter((line) => line.startsWith("ROW\t"))
    .map((line) => line.split("\t").slice(1).map(decodeField));
}

function responseFields(reply: string): string[] {
  const first = frameLines(reply)[0] ?? "";
  if (first.startsWith("ERR\t")) throw new Error(first.slice(4));
  return first.split("\t").map(decodeField);
}

export function parseStatus(reply: string): EngineStatus {
  const fields = responseFields(reply);
  if (fields[0] !== "OK" || fields.length < 3) throw new Error("引擎返回了无效的状态帧");
  return { schemaVersion: fields[1], taskCount: Number(fields[2]) };
}

export function parseTasks(reply: string): SyncTask[] {
  return frameRows(reply).map(([id, mode, role, root]) => ({
    id,
    mode: mode as TaskMode,
    role: role as TaskRole,
    root
  }));
}

export function parseEvents(reply: string): EngineEvent[] {
  return frameRows(reply).map(([id, taskId, level, message, timestamp]) => ({
    id,
    taskId: taskId || null,
    level: level as EngineEvent["level"],
    message,
    timestamp: Number(timestamp)
  }));
}

export function parseConflicts(reply: string): Conflict[] {
  return frameRows(reply).map(([id, state, originalPath, conflictPath, winningVersionId]) => ({
    id,
    state,
    originalPath,
    conflictPath,
    winningVersionId
  }));
}

export function parseDashboard(reply: string): DashboardSnapshot {
  const status = parseStatus(reply);
  const tasks: SyncTask[] = [];
  const events: EngineEvent[] = [];
  const conflicts: Conflict[] = [];
  for (const line of frameLines(reply).slice(1)) {
    const [kind, ...rawFields] = line.split("\t");
    if (kind === "END") break;
    const fields = rawFields.map(decodeField);
    if (kind === "TASK") {
      const [id, mode, role, root] = fields;
      tasks.push({ id, mode: mode as TaskMode, role: role as TaskRole, root });
    } else if (kind === "EVENT") {
      const [id, taskId, level, message, timestamp] = fields;
      events.push({ id, taskId: taskId || null, level: level as EngineEvent["level"], message, timestamp: Number(timestamp) });
    } else if (kind === "CONFLICT") {
      const [id, state, originalPath, conflictPath, winningVersionId] = fields;
      conflicts.push({ id, state, originalPath, conflictPath, winningVersionId });
    } else {
      throw new Error("引擎返回了未知的工作台数据行");
    }
  }
  if (tasks.length !== status.taskCount) throw new Error("引擎返回的任务计数不一致");
  return { status, tasks, events, conflicts };
}

export function parseScanResult(reply: string): ScanResult {
  const fields = responseFields(reply);
  if (fields[0] !== "OK" || fields.length < 3) throw new Error("引擎返回了无效的扫描结果");
  return { changed: Number(fields[1]), deleted: Number(fields[2]) };
}

export function parseIgnorePolicy(reply: string): IgnorePolicyState {
  const fields = responseFields(reply);
  if (fields[0] !== "OK" || fields.length !== 5) throw new Error("引擎返回了无效的忽略规则状态");
  return { revision: Number(fields[1]), hash: fields[2], canUndo: fields[3] === "1", rules: fields[4] };
}

export function parseIgnorePreview(reply: string): IgnorePreview {
  const fields = responseFields(reply);
  if (fields[0] !== "OK" || fields.length !== 9) throw new Error("引擎返回了无效的忽略规则预览");
  const preview: IgnorePreview = {
    expectedHash: fields[1],
    scannedFiles: Number(fields[2]),
    currentlyIgnored: Number(fields[3]),
    proposedIgnored: Number(fields[4]),
    newlyIgnored: Number(fields[5]),
    newlyIncluded: Number(fields[6]),
    trackedNewlyIgnored: Number(fields[7]),
    truncated: fields[8] === "1",
    newlyIgnoredSamples: [],
    newlyIncludedSamples: [],
    trackedDeletionSamples: []
  };
  for (const line of frameLines(reply).slice(1)) {
    const [kind, rawPath = ""] = line.split("\t");
    const path = decodeField(rawPath);
    if (kind === "IGNORE") preview.newlyIgnoredSamples.push(path);
    else if (kind === "INCLUDE") preview.newlyIncludedSamples.push(path);
    else if (kind === "DELETE") preview.trackedDeletionSamples.push(path);
    else if (kind !== "END") throw new Error("引擎返回了未知的忽略规则预览行");
  }
  return preview;
}

export function parseAppliedIgnorePolicy(reply: string): Pick<IgnorePolicyState, "revision" | "hash"> {
  const fields = responseFields(reply);
  if (fields[0] !== "OK" || fields.length < 3) throw new Error("引擎返回了无效的忽略规则应用结果");
  return { revision: Number(fields[1]), hash: fields[2] };
}

export function ignorePreviewNeedsConfirmation(preview: IgnorePreview): boolean {
  return preview.trackedNewlyIgnored > 0 || preview.truncated;
}

const request = (command: string, args: string[] = []) => invoke<string>("ipc_request", { command, args });

export const engine = {
  ensure: () => invoke<void>("ensure_engine"),
  dashboard: async (eventLimit = 100) => parseDashboard(await request("dashboard", [String(eventLimit)])),
  status: async () => parseStatus(await request("status")),
  tasks: async () => parseTasks(await request("list_tasks")),
  events: async (limit = 100) => parseEvents(await request("list_events", [String(limit)])),
  conflicts: async (taskId?: string) => parseConflicts(await request("list_conflicts", taskId ? [taskId] : [])),
  createTask: (task: Pick<SyncTask, "id" | "mode" | "role" | "root">) => request("create_task", [task.id, task.mode, task.role, task.root]),
  deleteTask: (taskId: string) => request("delete_task", [taskId]),
  scanTask: async (taskId: string) => parseScanResult(await request("scan_task", [taskId, "desktop-local"])),
  resolveConflict: (conflictId: string) => request("resolve_conflict", [conflictId]),
  ignoreRules: async (taskId: string) => parseIgnorePolicy(await request("ignore_get", [taskId])),
  previewIgnoreRules: async (taskId: string, rules: string) => parseIgnorePreview(await request("ignore_preview", [taskId, rules])),
  applyIgnoreRules: async (taskId: string, expectedHash: string, rules: string, source: "manual" | "ai") =>
    parseAppliedIgnorePolicy(await request("ignore_apply", [taskId, expectedHash, rules, source])),
  undoIgnoreRules: async (taskId: string, expectedHash: string) => {
    await request("ignore_undo", [taskId, expectedHash]);
    return parseIgnorePolicy(await request("ignore_get", [taskId]));
  }
};
