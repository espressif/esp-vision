// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

import { createHash } from "node:crypto";
import { gunzipSync } from "node:zlib";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";
import { dirname, resolve } from "node:path";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = resolve(packageRoot, "../..");

function readTarball(bytes) {
  const tar = gunzipSync(bytes, { maxOutputLength: 128 * 1024 * 1024 });
  const entries = new Map();
  let pendingLongName;
  let offset = 0;
  while (offset + 512 <= tar.length) {
    const header = tar.subarray(offset, offset + 512);
    if (header.every((byte) => byte === 0)) break;
    const name = header.subarray(0, 100).toString("utf-8").replace(/\0.*$/s, "");
    const prefix = header.subarray(345, 500).toString("utf-8").replace(/\0.*$/s, "");
    const type = header[156];
    const rawSize = header.subarray(124, 136).toString("ascii").replace(/\0.*$/s, "").trim();
    const size = rawSize ? Number.parseInt(rawSize, 8) : 0;
    if (!Number.isSafeInteger(size) || size < 0) throw new Error("The npm tarball contains an invalid entry.");
    const dataStart = offset + 512;
    if (dataStart + size > tar.length) throw new Error("Truncated npm archive");
    const content = tar.subarray(dataStart, dataStart + size);
    if (type === 0x4c) {
      pendingLongName = content.toString("utf-8").replace(/\0.*$/s, "");
    } else if (type === 0x78 || type === 0x67) {
      // npm's file list is short enough to use ustar names; ignore PAX metadata.
    } else {
      const path = pendingLongName ?? [prefix, name].filter(Boolean).join("/");
      pendingLongName = undefined;
      if (entries.has(path) || path.split("/").includes("..")) throw new Error("Unsafe or duplicate npm entry");
      if (type === 0 || type === 0x30) entries.set(path, Buffer.from(content));
      else if (type !== 0x35) throw new Error("The npm tarball contains an unsupported entry type.");
    }
    offset = dataStart + Math.ceil(size / 512) * 512;
  }
  return entries;
}

async function validateIdentity(packageJson, catalogBytes, provenanceBytes, packBytes) {
  const provenance = JSON.parse(provenanceBytes);
  const digest = createHash("sha256").update(catalogBytes).digest("hex");
  if (provenance.schemaVersion !== 2 || provenance.packageName !== packageJson.name ||
      provenance.packageVersion !== packageJson.version || provenance.catalogSha256 !== digest ||
      !/^[a-f0-9]{40}$/.test(provenance.sourceCommit ?? "")) throw new Error("Package provenance does not match catalog/runtime identity");
  if (Object.keys(packageJson.dependencies ?? {}).length) throw new Error("Runtime dependencies are not allowed");
  const temporary = await mkdtemp(resolve(tmpdir(), "knowledge-check-"));
  try {
    const path = resolve(temporary, "mcp-catalog.json");
    await writeFile(path, catalogBytes);
    const validation = spawnSync("python3", [resolve(repositoryRoot, "tools/ci/mcp/catalog.py"), "check", "--catalog", path], { stdio: "inherit" });
    if (validation.status !== 0) throw new Error("Knowledge catalog validation failed");
    const catalog = JSON.parse(catalogBytes);
    if (!catalog.snapshots.some(s => Buffer.from(s.packJson).equals(packBytes))) throw new Error("Compatibility pack is outside catalog");
    // Validate every retained firmware pack, never only the active/first one.
    for (const snapshot of catalog.snapshots) {
      const packPath = resolve(temporary, "mcp-pack.json");
      await writeFile(packPath, snapshot.packJson);
      const checked = spawnSync("python3", [resolve(repositoryRoot, "tools/ci/mcp/validate_schema.py"), packPath, "--require-micropython"], { stdio: "inherit" });
      if (checked.status !== 0) throw new Error("Knowledge pack validation failed");
    }
  } finally { await rm(temporary, { recursive: true, force: true }); }
  return digest;
}

