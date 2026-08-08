import * as Dialog from "@radix-ui/react-dialog";
import { open as openFolder } from "@tauri-apps/plugin-dialog";
import { Copy, Fingerprint, FolderOpen, Link2, LoaderCircle, Radio, ShieldCheck, X } from "lucide-react";
import { useEffect, useState } from "react";
import { toast } from "sonner";
import { Button } from "@/components/ui/button";
import { engine, type DeviceIdentity, type PairingInvitation, type SyncTask } from "@/lib/ipc";

interface JoinInvitationDialogProps {
  onJoined: (taskId: string) => Promise<void>;
}

export function JoinInvitationDialog({ onJoined }: JoinInvitationDialogProps) {
  const [open, setOpen] = useState(false);
  const [token, setToken] = useState("");
  const [root, setRoot] = useState("");
  const [busy, setBusy] = useState(false);

  const chooseFolder = async () => {
    const selected = await openFolder({ directory: true, multiple: false, title: "选择本机同步目录" });
    if (typeof selected === "string") setRoot(selected);
  };

  const submit = async (event: React.FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    if (!token.trim() || !root) return;
    setBusy(true);
    try {
      const joined = await engine.joinInvitation(token.trim(), root);
      toast.success("设备配对完成", { description: `任务 ${joined.taskId} 已加入同步房间。` });
      setOpen(false);
      setToken("");
      setRoot("");
      await onJoined(joined.taskId);
    } catch (error) {
      toast.error("无法加入邀请", { description: String(error) });
    } finally {
      setBusy(false);
    }
  };

  return <Dialog.Root open={open} onOpenChange={setOpen}>
    <Dialog.Trigger asChild><Button variant="secondary"><Link2 className="size-4" />接入邀请</Button></Dialog.Trigger>
    <Dialog.Portal>
      <Dialog.Overlay className="fixed inset-0 z-40 bg-[#050914]/70 backdrop-blur-[2px]" />
      <Dialog.Content className="pairing-dialog" aria-describedby="join-invitation-description">
        <PairingHeader eyebrow="PAIR A DEVICE" title="接入已有同步任务" description="邀请会绑定 Tracker、任务拓扑和邀请方设备指纹。" descriptionId="join-invitation-description" />
        <form className="pairing-form" onSubmit={submit}>
          <label className="field-label">邀请令牌<textarea required rows={5} value={token} onChange={(event) => setToken(event.target.value)} placeholder="VSINVITE1|…" className="pairing-textarea font-mono" spellCheck={false} /></label>
          <label className="field-label">本机同步目录<div className="pairing-folder"><input required value={root} onChange={(event) => setRoot(event.target.value)} placeholder="选择接收或双向同步目录" /><button type="button" onClick={chooseFolder}><FolderOpen className="size-4" />选择</button></div></label>
          <div className="pairing-note"><ShieldCheck className="size-5" /><p>Engine 会验证邀请签发者的设备指纹、房间角色和授权摘要；验证失败不会创建任务。</p></div>
          <div className="pairing-actions"><Dialog.Close asChild><Button type="button" variant="secondary">取消</Button></Dialog.Close><Button type="submit" disabled={busy || !token.trim() || !root}>{busy ? <LoaderCircle className="size-4 animate-spin" /> : <Link2 className="size-4" />}验证并加入</Button></div>
        </form>
      </Dialog.Content>
    </Dialog.Portal>
  </Dialog.Root>;
}

interface CreateInvitationDialogProps {
  task: SyncTask;
  trackerUrl: string;
  onTrackerUrlChange: (trackerUrl: string) => Promise<void>;
  onCreated: () => Promise<void>;
}

