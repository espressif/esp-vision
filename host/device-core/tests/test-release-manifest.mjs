// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { createDevice } from '../src/index.js'
import { loadOfficialFirmware, loadReleaseManifest, firmwareSha256 } from '../src/firmware.js'

const base = 'https://dl.espressif.com/AE/esp-vision/'
const commit = 'a'.repeat(40)
async function fixture(version = 'new', sourceCommit = commit) {
  const bytes = new Uint8Array(0x11000).fill(0xff)
  bytes.set([0xe9, 1, 2, 3], 0x2000); bytes.set([0xaa, 0x50, 4, 5], 0x8000); bytes.set([0xe9, 6, 7, 8], 0x10000)
  const segment = async offset => ({ offset, bytes: 4, sha256: await firmwareSha256(bytes.subarray(offset, offset + 4)) })
  const source = { url: `${base}bin/esp-vision-BOARD-${version}.bin`, bytes: bytes.byteLength, sha256: await firmwareSha256(bytes), offset: 0x10000 }
  const release = { board: 'BOARD', version, image: {
    evMuxVersion: 4, chipFamily: 0x3280, chipRevision: 302, ...(await segment(0x10000)),
    partitionSize: 4096, partitionTable: await segment(0x8000), url: source.url, source,
  }, install: { url: source.url, offset: 0, bytes: source.bytes, sha256: source.sha256, flashSize: 0x1000000,
    chipRevisionMin: 302, chipRevisionMax: 302, bootloader: await segment(0x2000) } }
  const manifest = { schemaVersion: 1, version, sourceCommit, pipelineId: 123, layout: 'single-bin', releases: [release], website: {
    schemaVersion: '1.0.0', release: version, releases: [version], binaryNameTemplate: 'esp-vision-{board}-{tag}.bin',
    boards: [{ slug: 'board', board: 'BOARD', name: 'Board', flashable: true }],
    models: [{ slug: 'model', sizeBytes: 4, path: 'models/a/model.espdl', downloadUrl: `https://raw.githubusercontent.com/espressif/esp-vision/${sourceCommit}/models/a/model.espdl` }],
  }, mcp: { url: `${base}bin/mcp/mcp-pack-${version}.json`, bytes: 1234, sha256: 'c'.repeat(64), version, sourceCommit } }
  const reference = async () => {
    const raw = new TextEncoder().encode(JSON.stringify(manifest))
    return { url: `${base}releases/${version}/manifest.json`, sha256: await firmwareSha256(raw), version, sourceCommit, boards: ['BOARD'], fetch: async () => new Response(raw) }
  }
  return { bytes, manifest, release, reference }
}
function config(current, history = [current]) {
  return `[publication]\nschemaVersion = 1\nversion = "${current.version}"\nsourceCommit = "${current.sourceCommit}"\nmanifestUrl = "${current.url}"\nmanifestSha256 = "${current.sha256}"\npipelineId = 123\npreviousConfigSha256 = "${'e'.repeat(64)}"\nreleases = [${history.map(item => `{version = "${item.version}", manifestUrl = "${item.url}", manifestSha256 = "${item.sha256}", boards = ["BOARD"]}`).join(',')}]\n`
}

