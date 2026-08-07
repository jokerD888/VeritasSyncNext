import * as AlertDialog from "@radix-ui/react-alert-dialog";
import * as Dialog from "@radix-ui/react-dialog";
import * as Select from "@radix-ui/react-select";
import { invoke } from "@tauri-apps/api/core";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { open } from "@tauri-apps/plugin-dialog";
import {
  AlertTriangle,
  ArrowDownUp,
  Bell,
  Bot,
  Check,
  ChevronDown,
  CircleHelp,
  Clock3,
  CloudCog,
  FolderOpen,
  Gauge,
  GitCompareArrows,
  HardDrive,
  LayoutDashboard,
  ListRestart,
  LoaderCircle,
  Maximize2,
  Minimize2,
  MonitorCog,
  Monitor,
  Moon,
  MoreHorizontal,
  Plus,
  RefreshCw,
  Settings2,
  ShieldCheck,
  Sun,
  Trash2,
  TriangleAlert,
  UploadCloud,
  X
} from "lucide-react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { toast, Toaster } from "sonner";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Card } from "@/components/ui/card";
import { AiProviderSettings } from "@/components/AiProviderSettings";
import { IgnoreRulesDialog } from "@/components/IgnoreRulesDialog";
import veritasSyncMark from "@/assets/veritassync-mark.svg";
import { applyColorMode, logError, logInfo, notify, readPreferences, resolveTheme, savePreferences, type DesktopPreferences, type ResolvedTheme } from "@/lib/desktop";
import { engine, type Conflict, type EngineEvent, type EngineStatus, type SyncTask, type TaskMode, type TaskRole } from "@/lib/ipc";

type View = "overview" | "tasks" | "conflicts" | "events" | "settings";

const views: Array<{ id: View; label: string; icon: typeof LayoutDashboard }> = [
  { id: "overview", label: "概览", icon: LayoutDashboard },
  { id: "tasks", label: "同步任务", icon: ArrowDownUp },
  { id: "conflicts", label: "冲突处理", icon: GitCompareArrows },
  { id: "events", label: "运行事件", icon: ListRestart },
  { id: "settings", label: "偏好设置", icon: Settings2 }
];

function formatTime(timestamp: number) {
  return new Intl.DateTimeFormat("zh-CN", { hour: "2-digit", minute: "2-digit", second: "2-digit" }).format(timestamp);
}

function eventTone(level: EngineEvent["level"]) {
  if (level === "error") return "border-[#ffd8db] bg-[#fff5f5] text-[#bd3440]";
  if (level === "warning") return "border-[#ffdeb5] bg-[#fff9ee] text-[#aa6410]";
  return "border-[#dfeaf9] bg-[#f5f9ff] text-[#396093]";
}

function roleLabel(task: SyncTask) {
  if (task.mode === "bidirectional") return "PEER";
  return task.role === "source" ? "SOURCE" : "TARGET";
}

function Brand() {
  return <div className="flex items-center gap-3 px-2"><img src={veritasSyncMark} alt="VeritasSync" className="size-10 rounded-full shadow-[0_9px_20px_rgba(42,129,255,.32)]" /><div className="leading-tight"><div className="text-sm font-extrabold tracking-[.13em] text-white">VERITAS</div><div className="text-sm font-extrabold tracking-[.13em] text-[#85dff0]">SYNC</div></div></div>;
}

function Titlebar() {
  const window = getCurrentWindow();
  const ignoreDrag = (event: React.MouseEvent<HTMLButtonElement>) => event.stopPropagation();
  return <div className="titlebar" data-tauri-drag-region onDoubleClick={() => void window.toggleMaximize().catch(() => undefined)}>
    <div className="flex items-center gap-2" data-tauri-drag-region><img src={veritasSyncMark} alt="" className="size-5 rounded-md" /><span className="font-semibold tracking-[-.015em]">VeritasSync Next</span><span className="titlebar-build">LOCAL WORKSPACE</span></div>
    <div className="titlebar-controls">
      <button aria-label="最小化" onMouseDown={ignoreDrag} onClick={() => void window.minimize().catch(() => undefined)}><Minimize2 className="size-4" /></button>
      <button aria-label="最大化或还原" onMouseDown={ignoreDrag} onClick={() => void window.toggleMaximize().catch(() => undefined)}><Maximize2 className="size-3.5" /></button>
      <button className="titlebar-close" aria-label="关闭窗口" onMouseDown={ignoreDrag} onClick={() => void window.close().catch(() => undefined)}><X className="size-4" /></button>
    </div>
  </div>;
}

