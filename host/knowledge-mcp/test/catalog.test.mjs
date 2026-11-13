// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { test } from 'node:test';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';
import { KnowledgeCatalog, createKnowledgeServer } from '../dist/service.js';
import { packPath, serviceRoot, startServer, temporaryDirectory } from './helpers.mjs';

async function fixture(t) {
  const directory = await temporaryDirectory(t, 'catalog-');
  const data = JSON.parse(await readFile(packPath));
  const snapshots = ['2026.10.21', '2026.11.06', '2026.10.21-r2'].map((revision, index) => {
    const pack = structuredClone(data);
    const firmwareVersion = index === 2 ? '2026.10.21' : revision;
    pack.meta = { ...pack.meta, version: firmwareVersion, sourceCommit: String(index + 1).repeat(40) };
    pack.modules.find(m => m.name === 'sensor').symbols[0].doc = `exclusive-content-${revision}`;
    const packJson = JSON.stringify(pack);
    return { firmwareVersion, snapshotVersion: revision, firmwareSourceCommit: index === 2 ? '1'.repeat(40) : pack.meta.sourceCommit,
      contentSourceCommit: pack.meta.sourceCommit, packSha256: createHash('sha256').update(packJson).digest('hex'), packJson };
  });
  const catalog = { schemaVersion: 1, latestFirmwareVersion: '2026.11.06', activeSnapshots: { '2026.10.21': '2026.10.21-r2', '2026.11.06': '2026.11.06' }, snapshots };
  const path = join(directory, 'mcp-catalog.json');
  await writeFile(path, JSON.stringify(catalog));
  return { path, catalog };
}

for (const mode of ['memory', 'stdio', 'http']) {
  test(`${mode}: exact firmware selection, concurrent isolation, historical revisions and no fallback`, async t => {
    const { path } = await fixture(t);
    const client = new Client({ name: 'catalog-test', version: '1.0.0' });
    let transport, service;
    if (mode === 'memory') {
      service = createKnowledgeServer({ catalog: KnowledgeCatalog.load(path) });
      const [left, right] = InMemoryTransport.createLinkedPair();
      transport = left;
      await service.connect(right);
    } else if (mode === 'stdio') {
      transport = new StdioClientTransport({ command: process.execPath,
        args: [join(serviceRoot, 'dist/cli.js'), '--catalog', path], stderr: 'pipe' });
    } else {
      const running = await startServer(t, { extraEnv: { MCP_PACK_PATH: '', MCP_CATALOG_PATH: path } });
      transport = new StreamableHTTPClientTransport(new URL(running.baseUrl + '/mcp'));
    }
    try {
      await client.connect(transport);
      const discovery = await client.callTool({ name: 'list_firmware_versions', arguments: {} });
      assert.equal(JSON.parse(discovery.content[0].text).latestFirmwareVersion, '2026.11.06');
      assert.equal(JSON.parse((await client.callTool({ name: 'server_info', arguments: {} })).content[0].text).latestFirmwareVersion, '2026.11.06');
      assert.deepEqual(JSON.parse(discovery.content[0].text).firmwareVersions, ['2026.10.21', '2026.11.06']);
      const tools = (await client.listTools()).tools.filter(t => !['server_info', 'list_firmware_versions'].includes(t.name));
      for (const tool of tools) assert.ok(tool.inputSchema.required.includes('firmwareVersion'), tool.name);
      const args = { query: 'sensor', name: 'p4', topic: 'camera-pipeline', module: 'sensor' };
      for (const tool of tools) {
        for (const version of [undefined, '2026.11.12', '2026.11.06-dirty', 'v2026.11.06']) {
          const result = await client.callTool({ name: tool.name, arguments: { ...args, ...(version ? { firmwareVersion: version } : {}) } });
          assert.equal(result.isError, true, `${tool.name}: ${version}`);
          assert.doesNotMatch(JSON.stringify(result), /exclusive-content/);
        }
      }
      await Promise.all(Array.from({ length: 20 }, async (_, i) => {
        const firmwareVersion = i % 2 ? '2026.10.21' : '2026.11.06';
        const revision = i % 2 ? '2026.10.21-r2' : '2026.11.06';
        const result = await client.callTool({ name: 'get_api', arguments: { query: 'sensor.snapshot', firmwareVersion } });
        assert.notEqual(result.isError, true);
        assert.equal(result._meta.knowledgeSnapshot.snapshotVersion, revision);
        assert.equal(JSON.parse(result.content[0].text).snapshot.snapshotVersion, revision);
        assert.match(result.content[0].text, new RegExp(`exclusive-content-${revision}`));
        assert.doesNotMatch(result.content[0].text, new RegExp(`exclusive-content-${i % 2 ? '2026.11.06' : '2026.10.21'}`));
      }));
      const old = await client.callTool({ name: 'get_api', arguments: { query: 'sensor.snapshot', firmwareVersion: '2026.10.21', snapshotVersion: '2026.10.21' } });
      assert.equal(old._meta.knowledgeSnapshot.snapshotVersion, '2026.10.21');
      assert.doesNotMatch(old.content[0].text, /exclusive-content-2026.10.21-r2/);
      const wrong = await client.callTool({ name: 'get_api', arguments: { query: 'sensor.snapshot', firmwareVersion: '2026.11.06', snapshotVersion: '2026.10.21' } });
      assert.equal(wrong.isError, true);
      const resource = await client.readResource({ uri: 'esp-vision://firmware/2026.10.21/snapshot/2026.10.21/module/sensor' });
      assert.equal(JSON.parse(resource.contents[0].text).snapshot.snapshotVersion, '2026.10.21');
      await assert.rejects(client.readResource({ uri: 'esp-vision://module/sensor' }));
      await assert.rejects(client.readResource({ uri: 'esp-vision://firmware/unknown/module/sensor' }));
    } finally { await client.close(); await service?.close(); }
  });
}

test('catalog rejects tampering, duplicate revisions, commit mismatch and cross-firmware active mappings', async t => {
  const { path, catalog } = await fixture(t);
  for (const mutate of [
    c => { delete c.latestFirmwareVersion; },
    c => { c.latestFirmwareVersion = 'unavailable'; },
    c => {
      const snapshot = c.snapshots[0];
      const pack = JSON.parse(snapshot.packJson); pack.meta.version = 'latest';
      Object.assign(snapshot, { firmwareVersion: 'latest', snapshotVersion: 'latest', packJson: JSON.stringify(pack) });
      snapshot.packSha256 = createHash('sha256').update(snapshot.packJson).digest('hex');
      c.snapshots = [snapshot]; c.activeSnapshots = { latest: 'latest' }; c.latestFirmwareVersion = 'latest';
    },
    c => { c.snapshots[0].packJson += ' '; },
    c => { c.snapshots.push(c.snapshots[0]); },
    c => { c.snapshots[0].contentSourceCommit = 'a'.repeat(40); },
    c => { c.snapshots[2].firmwareSourceCommit = 'a'.repeat(40); },
    c => { c.activeSnapshots['2026.10.21'] = '2026.11.06'; },
    c => { delete c.activeSnapshots['2026.10.21']; },
  ]) {
    const changed = structuredClone(catalog); mutate(changed);
    await writeFile(path, JSON.stringify(changed));
    assert.throws(() => KnowledgeCatalog.load(path));
  }
});
