/*
 * SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 */

const SOF = 0x1e
const EOF_GUARD = 0x1f
const HEADER_RE = /^EVMUX\/1 h=(\d+) p=(\d+) c=([0-9A-Fa-f]{8})\r\n$/
const MAX_BUFFER = 1024 * 1024
const MAX_HEADER = 96

let crc32Table = null

function crc32(bytes) {
    if (!crc32Table) {
        crc32Table = new Uint32Array(256)
        for (let i = 0; i < 256; i++) {
            let c = i
            for (let j = 0; j < 8; j++) {
                c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1)
            }
            crc32Table[i] = c >>> 0
        }
    }
    let crc = 0xffffffff
    for (const byte of bytes) {
        crc = crc32Table[(crc ^ byte) & 0xff] ^ (crc >>> 8)
    }
    return (crc ^ 0xffffffff) >>> 0
}

function concatBytes(a, b) {
    if (!a || a.length === 0) {
        return b
    }
    if (!b || b.length === 0) {
        return a
    }
    const out = new Uint8Array(a.length + b.length)
    out.set(a, 0)
    out.set(b, a.length)
    return out
}

function bytesToBase64(bytes) {
    let binary = ''
    const chunkSize = 0x8000
    for (let i = 0; i < bytes.length; i += chunkSize) {
        binary += String.fromCharCode(...bytes.subarray(i, i + chunkSize))
    }
    return btoa(binary)
}

function indexOfByte(bytes, value, from = 0) {
    for (let i = from; i < bytes.length; i++) {
        if (bytes[i] === value) {
            return i
        }
    }
    return -1
}

function findHeaderEnd(bytes) {
    for (let i = 1; i < bytes.length; i++) {
        if (bytes[i - 1] === 0x0d && bytes[i] === 0x0a) {
            return i + 1
        }
        if (i > MAX_HEADER) {
            return -2
        }
    }
    return -1
}

function appendLine(value) {
    if (!value.endsWith('\n')) {
        return value + '\n'
    }
    return value
}

function decodePayloadText(payload) {
    return new TextDecoder('utf-8', { fatal: false }).decode(payload)
}

export class EvMuxParser {
    constructor(handlers = {}) {
        this.handlers = handlers
        this.buffer = new Uint8Array(0)
        this.rawTextDecoder = new TextDecoder('utf-8', { fatal: false })
        this.replTextDecoders = {
            'repl.stdout': new TextDecoder('utf-8', { fatal: false }),
            'repl.stderr': new TextDecoder('utf-8', { fatal: false }),
        }
        this.metadataDecoder = new TextDecoder('utf-8', { fatal: false })
        this.active = false
        this.rxBytes = 0
        this.rxChunks = 0
        this.frames = 0
        this.droppedFrames = 0
        this.lastChannel = null
        this.maxMetadataLength = 65535
        this.maxPayloadLength = MAX_BUFFER
        this.frameProgressTimeoutMs = 12000
        this.frameTotalTimeoutMs = 12000
        this.partialFrame = null
        this.partialFrameTimer = null
    }

    setLimits({ metadataMax, payloadMax, frameProgressTimeoutMs, frameTotalTimeoutMs } = {}) {
        if (Number.isSafeInteger(metadataMax) && metadataMax > 0) this.maxMetadataLength = metadataMax
        if (Number.isSafeInteger(payloadMax) && payloadMax > 0) this.maxPayloadLength = payloadMax
        if (Number.isSafeInteger(frameProgressTimeoutMs) && frameProgressTimeoutMs > 0) {
            this.frameProgressTimeoutMs = frameProgressTimeoutMs
        }
        if (Number.isSafeInteger(frameTotalTimeoutMs) && frameTotalTimeoutMs > 0) {
            this.frameTotalTimeoutMs = frameTotalTimeoutMs
        }
    }

    resetReplTextDecoders() {
        this.replTextDecoders = {
            'repl.stdout': new TextDecoder('utf-8', { fatal: false }),
            'repl.stderr': new TextDecoder('utf-8', { fatal: false }),
        }
    }

