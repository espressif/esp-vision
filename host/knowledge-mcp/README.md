# ESP-VISION Knowledge MCP

The public `@esp-vision/knowledge-mcp` npm package provides the ESP-VISION API knowledge pack over MCP stdio or Streamable HTTP. The `esp-vision-knowledge-mcp` command defaults to stdio for local MCP clients. It preserves the existing read-only HTTP service, its tools and `esp-vision://module/{name}` resource, the `/` and `/mcp` request addresses, and `/health` and `/mcp/health` health addresses. The official online endpoint remains `https://mcp.vision.espressif.com/mcp`; the website plugin continues to use HTTP.

The npm archive bundles its JavaScript runtime dependencies and ships the selected immutable knowledge pack with `data/provenance.json`. It requires Node.js 24 and has no runtime npm dependencies. The package includes the Apache-2.0 license and bundled dependency licenses in `NOTICE` and `THIRD_PARTY_NOTICES.md`.

## Stdio MCP

Install the package or use `npx` from an MCP client configuration:

```sh
npm install --global @esp-vision/knowledge-mcp
esp-vision-knowledge-mcp
```

The stdio transport is the default. Its stdout contains MCP protocol messages only; diagnostics go to stderr. It does not read `MCP_STATS_*`, open a SQLite database, or make outbound requests. The npm package contains the immutable pack selected at release time. `MCP_PACK_PATH=/path/to/mcp-pack.json` or `--pack /path/to/mcp-pack.json` selects a different pack explicitly.

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

HTTP reads `PORT` (default `8080`) and `MCP_PACK_PATH` (default the pack in the installed npm archive). Anonymous aggregate usage statistics remain opt-in through `MCP_STATS_DB_PATH`; set `MCP_STATS_RETENTION_DAYS` to select retention from 1 through 3650 days (default 90). The `--stats-db` and `--stats-retention-days` command-line options configure the same HTTP-only settings. Statistics are never exposed by an HTTP endpoint or MCP tool.

The internal read-only report command reads the configured SQLite database without changing it:

```sh
MCP_STATS_DB_PATH=/path/to/usage.sqlite node dist/stats-cli.js --days 7 --format table
```

The command can emit JSON with `--format json`. It requires `MCP_STATS_DB_PATH`.

## Public service factory

The package exports `createKnowledgeServer({ pack, usageStats? })` and `Pack` from `@esp-vision/knowledge-mcp` (or `@esp-vision/knowledge-mcp/service`). Call `Pack.load(path)` to load an explicitly selected pack. Importing the factory does not load a pack, read environment variables, open a database, create a transport, make a network request, or listen on a port. The caller connects the returned MCP server to its chosen transport.

## Build, tests, and npm release

Use Node.js 24 and npm. Install the locked development toolchain and run the package checks:

```sh
npm ci
npm run build
npm test
```

The tests select `test/fixtures/mcp-pack.json` explicitly. Ordinary builds produce bundled code only and never select that fixture as production data. `npm pack` and release checks fail unless a validated production pack and matching provenance were generated.

To build a release archive, provide the exact pack selected by the main repository release:

```sh
MCP_PACK_PATH=/path/to/mcp-pack.json npm run build:release
npm run check:package
npm pack
```

The release build runs `tools/ci/mcp/validate_schema.py <pack> --require-micropython`, verifies a full 40-character lowercase `meta.sourceCommit`, copies the original pack bytes unchanged to `data/mcp-pack.json`, and writes provenance containing the package name/version, pack version/source commit, and SHA-256 of those exact bytes. Every new production pack snapshot requires a package-version bump. The protected firmware-release workflow binds the exact npm tarball SHA-256 to its image and release descriptor.

To use a one-off pack with the built code, pass `MCP_PACK_PATH` or `--pack`. The server never downloads or refreshes knowledge data.
