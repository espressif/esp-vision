// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

import { readFile, writeFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const bundledRuntimePackages = ["@modelcontextprotocol/sdk", "express", "minisearch", "zod"];

function normalizeMarkdown(text) {
  return text
    .replace(/\r\n?/g, "\n")
    .replace(/[ \t]+$/gm, "")
    .replace(/\n*$/, "\n");
}

async function findLicense(packageKey) {
  const packagePath = resolve(packageRoot, packageKey);
  for (const candidate of [
    "LICENSE", "LICENSE.md", "LICENSE.txt", "LICENCE", "COPYING", "license", "license.md",
  ]) {
    try {
      const text = await readFile(resolve(packagePath, candidate), "utf-8");
      if (text.trim()) return { file: candidate, text: text.trimEnd() };
    } catch {
      // Try the other common license names.
    }
  }
  return undefined;
}

export async function generateNotices() {
  const packageLock = JSON.parse(await readFile(resolve(packageRoot, "package-lock.json"), "utf-8"));
  const packages = packageLock.packages ?? {};
  const included = new Set();
  const pending = bundledRuntimePackages.map((name) => ({ name, parent: "" }));

  while (pending.length) {
    const { name, parent } = pending.shift();
    const nestedKey = parent ? `${parent}/node_modules/${name}` : `node_modules/${name}`;
    const packageKey = packages[nestedKey] ? nestedKey : `node_modules/${name}`;
    if (included.has(packageKey) || !packages[packageKey]) continue;
    included.add(packageKey);
    const dependencyNames = new Set([
      ...Object.keys(packages[packageKey].dependencies ?? {}),
      ...Object.keys(packages[packageKey].optionalDependencies ?? {}),
      ...Object.keys(packages[packageKey].peerDependencies ?? {}),
    ]);
    for (const child of dependencyNames) pending.push({ name: child, parent: packageKey });
  }

  const sections = [];
  for (const packageKey of [...included].sort()) {
    const metadata = packages[packageKey];
    const license = await findLicense(packageKey);
    if (!license) throw new Error(`Bundled dependency has no copied license file: ${packageKey}`);
    sections.push([
      `## ${packageKey.replace(/^node_modules\//, "")}@${metadata.version}`,
      `SPDX license: ${metadata.license ?? "see bundled license text"}`,
      `Original file: ${packageKey}/${license.file}`,
      "",
      "```text",
      license.text,
      "```",
    ].join("\n"));
  }

  const content = [
    "# Third-party notices",
    "",
    "The ESP-VISION Knowledge MCP package bundles the runtime dependencies listed below. Their original license texts are reproduced here.",
    "",
    ...sections,
    "",
  ].join("\n");
  await writeFile(resolve(packageRoot, "THIRD_PARTY_NOTICES.md"), normalizeMarkdown(content));
  return included.size;
}
