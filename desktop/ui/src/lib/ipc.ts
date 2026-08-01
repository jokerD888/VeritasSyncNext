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

export function parseScanResult(reply: string): ScanResult {
  const fields = responseFields(reply);
  if (fields[0] !== "OK" || fields.length < 3) throw new Error("引擎返回了无效的扫描结果");
  return { changed: Number(fields[1]), deleted: Number(fields[2]) };
}

const request = (command: string, args: string[] = []) => invoke<string>("ipc_request", { command, args });

export const engine = {
  ensure: () => invoke<void>("ensure_engine"),
  status: async () => parseStatus(await request("status")),
  tasks: async () => parseTasks(await request("list_tasks")),
  events: async (limit = 100) => parseEvents(await request("list_events", [String(limit)])),
  conflicts: async (taskId: string) => parseConflicts(await request("list_conflicts", [taskId])),
  createTask: (task: Pick<SyncTask, "id" | "mode" | "role" | "root">) => request("create_task", [task.id, task.mode, task.role, task.root]),
  deleteTask: (taskId: string) => request("delete_task", [taskId]),
  scanTask: async (taskId: string) => parseScanResult(await request("scan_task", [taskId, "desktop-local"])),
  resolveConflict: (conflictId: string) => request("resolve_conflict", [conflictId])
};