test('public manifest reader verifies the raw snapshot and preserves website, MCP and source metadata', async () => {
  const f = await fixture(), request = await f.reference()
  const result = await loadReleaseManifest(request)
  assert.deepEqual(result, f.manifest)
  assert.equal(result.releases[0].image.bytes, 4)
  assert.equal(result.releases[0].image.source.bytes, f.bytes.length)
})
test('raw manifest hash is checked before parsing or returning any metadata', async () => {
  const f = await fixture(), request = await f.reference()
  await assert.rejects(loadReleaseManifest({ ...request, fetch: async () => new Response(JSON.stringify(f.manifest) + '\n') }), /manifest SHA-256/)
  await assert.rejects(loadReleaseManifest({ ...request, sha256: 'invalid', fetch: () => { throw new Error('must not fetch') } }), /qualified SHA-256/)
})
for (const mutation of ['schema', 'version', 'commit', 'pipeline', 'layout', 'layout-source', 'duplicate-board', 'entry-version', 'entry-commit', 'app-hash', 'source-size', 'firmware-host', 'website-version', 'website-board', 'model-master', 'model-traversal', 'mcp-url', 'mcp-latest', 'mcp-version', 'mcp-commit', 'mcp-size']) {
  test(`invalid manifest ${mutation} never yields an approved release`, async () => {
    const f = await fixture(), m = f.manifest
    if (mutation === 'schema') m.schemaVersion = 2
    if (mutation === 'version') m.version = 'other'
    if (mutation === 'commit') m.sourceCommit = 'master'
    if (mutation === 'pipeline') m.pipelineId = 0
    if (mutation === 'layout') m.layout = 'unknown'
    if (mutation === 'layout-source') m.layout = 'legacy'
    if (mutation === 'duplicate-board') m.releases.push(structuredClone(f.release))
    if (mutation === 'entry-version') f.release.version = 'other'
    if (mutation === 'entry-commit') f.release.sourceCommit = 'b'.repeat(40)
    if (mutation === 'app-hash') f.release.image.sha256 = 'invalid'
    if (mutation === 'source-size') f.release.image.source.bytes++
    if (mutation === 'firmware-host') {
      const url = 'https://untrusted.test/app.bin'
      f.release.image.url = url; f.release.image.source.url = url; f.release.install.url = url
    }
    if (mutation === 'website-version') m.website.release = 'other'
    if (mutation === 'website-board') m.website.boards[0].board = 'UNQUALIFIED'
    if (mutation === 'model-master') m.website.models[0].downloadUrl = m.website.models[0].downloadUrl.replace(commit, 'master')
    if (mutation === 'model-traversal') {
      const model = m.website.models[0]
      model.path = 'models/%2e%2e/%2e%2e/master/models/evil.espdl'
      model.downloadUrl = `https://raw.githubusercontent.com/espressif/esp-vision/${commit}/${model.path}`
    }
    if (mutation === 'mcp-url') m.mcp.url = 'https://untrusted.test/pack.json'
    if (mutation === 'mcp-latest') m.mcp.url = `${base}bin/mcp/mcp-pack-latest.json`
    if (mutation === 'mcp-version') m.mcp.version = 'other'
    if (mutation === 'mcp-commit') m.mcp.sourceCommit = 'b'.repeat(40)
    if (mutation === 'mcp-size') m.mcp.bytes = 17 * 1024 * 1024
    const reference = await f.reference()
    reference.version = 'new'; reference.sourceCommit = commit
    await assert.rejects(loadReleaseManifest(reference))
  })
}
test('multi-file model groups may share a slug while each model path remains unique', async () => {
  const f = await fixture(), model = f.manifest.website.models[0]
  const otherPath = 'models/a/other.espdl'
  f.manifest.website.models.push({ ...model, path: otherPath, downloadUrl: model.downloadUrl.replace(model.path, otherPath) })
  assert.equal((await loadReleaseManifest(await f.reference())).website.models.length, 2)
  f.manifest.website.models.push(structuredClone(model))
  await assert.rejects(loadReleaseManifest(await f.reference()), /Invalid manifest model/)
})
test('manifest board set is checked against its immutable publication reference', async () => {
  const f = await fixture()
  await assert.rejects(loadReleaseManifest({ ...await f.reference(), boards: ['OTHER'] }), /board set/)
})
test('legacy manifest layout accepts qualified application-only artifacts and explicitly missing MCP', async () => {
  const f = await fixture(); f.manifest.layout = 'legacy'; f.manifest.mcp = null
  delete f.release.image.source
  f.release.image.url = `${base}bin/app.bin`
  assert.equal((await loadReleaseManifest(await f.reference())).layout, 'legacy')
})
test('official check loads only the requested version snapshot and never traverses old board/version metadata', async () => {
  const current = await fixture(), old = await fixture('old', 'b'.repeat(40)), refs = [await current.reference(), await old.reference()], calls = []
  const fetch = async (url, options) => {
    calls.push(url)
    assert.equal(options.cache, url.endsWith('config.toml') ? 'no-store' : 'default')
    if (url.endsWith('config.toml')) return new Response(config(refs[0], refs))
    assert.equal(url, refs[1].url)
    assert.equal(options.redirect, 'error')
    return refs[1].fetch()
  }
  const checked = await createDevice().firmware.check({ identity: { board: 'BOARD', firmwareVersion: 'older' }, version: 'old', fetch })
  assert.equal(checked.release.version, 'old')
  assert.deepEqual(calls, [base + 'config.toml', refs[1].url])
  assert.equal(checked.releases.length, 1)
})
test('default official reader returns one current snapshot and its version references', async () => {
  const f = await fixture(), reference = await f.reference(), calls = []
  const result = await loadOfficialFirmware({ fetch: async url => {
    calls.push(url)
    return url.endsWith('config.toml') ? new Response(config(reference)) : reference.fetch()
  } })
  assert.equal(result.sourceCommit, commit); assert.equal(result.publication.releases[0].version, 'new')
  assert.deepEqual(result.website, f.manifest.website)
  assert.equal(calls.length, 2)
})
for (const mutation of ['manifest-404', 'manifest-corrupt', 'publication-invalid', 'missing-version', 'wrong-board']) {
  test(`explicit publication ${mutation} never falls back to legacy per-board metadata`, async () => {
    const f = await fixture(), reference = await f.reference(), calls = []
    const request = { version: mutation === 'missing-version' ? 'missing' : 'new', board: mutation === 'wrong-board' ? 'OTHER' : 'BOARD', fetch: async url => {
      calls.push(url)
      if (url.endsWith('config.toml')) return new Response(mutation === 'publication-invalid' ? '[publication]\nschemaVersion = 7' : config(reference))
      return mutation === 'manifest-corrupt' ? new Response('{}') : new Response('', { status: 404 })
    } }
    await assert.rejects(loadOfficialFirmware(request))
    assert.ok(!calls.some(url => url.includes('/firmware/')))
    assert.equal(calls.length, ['manifest-404', 'manifest-corrupt'].includes(mutation) ? 2 : 1)
  })
}
test('old official configs retain compatibility but explicit version avoids historical requests', async () => {
  const f = await fixture('old'), calls = []
  delete f.release.image.source
  const text = `firmware_images_url = "${base}bin/"\n[esp-vision.image]\nBOARD = "latest.bin"\n[website]\nrelease = "new"\nreleases = ["new", "old"]\nbinaryNameTemplate = "esp-vision-{board}-{tag}.bin"\n`
  const result = await loadOfficialFirmware({ version: 'old', fetch: async url => {
    calls.push(url)
    return url.endsWith('config.toml') ? new Response(text) : new Response(JSON.stringify(f.release))
  } })
  assert.equal(result.releases[0].version, 'old')
  assert.deepEqual(calls, [base + 'config.toml', base + 'firmware/BOARD/old.json'])
})
test('manifest responses enforce a byte limit before allocation and during streaming', async () => {
  const f = await fixture(), reference = await f.reference()
  await assert.rejects(loadReleaseManifest({ ...reference, fetch: async () => ({ ok: true, headers: new Headers({ 'content-length': String(5 * 1024 * 1024) }), arrayBuffer() { throw new Error('must not allocate') } }) }), /byte limit/)
  let cancelled = false
  const response = new Response(new ReadableStream({ start(controller) { controller.enqueue(new Uint8Array(4 * 1024 * 1024 + 1)) }, cancel() { cancelled = true } }))
  await assert.rejects(loadReleaseManifest({ ...reference, fetch: async () => response }), /byte limit/)
  assert.equal(cancelled, true)
})
test('a cancelled pending manifest stream closes its reader and rejects', async () => {
  const f = await fixture(), controller = new AbortController()
  let cancelled = false
  const response = new Response(new ReadableStream({ cancel() { cancelled = true } }))
  const pending = loadReleaseManifest({ ...await f.reference(), signal: controller.signal, fetch: async () => response })
  await new Promise(resolve => setTimeout(resolve, 5))
  controller.abort()
  await assert.rejects(pending, { name: 'AbortError' })
  assert.equal(cancelled, true)
})
test('official manifest URLs reject HTTP, credentials, redirects and outside-root origins', async () => {
  const f = await fixture(), reference = await f.reference()
  for (const url of ['http://dl.espressif.com/AE/esp-vision/release.json', 'https://user:pass@dl.espressif.com/AE/esp-vision/release.json', 'https://dl.espressif.com/other/release.json', 'https://example.test/release.json']) {
    await assert.rejects(loadReleaseManifest({ ...reference, url }))
  }
  await assert.rejects(loadReleaseManifest({ ...reference, fetch: async () => ({ ok: true, url: 'https://untrusted.test/redirect', arrayBuffer() { throw new Error('must not read') } }) }), /official source/)
})