interface CreateTaskDialogProps { onCreated: (taskId: string) => Promise<void>; }

function CreateTaskDialog({ onCreated }: CreateTaskDialogProps) {
  const [dialogOpen, setDialogOpen] = useState(false);
  const [saving, setSaving] = useState(false);
  const [id, setId] = useState("");
  const [mode, setMode] = useState<TaskMode>("one_way");
  const [role, setRole] = useState<TaskRole>("source");
  const [root, setRoot] = useState("");

  const chooseFolder = async () => {
    const folder = await open({ directory: true, multiple: false, title: "选择同步目录" });
    if (typeof folder === "string") setRoot(folder);
  };

  const submit = async (event: React.FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (!id || !root) return;
    setSaving(true);
    try {
      await engine.createTask({ id, mode, role: mode === "bidirectional" ? "peer" : role, root });
      toast.success("同步任务已创建", { description: `任务 ${id} 已交给引擎管理。` });
      setDialogOpen(false);
      setId(""); setRoot(""); setMode("one_way"); setRole("source");
      await onCreated(id);
    } catch (error) {
      toast.error("无法创建任务", { description: String(error) });
    } finally { setSaving(false); }
  };

  return <Dialog.Root open={dialogOpen} onOpenChange={setDialogOpen}>
    <Dialog.Trigger asChild><Button><Plus className="size-4" /> 新建任务</Button></Dialog.Trigger>
    <Dialog.Portal>
      <Dialog.Overlay className="fixed inset-0 z-40 bg-[#101b33]/38 backdrop-blur-sm" />
      <Dialog.Content className="fixed left-1/2 top-1/2 z-50 w-[min(580px,calc(100vw-32px))] -translate-x-1/2 -translate-y-1/2 rounded-2xl border border-line bg-white p-0 shadow-[0_28px_90px_rgba(16,27,51,.28)] focus:outline-none">
        <div className="border-b border-line px-7 py-6"><p className="eyebrow">NEW SYNC TASK</p><Dialog.Title className="mt-1 text-2xl font-extrabold tracking-[-.04em] text-ink">创建同步任务</Dialog.Title><Dialog.Description className="mt-2 text-sm text-muted">任务配置由 Engine 持久化；桌面壳不会直接操作同步目录。</Dialog.Description></div>
        <form onSubmit={submit} className="space-y-5 px-7 py-6">
          <label className="grid gap-2 text-sm font-semibold text-[#44546d]">任务名称<input required value={id} onChange={(event) => setId(event.target.value)} pattern="[A-Za-z0-9_-]+" placeholder="例如 home-photos" className="h-11 rounded-lg border border-[#cad6e5] px-3 text-ink outline-none transition focus:border-[#79a8ff] focus:ring-3 focus:ring-[#dce9ff]" /></label>
          <div className="grid grid-cols-2 gap-4">
            <SelectField label="同步拓扑" value={mode} onValueChange={(value) => setMode(value as TaskMode)} options={[{ value: "one_way", label: "单权威源" }, { value: "bidirectional", label: "两节点双向" }]} />
            <SelectField label="本机角色" value={mode === "bidirectional" ? "peer" : role} disabled={mode === "bidirectional"} onValueChange={(value) => setRole(value as TaskRole)} options={mode === "bidirectional" ? [{ value: "peer", label: "Peer" }] : [{ value: "source", label: "Source" }, { value: "target", label: "Target" }]} />
          </div>
          <label className="grid gap-2 text-sm font-semibold text-[#44546d]">同步目录<div className="flex"><input required value={root} onChange={(event) => setRoot(event.target.value)} placeholder="选择一个本机目录" className="h-11 min-w-0 flex-1 rounded-l-lg border border-[#cad6e5] px-3 text-ink outline-none transition focus:border-[#79a8ff] focus:ring-3 focus:ring-[#dce9ff]" /><button type="button" onClick={chooseFolder} className="inline-flex h-11 items-center gap-2 rounded-r-lg border border-l-0 border-[#cad6e5] bg-[#f7faff] px-3 font-semibold text-brand hover:bg-[#edf3ff]"><FolderOpen className="size-4" /> 选择</button></div></label>
          <div className="flex items-start gap-3 rounded-xl border border-[#dce8f8] bg-[#f6f9ff] p-4 text-sm text-[#4f6689]"><ShieldCheck className="mt-0.5 size-5 shrink-0 text-[#2b71dc]" />双向模式严格限制为两个 Peer；Target 不会向 Source 回写本地变更。</div>
          <div className="flex justify-end gap-3 pt-1"><Dialog.Close asChild><Button type="button" variant="secondary">取消</Button></Dialog.Close><Button type="submit" disabled={saving}>{saving ? <LoaderCircle className="size-4 animate-spin" /> : <Plus className="size-4" />} 创建任务</Button></div>
        </form>
      </Dialog.Content>
    </Dialog.Portal>
  </Dialog.Root>;
}

