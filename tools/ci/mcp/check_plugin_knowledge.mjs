// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'

const contentTools = ['get_api', 'get_capabilities', 'search_docs', 'list_modules', 'list_symbols', 'get_example', 'list_examples', 'get_concept']
const identityKeys = ['firmwareVersion', 'snapshotVersion', 'firmwareSourceCommit', 'contentSourceCommit', 'packSha256']
const sorted = values => [...values].sort((a, b) => a.snapshotVersion.localeCompare(b.snapshotVersion))
const toolJson = result => {
  assert.ok(!result?.isError, 'Knowledge content request failed')
  return JSON.parse(result.content.find(item => item.type === 'text').text)
}

// Run against the Plugin's configured endpoint immediately before publishing.
// Injectable transport and timeout are for local regression tests, not CI bypasses.
export async function verifyPluginKnowledge(endpoint, { fetchImpl = globalThis.fetch, timeoutMs = 30000 } = {}) {
  const url = new URL(endpoint)
  assert.ok(url.protocol === 'https:' || (url.protocol === 'http:' && ['127.0.0.1', 'localhost'].includes(url.hostname)), 'Knowledge endpoint must use HTTPS')
  const signal = AbortSignal.timeout(timeoutMs)
  async function request(target, options = {}) {
    const response = await fetchImpl(target, { ...options, signal, redirect: 'error' })
    assert.ok(response.ok, 'Knowledge endpoint is unavailable')
    let bytes = 0
    const chunks = []
    for await (const chunk of response.body) {
      bytes += chunk.byteLength
      assert.ok(bytes <= 16 * 1024 * 1024, 'Knowledge response exceeds the size limit')
      chunks.push(Buffer.from(chunk))
    }
    return { text: Buffer.concat(chunks).toString('utf8'), type: response.headers.get('content-type') ?? '' }
  }
  let id = 0
  async function rpc(method, params, allowError = false) {
    const current = ++id
    const response = await request(url, { method: 'POST', headers: { 'Content-Type': 'application/json', Accept: 'application/json, text/event-stream' },
      body: JSON.stringify({ jsonrpc: '2.0', id: current, method, params }) })
    const values = response.type.includes('text/event-stream') ? response.text.split(/\r?\n\r?\n/)
      .map(event => event.split(/\r?\n/).filter(line => line.startsWith('data:')).map(line => line.slice(5).trim()).join('\n'))
      .filter(Boolean).map(value => JSON.parse(value)) : [JSON.parse(response.text)]
    const reply = values.find(value => value.id === current)
    assert.ok(reply, 'Knowledge RPC response is missing')
    if (allowError) return reply
    assert.ok(!reply.error && reply.result && !reply.result.isError, 'Knowledge RPC contract is unavailable')
    return reply.result
  }
  const healthUrl = new URL(url)
  healthUrl.pathname = healthUrl.pathname.replace(/\/$/, '') + '/health'
  const health = JSON.parse((await request(healthUrl)).text)
  assert.equal(health.schemaVersion, 2, 'Plugin requires versioned Knowledge health')
  for (const key of ['catalogSha256', 'npmArtifactSha256']) assert.match(health[key], /^[a-f0-9]{64}$/)
  assert.match(health.sourceCommit, /^[a-f0-9]{40}$/)
  assert.ok(Array.isArray(health.snapshots) && health.snapshots.length, 'Knowledge snapshots are missing')
  const init = await rpc('initialize', { protocolVersion: '2025-03-26', capabilities: {}, clientInfo: { name: 'plugin-release-check', version: '1.0.0' } })
  assert.equal(init.serverInfo.version, health.packageVersion)
  const info = toolJson(await rpc('tools/call', { name: 'server_info', arguments: {} }))
  assert.deepEqual(info, health, 'Knowledge health and runtime identities differ')
  const versions = toolJson(await rpc('tools/call', { name: 'list_firmware_versions', arguments: {} }))
  assert.deepEqual(sorted(versions.snapshots), sorted(health.snapshots))
  assert.deepEqual([...versions.firmwareVersions].sort(), [...new Set(health.snapshots.map(snapshot => snapshot.firmwareVersion))].sort())
  assert.equal(versions.latestFirmwareVersion, health.latestFirmwareVersion)
  assert.ok(typeof versions.latestFirmwareVersion === 'string' && versions.firmwareVersions.includes(versions.latestFirmwareVersion), 'Latest firmware must be available')
  const selected = versions.snapshots.filter(snapshot => snapshot.active && snapshot.firmwareVersion === versions.latestFirmwareVersion)
  assert.equal(selected.length, 1, 'Latest firmware must have one active snapshot')
  const snapshot = Object.fromEntries(identityKeys.map(key => [key, selected[0][key]]))
  const { tools } = await rpc('tools/list', {})
  for (const name of contentTools) {
    const schema = tools.find(tool => tool.name === name)?.inputSchema
    assert.ok(schema?.required?.includes('firmwareVersion') && schema.properties?.snapshotVersion, 'Knowledge content selectors are missing: ' + name)
  }
  const reply = await rpc('tools/call', { name: 'get_api', arguments: { query: 'sensor.snapshot', firmwareVersion: snapshot.firmwareVersion, snapshotVersion: snapshot.snapshotVersion } })
  const body = toolJson(reply)
  assert.deepEqual(body.snapshot, snapshot)
  assert.deepEqual(reply._meta?.knowledgeSnapshot, snapshot)
  assert.ok(body.result?.matches?.some(symbol => symbol.id === 'sensor.snapshot'), 'Versioned Knowledge API query failed')
  const missing = await rpc('tools/call', { name: 'get_api', arguments: { query: 'sensor.snapshot' } }, true)
  assert.ok(missing.error?.code === -32602 || missing.result?.isError === true, 'Knowledge silently accepts missing firmware')
  const unknown = await rpc('tools/call', { name: 'get_api', arguments: { query: 'sensor.snapshot', firmwareVersion: 'latest' } }, true)
  assert.equal(unknown.result?.isError, true, 'Knowledge silently substitutes latest')
  assert.equal(JSON.parse(unknown.result.content.find(item => item.type === 'text').text).code, 'KNOWLEDGE_VERSION_UNAVAILABLE')
  return { packageVersion: health.packageVersion, catalogSha256: health.catalogSha256 }
}
