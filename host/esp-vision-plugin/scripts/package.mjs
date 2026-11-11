// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { chmod, copyFile, cp, mkdir, readFile, rm, writeFile } from 'node:fs/promises'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { syncHostManifests } from './host-manifests.mjs'

const pluginRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
await syncHostManifests(pluginRoot)
const cliPackage = process.env.ESP_VISION_PLUGIN_CLI_PACKAGE
  ? resolve(process.env.ESP_VISION_PLUGIN_CLI_PACKAGE)
  : resolve(pluginRoot, '../device-cli')
const built = resolve(cliPackage, 'dist')
const cli = resolve(pluginRoot, 'cli')

await rm(cli, { recursive: true, force: true })
await rm(resolve(pluginRoot, 'server'), { recursive: true, force: true })
await mkdir(cli, { recursive: true })
await Promise.all([
  ...['device-cli.mjs'].map(name =>
    copyFile(resolve(built, name), resolve(cli, name))),
  cp(resolve(built, 'native'), resolve(cli, 'native'), { recursive: true }),
  copyFile(resolve(cliPackage, 'DEVICE_CORE_NOTICE.md'), resolve(pluginRoot, 'DEVICE_CORE_NOTICE.md')),
  cp(resolve(cliPackage, 'licenses'), resolve(cli, 'licenses'), { recursive: true }),
])
await chmod(resolve(cli, 'device-cli.mjs'), 0o755)

const notices = await readFile(resolve(cliPackage, 'THIRD_PARTY_NOTICES.md'), 'utf8')
await writeFile(resolve(pluginRoot, 'THIRD_PARTY_NOTICES.md'), notices.replaceAll('`licenses/', '`cli/licenses/'))