function SelectField({ label, value, onValueChange, options, disabled }: { label: string; value: string; onValueChange: (value: string) => void; options: Array<{ value: string; label: string }>; disabled?: boolean }) {
  return <label className="grid gap-2 text-sm font-semibold text-[#44546d]">{label}<Select.Root value={value} onValueChange={onValueChange} disabled={disabled}><Select.Trigger className="flex h-11 items-center justify-between rounded-lg border border-[#cad6e5] bg-white px-3 text-left font-normal text-ink outline-none transition focus:border-[#79a8ff] focus:ring-3 focus:ring-[#dce9ff] disabled:bg-[#f3f5f8] disabled:text-muted"><Select.Value /><ChevronDown className="size-4 text-muted" /></Select.Trigger><Select.Portal><Select.Content position="popper" className="z-[60] overflow-hidden rounded-lg border border-line bg-white p-1 shadow-xl"><Select.Viewport>{options.map((option) => <Select.Item key={option.value} value={option.value} className="relative flex h-9 cursor-pointer select-none items-center rounded-md py-1.5 pl-8 pr-3 text-sm outline-none data-[highlighted]:bg-[#eef4ff] data-[state=checked]:font-semibold"><Select.ItemIndicator className="absolute left-2"><Check className="size-4 text-brand" /></Select.ItemIndicator><Select.ItemText>{option.label}</Select.ItemText></Select.Item>)}</Select.Viewport></Select.Content></Select.Portal></Select.Root></label>;
}

