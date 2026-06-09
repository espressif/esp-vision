#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Post ESP-Docs preview links to the current GitLab merge request."""

import json
import os
import re
import sys
import urllib.parse
import urllib.request
from pathlib import Path


LINK_PATTERN = re.compile(r'^\[document preview\]\[([^\]]+)\]\s+(\S+)$')


def main():
    token = os.getenv('GITLAB_MR_NOTE_TOKEN')
    project = os.getenv('CI_PROJECT_ID')
    mr_iid = os.getenv('CI_MERGE_REQUEST_IID')
    api_url = os.getenv('CI_API_V4_URL')
    if not all((token, project, mr_iid, api_url)):
        print('Preview note skipped because GitLab MR note variables are unavailable.')
        return 0

    entry_url = None
    for line in Path(sys.argv[1]).read_text(encoding='utf-8').splitlines():
        match = LINK_PATTERN.match(line)
        if match:
            label, url = match.groups()
            if label == 'zh_CN':
                entry_url = url
                break

    if entry_url is None:
        print('Preview note skipped because the default entry URL was not found.')
        return 0

    body = 'Documentation preview:\n\n[Open ESP-VISION documentation]({})'.format(
        entry_url
    )
    endpoint = '{}/projects/{}/merge_requests/{}/notes'.format(
        api_url,
        urllib.parse.quote(project, safe=''),
        mr_iid,
    )
    request = urllib.request.Request(
        endpoint,
        data=json.dumps({'body': body}).encode('utf-8'),
        headers={
            'Content-Type': 'application/json',
            'PRIVATE-TOKEN': token,
        },
        method='POST',
    )
    with urllib.request.urlopen(request) as response:
        print('Posted documentation preview note: HTTP {}'.format(response.status))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
