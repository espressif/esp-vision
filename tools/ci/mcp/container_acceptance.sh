#!/bin/sh
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
# This uses a unique disposable container/volume, never a deployment service.
set -eu
: "MCP_ACCEPTANCE_IMAGE:?exact producer image digest is required}"
case "$MCP_ACCEPTANCE_IMAGE" in *@sha256:*) ;; *) echo 'Immutable image required' >&2; exit 1 ;; esac
task_name="mcp-acceptance-${CI_PROJECT_ID:-local}-${CI_PIPELINE_ID:-local}-${CI_JOB_ID:-local}"
volume_name="$task_name-stats"
cleanup() {
    docker rm -f "$task_name" >/dev/null 2>&1 || true
    docker volume rm "$volume_name" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM
docker pull "$MCP_ACCEPTANCE_IMAGE"
docker volume create "$volume_name" >/dev/null
start() {
    docker run -d --name "$task_name" --network none \
        --mount "type=volume,source=$volume_name,target=/srv/stats" \
        "$MCP_ACCEPTANCE_IMAGE" >/dev/null
}
check() {
    docker exec -i -e MCP_EXPECTED_NPM_SHA256 -e MCP_EXPECTED_PACK_SHA256 -e MCP_EXPECTED_SOURCE_COMMIT "$task_name" node --input-type=module - "$1" <<'JS'
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
let healthy = false;
for (let i = 0; i < 40; i++) {
  try { healthy = (await fetch('http://127.0.0.1:8080/health')).ok; } catch {}
  if (healthy) break;
  await new Promise(resolve => setTimeout(resolve, 250));
}
assert.ok(healthy, 'Container did not become healthy');
const identity = await (await fetch('http://127.0.0.1:8080/health')).json();
for (const [field, variable] of [['npmArtifactSha256','MCP_EXPECTED_NPM_SHA256'], ['packSha256','MCP_EXPECTED_PACK_SHA256'], ['sourceCommit','MCP_EXPECTED_SOURCE_COMMIT']]) {
  assert.ok(process.env[variable], 'Expected artifact identity must be configured');
  assert.equal(identity[field], process.env[variable], 'Container artifact identity mismatch: ' + field);
}
const reply = await fetch('http://127.0.0.1:8080/mcp', {
  method: 'POST', headers: {'Content-Type':'application/json', Accept:'application/json, text/event-stream'},
  body: JSON.stringify({jsonrpc:'2.0',id:1,method:'tools/call',params:{name:'server_info',arguments:{}}})
});
assert.ok(reply.ok);
const body = await reply.text();
assert.ok(body.includes('esp-vision-mcp'), 'Tool call did not succeed');
const report = JSON.parse(execFileSync(process.execPath,['dist/stats-cli.js','--days','1','--format','json'],{encoding:'utf8'}));
assert.equal(report.totals.count, Number(process.argv[2]));
assert.equal(report.totals.error, 0);
assert.ok(!JSON.stringify(report).includes('requestId'));
console.log(JSON.stringify({containerAcceptance:'passed',expectedExecutions:Number(process.argv[2])}));
JS
}
start
check 1
docker rm -f "$task_name" >/dev/null
start
check 2
