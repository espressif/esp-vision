/*
 * SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 *
 * Framed friendly-REPL client over EV-MUX repl channels.
 *
 * With boot-default EV-MUX firmware there is no raw REPL bootstrap: the
 * device speaks frames from power-on, and the friendly REPL is reachable
 * through repl.stdin / repl.stdout frames. This module executes Python
 * snippets over that channel and resolves their stdout, so device info,
 * the file tree and all filesystem operations (read/write/create/delete)
 * run framed, with no raw REPL access anywhere.
 */

const PROMPT = '>>> '
// The firmware ring has 512 slots and keeps one slot empty to distinguish
// full from empty. Include CRLF in this limit so an oversized friendly-REPL
// line fails locally instead of being truncated into a target-side hang.
export const EV_REPL_INPUT_MAX_BYTES = 511
// Firmware keeps 512 bytes for framed REPL input. A file chunk is hex-encoded
// inside exec("f.write(u('...'))"), so 224 source bytes leave room for the
// command wrapper and CRLF instead of overflowing the target ring buffer.
export const EV_REPL_FILE_CHUNK_BYTES = 224

function pythonString(value) {
    // JSON string literals are valid Python string literals and correctly
    // preserve quotes, backslashes and Unicode in device paths.
    return JSON.stringify(String(value))
}

