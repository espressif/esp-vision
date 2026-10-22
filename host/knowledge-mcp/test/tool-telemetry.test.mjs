import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { test } from "node:test";
import { measureTool } from "../dist/tool-telemetry.js";
import { readUsageReport } from "../dist/usage-stats.js";
import { runNode, serviceRoot, temporaryDirectory } from "./helpers.mjs";

test("measureTool returns the same result and records one successful completion", async () => {
  const calls = [];
  const stats = { record: (...args) => calls.push(args) };
  const args = { query: "the caller's input stays with the handler" };
  const result = { content: [{ type: "text", text: "answer" }] };
  let received;
  const wrapped = measureTool(stats, "get_api", "pack-v1", async (input) => {
    received = input;
    return result;
  });

  assert.equal(await wrapped(args), result);
  assert.equal(received, args);
  assert.equal(calls.length, 1);
  assert.equal(calls[0][0], "get_api");
  assert.equal(calls[0][1], "pack-v1");
  assert.equal(calls[0][2], "success");
  assert.ok(Number.isFinite(calls[0][3]) && calls[0][3] >= 0);
});

test("measureTool classifies MCP error results and thrown errors exactly once", async () => {
  const calls = [];
  const stats = { record: (...args) => calls.push(args) };
  const errorResult = { content: [{ type: "text", text: "handled error" }], isError: true };
  const resultWrapper = measureTool(stats, "get_capabilities", "pack-v1", () => errorResult);
  assert.equal(await resultWrapper(), errorResult);

  const thrown = new Error("THROWN_EXCEPTION_CANARY_8274");
  const throwWrapper = measureTool(stats, "search_docs", "pack-v1", () => { throw thrown; });
  await assert.rejects(throwWrapper(), (error) => error === thrown);

  assert.deepEqual(calls.map(([tool, , outcome]) => [tool, outcome]), [
    ["get_capabilities", "error"],
    ["search_docs", "error"],
  ]);
  assert.equal(calls.length, 2);
});

test("telemetry failures do not replace successful results or original errors", async () => {
  const unavailable = { record() { throw new Error("storage unavailable"); } };
  const result = { content: [] };
  assert.equal(await measureTool(unavailable, "get_api", "v", () => result)(), result);

  const original = new Error("handler failure");
  await assert.rejects(
    measureTool(unavailable, "get_api", "v", () => { throw original; })(),
    (error) => error === original,
  );
});

test("thrown exception text is not persisted or emitted", async (t) => {
  const dir = await temporaryDirectory(t);
  const dbPath = join(dir, "exception.sqlite");
  const sentinel = "PRIVATE_EXCEPTION_CANARY_4321";
  const statsUrl = pathToFileURL(resolve(serviceRoot, "dist/usage-stats.js")).href;
  const telemetryUrl = pathToFileURL(resolve(serviceRoot, "dist/tool-telemetry.js")).href;
  const childScript = `
    import { UsageStats } from ${JSON.stringify(statsUrl)};
    import { measureTool } from ${JSON.stringify(telemetryUrl)};
    const stats = new UsageStats({ dbPath: process.env.TEST_STATS_DB });
    const wrapped = measureTool(stats, "get_api", "1.0.0", () => {
      throw new Error(process.env.TEST_EXCEPTION_CANARY);
    });
    try { await wrapped(); } catch {}
    stats.close();
  `;
  const child = await runNode(["--input-type=module", "-e", childScript], {
    env: { TEST_STATS_DB: dbPath, TEST_EXCEPTION_CANARY: sentinel },
  });
  assert.equal(child.code, 0, child.stderr);

  const report = readUsageReport(dbPath, 1);
  assert.equal(report.totals.count, 1);
  assert.equal(report.totals.error, 1);
  assert.doesNotMatch(JSON.stringify(report), new RegExp(sentinel));
  assert.doesNotMatch(child.stdout, new RegExp(sentinel));
  assert.doesNotMatch(child.stderr, new RegExp(sentinel));
  assert.doesNotMatch((await readFile(dbPath)).toString("utf8"), new RegExp(sentinel));
});
