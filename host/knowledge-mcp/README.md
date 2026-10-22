# ESP-VISION Knowledge MCP runtime

This private Node 24 service exposes the ESP-VISION knowledge pack over stateless Streamable HTTP. It preserves the existing MCP tools, `esp-vision://module/{name}` resource, root and `/mcp` request addresses, and `/health` and `/mcp/health` health addresses. It does not implement stdio or distribute a local knowledge corpus; the service is built as an immutable server image and deployed by the website project.

The main ESP-VISION repository owns knowledge generation and release. CI validates the selected release pack and provides it as `mcp-pack.json` at this directory's Docker build context root. The Dockerfile copies those exact bytes to `/srv/data/mcp-pack.json`; the running server does not fetch or refresh knowledge data. Deployments select the image digest bound to the same release descriptor.

The image listens on port 8080. Anonymous aggregate statistics use `/srv/stats/usage.sqlite`, with 90 days of retention by default. Statistics are never exposed through HTTP or an MCP tool. The internal read-only report command runs in the existing service container:

```sh
MCP_STATS_DB_PATH=/srv/stats/usage.sqlite node dist/stats-cli.js --days 7 --format table
```

Reports can also be emitted as JSON with `--format json`; the command reads the SQLite database without changing it. The database should remain on the project's local persistent stats volume.

## Local build and verification

Use Node.js 24 and npm. Install from this package's independent lockfile, then build and run the 18 package tests:

```sh
npm ci
npm run build
npm test
```

The tests use a deliberately small real-format fixture at `test/fixtures/mcp-pack.json`, so they do not depend on the website repository or a full documentation build. To exercise the HTTP contract against a generated production pack, set `MCP_TEST_PACK_PATH` to its path when running `npm test`.

To run the HTTP service locally, set `MCP_PACK_PATH` to an explicitly selected pack file and optionally set `PORT` and `MCP_STATS_DB_PATH`. The server has no knowledge-pack download behavior. `npm run stats:report -- --days 7 --format table` reads the database configured through `MCP_STATS_DB_PATH`.
