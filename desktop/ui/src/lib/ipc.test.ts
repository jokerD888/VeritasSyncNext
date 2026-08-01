import { describe, expect, it } from "vitest";
import { frameRows, parseEvents, parseStatus, parseTasks } from "./ipc";

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
});
