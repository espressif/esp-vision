import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { DatabaseSync } from "node:sqlite";
import { readFile, stat } from "node:fs/promises";
import { pathToFileURL } from "node:url";
import { join, resolve } from "node:path";
import { test } from "node:test";
import { readUsageReport, TOOL_NAMES, UsageStats } from "../dist/usage-stats.js";
import { runNode, serviceRoot, temporaryDirectory } from "./helpers.mjs";

const cliPath = resolve(serviceRoot, "dist/stats-cli.js");

function frozenDate(value) {
  let current = new Date(value);
  return {
    now: () => new Date(current),
    set: (next) => { current = new Date(next); },
  };
}

async function fileHash(path) {
  return createHash("sha256").update(await readFile(path)).digest("hex");
}

function reportShape(report) {
  assert.deepEqual(Object.keys(report).sort(), ["daily", "from", "timezone", "to", "tools", "totals"]);
  assert.deepEqual(Object.keys(report.totals).sort(), [
    "avgDurationMs", "count", "error", "errorRate", "maxDurationMs", "success",
  ]);
  for (const row of [...report.daily, ...report.tools]) {
    assert.deepEqual(Object.keys(row).sort(), [
      "avgDurationMs", "count", "error", "errorRate", "maxDurationMs", "success",
      ...(Object.hasOwn(row, "date") ? ["date"] : []),
      ...(Object.hasOwn(row, "tool") ? ["tool", "version"] : []),
    ].sort());
  }
}

test("disabled statistics and rejected records do not create a store", async (t) => {
  const disabled = new UsageStats({ now: () => new Date("2026-03-15T00:00:00.000Z") });
  assert.equal(disabled.record("get_api", "1.0.0", "success", 1), false);
  assert.doesNotThrow(() => disabled.close());

  const dir = await temporaryDirectory(t);
  const dbPath = join(dir, "validated.sqlite");
  const stats = new UsageStats({ dbPath, now: () => new Date("2026-03-15T12:00:00.000Z") });
  try {
    assert.equal(stats.record("not_registered", "1.0.0", "success", 1), false);
    assert.equal(stats.record("get_api", "1.0.0", "warning", 1), false);
    assert.equal(stats.record("get_api", "1.0.0", "success", Number.NaN), false);
    assert.equal(readUsageReport(dbPath, 1, () => new Date("2026-03-15T12:00:00.000Z")).totals.count, 0);
  } finally {
    stats.close();
  }
});

test("storage initialization failure disables collection without exposing its path", async (t) => {
  const dir = await temporaryDirectory(t);
  const secretPath = join(dir, "PRIVATE_STATS_PATH_CANARY", "usage.sqlite");
  const messages = [];
  const oldWarn = console.warn;
  console.warn = (message) => messages.push(String(message));
  let stats;
  try {
    assert.doesNotThrow(() => { stats = new UsageStats({ dbPath: secretPath }); });
    assert.equal(stats.record("get_api", "1.0.0", "success", 1), false);
    assert.doesNotThrow(() => stats.close());
  } finally {
    console.warn = oldWarn;
  }
  assert.equal(messages.length, 1);
  assert.doesNotMatch(messages[0], /PRIVATE_STATS_PATH_CANARY|usage\.sqlite/);
  await assert.rejects(readFile(secretPath));
});

test("aggregate counts and durations by UTC day and tool/version", async (t) => {
  const dir = await temporaryDirectory(t);
  const dbPath = join(dir, "usage.sqlite");
  const clock = frozenDate("2026-03-15T00:00:00.000Z");
  const stats = new UsageStats({ dbPath, retentionDays: 90, now: clock.now });
  try {
    assert.equal(stats.record("get_api", "2.0.0", "success", 10), true);
    assert.equal(stats.record("get_api", "1.0.0", "error", 20), true);
    assert.equal(stats.record("list_modules", "1.0.0", "success", 5), true);
    assert.equal(stats.record("list_modules", "1.0.0", "error", 40), true);

    const report = readUsageReport(dbPath, 1, clock.now);
    reportShape(report);
    assert.equal(report.timezone, "UTC");
    assert.equal(report.from, "2026-03-15");
    assert.equal(report.to, "2026-03-15");
    assert.deepEqual(report.totals, {
      count: 4, success: 2, error: 2, errorRate: 0.5,
      avgDurationMs: 18.75, maxDurationMs: 40,
    });
    assert.deepEqual(report.daily, [{
      date: "2026-03-15", count: 4, success: 2, error: 2, errorRate: 0.5,
      avgDurationMs: 18.75, maxDurationMs: 40,
    }]);
    assert.deepEqual(report.tools, [
      { tool: "get_api", version: "1.0.0", count: 1, success: 0, error: 1, errorRate: 1, avgDurationMs: 20, maxDurationMs: 20 },
      { tool: "get_api", version: "2.0.0", count: 1, success: 1, error: 0, errorRate: 0, avgDurationMs: 10, maxDurationMs: 10 },
      { tool: "list_modules", version: "1.0.0", count: 2, success: 1, error: 1, errorRate: 0.5, avgDurationMs: 22.5, maxDurationMs: 40 },
    ]);
  } finally {
    stats.close();
  }
});

