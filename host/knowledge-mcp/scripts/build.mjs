// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

import { createHash } from "node:crypto";
import { chmod, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { builtinModules } from "node:module";
import { spawnSync } from "node:child_process";
import { build } from "esbuild";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { generateNotices } from "./generate-notices.mjs";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = resolve(packageRoot, "../..");
const packageJson = JSON.parse(await readFile(resolve(packageRoot, "package.json"), "utf-8"));

function parseArgs(args) {
  let release = false;
  let packPath;
  for (let index = 0; index < args.length; index += 1) {
    const argument = args[index];
    if (argument === "--release") {
      release = true;
      continue;
    }
    if (argument === "--pack" || argument.startsWith("--pack=")) {
      if (packPath) throw new Error("Specify --pack only once.");
      if (argument.startsWith("--pack=")) packPath = argument.slice("--pack=".length);
      else packPath = args[++index];
      if (!packPath || packPath.startsWith("--")) throw new Error("--pack requires a path.");
      release = true;
      continue;
    }
    throw new Error("Unknown build option.");
  }
  if (release && !packPath) packPath = process.env.MCP_PACK_PATH;
  if (release && !packPath) throw new Error("A production pack is required; set MCP_PACK_PATH or pass --pack.");
  return { release, packPath: packPath ? resolve(process.cwd(), packPath) : undefined };
}

function runChecked(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: options.cwd ?? packageRoot,
    env: {
      ...process.env,
      TMPDIR: process.env.TMPDIR ?? resolve(repositoryRoot, ".esp-vision/tmp"),
      TMP: process.env.TMP ?? resolve(repositoryRoot, ".esp-vision/tmp"),
      TEMP: process.env.TEMP ?? resolve(repositoryRoot, ".esp-vision/tmp"),
    },
    encoding: "utf-8",
    stdio: "inherit",
  });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${command} failed with exit code ${result.status}.`);
}

function validatePack(packBytes, packPath) {
  const pack = JSON.parse(packBytes.toString("utf-8"));
  const version = pack?.meta?.version;
  const sourceCommit = pack?.meta?.sourceCommit;
  if (typeof version !== "string" || !/^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$/.test(version)) {
    throw new Error("The knowledge pack has an invalid meta.version.");
  }
  if (typeof sourceCommit !== "string" || !/^[a-f0-9]{40}$/.test(sourceCommit)) {
    throw new Error("The knowledge pack meta.sourceCommit must be a full 40-character lowercase Git commit.");
  }
  runChecked("python3", [
    resolve(repositoryRoot, "tools/ci/mcp/validate_schema.py"),
    packPath,
    "--require-micropython",
  ], { cwd: repositoryRoot });
  return { pack, version, sourceCommit };
}

async function main() {
  const { release, packPath } = parseArgs(process.argv.slice(2));
  const distPath = resolve(packageRoot, "dist");
  const dataPath = resolve(packageRoot, "data");
  await mkdir(resolve(repositoryRoot, ".esp-vision/tmp"), { recursive: true });
  await rm(distPath, { recursive: true, force: true });
  await rm(resolve(dataPath, "mcp-pack.json"), { force: true });
  await rm(resolve(dataPath, "provenance.json"), { force: true });

  runChecked(process.execPath, [resolve(packageRoot, "node_modules/typescript/bin/tsc"), "--noEmit", "-p", "tsconfig.json"]);
  await generateNotices();

  const coreModules = new Set(builtinModules.filter((name) => !name.startsWith("node:")));
  const result = await build({
    absWorkingDir: packageRoot,
    entryPoints: {
      "cli": "src/cli.ts",
      "pack": "src/pack.ts",
      "server": "src/http-main.ts",
      "service": "src/service.ts",
      "stats-cli": "src/stats-cli.ts",
      "stdio": "src/stdio.ts",
      "tool-telemetry": "src/tool-telemetry.ts",
      "usage-stats": "src/usage-stats.ts",
    },
    outdir: distPath,
    bundle: true,
    splitting: false,
    format: "esm",
    platform: "node",
    target: "node24",
    external: ["node:*", ...coreModules],
    legalComments: "none",
    sourcemap: false,
    metafile: true,
    logLevel: "info",
  });

  const externalImports = Object.values(result.metafile.outputs)
    .flatMap((output) => output.imports)
    .filter((item) => item.external)
    .map((item) => item.path);
  const invalidExternal = externalImports.filter((item) => !item.startsWith("node:") && !coreModules.has(item));
  if (invalidExternal.length) {
    throw new Error("Unexpected external runtime imports: " + invalidExternal.join(", "));
  }

  for (const entry of Object.keys({
    cli: true, pack: true, server: true, service: true, "stats-cli": true,
    stdio: true, "tool-telemetry": true, "usage-stats": true,
  })) {
    const outputPath = resolve(distPath, `${entry}.js`);
    const output = await readFile(outputPath, "utf-8");
    const shebang = output.startsWith("#!") ? output.slice(0, output.indexOf("\n") + 1) : "";
    const code = shebang ? output.slice(shebang.length) : output;
    await writeFile(outputPath,
      shebang + 'import { createRequire as __createRequire } from "node:module";\n' +
      "const require = __createRequire(import.meta.url);\n" + code,
    );
  }
  await chmod(resolve(distPath, "cli.js"), 0o755);

  if (!release) return;
  const bytes = await readFile(packPath);
  const { pack, version, sourceCommit } = validatePack(bytes, packPath);
  await mkdir(dataPath, { recursive: true });
  await writeFile(resolve(dataPath, "mcp-pack.json"), bytes);
  const provenance = {
    schemaVersion: 1,
    packageName: packageJson.name,
    packageVersion: packageJson.version,
    packVersion: version,
    sourceCommit,
    packSha256: createHash("sha256").update(bytes).digest("hex"),
  };
  await writeFile(resolve(dataPath, "provenance.json"), JSON.stringify(provenance, null, 2) + "\n");
  runChecked(process.execPath, [resolve(packageRoot, "scripts/check-package.mjs")]);
}

try {
  await main();
} catch (error) {
  process.stderr.write((error instanceof Error ? error.message : "Knowledge MCP build failed.") + "\n");
  process.exitCode = 1;
}
