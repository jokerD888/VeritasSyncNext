import * as Dialog from "@radix-ui/react-dialog";
import { AlertTriangle, Bot, Check, Eye, LoaderCircle, RotateCcw, ShieldCheck, Sparkles, WandSparkles, X } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import { toast } from "sonner";
import { Button } from "@/components/ui/button";
import { ai, mergeGeneratedRules, type GeneratedIgnoreRules, type IgnoreContextMode } from "@/lib/ai";
import { engine, type IgnorePolicyState, type IgnorePreview, type SyncTask } from "@/lib/ipc";

interface IgnoreRulesDialogProps {
  task: SyncTask;
}

const editableTask = (task: SyncTask) => task.mode === "one_way" && task.role === "source";

function countRules(rules: string) {
  return rules.split(/\r?\n/).filter((line) => line.trim() && !line.trim().startsWith("#")).length;
}

export function IgnoreRulesDialog({ task }: IgnoreRulesDialogProps) {
  const [open, setOpen] = useState(false);
  const [policy, setPolicy] = useState<IgnorePolicyState | null>(null);
  const [rules, setRules] = useState("");
  const [description, setDescription] = useState("");
  const [contextMode, setContextMode] = useState<IgnoreContextMode>("private");
  const [preview, setPreview] = useState<IgnorePreview | null>(null);
  const [generated, setGenerated] = useState<GeneratedIgnoreRules | null>(null);
  const [busy, setBusy] = useState<"load" | "generate" | "preview" | "apply" | "undo" | null>(null);
  const [applySource, setApplySource] = useState<"manual" | "ai">("manual");
  const [confirmRisk, setConfirmRisk] = useState(false);
  const editable = editableTask(task);
  const dirty = policy ? rules !== policy.rules : false;

  const load = async () => {
    setBusy("load");
    try {
      const next = await engine.ignoreRules(task.id);
      setPolicy(next);
      setRules(next.rules);
      setPreview(null);
      setGenerated(null);
      setConfirmRisk(false);
      setApplySource("manual");
    } catch (error) {
      toast.error("无法读取忽略规则", { description: String(error) });
    } finally {
      setBusy(null);
    }
  };

  useEffect(() => {
    if (open) void load();
  }, [open, task.id]);

  const runPreview = async (candidate = rules) => {
    setBusy("preview");
    try {
      const next = await engine.previewIgnoreRules(task.id, candidate);
      setPreview(next);
      setConfirmRisk(false);
      return next;
    } catch (error) {
      toast.error("规则预览失败", { description: String(error) });
      return null;
    } finally {
      setBusy(null);
    }
  };

  const generate = async () => {
    if (!description.trim()) return;
    setBusy("generate");
    try {
      const suggestion = await ai.generateIgnoreRules(task.id, description.trim(), contextMode);
      const merged = mergeGeneratedRules(rules, suggestion.rules);
      setGenerated(suggestion);
      setRules(merged);
      setApplySource("ai");
      setPreview(null);
      setConfirmRisk(false);
      toast.success("AI 建议已加入草稿", { description: "尚未写入同步目录，请先检查影响预览。" });
    } catch (error) {
      toast.error("AI 规则生成失败", { description: String(error) });
    } finally {
      setBusy(null);
    }
  };

  const apply = async () => {
    if (!policy || !dirty) return;
    let checked = preview;
    if (!checked || checked.expectedHash !== policy.hash) checked = await runPreview();
    if (!checked) return;
    if (checked.trackedNewlyIgnored > 0 && !confirmRisk) {
      setConfirmRisk(true);
      return;
    }
    setBusy("apply");
    try {
      await engine.applyIgnoreRules(task.id, policy.hash, rules, applySource);
      toast.success("忽略规则已原子写入", { description: "新修订已保存，可在此撤销。" });
      await load();
    } catch (error) {
      toast.error("无法应用忽略规则", { description: String(error) });
      await load();
    } finally {
      setBusy(null);
    }
  };

  const undo = async () => {
    if (!policy) return;
    setBusy("undo");
    try {
      const restored = await engine.undoIgnoreRules(task.id, policy.hash);
      setPolicy(restored);
      setRules(restored.rules);
      setPreview(null);
      setGenerated(null);
      toast.success("已恢复上一版忽略规则");
    } catch (error) {
      toast.error("无法撤销规则", { description: String(error) });
      await load();
    } finally {
      setBusy(null);
    }
  };

  const statusLabel = useMemo(() => {
    if (!policy) return "读取中";
    if (dirty) return `${countRules(rules)} 条草稿规则`;
    return `修订 ${policy.revision} · ${countRules(rules)} 条规则`;
  }, [dirty, policy, rules]);

  return <Dialog.Root open={open} onOpenChange={setOpen}>
    <Dialog.Trigger asChild><Button variant="ghost" size="sm"><WandSparkles className="size-3.5" />忽略规则</Button></Dialog.Trigger>
    <Dialog.Portal>
      <Dialog.Overlay className="fixed inset-0 z-40 bg-[#050914]/70 backdrop-blur-[2px]" />
      <Dialog.Content className="ignore-dialog" aria-describedby="ignore-dialog-description">
        <header className="ignore-dialog-header">
          <div><p className="eyebrow">IGNORE POLICY / {task.id}</p><Dialog.Title>忽略规则工作台</Dialog.Title><Dialog.Description id="ignore-dialog-description">AI 只生成候选规则；预览和最终写入始终由本机 Engine 执行。</Dialog.Description></div>
          <Dialog.Close className="icon-close" aria-label="关闭"><X className="size-4" /></Dialog.Close>
        </header>

        {!editable && <div className="ignore-readonly"><ShieldCheck className="size-5" /><div><strong>此任务只读</strong><p>{task.role === "target" ? "单向 Target 必须沿用 Source 的忽略策略。" : "双向任务需先实现两节点规则协商，当前不会允许本机单独改动。"}</p></div></div>}

        <div className="ignore-workspace">
          <section className="ignore-editor-pane">
            <div className="pane-heading"><div><span>01 / POLICY</span><h3>.veritasignore</h3></div><small>{statusLabel}</small></div>
            <textarea className="rule-editor" spellCheck={false} value={rules} readOnly={!editable} onChange={(event) => { setRules(event.target.value); setPreview(null); setConfirmRisk(false); setApplySource("manual"); }} placeholder={busy === "load" ? "读取中…" : "# 每行一条规则\n*.log\nbuild/\n!build/keep.txt"} />
            <div className="syntax-strip"><span><code>**</code> 任意层级</span><span><code>/</code> 根目录锚定</span><span><code>!</code> 重新包含</span><span>最大 128 条 / 16 KiB</span></div>
          </section>

          <aside className="ignore-ai-pane">
            <div className="pane-heading"><div><span>02 / ASSIST</span><h3><Sparkles className="size-4" />AI 建议</h3></div><small>不会自动应用</small></div>
            <label className="field-label">用自然语言描述要排除的内容<textarea className="prompt-editor" value={description} disabled={!editable} onChange={(event) => setDescription(event.target.value)} maxLength={4096} placeholder="例如：忽略 C++ 和 Node 项目的编译缓存，但保留示例配置文件。" /></label>
            <div className="privacy-picker" role="group" aria-label="发送给 AI 的上下文范围">
              <button type="button" data-active={contextMode === "private"} onClick={() => setContextMode("private")}><ShieldCheck className="size-4" /><span><strong>隐私模式</strong><small>仅目录统计与扩展名</small></span></button>
              <button type="button" data-active={contextMode === "precise"} onClick={() => setContextMode("precise")}><Eye className="size-4" /><span><strong>精确模式</strong><small>附带少量相对路径样本</small></span></button>
            </div>
            <p className="privacy-note">不会读取或上传文件内容。项目上下文按不可信数据处理，模型输出必须通过严格 JSON 与 Engine 规则校验。</p>
            <Button className="w-full" disabled={!editable || !description.trim() || busy !== null} onClick={() => void generate()}>{busy === "generate" ? <LoaderCircle className="size-4 animate-spin" /> : <Bot className="size-4" />}生成候选规则</Button>
            {generated && <div className="ai-result"><div><span>{generated.provider}</span><span>{generated.model}</span></div><p>{generated.explanation}</p><ul>{generated.rules.map((rule) => <li key={rule}><code>{rule}</code></li>)}</ul></div>}
          </aside>
        </div>

        <section className="preview-panel">
          <div className="pane-heading"><div><span>03 / DRY RUN</span><h3>影响预览</h3></div><Button variant="secondary" size="sm" disabled={!editable || !policy || busy !== null} onClick={() => void runPreview()}>{busy === "preview" ? <LoaderCircle className="size-3.5 animate-spin" /> : <Eye className="size-3.5" />}重新计算</Button></div>
          {preview ? <div className="preview-grid">
            <div><strong>{preview.scannedFiles}</strong><span>已扫描文件</span></div><div><strong>{preview.proposedIgnored}</strong><span>应用后忽略</span></div><div><strong className="text-[#43c6ad]">+{preview.newlyIgnored}</strong><span>新增忽略</span></div><div><strong className={preview.trackedNewlyIgnored ? "text-[#ff7c87]" : ""}>{preview.trackedNewlyIgnored}</strong><span>已跟踪删除风险</span></div>
          </div> : <div className="preview-empty">修改或生成规则后，请先运行影响预览。Engine 只读取元数据，不读取文件内容。</div>}
          {preview?.trackedNewlyIgnored ? <div className="risk-warning"><AlertTriangle className="size-5" /><div><strong>{preview.trackedNewlyIgnored} 个已跟踪文件会在下次扫描成为删除记录</strong><p>{preview.trackedDeletionSamples.join(" · ")}{preview.truncated ? " · 样本已截断" : ""}</p></div></div> : null}
        </section>

        <footer className="ignore-dialog-footer"><div className="flex items-center gap-2"><Button variant="ghost" disabled={!editable || !policy?.canUndo || dirty || busy !== null} onClick={() => void undo()}><RotateCcw className="size-4" />撤销上一版</Button><span>{dirty ? "有未应用的更改" : "规则与磁盘一致"}</span></div><div className="flex gap-2"><Dialog.Close asChild><Button variant="secondary">关闭</Button></Dialog.Close><Button disabled={!editable || !dirty || busy !== null} className={confirmRisk ? "!bg-[#d94453]" : ""} onClick={() => void apply()}>{busy === "apply" ? <LoaderCircle className="size-4 animate-spin" /> : confirmRisk ? <AlertTriangle className="size-4" /> : <Check className="size-4" />}{confirmRisk ? "确认产生删除记录" : "预览并应用"}</Button></div></footer>
      </Dialog.Content>
    </Dialog.Portal>
  </Dialog.Root>;
}
