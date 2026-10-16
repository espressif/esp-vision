// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict'

// Publication is one write. Only reads are retried while registry metadata propagates.
export async function verifyPublishedIntegrity(url, integrity, {
  fetcher = globalThis.fetch,
  wait = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds)),
  report = message => console.log(message),
} = {}) {
  const attempts = 10
  for (let attempt = 0; attempt < attempts; attempt++) {
    let response, transient
    try {
      response = await fetcher(url, { cache: 'no-store', headers: { 'Cache-Control': 'no-cache' }, signal: AbortSignal.timeout(15000) })
    } catch (error) {
      if (!(error instanceof TypeError) && !['TimeoutError', 'AbortError'].includes(error.name)) throw error
      transient = 'network timeout or unavailable'
    }
    if (response?.ok) {
      const metadata = await response.json()
      assert.equal(metadata.dist?.integrity, integrity, 'Published tarball integrity mismatch')
      return
    }
    if (response) {
      const retryable = response.status === 404 || response.status === 429 || response.status >= 500 && response.status <= 599
      await response.body?.cancel()
      assert.ok(retryable, `Registry verification failed: HTTP ${response.status}`)
      transient = `HTTP ${response.status}`
    }
    assert.ok(attempt + 1 < attempts, `Published metadata verification unavailable after ${attempts} reads (${transient}); retry the CI job, not publication manually`)
    const delay = Math.min(30000, 1000 * 2 ** attempt)
    report(`Registry verification ${transient}; retrying read ${attempt + 2}/${attempts} in ${delay}ms`)
    await wait(delay)
  }
}