test("UTC midnight boundary splits executions across calendar days", async (t) => {
  const dir = await temporaryDirectory(t);
  const dbPath = join(dir, "usage.sqlite");
  const clock = frozenDate("2025-12-31T23:59:59.999Z");
  const stats = new UsageStats({ dbPath, now: clock.now });
  try {
    assert.equal(stats.record("server_info", "1.0.0", "success", 1), true);
    clock.set("2026-01-01T00:00:00.000Z");
    assert.equal(stats.record("server_info", "1.0.0", "success", 3), true);
    const report = readUsageReport(dbPath, 2, clock.now);
    assert.equal(report.from, "2025-12-31");
    assert.equal(report.to, "2026-01-01");
    assert.deepEqual(report.daily.map(({ date }) => date), ["2025-12-31", "2026-01-01"]);
    assert.equal(report.totals.count, 2);
  } finally {
    stats.close();
  }
});

test("retention keeps its inclusive cutoff and current UTC day after reopen", async (t) => {
  const dir = await temporaryDirectory(t);
  const dbPath = join(dir, "usage.sqlite");
  const clock = frozenDate("2026-03-12T10:00:00.000Z");
  const initial = new UsageStats({ dbPath, retentionDays: 30, now: clock.now });
  assert.equal(initial.record("get_api", "1.0.0", "success", 1), true);
  clock.set("2026-03-13T10:00:00.000Z");
  assert.equal(initial.record("get_api", "1.0.0", "success", 2), true);
  clock.set("2026-03-15T00:00:00.000Z");
  assert.equal(initial.record("get_api", "1.0.0", "error", 3), true);
  initial.close();

  const reopened = new UsageStats({ dbPath, retentionDays: 3, now: clock.now });
  try {
    const report = readUsageReport(dbPath, 20, clock.now);
    assert.equal(report.from, "2026-03-13");
    assert.equal(report.to, "2026-03-15");
    assert.deepEqual(report.daily.map(({ date }) => date), ["2026-03-13", "2026-03-15"]);
    assert.equal(report.totals.count, 2);
  } finally {
    reopened.close();
  }
});

test("busy initialization disables collection without exposing database details", async (t) => {
  const dir = await temporaryDirectory(t);
  const dbPath = join(dir, "SETUP_CONTENTION_CANARY.sqlite");
  const initial = new UsageStats({ dbPath });
  initial.close();
  const lock = new DatabaseSync(dbPath);
  lock.exec("BEGIN IMMEDIATE");
  const messages = [];
  const oldWarn = console.warn;
  let stats;
  try {
    console.warn = (message) => messages.push(String(message));
    stats = new UsageStats({ dbPath });
    assert.equal(stats.record("get_api", "1.0.0", "success", 1), false);
  } finally {
    console.warn = oldWarn;
    lock.exec("ROLLBACK");
    lock.close();
    stats?.close();
  }
  assert.equal(messages.length, 1);
  assert.doesNotMatch(messages[0], /SETUP_CONTENTION_CANARY|sqlite|SQLITE|busy|locked/i);
});