function parseTarballArgument(args) {
  if (args.length === 0) return undefined;
  if (args.length !== 2 || args[0] !== "--tarball" || !args[1] || args[1].startsWith("--")) {
    throw new Error("Usage: node scripts/check-package.mjs [--tarball FILE]");
  }
  return resolve(process.cwd(), args[1]);
}

async function main() {
  const tarballPath = parseTarballArgument(process.argv.slice(2));
  const expectedPackage = JSON.parse(await readFile(resolve(packageRoot, "package.json"), "utf-8"));
  let packageJson = expectedPackage;
  let packBytes;
  let catalogBytes;
  let provenanceBytes;

  if (tarballPath) {
    const entries = readTarball(await readFile(tarballPath));
    const expectedFiles = new Set([
      "package/package.json",
      "package/data/mcp-pack.json",
      "package/data/mcp-catalog.json",
      "package/data/provenance.json",
      "package/LICENSE",
      "package/NOTICE",
      "package/README.md",
      "package/THIRD_PARTY_NOTICES.md",
    ]);
    for (const path of entries.keys()) {
      if (path.includes("node_modules/")) throw new Error("The npm tarball must not contain node_modules.");
      if (!path.startsWith("package/") ||
          !(expectedFiles.has(path) || path.startsWith("package/dist/"))) {
        throw new Error(`Unexpected npm tarball path: ${path}`);
      }
    }
    for (const path of expectedFiles) {
      if (!entries.has(path)) throw new Error(`The npm tarball is missing ${path}.`);
    }
    for (const path of ["package/dist/cli.js", "package/dist/server.js", "package/dist/service.js", "package/dist/stdio.js", "package/dist/stats-cli.js"]) {
      if (!entries.has(path)) throw new Error(`The npm tarball is missing ${path}.`);
    }

    packageJson = JSON.parse(entries.get("package/package.json").toString("utf-8"));
    if (packageJson.name !== expectedPackage.name || packageJson.version !== expectedPackage.version) {
      throw new Error("The npm tarball package name/version does not match the current package release.");
    }
    packBytes = entries.get("package/data/mcp-pack.json");
    provenanceBytes = entries.get("package/data/provenance.json");
    catalogBytes = entries.get("package/data/mcp-catalog.json");
    const digest = await validateIdentity(packageJson, catalogBytes, provenanceBytes, packBytes);
    process.stdout.write(`Tarball ready: ${packageJson.name}@${packageJson.version}, catalog ${digest}\n`);
    return;
  }

  packBytes = await readFile(resolve(packageRoot, "data/mcp-pack.json")).catch(() => undefined);
  provenanceBytes = await readFile(resolve(packageRoot, "data/provenance.json")).catch(() => undefined);
  if (!packBytes || !provenanceBytes) {
    throw new Error("A schema-validated production pack is required before npm pack or release.");
  }
  catalogBytes = await readFile(resolve(packageRoot, "data/mcp-catalog.json"));
  const digest = await validateIdentity(packageJson, catalogBytes, provenanceBytes, packBytes);
  for (const artifact of ["cli.js", "server.js", "service.js", "stdio.js", "stats-cli.js"]) {
    await readFile(resolve(packageRoot, "dist", artifact)).catch(() => {
      throw new Error(`Missing bundled runtime artifact dist/${artifact}.`);
    });
  }
  for (const filename of ["LICENSE", "NOTICE", "README.md", "THIRD_PARTY_NOTICES.md"]) {
    await readFile(resolve(packageRoot, filename)).catch(() => {
      throw new Error(`Missing package notice artifact ${filename}.`);
    });
  }
  process.stdout.write(`Package ready: ${packageJson.name}@${packageJson.version}, catalog ${digest}\n`);
}

main().catch(async (error) => {
  process.stderr.write((error instanceof Error ? error.message : "Package check failed.") + "\n");
  process.exitCode = 1;
});
