import { describe, expect, it } from "vitest";
import { frameRows, ignorePreviewNeedsConfirmation, parseEvents, parseIgnorePolicy, parseIgnorePreview, parseStatus, parseTasks } from "./ipc";

describe("IPC text frame adapter", () => {
  it("keeps row parsing independent from newlines and terminal END frames", () => {
    expect(frameRows("ROW\talpha\tone_way\tsource\tD:%5CData\nEND\n")).toEqual([["alpha", "one_way", "source", "D:\\Data"]]);
  });

  it("parses an engine status frame", () => {
    expect(parseStatus("OK\t4\t2\n")).toEqual({ schemaVersion: "4", taskCount: 2 });
  });

  it("maps task and event rows to typed records", () => {
    expect(parseTasks("ROW\tphoto\tbidirectional\tpeer\tD:%5CPhotos\nEND\n")[0]).toMatchObject({ id: "photo", mode: "bidirectional", role: "peer" });
    expect(parseEvents("ROW\t8\tphoto\twarning\tScan%20delayed\t1700000000000\nEND\n")[0]).toMatchObject({ id: "8", taskId: "photo", level: "warning", message: "Scan delayed" });
  });

  it("parses versioned ignore policy and risk preview frames", () => {
    expect(parseIgnorePolicy("OK\t2\tabc\t1\t*.log%0Abuild%2F%0A\n")).toEqual({
      revision: 2, hash: "abc", canUndo: true, rules: "*.log\nbuild/\n"
    });
    const preview = parseIgnorePreview("OK\tabc\t20\t1\t3\t2\t0\t1\t0\nIGNORE\tcache%2Fa.bin\nDELETE\tlogs%2Fold.log\nEND\n");
    expect(preview).toMatchObject({
      expectedHash: "abc", scannedFiles: 20, newlyIgnored: 2, trackedNewlyIgnored: 1,
      newlyIgnoredSamples: ["cache/a.bin"], trackedDeletionSamples: ["logs/old.log"]
    });
    expect(ignorePreviewNeedsConfirmation(preview)).toBe(true);
    expect(ignorePreviewNeedsConfirmation({ ...preview, trackedNewlyIgnored: 0, truncated: true })).toBe(true);
    expect(ignorePreviewNeedsConfirmation({ ...preview, trackedNewlyIgnored: 0, truncated: false })).toBe(false);
  });
});
