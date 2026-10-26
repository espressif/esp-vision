// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import * as api from '../src/index.js'
import { DeviceSession, EV_MUX_REQUIRED_FEATURES, EV_MUX_REQUIRED_CHANNELS, EV_MUX_DEFAULT_LIMITS } from '../src/device_session.js'
import { EvMuxParser, makeEvMuxFrame } from '../src/evmux.js'
import { EvRepl } from '../src/evrepl.js'

const capabilities = { ok: true, protocol: { evMuxVersion: 4 },
  features: [...EV_MUX_REQUIRED_FEATURES, 'debug.capture_frame', 'script.write', 'script.run', 'device.control'],
  channels: EV_MUX_REQUIRED_CHANNELS, limits: { ...EV_MUX_DEFAULT_LIMITS, txLargePayloadMax: 262144 },
  session: { policy: 'firstCome', leaseMs: 15000, keepaliveMs: 5000 } }
function peer({ silent = false, screen = false, vmReady = true, interactiveScripts = false } = {}) {
  let handlers, subscriptions = 0, disconnects = 0
  const requests = [], order = [], id = 'a'.repeat(32)
  const requestWaiters = []
  const stdout = payload => handlers?.data(makeEvMuxFrame({ channel: 'repl.stdout', type: 'data', sessionId: id, vmEpoch: 1, payload }))
  const recordRequest = request => {
    requests.push(request)
    for (let i = requestWaiters.length - 1; i >= 0; i -= 1) {
      if (requestWaiters[i].predicate(request)) {
        requestWaiters.splice(i, 1)[0].resolve(request)
      }
    }
  }
  const waitForRequest = predicate => {
    const existing = requests.find(predicate)
    return existing ? Promise.resolve(existing) : new Promise(resolve => requestWaiters.push({ predicate, resolve }))
  }
  const parser = new EvMuxParser({ filterFrame(metadata, bytes) {
    if (silent) return false
    const payload = metadata.channel.endsWith('.rpc') ? JSON.parse(new TextDecoder().decode(bytes)) : new TextDecoder().decode(bytes)
    recordRequest({ metadata, payload })
    if (metadata.channel === 'repl.stdin') {
      const startMarker = interactiveScripts && /~~EVSCRIPT-START:[A-Za-z0-9-]+~~/.exec(payload)?.[0]
      if (startMarker) stdout(`${payload}\r\n${startMarker}\r\n`)
      else stdout(`${payload}\r\n${payload.includes('EVIDE_BOOT_OK') ? 'EVIDE_BOOT_OK' : 'result'}\r\n>>> `)
      return false
    }
    if (!metadata.channel.endsWith('.rpc')) return false
    let result = { ok: true }
    if (metadata.method === 'hello') result = { ok: true, evMuxVersion: 4, deviceId: 'e8f60ae75642', hostname: 'esp-vision-test', board: 'TEST', firmware: { version: 'test' }, vmReady, session: { id, vmEpoch: 1, leaseMs: 15000, keepaliveMs: 5000 } }
    if (metadata.method === 'capabilities') result = { ...capabilities, features: [...capabilities.features, ...(screen ? ['debug.capture_screen'] : [])] }
    if (metadata.method === 'session.keepalive') result = { ok: true, sessionId: id, vmEpoch: 1 }
    if (metadata.method === 'debug.capture_frame' || (screen && metadata.method === 'debug.capture_screen')) {
      handlers.data(makeEvMuxFrame({ channel: 'debug.rpc', type: 'rsp', method: metadata.method, seq: metadata.seq, sessionId: id, vmEpoch: 1, contentType: 'image/jpeg', width: 2, height: 1, payload: new Uint8Array([255,216,0,255,217]) })); return false
    }
    handlers.data(makeEvMuxFrame({ channel: metadata.channel, type: 'rsp', method: metadata.method, seq: metadata.seq, ...((metadata.method !== 'capabilities' && (metadata.method !== 'hello' || payload.acquire)) ? {sessionId: id, vmEpoch: 1} : {}), payload: JSON.stringify(result) }))
    return false
  } })
  const transport = {
    subscribe(value) { handlers = value; subscriptions++; order.push('subscribe'); return () => { subscriptions--; order.push('unsubscribe') } },
    async connect() { order.push('connect') },
    async disconnect() { disconnects++; order.push('disconnect') },
    async writeBytes(bytes) { parser.feedBytes(bytes) },
  }
  return { transport, requests, order, stdout, waitForRequest, close: () => handlers.close(), get subscriptions() { return subscriptions }, get disconnects() { return disconnects } }
}

