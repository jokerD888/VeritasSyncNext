import { describe, expect, it } from "vitest";
import { mergeGeneratedRules } from "./ai";

describe("mergeGeneratedRules", () => {
  it("deduplicates suggestions and preserves existing user rules", () => {
    expect(mergeGeneratedRules("*.log\n", ["*.log", "build/", " build/ "])).toBe(
      "*.log\n\n# AI 建议（应用前已由用户确认）\nbuild/\n"
    );
  });

  it("does not modify rules when every suggestion already exists", () => {
    expect(mergeGeneratedRules("*.log\n", ["*.log"])).toBe("*.log\n");
  });
});
