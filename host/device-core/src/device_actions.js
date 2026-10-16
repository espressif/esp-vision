/*
 * SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
 * SPDX-License-Identifier: Apache-2.0
 */

export const DEVICE_ACTION_SCHEMA_VERSION = 1

export const DEVICE_ACTORS = Object.freeze([
  'agent',
])

function objectSchema(properties = {}, required = []) {
  return {
    type: 'object',
    properties,
    ...(required.length ? { required } : {}),
    additionalProperties: false,
  }
}

function outputSchema(properties = {}) {
  return { type: 'object', properties, additionalProperties: true }
}

function action(name, description, inputSchema, {
  output = outputSchema(),
  readOnly = false,
  destructive = false,
  openWorld = false,
  requiresReadyDevice = true,
  concurrency = 'exclusive',
} = {}) {
  return Object.freeze({
    name,
    description,
    inputSchema,
    outputSchema: output,
    risk: Object.freeze({ readOnly, destructive, openWorld }),
    requiresReadyDevice,
    concurrency,
  })
}

export const DEVICE_ACTION_DESCRIPTORS = Object.freeze([
  action('device.listPorts', 'List platform-native serial candidates for the user to select; this action never chooses a device.', objectSchema(), {
    readOnly: true,
    requiresReadyDevice: false,
    concurrency: 'shared-read',
  }),
  action('device.connect', 'Connect to an ESP Vision device using standard elicitation for a serial port or an explicit device TCP address, then acquire an EV-MUX v4 session.', objectSchema({
    transport: {
      enum: ['serial', 'tcp'],
      default: 'serial',
      description: 'Use serial for a platform port or tcp for an IP address on the device network.',
    },
    port: {
      type: 'string',
      description: 'Platform-native serial identifier, such as /dev/ttyACM0 or /dev/cu.usbmodem*. Omit it only when the MCP host advertises elicitation or ESP_VISION_PORT is configured. Non-interactive callers must pass it explicitly. A direct network address may also be supplied as tcp://IP:PORT or IP:PORT; alternatively set transport to tcp and provide host and tcpPort.',
    },
    host: { type: 'string', description: 'IPv4 or IPv6 address of the device when transport is tcp.' },
    tcpPort: { type: 'integer', minimum: 1, maximum: 65535, default: 3232, description: 'Device EV-MUX TCP port.' },
    baudRate: { type: 'integer', minimum: 1200, maximum: 4000000, default: 115200 },
    drainMs: { type: 'integer', minimum: 0, maximum: 3000, default: 300 },
  }), { openWorld: true, requiresReadyDevice: false }),
  action('device.disconnect', 'Disconnect the active device session and release its lease.', objectSchema(), {
    destructive: true,
    requiresReadyDevice: false,
  }),
  action('device.negotiate', 'Discover EV-MUX v4, explicitly acquire a session, and prove the framed REPL is live.', objectSchema({
    role: { type: 'string', default: 'unknown' },
    drainMs: { type: 'integer', minimum: 0, maximum: 3000, default: 300 },
  }), { requiresReadyDevice: false }),
  action('device.inspect', 'Inspect protocol state, device identity, filesystem, and recent logs.', objectSchema({
    includeFiles: { type: 'boolean', default: true },
    logLimit: { type: 'integer', minimum: 0, maximum: 500, default: 100 },
  }), { readOnly: true, concurrency: 'shared-read' }),
  action('python.execute', 'Execute MicroPython through the shared interpreter using bounded REPL or binary file staging.', objectSchema({
    code: { type: 'string' },
    timeoutMs: { type: 'integer', minimum: 100, maximum: 120000, default: 8000 },
  }, ['code']), { destructive: true, openWorld: true }),
  action('python.runEphemeral', 'Run one Python draft from the reserved temporary device path without modifying a user-owned file.', objectSchema({
    code: { type: 'string' },
    maxDurationMs: { type: 'integer', minimum: 1000, maximum: 180000, default: 180000 },
  }, ['code']), { destructive: true, openWorld: true }),
  action('python.observeEphemeral', 'Observe the bounded output and status of the current temporary Python run.', objectSchema({
    runId: { type: 'string' },
    waitMs: { type: 'integer', minimum: 0, maximum: 10000, default: 1000 },
    outputOffset: { type: 'integer', minimum: 0, maximum: 1048576, default: 0 },
  }, ['runId']), { readOnly: true, concurrency: 'shared-read' }),
  action('python.stopEphemeral', 'Stop the current temporary Python run and remove its reserved device file.', objectSchema({
    runId: { type: 'string' },
  }, ['runId']), { destructive: true }),
  action('deviceFile.read', 'Read a file from the device filesystem.', objectSchema({
    path: { type: 'string' },
    encoding: { enum: ['utf-8', 'base64'], default: 'utf-8' },
  }, ['path']), { readOnly: true }),
  action('deviceFile.write', 'Write a file to the device using the negotiated transfer path.', objectSchema({
    path: { type: 'string' },
    content: { type: 'string' },
    encoding: { enum: ['utf-8', 'base64'], default: 'utf-8' },
    runAfterWrite: { type: 'boolean', default: false },
  }, ['path', 'content']), { destructive: true, openWorld: true }),
  action('workspaceFile.deploy', 'Stream one workspace-relative file to the device without placing binary content in the model context.', objectSchema({
    sourcePath: { type: 'string', description: 'Workspace-relative source file path.' },
    devicePath: { type: 'string', description: 'Absolute destination path on the device.' },
    expectedBytes: { type: 'integer', minimum: 0, maximum: 16777216 },
    expectedSha256: { type: 'string', description: 'Optional lowercase SHA-256 expected before transfer.' },
    overwrite: { type: 'boolean', default: false },
  }, ['sourcePath', 'devicePath']), { destructive: true, openWorld: true }),
  action('deviceFile.remove', 'Remove one file or one empty directory from the device filesystem.', objectSchema({
    path: { type: 'string' },
    kind: { enum: ['file', 'directory'], default: 'file' },
  }, ['path']), { destructive: true, openWorld: true }),
  action('deviceTest.run', 'Write, run, and capture a target-side MicroPython test.', objectSchema({
    path: { type: 'string', default: '/.agent_test.py' },
    source: { type: 'string' },
    timeoutMs: { type: 'integer', minimum: 500, maximum: 120000, default: 10000 },
  }, ['source']), { destructive: true, openWorld: true }),
  action('device.control', 'Interrupt, reset, or enter the bootloader.', objectSchema({
    operation: { enum: ['stop', 'soft-reset', 'hard-reset', 'bootloader'] },
    confirmBootloader: { type: 'boolean', default: false },
  }, ['operation']), { destructive: true }),
  action('device.rpc', 'Call a negotiated user or debug RPC method. For network.configure, network.enable, and network.forget, include a 32-character lowercase hexadecimal operationId and reuse the exact same ID and payload when retrying after a timeout.', objectSchema({
    method: { type: 'string' },
    payload: { type: 'object', default: {} },
    channel: { enum: ['user.rpc', 'debug.rpc'] },
    timeoutMs: { type: 'integer', minimum: 100, maximum: 120000, default: 4000 },
  }, ['method']), { destructive: true, openWorld: true }),
  action('frame.capture', 'Capture one JPEG frame for an explicitly requested observation.', objectSchema({
    timeoutMs: { type: 'integer', minimum: 500, maximum: 30000, default: 8000 },
  }), { readOnly: true }),
])

