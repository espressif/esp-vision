// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'
import test from 'node:test'
import { createFilesApi } from '../src/device_files.js'
import { EvRepl, evReplExecInputBytes } from '../src/evrepl.js'

function fixture() {
  const calls = [], state = {}
  const session = {
    repl: {
      listFs: async (path, options) => {
        calls.push(['listFs', path, options])
        return [{ name: 'a', path: `${path === '/' ? '' : path}/a`, type: 'file', size: 10 }]
      },
      readFile: async (...args) => { calls.push(['read', ...args]); return new TextEncoder().encode('你好') },
      removeFile: async (...args) => calls.push(['removeFile', ...args]),
      removeDir: async (...args) => calls.push(['removeDir', ...args]),
      execPy: async (...args) => { calls.push(['execPy', ...args]); return '__EVMK__ 0\n' },
    },
    writeFile: async (...args) => { calls.push(['write', ...args]); return { ok: true } },
  }
  return { files: createFilesApi({ session, state }), session, calls, state }
}

test('file upload selects encoding and creation policy from public parameters', async () => {
  const { files, calls } = fixture()
  const onProgress = () => {}, signal = new AbortController().signal
  await files.write({ path: '/model.bin', data: new Uint8Array([0, 255]), overwrite: false, onProgress, signal })
  assert.deepEqual(calls[0], ['write', '/model.bin', new Uint8Array([0, 255]), { encoding: 'binary', mode: 'create', onProgress, signal }])
  await files.write({ path: '/main.py', data: 'print(1)' })
  assert.equal(calls[1][3].encoding, 'utf-8')
  assert.equal(calls[1][3].mode, 'overwrite')
})

test('file read is binary by default with explicit UTF-8 decoding and bounded options', async () => {
  const { files, calls } = fixture()
  assert.ok(await files.read({ path: '/model.bin' }) instanceof Uint8Array)
  assert.deepEqual(calls[0][2], { timeout: null, chunkTimeout: 30000, maxBytes: null, signal: null })
  assert.equal(await files.read({ path: '/a.txt', encoding: 'utf-8', maxBytes: 512 }), '你好')
  assert.deepEqual(calls[1][2], { timeout: null, chunkTimeout: 30000, maxBytes: 512, signal: null })
})

test('files.read forwards aggregate and per-command timeouts and validates both', async () => {
  const { files, calls } = fixture()
  await files.read({ path: '/a', timeout: 90000, chunkTimeout: 4500, maxBytes: 2048 })
  assert.deepEqual(calls[0][2], { timeout: 90000, chunkTimeout: 4500, maxBytes: 2048, signal: null })
  await files.read({ path: '/a', timeout: null, chunkTimeout: 12000 })
  assert.deepEqual(calls[1][2], { timeout: null, chunkTimeout: 12000, maxBytes: null, signal: null })
  for (const timeout of [0, -1, NaN, Infinity, '100']) {
    await assert.rejects(files.read({ path: '/a', timeout }), RangeError)
  }
  for (const chunkTimeout of [null, 0, -1, NaN, Infinity, '100']) {
    await assert.rejects(files.read({ path: '/a', chunkTimeout }), RangeError)
  }
  assert.equal(calls.length, 2, 'invalid timeout settings must fail before device I/O')
})

function installSimulatedFileRead(repl, data, { commandDurationMs, advanceClock }) {
  const budgets = []
  repl._execPy = async (source, timeout) => {
    budgets.push(timeout)
    if (commandDurationMs > timeout) {
      advanceClock(timeout)
      throw Object.assign(new Error('repl exec timeout'), { code: 'TIMEOUT' })
    }
    advanceClock(commandDurationMs)
    const offset = Number(/f.seek\((\d+)\)/.exec(source)[1])
    const count = Number(/f.read\((\d+)\)/.exec(source)[1])
    const chunk = data.slice(offset, offset + count)
    const hex = [...chunk].map(byte => byte.toString(16).padStart(2, '0')).join('')
    return `__EVREAD_OK__${data.length}:${chunk.length}:${hex}\n`
  }
  return budgets
}

test('a 56 KiB file read can take longer than 30 seconds with a fresh bound per REPL command', async () => {
  const repl = new EvRepl({ writeEvmux() {} })
  const data = Uint8Array.from({ length: 56 * 1024 }, (_, index) => index % 256)
  const originalNow = Date.now
  let now = 100000
  Date.now = () => now
  try {
    const budgets = installSimulatedFileRead(repl, data, {
      commandDurationMs: 1000,
      advanceClock: duration => { now += duration },
    })
    assert.deepEqual(await repl.readFile('/slow.bin'), data)
    assert.equal(now - 100000, 56000)
    assert.equal(budgets.length, 56)
    assert.ok(budgets.every(timeout => timeout === 30000))
  } finally {
    Date.now = originalNow
  }
})

