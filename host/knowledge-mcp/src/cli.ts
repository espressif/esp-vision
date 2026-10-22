#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import process from "node:process";
import { Pack } from "./pack.js";
import { attachHttpShutdown, startHttpServer } from "./server.js";
import { startStdioServer } from "./stdio.js";

const packagePackPath = resolve(dirname(fileURLToPath(import.meta.url)), "../data/mcp-pack.json");

export interface CliOptions {
  transport: "stdio" | "http";
  packPath?: string;
  port?: number;
  statsDbPath?: string;
  retentionDays?: number;
  help: boolean;
}

function optionValue(args: string[], index: number, name: string): [string, number] {
  const argument = args[index];
  const prefix = name + "=";
  if (argument.startsWith(prefix)) {
    const value = argument.slice(prefix.length);
    if (!value) throw new Error(`Missing value for ${name}.`);
    return [value, index];
  }
  const value = args[index + 1];
  if (typeof value !== "string" || value.startsWith("--")) throw new Error(`Missing value for ${name}.`);
  return [value, index + 1];
}

function parseInteger(value: string, name: string, min: number, max: number): number {
  if (!/^\d+$/.test(value)) throw new Error(`${name} must be an integer from ${min} through ${max}.`);
  const number = Number(value);
  if (!Number.isSafeInteger(number) || number < min || number > max) {
    throw new Error(`${name} must be an integer from ${min} through ${max}.`);
  }
  return number;
}

export function parseCliArgs(args: string[]): CliOptions {
  let transport: "stdio" | "http" = "stdio";
  let packPath: string | undefined;
  let port: number | undefined;
  let statsDbPath: string | undefined;
  let retentionDays: number | undefined;
  let help = false;
  const seen = new Set<string>();

  for (let index = 0; index < args.length; index += 1) {
    const argument = args[index];
    if (argument === "--help" || argument === "-h") {
      help = true;
      continue;
    }
    if (argument === "--transport" || argument.startsWith("--transport=")) {
      if (seen.has("--transport")) throw new Error("Specify --transport only once.");
      seen.add("--transport");
      const [value, next] = optionValue(args, index, "--transport");
      if (value !== "stdio" && value !== "http") throw new Error("--transport must be stdio or http.");
      transport = value;
      index = next;
      continue;
    }
    if (argument === "--pack" || argument.startsWith("--pack=")) {
      if (seen.has("--pack")) throw new Error("Specify --pack only once.");
      seen.add("--pack");
      [packPath, index] = optionValue(args, index, "--pack");
      continue;
    }
    if (argument === "--port" || argument.startsWith("--port=")) {
      if (seen.has("--port")) throw new Error("Specify --port only once.");
      seen.add("--port");
      const [value, next] = optionValue(args, index, "--port");
      port = parseInteger(value, "--port", 1, 65535);
      index = next;
      continue;
    }
    if (argument === "--stats-db" || argument.startsWith("--stats-db=")) {
      if (seen.has("--stats-db")) throw new Error("Specify --stats-db only once.");
      seen.add("--stats-db");
      [statsDbPath, index] = optionValue(args, index, "--stats-db");
      continue;
    }
    if (argument === "--stats-retention-days" || argument.startsWith("--stats-retention-days=")) {
      if (seen.has("--stats-retention-days")) throw new Error("Specify --stats-retention-days only once.");
      seen.add("--stats-retention-days");
      const [value, next] = optionValue(args, index, "--stats-retention-days");
      retentionDays = parseInteger(value, "--stats-retention-days", 1, 3650);
      index = next;
      continue;
    }
    throw new Error("Unknown command-line option.");
  }

  if (transport === "stdio" && (port !== undefined || statsDbPath !== undefined || retentionDays !== undefined)) {
    throw new Error("--port and statistics options require --transport http.");
  }
  return { transport, packPath, port, statsDbPath, retentionDays, help };
}

function helpText(): string {
  return [
    "Usage: esp-vision-knowledge-mcp [--transport stdio|http] [options]",
    "",
    "Transport defaults to stdio. Options:",
    "  --pack PATH                  Knowledge pack (default: MCP_PACK_PATH or packaged data)",
    "  --transport stdio|http       stdio by default; HTTP keeps / and /mcp endpoints",
    "  --port PORT                  HTTP port (default: PORT or 8080)",
    "  --stats-db PATH              HTTP aggregate statistics database",
    "  --stats-retention-days DAYS  HTTP retention, 1 through 3650 (default: 90)",
    "  --help                       Show this help",
    "",
    "Stdio writes MCP protocol messages to stdout and diagnostics to stderr. It does not",
    "read MCP_STATS_* settings or create a statistics database.",
  ].join("\n");
}

export async function main(args = process.argv.slice(2), env: NodeJS.ProcessEnv = process.env): Promise<number> {
  let options: CliOptions;
  try {
    options = parseCliArgs(args);
  } catch (error) {
    process.stderr.write((error instanceof Error ? error.message : "Invalid command-line options.") + "\n");
    process.stderr.write(helpText() + "\n");
    return 2;
  }

  if (options.help) {
    process.stderr.write(helpText() + "\n");
    return 0;
  }

  // Stdio defaults to the immutable pack shipped in the npm archive. There is
  // no fixture fallback, and explicit paths remain useful for local testing.
  const packPath = options.packPath ?? env.MCP_PACK_PATH ?? packagePackPath;
  let pack: Pack;
  try {
    pack = Pack.load(packPath);
  } catch {
    process.stderr.write("Knowledge pack could not be loaded from the configured path.\n");
    return 1;
  }

  if (options.transport === "stdio") {
    try {
      await startStdioServer({ pack });
      return 0;
    } catch {
      process.stderr.write("Knowledge MCP stdio server failed.\n");
      return 1;
    }
  }

  try {
    const running = await startHttpServer({
      pack,
      ...(options.port === undefined ? {} : { port: options.port }),
      ...(options.statsDbPath === undefined ? {} : { statsDbPath: options.statsDbPath }),
      ...(options.retentionDays === undefined ? {} : { retentionDays: options.retentionDays }),
      npmArtifactSha256: env.MCP_NPM_ARTIFACT_SHA256,
    });
    attachHttpShutdown(running);
    return 0;
  } catch {
    process.stderr.write("Knowledge MCP HTTP server failed to start.\n");
    return 1;
  }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  void main().then((code) => { if (code !== 0) process.exitCode = code; });
}