function actionAbortError(message = 'Device action cancelled') {
  const error = new Error(message)
  error.name = 'AbortError'
  return error
}

export class DeviceActionError extends Error {
  constructor(code, message, { recovery = null, details = null, cause = null } = {}) {
    super(message, cause ? { cause } : undefined)
    this.name = 'DeviceActionError'
    this.code = code
    this.recovery = recovery
    this.details = details
  }

  toJSON() {
    return {
      code: this.code,
      message: this.message,
      ...(this.recovery ? { recovery: this.recovery } : {}),
      ...(this.details ? { details: this.details } : {}),
    }
  }
}

export class DeviceLeaseConflictError extends DeviceActionError {
  constructor(owner, requester) {
    super('DEVICE_BUSY', `Device is controlled by ${owner}; ${requester} cannot acquire it`, {
      recovery: 'Disconnect the current controller or perform an explicit handoff, then retry.',
      details: { owner, requester },
    })
    this.name = 'DeviceLeaseConflictError'
  }
}

function assertActor(actor) {
  if (!DEVICE_ACTORS.includes(actor)) {
    throw new DeviceActionError('INVALID_ACTOR', `Unsupported device actor: ${actor}`, {
      recovery: `Use one of: ${DEVICE_ACTORS.join(', ')}.`,
    })
  }
}

