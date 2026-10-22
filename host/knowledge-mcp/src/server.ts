// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//
// ESP-VISION documentation MCP server (TypeScript). Serves the API knowledge
// pack (produced by the main repo's docs/gen_mcp.py) so AI coding agents query
// authoritative signatures, per-chip availability, examples and concepts without
// reading docs.
//
// Transport: Streamable HTTP, stateless (a fresh server+transport per request) —
// used by the website deployment for the public, read-only knowledge endpoint.

import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import express, { type ErrorRequestHandler, type Request, type Response } from "express";
import { McpServer, ResourceTemplate } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StreamableHTTPServerTransport } from "@modelcontextprotocol/sdk/server/streamableHttp.js";
import { z } from "zod";
import { Pack } from "./pack.js";
import { measureTool } from "./tool-telemetry.js";
import { UsageStats } from "./usage-stats.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const PACK_PATH = process.env.MCP_PACK_PATH ?? resolve(__dirname, "../data/mcp-pack.json");
const PORT = Number(process.env.PORT ?? 8080);

const pack = Pack.load(PACK_PATH);
const packVersion = pack.version();
const json = (v: unknown) => ({ content: [{ type: "text" as const, text: JSON.stringify(v, null, 2) }] });

function configureUsageStats(): UsageStats | undefined {
  const dbPath = process.env.MCP_STATS_DB_PATH?.trim();
  if (!dbPath) return undefined;

  const rawRetention = process.env.MCP_STATS_RETENTION_DAYS;
  let retentionDays = 90;
  if (rawRetention !== undefined) {
    if (!/^[1-9]\d*$/.test(rawRetention)) {
      console.warn("MCP usage statistics disabled: invalid retention configuration");
      return undefined;
    }
    retentionDays = Number(rawRetention);
    if (!Number.isSafeInteger(retentionDays) || retentionDays < 1 || retentionDays > 3650) {
      console.warn("MCP usage statistics disabled: invalid retention configuration");
      return undefined;
    }
  }

  try {
    return new UsageStats({ dbPath, retentionDays });
  } catch {
    // A local statistics-store problem must not prevent knowledge queries.
    console.warn("MCP usage statistics disabled: store initialization failed");
    return undefined;
  }
}

const usageStats = configureUsageStats();

function measuredTool<TArgs extends unknown[], TResult>(
  tool: string,
  handler: (...args: TArgs) => Promise<TResult>,
): (...args: TArgs) => Promise<TResult> {
  if (usageStats) return measureTool(usageStats, tool, packVersion, handler);
  return async (...args: TArgs) => handler(...args);
}

