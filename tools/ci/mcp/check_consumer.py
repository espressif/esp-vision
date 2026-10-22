#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Validate the actual producer npm artifact through the website consumer."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
from release import descriptor, release_files

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--website', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    artifacts = args.artifacts.resolve()
    pack = (artifacts / 'mcp-pack.json').read_bytes()
    archive = (artifacts / 'knowledge-mcp.tgz').read_bytes()
    identity = json.loads(pack)['meta']
    version, commit = identity['version'], identity['sourceCommit']
    repository = 'registry.example/main/knowledge-mcp'
    manifest = descriptor(pack, version, commit, repository, 'sha256:' + 'b' * 64, archive)
    package = output / 'producer-package'
    package.mkdir(exist_ok=True)
    for name, data in release_files(pack, manifest, archive).items():
        (package / name).write_bytes(data)
    source = (ROOT / '.gitlab/ci/mcp.yml').read_text().split('trigger_website_mcp_release:', 1)[1]
    variables = source.split('  variables:', 1)[1].split('  trigger:', 1)[0]
    upstream = dict(CI_COMMIT_TAG=version, CI_PROJECT_ID='123', CI_COMMIT_SHA=commit,
                    CI_REGISTRY_IMAGE='registry.example/main')
    forwarded = {}
    for key, value in re.findall(r'^    ([A-Z_]+): "([^"]*)"$', variables, re.M):
        for name, replacement in upstream.items():
            value = value.replace('$' + name, replacement)
        forwarded[key] = value
    payload = output / 'bridge-env.json'
    payload.write_text(json.dumps({**forwarded, 'CI_PIPELINE_SOURCE': 'pipeline',
                                  'CI_API_V4_URL': 'https://gitlab.example/api/v4',
                                  'CI_JOB_TOKEN': 'test-only-token', 'CI_REGISTRY': 'registry.example'}))
    script = '''
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
const {releaseRequestFromEnv,validateRelease}=await import(pathToFileURL(process.argv[2]));
const p=process.argv[3], read=n=>readFileSync(p+'/'+n);
const options=releaseRequestFromEnv(JSON.parse(readFileSync(process.argv[4])));
const input={...options,descriptorBytes:read('mcp-release.json'),descriptorChecksumBytes:read('mcp-release.sha256'),packBytes:read('mcp-pack.json'),packChecksumBytes:read('mcp-pack.sha256'),tarballBytes:read('knowledge-mcp.tgz')};
const accepted=validateRelease(input);
assert.equal(accepted.schemaVersion,2);
assert.equal(accepted.sourceCommit,options.expectedSourceCommit);
assert.equal(accepted.version,options.version);
assert.equal(accepted.imageReference,options.imageRepository+'@sha256:'+ 'b'.repeat(64));
assert.equal(accepted.npm.name,'@esp-vision/knowledge-mcp');
assert.throws(()=>validateRelease({...input,packBytes:Buffer.from('changed')}));
assert.throws(()=>validateRelease({...input,tarballBytes:Buffer.from('changed')}));
assert.throws(()=>validateRelease({...input,expectedSourceCommit:'c'.repeat(40)}));
console.log('PASS exact npm tarball, snapshot and actual bridge variables accepted by website; tampering rejected');
'''
    entrypoint = output / 'cross-repository.mjs'
    entrypoint.write_text(script)
    subprocess.run(['node', str(entrypoint), str(args.website.resolve() / 'scripts/mcp-release-sync.mjs'),
                    str(package), str(payload)], env=os.environ, check=True)


if __name__ == '__main__':
    main()