export class DeviceLease {
  constructor() {
    this.primary = null
    this.holders = new Map()
    this.sequence = 1
  }

  acquire(actor) {
    assertActor(actor)
    if (this.primary && actor !== this.primary) {
      throw new DeviceLeaseConflictError(this.primary, actor)
    }
    if (!this.primary) this.primary = actor
    const id = `lease-${this.sequence++}`
    this.holders.set(id, actor)
    let released = false
    return Object.freeze({
      id,
      actor,
      release: () => {
        if (released) return
        released = true
        this.release(id)
      },
    })
  }

  release(id) {
    this.holders.delete(id)
    if (this.holders.size === 0) {
      this.primary = null
      return
    }
    if (![...this.holders.values()].includes(this.primary)) {
      this.primary = this.holders.values().next().value || null
    }
  }

  assertAccess(actor) {
    assertActor(actor)
    if (!this.primary) {
      throw new DeviceActionError('DEVICE_LEASE_REQUIRED', 'No actor owns the active device session', {
        recovery: 'Connect the device and acquire its lease before invoking device actions.',
      })
    }
    if (![...this.holders.values()].includes(actor)) {
      throw new DeviceLeaseConflictError(this.primary, actor)
    }
  }

  snapshot() {
    return {
      state: this.primary ? 'held' : 'free',
      primary: this.primary,
      actors: [...new Set(this.holders.values())],
      holders: this.holders.size,
    }
  }
}

function validateValue(value, schema, field) {
  if (value === undefined) return
  if (schema.enum && !schema.enum.includes(value)) {
    throw new DeviceActionError('INVALID_INPUT', `${field} must be one of: ${schema.enum.join(', ')}`)
  }
  if (schema.type === 'string' && typeof value !== 'string') {
    throw new DeviceActionError('INVALID_INPUT', `${field} must be a string`)
  }
  if (schema.type === 'boolean' && typeof value !== 'boolean') {
    throw new DeviceActionError('INVALID_INPUT', `${field} must be a boolean`)
  }
  if (schema.type === 'object' && (typeof value !== 'object' || value === null || Array.isArray(value))) {
    throw new DeviceActionError('INVALID_INPUT', `${field} must be an object`)
  }
  if (schema.type === 'integer' && !Number.isInteger(value)) {
    throw new DeviceActionError('INVALID_INPUT', `${field} must be an integer`)
  }
  if (typeof value === 'number' && schema.minimum !== undefined && value < schema.minimum) {
    throw new DeviceActionError('INVALID_INPUT', `${field} must be at least ${schema.minimum}`)
  }
  if (typeof value === 'number' && schema.maximum !== undefined && value > schema.maximum) {
    throw new DeviceActionError('INVALID_INPUT', `${field} must be at most ${schema.maximum}`)
  }
}

function applyDefaultsAndValidate(input, schema) {
  const value = input ?? {}
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new DeviceActionError('INVALID_INPUT', 'Device action input must be an object')
  }
  const properties = schema.properties || {}
  if (schema.additionalProperties === false) {
    const unknown = Object.keys(value).filter((key) => !(key in properties))
    if (unknown.length) {
      throw new DeviceActionError('INVALID_INPUT', `Unknown input fields: ${unknown.join(', ')}`)
    }
  }
  const result = { ...value }
  for (const [field, fieldSchema] of Object.entries(properties)) {
    if (result[field] === undefined && fieldSchema.default !== undefined) {
      result[field] = fieldSchema.default
    }
    validateValue(result[field], fieldSchema, field)
  }
  for (const field of schema.required || []) {
    if (result[field] === undefined) {
      throw new DeviceActionError('INVALID_INPUT', `Missing required input: ${field}`)
    }
  }
  return result
}