test('hash-pinned manifest uses browser cache and retries a fetch-stage CORS TypeError once without cache', async () => {
  const f = await fixture(), reference = await f.reference(), caches = []
  const result = await loadReleaseManifest({ ...reference, fetch: async (_url, options) => {
    caches.push(options.cache)
    if (caches.length === 1) throw new TypeError('Cached navigation response has no CORS headers')
    return reference.fetch()
  } })
  assert.equal(result.version, 'new')
  assert.deepEqual(caches, ['default', 'no-store'])
})
test('manifest cache recovery never retries HTTP, hash or metadata failures', async () => {
  const f = await fixture(), reference = await f.reference()
  for (const failure of ['http', 'body', 'hash', 'metadata']) {
    const caches = []
    let bytes
    if (failure === 'metadata') {
      f.manifest.pipelineId = 0
      bytes = new TextEncoder().encode(JSON.stringify(f.manifest))
    }
    await assert.rejects(loadReleaseManifest({ ...reference, sha256: bytes ? await firmwareSha256(bytes) : reference.sha256, fetch: async (_url, options) => {
      caches.push(options.cache)
      if (failure === 'body') return new Response(new ReadableStream({ start(controller) { controller.error(new TypeError('body read failed')) } }))
      return failure === 'http' ? new Response('', { status: 503 }) : new Response(bytes ?? '{}')
    } }))
    assert.deepEqual(caches, ['default'])
  }
})
test('manifest fetch TypeError recovery stops after one retry and does not retry after cancellation', async () => {
  const f = await fixture(), reference = await f.reference(), caches = []
  await assert.rejects(loadReleaseManifest({ ...reference, fetch: async (_url, options) => {
    caches.push(options.cache); throw new TypeError('CORS still unavailable')
  } }), /CORS still unavailable/)
  assert.deepEqual(caches, ['default', 'no-store'])
  const controller = new AbortController(), cancelledCalls = []
  await assert.rejects(loadReleaseManifest({ ...reference, signal: controller.signal, fetch: async (_url, options) => {
    cancelledCalls.push(options.cache); controller.abort(); throw new TypeError('request cancelled')
  } }))
  assert.deepEqual(cancelledCalls, ['default'])
})