function escapeForExec(source) {
    return source
        .replace(/\\/g, '\\\\')
        .replace(/"/g, '\\"')
        .replace(/\r/g, '')
        .replace(/\n/g, '\\n')
        // Friendly REPL discards non-ASCII keystrokes. Escape code points in
        // the outer Python literal so exec still receives the original source.
        .replace(/[^\x20-\x7e]/gu, char => {
            const code = char.codePointAt(0)
            return code <= 0xffff
                ? `\\u${code.toString(16).padStart(4, '0')}`
                : `\\U${code.toString(16).padStart(8, '0')}`
        })
}

export function evReplExecInputBytes(source) {
    const command = `exec("${escapeForExec(source)}")\r\n`
    return new TextEncoder().encode(command).length
}

function abortReason(signal) {
    if (signal?.reason instanceof Error) {
        return signal.reason
    }
    const error = new Error(signal?.reason ? String(signal.reason) : 'REPL operation cancelled')
    error.name = 'AbortError'
    return error
}

function throwIfAborted(signal) {
    if (signal?.aborted) {
        throw abortReason(signal)
    }
}

export class EvRepl {
    /**
     * @param {object} io
     * @param {(channel: string, type: string, method: string|null, payload: string|Uint8Array) => Promise<unknown>} io.writeEvmux
     */
    constructor(io) {
        this.writeEvmux = io.writeEvmux
        this.buffer = ''
        this.waiters = []
        this.onScriptEnd = null
        this.scriptStartMarker = null
        this.scriptStartSeen = false
        this.scriptStartWaiter = null
        this.scriptEndMarker = null
        this.scriptEndCommand = null
        this.scriptSentinelSeq = 0
        this.scriptEndPromptOffset = null
        this.execQueue = Promise.resolve()
        this.closedError = null
        this.inputMaxBytes = EV_REPL_INPUT_MAX_BYTES
    }

    execInputBytes(source) {
        return evReplExecInputBytes(source)
    }

    createScriptEndSentinel() {
        const randomId = globalThis.crypto?.randomUUID?.() ||
            `${Date.now().toString(36)}-${++this.scriptSentinelSeq}`
        this.scriptEndMarker = `~~EVSCRIPT-END:${randomId}~~`
        this.scriptEndCommand = `print(${pythonString(this.scriptEndMarker)})`
        return `${this.scriptEndCommand}\r\n`
    }

    createScriptStartSentinel() {
        const randomId = globalThis.crypto?.randomUUID?.() ||
            `${Date.now().toString(36)}-${++this.scriptSentinelSeq}`
        this.scriptStartMarker = `~~EVSCRIPT-START:${randomId}~~`
        this.scriptStartSeen = false
        return `print(${pythonString(this.scriptStartMarker)})`
    }

    waitForScriptStart({ timeout = 5000, signal = null } = {}) {
        if (!Number.isFinite(timeout) || timeout <= 0) {
            throw new RangeError('Script start acknowledgement timeout must be positive')
        }
        if (this.scriptStartSeen) {
            this.scriptStartSeen = false
            return Promise.resolve()
        }
        if (!this.scriptStartMarker) {
            return Promise.reject(Object.assign(new Error('Script start acknowledgement is unavailable'), {
                code: 'SCRIPT_START_MISSING',
            }))
        }
        if (signal?.aborted) {
            return Promise.reject(abortReason(signal))
        }
        if (this.scriptStartWaiter) return this.scriptStartWaiter.promise

        let waiter
        const promise = new Promise((resolve, reject) => {
            const cleanup = () => {
                clearTimeout(waiter.timer)
                signal?.removeEventListener('abort', onAbort)
                if (this.scriptStartWaiter === waiter) this.scriptStartWaiter = null
            }
            const settle = (callback, value) => {
                cleanup()
                callback(value)
            }
            const onAbort = () => settle(reject, abortReason(signal))
            waiter = {
                promise: null,
                resolve: () => {
                    this.scriptStartSeen = false
                    settle(resolve)
                },
                reject: error => settle(reject, error),
                timer: setTimeout(() => settle(reject, Object.assign(
                    new Error(`script start acknowledgement timeout after ${timeout}ms`),
                    { code: 'SCRIPT_START_TIMEOUT' },
                )), timeout),
            }
            this.scriptStartWaiter = waiter
            signal?.addEventListener('abort', onAbort, { once: true })
            if (signal?.aborted) onAbort()
        })
        waiter.promise = promise
        return promise
    }

    createScriptRunCommand(path, { acknowledgeStart = false } = {}) {
        const start = acknowledgeStart ? this.createScriptStartSentinel() : null
        const end = this.createScriptEndSentinel().trim()
        // Generate completion inside the running statement. A second queued
        // stdin command would otherwise be consumed by Python input().
        const source = target => `try:\n${start ? ` ${start}\n` : ''} execfile(${target})\nexcept BaseException as __evcore_error:\n __import__('sys').print_exception(__evcore_error)\nfinally:\n ${end}`
        let command = `exec("${escapeForExec(source(pythonString(path)))}")\r\n`
        this.scriptRunPrelude = null
        if (new TextEncoder().encode(command).length > this.inputMaxBytes) {
            this.scriptRunPrelude = `__evcore_script_path=${pythonString(path)}`
            command = `exec("${escapeForExec(source('__evcore_script_path'))}")\r\n`
        }
        if (new TextEncoder().encode(command).length > this.inputMaxBytes ||
                (this.scriptRunPrelude && this.execInputBytes(this.scriptRunPrelude) > this.inputMaxBytes)) {
            this.cancelScriptEndSentinel()
            throw new Error('Script runner command exceeds the REPL input limit')
        }
        this.scriptEndCommand = command.trimEnd()
        this.scriptRunEcho = true
        return command
    }

    createInlineSourceAssignments(source, variableName) {
        if (typeof source !== 'string' || !/^__[A-Za-z0-9_]+$/.test(variableName)) {
            throw new TypeError('Inline Python source requires a string and a private variable name')
        }
        const characters = Array.from(source)
        const statements = []
        let chunk = ''
        const statementFor = (value, first) => `${variableName}${first ? '=' : '+='}${JSON.stringify(value)}`
        for (const character of characters) {
            const candidate = chunk + character
            const candidateStatement = statementFor(candidate, statements.length === 0)
            if (this.execInputBytes(candidateStatement) <= this.inputMaxBytes) {
                chunk = candidate
                continue
            }
            if (!chunk) throw new Error('Inline Python source character exceeds the REPL input limit')
            statements.push(statementFor(chunk, statements.length === 0))
            chunk = character
            if (this.execInputBytes(statementFor(chunk, false)) > this.inputMaxBytes) {
                throw new Error('Inline Python source character exceeds the REPL input limit')
            }
        }
        if (chunk || statements.length === 0) statements.push(statementFor(chunk, statements.length === 0))
        return statements
    }

    createInlineScriptRunCommand(variableName) {
        if (!/^__[A-Za-z0-9_]+$/.test(variableName)) {
            throw new TypeError('Inline Python run requires a private variable name')
        }
        const end = this.createScriptEndSentinel().trim()
        const source = `try:\n exec(${variableName})\nexcept BaseException as __evcore_error:\n __import__('sys').print_exception(__evcore_error)\nfinally:\n try:\n  del ${variableName}\n except NameError:\n  pass\n ${end}`
        const command = `exec("${escapeForExec(source)}")\r\n`
        if (new TextEncoder().encode(command).length > this.inputMaxBytes) {
            this.cancelScriptEndSentinel()
            throw new Error('Inline Python runner exceeds the REPL input limit')
        }
        this.scriptEndCommand = command.trimEnd()
        this.scriptRunEcho = true
        return command
    }

    cancelScriptEndSentinel(error = null) {
        this.scriptStartMarker = null
        this.scriptStartSeen = false
        this.scriptStartWaiter?.reject(error || Object.assign(new Error('Script ended before its start acknowledgement'), {
            code: 'SCRIPT_START_MISSING',
        }))
        this.scriptEndMarker = null
        this.scriptEndCommand = null
        this.scriptRunEcho = false
        this.scriptRunPrelude = null
        this.scriptEndPromptOffset = null
    }

    /**
     * Swallow the current run's unique sentinel output line and fire
     * onScriptEnd. Its echoed print command is buffered and removed with it.
     */
    drainScriptEnd() {
        if (this.scriptRunEcho && this.scriptEndCommand) {
            const at = this.buffer.indexOf(this.scriptEndCommand)
            const end = at + this.scriptEndCommand.length
            if (at >= 0 && this.buffer.slice(end, end + 2) === '\r\n') {
                // Remove the echo immediately, so an input() prompt is visible
                // while execution is still waiting for terminal input.
                this.buffer = this.buffer.slice(0, at) + this.buffer.slice(end + 2)
                this.scriptEndCommand = null
                this.scriptRunEcho = false
            }
        }
        this.drainScriptStart()
        const marker = this.scriptEndMarker
        if (!marker) {
            return
        }
        let searchFrom = 0
        for (;;) {
            const at = this.buffer.indexOf(marker, searchFrom)
            if (at < 0) {
                return
            }
            let end = at + marker.length
            const tail = this.buffer[end]
            if (tail === undefined) {
                return // need one more byte to tell echo from output
            }
            if (tail !== '\r' && tail !== '\n') {
                // The echoed print command contains the same marker followed
                // by a quote. Keep searching for the actual output line.
                searchFrom = end
                continue
            }
            if (tail === '\r') {
                if (end + 1 >= this.buffer.length) {
                    return // wait for the byte after \r
                }
                end += (this.buffer[end + 1] === '\n') ? 2 : 1
            } else {
                end += 1
            }
            let before = this.buffer.slice(0, at)
            const echoAt = this.scriptEndCommand ? before.lastIndexOf(this.scriptEndCommand) : -1
            if (echoAt >= 0) {
                let echoEnd = echoAt + this.scriptEndCommand.length
                if (before.slice(echoEnd, echoEnd + 2) === '\r\n') {
                    echoEnd += 2
                } else if (before[echoEnd] === '\r' || before[echoEnd] === '\n') {
                    echoEnd += 1
                }
                before = before.slice(0, echoAt) + before.slice(echoEnd)
            }
            this.buffer = before + this.buffer.slice(end)
            this.cancelScriptEndSentinel()
            this.scriptEndPromptOffset = before.length
            this.onScriptEnd?.()
            return
        }
    }

    drainScriptStart() {
        const marker = this.scriptStartMarker
        if (!marker) return
        let searchFrom = 0
        for (;;) {
            const at = this.buffer.indexOf(marker, searchFrom)
            if (at < 0) return
            let end = at + marker.length
            const tail = this.buffer[end]
            if (tail === undefined) return
            if (tail !== '\r' && tail !== '\n') {
                // The friendly-REPL echo includes the marker inside the
                // runner command. Only its standalone printed line is proof
                // that the VM entered the runner.
                searchFrom = end
                continue
            }
            if (tail === '\r') {
                if (end + 1 >= this.buffer.length) return
                end += this.buffer[end + 1] === '\n' ? 2 : 1
            } else {
                end += 1
            }
            this.buffer = this.buffer.slice(0, at) + this.buffer.slice(end)
            this.scriptStartMarker = null
            this.scriptStartSeen = true
            this.scriptStartWaiter?.resolve()
            return
        }
    }

    /**
     * Length of the buffer suffix that could grow into the sentinel marker
     * (starts with at least '~~'). Held back from passthrough so a marker
     * split across frames never leaks half into the terminal.
     */
    partialMarkerTail() {
        const marker = this.scriptEndMarker
        if (!marker) {
            return 0
        }
        const commandAt = this.scriptEndCommand ? this.buffer.lastIndexOf(this.scriptEndCommand) : -1
        if (commandAt >= 0) {
            // Keep the internal command (including its echo/newline) private
            // until its output marker arrives and both can be removed.
            return this.buffer.length - commandAt
        }
        for (const activeMarker of [this.scriptStartMarker, marker]) {
            if (activeMarker && this.buffer.endsWith(activeMarker + '\r')) {
                return activeMarker.length + 1
            }
        }
        let hold = 0
        for (const candidate of [this.scriptStartMarker, marker, this.scriptEndCommand]) {
            if (!candidate) {
                continue
            }
            const max = Math.min(candidate.length, this.buffer.length)
            for (let len = max; len >= 1; len--) {
                if (this.buffer.endsWith(candidate.slice(0, len))) {
                    hold = Math.max(hold, len)
                    break
                }
            }
        }
        return hold
    }

    /**
     * Feed decoded repl.stdout text. Returns the part not consumed by an
     * internal exec — the caller writes only that part to the
     * terminal, so bootstrap and file-transfer traffic (device info, fs walk,
     * hex file reads) never shows up as garbage in the user's console.
     * Interactive typing is unaffected: it registers no waiters, so every
     * byte passes straight through.
     */
    feedStdout(text) {
        if (!text) {
            return ''
        }
        this.buffer += text
        this.drainScriptEnd()
        let passthrough = ''
        let holdPromptTail = false
        if (this.scriptEndPromptOffset !== null) {
            const prefix = this.buffer.slice(0, this.scriptEndPromptOffset)
            const tail = this.buffer.slice(this.scriptEndPromptOffset)
            if (tail.length < PROMPT.length && PROMPT.startsWith(tail)) {
                this.buffer = tail
                this.scriptEndPromptOffset = 0
                passthrough = prefix
                holdPromptTail = true
            } else if (tail.startsWith(PROMPT)) {
                this.buffer = prefix + tail.slice(PROMPT.length)
                this.scriptEndPromptOffset = null
            } else {
                this.scriptEndPromptOffset = null
            }
        }
        this.drain()
        if (this.waiters.length === 0) {
            const hold = holdPromptTail ? this.buffer.length : this.partialMarkerTail()
            const out = this.buffer.slice(0, this.buffer.length - hold)
            this.buffer = this.buffer.slice(this.buffer.length - hold)
            return passthrough + out
        }
        return ''
    }

    drain() {
        for (let i = this.waiters.length - 1; i >= 0; i--) {
            const waiter = this.waiters[i]
            // Resolve only when the echoed command AND a prompt after it have
            // arrived: concurrent REPL output (e.g. a KeyboardInterrupt
            // traceback) must not resolve the exec early.
            let start = 0
            if (waiter.cmd) {
                const echoAt = this.buffer.indexOf(waiter.cmd)
                if (echoAt < 0) {
                    continue
                }
                start = echoAt + waiter.cmd.length
            }
            const promptAt = this.buffer.indexOf(PROMPT, start)
            if (promptAt < 0) {
                continue
            }
            const output = this.buffer.slice(waiter.cmd ? start : 0, promptAt)
            this.buffer = this.buffer.slice(promptAt + PROMPT.length)
            this.waiters.splice(i, 1)
            clearTimeout(waiter.timer)
            waiter.removeAbortListener?.()
            waiter.resolve(output)
        }
    }

    failAll(error) {
        this.closedError = error
        for (const waiter of this.waiters.splice(0)) {
            clearTimeout(waiter.timer)
            waiter.removeAbortListener?.()
            waiter.reject(error)
        }
        this.cancelScriptEndSentinel(error)
        this.buffer = ''
    }

    resetEpoch(error) {
        this.failAll(error)
        this.closedError = null
    }

    /**
     * Execute one REPL input (single line) and resolve with everything it
     * printed before the next prompt, minus the echoed input line.
     */
    enqueue(operation, { signal = null } = {}) {
        const result = this.execQueue.then(() => {
            if (this.closedError) {
                throw this.closedError
            }
            throwIfAborted(signal)
            return operation()
        })
        // Keep the queue usable after an operation fails; callers still
        // receive the original rejection through result.
        this.execQueue = result.catch(() => {})
        return result
    }

    writeStdin(data, { signal = null } = {}) {
        return this.enqueue(async () => {
            const bytes = data instanceof Uint8Array ? data : new TextEncoder().encode(data)
            if (bytes.length <= this.inputMaxBytes) {
                return this.writeEvmux('repl.stdin', 'data', null, data, { signal })
            }
            let result
            for (let offset = 0; offset < bytes.length; offset += this.inputMaxBytes) {
                result = await this.writeEvmux(
                    'repl.stdin',
                    'data',
                    null,
                    bytes.subarray(offset, Math.min(offset + this.inputMaxBytes, bytes.length)),
                    { signal },
                )
            }
            return result
        }, { signal })
    }

    execLine(line, { timeout = 8000, signal = null } = {}) {
        return this.enqueue(() => this._execLine(line, timeout, signal), { signal })
    }

    async _execLine(line, timeout, signal = null) {
        throwIfAborted(signal)
        const inputBytes = new TextEncoder().encode(`${line}\r\n`).length
        if (inputBytes > this.inputMaxBytes) {
            throw new Error(
                `Friendly-REPL input is ${inputBytes} bytes; firmware accepts at most ` +
                `${this.inputMaxBytes}. Use chunked script execution.`,
            )
        }
        let waiter
        let commandWritten = false
        const completion = new Promise((resolve, reject) => {
            const removeWaiter = () => {
                const index = this.waiters.indexOf(waiter)
                if (index >= 0) {
                    this.waiters.splice(index, 1)
                }
            }
            const fail = (error) => {
                removeWaiter()
                clearTimeout(waiter.timer)
                waiter.removeAbortListener?.()
                reject(error)
            }
            const timer = setTimeout(() => {
                fail(Object.assign(new Error(`repl exec timeout: ${line.slice(0, 60)}`), { code: 'TIMEOUT' }))
            }, timeout)
            const abort = () => {
                fail(abortReason(signal))
                if (commandWritten) {
                    void this.writeEvmux(
                        'repl.signal',
                        'signal',
                        null,
                        new Uint8Array([0x03]),
                    ).catch(() => {})
                }
            }
            waiter = {
                cmd: line,
                timer,
                resolve,
                reject,
                removeAbortListener: signal
                    ? () => signal.removeEventListener('abort', abort)
                    : null,
            }
            this.waiters.push(waiter)
            if (signal) {
                signal.addEventListener('abort', abort, { once: true })
                if (signal.aborted) abort()
            }
        })
        // Cancellation can reject the waiter while the transport write is pending.
        completion.catch(() => {})
        if (signal?.aborted) return completion
        try {
            await this.writeEvmux('repl.stdin', 'data', null, line + '\r\n', { signal })
            commandWritten = true
        } catch (err) {
            const index = this.waiters.indexOf(waiter)
            if (index >= 0) {
                this.waiters.splice(index, 1)
            }
            clearTimeout(waiter.timer)
            waiter.removeAbortListener?.()
            // Observe the promise rejection path through the same waiter so
            // it cannot become an unhandled timeout later.
            waiter.reject(err)
            try {
                await completion
            } catch (_err) {
                // The write error below is the useful failure.
            }
            throw err
        }
        const output = await completion
        return output.replace(/\r/g, '')
    }

    /** Execute a (possibly multi-line) Python snippet via exec("..."). */
    execPy(source, { timeout = 8000, signal = null } = {}) {
        return this.enqueue(() => this._execPy(source, timeout, signal), { signal })
    }

    _execPy(source, timeout = 8000, signal = null) {
        return this._execLine(`exec("${escapeForExec(source)}")`, timeout, signal)
    }

    async getDeviceInfo({ signal = null } = {}) {
        // The framed REPL starts with an empty namespace: unlike the raw-REPL
        // bootstrap (which sends `import sys,os` first), every snippet here
        // must import what it uses or it dies with NameError and the reply
        // is a traceback instead of the expected fields.
        const rsp = await this.execPy(`
import os,sys
try: u=os.uname()
except: u=('','','','',sys.platform)
try: v=sys.version.split(';')[1].strip()
except: v='MicroPython '+u[2]
mpy=getattr(sys.implementation, '_mpy', 0)
sp=':'.join(sys.path)
d=[u[4],u[2],u[0],v,mpy>>10,mpy&0xFF,(mpy>>8)&3,sp]
print('|'.join(str(x) for x in d))
`, { signal })
        const fields = rsp.trim().split('|')
        if (fields.length < 8) {
            throw new Error(`unexpected device info response: ${rsp.trim().slice(0, 200)}`)
        }
        let [machine, release, sysname, version, mpy_arch, mpy_ver, mpy_sub, sys_path] = fields
        sys_path = sys_path.split(':')
        try {
            mpy_arch = [null, 'x86', 'x64', 'armv6', 'armv6m', 'armv7m', 'armv7em', 'armv7emsp', 'armv7emdp', 'xtensa', 'xtensawin', 'rv32imc'][mpy_arch]
        } catch (_err) {
            mpy_arch = null
        }
        mpy_ver = parseInt(mpy_ver, 10)
        mpy_sub = parseInt(mpy_sub, 10)
        if (!mpy_ver) { mpy_ver = 'py' }
        return { machine, release, sysname, version, mpy_arch, mpy_ver, mpy_sub, sys_path }
    }

    async getFsStats(path = '/', { signal = null } = {}) {
        const pathLiteral = pythonString(path)
        const rsp = await this.execPy(`
import os
s = os.statvfs(${pathLiteral})
fs = s[1] * s[2]
ff = s[3] * s[0]
fu = fs - ff
print('%s|%s|%s'%(fu,ff,fs))
`, { signal })
        return rsp.trim().split('|')
    }

    /** List only the requested directory; recursion visits its descendants only. */
    async listFs(path, { recursive = false, signal = null } = {}) {
        const entries = []
        const pending = [path]
        while (pending.length) {
            throwIfAborted(signal)
            const directory = pending.pop()
            const assignment = `p=${pythonString(directory)}.rstrip('/')`
            const body = `
import os,json
try:
 r=[(n,os.stat(p+'/'+n)[0],os.stat(p+'/'+n)[6])for n in os.listdir(p or '/')]
except OSError as e:r=e.args[0]
print('__EVLIST__'+json.dumps(r))
`
            let source = `${assignment}\n${body}`
            if (this.execInputBytes(source) > this.inputMaxBytes) {
                await this.execPy(assignment, { signal })
                source = body
            }
            const response = await this.execPy(source, { signal })
            const line = response.trim().split('\n').findLast(value => value.startsWith('__EVLIST__'))
            if (!line) throw Object.assign(new Error('Incomplete directory listing'), { code: 'DEVICE_DIRECTORY_READ_INCOMPLETE' })
            const result = JSON.parse(line.slice('__EVLIST__'.length))
            if (Number.isInteger(result)) {
                throw Object.assign(new Error(`Cannot list ${directory}: OSError ${result}`), {
                    code: result === 2 ? 'NOT_FOUND' : result === 20 ? 'NOT_DIRECTORY' : 'FILESYSTEM_ERROR', errno: result,
                })
            }
            if (!Array.isArray(result)) throw new Error('Invalid directory listing')
            for (const item of result) {
                if (!Array.isArray(item) || item.length !== 3 || typeof item[0] !== 'string'
                    || !Number.isInteger(item[1]) || !Number.isSafeInteger(item[2]) || item[2] < 0
                    || item[0].includes('/') || ['.', '..'].includes(item[0])) throw new Error('Invalid directory entry')
                const [name, mode, size] = item
                const fullpath = `${directory === '/' ? '' : directory}/${name}`
                const isDirectory = Boolean(mode & 0x4000)
                entries.push({ name, path: fullpath, type: isDirectory ? 'directory' : 'file', size: isDirectory ? 0 : size })
                if (recursive && isDirectory) pending.push(fullpath)
            }
        }
        return entries
    }

    async walkFs({ signal = null } = {}) {
        const rsp = await this.execPy(`
import os
def walk(p):
 for n in os.listdir(p if p else '/'):
  fn=p+'/'+n
  try: s=os.stat(fn)
  except: s=(0,)*7
  try:
   if s[0] & 0x4000 == 0:
    print('f|'+fn+'|'+str(s[6]))
   elif n not in ('.','..'):
    print('d|'+fn+'|'+str(s[6]))
    walk(fn)
  except:
   print('f|'+p+'/???|'+str(s[6]))
walk('')
`, { timeout: 20000, signal })
        const result = []
        for (const line of rsp.split('\n')) {
            if (line === '') continue
            let current = result
            const [type, fullpath, size] = line.trim().split('|')
            const path = fullpath.split('/')
            let file
            if (type == 'f') {
                file = path.pop()
            }
            for (const segment of path) {
                if (segment === '') continue
                const next = current.filter((x) => x.name === segment && 'content' in x)
                if (next.length) {
                    current = next[0].content
                } else {
                    const prev = current
                    current = []
                    prev.push({ name: segment, path: path.join('/'), content: current })
                }
            }
            if (type == 'f') {
                current.push({ name: file, path: fullpath, size: parseInt(size, 10) })
            }
        }
        return result
    }

    async readFile(fn, { timeout = null, chunkTimeout = 30000, signal = null, maxBytes = null } = {}) {
        if (timeout !== null && (!Number.isFinite(timeout) || timeout <= 0)) {
            throw new RangeError('Device file read timeout must be positive or null')
        }
        if (!Number.isFinite(chunkTimeout) || chunkTimeout <= 0) {
            throw new RangeError('Device file read chunk timeout must be positive')
        }
        if (maxBytes !== null && (!Number.isSafeInteger(maxBytes) || maxBytes < 0 || maxBytes > 16 * 1024 * 1024)) {
            throw Object.assign(new Error('Invalid bounded file read limit'), { code: 'DEVICE_READ_LIMIT_INVALID' })
        }
        // Keep each stdout burst bounded, allowing the transport task to send
        // keepalives between chunks. Serialize the whole read with REPL work.
        return this.enqueue(async () => {
            const deadline = timeout === null ? null : Date.now() + timeout
            const commandBudget = () => {
                if (deadline === null) return chunkTimeout
                const remaining = deadline - Date.now()
                if (remaining <= 0) throw Object.assign(new Error('Device file read timeout'), { code: 'TIMEOUT' })
                return Math.min(chunkTimeout, remaining)
            }
            const chunks = []
            let offset = 0
            let size = null
            let filePath = pythonString(fn)
            const incomplete = () => Object.assign(new Error(`Incomplete file response for ${fn}`), { code: 'DEVICE_FILE_READ_INCOMPLETE' })
            for (;;) {
                throwIfAborted(signal)
                const count = maxBytes === null ? 1024 : Math.min(1024, maxBytes - offset + 1)
                let source = `
import binascii
try:
 with open(${filePath},'rb') as f:
  f.seek(0,2)
  z=f.tell()
  f.seek(${offset})
  b=f.read(${count})
  print('__EVREAD_OK__%d:%d:'%(z,len(b))+binascii.hexlify(b).decode())
except Exception as e:
 print('__EVREAD_ERROR__'+repr(e))
`
                if (this.execInputBytes(source) > this.inputMaxBytes) {
                    await this._execPy(`__evcore_read_path=${pythonString(fn)}`, commandBudget(), signal)
                    commandBudget()
                    source = source.replace(filePath, '__evcore_read_path')
                    filePath = '__evcore_read_path'
                }
                const rsp = await this._execPy(source, commandBudget(), signal)
                commandBudget()
                const errorAt = rsp.indexOf('__EVREAD_ERROR__')
                if (errorAt >= 0) throw new Error(`Unable to read ${fn}: ${rsp.slice(errorAt + 16).trim()}`)
                const match = /(?:^|\n)__EVREAD_OK__(\d+):(\d+):([0-9a-fA-F]*)\s*$/.exec(rsp)
                if (!match) throw incomplete()
                const reportedSize = Number(match[1])
                const reportedCount = Number(match[2])
                const hex = match[3]
                if (!Number.isSafeInteger(reportedSize) || !Number.isSafeInteger(reportedCount)
                    || reportedCount > count || hex.length !== reportedCount * 2) throw incomplete()
                if (size !== null && size !== reportedSize) {
                    throw Object.assign(new Error(`Device file changed during read: ${fn}`), { code: 'DEVICE_FILE_CHANGED' })
                }
                size = reportedSize
                const bytes = Uint8Array.from(hex.match(/../g) || [], value => parseInt(value, 16))
                offset += bytes.length
                if (offset > size || (!bytes.length && offset < size)) throw incomplete()
                if (maxBytes !== null && size > maxBytes) {
                    throw Object.assign(new Error('Device file exceeds the bounded read limit'), { code: 'DEVICE_FILE_TOO_LARGE' })
                }
                chunks.push(bytes)
                if (offset === size) break
            }
            const result = new Uint8Array(offset)
            let position = 0
            for (const chunk of chunks) {
                result.set(chunk, position)
                position += chunk.length
            }
            return result
        }, { signal })
    }

    /**
     * Run one filesystem statement and raise on failure. The snippet prints a
     * JSON marker as its last line so errors (e.g. removing a non-empty
     * directory) surface as exceptions instead of silent REPL tracebacks.
     */
    async _fsOp(stmt, { signal = null, path = undefined } = {}) {
        const source = statement => `
import os,json
try:
 ${statement}
 print(json.dumps({'ok':True}))
except OSError as e:
 print(json.dumps({'ok':False,'err':e.args[0] if e.args else -1}))
`
        let rsp
        if (path !== undefined && this.execInputBytes(source(stmt)) > this.inputMaxBytes) {
            rsp = await this.enqueue(async () => {
                await this._execPy(`__evcore_fs_path=${pythonString(path)}`, 8000, signal)
                return this._execPy(source(stmt.replace(pythonString(path), '__evcore_fs_path')), 8000, signal)
            }, { signal })
        } else {
            rsp = await this.execPy(source(stmt), { signal })
        }
        const result = JSON.parse(rsp.trim().split('\n').pop())
        if (!result.ok) {
            if (result.err === 39) {
                throw new Error('Directory not empty')
            }
            throw new Error(`OSError ${result.err}`)
        }
    }

    async touchFile(fn) {
        await this._fsOp(`open(${pythonString(fn)},'wb').close()`)
    }

    async removeFile(path, { signal = null } = {}) {
        await this._fsOp(`os.remove(${pythonString(path)})`, { signal, path })
    }

    async removeDir(path, { signal = null } = {}) {
        await this._fsOp(`os.rmdir(${pythonString(path)})`, { signal, path })
    }

    async makePath(path) {
        const pathLiteral = pythonString(path)
        // TODO: remove error code 20 once it is fixed in wasm port
        const rsp = await this.execPy(`
import os,json
try:
 p=''
 for d in ${pathLiteral}.split('/'):
  if not d: continue
  p += '/'+d
  try: os.mkdir(p)
  except OSError as e:
   if e.args[0] not in (17, 20): raise
 print(json.dumps({'ok':True}))
except OSError as e:
 print(json.dumps({'ok':False,'err':e.args[0] if e.args else -1}))
`)
        const result = JSON.parse(rsp.trim().split('\n').pop())
        if (!result.ok) {
            throw new Error(`OSError ${result.err}`)
        }
    }

    writeFile(fn, data, chunk_size = EV_REPL_FILE_CHUNK_BYTES, direct = false, { signal = null } = {}) {
        return this.enqueue(
            () => this._writeFile(fn, data, chunk_size, direct, signal),
            { signal },
        )
    }

    async _writeFile(fn, data, chunk_size, direct, signal = null) {
        throwIfAborted(signal)
        if (typeof data === 'string' || data instanceof String) {
            data = new TextEncoder().encode(data)
        } else if (data instanceof ArrayBuffer) {
            data = new Uint8Array(data)
        }
        const dest = direct ? fn : '.viper.tmp'
        const destLiteral = pythonString(dest)
        const pathLiteral = pythonString(fn)
        await this._execPy(`
import os
try:
 import binascii
 u=binascii.unhexlify
except:
 u=lambda s: bytes(int(s[i:i+2], 16) for i in range(0, len(s), 2))
f=open(${destLiteral},'wb')
`, 8000, signal)
        // Split into chunks and send
        for (let i = 0; i < data.byteLength; i += chunk_size) {
            throwIfAborted(signal)
            const chunk = data.subarray(i, Math.min(i + chunk_size, data.byteLength))
            const hex = [...chunk].map((b) => b.toString(16).padStart(2, '0')).join('')
            await this._execPy(`f.write(u('${hex}'))`, 8000, signal)
        }
        if (direct) {
            await this._execPy('f.close()', 8000, signal)
        } else {
            await this._execPy(`f.close()
try: os.remove(${pathLiteral})
except: pass
os.rename(${destLiteral},${pathLiteral})
`, 8000, signal)
        }
    }

    async readSysInfoMD() {
        return await this.execPy(`
import os,sys,gc
gc.collect()
mu = gc.mem_alloc()
mf = gc.mem_free()
ms = mu + mf
uname=os.uname()
p=print
def size_fmt(size):
 suffixes = ['B','KiB','MiB','GiB','TiB']
 i = 0
 while size > 1024 and i < len(suffixes)-1:
  i += 1
  size //= 1024
 return "%d%s" % (size, suffixes[i])
p('## Machine')
p('- Name: \`'+uname.machine+'\`')
try:
 gc.collect()
 import microcontroller as uc
 p('- CPU: \`%s @ %s MHz\`' % (sys.platform, uc.cpu.frequency // 1_000_000))
 p('- UID: \`%s\`' % (uc.cpu.uid.hex(),))
 p('- Temp.: \`%s °C\`' % (uc.cpu.temperature,))
 p('- Voltage: \`%s V\`' % (uc.cpu.voltage,))
except:
 try:
  gc.collect()
  import machine
  p('- CPU: \`%s @ %s MHz\`' % (sys.platform, machine.freq() // 1_000_000))
 except:
  p('- CPU: \`'+sys.platform+'\`')
p()
p('## System')
p('- Version: \`'+sys.version.split(";")[1].strip()+'\`')
if ms:
 p('- Memory use:  \`%s / %s, free: %d%%\`' % (size_fmt(mu), size_fmt(ms), (mf * 100) // ms))
`)
    }
}