function buildServer(): McpServer {
  const server = new McpServer({ name: "esp-vision-mcp", version: pack.version() });

  server.tool(
    "get_api",
    "Authoritative API for an ESP-VISION or MicroPython symbol/module. query: " +
      'symbol id ("sensor.snapshot"), bare name ("snapshot", "Pin.value"), or a ' +
      'module name ("sensor", "machine"). target: optional chip filter ("esp32p4"/' +
      '"p4", "s3", "s31") — adds `available` per match. source: "esp-vision"|"micropython".',
    { query: z.string(), target: z.string().optional(), source: z.string().optional() },
    measuredTool("get_api", async ({ query, target, source }: {
      query: string; target?: string; source?: string;
    }) => json(pack.getApi(query, target, source))),
  );

  server.tool(
    "get_capabilities",
    "Capabilities of a chip target or board: available modules, boards, feature " +
      "gates (e.g. barcode). Call before generating code so you only use APIs the " +
      'hardware supports. name: a target ("esp32p4") or board ("ESP32_S3_EYE").',
    { name: z.string() },
    measuredTool("get_capabilities", async ({ name }: { name: string }) => json(pack.getCapabilities(name))),
  );

  server.tool(
    "search_docs",
    "Full-text search across symbols, concept/how-to docs and examples. kind: " +
      '"symbol"|"doc"|"example". target/source: optional filters.',
    {
      query: z.string(), target: z.string().optional(), kind: z.string().optional(),
      source: z.string().optional(), limit: z.number().int().positive().max(50).optional(),
    },
    measuredTool("search_docs", async ({ query, target, kind, source, limit }: {
      query: string; target?: string; kind?: string; source?: string; limit?: number;
    }) =>
      json(pack.search(query, target, kind, source, limit ?? 10)),
    ),
  );

  server.tool(
    "list_modules",
    "List available modules with their source, per-chip targets and symbol count.",
    { source: z.string().optional(), target: z.string().optional() },
    measuredTool("list_modules", async ({ source, target }: { source?: string; target?: string }) =>
      json(pack.listModules(source, target))),
  );

  server.tool(
    "list_symbols",
    "List all symbols of a module (optionally filtered to a chip target).",
    { module: z.string(), target: z.string().optional() },
    measuredTool("list_symbols", async ({ module, target }: { module: string; target?: string }) =>
      json(pack.listSymbols(module, target))),
  );

  server.tool(
    "get_example",
    "Return a runnable example program by path/title/keyword.",
    { query: z.string() },
    measuredTool("get_example", async ({ query }: { query: string }) => json(pack.getExample(query))),
  );

  server.tool(
    "list_examples",
    "List runnable examples, optionally those that use a given module.",
    { module: z.string().optional() },
    measuredTool("list_examples", async ({ module }: { module?: string }) =>
      json(pack.listExamples(module))),
  );

  server.tool(
    "get_concept",
    "Return a concept / how-to page (camera-pipeline, ai-inference, add-model, ...).",
    { topic: z.string() },
    measuredTool("get_concept", async ({ topic }: { topic: string }) => json(pack.getConcept(topic))),
  );

  server.tool(
    "server_info",
    "Pack version, source commit, MicroPython version and content counts.",
    {},
    measuredTool("server_info", async () => json(pack.serverInfo())),
  );

  server.resource(
    "module",
    new ResourceTemplate("esp-vision://module/{name}", { list: undefined }),
    async (uri, { name }) => ({
      contents: [{
        uri: uri.href,
        mimeType: "application/json",
        text: JSON.stringify(pack.listSymbols(String(name)), null, 2),
      }],
    }),
  );

  return server;
}

const app = express();
app.use(express.json({ limit: "4mb" }));
const MCP_ENDPOINTS = ["/", "/mcp"] as const;
const HEALTH_ENDPOINTS = ["/health", "/mcp/health"] as const;

// Permissive CORS for a public read-only endpoint (browser-based MCP clients).
app.use((req, res, next) => {
  res.header("Access-Control-Allow-Origin", "*");
  res.header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
  res.header("Access-Control-Allow-Headers", "Content-Type, mcp-session-id, mcp-protocol-version");
  res.header("Access-Control-Expose-Headers", "mcp-session-id");
  if (req.method === "OPTIONS") return res.sendStatus(204);
  next();
});

for (const healthPath of HEALTH_ENDPOINTS) {
  app.get(healthPath, (_req, res) => res.json(pack.serverInfo()));
}

for (const endpoint of MCP_ENDPOINTS) {
  // Stateless Streamable HTTP: a fresh server + transport per request.
  app.all(endpoint, async (req: Request, res: Response) => {
    try {
      const server = buildServer();
      const transport = new StreamableHTTPServerTransport({ sessionIdGenerator: undefined });
      res.on("close", () => { transport.close(); server.close(); });
      await server.connect(transport);
      await transport.handleRequest(req, res, req.body);
    } catch (err) {
      // SDK exceptions may include submitted arguments. Keep request errors
      // content-free; client-facing behavior remains the generic 500 response.
      console.error("MCP request failed (details omitted)");
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

app.listen(PORT, () => {
  const info = pack.serverInfo() as any;
  console.log(`esp-vision MCP server on :${PORT} (pack ${info.version}, ` +
    `${JSON.stringify(info.counts)})`);
});