test('root exports only the application API; protocols remain explicit adapters', () => {
  assert.deepEqual(Object.keys(api).sort(), ['createDevice'])
  const device = api.createDevice()
  assert.equal(device.session, undefined)
  assert.equal(device.repl, undefined)
  assert.equal(device.rpc, undefined)
  assert.deepEqual(Object.keys(device.python).sort(), ['execute', 'input'])
  assert.deepEqual(Object.keys(device.files).sort(), ['list','mkdir','read','remove','write'])
})

test('facade subscribes before connection, negotiates, streams input and disposes exactly once', async () => {
  const p = peer(), d = api.createDevice({ transport: p.transport }), output = []
  const unsubscribe = d.on('python.output', value => output.push(value))
  const ready = await d.connect({ role: 'tcp' })
  assert.equal(ready.identity.deviceId, 'e8f60ae75642')
  assert.deepEqual(p.order.slice(0, 2), ['subscribe','connect'])
  assert.equal(p.subscriptions, 1)
  assert.ok(p.requests.some(x => x.metadata.channel === 'repl.signal'), 'default connection retains the REPL readiness probe')
  assert.ok(p.requests.some(x => x.metadata.channel === 'repl.stdin' && x.payload.includes('EVIDE_BOOT_OK')))
  await d.python.input('print(2)\r')
  assert.ok(output.some(text => text.includes('result')))
  unsubscribe(); const count = output.length; p.stdout('ignored\r\n'); assert.equal(output.length,count)
  await Promise.all([d.disconnect(), d.disconnect()])
  assert.equal(p.subscriptions,0); assert.equal(p.disconnects,1)
  assert.ok(p.requests.some(x => x.metadata.method === 'session.release'))
  await assert.rejects(d.files.read({path:'/x'}), {code:'DEVICE_NOT_READY'})
})

test('hard reset disposes an open transport without releasing its expired lease and permits reconnect', async () => {
  const p = peer(), d = api.createDevice({ transport: p.transport })
  try {
    await d.connect({ role: 'tcp', probeRepl: false })
    assert.deepEqual(await d.control({ action: 'reset', mode: 'hard' }), { ok: true })
    assert.equal(p.disconnects, 0, 'the transport remains open after the reset ACK')
    await d.disconnect()
    assert.equal(p.disconnects, 1)
    assert.equal(p.subscriptions, 0)
    assert.equal(p.requests.filter(request => request.metadata.method === 'session.release').length, 0)
    await d.connect({ role: 'tcp', probeRepl: false })
    await d.disconnect()
    assert.equal(p.disconnects, 2)
    assert.equal(p.requests.filter(request => request.metadata.method === 'session.release').length, 1,
      'normal disconnect still releases the new session')
  } finally { await d.disconnect() }
})

test('connect can acquire and inspect a session without interrupting the REPL', async () => {
  const p = peer({ vmReady: false }), d = api.createDevice({ transport: p.transport })
  try {
    const ready = await d.connect({ role: 'tcp', probeRepl: false })
    assert.equal(ready.runtime.vmReady, false, 'passive negotiation preserves the device readiness report')
    assert.ok(p.requests.some(x => x.metadata.method === 'hello' && x.payload.acquire === true))
    assert.ok(p.requests.some(x => x.metadata.method === 'capabilities'))
    assert.equal(p.requests.some(x => x.metadata.channel === 'repl.signal' || x.metadata.channel === 'repl.stdin'), false)
    await assert.rejects(d.python.execute({ source: 'print(1)' }), /requires a ready VM/)
    assert.equal(p.requests.some(x => x.metadata.channel === 'repl.signal' || x.metadata.channel === 'repl.stdin'), false)
  } finally { await d.disconnect() }
})

test('connect validates probeRepl before opening the transport', async () => {
  const p = peer(), d = api.createDevice({ transport: p.transport })
  for (const probeRepl of [null, 'false', 0]) {
    await assert.rejects(d.connect({ role: 'tcp', probeRepl }), { name: 'TypeError' })
  }
  assert.deepEqual(p.order, [])
  assert.deepEqual(p.requests, [])
})

test('capture returns binary bytes and unsupported LCD capture never captures the camera', async () => {
  const p = peer(), d = api.createDevice({transport:p.transport})
  try {
    await d.connect({role:'tcp'})
    const image = await d.capture({source:'camera',quality:70})
    assert.deepEqual(image.data,new Uint8Array([255,216,0,255,217]))
    assert.equal(image.mimeType,'image/jpeg')
    const count=p.requests.length
    await assert.rejects(d.capture({source:'screen'}),{code:'UNSUPPORTED_OPERATION'})
    assert.equal(p.requests.length,count)
  } finally {await d.disconnect()}
})

