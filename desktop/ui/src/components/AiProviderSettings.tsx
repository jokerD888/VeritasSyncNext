import { Bot, CheckCircle2, KeyRound, LoaderCircle, ShieldCheck, Trash2 } from "lucide-react";
import { useEffect, useState } from "react";
import { toast } from "sonner";
import { Button } from "@/components/ui/button";
import { Card } from "@/components/ui/card";
import { ai, type AiProviderStatus } from "@/lib/ai";

export function AiProviderSettings() {
  const [status, setStatus] = useState<AiProviderStatus | null>(null);
  const [endpoint, setEndpoint] = useState("");
  const [model, setModel] = useState("");
  const [apiKey, setApiKey] = useState("");
  const [jsonMode, setJsonMode] = useState(true);
  const [busy, setBusy] = useState(false);

  const adopt = (next: AiProviderStatus) => {
    setStatus(next);
    setEndpoint(next.endpoint);
    setModel(next.model);
    setJsonMode(next.jsonMode);
    setApiKey("");
  };

  useEffect(() => {
    void ai.status().then(adopt).catch((error) => toast.error("无法读取 AI Provider 配置", { description: String(error) }));
  }, []);

  const save = async (event: React.FormEvent) => {
    event.preventDefault();
    setBusy(true);
    try {
      adopt(await ai.configure({ endpoint, model, jsonMode, apiKey: apiKey || undefined }));
      toast.success("AI Provider 配置已保存", { description: "API Key 已写入 Windows 凭据管理器。" });
    } catch (error) {
      toast.error("无法保存 AI Provider", { description: String(error) });
    } finally {
      setBusy(false);
    }
  };

  const clear = async () => {
    setBusy(true);
    try {
      adopt(await ai.clearKey());
      toast.success("AI API Key 已从 Windows 凭据管理器删除");
    } catch (error) {
      toast.error("无法删除 AI API Key", { description: String(error) });
    } finally {
      setBusy(false);
    }
  };

  return <Card className="p-6">
    <div className="mb-5 flex items-start justify-between"><div><p className="eyebrow">AI PROVIDER</p><h2 className="mt-1 text-xl font-extrabold tracking-[-.04em] text-ink">规则生成服务</h2></div><span className={`ai-provider-state ${status?.keyConfigured ? "ready" : ""}`}>{status?.keyConfigured ? <CheckCircle2 className="size-3.5" /> : <KeyRound className="size-3.5" />}{status?.keyConfigured ? "凭据就绪" : "未配置 Key"}</span></div>
    <form className="ai-setting-form" onSubmit={save}>
      <label>OpenAI 兼容端点<input className="ai-setting-input" required type="url" value={endpoint} onChange={(event) => setEndpoint(event.target.value)} placeholder="https://…/v1/chat/completions" /></label>
      <div className="grid grid-cols-[1fr_auto] gap-3"><label>模型<input className="ai-setting-input" required value={model} onChange={(event) => setModel(event.target.value)} placeholder="qwen-turbo" /></label><label className="min-w-32">响应格式<button className="json-mode-toggle" type="button" aria-pressed={jsonMode} data-active={jsonMode} onClick={() => setJsonMode((value) => !value)}>{jsonMode ? "严格 JSON" : "普通文本"}</button></label></div>
      <label>API Key<input className="ai-setting-input" type="password" autoComplete="off" value={apiKey} onChange={(event) => setApiKey(event.target.value)} placeholder={status?.keyConfigured ? "已安全保存；留空表示不更改" : "输入后保存到 Windows 凭据管理器"} /></label>
      <div className="ai-security-note"><ShieldCheck className="size-4" /><span>端点与模型保存在本地偏好中；API Key 不会写入 JSON、SQLite、日志或同步目录。公网端点强制 HTTPS。</span></div>
      <div className="flex justify-between pt-1"><Button type="button" variant="danger" disabled={!status?.keyConfigured || busy} onClick={() => void clear()}><Trash2 className="size-3.5" />删除凭据</Button><Button type="submit" disabled={busy || !endpoint || !model}>{busy ? <LoaderCircle className="size-4 animate-spin" /> : <Bot className="size-4" />}保存 Provider</Button></div>
    </form>
  </Card>;
}
