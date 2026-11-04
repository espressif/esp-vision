// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { execFile } from 'node:child_process'
import { promisify } from 'node:util'

// Run only after a serial failure. No port is opened, alternate executable is
// tried, package is installed, or user-supplied text evaluated by a shell.
const check = `import sys
try:
 import serial
except ModuleNotFoundError as error:
 sys.exit(71 if error.name == 'serial' else 73)
except Exception:
 sys.exit(73)
sys.exit(0 if callable(getattr(serial, 'Serial', None)) else 72)
`

export async function explainSerialFailure(failure, options, dependencies = {}) {
  if (failure?.code === 'SERIAL_BUSY') {
    failure.message = 'Serial port is busy. Close the IDE, serial monitor or other process using this port before retrying.'
    return failure
  }
  if (failure?.code === 'SERIAL_ACCESS_DENIED') {
    failure.message = 'Serial port access was denied. Check port permissions and close other applications using it; Windows may report an occupied port as access denied.'
    return failure
  }
  if (!['SERIAL_ERROR', 'SERIAL_UNAVAILABLE'].includes(failure?.code)) return failure
  if (dependencies.signal?.aborted) return failure
  // A bridge that reached read/write has already imported pyserial. Preserve
  // its runtime diagnosis rather than misclassifying a later cable/OS failure.
  if (['read', 'write', 'stdout', 'stdin'].includes(failure.details?.stage)) return failure
  let reason = 'unknown'
  try {
    await (dependencies.execFile ?? promisify(execFile))(options.python, ['-c', check], {
      signal: dependencies.signal, timeout: Math.min(options.timeout ?? 3000, 3000),
      maxBuffer: 4096, windowsHide: true,
    })
    reason = 'available'
  } catch (cause) {
    if (dependencies.signal?.aborted) return failure
    if (cause.code === 'ENOENT') reason = 'missing-python'
    else if (cause.code === 'EACCES') reason = 'python-permission'
    else if (cause.code === 71) reason = 'missing-pyserial'
    else if (cause.code === 72) reason = 'invalid-pyserial'
    else if (cause.code === 73) reason = 'import-failed'
    // Never copy stderr, stdout, paths or arbitrary exception text.
  }
  const messages = {
    'missing-python': 'The configured Python executable was not found. Check python3 --version (Windows: python --version or py --version), then select an installed interpreter with --python PATH; Windows Python Launcher users can use --python py.',
    'python-permission': 'The configured Python executable could not be started because access was denied. Check executable permissions or select an accessible interpreter with --python PATH.',
    'missing-pyserial': 'The configured Python interpreter has no pyserial module. Install it in that same interpreter with PYTHON -m pip install pyserial (Windows Launcher: py -m pip install pyserial and --python py), then verify PYTHON -c "import serial". Replace PYTHON with the executable selected by --python.',
    'invalid-pyserial': 'The configured Python imported a serial module without the pyserial Serial API. Check for a conflicting serial.py or serial package; verify pyserial in the interpreter selected by --python.',
    'import-failed': 'The configured Python could not import pyserial successfully. Run PYTHON -c "import serial" in the same interpreter to inspect the environment; select it with --python PATH.',
    available: 'Python and the pyserial Serial API are available. Check the selected port, cable, OS permissions and whether another application owns the port. The serial failure does not establish a missing dependency.',
    unknown: 'Serial adapter failed; Python/pyserial availability could not be established. Check PYTHON --version and PYTHON -c "import serial", select that interpreter with --python PATH (Windows Launcher: --python py), and check port permissions and other applications using the port.',
  }
  failure.message = messages[reason]
  return failure
}
