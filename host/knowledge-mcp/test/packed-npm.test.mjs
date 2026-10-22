import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { cp, mkdir, readFile, writeFile } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { join, resolve } from "node:path";
import { test } from "node:test";
import { packPath, serviceRoot, temporaryDirectory } from "./helpers.mjs";

function run(command, args, options) {
  const result = spawnSync(command, args, { encoding: "utf-8", ...options });
  if (result.error) throw result.error;
  return result;
}

test("packed npm artifact has no node_modules and serves stdio from its shipped pack", async (t) => {
  const temporary = await temporaryDirectory(t, "npm-packed-");
  const stage = join(temporary, "stage");
  const archiveDirectory = join(temporary, "archive");
  const extractDirectory = join(temporary, "extract");
  const stagedPackage = join(stage, "package");
  await Promise.all([
    mkdir(stagedPackage, { recursive: true }),
    mkdir(archiveDirectory, { recursive: true }),
    mkdir(extractDirectory, { recursive: true }),
  ]);

  for (const name of ["package.json", "LICENSE", "NOTICE", "README.md", "THIRD_PARTY_NOTICES.md"]) {
    await cp(resolve(serviceRoot, name), join(stagedPackage, name));
  }
  await cp(resolve(serviceRoot, "dist"), join(stagedPackage, "dist"), { recursive: true });
  const packBytes = await readFile(packPath);
  const pack = JSON.parse(packBytes.toString("utf-8"));
  const packSha256 = createHash("sha256").update(packBytes).digest("hex");
  await mkdir(join(stagedPackage, "data"), { recursive: true });
  await writeFile(join(stagedPackage, "data/mcp-pack.json"), packBytes);
  await writeFile(join(stagedPackage, "data/provenance.json"), JSON.stringify({
    schemaVersion: 1,
    packageName: "@esp-vision/knowledge-mcp",
    packageVersion: "0.1.0",
    packVersion: pack.meta.version,
    sourceCommit: pack.meta.sourceCommit,
    packSha256,
  }, null, 2) + "\n");

  const env = {
    ...process.env,
    TMPDIR: process.env.TMPDIR,
    TMP: process.env.TMP,
    TEMP: process.env.TEMP,
  };
  const packed = run("npm", ["pack", "--ignore-scripts", "--pack-destination", archiveDirectory, "--json"], {
    cwd: stagedPackage,
    env,
    timeout: 30_000,
  });
  assert.equal(packed.status, 0, packed.stderr);
  const archiveName = JSON.parse(packed.stdout)[0].filename;
  const archivePath = join(archiveDirectory, archiveName);
  const listing = run("tar", ["-tzf", archivePath], { encoding: "utf-8", timeout: 30_000 });
  assert.equal(listing.status, 0, listing.stderr);
  const paths = listing.stdout.trim().split("\n");
  assert.ok(paths.includes("package/package.json"));
  assert.ok(paths.includes("package/data/mcp-pack.json"));
  assert.ok(paths.includes("package/data/provenance.json"));
  assert.ok(paths.includes("package/NOTICE"));
  assert.ok(paths.includes("package/THIRD_PARTY_NOTICES.md"));
  assert.ok(paths.every((path) => !path.includes("node_modules/")), listing.stdout);

  const extracted = run("tar", ["-xzf", archivePath, "-C", extractDirectory], { timeout: 30_000 });
  assert.equal(extracted.status, 0, extracted.stderr);
  const cliPath = join(extractDirectory, "package/dist/cli.js");
  const statsPath = join(temporary, "stdio-must-not-create.sqlite");
  const transport = new StdioClientTransport({
    command: cliPath,
    args: [],
    cwd: temporary,
    env: {
      MCP_STATS_DB_PATH: statsPath,
      MCP_STATS_RETENTION_DAYS: "invalid",
      TMPDIR: process.env.TMPDIR,
      TMP: process.env.TMP,
      TEMP: process.env.TEMP,
    },
    stderr: "pipe",
  });
  const stderr = [];
  transport.stderr?.on("data", (chunk) => stderr.push(chunk));
  const client = new Client({ name: "packed-knowledge-test", version: "1.0.0" });
  try {
    await client.connect(transport);
    const tools = await client.listTools();
    assert.equal(tools.tools.length, 9);
    const reply = await client.callTool({ name: "server_info", arguments: {} });
    const info = JSON.parse(reply.content[0].text);
    assert.equal(info.packageVersion, "0.1.0");
    assert.equal(info.packSha256, packSha256);
    assert.equal(info.sourceCommit, pack.meta.sourceCommit);
    assert.equal(info.version, pack.meta.version);
  } finally {
    await client.close().catch(() => {});
  }
  assert.equal(await readFile(statsPath).then(() => true, () => false), false);
  assert.equal(Buffer.concat(stderr).toString("utf-8"), "");
});

test("package check rejects a code-only build without validated release data", async () => {
  const check = run(process.execPath, [resolve(serviceRoot, "scripts/check-package.mjs")], {
    cwd: serviceRoot,
    env: process.env,
  });
  assert.equal(check.status, 1);
  assert.match(check.stderr, /production pack is required/);
});