test('file reads keep per-command bounds and an optional aggregate deadline', async () => {
  const data = Uint8Array.from({ length: 4096 }, (_, index) => index % 256)
  const originalNow = Date.now
  let now = 200000
  Date.now = () => now
  try {
    const chunkedRepl = new EvRepl({ writeEvmux() {} })
    const chunkBudgets = installSimulatedFileRead(chunkedRepl, data, {
      commandDurationMs: 4000,
      advanceClock: duration => { now += duration },
    })
    assert.deepEqual(await chunkedRepl.readFile('/slow.bin', { timeout: 20000, chunkTimeout: 5000 }), data)
    assert.deepEqual(chunkBudgets, [5000, 5000, 5000, 5000])

    now = 300000
    const totalTimeoutRepl = new EvRepl({ writeEvmux() {} })
    const totalBudgets = installSimulatedFileRead(totalTimeoutRepl, data, {
      commandDurationMs: 8000,
      advanceClock: duration => { now += duration },
    })
    await assert.rejects(
      totalTimeoutRepl.readFile('/slow.bin', { timeout: 25000, chunkTimeout: 30000 }),
      { code: 'TIMEOUT' },
    )
    assert.deepEqual(totalBudgets, [25000, 17000, 9000, 1000])
  } finally {
    Date.now = originalNow
  }
})

test('file listing passes scoped path and recursion without walking the entire device', async () => {
  const { files, calls } = fixture()
  assert.deepEqual(await files.list({ path: '/folder' }), [{ name: 'a', path: '/folder/a', type: 'file', size: 10 }])
  assert.deepEqual(calls[0], ['listFs', '/folder', { recursive: false, signal: undefined }])
  await files.list({ recursive: true })
  assert.equal(calls[1][2].recursive, true)
})

test('REPL directory listing confines recursion to selected subtree and validates missing directories', async () => {
  const repl = new EvRepl({ writeEvmux() {} })
  const seen = []
  repl.execPy = async source => {
    const path = /p=("[^"\n]*")/.exec(source)[1]
    seen.push(JSON.parse(path))
    return '__EVLIST__' + JSON.stringify(seen.length === 1 ? [['deep', 0x4000, 0], ['a.bin', 0x8000, 12]] : [['b.bin', 0x8000, 20]])
  }
  assert.equal((await repl.listFs('/models')).length, 2)
  assert.deepEqual(seen, ['/models'])
  seen.length = 0
  const entries = await repl.listFs('/models', { recursive: true })
  assert.deepEqual(seen, ['/models', '/models/deep'])
  assert.equal(entries[2].path, '/models/deep/b.bin')
  repl.execPy = async () => '__EVLIST__2'
  await assert.rejects(repl.listFs('/missing'), { code: 'NOT_FOUND', errno: 2 })
  repl.execPy = async () => '__EVLIST__20'
  await assert.rejects(repl.listFs('/models/a.bin'), { code: 'NOT_DIRECTORY' })
})

test('recursive listing cancellation prevents traversal of the next directory', async () => {
  const repl = new EvRepl({ writeEvmux() {} })
  const controller = new AbortController()
  let calls = 0
  repl.execPy = async (_source, { signal }) => {
    assert.equal(signal, controller.signal)
    calls++
    controller.abort()
    return '__EVLIST__[["deep",16384,0]]'
  }
  await assert.rejects(repl.listFs('/models', { recursive: true, signal: controller.signal }), { name: 'AbortError' })
  assert.equal(calls, 1)
})

test('delete explicitly distinguishes file and directory and forwards cancellation', async () => {
  const { files, calls } = fixture()
  const signal = new AbortController().signal
  await files.remove({ path: '/a', signal })
  await files.remove({ path: '/b', type: 'directory', signal })
  assert.deepEqual(calls, [['removeFile', '/a', { signal }], ['removeDir', '/b', { signal }]])
})

test('directory creation honors recursive mode and surfaces filesystem errors', async () => {
  const { files, calls, session } = fixture()
  await files.mkdir({ path: '/a/b', recursive: true })
  assert.equal(calls.length, 2)
  assert.match(calls[0][1], /p="\/a"/)
  assert.match(calls[1][1], /p="\/a\/b"/)
  assert.match(calls[0][1], /os.stat\(p\)\[0\]&16384/)
  session.repl.execPy = async () => '__EVMK__ 28'
  await assert.rejects(files.mkdir({ path: '/a' }), { code: 'FILESYSTEM_ERROR', errno: 28 })
})