export function CreateInvitationDialog({ task, trackerUrl, onTrackerUrlChange, onCreated }: CreateInvitationDialogProps) {
  const [open, setOpen] = useState(false);
  const [url, setUrl] = useState(trackerUrl);
  const [invitation, setInvitation] = useState<PairingInvitation | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => { if (!open) setUrl(trackerUrl); }, [open, trackerUrl]);

  const create = async (event: React.FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    setBusy(true);
    try {
      const normalized = url.trim().replace(/\/+$/, "");
      await onTrackerUrlChange(normalized);
      const created = await engine.createInvitation(task.id, normalized);
      setInvitation(created);
      toast.success("邀请已创建", { description: "令牌十分钟内有效且只能使用一次。" });
      await onCreated();
    } catch (error) {
      toast.error("无法创建邀请", { description: String(error) });
    } finally {
      setBusy(false);
    }
  };

  const copy = async () => {
    if (!invitation) return;
    try {
      await navigator.clipboard.writeText(invitation.token);
      toast.success("邀请令牌已复制");
    } catch {
      toast.error("复制失败", { description: "请在文本框中手动选择邀请令牌。" });
    }
  };

  return <Dialog.Root open={open} onOpenChange={(next) => { setOpen(next); if (!next) setInvitation(null); }}>
    <Dialog.Trigger asChild><Button variant="ghost" size="sm"><Radio className="size-3.5" />邀请设备</Button></Dialog.Trigger>
    <Dialog.Portal>
      <Dialog.Overlay className="fixed inset-0 z-40 bg-[#050914]/70 backdrop-blur-[2px]" />
      <Dialog.Content className="pairing-dialog" aria-describedby="create-invitation-description">
        <PairingHeader eyebrow={`INVITE / ${task.id}`} title="邀请另一台设备" description={task.mode === "one_way" ? "对方将作为只读 Target 加入；当前 Source 保持唯一写入权。" : "对方将作为第二个 Peer 加入双向任务。"} descriptionId="create-invitation-description" />
        <form className="pairing-form" onSubmit={create}>
          <label className="field-label">Tracker 地址<input required type="url" value={url} onChange={(event) => setUrl(event.target.value)} placeholder="https://tracker.example.com" className="pairing-input font-mono" /></label>
          {invitation ? <div className="invitation-result"><div><span>一次性配对码</span><strong>{invitation.code}</strong><small>ROOM {invitation.roomId}</small></div><label className="field-label">完整邀请令牌<textarea readOnly rows={5} value={invitation.token} className="pairing-textarea font-mono" onFocus={(event) => event.currentTarget.select()} /></label><Button type="button" variant="secondary" onClick={copy}><Copy className="size-4" />复制邀请令牌</Button></div> : <div className="pairing-note"><Radio className="size-5" /><p>Tracker 仅保存设备成员关系与 WebRTC 信令，不接收文件名、清单、哈希或数据块。</p></div>}
          <div className="pairing-actions"><Dialog.Close asChild><Button type="button" variant="secondary">关闭</Button></Dialog.Close>{!invitation && <Button type="submit" disabled={busy || !url.trim()}>{busy ? <LoaderCircle className="size-4 animate-spin" /> : <Radio className="size-4" />}生成邀请</Button>}</div>
        </form>
      </Dialog.Content>
    </Dialog.Portal>
  </Dialog.Root>;
}

function PairingHeader({ eyebrow, title, description, descriptionId }: { eyebrow: string; title: string; description: string; descriptionId: string }) {
  return <header className="pairing-header"><div><p className="eyebrow">{eyebrow}</p><Dialog.Title>{title}</Dialog.Title><Dialog.Description id={descriptionId}>{description}</Dialog.Description></div><Dialog.Close className="icon-close" aria-label="关闭"><X className="size-4" /></Dialog.Close></header>;
}

export function DeviceIdentityCard() {
  const [identity, setIdentity] = useState<DeviceIdentity | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    void engine.deviceIdentity().then(setIdentity).catch((reason) => setError(String(reason)));
  }, []);

  return <div className="device-identity"><div className="grid size-10 place-items-center rounded-xl bg-[#edf3ff] text-brand"><Fingerprint className="size-5" /></div><div className="min-w-0"><p className="font-semibold text-ink">本机设备身份</p>{identity ? <><p className="mt-1 font-mono text-[11px] text-muted">ID {identity.deviceId}</p><p className="mt-1 truncate font-mono text-[10px] text-muted" title={identity.fingerprint}>FP {identity.fingerprint}</p></> : <p className="mt-1 text-sm text-muted">{error ?? "正在从 Engine 读取 Ed25519 身份…"}</p>}</div></div>;
}
