import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { createServer } from "node:net";
import { spawn } from "node:child_process";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { pathToFileURL } from "node:url";
import { join, resolve } from "node:path";
import { test } from "node:test";
import { Pack, createKnowledgeServer } from "../dist/service.js";
import { packPath, runNode, serviceRoot, temporaryDirectory } from "./helpers.mjs";

const cliPath = resolve(serviceRoot, "dist/cli.js");
const packVersion = JSON.parse(readFileSync(packPath, "utf8")).meta.version;

test("service factory imports without loading packs, opening stores, or listening", async (t) => {
  const directory = await temporaryDirectory(t, "factory-import-");
  const databasePath = join(directory, "must-not-exist.sqlite");
  const missingPackPath = join(directory, "must-not-load.json");
  const factoryUrl = pathToFileURL(resolve(serviceRoot, "dist/service.js")).href;
  const child = await runNode(["--input-type=module", "-e", `
    import assert from "node:assert/strict";
    import { Server } from "node:net";
    import { Socket } from "node:net";
    import { existsSync, readFileSync } from "node:fs";
    const forbidden = () => { throw new Error("unexpected I/O during module import"); };
    Server.prototype.listen = forbidden;
    Socket.prototype.connect = forbidden;
    globalThis.fetch = forbidden;
    await import(${JSON.stringify(factoryUrl)});
    assert.equal(existsSync(process.env.TEST_STATS_DB), false);
    assert.equal(process.env.MCP_PACK_PATH, process.env.TEST_MISSING_PACK);
  `], {
    env: {
      MCP_PACK_PATH: missingPackPath,
      MCP_STATS_DB_PATH: databasePath,
      TEST_STATS_DB: databasePath,
      TEST_MISSING_PACK: missingPackPath,
    },
  });
  assert.equal(child.code, 0, child.stderr);
  assert.equal(existsSync(databasePath), false);
});

test("public factory serves the existing tools through an in-memory MCP client", async () => {
  const pack = Pack.load(packPath);
  const server = createKnowledgeServer({ pack });
  const client = new Client({ name: "knowledge-factory-test", version: "1.0.0" });
  const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
  try {
    await server.connect(serverTransport);
    await client.connect(clientTransport);
    const tools = await client.listTools();
    assert.equal(tools.tools.length, 9);
    assert.deepEqual(tools.tools.map(({ name }) => name).sort(), [
      "get_api", "get_capabilities", "get_concept", "get_example", "list_examples",
      "list_modules", "list_symbols", "search_docs", "server_info",
    ]);
    const reply = await client.callTool({ name: "server_info", arguments: {} });
    const info = JSON.parse(reply.content[0].text);
    assert.equal(info.name, "esp-vision-mcp");
    assert.equal(info.version, packVersion);
    assert.equal(info.packageVersion, undefined);
  } finally {
    await client.close().catch(() => {});
    await server.close().catch(() => {});
  }
});

test("stdio is the default, emits MCP only to stdout, and ignores statistics settings", async (t) => {
  const directory = await temporaryDirectory(t, "stdio-");
  const dbPath = join(directory, "stdio-must-ignore.sqlite");
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: [cliPath, "--pack", packPath],
    cwd: serviceRoot,
    env: {
      MCP_STATS_DB_PATH: dbPath,
      MCP_STATS_RETENTION_DAYS: "invalid",
      TMPDIR: process.env.TMPDIR,
      TMP: process.env.TMP,
      TEMP: process.env.TEMP,
    },
    stderr: "pipe",
  });
  const stderr = [];
  transport.stderr?.on("data", (chunk) => stderr.push(chunk));
  const client = new Client({ name: "knowledge-stdio-test", version: "1.0.0" });
  try {
    await client.connect(transport);
    const tools = await client.listTools();
    assert.equal(tools.tools.length, 9);
    const reply = await client.callTool({ name: "server_info", arguments: {} });
    assert.equal(JSON.parse(reply.content[0].text).version, packVersion);
  } finally {
    await client.close().catch(() => {});
  }
  assert.equal(existsSync(dbPath), false);
  assert.doesNotMatch(Buffer.concat(stderr).toString("utf8"), /fixture|MCP_STATS|sqlite/i);
});

