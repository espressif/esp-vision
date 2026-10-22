// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
import { attachHttpShutdown, startHttpServer } from "./server.js";

async function main(): Promise<void> {
  const running = await startHttpServer();
  attachHttpShutdown(running);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  void main().catch(() => {
    console.error("Knowledge MCP HTTP server failed to start.");
    process.exitCode = 1;
  });
}
