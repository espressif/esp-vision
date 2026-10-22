import assert from "node:assert/strict";
import { DatabaseSync } from "node:sqlite";
import { readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { test } from "node:test";
import { readUsageReport, TOOL_NAMES } from "../dist/usage-stats.js";
import {
  callTool,
  packPath,
  rpc,
  startServer,
  stopServer,
  temporaryDirectory,
} from "./helpers.mjs";

const queryCanary = "QUERY_CANARY_private-search-input_9841";
const headerCanary = "HEADER_CANARY_private-client-header_8210";
const clientCanary = "CLIENT_CANARY_private-client-name_0923";
const bodyCanary = "BODY_CANARY_private-malformed-json_4732";

function assertSuccessfulTool(exchange) {
  assert.equal(exchange.response.ok, true, `${exchange.response.status}: ${exchange.text}`);
  assert.equal(exchange.payload?.error, undefined, exchange.text);
  assert.ok(exchange.payload?.result, exchange.text);
  assert.notEqual(exchange.payload.result.isError, true, exchange.text);
  return exchange.payload.result;
}

async function initialize(server, options = {}) {
  const initialized = await rpc(server, "initialize", {
    protocolVersion: "2025-03-26",
    capabilities: {},
    clientInfo: { name: clientCanary, version: "1.0" },
  }, options);
  assert.equal(initialized.response.ok, true, initialized.text);
  assert.equal(initialized.payload?.error, undefined, initialized.text);
  await rpc(server, "notifications/initialized", {}, { ...options, notification: true });
}

async function readReport(path, days = 3) {
  return readUsageReport(path, days);
}

test("real Streamable HTTP execution records nine tools, aliases and restart persistence", async (t) => {
  const dir = await temporaryDirectory(t);
  const pathCanary = "PATH_CANARY_private-db-location_7102";
  const dbPath = join(dir, `stats-${pathCanary}.sqlite`);
  const server = await startServer(t, { statsPath: dbPath });
  const health = await fetch(`${server.baseUrl}/health`, { headers: { "x-test-canary": headerCanary } });
  const healthInfo = await health.json();
  assert.equal(health.status, 200);
  assert.equal(healthInfo.name, "esp-vision-mcp");
  assert.equal("usage" in healthInfo, false);
  assert.equal("statistics" in healthInfo, false);
  await initialize(server, { headerCanary });

  const discovery = await rpc(server, "tools/list", {}, { headerCanary });
  assert.equal(discovery.response.ok, true, discovery.text);
  const names = discovery.payload.result.tools.map(({ name }) => name).sort();
  assert.deepEqual(names, [...TOOL_NAMES].sort());

  const invocations = [
    ["get_api", { query: queryCanary }],
    ["get_capabilities", { name: "p4" }],
    ["search_docs", { query: "camera", limit: 3 }],
    ["list_modules", { target: "p4" }],
    ["list_symbols", { module: "sensor" }],
    ["get_example", { query: "save_snapshot" }],
    ["list_examples", { module: "sensor" }],
    ["get_concept", { topic: "camera-pipeline" }],
    ["server_info", {}],
  ];
  for (const [name, args] of invocations) {
    const exchange = await callTool(server, name, args, {
      endpoint: `/mcp?requestPathCanary=${encodeURIComponent(pathCanary)}`,
      headerCanary,
    });
    assertSuccessfulTool(exchange);
  }

  // The root and /mcp addresses are aliases for the same stateless tool set.
  assertSuccessfulTool(await callTool(server, "server_info", {}, { endpoint: "/", headerCanary }));

  // Initialize, discovery, health, resources, unknown tools and SDK argument
  // validation are outside the completed-handler measurement boundary.
  const unknownTool = await callTool(server, "unknown_tool", { query: queryCanary }, { headerCanary });
  assert.equal(unknownTool.response.ok, true);
  const rejectedArgs = await callTool(server, "get_api", { query: 7 }, { headerCanary });
  assert.equal(rejectedArgs.response.ok, true);
  const resource = await rpc(server, "resources/read", {
    uri: "esp-vision://module/sensor",
  }, { headerCanary });
  assert.equal(resource.response.ok, true, resource.text);
  const secondHealth = await fetch(`${server.baseUrl}/mcp/health`, {
    headers: { "x-test-canary": headerCanary },
  });
  assert.equal(secondHealth.status, 200);

  // Express parser failures must not echo body bytes or include them in logs.
  const malformed = await fetch(`${server.baseUrl}/mcp?${encodeURIComponent(pathCanary)}`, {
    method: "POST",
    headers: { "content-type": "application/json", "x-test-canary": headerCanary },
    body: `{"submitted":"${bodyCanary}`,
  });
  const malformedText = await malformed.text();
  assert.ok(malformed.status >= 400 && malformed.status < 500, `${malformed.status}: ${malformedText}`);
  assert.doesNotMatch(malformedText, new RegExp(bodyCanary));

  const totals = await readReport(dbPath);
  assert.equal(totals.totals.count, 10);
  assert.equal(totals.totals.success, 10);
  assert.equal(totals.totals.error, 0);
  assert.equal(totals.tools.length, TOOL_NAMES.length);
  assert.equal(totals.tools.find(({ tool }) => tool === "server_info")?.count, 2);
  assert.ok(totals.tools.every(({ version }) => version === healthInfo.version));
  assert.doesNotMatch(JSON.stringify(totals), /query|client|session|ip|request|argument|result|exception/i);

  const publicPaths = ["/usage", "/stats", "/metrics", "/admin/usage"];
  for (const path of publicPaths) {
    const response = await fetch(`${server.baseUrl}${path}`);
    assert.equal(response.status, 404, `${path} unexpectedly exposed an endpoint`);
  }
  assert.equal(names.some((name) => /usage|stats/i.test(name)), false);

  await stopServer(server);
  const restarted = await startServer(t, { statsPath: dbPath });
  await initialize(restarted, { headerCanary });
  assertSuccessfulTool(await callTool(restarted, "server_info", {}, { headerCanary }));
  const persisted = await readReport(dbPath);
  assert.equal(persisted.totals.count, 11);
  assert.equal(persisted.tools.find(({ tool }) => tool === "server_info")?.count, 3);

  const sidecars = [dbPath, `${dbPath}-wal`, `${dbPath}-shm`];
  let storedText = "";
  for (const path of sidecars) {
    try { storedText += (await readFile(path)).toString("utf8"); }
    catch (error) { if (error.code !== "ENOENT") throw error; }
  }
  for (const sentinel of [queryCanary, headerCanary, clientCanary, bodyCanary, pathCanary]) {
    assert.ok(!storedText.includes(sentinel), `database contains ${sentinel}`);
  }
  assert.doesNotMatch(`${server.output()}${restarted.output()}`, /QUERY_CANARY|HEADER_CANARY|CLIENT_CANARY|BODY_CANARY|PATH_CANARY/);
});

test("storage initialization failure leaves knowledge queries available and content-free", async (t) => {
  const dir = await temporaryDirectory(t);
  const blockingFile = join(dir, "STORAGE_FAILURE_PATH_CANARY");
  const blockingContent = "STORAGE_FAILURE_CONTENT_CANARY";
  await writeFile(blockingFile, blockingContent);
  const unusableDbPath = join(blockingFile, "stats.sqlite");
  const server = await startServer(t, { statsPath: unusableDbPath });
  await initialize(server);
  const result = assertSuccessfulTool(await callTool(server, "get_api", { query: "sensor.snapshot" }));
  assert.ok(result.content?.length > 0);
  assert.doesNotMatch(server.output(), /STORAGE_FAILURE_PATH_CANARY|STORAGE_FAILURE_CONTENT_CANARY|stats\.sqlite/);
  assert.equal((await fetch(`${server.baseUrl}/health`)).status, 200);
});

test("a locked statistics database cannot stall or fail a live knowledge query", async (t) => {
  const dir = await temporaryDirectory(t);
  const secretPath = "LIVE_WRITE_FAILURE_PATH_CANARY";
  const dbPath = join(dir, `${secretPath}.sqlite`);
  const server = await startServer(t, { statsPath: dbPath });
  await initialize(server);
  assertSuccessfulTool(await callTool(server, "server_info", {}));

  const lock = new DatabaseSync(dbPath);
  lock.exec("BEGIN IMMEDIATE");
  let elapsedMs;
  try {
    const started = performance.now();
    const response = await callTool(server, "get_api", { query: "sensor.snapshot" });
    elapsedMs = performance.now() - started;
    assertSuccessfulTool(response);
  } finally {
    lock.exec("ROLLBACK");
    lock.close();
  }

  assert.ok(elapsedMs < 500, `query under a SQLite writer lock took ${elapsedMs.toFixed(1)} ms`);
  assert.doesNotMatch(server.output(), /LIVE_WRITE_FAILURE_PATH_CANARY|SQLITE_|busy|locked/i);
  assert.equal((await readReport(dbPath)).totals.count, 1);
  assertSuccessfulTool(await callTool(server, "server_info", {}));
  assert.equal((await readReport(dbPath)).totals.count, 2);
});

test("invalid retention configuration disables collection but keeps the MCP server online", async (t) => {
  const dir = await temporaryDirectory(t);
  const dbPath = join(dir, "invalid-retention.sqlite");
  const canary = "INVALID_RETENTION_CANARY";
  const server = await startServer(t, {
    statsPath: dbPath,
    extraEnv: { MCP_STATS_RETENTION_DAYS: "0" },
  });
  await initialize(server);
  assertSuccessfulTool(await callTool(server, "server_info", {}));
  assert.doesNotMatch(server.output(), new RegExp(canary));
  await assert.rejects(readFile(dbPath));
});