test("acknowledged concurrent writes remain atomic across writer processes", async (t) => {
  const dir = await temporaryDirectory(t);
  const dbPath = join(dir, "concurrent.sqlite");
  const moduleUrl = pathToFileURL(resolve(serviceRoot, "dist/usage-stats.js")).href;
  const writer = `
    import assert from "node:assert/strict";
    import { UsageStats } from ${JSON.stringify(moduleUrl)};
    const stats = new UsageStats({ dbPath: process.env.TEST_STATS_DB, retentionDays: 30 });
    // Establish the writer before releasing the concurrent-write barrier.
    // Initialization can legitimately disable statistics on a busy database.
    assert.equal(stats.record("get_api", "1.0.0", "success", 0), true);
    process.on("message", () => {
      let count = 1;
      let durationSum = 0;
      let maxDurationMs = 0;
      for (let i = 0; i < 40; i++) {
        const durationMs = i + 1;
        if (stats.record("get_api", "1.0.0", "success", durationMs)) {
          count += 1;
          durationSum += durationMs;
          maxDurationMs = Math.max(maxDurationMs, durationMs);
        }
      }
      stats.close();
      process.send({ count, durationSum, maxDurationMs }, () => process.disconnect());
    });
    process.send("ready");
  `;
  const processes = [];
  for (let index = 0; index < 4; index++) {
    const child = spawn(process.execPath, ["--input-type=module", "-e", writer], {
      cwd: serviceRoot,
      env: { ...process.env, TEST_STATS_DB: dbPath },
      stdio: ["ignore", "ignore", "pipe", "ipc"],
    });
    t.after(() => { if (child.exitCode === null) child.kill("SIGKILL"); });
    let stderr = "";
    let acknowledgment;
    child.stderr.setEncoding("utf8").on("data", (chunk) => { stderr += chunk; });
    const complete = new Promise((resolveExit) => child.once("close", (code) => {
      resolveExit({ code, stderr, acknowledgment });
    }));
    await new Promise((resolveReady, rejectReady) => {
      const timer = setTimeout(() => rejectReady(new Error("stats writer did not become ready")), 5000);
      child.on("message", (message) => {
        if (message === "ready") {
          clearTimeout(timer);
          resolveReady();
        } else acknowledgment = message;
      });
      child.once("error", (error) => { clearTimeout(timer); rejectReady(error); });
      child.once("exit", (code) => {
        clearTimeout(timer);
        rejectReady(new Error(`stats writer exited ${code}: ${stderr}`));
      });
    });
    processes.push({ child, complete });
  }
  for (const { child } of processes) child.send("write");
  const writers = await Promise.all(processes.map(({ complete }) => complete));
  const acknowledgments = writers.map((result) => {
    assert.equal(result.code, 0, result.stderr);
    assert.doesNotMatch(result.stderr, /SQLITE_|busy|locked|concurrent\.sqlite/i);
    const acknowledgment = result.acknowledgment;
    assert.ok(acknowledgment.count > 0, "each writer must persist at least one acknowledged execution");
    return acknowledgment;
  });

  const report = readUsageReport(dbPath, 1);
  const acceptedCount = acknowledgments.reduce((sum, value) => sum + value.count, 0);
  const acceptedDurationSum = acknowledgments.reduce((sum, value) => sum + value.durationSum, 0);
  const acceptedMax = Math.max(...acknowledgments.map(({ maxDurationMs }) => maxDurationMs));
  assert.ok(acceptedCount > processes.length, "concurrent writes must add to the setup probes");
  assert.equal(report.totals.count, acceptedCount);
  assert.equal(report.totals.avgDurationMs, acceptedDurationSum / acceptedCount);
  assert.equal(report.totals.error, 0);
  assert.equal(report.totals.maxDurationMs, acceptedMax);
  assert.equal(report.tools[0].tool, "get_api");
  assert.equal(report.tools[0].count, acceptedCount);
  assert.equal(report.tools[0].avgDurationMs, acceptedDurationSum / acceptedCount);
});

