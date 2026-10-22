# Build context: host/knowledge-mcp. CI supplies the validated release pack as
# mcp-pack.json before invoking docker build.
FROM node:24-bookworm-slim AS build
WORKDIR /srv
COPY package.json package-lock.json ./
RUN npm ci --ignore-scripts --no-audit --no-fund
COPY tsconfig.json ./
COPY src ./src
RUN npm run build && npm prune --omit=dev --ignore-scripts --no-audit --no-fund

FROM node:24-bookworm-slim AS runtime
WORKDIR /srv
ENV MCP_PACK_PATH=/srv/data/mcp-pack.json
ENV MCP_STATS_DB_PATH=/srv/stats/usage.sqlite
ENV MCP_STATS_RETENTION_DAYS=90
ENV PORT=8080
EXPOSE 8080
COPY --from=build /srv/package.json ./package.json
COPY --from=build /srv/node_modules ./node_modules
COPY --from=build /srv/dist ./dist
COPY LICENSE ./LICENSE
COPY mcp-pack.json /srv/data/mcp-pack.json
CMD ["node", "dist/server.js"]