    _clearPartialFrame() {
        clearTimeout(this.partialFrameTimer)
        this.partialFrameTimer = null
        this.partialFrame = null
    }

    _armPartialFrameTimeout(progress = false) {
        const now = Date.now()
        if (!this.partialFrame) this.partialFrame = { startedAt: now, lastProgressAt: now }
        else if (progress) this.partialFrame.lastProgressAt = now
        clearTimeout(this.partialFrameTimer)
        const remaining = Math.max(1, Math.min(
            this.frameProgressTimeoutMs - (now - this.partialFrame.lastProgressAt),
            this.frameTotalTimeoutMs - (now - this.partialFrame.startedAt),
        ))
        this.partialFrameTimer = setTimeout(() => {
            const expiredAt = Date.now()
            if (!this.partialFrame) return
            if (expiredAt - this.partialFrame.lastProgressAt < this.frameProgressTimeoutMs
                    && expiredAt - this.partialFrame.startedAt < this.frameTotalTimeoutMs) {
                this._armPartialFrameTimeout()
                return
            }
            this._clearPartialFrame()
            this.buffer = new Uint8Array(0)
            this.rawTextDecoder = new TextDecoder('utf-8', { fatal: false })
            this.replTextDecoders = {
                'repl.stdout': new TextDecoder('utf-8', { fatal: false }),
                'repl.stderr': new TextDecoder('utf-8', { fatal: false }),
            }
            this.droppedFrames += 1
            this.handlers.other?.('[evmux] dropped an incomplete frame after receive timeout\n')
        }, remaining)
        this.partialFrameTimer.unref?.()
    }

    reset() {
        this._clearPartialFrame()
        this.buffer = new Uint8Array(0)
        this.rawTextDecoder = new TextDecoder('utf-8', { fatal: false })
        this.replTextDecoders = {
            'repl.stdout': new TextDecoder('utf-8', { fatal: false }),
            'repl.stderr': new TextDecoder('utf-8', { fatal: false }),
        }
        this.metadataDecoder = new TextDecoder('utf-8', { fatal: false })
        this.active = false
        this.rxBytes = 0
        this.rxChunks = 0
        this.frames = 0
        this.droppedFrames = 0
        this.lastChannel = null
    }

    getDiagnostics() {
        return {
            active: this.active,
            rxBytes: this.rxBytes,
            rxChunks: this.rxChunks,
            bufferedBytes: this.buffer.length,
            frames: this.frames,
            droppedFrames: this.droppedFrames,
            lastChannel: this.lastChannel,
        }
    }

    hasPartialFrame() {
        if (!this.partialFrame) return false
        const text = this.metadataDecoder.decode(this.buffer)
        const tokens = ['\x1eEVMUX/1 h=', /^\d+/, ' p=', /^\d+/, ' c=', /^[0-9a-fA-F]{8}/, '\r\n']
        let tail = text
        for (const token of tokens) {
            if (typeof token === 'string') {
                if (tail.length < token.length) return token.startsWith(tail)
                if (!tail.startsWith(token)) return false
                tail = tail.slice(token.length)
            } else {
                if (!tail) return true
                if (token.source.includes('{8}') && /^[0-9a-fA-F]{1,7}$/.test(tail)) return true
                const match = token.exec(tail)
                if (!match) return false
                tail = tail.slice(match[0].length)
            }
        }
        return true
    }

    feedText(text) {
        if (text) {
            this.handlers.rawText?.(text)
        }
    }