export function App() {
  const [view, setView] = useState<View>("overview");
  const [status, setStatus] = useState<EngineStatus | null>(null);
  const [tasks, setTasks] = useState<SyncTask[]>([]);
  const [events, setEvents] = useState<EngineEvent[]>([]);
  const [conflicts, setConflicts] = useState<Conflict[]>([]);
  const [refreshing, setRefreshing] = useState(true);
  const [engineError, setEngineError] = useState<string | null>(null);
  const [preferences, setPreferences] = useState<DesktopPreferences>({ notificationsEnabled: true, colorMode: "dark" });
  const [resolvedTheme, setResolvedTheme] = useState<ResolvedTheme>("dark");
  const lastEngineError = useRef<string | null>(null);
  const engineWasOnline = useRef(false);
  const lastEngineNoticeAt = useRef(0);

  useEffect(() => {
    void readPreferences()
      .then(async (loaded) => {
        setPreferences(loaded);
        setResolvedTheme(await applyColorMode(loaded.colorMode));
      })
      .catch((error) => logError(`Unable to load desktop preferences: ${String(error)}`));
    logInfo("Desktop React workspace initialized");
  }, []);

  useEffect(() => {
    if (preferences.colorMode !== "system") return;
    const media = window.matchMedia("(prefers-color-scheme: dark)");
    const update = () => setResolvedTheme(resolveTheme("system"));
    media.addEventListener("change", update);
    return () => media.removeEventListener("change", update);
  }, [preferences.colorMode]);

  const refresh = useCallback(async (quiet = false) => {
    if (!quiet) setRefreshing(true);
    try {
      const { status: nextStatus, tasks: nextTasks, events: nextEvents,
        conflicts: nextConflicts } = await engine.dashboard(100);
      setStatus(nextStatus); setTasks(nextTasks); setEvents(nextEvents); setConflicts(nextConflicts); setEngineError(null); lastEngineError.current = null; engineWasOnline.current = true;
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      setEngineError(message);
      const isNewFailure = lastEngineError.current !== message;
      if (isNewFailure) logError(`Engine refresh failed: ${message}`);
      // Starting a sidecar is allowed to take a few seconds. Never turn an
      // initial connection retry into a Windows notification; only report a
      // loss after this window has already observed the Engine online.
      if (engineWasOnline.current && isNewFailure && Date.now() - lastEngineNoticeAt.current > 5 * 60_000) {
        lastEngineError.current = message;
        lastEngineNoticeAt.current = Date.now();
        void notify(preferences, "VeritasSync 引擎不可用", message);
      }
      lastEngineError.current = message;
      if (!quiet) toast.error("Engine 不可用", { description: message });
    } finally { if (!quiet) setRefreshing(false); }
  }, [preferences]);

  useEffect(() => { void refresh(); const timer = window.setInterval(() => void refresh(true), 5_000); return () => window.clearInterval(timer); }, [refresh]);

  const taskConflicts = useMemo(() => conflicts.filter((conflict) => conflict.state !== "resolved"), [conflicts]);
  const pageTitle = views.find((item) => item.id === view)?.label ?? "VeritasSync";

  const scan = async (task: SyncTask) => {
    try { const result = await engine.scanTask(task.id); const body = `${result.changed} 项变化，${result.deleted} 项删除。`; toast.success("本地扫描已完成", { description: body }); logInfo(`Scan completed for ${task.id}: ${body}`); void notify(preferences, "VeritasSync 扫描完成", `${task.id}：${body}`); await refresh(true); }
    catch (error) { logError(`Scan failed for ${task.id}: ${String(error)}`); toast.error("扫描未完成", { description: String(error) }); }
  };
  const remove = async (task: SyncTask) => { try { await engine.deleteTask(task.id); toast.success("任务已删除", { description: `未删除 ${task.root} 中的文件。` }); logInfo(`Task removed: ${task.id}`); await refresh(true); } catch (error) { logError(`Delete task failed for ${task.id}: ${String(error)}`); toast.error("删除失败", { description: String(error) }); } };
  const resolve = async (conflict: Conflict) => { try { await engine.resolveConflict(conflict.id); toast.success("冲突已标记为已处理"); logInfo(`Conflict resolved: ${conflict.id}`); await refresh(true); } catch (error) { logError(`Resolve conflict failed for ${conflict.id}: ${String(error)}`); toast.error("操作失败", { description: String(error) }); } };
  const checkUpdates = async () => { try { const update = await invoke<{ version: string; notes?: string } | null>("check_for_update"); if (!update) return toast.success("当前已是最新版本"); toast(`发现 ${update.version}`, { description: update.notes ?? "下载、验证并安装后会重启应用。", action: { label: "安装", onClick: () => void invoke("install_update") } }); } catch (error) { toast.error("更新检查失败", { description: String(error) }); } };
  const updatePreferences = async (next: DesktopPreferences) => { setPreferences(next); try { setResolvedTheme(await applyColorMode(next.colorMode)); await savePreferences(next); logInfo(`Desktop preferences saved: notifications=${next.notificationsEnabled}, theme=${next.colorMode}`); } catch (error) { setPreferences(preferences); setResolvedTheme(await applyColorMode(preferences.colorMode)); logError(`Unable to save desktop preferences: ${String(error)}`); toast.error("偏好保存失败", { description: String(error) }); } };

  return <><div className="app-root" data-theme={resolvedTheme}><Titlebar /><div className="window-shell"><aside className="mica-sidebar"><div className="sidebar-content"><Brand /><nav className="mt-13 grid gap-1">{views.map(({ id, label, icon: Icon }) => <button key={id} className="nav-item" data-active={view === id} onClick={() => setView(id)}><Icon className="size-4" /><span className="flex-1">{label}</span>{id === "conflicts" && taskConflicts.length > 0 ? <span className="rounded-full bg-[#80dbf0] px-2 py-0.5 font-mono text-[10px] font-semibold text-[#112039]">{taskConflicts.length}</span> : null}</button>)}</nav><div className="mt-auto border-t border-white/12 px-2 pt-5"><div className="flex items-center gap-2 text-sm text-[#d6e3f6]"><span className={`status-dot ${engineError ? "problem" : status ? "online" : ""}`} />{engineError ? "需要引擎" : status ? "Engine online" : "连接中"}</div><p className="mt-2 font-mono text-[10px] text-[#8496b4]">IPC / v1 · schema {status?.schemaVersion ?? "—"}</p></div></div></aside>
    <main className="min-w-0 bg-canvas"><div className="mx-auto max-w-[1540px] px-[clamp(32px,5vw,80px)] py-10"><header className="flex min-h-20 items-center justify-between border-b border-line pb-7"><div><p className="eyebrow">VERITASSYNC NEXT / LOCAL CONTROL</p><h1 className="mt-1 text-4xl font-extrabold tracking-[-.05em] text-ink">{pageTitle}</h1></div><div className="flex items-center gap-2"><Button variant="ghost" onClick={checkUpdates}><UploadCloud className="size-4" />检查更新</Button><Button size="icon" variant="secondary" aria-label="刷新状态" onClick={() => void refresh()}><RefreshCw className={`size-4 ${refreshing ? "animate-spin" : ""}`} /></Button><CreateTaskDialog onCreated={async (taskId) => { logInfo(`Task created: ${taskId}`); void notify(preferences, "VeritasSync 已创建任务", `任务 ${taskId} 已由本机引擎接管。`); await refresh(true); }} /></div></header>
      {view === "overview" && <Overview status={status} tasks={tasks} events={events} conflicts={taskConflicts} setView={setView} />}
      {view === "tasks" && <TaskList tasks={tasks} onScan={scan} onRemove={remove} />}
      {view === "conflicts" && <ConflictList conflicts={conflicts} onResolve={resolve} />}
      {view === "events" && <EventList events={events} />}
      {view === "settings" && <SettingsPanel preferences={preferences} onPreferencesChange={updatePreferences} />}
    </div></main></div></div><Toaster richColors position="bottom-right" /></>;
}

