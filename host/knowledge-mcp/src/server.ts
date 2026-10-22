// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//
// Streamable HTTP entry point retained for the website service and legacy
// `node dist/server.js` callers.

import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import express, { type ErrorRequestHandler, type Request, type Response } from "express";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { Pack } from "./pack.js";
import { createKnowledgeServer } from "./service.js";
import { UsageStats } from "./usage-stats.js";

const moduleDirectory = dirname(fileURLToPath(import.meta.url));
const defaultPackPath = resolve(moduleDirectory, "../data/mcp-pack.json");

export interface HttpKnowledgeOptions {
  pack: Pack;
  usageStats?: UsageStats;
  npmArtifactSha256?: string;
}

export interface StartHttpServerOptions {
  pack?: Pack;
  packPath?: string;
  port?: number;
  statsDbPath?: string;
  retentionDays?: number;
  npmArtifactSha256?: string;
}

export function createHttpApplication({ pack, usageStats, npmArtifactSha256 }: HttpKnowledgeOptions) {
  const app = express();
  app.use(express.json({ limit: "4mb" }));
  const mcpEndpoints = ["/", "/mcp"] as const;
  const healthEndpoints = ["/health", "/mcp/health"] as const;

  // Permissive CORS for a public read-only endpoint (browser-based MCP clients).
  app.use((req, res, next) => {
    res.header("Access-Control-Allow-Origin", "*");
    res.header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    res.header("Access-Control-Allow-Headers", "Content-Type, mcp-session-id, mcp-protocol-version");
    res.header("Access-Control-Expose-Headers", "mcp-session-id");
    if (req.method === "OPTIONS") return res.sendStatus(204);
    next();
  });

  for (const healthPath of healthEndpoints) {
    app.get(healthPath, (_req, res) => res.json(pack.serverInfo({ npmArtifactSha256 })));
  }

  for (const endpoint of mcpEndpoints) {
    // Stateless Streamable HTTP: a fresh server + transport per request.
    app.all(endpoint, async (req: Request, res: Response) => {
      const server = createKnowledgeServer({ pack, usageStats, npmArtifactSha256 });
      const transport = new StreamableHTTPServerTransport({ sessionIdGenerator: undefined });
      let closed = false;
      const closeRequest = () => {
        if (closed) return;
        closed = true;
        void transport.close();
        void server.close();
      };
      res.on("close", closeRequest);

      try {
        await server.connect(transport);
        await transport.handleRequest(req, res, req.body);
      } catch {
        // SDK exceptions may include submitted arguments. Keep request errors
        // content-free; client-facing behavior remains the generic 500 response.
        console.error("MCP request failed (details omitted)");
        closeRequest();
        if (!res.headersSent) {
          res.status(500).json({ jsonrpc: "2.0", error: { code: -32603, message: "Internal error" }, id: null });
        }
      }
    });
  }

  const contentFreeHttpErrorHandler: ErrorRequestHandler = (error, _req, res, _next) => {
    const status = typeof error === "object" && error !== null && "status" in error
      ? (error as { status?: unknown }).status
      : undefined;
    const httpStatus = typeof status === "number" && Number.isInteger(status) && status >= 400 && status < 500
      ? status
      : 500;

    // Body-parser errors can contain submitted request data. Preserve the HTTP
    // status while keeping both the log event and response content-free.
    console.error("MCP HTTP request failed (details omitted)");
    if (res.headersSent) return res.end();
    const message = httpStatus === 413
      ? "Payload too large"
      : httpStatus < 500 ? "Invalid request" : "Internal error";
    res.status(httpStatus).json({ error: message });
  };

  app.use(contentFreeHttpErrorHandler);
  return app;
}

function configureUsageStats(dbPath: string | undefined, retentionDays: number | undefined): UsageStats | undefined {
  if (!dbPath?.trim()) return undefined;
  try {
    return new UsageStats({ dbPath, retentionDays });
  } catch {
    // A local statistics-store problem must not prevent knowledge queries.
    console.warn("MCP usage statistics disabled: store initialization failed");
    return undefined;
  }
}

function readRetentionDays(raw: string | undefined): number | undefined {
  if (raw === undefined) return undefined;
  if (!/^[1-9]\d*$/.test(raw)) return Number.NaN;
  const value = Number(raw);
  return Number.isSafeInteger(value) && value >= 1 && value <= 3650 ? value : Number.NaN;
}

function readPort(raw: string | undefined): number {
  if (raw === undefined) return 8080;
  if (!/^\d+$/.test(raw)) throw new Error("Invalid HTTP port.");
  const port = Number(raw);
  if (!Number.isSafeInteger(port) || port < 1 || port > 65535) throw new Error("Invalid HTTP port.");
  return port;
}

export async function startHttpServer(options: StartHttpServerOptions = {}) {
  const packPath = options.packPath ?? process.env.MCP_PACK_PATH ?? defaultPackPath;
  let pack = options.pack;
  if (!pack) {
    try {
      pack = Pack.load(packPath);
    } catch {
      throw new Error("Knowledge pack could not be loaded from the configured path.");
    }
  }

  const port = options.port ?? readPort(process.env.PORT);
  const dbPath = options.statsDbPath ?? process.env.MCP_STATS_DB_PATH;
  const rawRetention = options.retentionDays === undefined
    ? process.env.MCP_STATS_RETENTION_DAYS
    : String(options.retentionDays);
  const parsedRetention = readRetentionDays(rawRetention);
  let usageStats: UsageStats | undefined;
  if (dbPath?.trim() && parsedRetention !== undefined && Number.isNaN(parsedRetention)) {
    console.warn("MCP usage statistics disabled: invalid retention configuration");
  } else {
    usageStats = configureUsageStats(dbPath, parsedRetention);
  }

  const npmArtifactSha256 = options.npmArtifactSha256 ?? process.env.MCP_NPM_ARTIFACT_SHA256;
  const app = createHttpApplication({ pack, usageStats, npmArtifactSha256 });
  const httpServer = app.listen(port, () => {
    const info = pack!.serverInfo() as { version?: string; counts?: unknown };
    console.log(`esp-vision MCP server on :${port} (pack ${info.version}, ` +
      `${JSON.stringify(info.counts ?? {})})`);
  });

  let closing: Promise<void> | undefined;
  const close = () => {
    if (closing) return closing;
    closing = new Promise<void>((resolveClose) => {
      httpServer.close(() => {
        usageStats?.close();
        resolveClose();
      });
      httpServer.closeIdleConnections?.();
    });
    return closing;
  };

  return { app, server: httpServer, usageStats, close };
}

export function attachHttpShutdown(running: { close: () => Promise<void> }): void {
  let stopping = false;
  const stop = (signal: "SIGINT" | "SIGTERM") => {
    if (stopping) return;
    stopping = true;
    process.exitCode = signal === "SIGINT" ? 130 : 143;
    void running.close();
  };
  process.once("SIGINT", () => stop("SIGINT"));
  process.once("SIGTERM", () => stop("SIGTERM"));
}
