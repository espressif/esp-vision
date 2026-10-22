import assert from "node:assert/strict";
import { createServer } from "node:net";
import { spawn } from "node:child_process";
import { mkdir, mkdtemp, rm } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";

export const serviceRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
export const packPath = process.env.MCP_TEST_PACK_PATH?.trim() ||
  resolve(serviceRoot, "test/fixtures/mcp-pack.json");

const testTempRoot = process.env.TMPDIR || join(serviceRoot, ".cache", "mcp-usage-statistics-tests");

export async function temporaryDirectory(t, prefix = "run-") {
  await mkdir(testTempRoot, { recursive: true });
  const path = await mkdtemp(join(testTempRoot, prefix));
  t.after(() => rm(path, { recursive: true, force: true }));
  return path;
}

export function runNode(args, { env = {}, cwd = serviceRoot } = {}) {
  return new Promise((resolveRun, rejectRun) => {
    const child = spawn(process.execPath, args, {
      cwd,
      env: { ...process.env, ...env },
      stdio: ["ignore", "pipe", "pipe"],
    });
    const stdout = [];
    const stderr = [];
    child.stdout.on("data", (chunk) => stdout.push(chunk));
    child.stderr.on("data", (chunk) => stderr.push(chunk));
    child.once("error", rejectRun);
    child.once("close", (code, signal) => resolveRun({
      code,
      signal,
      stdout: Buffer.concat(stdout).toString("utf8"),
      stderr: Buffer.concat(stderr).toString("utf8"),
    }));
  });
}

async function freePort() {
  const server = createServer();
  await new Promise((resolveListen, rejectListen) => {
    server.once("error", rejectListen);
    server.listen(0, "127.0.0.1", resolveListen);
  });
  const address = server.address();
  assert.ok(address && typeof address === "object");
  await new Promise((resolveClose, rejectClose) => server.close((error) => error ? rejectClose(error) : resolveClose()));
  return address.port;
}

export async function startServer(t, { statsPath, pack = packPath, extraEnv = {} } = {}) {
  const port = await freePort();
  const child = spawn(process.execPath, [resolve(serviceRoot, "dist/server.js")], {
    cwd: serviceRoot,
    env: {
      ...process.env,
      PORT: String(port),
      MCP_PACK_PATH: pack,
      ...(statsPath ? { MCP_STATS_DB_PATH: statsPath } : {}),
      ...extraEnv,
    },
    stdio: ["ignore", "pipe", "pipe"],
  });
  const stdout = [];
  const stderr = [];
  child.stdout.on("data", (chunk) => stdout.push(chunk));
  child.stderr.on("data", (chunk) => stderr.push(chunk));

  const server = {
    child,
    baseUrl: `http://127.0.0.1:${port}`,
    output: () => `${Buffer.concat(stdout).toString("utf8")}${Buffer.concat(stderr).toString("utf8")}`,
    stopped: false,
  };
  t.after(() => stopServer(server));
  await waitForHealth(server);
  return server;
}

export async function waitForHealth(server) {
  const deadline = Date.now() + 10_000;
  let lastError;
  while (Date.now() < deadline) {
    if (server.child.exitCode !== null) {
      throw new Error(`MCP server exited before readiness (code ${server.child.exitCode}): ${server.output()}`);
    }
    try {
      const response = await fetch(`${server.baseUrl}/health`);
      if (response.ok) return await response.json();
      lastError = new Error(`health returned HTTP ${response.status}`);
    } catch (error) {
      lastError = error;
    }
    await new Promise((resolveDelay) => setTimeout(resolveDelay, 40));
  }
  throw new Error(`MCP server did not become ready: ${lastError}\n${server.output()}`);
}

export async function stopServer(server) {
  if (server.stopped) return;
  server.stopped = true;
  if (server.child.exitCode !== null) return;
  server.child.kill("SIGTERM");
  await new Promise((resolveExit) => {
    const timer = setTimeout(() => {
      server.child.kill("SIGKILL");
      resolveExit();
    }, 5_000);
    server.child.once("exit", () => {
      clearTimeout(timer);
      resolveExit();
    });
  });
}

let rpcId = 0;

export async function rpc(server, method, params = {}, { endpoint = "/mcp", notification = false, headerCanary } = {}) {
  const url = `${server.baseUrl}${endpoint}`;
  const request = {
    jsonrpc: "2.0",
    method,
    params,
    ...(notification ? {} : { id: ++rpcId }),
  };
  const response = await fetch(url, {
    method: "POST",
    headers: {
      accept: "application/json, text/event-stream",
      "content-type": "application/json",
      "mcp-protocol-version": "2025-03-26",
      ...(headerCanary ? { "x-test-canary": headerCanary } : {}),
    },
    body: JSON.stringify(request),
  });
  const text = await response.text();
  if (!text) return { response, payload: undefined, text };

  let payload;
  if (response.headers.get("content-type")?.includes("text/event-stream")) {
    const data = text.split(/\r?\n/).find((line) => line.startsWith("data:"))?.slice(5).trim();
    payload = data ? JSON.parse(data) : undefined;
  } else {
    payload = JSON.parse(text);
  }
  return { response, payload, text };
}

export async function callTool(server, name, args, options = {}) {
  return rpc(server, "tools/call", { name, arguments: args }, options);
}
