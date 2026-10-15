// SPDX-License-Identifier: Apache-2.0
import { evReplExecInputBytes } from './evrepl.js'
import { checkAbort, snapshotRequest, validateDevicePath, withDeviceOperation } from './device_python.js'

function requestObject(request) {
  if (!request || typeof request !== 'object' || Array.isArray(request)) throw new TypeError('Expected a file operation request')
  return request
}

export function createFilesApi({ session, state = {} }) {
  const operate = (kind, request, fn) => {
    requestObject(request)
    checkAbort(request.signal)
    validateDevicePath(request.path, { root: kind === 'list' })
    return withDeviceOperation(state, `files.${kind}`, async () => {
      if (session.runCapture || session.runStarting) throw Object.assign(new Error('Device Python interpreter is busy'), { code: 'DEVICE_BUSY' })
      return fn()
    })
  }
  return Object.freeze({
    async list(request = {}) {
      request = snapshotRequest(request, ['path', 'recursive', 'signal'], 'files.list')
      const options = { path: '/', recursive: false, ...request }
      if (typeof options.recursive !== 'boolean') throw new TypeError('recursive must be a boolean')
      return operate('list', options, () => session.repl.listFs(options.path, {
        recursive: options.recursive, signal: options.signal,
      }))
    },
    async read(request) {
      request = snapshotRequest(request, ['path', 'encoding', 'timeout', 'maxBytes', 'signal'], 'files.read')
      const { path, encoding = 'binary', timeout = 30000, maxBytes = null, signal = null } = request
      if (!['binary', 'utf-8'].includes(encoding)) throw new TypeError('File encoding must be binary or utf-8')
      if (!Number.isFinite(timeout) || timeout <= 0) throw new RangeError('File read timeout must be positive')
      if (maxBytes !== null && (!Number.isSafeInteger(maxBytes) || maxBytes < 0 || maxBytes > 16 * 1024 * 1024)) throw new RangeError('maxBytes must be between zero and 16 MiB')
      return operate('read', request, async () => {
        const bytes = await session.repl.readFile(path, { timeout, maxBytes, signal })
        return encoding === 'binary' ? bytes : new TextDecoder().decode(bytes)
      })
    },
    async write(request) {
      request = snapshotRequest(request, ['path', 'data', 'overwrite', 'onProgress', 'signal'], 'files.write')
      const { path, data, overwrite = true, onProgress = null, signal = null } = request
      if (typeof data !== 'string' && !(data instanceof Uint8Array)) throw new TypeError('File data must be a string or Uint8Array')
      if (typeof overwrite !== 'boolean') throw new TypeError('overwrite must be a boolean')
      if (onProgress !== null && typeof onProgress !== 'function') throw new TypeError('onProgress must be a function')
      const content = data instanceof Uint8Array ? new Uint8Array(data) : data
      return operate('write', request, () => session.writeFile(path, content, {
        encoding: typeof data === 'string' ? 'utf-8' : 'binary', mode: overwrite ? 'overwrite' : 'create', onProgress, signal,
      }))
    },
    async remove(request) {
      request = snapshotRequest(request, ['path', 'type', 'signal'], 'files.remove')
      const { path, type = 'file', signal = null } = request
      if (!['file', 'directory'].includes(type)) throw new TypeError('Removal type must be file or directory')
      return operate('remove', request, () => type === 'directory'
        ? session.repl.removeDir(path, { signal }) : session.repl.removeFile(path, { signal }))
    },
    async mkdir(request) {
      request = snapshotRequest(request, ['path', 'recursive', 'signal'], 'files.mkdir')
      const { path, recursive = false, signal = null } = request
      if (typeof recursive !== 'boolean') throw new TypeError('recursive must be a boolean')
      return operate('mkdir', request, async () => {
        const directories = recursive
          ? path.split('/').filter(Boolean).map((_part, index, parts) => '/' + parts.slice(0, index + 1).join('/'))
          : [path]
        for (const directory of directories) {
          checkAbort(signal)
          // Keep the longest valid Unicode path within one friendly REPL line.
          // In recursive mode EEXIST is success only when the entry is a directory.
          const exists = recursive ? `
 if r==17:
  try:r=0 if os.stat(p)[0]&16384 else 20
  except OSError as e:r=e.args[0]` : ''
          const assignment = `p=${JSON.stringify(directory)}`
          const body = `r=0
try:os.mkdir(p)
except OSError as e:
 r=e.args[0]${exists}
print('__EVMK__',r)`
          let command = `import os
${assignment}
${body}`
          if (evReplExecInputBytes(command) > (session.repl.inputMaxBytes ?? 511)) {
            await session.repl.execPy(assignment, { signal })
            checkAbort(signal)
            command = `import os
${body}`
          }
          const response = await session.repl.execPy(command, { signal })
          const match = /(?:^|\n)__EVMK__\s+(\d+)\s*$/.exec(response.trim())
          if (!match) throw Object.assign(new Error('Incomplete directory creation response'), { code: 'DEVICE_DIRECTORY_WRITE_INCOMPLETE' })
          const errno = Number(match[1])
          if (errno) throw Object.assign(new Error(`Cannot create directory ${directory}: OSError ${errno}`), { code: 'FILESYSTEM_ERROR', errno })
        }
        return { ok: true }
      })
    },
  })
}