test("a contended write fails open within a bounded time and emits no database details", async (t) => {
  const dir = await temporaryDirectory(t);
  const dbPath = join(dir, "DB_WRITE_FAILURE_CANARY.sqlite");
  const setup = new UsageStats({ dbPath });
  setup.close();
  const stats = new UsageStats({ dbPath });
  const holderScript = `
    import { DatabaseSync } from "node:sqlite";
    const db = new DatabaseSync(process.env.TEST_STATS_DB);
    db.exec("BEGIN IMMEDIATE");
    process.stdout.write("locked\\n");
    setTimeout(() => { db.exec("ROLLBACK"); db.close(); }, 1000);
  `;
  const holder = spawn(process.execPath, ["--input-type=module", "-e", holderScript], {
    cwd: serviceRoot,
    env: { ...process.env, TEST_STATS_DB: dbPath },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let holderOutput = "";
  let holderError = "";
  holder.stdout.setEncoding("utf8").on("data", (chunk) => { holderOutput += chunk; });
  holder.stderr.setEncoding("utf8").on("data", (chunk) => { holderError += chunk; });
  t.after(() => { if (holder.exitCode === null) holder.kill("SIGKILL"); });
  await new Promise((resolveReady, rejectReady) => {
    const timeout = setTimeout(() => rejectReady(new Error(`writer lock was not acquired: ${holderError}`)), 5000);
    const check = () => {
      if (holderOutput.includes("locked\n")) {
        clearTimeout(timeout);
        resolveReady();
      }
    };
    holder.stdout.on("data", check);
    holder.once("error", (error) => { clearTimeout(timeout); rejectReady(error); });
    holder.once("exit", (code) => {
      if (!holderOutput.includes("locked\n")) {
        clearTimeout(timeout);
        rejectReady(new Error(`lock holder exited ${code}: ${holderError}`));
      }
    });
    check();
  });

  const warnings = [];
  const oldWarn = console.warn;
  console.warn = (message) => warnings.push(String(message));
  let elapsedMs;
  try {
    const started = performance.now();
    assert.equal(stats.record("get_api", "1.0.0", "success", 1), false);
    elapsedMs = performance.now() - started;
  } finally {
    console.warn = oldWarn;
  }
  assert.ok(elapsedMs < 500, `contended stats write took ${elapsedMs.toFixed(1)} ms`);
  assert.equal(warnings.length, 1);
  assert.doesNotMatch(warnings[0], /DB_WRITE_FAILURE_CANARY|sqlite|SQLITE|busy|locked/i);

  await new Promise((resolveExit) => holder.once("exit", resolveExit));
  assert.equal(stats.record("get_api", "1.0.0", "success", 7), true);
  stats.close();
  const report = readUsageReport(dbPath, 1);
  assert.equal(report.totals.count, 1);
  assert.equal(report.totals.avgDurationMs, 7);
});

test("aggregate-only storage and report omit request content and identity fields", async (t) => {
  const dir = await temporaryDirectory(t);
  const dbPath = join(dir, "aggregate-only.sqlite");
  const stats = new UsageStats({ dbPath });
  try {
    for (const tool of TOOL_NAMES) assert.equal(stats.record(tool, "1.0.0", "success", 4), true);
  } finally {
    stats.close();
  }

  const db = new DatabaseSync(dbPath);
  try {
    const tables = db.prepare("SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'").all();
    assert.ok(tables.length > 0, "expected an aggregate table");
    const columns = tables.flatMap(({ name }) => db.prepare(`PRAGMA table_info('${String(name).replaceAll("'", "''")}')`).all().map(({ name: column }) => String(column).toLowerCase()));
    assert.ok(columns.length > 0);
    assert.doesNotMatch(columns.join(" "), /(?:^|_)id(?:_|$)|ip|session|request|client|user|query|argument|result|exception|raw|error_text/);
  } finally {
    db.close();
  }

  const report = readUsageReport(dbPath, 1);
  reportShape(report);
  assert.equal(report.totals.count, TOOL_NAMES.length);
  assert.doesNotMatch(JSON.stringify(report), /ip|session|request|client|user|query|argument|result|exception|raw/);
});

test("read reports and the CLI without changing database bytes", async (t) => {
  const dir = await temporaryDirectory(t);
  const dbPath = join(dir, "read-only.sqlite");
  const stats = new UsageStats({ dbPath });
  assert.equal(stats.record("server_info", "1.0.0", "success", 12), true);
  stats.close();

  const originalHash = await fileHash(dbPath);
  const before = await stat(dbPath);
  const direct = readUsageReport(dbPath, 7);
  assert.equal(direct.totals.count, 1);

  const cli = await runNode([cliPath, "--days", "7", "--format", "json"], {
    env: { MCP_STATS_DB_PATH: dbPath },
  });
  assert.equal(cli.code, 0, cli.stderr);
  assert.deepEqual(JSON.parse(cli.stdout), direct);
  assert.equal(await fileHash(dbPath), originalHash);
  assert.equal((await stat(dbPath)).mtimeMs, before.mtimeMs);

  const table = await runNode([cliPath, "--days", "1", "--format", "table"], {
    env: { MCP_STATS_DB_PATH: dbPath },
  });
  assert.equal(table.code, 0, table.stderr);
  assert.match(table.stdout, /server_info/);
  assert.match(table.stdout, /1\.0\.0/);
});

test("CLI rejects malformed ranges and unavailable stores without creating them", async (t) => {
  const dir = await temporaryDirectory(t);
  const validDb = join(dir, "valid.sqlite");
  const stats = new UsageStats({ dbPath: validDb });
  assert.equal(stats.record("server_info", "1.0.0", "success", 0), true);
  stats.close();

  for (const args of [
    ["--days", "0", "--format", "json"],
    ["--days", "3651", "--format", "json"],
    ["--days", "1.5", "--format", "json"],
    ["--days", "--format", "json"],
    ["--days", "7", "--format", "yaml"],
    ["--days", "7", "--format", "json", "--extra"],
  ]) {
    const result = await runNode([cliPath, ...args], { env: { MCP_STATS_DB_PATH: validDb } });
    assert.notEqual(result.code, 0, `${args.join(" ")} unexpectedly succeeded`);
  }

  const missingDb = join(dir, "missing-directory", "stats.sqlite");
  const unavailable = await runNode([cliPath, "--days", "7", "--format", "json"], {
    env: { MCP_STATS_DB_PATH: missingDb },
  });
  assert.notEqual(unavailable.code, 0);
  assert.doesNotMatch(`${unavailable.stdout}${unavailable.stderr}`, /missing-directory|stats\.sqlite/);
  assert.throws(() => readUsageReport(missingDb, 7), /Usage statistics report is unavailable/);
  await assert.rejects(readFile(missingDb));

  const disabled = await runNode([cliPath, "--days", "7", "--format", "json"], {
    env: { MCP_STATS_DB_PATH: "" },
  });
  assert.notEqual(disabled.code, 0);
});