test('rejects dangerous paths, unsupported data, mode confusion and pre-cancellation before writes', async () => {
  const { files, calls } = fixture()
  for (const path of ['relative', '/', '/a/../b', '/a//b', '/a\0b', '/a"b', '/a\\b', '/' + '中'.repeat(42)]) {
    await assert.rejects(files.write({ path, data: 'x' }), { code: 'INVALID_PATH' })
  }
  for (const data of [null, {}, [], 12]) await assert.rejects(files.write({ path: '/a', data }), TypeError)
  await assert.rejects(files.write({ path: '/a', data: '', overwrite: 'yes' }), TypeError)
  await assert.rejects(files.write({ path: '/a', data: '', runAfterWrite: true }), TypeError)
  await assert.rejects(files.write({ path: '/a', data: '', signal: AbortSignal.abort() }), { name: 'AbortError' })
  assert.equal(calls.length, 0)
})

test('shared lock excludes concurrent filesystem work and releases after failure', async () => {
  const { files, session, state } = fixture()
  let fail
  session.writeFile = async () => new Promise((_resolve, reject) => { fail = reject })
  const writing = files.write({ path: '/a', data: 'x' })
  await assert.rejects(files.read({ path: '/a' }), { code: 'DEVICE_BUSY' })
  fail(new Error('disk full'))
  await assert.rejects(writing, /disk full/)
  assert.equal(state.operation, null)
  await files.read({ path: '/a' })
})

test('every file operation rejects unknown fields and invalid signals before device I/O', async () => {
  const { files, calls } = fixture()
  for (const [method, options] of [
    ['list', { path: '/' }], ['read', { path: '/a' }], ['write', { path: '/a', data: 'x' }],
    ['remove', { path: '/a' }], ['mkdir', { path: '/a' }],
  ]) {
    await assert.rejects(files[method]({ ...options, typo: true }), /Unknown files\./)
    await assert.rejects(files[method]({ ...options, signal: {} }), TypeError)
  }
  for (const maxBytes of [-1, 0.5, '1', 16 * 1024 * 1024 + 1]) await assert.rejects(files.read({ path: '/a', maxBytes }), RangeError)
  assert.equal(calls.length, 0)
})

test('file paths and caller byte buffers are snapshotted before asynchronous writing', async () => {
  const { files, session } = fixture()
  let release, observed, reads = 0
  session.writeFile = async (path, data) => {
    await new Promise(resolve => { release = resolve })
    observed = { path, bytes: Array.from(data) }
  }
  // Buffer.slice() shares backing memory; the API must copy all Uint8Array subclasses.
  const data = Buffer.from([0, 127, 255])
  const request = { get path() { reads++; return reads === 1 ? '/safe.bin' : '/other.bin' }, data }
  const writing = files.write(request)
  data.fill(1)
  release()
  await writing
  assert.equal(reads, 1)
  assert.deepEqual(observed, { path: '/safe.bin', bytes: [0, 127, 255] })
})


test('recursive mkdir keeps longest legal Unicode paths inside the real REPL input budget', async () => {
  const { files, session } = fixture()
  const commands = []
  session.repl.execPy = async source => {
    commands.push(source)
    assert.ok(evReplExecInputBytes(source) <= 511, `REPL input is ${evReplExecInputBytes(source)} bytes`)
    return '__EVMK__ 0'
  }
  await files.mkdir({ path: '/' + '中'.repeat(40), recursive: true })
  await files.mkdir({ path: '/' + 'a'.repeat(59) + '/' + '中'.repeat(20), recursive: true })
  assert.equal(commands.length, 3)
  await files.mkdir({ path: '/' + 'é'.repeat(61), recursive: true })
  assert.equal(commands.length, 5, 'two-byte Unicode paths split assignment from the fixed mkdir command')
})

test('recursive mkdir checks existing entry type and preserves errors for files', async () => {
  const { files, session } = fixture()
  const commands = []
  session.repl.execPy = async source => { commands.push(source); return '__EVMK__ 0' }
  await files.mkdir({ path: '/existing/child', recursive: true })
  assert.match(commands[0], /if r==17:/)
  assert.match(commands[0], /0 if os.stat\(p\)\[0\]&16384 else 20/)
  session.repl.execPy = async () => '__EVMK__ 20'
  await assert.rejects(files.mkdir({ path: '/existing-file/child', recursive: true }), { code: 'FILESYSTEM_ERROR', errno: 20 })
  session.repl.execPy = async source => {
    assert.ok(!source.includes('if r==17:'))
    return '__EVMK__ 17'
  }
  await assert.rejects(files.mkdir({ path: '/already-exists' }), { code: 'FILESYSTEM_ERROR', errno: 17 })
})

test('recursive mkdir cancellation prevents creating the next path segment', async () => {
  const { files, session } = fixture()
  const controller = new AbortController()
  let calls = 0
  session.repl.execPy = async (_source, { signal }) => {
    assert.equal(signal, controller.signal)
    calls++
    controller.abort()
    return '__EVMK__ 0'
  }
  await assert.rejects(files.mkdir({ path: '/parent/child', recursive: true, signal: controller.signal }), { name: 'AbortError' })
  assert.equal(calls, 1)
})
