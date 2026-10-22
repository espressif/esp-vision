// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

import { createHash } from "node:crypto";
import { gunzipSync } from "node:zlib";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = resolve(packageRoot, "../..");

function readTarball(bytes) {
  const tar = gunzipSync(bytes);
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
    const content = tar.subarray(dataStart, dataStart + size);
    if (type === 0x4c) {
      pendingLongName = content.toString("utf-8").replace(/\0.*$/s, "");
    } else if (type === 0x78 || type === 0x67) {
      // npm's file list is short enough to use ustar names; ignore PAX metadata.
    } else {
      const path = pendingLongName ?? [prefix, name].filter(Boolean).join("/");
      pendingLongName = undefined;
      if (type === 0 || type === 0x30) entries.set(path, Buffer.from(content));
      else if (type !== 0x35) throw new Error("The npm tarball contains an unsupported entry type.");
    }
    offset = dataStart + Math.ceil(size / 512) * 512;
  }
  return entries;
}

function validateIdentity(packageJson, packBytes, provenanceBytes) {
  const pack = JSON.parse(packBytes.toString("utf-8"));
  const provenance = JSON.parse(provenanceBytes.toString("utf-8"));
  const packVersion = pack?.meta?.version;
  const sourceCommit = pack?.meta?.sourceCommit;
  if (!/^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$/.test(packVersion ?? "")) {
    throw new Error("The knowledge pack has an invalid meta.version.");
  }
  if (!/^[a-f0-9]{40}$/.test(sourceCommit ?? "")) {
    throw new Error("The knowledge pack meta.sourceCommit is not a full 40-character Git commit.");
  }
  const digest = createHash("sha256").update(packBytes).digest("hex");
  if (provenance.schemaVersion !== 1 ||
      provenance.packageName !== packageJson.name ||
      provenance.packageVersion !== packageJson.version ||
      provenance.packVersion !== packVersion ||
      provenance.sourceCommit !== sourceCommit ||
      provenance.packSha256 !== digest) {
    throw new Error("Package provenance does not match package.json and the exact knowledge-pack bytes.");
  }
  if (Object.keys(packageJson.dependencies ?? {}).length !== 0) {
    throw new Error("The public npm package must not have runtime npm dependencies.");
  }
  return { pack, provenance, digest };
}

function validateSchema(packPath) {
  const validation = spawnSync("python3", [
    resolve(repositoryRoot, "tools/ci/mcp/validate_schema.py"),
    packPath,
    "--require-micropython",
  ], {
    cwd: repositoryRoot,
    env: {
      ...process.env,
      TMPDIR: process.env.TMPDIR ?? resolve(repositoryRoot, ".esp-vision/tmp"),
      TMP: process.env.TMP ?? resolve(repositoryRoot, ".esp-vision/tmp"),
      TEMP: process.env.TEMP ?? resolve(repositoryRoot, ".esp-vision/tmp"),
    },
    stdio: "inherit",
  });
  if (validation.error) throw validation.error;
  if (validation.status !== 0) throw new Error("Knowledge pack validation failed.");
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
  let provenanceBytes;
  let temporaryDirectory;

  if (tarballPath) {
    const entries = readTarball(await readFile(tarballPath));
    const expectedFiles = new Set([
      "package/package.json",
      "package/data/mcp-pack.json",
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
    const { pack, provenance, digest } = validateIdentity(packageJson, packBytes, provenanceBytes);
    await mkdir(resolve(repositoryRoot, ".esp-vision/tmp"), { recursive: true });
    temporaryDirectory = await mkdtemp(resolve(repositoryRoot, ".esp-vision/tmp/knowledge-npm-check-"));
    const packPath = resolve(temporaryDirectory, "mcp-pack.json");
    try {
      await writeFile(packPath, packBytes);
      validateSchema(packPath);
    } finally {
      await rm(temporaryDirectory, { recursive: true, force: true });
    }
    process.stdout.write(`Tarball ready: ${packageJson.name}@${packageJson.version}, pack ${pack.meta.version}, ${digest}\n`);
    return;
  }

  packBytes = await readFile(resolve(packageRoot, "data/mcp-pack.json")).catch(() => undefined);
  provenanceBytes = await readFile(resolve(packageRoot, "data/provenance.json")).catch(() => undefined);
  if (!packBytes || !provenanceBytes) {
    throw new Error("A schema-validated production pack is required before npm pack or release.");
  }
  const { pack, digest } = validateIdentity(packageJson, packBytes, provenanceBytes);
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
  validateSchema(resolve(packageRoot, "data/mcp-pack.json"));
  process.stdout.write(`Package ready: ${packageJson.name}@${packageJson.version}, pack ${pack.meta.version}, ${digest}\n`);
}

main().catch(async (error) => {
  process.stderr.write((error instanceof Error ? error.message : "Package check failed.") + "\n");
  process.exitCode = 1;
});
