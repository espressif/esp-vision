import assert from 'node:assert/strict'
import { test } from 'node:test'
import { FirmwareFlashSession, firmwareSha256, validateFirmwareImage } from '../src/firmware_flash.js'
async function fixture() {
  const bytes = new Uint8Array([0xe9, 1, 2, 3]), table = new Uint8Array([4, 5, 6]), calls = []
  const image = { evMuxVersion: 4, chipFamily: 0x3280, chipRevision: 302, offset: 0x10000, bytes: 4, partitionSize: 0x700000, sha256: await firmwareSha256(bytes), partitionTable: { offset: 0x8000, bytes: 3, sha256: await firmwareSha256(table) } }
  const stub = { async readFlash(offset) { calls.push(['read', offset]); return offset === 0x8000 ? table : bytes }, async readRegister() { return 0x1dfbc7f8 }, async writeRegister() { throw new Error('unexpected register write') }, async flashData(_data, progress, offset, compress) { calls.push(['write', offset, compress]); progress(4, 4) } }
  const loader = { chipFamily: image.chipFamily, chipRevision: image.chipRevision, async initialize() { calls.push(['initialize']) }, async getSecurityInfo() { return { flags: 0, flashCryptCnt: 0 } }, async runStub(skip) { calls.push(['stub', skip]); return stub }, async readRegister() { return 0x1dfbc7f8 }, async writeRegister() { throw new Error('unexpected register write') }, async hardReset() { calls.push(['reset']) }, async disconnect() { calls.push(['close']) } }
  return { bytes, table, image, calls, loader, stub, session: new FirmwareFlashSession(loader, image) }
}
test('only exact approved v4 images pass before touching the ROM loader', async () => {
  const f = await fixture()
  await assert.rejects(validateFirmwareImage({ ...f.image, evMuxVersion: 3 }, f.bytes), /v4/)
  await assert.rejects(f.session.connect(new Uint8Array([0xe9, 1, 2, 0])), /SHA-256/)
  assert.deepEqual(f.calls, [['close']])
})
test('wrong chip/revision and partition layout fail before any flash write', async () => {
  for (const change of ['revision', 'partition']) {
    const f = await fixture()
    if (change === 'revision') f.loader.chipRevision = 301
    else f.image.partitionTable.sha256 = '0'.repeat(64)
    await assert.rejects(f.session.connect(f.bytes), change === 'revision' ? /revision/ : /Partition layout/)
    assert.ok(!f.calls.some(([operation]) => operation === 'write'))
    assert.equal(f.calls.at(-1)[0], 'close')
  }
})
test('application-only write is read back in full before reset, with port cleanup', async () => {
  const f = await fixture()
  await f.session.connect(f.bytes)
  await f.session.flash(f.bytes)
  assert.deepEqual(f.calls, [['initialize'], ['stub', true], ['read', 0x8000], ['write', 0x10000, false], ['read', 0x10000], ['reset'], ['close']])
})
test('corrupt readback never reports success or resets into unverified firmware', async () => {
  const f = await fixture()
  await f.session.connect(f.bytes)
  f.stub.readFlash = async () => new Uint8Array([9, 9, 9, 9])
  await assert.rejects(f.session.flash(f.bytes), /verification failed/)
  assert.ok(!f.calls.some(([operation]) => operation === 'reset'))
  assert.equal(f.calls.at(-1)[0], 'close')
})
test('reset failure preserves verified result and asks for manual reset', async () => {
  const f = await fixture(), messages = []
  await f.session.connect(f.bytes)
  f.loader.hardReset = async () => { throw new Error('reset unavailable') }
  await f.session.flash(f.bytes, { onPhase: message => messages.push(message) })
  assert.match(messages.at(-1), /verified.*RESET/)
})
test('full verification uses bounded 1 KiB packets and a single outstanding packet', async () => {
  const f = await fixture(), bytes = new Uint8Array(65537), reads = []
  f.image.bytes = bytes.length; f.image.sha256 = await firmwareSha256(bytes)
  f.stub.readFlash = async (offset, count, _progress, options) => {
    if (offset === 0x8000) return f.table
    reads.push({ offset, count, options })
    return bytes.slice(offset - f.image.offset, offset - f.image.offset + count)
  }
  await f.session.connect(bytes)
  await f.session.flash(bytes)
  assert.deepEqual(reads.map(item => item.count), [65536, 1])
  for (const read of reads) assert.deepEqual(read.options, { chunkSize: 65536, blockSize: 1024, maxInFlight: 1 })
})

test('unsigned application updates refuse secure boot and flash encryption before loading a stub', async () => {
  for (const security of [{ flags: 1, flashCryptCnt: 0 }, { flags: 0, flashCryptCnt: 1 }]) {
    const f = await fixture()
    f.loader.getSecurityInfo = async () => security
    await assert.rejects(f.session.connect(f.bytes), /signed flashing workflow/)
    assert.deepEqual(f.calls, [['initialize'], ['close']])
  }
})


test('P4 software download latch is cleared with a mask and verified before reset', async () => {
  const f = await fixture(), registers = []; let register = 0x1dfbc7fc
  f.stub.readRegister = async address => { registers.push(['read', address]); return register }
  f.stub.writeRegister = async (address, value, mask) => { registers.push(['write', address, value, mask]); register &= ~mask }
  f.loader.hardReset = async () => { assert.equal(register, 0x1dfbc7f8); f.calls.push(['reset']) }
  await f.session.connect(f.bytes); await f.session.flash(f.bytes)
  assert.equal(register, 0x1dfbc7f8)
  assert.deepEqual(registers, [['read', 0x50110008], ['write', 0x50110008, 0, 4], ['read', 0x50110008]])
  assert.ok(f.calls.some(([operation]) => operation === 'reset'))
})
test('P4 latch clear failure does not reset and retains manual recovery guidance', async () => {
  const f = await fixture(), messages = []
  f.stub.readRegister = async () => 0x1dfbc7fc
  f.stub.writeRegister = async () => {}
  await f.session.connect(f.bytes); await f.session.flash(f.bytes, { onPhase: message => messages.push(message) })
  assert.ok(!f.calls.some(([operation]) => operation === 'reset'))
  assert.match(messages.at(-1), /verified.*RESET/)
})
