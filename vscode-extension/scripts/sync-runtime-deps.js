/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

const fs = require("fs");
const path = require("path");

const extensionRoot = path.resolve(__dirname, "..");
const sourceRoot = path.join(extensionRoot, "node_modules");
const runtimeRoot = path.join(extensionRoot, "runtime", "node_modules");

const packages = [
    "@serialport",
    "debug",
    "ms",
    "node-addon-api",
    "node-gyp-build",
    "serialport",
];

if (!fs.existsSync(sourceRoot)) {
    throw new Error("node_modules not found. Run npm install before packaging.");
}

fs.rmSync(path.dirname(runtimeRoot), { recursive: true, force: true });
fs.mkdirSync(runtimeRoot, { recursive: true });

for (const name of packages) {
    const src = path.join(sourceRoot, name);
    const dst = path.join(runtimeRoot, name);
    if (!fs.existsSync(src)) {
        throw new Error(`runtime dependency not found: ${src}`);
    }
    fs.cpSync(src, dst, { recursive: true });
}

console.log(`synced runtime dependencies: ${runtimeRoot}`);