test('listener failures cannot break parser delivery and are diagnosed without leaking error text', async () => {
  const p=peer(),d=api.createDevice({transport:p.transport}),diagnostics=[]
  d.on('python.output',()=>{throw new Error('secret listener text')})
  d.on('diagnostic',value=>diagnostics.push(value))
  try {await d.connect({role:'tcp'});p.stdout('hello\r\n');assert.ok(diagnostics.some(x=>x.event==='listener.error'));assert.ok(!JSON.stringify(diagnostics).includes('secret listener text'))}finally{await d.disconnect()}
})

test('connection timeout and cancellation dispose reader and transport', async () => {
  const p=peer({silent:true}),d=api.createDevice({transport:p.transport})
  await assert.rejects(d.connect({role:'tcp',timeout:20}),{code:'TIMEOUT'})
  assert.equal(p.subscriptions,0);assert.equal(p.disconnects,1)
  const controller=new AbortController();controller.abort(new Error('cancelled'))
  await assert.rejects(d.connect({role:'tcp',signal:controller.signal}),/cancelled/)
  assert.equal(p.disconnects,1)
})

test('transport loss clears the connection and permits reconnection',async()=>{
  const p=peer(),d=api.createDevice({transport:p.transport})
  await d.connect({role:'tcp'});p.close();await d.disconnect()
  assert.equal(p.subscriptions,0)
  await d.connect({role:'tcp'});await d.disconnect();assert.equal(p.disconnects,2)
})

test('disconnect releases the session without interrupting a publicly started run', async () => {
  const p = peer({ interactiveScripts: true }), d = api.createDevice({ transport: p.transport })
  try {
    await d.connect({ role: 'tcp', probeRepl: false })
    const run = await d.python.execute({ path: '/main.py', completion: 'start' })
    const doneRejected = assert.rejects(run.done)
    const beforeDisconnect = p.requests.length
    await d.disconnect()
    await doneRejected
    const released = p.requests.slice(beforeDisconnect)
    assert.ok(released.some(request => request.metadata.method === 'session.release'))
    assert.equal(released.some(request => request.metadata.channel === 'repl.signal'
      || (request.metadata.method === 'device.control' && request.payload.action === 'stop')), false)
  } finally { await d.disconnect() }
})

test('caller AbortSignal still interrupts a publicly started run', async () => {
  const p = peer({ interactiveScripts: true }), d = api.createDevice({ transport: p.transport })
  const controller = new AbortController()
  try {
    await d.connect({ role: 'tcp', probeRepl: false })
    const run = await d.python.execute({ path: '/main.py', completion: 'start', signal: controller.signal })
    const doneRejected = assert.rejects(run.done, { name: 'AbortError' })
    const beforeAbort = p.requests.length
    controller.abort()
    await doneRejected
    assert.ok(p.requests.slice(beforeAbort).some(request => request.metadata.method === 'device.control'
      && request.payload.action === 'stop'))
  } finally { await d.disconnect() }
})

test('disconnect still interrupts Python execution in completion-wait mode', async () => {
  const p = peer(), d = api.createDevice({ transport: p.transport })
  try {
    await d.connect({ role: 'tcp', probeRepl: false })
    const execution = d.python.execute({ path: '/main.py' })
    await p.waitForRequest(request => request.metadata.channel === 'repl.stdin'
      && String(request.payload).includes('~~EVSCRIPT-END:'))
    const failed = assert.rejects(execution)
    const beforeDisconnect = p.requests.length
    await d.disconnect()
    await failed
    assert.ok(p.requests.slice(beforeDisconnect).some(request => request.metadata.method === 'device.control'
      && request.payload.action === 'stop'))
  } finally { await d.disconnect() }
})

test('discovery works before any transport is connected',async()=>{
  const d=api.createDevice({discovery:{async list(){return [{deviceId:'e8f60ae75642',hostname:'esp-vision-test',board:'TEST',firmwareVersion:'1',evMuxVersion:4,port:3232,addresses:['192.168.1.2']}]}}})
  assert.equal((await d.discover({timeout:50}))[0].compatible,true)
})

test('binary capture results correlate by sequence even when responses arrive in reverse order',async()=>{
  let s;const requests=[]
  const parser=new EvMuxParser({filterFrame(metadata){requests.push(metadata);return false}})
  s=new DeviceSession({transport:{async writeBytes(b){parser.feedBytes(b)}},EvMuxParser,EvRepl,makeEvMuxFrame})
  s.state='ready';s.sessionId='a'.repeat(32);s.vmEpoch=1;s.vmReady=true;s.capabilities={features:[]}
  try {
    const first=s.captureImage('debug.capture_frame'),second=s.captureImage('debug.capture_frame')
    first.catch(()=>{});second.catch(()=>{})
    await new Promise(setImmediate)
    for(const [index,payload] of [[1,22],[0,11]]){const req=requests[index];s.feedBytes(makeEvMuxFrame({channel:'debug.rpc',type:'rsp',method:req.method,seq:req.seq,sessionId:s.sessionId,vmEpoch:1,contentType:'image/jpeg',payload:new Uint8Array([payload])}))}
    assert.deepEqual((await first).data,new Uint8Array([11]));assert.deepEqual((await second).data,new Uint8Array([22]))
  }finally{s.close()}
})

