# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Record effective build capabilities using the actual compiler/preprocessor."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

PROBE = '''
#include "boardconfig.h"
#include "py/mpconfig.h"
#if MICROPY_PY_NETWORK_WLAN && ESP_VISION_ENABLE_BACKGROUND_NETWORK && \
    ((defined(CONFIG_ESP_WIFI_ENABLED) && CONFIG_ESP_WIFI_ENABLED) || \
     (defined(CONFIG_ESP_WIFI_REMOTE_ENABLED) && CONFIG_ESP_WIFI_REMOTE_ENABLED))
ESP_VISION_RELEASE_WIFI_RESULT 1
#else
ESP_VISION_RELEASE_WIFI_RESULT 0
#endif
'''


def preprocess_command(entry):
    arguments = entry.get('arguments') or shlex.split(entry['command'])
    result = []
    skip = False
    for argument in arguments:
        if skip:
            skip = False
            continue
        if argument in ('-o', '-MF', '-MT', '-MQ'):
            skip = True
            continue
        if argument in ('-c', '-MD', '-MMD', '-MP') or argument == entry['file']:
            continue
        if any(argument.startswith(prefix) for prefix in ('-MF', '-MT', '-MQ')):
            continue
        result.append(argument)
    return [*result, '-E', '-P', '-x', 'c', '-']


def write_identity(directory, board, source_commit):
    directory = Path(directory)
    entries = json.loads((directory / 'compile_commands.json').read_text())
    matches = [entry for entry in entries if entry['file'].replace('\\', '/').endswith('/platform/network_service.c')]
    if not matches or re.fullmatch(r'[0-9a-f]{40}', source_commit) is None:
        raise ValueError('Expected actual network-service compile commands and full source commit')
    # CMake INTERFACE sources can be compiled for both the executable and main
    # archive. Their flags differ, so evaluate every context instead of picking
    # an arbitrary object or assuming repeated source paths are duplicates.
    capabilities = set()
    for index, entry in enumerate(matches):
        result = subprocess.run(preprocess_command(entry), input=PROBE, text=True, capture_output=True,
                                cwd=entry['directory'])
        if result.returncode:
            raise ValueError(f'Network capability compiler context {index + 1} failed: {result.stderr}')
        values = re.findall(r'^ESP_VISION_RELEASE_WIFI_RESULT ([01])$', result.stdout, re.MULTILINE)
        if len(values) != 1:
            raise ValueError(f'Compiler context {index + 1} did not produce one effective WiFi capability')
        capabilities.add(values[0] == '1')
    if len(capabilities) != 1:
        raise ValueError('Network-service compiler contexts disagree on WiFi capability')
    args = json.loads((directory / 'flasher_args.json').read_text())
    app = (directory / args['app']['file']).read_bytes()
    description = json.loads((directory / 'project_description.json').read_text())
    identity = dict(schemaVersion=1, board=board, sourceCommit=source_commit,
                    firmwareVersion=description['project_version'], appSha256=hashlib.sha256(app).hexdigest(),
                    capabilities={'wifi': capabilities.pop()})
    (directory / 'release-build-identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    return identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('board')
    parser.add_argument('source_commit')
    args = parser.parse_args()
    directories = [path.parent for path in Path('build', args.board).glob('idf*/flasher_args.json')]
    if len(directories) != 1:
        raise ValueError('Expected one successful IDF build directory')
    identity = write_identity(directories[0], args.board, args.source_commit)
    print(f"Recorded build identity for {identity['board']}; wifi={identity['capabilities']['wifi']}")


if __name__ == '__main__':
    main()