function Overview({ status, tasks, events, conflicts, setView }: { status: EngineStatus | null; tasks: SyncTask[]; events: EngineEvent[]; conflicts: Conflict[]; setView: (view: View) => void }) {
  return <><section className="mt-7 grid grid-cols-4 gap-3"><Metric label="ENGINE" value={status ? "在线" : "检查中"} detail="独立本机进程" icon={CloudCog} brand /><Metric label="同步任务" value={String(tasks.length)} detail="活动配置" icon={ArrowDownUp} /><Metric label="待处理冲突" value={String(conflicts.length)} detail="需要人工确认" icon={AlertTriangle} /><Metric label="数据库结构" value={status ? `v${status.schemaVersion}` : "—"} detail="SQLite schema" icon={HardDrive} /></section><section className="mt-4 grid grid-cols-[minmax(0,1.15fr)_minmax(330px,.85fr)] gap-4"><Card className="p-6"><PanelHeading eyebrow="SYNCHRONIZATION" title="同步任务" action={<Button variant="ghost" size="sm" onClick={() => setView("tasks")}>查看全部</Button>} />{tasks.length ? <div className="grid gap-2">{tasks.slice(0, 4).map((task) => <TaskRow key={task.id} task={task} compact />)}</div> : <Empty icon={ArrowDownUp} title="还没有同步任务" detail="创建第一个任务，即可开始由 Engine 管理同步状态。" />}</Card><Card className="p-6"><PanelHeading eyebrow="ENGINE HEALTH" title="本机状态" /><div className="space-y-3"><HealthRow icon={ShieldCheck} label="IPC 通道" value={status ? "已连接" : "检查中"} ok={Boolean(status)} /><HealthRow icon={HardDrive} label="状态数据库" value={status ? `Schema v${status.schemaVersion}` : "等待 Engine"} ok={Boolean(status)} /><HealthRow icon={Gauge} label="传输状态" value="本机准备就绪" ok /></div><div className="mt-6 rounded-xl border border-[#dce8fa] bg-[#f6f9ff] p-4 text-sm text-[#557096]"><Bell className="mr-2 inline size-4 text-brand" />同步完成、冲突和引擎异常会显示为可关闭的 Windows 通知。</div></Card></section><section className="mt-4"><Card className="p-6"><PanelHeading eyebrow="RECENT ACTIVITY" title="运行事件" action={<Button variant="ghost" size="sm" onClick={() => setView("events")}>完整日志</Button>} />{events.length ? <div className="grid divide-y divide-[#ecf0f5]">{events.slice(0, 6).map((event) => <EventRow key={event.id} event={event} />)}</div> : <Empty icon={Clock3} title="暂时没有事件" detail="引擎的启动、扫描、传输和恢复记录会显示在这里。" />}</Card></section></>;
}

