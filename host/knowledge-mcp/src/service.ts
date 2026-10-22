// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//
// Public, transport-independent ESP-VISION Knowledge MCP service factory.

import { McpServer, ResourceTemplate } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import type { Pack } from "./pack.js";
import { measureTool } from "./tool-telemetry.js";
import type { UsageStats } from "./usage-stats.js";

export { Pack } from "./pack.js";

const json = (value: unknown) => ({
  content: [{ type: "text" as const, text: JSON.stringify(value, null, 2) }],
});

export interface CreateKnowledgeServerOptions {
  pack: Pack;
  usageStats?: UsageStats;
  npmArtifactSha256?: string;
}

/**
 * Create the Knowledge MCP server without loading files, opening databases,
 * reading environment variables, creating a transport, or listening on a port.
 */
export function createKnowledgeServer({ pack, usageStats, npmArtifactSha256 }: CreateKnowledgeServerOptions): McpServer {
  const packVersion = pack.version();
  const server = new McpServer({ name: "esp-vision-mcp", version: packVersion });

  function measuredTool<TArgs extends unknown[], TResult>(
    tool: string,
    handler: (...args: TArgs) => Promise<TResult>,
  ): (...args: TArgs) => Promise<TResult> {
    if (usageStats) return measureTool(usageStats, tool, packVersion, handler);
    return async (...args: TArgs) => handler(...args);
  }

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
    }) => json(pack.search(query, target, kind, source, limit ?? 10)),
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
    measuredTool("server_info", async () => json(pack.serverInfo({ npmArtifactSha256 }))),
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
