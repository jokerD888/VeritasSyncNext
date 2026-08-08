import { describe, expect, it } from "vitest";
import { frameRows, ignorePreviewNeedsConfirmation, parseDashboard, parseDeviceIdentity, parseEvents, parseIgnorePolicy, parseIgnorePreview, parseJoinedInvitation, parsePairingInvitation, parseStatus, parseTasks } from "./ipc";

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

  it("parses a consistent dashboard snapshot", () => {
    const snapshot = parseDashboard("OK\t7\t1\nTASK\tphoto\tone_way\tsource\tD:%5CPhotos\t1\twatching\t0\t1700000000000\t\tonline\t\nEVENT\t8\tphoto\tinfo\tReady\t1700000000000\nCONFLICT\tc1\tunresolved\ta.txt\ta.conflict.txt\tv2\nEND\n");
    expect(snapshot.status).toEqual({ schemaVersion: "7", taskCount: 1 });
    expect(snapshot.tasks[0].root).toBe("D:\\Photos");
    expect(snapshot.tasks[0]).toMatchObject({ enabled: true, runtimeStatus: "watching", networkStatus: "online" });
    expect(snapshot.events[0].message).toBe("Ready");
    expect(snapshot.conflicts[0].id).toBe("c1");
  });

  it("parses device identity and invitation lifecycle frames", () => {
    expect(parseDeviceIdentity("OK\tdevice-a\tfingerprint-a\tpublic-a\n")).toEqual({ deviceId: "device-a", fingerprint: "fingerprint-a", publicKey: "public-a" });
    expect(parsePairingInvitation("OK\tVSINVITE1%7Ctoken\tABCD-EFGH\troom-a\n")).toEqual({ token: "VSINVITE1|token", code: "ABCD-EFGH", roomId: "room-a" });
    expect(parseJoinedInvitation("OK\tphotos\troom-a\n")).toEqual({ taskId: "photos", roomId: "room-a" });
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