function Metric({ label, value, detail, icon: Icon, brand }: { label: string; value: string; detail: string; icon: typeof Gauge; brand?: boolean }) { return <article className={`metric-card ${brand ? "is-brand" : ""}`}><div className="flex items-center justify-between"><p className="eyebrow text-[#71829c]">{label}</p><Icon className={`size-4 ${brand ? "text-brand" : "text-[#8091aa]"}`} /></div><strong className="mt-7 block text-[30px] font-extrabold tracking-[-.055em] text-ink">{value}</strong><small className="mt-1 block text-sm text-muted">{detail}</small></article>; }
function PanelHeading({ eyebrow, title, action }: { eyebrow: string; title: string; action?: React.ReactNode }) { return <div className="mb-5 flex items-start justify-between"><div><p className="eyebrow">{eyebrow}</p><h2 className="mt-1 text-xl font-extrabold tracking-[-.04em] text-ink">{title}</h2></div>{action}</div>; }
function Empty({ icon: Icon, title, detail }: { icon: typeof Gauge; title: string; detail: string }) { return <div className="grid min-h-45 place-items-center rounded-xl border border-dashed border-[#ccd7e7] bg-[#fbfcfe] p-7 text-center"><div><div className="mx-auto grid size-11 place-items-center rounded-xl bg-[#edf3ff] text-brand"><Icon className="size-5" /></div><p className="mt-3 font-semibold text-ink">{title}</p><p className="mt-1 text-sm text-muted">{detail}</p></div></div>; }
function HealthRow({ icon: Icon, label, value, ok = false }: { icon: typeof ShieldCheck; label: string; value: string; ok?: boolean }) { return <div className="flex items-center gap-3 rounded-xl border border-[#e7edf5] bg-[#fbfcfe] p-3"><div className="grid size-9 place-items-center rounded-lg bg-white text-[#52709d] shadow-sm"><Icon className="size-4" /></div><div className="min-w-0 flex-1"><p className="text-sm font-semibold text-ink">{label}</p><p className="text-xs text-muted">{value}</p></div><span className={`status-dot ${ok ? "online" : ""}`} /></div>; }