export class DeviceActionRegistry {
  constructor(descriptors = DEVICE_ACTION_DESCRIPTORS, { schemaVersion = DEVICE_ACTION_SCHEMA_VERSION } = {}) {
    this.schemaVersion = schemaVersion
    this.descriptors = new Map()
    this.handlers = new Map()
    for (const descriptor of descriptors) this.register(descriptor)
  }

  register(descriptor, handler = null) {
    if (!descriptor?.name || !descriptor.inputSchema || !descriptor.outputSchema || !descriptor.risk) {
      throw new Error('Invalid device action descriptor')
    }
    if (this.descriptors.has(descriptor.name)) {
      throw new Error(`Duplicate device action: ${descriptor.name}`)
    }
    this.descriptors.set(descriptor.name, Object.freeze({ ...descriptor }))
    if (handler) this.bind(descriptor.name, handler)
    return this
  }

  bind(name, handler) {
    if (!this.descriptors.has(name)) throw new Error(`Unknown device action: ${name}`)
    if (typeof handler !== 'function') throw new Error(`Device action handler must be a function: ${name}`)
    this.handlers.set(name, handler)
    return this
  }

  descriptor(name) {
    return this.descriptors.get(name) || null
  }

  handler(name) {
    return this.handlers.get(name) || null
  }

  list() {
    return [...this.descriptors.values()]
  }

  validate(name, input) {
    const descriptor = this.descriptor(name)
    if (!descriptor) {
      throw new DeviceActionError('UNKNOWN_ACTION', `Unknown device action: ${name}`)
    }
    return applyDefaultsAndValidate(input, descriptor.inputSchema)
  }
}

export class OperationCoordinator {
  constructor() {
    this.queue = []
    this.active = null
    this.sequence = 1
    this.closed = false
  }

  run({ actor, requestId, actionName, signal }, operation) {
    assertActor(actor)
    if (this.closed) return Promise.reject(new DeviceActionError('COORDINATOR_CLOSED', 'Device operation coordinator is closed'))
    if (signal?.aborted) return Promise.reject(actionAbortError())

    return new Promise((resolve, reject) => {
      const controller = new AbortController()
      const item = {
        id: this.sequence++,
        actor,
        requestId,
        actionName,
        operation,
        controller,
        resolve,
        reject,
        removeAbortListener: null,
      }
      if (signal) {
        const abort = () => controller.abort(signal.reason || actionAbortError())
        signal.addEventListener('abort', abort, { once: true })
        item.removeAbortListener = () => signal.removeEventListener('abort', abort)
      }
      this.queue.push(item)
      this._drain()
    })
  }

  close(reason = actionAbortError('Device session closed')) {
    this.closed = true
    for (const item of this.queue.splice(0)) {
      item.controller.abort(reason)
      item.removeAbortListener?.()
      item.reject(reason)
    }
    this.active?.controller.abort(reason)
  }

  snapshot() {
    return {
      active: this.active ? {
        actor: this.active.actor,
        requestId: this.active.requestId,
        actionName: this.active.actionName,
      } : null,
      queued: this.queue.map(({ actor, requestId, actionName }) => ({ actor, requestId, actionName })),
    }
  }

  _drain() {
    if (this.active || this.closed) return
    const item = this.queue.shift()
    if (!item) return
    if (item.controller.signal.aborted) {
      item.removeAbortListener?.()
      item.reject(item.controller.signal.reason || actionAbortError())
      this._drain()
      return
    }
    this.active = item
    Promise.resolve()
      .then(() => item.operation(item.controller.signal))
      .then(item.resolve, item.reject)
      .finally(() => {
        item.removeAbortListener?.()
        this.active = null
        this._drain()
      })
  }
}