test("stdio closes cleanly on EOF and does not fall back to a fixture pack", async (t) => {
  const directory = await temporaryDirectory(t, "stdio-eof-");
  const databasePath = join(directory, "eof-must-ignore.sqlite");
  const eof = await runNode([cliPath, "--pack", packPath], {
    env: { MCP_STATS_DB_PATH: databasePath },
  });
  assert.equal(eof.code, 0, eof.stderr);
  assert.equal(eof.stdout, "");
  assert.equal(existsSync(databasePath), false);

  const noPack = await runNode([cliPath], {
    env: { MCP_PACK_PATH: join(directory, "missing-production-pack.json") },
  });
  assert.equal(noPack.code, 1);
  assert.equal(noPack.stdout, "");
  assert.match(noPack.stderr, /configured path/);
});

test("stdio closes the server transport on SIGTERM", async (t) => {
  const directory = await temporaryDirectory(t, "stdio-signal-");
  const databasePath = join(directory, "signal-must-ignore.sqlite");
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: [cliPath, "--pack", packPath],
    cwd: serviceRoot,
    env: {
      MCP_STATS_DB_PATH: databasePath,
      TMPDIR: process.env.TMPDIR,
      TMP: process.env.TMP,
      TEMP: process.env.TEMP,
    },
    stderr: "pipe",
  });
  const stderr = [];
  transport.stderr?.on("data", (chunk) => stderr.push(chunk));
  const client = new Client({ name: "knowledge-signal-test", version: "1.0.0" });
  await client.connect(transport);
  await client.listTools();
  const child = transport._process;
  assert.ok(child?.pid);
  t.after(() => { if (child.exitCode === null) child.kill("SIGKILL"); });
  const exit = new Promise((resolveExit) => child.once("exit", (code, signal) => resolveExit({ code, signal })));
  child.kill("SIGTERM");
  const result = await exit;
  await client.close().catch(() => {});
  assert.equal(result.signal, null);
  assert.equal(result.code, 143);
  assert.equal(existsSync(databasePath), false);
  assert.doesNotMatch(Buffer.concat(stderr).toString("utf8"), /fixture|sqlite|MCP_STATS/i);
});

test("CLI HTTP mode accepts port and statistics config and serves the existing health route", async (t) => {
  const directory = await temporaryDirectory(t, "http-cli-");
  const databasePath = join(directory, "cli-usage.sqlite");
  const listener = createServer();
  await new Promise((resolveListen, rejectListen) => {
    listener.once("error", rejectListen);
    listener.listen(0, "127.0.0.1", resolveListen);
  });
  const address = listener.address();
  assert.ok(address && typeof address === "object");
  const port = address.port;
  await new Promise((resolveClose, rejectClose) => listener.close((error) => error ? rejectClose(error) : resolveClose()));

  const child = spawn(process.execPath, [
    cliPath,
    "--transport", "http",
    "--pack", packPath,
    "--port", String(port),
    "--stats-db", databasePath,
    "--stats-retention-days", "2",
  ], {
    cwd: serviceRoot,
    env: { ...process.env, TMPDIR: process.env.TMPDIR, TMP: process.env.TMP, TEMP: process.env.TEMP },
    stdio: ["ignore", "pipe", "pipe"],
  });
  const stdout = [];
  const stderr = [];
  child.stdout.on("data", (chunk) => stdout.push(chunk));
  child.stderr.on("data", (chunk) => stderr.push(chunk));
  t.after(() => { if (child.exitCode === null) child.kill("SIGKILL"); });

  const baseUrl = `http://127.0.0.1:${port}`;
  let response;
  const deadline = Date.now() + 5_000;
  while (Date.now() < deadline) {
    if (child.exitCode !== null) break;
    try {
      response = await fetch(`${baseUrl}/health`);
      if (response.ok) break;
    } catch {
      // The CLI is still binding its selected port.
    }
    await new Promise((resolveDelay) => setTimeout(resolveDelay, 30));
  }
  assert.ok(response?.ok, `health route did not start: ${Buffer.concat(stderr).toString("utf8")}`);
  assert.equal((await response.json()).version, packVersion);
  assert.equal((await fetch(`${baseUrl}/mcp/health`)).status, 200);
  assert.equal(existsSync(databasePath), true);

  const exit = new Promise((resolveExit) => child.once("exit", (code, signal) => resolveExit({ code, signal })));
  child.kill("SIGTERM");
  const result = await exit;
  assert.equal(result.signal, null);
  assert.equal(result.code, 143);
  assert.match(Buffer.concat(stdout).toString("utf8"), /esp-vision MCP server on/);
});
