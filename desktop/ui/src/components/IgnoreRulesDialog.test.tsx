import "@testing-library/jest-dom/vitest";
import { cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { IgnoreRulesDialog } from "./IgnoreRulesDialog";
import { ai } from "@/lib/ai";
import { engine, type IgnorePreview, type SyncTask } from "@/lib/ipc";

const task: SyncTask = {
  id: "native", mode: "bidirectional", role: "peer", root: "D:/同步🙂",
  enabled: true, runtimeStatus: "watching", dirty: false, lastScanAt: null,
  runtimeError: null, networkStatus: "online", networkError: null
};
const policy = { revision: 2, hash: "policy-hash", rules: "*.log\n", canUndo: true };
const preview: IgnorePreview = {
  expectedHash: policy.hash, scannedFiles: 10, currentlyIgnored: 1, proposedIgnored: 2,
  newlyIgnored: 1, newlyIncluded: 0, trackedNewlyIgnored: 1, truncated: false,
  newlyIgnoredSamples: ["build/a.bin"], newlyIncludedSamples: [], trackedDeletionSamples: ["build/a.bin"]
};

async function openDialog(value: SyncTask = task) {
  render(<IgnoreRulesDialog task={value} />);
  fireEvent.click(screen.getByRole("button", { name: "忽略规则" }));
  const editor = await screen.findByRole("textbox", { name: "忽略规则内容" });
  await waitFor(() => expect(editor).toHaveValue(policy.rules));
  return editor;
}

beforeEach(() => {
  vi.spyOn(engine, "ignoreRules").mockResolvedValue(policy);
  vi.spyOn(engine, "previewIgnoreRules").mockResolvedValue(preview);
  vi.spyOn(engine, "applyIgnoreRules").mockResolvedValue(policy);
  vi.spyOn(engine, "undoIgnoreRules").mockResolvedValue({ ...policy, revision: 1, canUndo: false });
});
afterEach(() => { cleanup(); vi.restoreAllMocks(); });

describe("ignore policy interactions", () => {
  it.each([
    { ...task, mode: "one_way" as const, role: "target" as const },
    { ...task, networkStatus: "offline" as const },
    { ...task, enabled: false }
  ])("keeps target/offline/paused-peer policies read-only ($role/$networkStatus/$enabled)", async (value) => {
    const editor = await openDialog(value);
    expect(editor).toHaveAttribute("readonly");
    expect(screen.getByRole("button", { name: "预览并应用" })).toBeDisabled();
    expect(screen.getByRole("button", { name: "撤销上一版" })).toBeDisabled();
    expect(engine.applyIgnoreRules).not.toHaveBeenCalled();
  });

  it("allows the authoritative source to edit without a paired peer", async () => {
    const editor = await openDialog({ ...task, mode: "one_way", role: "source", networkStatus: "unpaired" });
    expect(editor).not.toHaveAttribute("readonly");
    fireEvent.change(editor, { target: { value: "build/\n" } });
    expect(screen.getByRole("button", { name: "预览并应用" })).toBeEnabled();
  });

  it("requires a separate risk confirmation before applying connected-peer rules", async () => {
    const editor = await openDialog();
    fireEvent.change(editor, { target: { value: "build/\n" } });
    fireEvent.click(screen.getByRole("button", { name: "预览并应用" }));
    const confirm = await screen.findByRole("button", { name: "确认风险并应用" });
    expect(engine.applyIgnoreRules).not.toHaveBeenCalled();
    fireEvent.click(confirm);
    await waitFor(() => expect(engine.applyIgnoreRules).toHaveBeenCalledWith("native", policy.hash, "build/\n", "manual"));
  });

  it("undoes only a clean policy and uses its current hash", async () => {
    const editor = await openDialog();
    fireEvent.change(editor, { target: { value: "build/\n" } });
    expect(screen.getByRole("button", { name: "撤销上一版" })).toBeDisabled();
    fireEvent.change(editor, { target: { value: policy.rules } });
    fireEvent.click(screen.getByRole("button", { name: "撤销上一版" }));
    await waitFor(() => expect(engine.undoIgnoreRules).toHaveBeenCalledWith("native", policy.hash));
  });

  it("keeps private AI suggestions as an unapplied draft", async () => {
    vi.spyOn(ai, "generateIgnoreRules").mockResolvedValue({
      rules: ["build/"], explanation: "Ignore build output", provider: "local test", model: "test"
    });
    await openDialog();
    fireEvent.change(screen.getByLabelText("用自然语言描述要排除的内容"), { target: { value: "忽略构建文件" } });
    fireEvent.click(screen.getByRole("button", { name: "生成候选规则" }));
    await waitFor(() => expect(screen.getByRole("textbox", { name: "忽略规则内容" })).toHaveValue("*.log\n\n# AI 建议（应用前已由用户确认）\nbuild/\n"));
    expect(ai.generateIgnoreRules).toHaveBeenCalledWith("native", "忽略构建文件", "private");
    expect(engine.applyIgnoreRules).not.toHaveBeenCalled();
  });
});