    feedBytes(bytes) {
        const hadPartial = Boolean(this.partialFrame)
        this.rxBytes += bytes.length
        this.rxChunks += 1
        this.buffer = concatBytes(this.buffer, bytes)
        if (this.buffer.length > MAX_BUFFER) {
            const keepFrom = Math.max(0, this.buffer.length - (MAX_BUFFER / 2))
            this.feedText(this.rawTextDecoder.decode(this.buffer.subarray(0, keepFrom), { stream: true }))
            this.buffer = this.buffer.subarray(keepFrom)
        }

        for (;;) {
            const sof = indexOfByte(this.buffer, SOF)
            if (sof < 0) {
                this.feedText(this.rawTextDecoder.decode(this.buffer, { stream: true }))
                this.buffer = new Uint8Array(0)
                return
            }

            if (sof > 0) {
                this.feedText(this.rawTextDecoder.decode(this.buffer.subarray(0, sof), { stream: true }))
                this.buffer = this.buffer.subarray(sof)
            }

            const headerEnd = findHeaderEnd(this.buffer)
            if (headerEnd === -1) {
                this._armPartialFrameTimeout(hadPartial)
                return
            }
            if (headerEnd === -2) {
                this.feedText(this.rawTextDecoder.decode(this.buffer.subarray(0, 1), { stream: true }))
                this.buffer = this.buffer.subarray(1)
                continue
            }

            const header = this.metadataDecoder.decode(this.buffer.subarray(1, headerEnd))
            const match = HEADER_RE.exec(header)
            if (!match) {
                this.feedText(this.rawTextDecoder.decode(this.buffer.subarray(0, 1), { stream: true }))
                this.buffer = this.buffer.subarray(1)
                continue
            }

            const metadataLen = Number(match[1])
            const payloadLen = Number(match[2])
            const expectedCrc = parseInt(match[3], 16) >>> 0
            if (!Number.isFinite(metadataLen) || !Number.isFinite(payloadLen) ||
                    metadataLen <= 0 || payloadLen < 0 || metadataLen > this.maxMetadataLength || payloadLen > this.maxPayloadLength) {
                this._clearPartialFrame()
                this.feedText(this.rawTextDecoder.decode(this.buffer.subarray(0, 1), { stream: true }))
                this.buffer = this.buffer.subarray(1)
                continue
            }

            const metadataStart = headerEnd
            const payloadStart = metadataStart + metadataLen
            const eofPos = payloadStart + payloadLen
            const frameEnd = eofPos + 1
            if (this.buffer.length < frameEnd) {
                this._armPartialFrameTimeout(hadPartial)
                return
            }

            this._clearPartialFrame()

            if (this.buffer[eofPos] !== EOF_GUARD) {
                const nextSof = indexOfByte(this.buffer, SOF, 1)
                if (nextSof > 0) {
                    this.buffer = this.buffer.subarray(nextSof)
                } else {
                    this.buffer = new Uint8Array(0)
                }
                this.handlers.other?.('[evmux] dropped frame without EOF guard\n')
                this.droppedFrames += 1
                continue
            }

            let metadata
            try {
                metadata = JSON.parse(this.metadataDecoder.decode(this.buffer.subarray(metadataStart, payloadStart)))
                if (!metadata || typeof metadata !== 'object' || Array.isArray(metadata)) {
                    throw new Error('metadata must be an object')
                }
            } catch (_err) {
                this.handlers.other?.('[evmux] dropped frame with invalid metadata\n')
                this.droppedFrames += 1
                this.buffer = this.buffer.subarray(frameEnd)
                continue
            }

            const payload = this.buffer.subarray(payloadStart, eofPos)
            this.buffer = this.buffer.subarray(frameEnd)
            if (expectedCrc !== 0 && crc32(payload) !== expectedCrc) {
                this.handlers.other?.('[evmux] dropped frame with invalid CRC\n')
                this.droppedFrames += 1
                continue
            }
            if (this.handlers.filterFrame?.(metadata, payload) === false) {
                this.droppedFrames += 1
                this.handlers.frameRejected?.(metadata)
                continue
            }
            this.frames += 1
            this.lastChannel = metadata.channel || 'unknown'
            this.dispatch(metadata, payload)
        }
    }