function TaskRow({ task, compact = false, onScan, onRemove }: { task: SyncTask; compact?: boolean; onScan?: (task: SyncTask) => void; onRemove?: (task: SyncTask) => void }) { return <div className="data-row"><div className="min-w-0"><div className="flex items-center gap-2"><span className="font-semibold text-ink">{task.id}</span><Badge>{roleLabel(task)}</Badge>{task.mode === "bidirectional" && <Badge className="border-[#b9e8df] bg-[#effbf8] text-[#16846a]">双向</Badge>}</div><p className="mt-1 truncate text-sm text-muted">{task.root}</p></div>{compact ? <MoreHorizontal className="size-5 text-[#8a99af]" /> : <div className="flex items-center gap-1"><IgnoreRulesDialog task={task} /><Button variant="ghost" size="sm" onClick={() => onScan?.(task)}><RefreshCw className="size-3.5" />扫描</Button><DeleteTaskButton task={task} onRemove={onRemove} /></div>}</div>; }
function DeleteTaskButton({ task, onRemove }: { task: SyncTask; onRemove?: (task: SyncTask) => void }) { return <AlertDialog.Root><AlertDialog.Trigger asChild><Button variant="danger" size="sm"><Trash2 className="size-3.5" />删除</Button></AlertDialog.Trigger><AlertDialog.Portal><AlertDialog.Overlay className="fixed inset-0 z-40 bg-[#101b33]/38 backdrop-blur-sm" /><AlertDialog.Content className="fixed left-1/2 top-1/2 z-50 w-[min(440px,calc(100vw-32px))] -translate-x-1/2 -translate-y-1/2 rounded-2xl border border-line bg-white p-6 shadow-2xl"><AlertDialog.Title className="text-lg font-extrabold text-ink">删除同步任务？</AlertDialog.Title><AlertDialog.Description className="mt-2 text-sm leading-6 text-muted">此操作只会删除引擎中的 <strong>{task.id}</strong> 配置，不会删除 <strong>{task.root}</strong> 内的文件。</AlertDialog.Description><div className="mt-6 flex justify-end gap-3"><AlertDialog.Cancel asChild><Button variant="secondary">取消</Button></AlertDialog.Cancel><AlertDialog.Action asChild><Button variant="danger" onClick={() => onRemove?.(task)}>删除任务</Button></AlertDialog.Action></div></AlertDialog.Content></AlertDialog.Portal></AlertDialog.Root>; }

