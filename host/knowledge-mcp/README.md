# ESP-VISION Knowledge MCP

The public `@esp-vision/knowledge-mcp` npm package provides the ESP-VISION versioned API knowledge catalog over MCP stdio or Streamable HTTP. The `esp-vision-knowledge-mcp` command defaults to stdio for local MCP clients. Knowledge 0.2 requires an exact `firmwareVersion` for content tools and uses `esp-vision://firmware/{firmwareVersion}/module/{name}` resources. It preserves the `/` and `/mcp` request addresses, and `/health` and `/mcp/health` health addresses. The official online endpoint remains `https://mcp.vision.espressif.com/mcp`; the website plugin continues to use HTTP.

The npm archive bundles its JavaScript runtime dependencies and ships all selected immutable firmware snapshots in `data/mcp-catalog.json` with `data/provenance.json`. It requires Node.js 24 and has no runtime npm dependencies. The package includes the Apache-2.0 license and bundled dependency licenses in `NOTICE` and `THIRD_PARTY_NOTICES.md`.

## Stdio MCP

Install the package or use `npx` from an MCP client configuration:

```sh
npm install --global @esp-vision/knowledge-mcp
esp-vision-knowledge-mcp
```

The stdio transport is the default. Its stdout contains MCP protocol messages only; diagnostics go to stderr. It does not read `MCP_STATS_*`, open a SQLite database, or make outbound requests. The npm package contains the immutable catalog selected at release time. `MCP_CATALOG_PATH=/path/to/mcp-catalog.json` or `--catalog /path/to/mcp-catalog.json` selects another catalog explicitly. `--pack` / `MCP_PACK_PATH` remains available for an explicit single-firmware development pack; it does not remove the query version requirement.

Example MCP client configuration:

```json
{
  "mcpServers": {
    "esp-vision": {
      "command": "esp-vision-knowledge-mcp",
      "args": []
    }
  }
}
```

## Streamable HTTP

The `--transport http` option starts the same stateless HTTP service used by the online endpoint:

```sh
esp-vision-knowledge-mcp --transport http --port 8080
```

HTTP reads `PORT` (default `8080`) and `MCP_CATALOG_PATH` (default the catalog in the installed npm archive). Anonymous aggregate usage statistics remain opt-in through `MCP_STATS_DB_PATH`; set `MCP_STATS_RETENTION_DAYS` to select retention from 1 through 3650 days (default 90). The `--stats-db` and `--stats-retention-days` command-line options configure the same HTTP-only settings. Statistics are never exposed by an HTTP endpoint or MCP tool.

The internal read-only report command reads the configured SQLite database without changing it:

```sh
MCP_STATS_DB_PATH=/path/to/usage.sqlite node dist/stats-cli.js --days 7 --format table
```

The command can emit JSON with `--format json`. It requires `MCP_STATS_DB_PATH`.

## Public service factory

The package exports `createKnowledgeServer({ catalog, usageStats? })`, `KnowledgeCatalog` and `Pack` from `@esp-vision/knowledge-mcp` (or `@esp-vision/knowledge-mcp/service`). Call `KnowledgeCatalog.load(path)` to load an explicitly selected catalog. The `pack` factory option accepts a `Pack.load(path)` singleton for development. Importing the factory does not load a pack, read environment variables, open a database, create a transport, make a network request, or listen on a port. The caller connects the returned MCP server to its chosen transport.

## Build, tests, and npm release

Use Node.js 24 and npm. Install the locked development toolchain and run the package checks:

```sh
npm ci
npm run build
npm test
```

The tests select `test/fixtures/mcp-pack.json` explicitly. Ordinary builds produce bundled code only and never select that fixture as production data. `npm pack` and release checks fail unless a validated production pack and matching provenance were generated.

To build a release archive, assemble the exact sources pinned in `snapshots.json`, then set the runtime commit separately (commands below run from the repository root):

```sh
python3 tools/ci/mcp/catalog.py assemble --lock host/knowledge-mcp/snapshots.json --output "$ESP_VISION_WORK_DIR/mcp-catalog.json"
MCP_CATALOG_PATH="$ESP_VISION_WORK_DIR/mcp-catalog.json" MCP_RUNTIME_SOURCE_COMMIT="$(git rev-parse HEAD)" npm --prefix host/knowledge-mcp run build:release
npm --prefix host/knowledge-mcp run check:package
```

The build verifies every snapshot's exact bytes, full commit identities and pack schema. `data/provenance.json` schema 2 binds the Knowledge package version, runtime source commit and catalog SHA-256. The compatibility export `data/mcp-pack.json` is one catalog entry for older offline tooling; the service never selects it as a query default. The runtime makes no outgoing requests.

## Exact firmware queries and history

Use `list_firmware_versions` to discover supported firmware, retained revisions and the catalog-maintained `latestFirmwareVersion`. The latest pointer must identify a firmware with an active snapshot; it preserves the original TAG and is never an implicit query default. Catalog, release descriptor, health and `server_info` expose the same value. Read the actual board's `identity.firmwareVersion` through Device CLI, then pass it unchanged to all eight content tools: `get_api`, `get_capabilities`, `search_docs`, `list_modules`, `list_symbols`, `get_example`, `list_examples` and `get_concept`. For example, call `get_api` with `{ "query": "sensor.snapshot", "firmwareVersion": "2026.10.21" }`. Every successful content result returns `{snapshot,result}` and includes `_meta.knowledgeSnapshot` with firmware version, snapshot revision, firmware commit, content commit and pack SHA-256. Optional `snapshotVersion` selects an exact retained revision for that firmware.

Omitted versions fail argument validation; unknown versions or revisions return `KNOWLEDGE_VERSION_UNAVAILABLE` and available firmware versions. The server never silently substitutes latest, nearest-release, prefix or development-suffix matches. The Plugin first offers an official firmware update for missing/unavailable versions; only after the user explicitly declines does it select the latest available official firmware snapshot from `list_firmware_versions`, pin both version arguments and disclose the compatibility uncertainty. When no device is connected, explicitly choose a target firmware version. Refresh the selection after switching devices or flashing. `server_info` and HTTP health identify the runtime package, runtime commit, catalog hash and snapshot inventory; there is no global current firmware version.

Resources use `esp-vision://firmware/{firmwareVersion}/module/{name}` or `esp-vision://firmware/{firmwareVersion}/snapshot/{snapshotVersion}/module/{name}` and return `{snapshot,result}`. The former unversioned resource is rejected. These required arguments, resource URIs and identity fields are breaking changes from 0.1.x, so this pre-1.0 release advances to 0.2.0 with release type `minor`; clients must adopt the new contracts before deployment.

Firmware tags archive baseline snapshots after firmware qualification/publication. Knowledge npm releases run independently on protected master after a manifest version change; catalog changes also require a new package version. A publication check compares the previous npm archive and rejects deletion or modification of any historical snapshot. Initial history restores 2026.10.21, 2026.10.23 and 2026.11.06 from their integrity-pinned original npm archives. See [release and deployment contracts](../../docs/mcp-release.md) for the complete procedure.
