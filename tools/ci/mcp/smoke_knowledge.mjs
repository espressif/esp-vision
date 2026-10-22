#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { access, mkdir, mkdtemp, readFile, rm } from 'node:fs/promises'
import { createRequire } from 'node:module'
import { createServer } from 'node:net'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { spawn, spawnSync } from 'node:child_process'
import { once } from 'node:events'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../..')
assert.ok(process.argv.length === 4 && process.argv[2] === '--tarball', 'Usage: smoke_knowledge.mjs --tarball FILE')
const tarball = resolve(process.argv[3])
const require = createRequire(join(root, 'host/knowledge-mcp/package.json'))
const { Client } = await import(pathToFileURL(require.resolve('@modelcontextprotocol/sdk/client/index.js')))
const { StdioClientTransport } = await import(pathToFileURL(require.resolve('@modelcontextprotocol/sdk/client/stdio.js')))
const { StreamableHTTPClientTransport } = await import(pathToFileURL(require.resolve('@modelcontextprotocol/sdk/client/streamableHttp.js')))
const base = resolve(process.env.TMPDIR ?? join(root, '.cache/knowledge-smoke'))
await mkdir(base, { recursive: true })
const temporary = await mkdtemp(join(base, 'npm-knowledge-'))
const sha256 = value => createHash('sha256').update(value).digest('hex')
let http
try {
  const unpack = spawnSync('tar', ['-xzf', tarball, '-C', temporary], { encoding: 'utf8' })
  assert.equal(unpack.status, 0, unpack.stderr)
  const packed = join(temporary, 'package')
  const archiveSha = sha256(await readFile(tarball))
  const provenance = JSON.parse(await readFile(join(packed, 'data/provenance.json'), 'utf8'))
  const noStdioDb = join(temporary, 'stdio-must-not-exist.sqlite')
  const env = { ...process.env, NODE_NO_WARNINGS: '1', MCP_STATS_DB_PATH: noStdioDb, MCP_STATS_RETENTION_DAYS: 'invalid' }
  delete env.NODE_PATH
  delete env.NODE_OPTIONS
  delete env.MCP_PACK_PATH
  const transport = new StdioClientTransport({ command: process.execPath,
    args: [join(packed, 'dist/cli.js')], cwd: packed, env, stderr: 'pipe' })
  const client = new Client({ name: 'knowledge-npm-acceptance', version: '1.0.0' })
  try {
    await client.connect(transport)
    assert.ok((await client.listTools()).tools.some(tool => tool.name === 'server_info'))
    const reply = await client.callTool({ name: 'server_info', arguments: {} })
    assert.ok(!reply.isError)
    const info = JSON.parse(reply.content.find(item => item.type === 'text').text)
    assert.equal(info.sourceCommit, provenance.sourceCommit)
    assert.equal(info.packSha256, provenance.packSha256)
    assert.equal(info.packageVersion, provenance.packageVersion)
    const api = await client.callTool({ name: 'get_api', arguments: { query: 'sensor.snapshot' } })
    assert.ok(!api.isError)
  } finally { await client.close() }
  await assert.rejects(access(noStdioDb), { code: 'ENOENT' })

  const listener = createServer()
  listener.listen(0, '127.0.0.1')
  await once(listener, 'listening')
  const port = listener.address().port
  await new Promise(resolve => listener.close(resolve))
  const db = join(temporary, 'http-usage.sqlite')
  http = spawn(process.execPath, [join(packed, 'dist/cli.js'), '--transport', 'http', '--port', String(port), '--stats-db', db], {
    cwd: packed, env: { ...env, MCP_STATS_RETENTION_DAYS: '90', MCP_NPM_ARTIFACT_SHA256: archiveSha }, stdio: ['ignore', 'pipe', 'pipe'],
  })
  let diagnostic = ''
  http.stderr.on('data', chunk => { diagnostic += chunk })
  http.stdout.on('data', () => {})
  const url = new URL(`http://127.0.0.1:${port}/mcp`)
  let health
  for (let i = 0; i < 100; i++) {
    if (http.exitCode !== null) throw new Error(`Packed HTTP server exited: ${diagnostic}`)
    try {
      const response = await fetch(new URL('/health', url), { signal: AbortSignal.timeout(500) })
      if (response.ok) { health = await response.json(); break }
    } catch {}
    await new Promise(resolve => setTimeout(resolve, 50))
  }
  assert.ok(health, `Packed HTTP server did not become healthy: ${diagnostic}`)
  assert.equal(health.npmArtifactSha256, archiveSha)
  for (const key of ['packageVersion', 'sourceCommit', 'packSha256']) assert.equal(health[key], provenance[key])
  const remote = new Client({ name: 'knowledge-npm-http-acceptance', version: '1.0.0' })
  try {
    await remote.connect(new StreamableHTTPClientTransport(url))
    for (const [name, args] of [['server_info', {}], ['get_api', { query: 'sensor.snapshot' }]]) {
      const result = await remote.callTool({ name, arguments: args })
      assert.ok(!result.isError)
    }
  } finally { await remote.close() }
  const report = spawnSync(process.execPath, [join(packed, 'dist/stats-cli.js'), '--days', '1', '--format', 'json'], {
    cwd: packed, env: { ...env, MCP_STATS_DB_PATH: db }, encoding: 'utf8',
  })
  assert.equal(report.status, 0, report.stderr)
  const aggregate = JSON.parse(report.stdout)
  assert.equal(aggregate.totals.count, 2)
  assert.equal(aggregate.totals.error, 0)
  assert.doesNotMatch(JSON.stringify(aggregate), /requestId|sessionId|query|userId|clientId/)
  console.log(JSON.stringify({ npm: archiveSha, stdio: 'passed; no statistics database', http: 'passed', anonymousToolCalls: 2 }))
} finally {
  if (http && http.exitCode === null) {
    const exited = once(http, 'exit')
    http.kill('SIGTERM')
    const timeout = setTimeout(() => http.kill('SIGKILL'), 3000)
    await exited
    clearTimeout(timeout)
  }
  await rm(temporary, { recursive: true, force: true })
}
