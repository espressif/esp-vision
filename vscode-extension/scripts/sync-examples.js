/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

const fs = require("fs");
const path = require("path");

const roots = [
    {
        name: "examples",
        src: path.resolve(__dirname, "..", "..", "example"),
        dst: path.resolve(__dirname, "..", "examples"),
    },
    {
        name: "stubs",
        src: path.resolve(__dirname, "..", "..", "stubs"),
        dst: path.resolve(__dirname, "..", "stubs"),
    },
];

function shouldCopy(src) {
    const basename = path.basename(src);
    return basename !== "__pycache__" && !basename.endsWith(".pyc") && !basename.endsWith(".pyo");
}

for (const root of roots) {
    if (!fs.existsSync(root.src)) {
        const dstStatus = fs.existsSync(root.dst) ? `kept existing ${root.name}/` : `no ${root.name} will be bundled`;
        console.warn(`[sync-examples] source not found: ${root.src} - ${dstStatus}.`);
        continue;
    }

    if (fs.existsSync(root.dst)) {
        fs.rmSync(root.dst, { recursive: true, force: true });
    }
    fs.cpSync(root.src, root.dst, { recursive: true, filter: shouldCopy });
    console.log(`synced ${root.name}: ${root.src} -> ${root.dst}`);
}
