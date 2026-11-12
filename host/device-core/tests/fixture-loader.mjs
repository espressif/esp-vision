// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { createHash } from 'node:crypto'
import { evReplExecInputBytes } from '../src/evrepl.js'

const MARKER = '__EVHILFIXTURE__'
const failure = (code, message) => Object.assign(new Error(message), { code })

// This is an application fixture transfer, not a transport. Each attempt enters
// the public Python API afresh, including its interrupt/synchronization path.
export async function loadFixture(source, { namespace, nonce, inputLimit, execute, signal, report }) {
  const started = performance.now()
  let lastSuccessfulOperation = null, retries = 0
  const scoped = body => `exec(${JSON.stringify(body)},${namespace})`
  const ack = (phase, expression) => `print('${MARKER}'+__import__('json').dumps([_fixture_nonce,'${phase}',${expression}]))`
  const cancelled = () => {
    if (signal?.aborted) throw signal.reason instanceof Error ? signal.reason : failure('CANCELLED', 'Fixture bootstrap cancelled')
  }
  const operation = async (phase, commands, expected, details = {}) => {
    const operationStarted = performance.now()
    for (let attempt = 1; ; attempt++) {
      let commandIndex = 0
      try {
        let output
        for (; commandIndex < commands.length; commandIndex++) {
          cancelled()
          const command = commands[commandIndex]
          if (evReplExecInputBytes(command) > inputLimit) throw failure('REPL_INPUT_LIMIT', 'Device REPL input limit cannot load hardware fixtures')
          output = await execute(command)
          cancelled()
        }
        let reply
        try {
          const line = output.trim()
          if (!line.startsWith(MARKER)) throw new Error()
          reply = JSON.parse(line.slice(MARKER.length))
        } catch { throw failure('FIXTURE_ACK_INVALID', 'Missing or malformed fixture acknowledgement') }
        if (!Array.isArray(reply) || reply.length !== 3 || reply[0] !== nonce || reply[1] !== phase || JSON.stringify(reply[2]) !== JSON.stringify(expected)) {
          throw failure('FIXTURE_ACK_INVALID', 'Fixture acknowledgement does not match the session or content')
        }
        lastSuccessfulOperation = { phase, ...details, attempt, elapsedMs: Math.round(performance.now() - operationStarted) }
        return
      } catch (error) {
        const failedCommand = Math.min(commandIndex, commands.length - 1)
        const diagnostics = { phase, ...details, attempt, commandIndex: failedCommand, inputBytes: evReplExecInputBytes(commands[failedCommand]), elapsedMs: Math.round(performance.now() - operationStarted), totalElapsedMs: Math.round(performance.now() - started), lastSuccessfulOperation }
        if (signal?.aborted || error.code !== 'TIMEOUT' || attempt === 3) {
          error.bootstrap = diagnostics
          throw error
        }
        retries++
        report({ check: 'fixture-bootstrap', event: 'retry', diagnostics })
      }
    }
  }

  // Initialization is replayed only before any source or hardware state exists.
  await operation('init', [
    `${namespace}={'_source':{},'_fixture_nonce':${JSON.stringify(nonce)},'_state':0}`,
    scoped(ack('init', 'len(_source)')),
  ], 0)

  const chunkCommand = (offset, chunk) => scoped(ack('chunk', `[${offset},_source.setdefault(${offset},${JSON.stringify(chunk)})]`))
  let chunk = '', offset = 0, chunkIndex = 0
  const sendChunk = async () => {
    const bytes = Buffer.byteLength(chunk)
    // setdefault never overwrites a committed chunk. Its returned content is
    // acknowledged, so identical replays succeed and conflicting data fails.
    await operation('chunk', [chunkCommand(offset, chunk)], [offset, chunk], { chunkIndex, offset, bytes })
    offset += bytes
    chunkIndex++
    chunk = ''
  }
  for (const character of source) {
    if (chunk && evReplExecInputBytes(chunkCommand(offset, chunk + character)) > inputLimit) await sendChunk()
    chunk += character
  }
  if (chunk) await sendChunk()

  const bytes = Buffer.byteLength(source), sha256 = createHash('sha256').update(source).digest('hex')
  await operation('assemble', [
    scoped("_text=''.join(_source[i] for i in sorted(_source))"),
    scoped(ack('assemble', 'len(_text.encode())')),
  ], bytes, { bytes })
  await operation('verify', [
    scoped("_digest=__import__('binascii').hexlify(__import__('hashlib').sha256(_text.encode()).digest()).decode()"),
    scoped(ack('verify', '_digest')),
  ], sha256, { bytes })
  await operation('install', [
    // A lost response after state=2 is recoverable. An interrupted installation
    // left in state=1 is deliberately not executed again: it may be partial.
    scoped('if _state==0:\n _state=1\n exec(_text,globals())\n _state=2\nassert _state==2'),
    scoped("_ready=_state==2 and all(callable(globals().get(k)) for k in ('run_check','close_display','close_model','model_command'))"),
    scoped("assert _state==2;_source.clear();globals().pop('_text',None)"),
    scoped(ack('install', "[_digest,_ready,len(_source),'_text' in globals()]")),
  ], [sha256, true, 0, false], { bytes })
  report({ check: 'fixture-bootstrap', event: 'ready', bytes, sha256, chunks: chunkIndex, retries, elapsedMs: Math.round(performance.now() - started) })
}
