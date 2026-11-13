# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Publish frozen Iris files to the Generic Registry, verifying downloaded bytes."""
import argparse
import os
from pathlib import Path
import sys
from urllib.error import HTTPError
from urllib.parse import quote, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.ci.irisfw.format import LIMIT, require, sha256  # noqa: E402
from tools.ci.irisfw.validate_release import validate_release  # noqa: E402

PACKAGE = 'esp-vision-irisfw'


class DownloadRedirects(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        require(req.get_method() == 'GET' and urlsplit(newurl).scheme == 'https', 'Unsafe registry redirect')
        redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
        if redirected and urlsplit(req.full_url).netloc != urlsplit(newurl).netloc:
            for mapping in (redirected.headers, redirected.unredirected_hdrs):
                for key in list(mapping):
                    if key.lower() in ('job-token', 'private-token', 'authorization', 'deploy-token'):
                        del mapping[key]
        return redirected


class Registry:
    def __init__(self, base, project, tag, token):
        parts = urlsplit(base)
        require(parts.scheme == 'https' and parts.netloc and not parts.username and not parts.password
                and not parts.query and not parts.fragment, 'Registry API must use HTTPS')
        require(project.isdigit() and token, 'CI project/token required')
        self.prefix = f'{base.rstrip("/")}/projects/{project}/packages/generic/{PACKAGE}/{quote(tag, safe="")}/'
        self.token = token
        self.opener = build_opener(DownloadRedirects())

    def request(self, name, data=None):
        method = 'GET' if data is None else 'PUT'
        req = Request(self.prefix + quote(name, safe=''), data=data, method=method,
                      headers={'JOB-TOKEN': self.token, 'Content-Type': 'application/octet-stream'})
        try:
            with self.opener.open(req, timeout=60) as response:
                require(response.status == (200 if data is None else 201), 'Unexpected registry response')
                result = response.read(LIMIT + 1)
                require(len(result) <= LIMIT, 'Registry response exceeds size limit')
                return result
        except HTTPError as error:
            if data is None and error.code == 404:
                return None
            # Do not expose headers, request objects or credential-bearing errors.
            raise RuntimeError(f'Registry {method} failed: HTTP {error.code}') from None


def publish_files(directory, registry):
    """Preflight the whole release; write the completion index only after readback."""
    paths = sorted(directory.iterdir(), key=lambda path: (path.name == 'release.json', path.name))
    payloads = [(path.name, path.read_bytes()) for path in paths]
    existing = {}
    for name, payload in payloads:
        remote = registry.request(name)
        require(remote is None or remote == payload, f'Immutable release conflict: {name}')
        existing[name] = remote is not None
    for name, payload in payloads:
        if not existing[name]:
            registry.request(name, data=payload)
        remote = registry.request(name)
        require(remote == payload, f'Registry download verification failed: {name}')
        print(f'Verified {name} sha256={sha256(payload)}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    tag = os.environ['CI_COMMIT_TAG']
    validate_release(args.root, tag, os.environ['CI_COMMIT_SHA'], int(os.environ['CI_PIPELINE_ID']))
    registry = Registry(os.environ['CI_API_V4_URL'], os.environ['CI_PROJECT_ID'], tag, os.environ['CI_JOB_TOKEN'])
    publish_files(args.root, registry)
    print(f'Published verified release index: {registry.prefix}release.json')


if __name__ == '__main__':
    main()