function normalizeActionError(error, actionName) {
  if (error instanceof DeviceActionError) return error
  if (error?.name === 'AbortError') {
    return new DeviceActionError('ACTION_CANCELLED', `${actionName} was cancelled`, {
      recovery: 'Inspect the current device state before retrying.',
      cause: error,
    })
  }
  const message = error?.message || String(error)
  if (error?.code === 'INVALID_PATH' || error?.code === 'PATH_TOO_LONG') {
    return new DeviceActionError(error.code, message, {
      recovery: 'Use a path within the device UTF-8 byte limit and without forbidden characters, then retry.',
      cause: error,
    })
  }
  if (error?.code === 'EACCES' || error?.code === 'EPERM' || /permission denied/i.test(message)) {
    return new DeviceActionError('PORT_PERMISSION_DENIED', message, {
      recovery: 'Grant serial-device access to the current user, reconnect the device, and retry.',
      cause: error,
    })
  }
  if (error?.code === 'EBUSY' || error?.code === 'EAGAIN' || /device or resource busy|resource busy|port.+busy/i.test(message)) {
    return new DeviceActionError('DEVICE_BUSY', message, {
      recovery: 'Disconnect the process that currently owns this serial port, wait for the port to be released, and retry the explicit handoff.',
      cause: error,
    })
  }
  if (error?.code === 'VERSION_MISMATCH' || /firmware|EV-MUX v4|missing EV-MUX/i.test(message)) {
    return new DeviceActionError('FIRMWARE_INCOMPATIBLE', message, {
      recovery: 'Install firmware that explicitly supports EV-MUX v4 and the requested features, reconnect, and retry.',
      cause: error,
    })
  }
  return new DeviceActionError('ACTION_FAILED', message, {
    recovery: 'Inspect device state and recent logs, then retry the smallest failing operation.',
    details: { actionName },
    cause: error,
  })
}

export class DeviceActionGateway {
  constructor({
    registry = new DeviceActionRegistry(),
    coordinator = new OperationCoordinator(),
    lease = new DeviceLease(),
    getSession = () => null,
    authorize = async () => true,
  } = {}) {
    this.registry = registry
    this.coordinator = coordinator
    this.lease = lease
    this.getSession = getSession
    this.authorize = authorize
  }

  async invoke(actionName, input = {}, context = {}) {
    const actor = context.actor
    assertActor(actor)
    if (!context.requestId) {
      throw new DeviceActionError('REQUEST_ID_REQUIRED', 'Device actions require a requestId')
    }
    const descriptor = this.registry.descriptor(actionName)
    if (!descriptor) throw new DeviceActionError('UNKNOWN_ACTION', `Unknown device action: ${actionName}`)
    const validatedInput = this.registry.validate(actionName, input)
    const handler = this.registry.handler(actionName)
    if (!handler) throw new DeviceActionError('ACTION_UNAVAILABLE', `No handler is bound for ${actionName}`)

    if (descriptor.requiresReadyDevice) {
      this.lease.assertAccess(actor)
      const session = this.getSession()
      if (!session || session.state !== 'ready') {
        throw new DeviceActionError('DEVICE_NOT_READY', 'No ready ESP Vision device session', {
          recovery: 'Connect the device and complete EV-MUX negotiation before retrying.',
        })
      }
    }

    const authorized = await this.authorize(descriptor, validatedInput, { ...context, actor })
    if (!authorized) {
      throw new DeviceActionError('ACTION_NOT_AUTHORIZED', `${actionName} was not authorized`, {
        recovery: 'Request approval with a narrower action or input scope.',
      })
    }

    try {
      return await this.coordinator.run({
        actor,
        requestId: context.requestId,
        actionName,
        signal: context.signal,
      }, (signal) => handler(validatedInput, {
        ...context,
        actor,
        signal,
        descriptor,
        session: this.getSession(),
      }))
    } catch (error) {
      throw normalizeActionError(error, actionName)
    }
  }
}