test('long Unicode paths stay within REPL budget for directory reads and inline execution',async()=>{
  const repl=new EvRepl({writeEvmux(){}}),path='/'+ 'é'.repeat(61),commands=[]
  repl.execPy=async source=>{commands.push(source);assert.ok(repl.execInputBytes(source)<=511);return source.includes('__EVLIST__')?'__EVLIST__[]\n':''}
  assert.deepEqual(await repl.listFs(path),[])
  assert.equal(commands.length,2,'Long escaped paths are assigned separately from directory code')
  const command=repl.createScriptRunCommand(path,{acknowledgeStart:true})
  assert.ok(new TextEncoder().encode(command).length<=511)
  assert.match(command,/EVSCRIPT-START:/)
  if(repl.scriptRunPrelude)assert.ok(repl.execInputBytes(repl.scriptRunPrelude)<=511)
  repl.cancelScriptEndSentinel()
})

test('inspection snapshots cannot change internal capabilities',async()=>{
  const p=peer(),d=api.createDevice({transport:p.transport})
  try{
    await d.connect({role:'tcp'})
    const value=await d.inspect({fields:['runtime']});value.runtime.capabilities.features.length=0
    const image=await d.capture({source:'camera'});assert.equal(image.mimeType,'image/jpeg')
  }finally{await d.disconnect()}
})

test('file read and removal keep the longest escaped paths below REPL input limits',async()=>{
  const repl=new EvRepl({writeEvmux(){}}),path='/'+ 'é'.repeat(61),sources=[]
  repl._execPy=async source=>{
    sources.push(source);assert.ok(repl.execInputBytes(source)<=511)
    return source.includes('__EVREAD_OK__')?'__EVREAD_OK__1:1:ff\n':source.includes("'ok':True")?'true\n'+JSON.stringify({ok:true}):''
  }
  assert.deepEqual(await repl.readFile(path),new Uint8Array([255]))
  await repl.removeFile(path);await repl.removeDir(path)
  assert.ok(sources.some(s=>s.includes('__evcore_read_path=')))
  assert.ok(sources.some(s=>s.includes('__evcore_fs_path=')))
})

test('LCD capture uses its advertised screen method and preserves dimensions and quality', async () => {
  const p = peer({ screen: true }), d = api.createDevice({ transport: p.transport })
  try {
    await d.connect({ role: 'tcp' })
    const image = await d.capture({ source: 'screen', format: 'jpeg', quality: 85 })
    assert.equal(image.mimeType, 'image/jpeg')
    assert.equal(image.metadata.width, 2)
    assert.equal(image.metadata.height, 1)
    assert.deepEqual(image.data, new Uint8Array([255,216,0,255,217]))
    const request = p.requests.find(x => x.metadata.method === 'debug.capture_screen')
    assert.deepEqual(request.payload, { quality: 85 })
    assert.equal(p.requests.some(x => x.metadata.method === 'debug.capture_frame'), false)
  } finally { await d.disconnect() }
})

test('firmware facade trusts connected identity and releases EV-MUX before opening ROM', async () => {
  const { firmwareSha256 } = await import('../src/firmware_flash.js')
  const p = peer(), device = api.createDevice({ transport: p.transport })
  const ready = await device.connect({ role: 'tcp' }), data = new Uint8Array([0xe9, 1])
  const release = { board: ready.identity.board, version: 'next', image: { evMuxVersion: 4, chipFamily: 1, chipRevision: 0, offset: 65536, bytes: 2, partitionSize: 4096, sha256: await firmwareSha256(data), partitionTable: { offset: 32768, bytes: 1, sha256: '0'.repeat(64) } } }
  const result = await device.firmware.check({ catalog: { schemaVersion: 1, releases: [release] }, identity: { board: 'OTHER', firmwareVersion: 'wrong' } })
  assert.equal(result.available, true); assert.equal(result.currentVersion, ready.identity.firmwareVersion)
  await assert.rejects(device.firmware.update({ release, data, async openLoader() {
    assert.equal(p.disconnects, 1)
    assert.equal(p.subscriptions, 0)
    throw new Error('expected port-selection failure')
  } }), /expected port-selection failure/)
  await assert.rejects(device.inspect(), { code: 'DEVICE_NOT_READY' })
})
