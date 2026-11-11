// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
export function serialFixture() {
  const events = [], input = []
  let pending
  const handle = {
    isOpen: true,
    async set(value) { events.push(['signals', value]) },
    async update(value) { events.push(['baud', value]) },
    async write(bytes) { events.push(['write', [...bytes]]) },
    async drain() { events.push(['drain']) },
    async close() { if (!this.isOpen) return; this.isOpen = false; events.push(['close']); pending?.reject(Object.assign(new Error('Closed'), { canceled: true })); pending = null },
    read(buffer) {
      return new Promise((resolve, reject) => { pending = { buffer, resolve, reject }; flush() })
    },
  }
  function flush() {
    if (!pending || !input.length) return
    const current = pending; pending = null
    const value = input.shift()
    if (value instanceof Error) current.reject(value)
    else { current.buffer.set(value); current.resolve({ bytesRead: value.length }) }
  }
  const binding = async () => ({
    async open(options) { events.push(['open', options]); handle.isOpen = true; return handle },
    async list() { return [] },
  })
  return { events, handle, binding, push(value) { input.push(value); flush() } }
}
