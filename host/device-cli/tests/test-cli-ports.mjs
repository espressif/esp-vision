// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import test from 'node:test'
import assert from 'node:assert/strict'
import { filterSerialPorts, inventory } from '@esp-vision/device-core/node'
import { parseCli, runCli } from '../src/cli.js'

const names = (ports, platform, allPorts = false) => filterSerialPorts(ports, { platform, allPorts }).map(port => port.path)

test('Linux USB candidates survive missing descriptors and unrecognized vendors; native ports are opt-in', () => {
  const ports = ['/dev/ttyACM0', '/dev/ttyUSB3', '/dev/ttyXRUSB0', '/dev/ttyS0', '/dev/ttyAMA0', '/dev/ttyTHS1', '/dev/rfcomm0', '/dev/tty0', '/dev/console', '/dev/pts/1', 'COM3'].map(path => ({ path }))
  ports.push({ path: '/dev/customDriver0', vendorId: 0x9999 }, { path: '/dev/another0', hwid: 'USB VID:PID=9999:0001' })
  assert.deepEqual(names(ports, 'linux'), ['/dev/ttyACM0', '/dev/ttyUSB3', '/dev/ttyXRUSB0', '/dev/customDriver0', '/dev/another0'])
  assert.deepEqual(names(ports, 'linux', true), [...ports.slice(0, 7), ...ports.slice(11)].map(port => port.path))
})

test('Windows COM10+ and extended handles are accepted; native and virtual COM ports require all-ports', () => {
  const ports = [{ path: 'COM3', vendorId: 0x303a }, { path: 'COM10', vendorId: 0x9999 }, { path: '\\\\.\\COM256', hwid: 'USB VID:PID=9999:0001' }, { path: 'com10', vendorId: 0x9999 }, { path: 'COM1' }, { path: 'COM7', hwid: 'BTHENUM' }, ...['COM0', 'COM-1', 'LPT1', '/dev/ttyUSB0', 'COM3\n'].map(path => ({ path, vendorId: 0x303a }))]
  assert.deepEqual(names(ports, 'win32'), ['COM3', 'COM10', '\\\\.\\COM256'])
  assert.deepEqual(names(ports, 'win32', true), ['COM3', 'COM10', '\\\\.\\COM256', 'COM1', 'COM7'])
})

test('macOS prefers callout endpoints without merging separate USB interfaces', () => {
  const paths = ['/dev/tty.usbmodem101', '/dev/cu.usbmodem101', '/dev/cu.usbmodem102', '/dev/cu.usbserial-A', '/dev/cu.wchusbserial42', '/dev/cu.SLAB_USBtoUART', '/dev/cu.Bluetooth-Incoming-Port', '/dev/cu.debug-console', '/dev/tty.usbserial-B']
  const ports = paths.map(path => ({ path }))
  ports.push({ path: '/dev/cu.vendor device', vendorId: 0x9999 }, { path: '/dev/ttyACM0', vendorId: 0x303a })
  const expected = [...paths.slice(1, 6), paths[8], '/dev/cu.vendor device']
  assert.deepEqual(names(ports, 'darwin'), expected)
  assert.deepEqual(names(ports, 'darwin', true), [...paths.slice(1), '/dev/cu.vendor device'])
})

test('inspect all-ports reaches passive enumeration and default query remains inspect', async () => {
  assert.equal(parseCli([]).command, 'inspect')
  assert.equal(parseCli(['inspect']).allPorts, false)
  const options = parseCli(['inspect', '--all-ports', '--interface', 'serial'])
  const result = await runCli(options, { platform: 'linux', inventory, async listSerial() { return [{ path: '/dev/ttyS0' }, { path: '/dev/ttyUSB0' }] }, createDevice() { throw new Error('Opened passive port') } })
  assert.equal(result.devices.length, 2)
  assert.throws(() => parseCli(['configure', '--port', 'COM3', '--forget-wifi', '--all-ports']))
})

test('explicit ports bypass enumeration policy, including UARTs and stable aliases', async () => {
  for (const path of ['/dev/ttyS0', '/dev/serial/by-id/usb-board', '/dev/tty.vendor', 'COM1']) {
    let opened
    await runCli(parseCli(['inspect', '--port', path]), {
      inventory() { throw new Error('Explicit target invoked discovery') }, transportFor: row => row,
      createDevice({ transport }) { return { async connect() { opened = transport.address }, async inspect() { return {} }, async disconnect() {} } },
    })
    assert.equal(opened, path)
  }
})
