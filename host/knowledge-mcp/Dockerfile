# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
# The context contains the exact npm tarball already checked by main CI.
FROM node:24-bookworm-slim
WORKDIR /srv
ARG MCP_NPM_SHA256
ENV MCP_NPM_ARTIFACT_SHA256=${MCP_NPM_SHA256}
ENV MCP_PACK_PATH=/srv/data/mcp-pack.json
ENV MCP_STATS_DB_PATH=/srv/stats/usage.sqlite
ENV MCP_STATS_RETENTION_DAYS=90
ENV PORT=8080
EXPOSE 8080
COPY knowledge-mcp.tgz /srv/knowledge-mcp.tgz
RUN test -n "$MCP_NPM_SHA256" && echo "$MCP_NPM_SHA256  knowledge-mcp.tgz" | sha256sum -c - \
    && tar -xzf knowledge-mcp.tgz --strip-components=1 -C /srv \
    && rm knowledge-mcp.tgz
CMD ["node", "dist/server.js"]