    dispatch(metadata, payload) {
        const channel = metadata.channel || 'unknown'
        if (channel === 'repl.stdout' || channel === 'repl.stderr') {
            this.handlers.text?.(this.replTextDecoders[channel].decode(payload, { stream: true }))
            return
        }
        if (channel === 'log.idf') {
            const text = decodePayloadText(payload)
            const prefix = metadata.method ? `[${metadata.method}] ` : ''
            this.handlers.log?.(appendLine(prefix + text))
            return
        }
        if (channel === 'user.rpc' || channel === 'debug.rpc') {
            // EV-MUX v4: debug.capture_frame answers with the rsp frame itself
            // carrying the binary JPEG payload (no separate data channel).
            if (channel === 'debug.rpc' && metadata.type === 'rsp' && metadata.contentType === 'image/jpeg') {
                this.handlers.debugImage?.(bytesToBase64(payload), {
                    width: Number(metadata.width) || 0,
                    height: Number(metadata.height) || 0,
                    size: payload.length,
                    format: 'jpg',
                    encoding: 'binary',
                    route: metadata.route || '',
                    seq: metadata.seq,
                }, payload, metadata)
                // Resolve the pending RPC as well, so the request/response
                // pair still shows up as a completed row in the System Log.
                this.handlers.rpc?.(metadata, { ok: true, contentType: 'image/jpeg', bytes: payload.length })
                return
            }
            if (metadata.type !== 'rsp' && metadata.type !== 'event') {
                this.handlers.command?.(appendLine(`${channel} ${metadata.type || 'frame'} ${metadata.method || ''} #${metadata.seq ?? '-'}`))
                return
            }
            const text = decodePayloadText(payload)
            let logFrame = true
            try {
                logFrame = this.handlers.rpc?.(metadata, JSON.parse(text)) !== false
            } catch (_err) {
                logFrame = this.handlers.rpc?.(metadata, null) !== false
            }
            if (!logFrame) {
                return
            }
            const line = `${metadata.type || 'frame'} ${metadata.method || ''} #${metadata.seq ?? '-'} ${text}`
            this.handlers.command?.(appendLine(`${channel} ${line}`))
            return
        }
        if (channel === 'preview.frame' && metadata.contentType === 'image/jpeg') {
            this.handlers.preview?.(bytesToBase64(payload), {
                width: Number(metadata.width) || 0,
                height: Number(metadata.height) || 0,
                size: payload.length,
                format: 'jpg',
                encoding: 'binary',
                route: metadata.route || '',
                seq: metadata.seq,
            }, payload)
            return
        }
        this.handlers.other?.(appendLine(`${channel} ${payload.length} bytes`))
    }
}

export function makeEvMuxFrame({ channel, type = 'data', method = null, payload = '', seq = 0, sid = 'web-ide', sessionId = undefined, vmEpoch = undefined, contentType = undefined, width = undefined, height = undefined, route = undefined }) {
    const payloadBytes = payload instanceof Uint8Array ? payload : new TextEncoder().encode(payload)
    const metadata = {
        sid,
        seq,
        channel,
        type,
    encoding: channel.endsWith('.rpc') ? 'json' : 'text',
    ...(sessionId ? { sessionId } : {}),
    ...(vmEpoch !== undefined ? { vmEpoch } : {}),
    ...(contentType ? { contentType } : {}),
    ...(Number.isFinite(width) ? { width } : {}),
    ...(Number.isFinite(height) ? { height } : {}),
    ...(route ? { route } : {}),
  }
    if (method) {
        metadata.method = method
    }
    const metadataBytes = new TextEncoder().encode(JSON.stringify(metadata))
    const header = new TextEncoder().encode(`\x1eEVMUX/1 h=${metadataBytes.length} p=${payloadBytes.length} c=00000000\r\n`)
    const out = new Uint8Array(header.length + metadataBytes.length + payloadBytes.length + 1)
    out.set(header, 0)
    out.set(metadataBytes, header.length)
    out.set(payloadBytes, header.length + metadataBytes.length)
    out[out.length - 1] = EOF_GUARD
    return out
}
