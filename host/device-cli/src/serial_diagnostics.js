// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// Diagnostics never start an interpreter or install packages.
export async function explainSerialFailure(failure) {
  const messages = {
    SERIAL_BUSY: 'Serial port is busy. Close the IDE, serial monitor or other process using this port before retrying.',
    SERIAL_ACCESS_DENIED: 'Serial port access was denied. Check port permissions and close other applications using it; Windows may report an occupied port as access denied.',
    SERIAL_BACKEND_UNAVAILABLE: 'The npm serial backend could not load. Reinstall the CLI for your operating system and architecture with optional dependencies enabled.',
  }
  if (messages[failure?.code]) failure.message = messages[failure.code]
  return failure
}