function TaskList({ tasks, onScan, onRemove }: { tasks: SyncTask[]; onScan: (task: SyncTask) => void; onRemove: (task: SyncTask) => void }) { return <section className="mt-7"><Card className="p-6"><PanelHeading eyebrow="LOCAL ROOTS" title="同步任务" />{tasks.length ? <div className="grid gap-2">{tasks.map((task) => <TaskRow key={task.id} task={task} onScan={onScan} onRemove={onRemove} />)}</div> : <Empty icon={FolderOpen} title="还没有同步任务" detail="从右上角的新建任务开始配置本机同步目录。" />}</Card></section>; }
function EventRow({ event }: { event: EngineEvent }) { return <div className="grid grid-cols-[78px_minmax(0,1fr)_auto] items-start gap-3 py-3"><time className="pt-0.5 font-mono text-[11px] text-[#7b8ba2]">{formatTime(event.timestamp)}</time><div className="min-w-0"><p className="truncate text-sm text-ink">{event.message}</p><p className="mt-1 font-mono text-[10px] text-muted">{event.taskId ?? "ENGINE"} · {event.level.toUpperCase()}</p></div><span className={`rounded-md border px-2 py-0.5 font-mono text-[10px] ${eventTone(event.level)}`}>{event.level}</span></div>; }
function EventList({ events }: { events: EngineEvent[] }) { return <section className="mt-7"><Card className="p-6"><PanelHeading eyebrow="PERSISTED ENGINE LOG" title="运行事件" />{events.length ? <div className="divide-y divide-[#ecf0f5]">{events.map((event) => <EventRow key={event.id} event={event} />)}</div> : <Empty icon={Clock3} title="暂时没有事件" detail="引擎事件会持久化在本地状态库中。" />}</Card></section>; }
function ConflictList({ conflicts, onResolve }: { conflicts: Conflict[]; onResolve: (conflict: Conflict) => void }) { return <section className="mt-7"><Card className="p-6"><PanelHeading eyebrow="MANUAL REVIEW" title="冲突副本" />{conflicts.length ? <div className="grid gap-2">{conflicts.map((conflict) => <div className="data-row" key={conflict.id}><div className="min-w-0"><div className="flex items-center gap-2"><TriangleAlert className="size-4 text-[#d78b1c]" /><span className="font-semibold text-ink">{conflict.originalPath}</span><Badge>{conflict.state}</Badge></div><p className="mt-1 truncate text-sm text-muted">冲突副本：{conflict.conflictPath}</p></div>{conflict.state !== "resolved" && <Button variant="secondary" size="sm" onClick={() => onResolve(conflict)}><Check className="size-3.5" />标记已处理</Button>}</div>)}</div> : <Empty icon={ShieldCheck} title="没有待处理冲突" detail="两节点并发修改造成的冲突会在这里保留可解释的副本。" />}</Card></section>; }
function SettingsPanel({ preferences, onPreferencesChange }: { preferences: DesktopPreferences; onPreferencesChange: (preferences: DesktopPreferences) => Promise<void> }) { return <section className="mt-7 grid grid-cols-[minmax(0,1fr)_minmax(300px,.6fr)] items-start gap-4"><div className="grid gap-4"><Card className="p-6"><PanelHeading eyebrow="DESKTOP PREFERENCES" title="桌面体验" /><div className="grid gap-2"><Setting icon={Moon} title="颜色模式" detail="默认使用深色工作台；也可以切换为浅色或跟随 Windows。" enabled control={<div className="theme-options">{([{ value: "dark", label: "深色", icon: Moon }, { value: "light", label: "浅色", icon: Sun }, { value: "system", label: "系统", icon: Monitor }] as const).map(({ value, label, icon: Icon }) => <button key={value} type="button" data-active={preferences.colorMode === value} onClick={() => void onPreferencesChange({ ...preferences, colorMode: value })}><Icon className="size-3.5" />{label}</button>)}</div>} /><Setting icon={MonitorCog} title="窗口状态" detail="下次启动时恢复上次的位置、尺寸和最大化状态。" enabled /><Setting icon={Bell} title="系统通知" detail="同步完成、冲突和需要注意的错误会显示 Windows 通知。" enabled={preferences.notificationsEnabled} control={<button type="button" role="switch" aria-checked={preferences.notificationsEnabled} onClick={() => void onPreferencesChange({ ...preferences, notificationsEnabled: !preferences.notificationsEnabled })} className={`relative h-6 w-11 rounded-full transition ${preferences.notificationsEnabled ? "bg-brand" : "bg-[#b8c4d3]"}`}><span className={`absolute top-1 size-4 rounded-full bg-white shadow transition ${preferences.notificationsEnabled ? "left-6" : "left-1"}`} /></button>} /><Setting icon={CircleHelp} title="诊断日志" detail="桌面壳和前端错误会写入本地诊断日志，不写入同步数据库。" enabled /></div></Card><AiProviderSettings /></div><Card className="p-6"><PanelHeading eyebrow="SECURITY" title="数据边界" /><div className="space-y-4 text-sm leading-6 text-muted"><p>桌面 UI 通过版本化命名管道请求 Engine，不直接读取 SQLite 数据库或同步目录。</p><p className="rounded-xl border border-[#dce8fa] bg-[#f6f9ff] p-4 text-[#516d94]"><ShieldCheck className="mr-2 inline size-4 text-brand" />系统级目录选择、更新验证和通知均由 Tauri 原生能力完成。</p><p className="rounded-xl border border-[#dce8fa] bg-[#f6f9ff] p-4 text-[#516d94]"><Bot className="mr-2 inline size-4 text-brand" />AI 只能提出候选忽略规则；Engine 会独立校验、预览风险，并在你确认后原子写入。</p></div></Card></section>; }
function Setting({ icon: Icon, title, detail, enabled, control }: { icon: typeof MonitorCog; title: string; detail: string; enabled: boolean; control?: React.ReactNode }) { return <div className="flex items-center gap-4 rounded-xl border border-[#e7edf5] bg-[#fbfcfe] p-4"><div className="grid size-10 place-items-center rounded-xl bg-[#edf3ff] text-brand"><Icon className="size-5" /></div><div className="min-w-0 flex-1"><p className="font-semibold text-ink">{title}</p><p className="mt-0.5 text-sm text-muted">{detail}</p></div>{control ?? <span className={`status-dot ${enabled ? "online" : ""}`} />}</div>; }
