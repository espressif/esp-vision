// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

import process from "node:process";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import type { Pack } from "./pack.js";
import { createKnowledgeServer } from "./service.js";

export interface StartStdioServerOptions {
  pack: Pack;
}

/** Run a protocol-only stdio session with no statistics store or env lookup. */
export async function startStdioServer({ pack }: StartStdioServerOptions): Promise<void> {
  const server = createKnowledgeServer({ pack });
  const transport = new StdioServerTransport();
  server.server.onerror = () => {
    process.stderr.write("Knowledge MCP stdio protocol error.\n");
  };

  let stopping = false;
  let resolveStopped: (() => void) | undefined;
  const stopped = new Promise<void>((resolve) => { resolveStopped = resolve; });
  const stop = (signal?: "SIGINT" | "SIGTERM") => {
    if (stopping) return;
    stopping = true;
    if (signal) process.exitCode = signal === "SIGINT" ? 130 : 143;
    process.stdin.off("end", onEnd);
    process.stdin.off("close", onClose);
    process.off("SIGINT", onSigint);
    process.off("SIGTERM", onSigterm);
    void server.close().catch(() => {
      process.stderr.write("Knowledge MCP stdio shutdown error.\n");
    }).finally(() => resolveStopped?.());
  };
  const onEnd = () => stop();
  const onClose = () => stop();
  const onSigint = () => stop("SIGINT");
  const onSigterm = () => stop("SIGTERM");

  process.stdin.once("end", onEnd);
  process.stdin.once("close", onClose);
  process.once("SIGINT", onSigint);
  process.once("SIGTERM", onSigterm);

  try {
    await server.connect(transport);
    await stopped;
  } catch (error) {
    stop();
    throw error;
  }
}
