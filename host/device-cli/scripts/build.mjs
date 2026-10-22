// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import { build } from 'esbuild'
import { nodeAdapterAssets } from '@esp-vision/device-core/node'
import { chmod, copyFile, mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises'
import { createRequire } from 'node:module'
import { dirname, join, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const dist = join(root, 'dist')
const licenses = join(root, 'licenses')
const require = createRequire(import.meta.url)

async function packageRoot(name, from) {
  let entry
  try { entry = require.resolve(name, { paths: [from] }) }
  catch (cause) { throw new Error(`Cannot resolve bundled dependency ${name}`, { cause }) }
  let directory = dirname(entry)
  while (directory !== dirname(directory)) {
    try {
      const manifest = JSON.parse(await readFile(join(directory, 'package.json'), 'utf8'))
      if (manifest.name === name) return { directory, manifest }
    } catch { /* Keep walking towards node_modules. */ }
    directory = dirname(directory)
  }
  throw new Error(`Cannot locate package metadata for ${name}`)
}

function packageNameFromInput(input) {
  const normalized = input.replaceAll('\\', '/')
  const marker = 'node_modules/'
  const index = normalized.lastIndexOf(marker)
  if (index < 0) return null
  const parts = normalized.slice(index + marker.length).split('/')
  if (parts[0].startsWith('@')) return parts[1] ? `${parts[0]}/${parts[1]}` : null
  return parts[0] || null
}

async function packageMetadataFromInput(input, name) {
  let directory = dirname(resolve(root, input))
  for (;;) {
    try {
      const manifest = JSON.parse(await readFile(join(directory, 'package.json'), 'utf8'))
      if (manifest.name === name) return { directory, manifest }
    } catch { /* Walk from the input file to its package root. */ }
    const parent = dirname(directory)
    if (parent === directory) break
    directory = parent
  }
  throw new Error(`Cannot locate package metadata for bundled dependency ${name} from ${input}`)
}

async function originalLegalComments(metafile) {
  const comments = new Map()
  for (const input of Object.keys(metafile.inputs)) {
    const source = await readFile(resolve(root, input), 'utf8')
    for (const match of source.matchAll(/\/\*[\s\S]*?\*\/|\/\/[^\r\n]*/g)) {
      const comment = match[0].trim()
      if (!/(?:SPDX-(?:FileCopyrightText|License-Identifier)|copyright|@license|licensed under)/i.test(comment)) continue
      const files = comments.get(comment) ?? new Set()
      files.add(input.replaceAll('\\', '/'))
      comments.set(comment, files)
    }
  }
  return [...comments].sort(([left], [right]) => left.localeCompare(right)).map(([comment, files], index) => [
    `### Source notice ${index + 1}`,
    '',
    `Bundled from: ${[...files].sort().map(file => `\`${file}\``).join(', ')}`,
    '',
    '```text',
    comment,
    '```',
    '',
  ].join('\n')).join('\n')
}

async function copyOriginalLicenses(metafile) {
  await rm(licenses, { recursive: true, force: true })
  await mkdir(licenses, { recursive: true })
  const records = []
  const bundledPackages = new Map()
  for (const input of Object.keys(metafile.inputs)) {
    const name = packageNameFromInput(input)
    if (name && name !== '@esp-vision/device-core' && !bundledPackages.has(name)) bundledPackages.set(name, input)
  }
  for (const [name, input] of [...bundledPackages].sort(([left], [right]) => left.localeCompare(right))) {
    const { directory, manifest } = await packageMetadataFromInput(input, name)
    const safe = name.replaceAll('/', '__').replace(/^@/, '')
    const entries = await readdir(directory)
    const candidates = entries.filter(entry => /^(license|licence|copying)(?:[.-].*|$)/i.test(entry))
    if (!candidates.length) throw new Error(`Bundled dependency ${name}@${manifest.version} has no original license file`)
    for (const file of candidates) await copyFile(join(directory, file), join(licenses, `${safe}-${file}`))
    records.push({ name, version: manifest.version, license: manifest.license ?? 'See included license file', files: candidates.map(file => `${safe}-${file}`) })
  }
  const lines = [
    '# Third-party notices',
    '',
    'The CLI bundle includes the following third-party packages. Their original license files are in `licenses/`.',
    '',
    '| Package | Version | License | License file |',
    '| --- | --- | --- | --- |',
    ...records.map(record => `| ${record.name} | ${record.version} | ${record.license} | ${record.files.map(file => `\`${file}\``).join(', ')} |`),
    '',
    '## Preserved source license comments',
    '',
    'Original copyright and SPDX comments from bundled source files are reproduced below. Recognized legal comments also remain inline in `dist/device-cli.mjs`.',
    '',
    await originalLegalComments(metafile),
  ]
  await writeFile(join(root, 'THIRD_PARTY_NOTICES.md'), lines.join('\n'))
}

await rm(dist, { recursive: true, force: true })
await mkdir(dist, { recursive: true })
const bundle = await build({
  entryPoints: [join(root, 'src/cli.js')],
  outfile: join(dist, 'device-cli.mjs'),
  bundle: true,
  platform: 'node',
  format: 'esm',
  target: 'node22',
  sourcemap: false,
  legalComments: 'inline',
  metafile: true,
})
const adapters = nodeAdapterAssets()
for (const [key, filename] of [['serial', 'node_serial.py'], ['rom', 'node_rom.py']]) {
  const asset = adapters[key]
  if (!(asset instanceof URL) || asset.protocol !== 'file:') throw new Error(`Core node adapter ${key} must be a file URL`)
  await copyFile(fileURLToPath(asset), join(dist, filename))
}
await chmod(join(dist, 'device-cli.mjs'), 0o755)
await copyOriginalLicenses(bundle.metafile)

// Keep the metadata synchronized with the exact Core package used to build.
const core = await packageRoot('@esp-vision/device-core', root)
await copyFile(join(core.directory, 'LICENSE'), join(root, 'LICENSE'))
await copyFile(join(core.directory, 'NOTICE'), join(root, 'DEVICE_CORE_NOTICE.md'))
console.log('Built dist/device-cli.mjs with bundled Node dependencies and Python adapters.')
