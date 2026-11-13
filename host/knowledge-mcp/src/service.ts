// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//
// Public, transport-independent ESP-VISION Knowledge MCP service factory.

import { McpServer, ResourceTemplate } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { KnowledgeCatalog, KnowledgeVersionError } from "./catalog.js";
import type { Pack } from "./pack.js";
import { measureTool } from "./tool-telemetry.js";
import type { UsageStats } from "./usage-stats.js";

export { KnowledgeCatalog } from "./catalog.js";
export { Pack } from "./pack.js";

const json = (value: unknown) => ({
  content: [{ type: "text" as const, text: JSON.stringify(value, null, 2) }],
});

export interface CreateKnowledgeServerOptions {
  pack?: Pack;
  catalog?: KnowledgeCatalog;
  usageStats?: UsageStats;
  npmArtifactSha256?: string;
}

/**
 * Create the Knowledge MCP server without loading files, opening databases,
 * reading environment variables, creating a transport, or listening on a port.
 */
export function createKnowledgeServer({ pack, catalog: configuredCatalog, usageStats, npmArtifactSha256 }: CreateKnowledgeServerOptions): McpServer {
  const catalog = configuredCatalog ?? (pack ? KnowledgeCatalog.fromPack(pack) : undefined);
  if (!catalog) throw new Error("A knowledge catalog is required");
  const server = new McpServer({ name: "esp-vision-mcp", version: catalog.serverInfo().packageVersion ?? "0.0.0" });
  const versionSchema = {
    firmwareVersion: z.string().describe("Exact device firmware version; required, never defaults to latest"),
    snapshotVersion: z.string().optional().describe("Optional immutable revision for this firmware"),
  };
  type Selection = { firmwareVersion?: string; snapshotVersion?: string };
  function measuredTool<TArgs extends Selection>(tool: string, handler: (args: TArgs, pack: Pack) => ReturnType<typeof json>) {
    return async (args: TArgs) => {
      let selected;
      try { selected = catalog!.select(args.firmwareVersion, args.snapshotVersion); }
      catch (error) {
        if (!(error instanceof KnowledgeVersionError)) throw error;
        return { ...json({ code: error.code, message: error.message, availableVersions: error.availableVersions }), isError: true };
      }
      const run = async () => {
        const result = JSON.parse(handler(args, selected.pack).content[0].text);
        // Keep provenance visible to assistants whose hosts omit MCP _meta.
        return { ...json({ snapshot: selected.identity, result }), _meta: { knowledgeSnapshot: selected.identity } };
      };
      return usageStats ? measureTool(usageStats, tool, selected.identity.firmwareVersion, run)() : run();
    };
  }

  server.tool(
    "get_api",
    "Authoritative API for an ESP-VISION or MicroPython symbol/module. query: " +
      'symbol id ("sensor.snapshot"), bare name ("snapshot", "Pin.value"), or a ' +
      'module name ("sensor", "machine"). target: optional chip filter ("esp32p4"/' +
      '"p4", "s3", "s31") — adds `available` per match. source: "esp-vision"|"micropython".',
    { ...versionSchema, query: z.string(), target: z.string().optional(), source: z.string().optional() },
    measuredTool("get_api", ({ query, target, source }: {
      query: string; target?: string; source?: string;
    } & Selection, pack) => json(pack.getApi(query, target, source))),
  );

  server.tool(
    "get_capabilities",
    "Capabilities of a chip target or board: available modules, boards, feature " +
      "gates (e.g. barcode). Call before generating code so you only use APIs the " +
      'hardware supports. name: a target ("esp32p4") or board ("ESP32_S3_EYE").',
    { ...versionSchema, name: z.string() },
    measuredTool("get_capabilities", ({ name }: { name: string } & Selection, pack) => json(pack.getCapabilities(name))),
  );

  server.tool(
    "search_docs",
    "Full-text search across symbols, concept/how-to docs and examples. kind: " +
      '"symbol"|"doc"|"example". target/source: optional filters.',
    {
      ...versionSchema, query: z.string(), target: z.string().optional(), kind: z.string().optional(),
      source: z.string().optional(), limit: z.number().int().positive().max(50).optional(),
    },
    measuredTool("search_docs", ({ query, target, kind, source, limit }: {
      query: string; target?: string; kind?: string; source?: string; limit?: number;
    } & Selection, pack) => json(pack.search(query, target, kind, source, limit ?? 10)),
    ),
  );

  server.tool(
    "list_modules",
    "List available modules with their source, per-chip targets and symbol count.",
    { ...versionSchema, source: z.string().optional(), target: z.string().optional() },
    measuredTool("list_modules", ({ source, target }: { source?: string; target?: string } & Selection, pack) =>
      json(pack.listModules(source, target))),
  );

  server.tool(
    "list_symbols",
    "List all symbols of a module (optionally filtered to a chip target).",
    { ...versionSchema, module: z.string(), target: z.string().optional() },
    measuredTool("list_symbols", ({ module, target }: { module: string; target?: string } & Selection, pack) =>
      json(pack.listSymbols(module, target))),
  );

  server.tool(
    "get_example",
    "Return a runnable example program by path/title/keyword.",
    { ...versionSchema, query: z.string() },
    measuredTool("get_example", ({ query }: { query: string } & Selection, pack) => json(pack.getExample(query))),
  );

  server.tool(
    "list_examples",
    "List runnable examples, optionally those that use a given module.",
    { ...versionSchema, module: z.string().optional() },
    measuredTool("list_examples", ({ module }: { module?: string } & Selection, pack) =>
      json(pack.listExamples(module))),
  );

  server.tool(
    "get_concept",
    "Return a concept / how-to page (camera-pipeline, ai-inference, add-model, ...).",
    { ...versionSchema, topic: z.string() },
    measuredTool("get_concept", ({ topic }: { topic: string } & Selection, pack) => json(pack.getConcept(topic))),
  );

  server.tool("server_info", "Runtime identity, catalog digest and immutable firmware snapshots.", {},
    async () => usageStats
      ? measureTool(usageStats, "server_info", "catalog", async () => json(catalog.serverInfo(npmArtifactSha256)))()
      : json(catalog.serverInfo(npmArtifactSha256)));
  server.tool("list_firmware_versions", "List available exact firmware versions and all retained snapshot revisions.", {},
    async () => json({ firmwareVersions: catalog.versions(), latestFirmwareVersion: catalog.latestFirmwareVersion, snapshots: catalog.list() }));

  server.resource("module", new ResourceTemplate("esp-vision://firmware/{firmwareVersion}/module/{name}", { list: undefined }),
    async (uri, { firmwareVersion, name }) => {
      const { pack, identity } = catalog.select(String(firmwareVersion));
      return { contents: [{ uri: uri.href, mimeType: "application/json",
        text: JSON.stringify({ snapshot: identity, result: pack.listSymbols(String(name)) }, null, 2) }] };
    });
  server.resource("snapshot-module", new ResourceTemplate("esp-vision://firmware/{firmwareVersion}/snapshot/{snapshotVersion}/module/{name}", { list: undefined }),
    async (uri, { firmwareVersion, snapshotVersion, name }) => {
      const { pack, identity } = catalog.select(String(firmwareVersion), String(snapshotVersion));
      return { contents: [{ uri: uri.href, mimeType: "application/json",
        text: JSON.stringify({ snapshot: identity, result: pack.listSymbols(String(name)) }, null, 2) }] };
    });
  return server;
}
