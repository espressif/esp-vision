// SPDX-FileCopyrightText: 2026 ESP-VISION-IDE
// SPDX-License-Identifier: Apache-2.0
import { build } from 'esbuild'
import { chmod, copyFile, cp, mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises'
import { createRequire } from 'node:module'
import { dirname, join, resolve, relative } from 'node:path'
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
    if (input.startsWith('native-loader:')) continue
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
    if (input.startsWith('native-loader:')) continue
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
// Keep the original npm native binaries; bundle their JS loader beside them.
// No install-time compilation or Python subprocess runs in the delivered CLI.
const corePackage = await packageRoot('@esp-vision/device-core', root)
const nativePackage = await packageRoot('@serialport/bindings-cpp', corePackage.directory)
const nativeDirectory = join(dist, 'native')
await mkdir(nativeDirectory)
const nativeBundle = await build({
  entryPoints: [require.resolve('@serialport/bindings-cpp', { paths: [corePackage.directory] })],
  outfile: join(nativeDirectory, 'bindings.cjs'), bundle: true, platform: 'node', format: 'cjs', target: 'node22',
  legalComments: 'inline', metafile: true,
  plugins: [{ name: 'native-loader-root', setup(builder) {
    builder.onResolve({ filter: /^node-gyp-build$/ }, () => ({ path: 'loader', namespace: 'native-loader' }))
    builder.onLoad({ filter: /.*/, namespace: 'native-loader' }, () => ({
      contents: `const load = require(${JSON.stringify(require.resolve('node-gyp-build/node-gyp-build.js', { paths: [nativePackage.directory] }))}); module.exports = () => load(__dirname);`,
      loader: 'js', resolveDir: root,
    }))
  } }],
})
await cp(join(nativePackage.directory, 'prebuilds'), join(nativeDirectory, 'prebuilds'), { recursive: true })
await copyFile(join(nativePackage.directory, 'LICENSE'), join(nativeDirectory, 'LICENSE'))
await writeFile(join(nativeDirectory, 'package.json'), JSON.stringify({ name: nativePackage.manifest.name, version: nativePackage.manifest.version, license: nativePackage.manifest.license }) + '\n')
// Koffi 2 includes portable prebuilds in one npm archive. Preserve its loader
// and Linux binaries verbatim; only Linux USJ's modem ioctl fallback uses it.
const koffi = await packageRoot('koffi', corePackage.directory)
const koffiDirectory = join(nativeDirectory, 'koffi')
await mkdir(koffiDirectory)
for (const file of ['index.js', 'package.json', 'LICENSE.txt']) await copyFile(join(koffi.directory, file), join(koffiDirectory, file))
for (const entry of await readdir(join(koffi.directory, 'build/koffi'))) {
  if (/^(linux|musl)_/.test(entry)) await cp(join(koffi.directory, 'build/koffi', entry), join(koffiDirectory, 'build/koffi', entry), { recursive: true })
}
for (const file of ['src/cnoke/LICENSE.txt', 'vendor/node-api-headers/LICENSE', 'vendor/node-addon-api/LICENSE.md']) {
  await mkdir(dirname(join(koffiDirectory, file)), { recursive: true })
  await copyFile(join(koffi.directory, file), join(koffiDirectory, file))
}
const bundle = await build({
  entryPoints: [join(root, 'src/cli.js')],
  outfile: join(dist, 'device-cli.mjs'),
  bundle: true,
  platform: 'node',
  format: 'esm',
  // Bundled CommonJS dependencies (multicast-dns/dns-packet) still require
  // Node built-ins. The build script's own require is not part of the output.
  banner: { js: "import { createRequire as createBundleRequire } from 'node:module'; const require = createBundleRequire(import.meta.url);" },
  target: 'node22',
  sourcemap: false,
  legalComments: 'inline',
  metafile: true,
  plugins: [{ name: 'external-native-backend', setup(builder) {
    builder.onResolve({ filter: /^@serialport\/bindings-cpp$/ }, () => ({ path: './native/bindings.cjs', external: true }))
    builder.onResolve({ filter: /^koffi$/ }, () => ({ path: './native/koffi/index.js', external: true }))
  } }],
})
await chmod(join(dist, 'device-cli.mjs'), 0o755)
const addonApi = await packageRoot('node-addon-api', nativePackage.directory)
await copyOriginalLicenses({ inputs: { ...bundle.metafile.inputs, ...nativeBundle.metafile.inputs,
  [relative(root, join(koffi.directory, 'index.js'))]: {},
  [relative(root, join(addonApi.directory, 'napi.h'))]: {},
} })

// Keep the metadata synchronized with the exact Core package used to build.
const core = await packageRoot('@esp-vision/device-core', root)
const cliManifest = JSON.parse(await readFile(join(root, 'package.json'), 'utf8'))
if (cliManifest.espVision?.deviceCore !== cliManifest.devDependencies?.['@esp-vision/device-core'] ||
    cliManifest.espVision?.resolved?.deviceCore !== core.manifest.version) {
  throw new Error('CLI metadata must identify the resolved bundled Core and its minor range')
}
await copyFile(join(core.directory, 'LICENSE'), join(root, 'LICENSE'))
await copyFile(join(core.directory, 'NOTICE'), join(root, 'DEVICE_CORE_NOTICE.md'))
console.log('Built dist/device-cli.mjs with shared esptool-js and npm native serial binaries (no host Python).')
