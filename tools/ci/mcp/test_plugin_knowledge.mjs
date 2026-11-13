// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { test } from 'node:test'
import { createServer } from 'node:http'
import { verifyPluginKnowledge } from './check_plugin_knowledge.mjs'

const snapshot = { firmwareVersion: 'v1.2.3', snapshotVersion: 'v1.2.3-docs2', firmwareSourceCommit: 'a'.repeat(40), contentSourceCommit: 'b'.repeat(40), packSha256: 'c'.repeat(64) }
const health = { name: 'esp-vision-mcp', schemaVersion: 2, packageVersion: '0.2.0', sourceCommit: 'd'.repeat(40),
  catalogSha256: 'e'.repeat(64), npmArtifactSha256: 'f'.repeat(64), latestFirmwareVersion: snapshot.firmwareVersion, snapshots: [{ ...snapshot, active: true }] }
const contentTools = ['get_api', 'get_capabilities', 'search_docs', 'list_modules', 'list_symbols', 'get_example', 'list_examples', 'get_concept']
const json = value => ({ content: [{ type: 'text', text: JSON.stringify(value) }] })
async function serve(t, fault, sse = false) {
  const server = createServer(async (req, res) => {
    if (fault === 'timeout') return
    if (fault === 'unavailable') { res.writeHead(503); res.end(); return }
    if (req.method === 'GET') {
      res.setHeader('Content-Type', 'application/json')
      res.end(JSON.stringify({ ...health, ...(fault === 'old-health' ? { schemaVersion: 1 } : {}) })); return
    }
    const parts = []
    for await (const part of req) parts.push(part)
    const request = JSON.parse(Buffer.concat(parts).toString())
    let result
    if (request.method === 'initialize') result = { serverInfo: { name: health.name, version: health.packageVersion } }
    else if (request.method === 'tools/list') result = { tools: contentTools.map(name => ({ name, inputSchema: {
      required: fault === 'old-selectors' ? [] : ['firmwareVersion'], properties: { snapshotVersion: { type: 'string' } },
    } })) }
    else if (request.params.name === 'server_info') result = json({ ...health, ...(fault === 'wrong-health' ? { sourceCommit: '0'.repeat(40) } : {}) })
    else if (request.params.name === 'list_firmware_versions') {
      if (fault === 'missing-tool') { res.end(JSON.stringify({ jsonrpc: '2.0', id: request.id, error: { code: -32602 } })); return }
      result = json({ firmwareVersions: [snapshot.firmwareVersion], latestFirmwareVersion: fault === 'wrong-latest' ? '2026.11.6' : health.latestFirmwareVersion,
        snapshots: fault === 'missing-history' ? [] : health.snapshots })
    } else {
      const version = request.params.arguments.firmwareVersion
      if (!version && fault !== 'implicit-default') result = { ...json({ code: 'FIRMWARE_VERSION_REQUIRED' }), isError: true }
      else if (version === 'latest' && fault !== 'implicit-latest') result = { ...json({ code: 'KNOWLEDGE_VERSION_UNAVAILABLE' }), isError: true }
      else result = { ...json({ snapshot: fault === 'wrong-snapshot' ? { ...snapshot, firmwareVersion: 'old' } : snapshot,
        result: { matches: [{ id: 'sensor.snapshot' }] } }), _meta: { knowledgeSnapshot: snapshot } }
    }
    const reply = JSON.stringify({ jsonrpc: '2.0', id: request.id, result })
    res.setHeader('Content-Type', sse ? 'text/event-stream' : 'application/json')
    res.end(sse ? 'event: message\ndata: ' + reply + '\n\n' : reply)
  })
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)) })
  return `http://127.0.0.1:${server.address().port}/mcp`
}
for (const sse of [false, true]) test(`publication gate accepts a compatible live endpoint (${sse ? 'SSE' : 'JSON'})`, async t => {
  const url = await serve(t, '', sse)
  assert.deepEqual(await verifyPluginKnowledge(url), { packageVersion: '0.2.0', catalogSha256: health.catalogSha256 })
})
for (const fault of ['unavailable', 'old-health', 'missing-tool', 'old-selectors', 'wrong-health', 'wrong-latest', 'missing-history', 'wrong-snapshot', 'implicit-default', 'implicit-latest']) {
  test(`publication gate blocks ${fault}`, async t => {
    await assert.rejects(verifyPluginKnowledge(await serve(t, fault)))
  })
}
test('publication gate has one bounded deadline and never retries a failed request', async t => {
  await assert.rejects(verifyPluginKnowledge(await serve(t, 'timeout'), { timeoutMs: 50 }))
})
