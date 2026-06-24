# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Extract a machine-readable API knowledge pack from the ESP-VISION single
# sources of truth, for the documentation MCP service (see the website repo
# docs/09 and docs/10). Two API surfaces are extracted:
#
#   1. ESP-VISION public modules  -- from stubs/*.pyi (the signature SSOT;
#      reuses docs/gen_api.py's AST helpers so signatures never drift).
#   2. MicroPython API as exposed by this firmware -- from the pinned
#      lib/micropython/docs/library/*.rst, scoped to the modules the esp32
#      port actually ships (core/extmod + machine/network/esp/esp32),
#      excluding other ports (pyb/rp2/wipy/mimxrt/zephyr/stm/...).
#
# Output is a single JSON pack. Run manually:
#     python docs/gen_mcp.py v0.0.0-dev --out /path/to/mcp-pack.json
#
# This is a build artifact: deterministic, diffable, offline-buildable. It is
# regenerated in CI and published alongside the docs (see docs/10 plan).

import argparse
import ast
import json
import os
import re
import subprocess
from typing import Dict, List, Optional, Tuple

import gen_api  # reuse the stub AST helpers (single parser, no drift)
from targets import (
    DOCUMENTED_MODULES,
    TARGET_CAPABILITIES,
    TARGETS,
    SYMBOL_TARGETS,
    module_targets,
)

DOCS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(DOCS_DIR)
STUBS_DIR = os.path.join(REPO_ROOT, 'stubs')
MP_DOCS = os.path.join(REPO_ROOT, 'lib', 'micropython', 'docs', 'library')
MP_MPCONFIG = os.path.join(REPO_ROOT, 'lib', 'micropython', 'py', 'mpconfig.h')

SCHEMA_VERSION = '1.0.0'

# --------------------------------------------------------------------------- #
# 1. ESP-VISION public API (from stubs/*.pyi)
# --------------------------------------------------------------------------- #


def _symbol_targets(module: str, kind: str, name: str):
    return SYMBOL_TARGETS.get(module, {}).get(kind, {}).get(name)


def _extract_espvision_module(module: str) -> dict:
    """Parse one stub into structured symbols, reusing gen_api's helpers."""
    path = os.path.join(STUBS_DIR, module + '.pyi')
    with open(path, 'r', encoding='utf-8') as f:
        source = f.read()
    src_lines = source.splitlines()
    tree = ast.parse(source)
    default_targets = list(module_targets(module))
    symbols: List[dict] = []

    def func_symbol(nodes, kind, owner=None):
        signatures = [gen_api._build_signature(n.name, n.args) for n in nodes]
        arg_names: List[str] = []
        for n in nodes:
            arg_names.extend(gen_api._collect_arg_names(n.args))
        comments: List[str] = []
        for n in nodes:
            c = gen_api._leading_comments(src_lines, n)
            if c:
                comments = c
                break
        prose, params = gen_api._split_doc(comments, arg_names)
        structured = []
        for p in params:
            mp = re.match(r':param\s+([^:]+):\s*(.*)', p)
            if mp:
                structured.append({'name': mp.group(1).strip(),
                                   'doc': mp.group(2).strip()})
        name = nodes[0].name
        full = '{}.{}'.format(owner, name) if owner else name
        return {
            'kind': kind,
            'name': full,
            'signatures': signatures,
            'params': structured,
            'doc': gen_api._join(prose),
        }

    # Constants (annotated module-level assignments, e.g. RGB565: int).
    for n in tree.body:
        if isinstance(n, ast.AnnAssign) and isinstance(n.target, ast.Name):
            comments = gen_api._leading_comments(src_lines, n)
            prose, _ = gen_api._split_doc(comments, [])
            t = _symbol_targets(module, 'constants', n.target.id)
            symbols.append({
                'kind': 'constant', 'name': n.target.id, 'signatures': [],
                'params': [], 'doc': gen_api._join(prose),
                'targets': list(t) if t else default_targets,
            })

    # Module-level functions.
    func_nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef)]
    for _name, nodes in gen_api._group_overloads(func_nodes):
        sym = func_symbol(nodes, 'function')
        sym['targets'] = default_targets
        symbols.append(sym)

    # Classes + their methods.
    for n in tree.body:
        if not isinstance(n, ast.ClassDef):
            continue
        init_nodes = [m for m in n.body
                      if isinstance(m, ast.FunctionDef) and m.name == '__init__']
        class_sigs = ([gen_api._build_signature(n.name, m.args) for m in init_nodes]
                      if init_nodes else [n.name])
        class_comments = gen_api._leading_comments(src_lines, n)
        class_prose, _ = gen_api._split_doc(class_comments, [])
        ct = _symbol_targets(module, 'classes', n.name)
        symbols.append({
            'kind': 'class', 'name': n.name, 'signatures': class_sigs,
            'params': [], 'doc': gen_api._join(class_prose),
            'targets': list(ct) if ct else default_targets,
        })
        method_nodes = [m for m in n.body if isinstance(m, ast.FunctionDef)]
        for _mname, mnodes in gen_api._group_overloads(method_nodes):
            if mnodes[0].name in gen_api.SKIP_METHODS:
                continue
            sym = func_symbol(mnodes, 'method', owner=n.name)
            mt = _symbol_targets(module, 'methods', sym['name'])
            sym['targets'] = list(mt) if mt else default_targets
            symbols.append(sym)

    return {
        'name': module,
        'source': 'esp-vision',
        'targets': default_targets,
        'symbols': symbols,
    }


def extract_espvision() -> List[dict]:
    return [_extract_espvision_module(m) for m in DOCUMENTED_MODULES]


# --------------------------------------------------------------------------- #
# 2. MicroPython API (from lib/micropython/docs/library/*.rst), esp32 scope
# --------------------------------------------------------------------------- #

# Core / extmod modules the esp32 firmware ships (stdlib + common extmod).
MP_CORE_MODULES = {
    'builtins', 'array', 'asyncio', 'binascii', 'collections', 'cmath',
    'cryptolib', 'deflate', 'errno', 'gc', 'gzip', 'hashlib', 'heapq', 'io',
    'json', 'marshal', 'math', 'micropython', 'os', 'platform', 'random', 're',
    'select', 'socket', 'ssl', 'struct', 'sys', '_thread', 'time', 'uctypes',
    'vfs', 'weakref', 'zlib', 'framebuf', 'btree', 'neopixel', 'bluetooth',
    'espnow',
}
# esp32-port modules and the submodule pages folded into them.
MP_ESP32_MODULES = {'esp', 'esp32', 'machine', 'network'}
MP_MACHINE_SUBMODULES = {
    'ADC', 'ADCBlock', 'I2C', 'I2CTarget', 'I2S', 'Pin', 'PWM', 'RTC',
    'SDCard', 'Signal', 'SPI', 'Timer', 'UART', 'WDT',
}
MP_NETWORK_SUBMODULES = {'WLAN', 'LAN', 'PPP'}
MP_EXCLUDED_SYMBOLS = {
    # esp.rst is shared by ESP8266 and ESP32. These symbols are documented
    # there, but are not exported by ports/esp32/modesp.c.
    'esp': {'sleep_type', 'deepsleep', 'flash_id', 'set_native_code_location'},
}

_DIRECTIVE = re.compile(
    r'^(?P<indent>\s*)\.\.\s+'
    r'(?P<kind>module|currentmodule|function|method|staticmethod|classmethod|'
    r'class|data|attribute|exception|decorator)::\s*(?P<sig>.*)$'
)
_KIND_MAP = {
    'function': 'function', 'method': 'method', 'staticmethod': 'method',
    'classmethod': 'method', 'class': 'class', 'data': 'constant',
    'attribute': 'attribute', 'exception': 'exception', 'decorator': 'function',
}
_RST_OPTION = re.compile(r'^\s*:[A-Za-z0-9_-]+:\s*')


def _mp_version() -> str:
    try:
        text = open(MP_MPCONFIG, encoding='utf-8').read()
        parts = []
        for key in ('MAJOR', 'MINOR', 'MICRO'):
            m = re.search(r'#define MICROPY_VERSION_{}\s+(\d+)'.format(key), text)
            parts.append(m.group(1) if m else '0')
        return '.'.join(parts)
    except OSError:
        return 'unknown'


def _block_body(lines: List[str], start: int, dindent: int) -> Tuple[List[str], int]:
    """Collect lines more-indented than the directive; return (body, next_idx)."""
    body: List[str] = []
    j = start
    while j < len(lines):
        ln = lines[j]
        if ln.strip() == '':
            body.append('')
            j += 1
            continue
        if (len(ln) - len(ln.lstrip())) <= dindent:
            break
        body.append(ln)
        j += 1
    return body, j


def _summary_from_body(body: List[str]) -> str:
    """First prose paragraph of a directive body, de-indented, no nested directives."""
    # Drop leading blank lines.
    idx = 0
    while idx < len(body) and body[idx].strip() == '':
        idx += 1
    para: List[str] = []
    for ln in body[idx:]:
        if ln.strip() == '':
            if para:
                break
            continue
        if _DIRECTIVE.match(ln):
            break
        para.append(ln.strip())
    text = ' '.join(para)
    # Light RST -> text cleanup for readability in tool output.
    text = re.sub(r':[a-z:]+:`~?([^`]+)`', r'\1', text)   # :func:`x` -> x
    text = re.sub(r'``([^`]+)``', r'\1', text)             # ``x`` -> x
    text = re.sub(r'\*\*([^*]+)\*\*', r'\1', text)         # **x** -> x
    return text.strip()


def _parse_rst(path: str, module_name: str) -> List[dict]:
    lines = open(path, encoding='utf-8').read().splitlines()
    symbols: List[dict] = []
    i = 0
    while i < len(lines):
        m = _DIRECTIVE.match(lines[i])
        if not m:
            i += 1
            continue
        kind = m.group('kind')
        sig = m.group('sig').strip()
        dindent = len(m.group('indent'))
        body, nxt = _block_body(lines, i + 1, dindent)
        i = nxt
        if kind in ('module', 'currentmodule') or not sig:
            continue
        # Stacked overload signatures: leading body lines aligned to the
        # signature column (deeper than the dindent+3 prose indent) are extra
        # signatures, not description text (e.g. gmtime/localtime in time.rst).
        # Sphinx options (":noindex:") are metadata, and a signature may wrap
        # across lines (machine.SDCard).
        sigs = [sig]
        k = 0
        while k < len(body) and body[k].strip() != '':
            extra = body[k].strip()
            if _RST_OPTION.match(extra):
                k += 1
                continue
            if (len(body[k]) - len(body[k].lstrip())) <= dindent + 3:
                break
            if sigs[-1].count('(') > sigs[-1].count(')'):
                sigs[-1] = '{} {}'.format(sigs[-1], extra)
            else:
                sigs.append(extra)
            k += 1
        name = sig.split('(')[0].strip()
        # Strip a leading "module." qualifier; keep "Class.method".
        if name.startswith(module_name + '.'):
            name = name[len(module_name) + 1:]
        if name in MP_EXCLUDED_SYMBOLS.get(module_name, set()):
            continue
        symbols.append({
            'kind': _KIND_MAP.get(kind, kind),
            'name': name,
            'signatures': sigs,
            'params': [],
            'doc': _summary_from_body(body[k:]),
            'targets': list(TARGETS),
        })
    return symbols


def extract_micropython() -> List[dict]:
    mpver = _mp_version()
    modules: Dict[str, dict] = {}
    if not os.path.isdir(MP_DOCS):
        # lib/micropython is a git submodule; in submodule-less CI jobs (e.g. the
        # fast check gate) it is absent. The MicroPython surface is then skipped;
        # the full pack is produced by the build_docs job which inits it.
        print('[gen_mcp] WARNING: {} missing (micropython submodule not '
              'checked out); skipping MicroPython API.'.format(MP_DOCS))
        return []

    def ensure(name: str) -> dict:
        if name not in modules:
            modules[name] = {
                'name': name, 'source': 'micropython', 'mpVersion': mpver,
                'targets': list(TARGETS), 'symbols': [],
            }
        return modules[name]

    for fname in sorted(os.listdir(MP_DOCS)):
        if not fname.endswith('.rst'):
            continue
        stem = fname[:-4]
        head, _, sub = stem.partition('.')
        # Scope: keep esp32-relevant modules, fold submodule pages in.
        if stem in MP_CORE_MODULES or stem in MP_ESP32_MODULES:
            target_module = stem
        elif head == 'machine' and sub in MP_MACHINE_SUBMODULES:
            target_module = 'machine'
        elif head == 'network' and sub in MP_NETWORK_SUBMODULES:
            target_module = 'network'
        else:
            continue  # other ports / unsupported: skip
        mod = ensure(target_module)
        mod['symbols'].extend(_parse_rst(os.path.join(MP_DOCS, fname), target_module))

    # MicroPython documents some symbols in multiple directive blocks (e.g.
    # overloaded constructors io.StringIO, vfs.mount). Merge by name: union the
    # signatures, keep the first non-empty description.
    for mod in modules.values():
        merged: List[dict] = []
        index: Dict[str, int] = {}
        for s in mod['symbols']:
            if s['name'] in index:
                tgt = merged[index[s['name']]]
                for sig in s['signatures']:
                    if sig not in tgt['signatures']:
                        tgt['signatures'].append(sig)
                if not tgt.get('doc') and s.get('doc'):
                    tgt['doc'] = s['doc']
            else:
                index[s['name']] = len(merged)
                merged.append(s)
        mod['symbols'] = merged

    return [modules[k] for k in sorted(modules)]


# --------------------------------------------------------------------------- #
# 3. Examples (example/**.py) and narrative prose (docs/en/<section>/*.rst)
# --------------------------------------------------------------------------- #

EXAMPLE_DIR = os.path.join(REPO_ROOT, 'example')
PROSE_SECTIONS = ('introduction', 'get-started', 'concepts', 'how-to',
                  'architecture', 'target-support', 'project-relationship')
_IMPORT_RE = re.compile(r'^\s*(?:import|from)\s+([A-Za-z_][\w]*)', re.M)


def extract_examples() -> List[dict]:
    out: List[dict] = []
    if not os.path.isdir(EXAMPLE_DIR):
        return out
    for root, dirs, files in os.walk(EXAMPLE_DIR):
        dirs.sort()  # deterministic traversal order
        for fn in sorted(files):
            if not fn.endswith('.py'):
                continue
            path = os.path.join(root, fn)
            code = open(path, encoding='utf-8').read()
            rel = os.path.relpath(path, REPO_ROOT)
            mods = sorted(set(_IMPORT_RE.findall(code)))
            out.append({
                'path': rel,
                'title': rel[len('example/'):][:-3] if rel.startswith('example/') else rel,
                'code': code,
                'modules': mods,
                'targets': list(TARGETS),
            })
    return out


def _dedent(block: List[str]) -> str:
    real = [ln for ln in block if ln.strip()]
    if not real:
        return ''
    cut = min(len(ln) - len(ln.lstrip()) for ln in real)
    return '\n'.join(ln[cut:] if len(ln) >= cut else ln for ln in block).strip('\n')


def _clean_inline(s: str) -> str:
    s = re.sub(r':link_to_translation:`[^`]*`', '', s)
    s = re.sub(r':[a-z:]+:`~?([^`<]+?)(?:\s*<[^`>]+>)?`', r'\1', s)  # :role:`x <t>`
    s = re.sub(r'``([^`]+)``', r'\1', s)
    s = re.sub(r'\*\*([^*]+)\*\*', r'\1', s)
    s = re.sub(r'(?<!\*)\*([^*]+)\*(?!\*)', r'\1', s)
    return s


_UNDERLINE_RE = re.compile(r'^[=\-~^"\'`#*+.]{3,}\s*$')


def _rst_prose(path: str) -> Tuple[str, str, List[str]]:
    """Heuristic RST -> markdown-ish text + python code blocks. Title returned."""
    lines = open(path, encoding='utf-8').read().splitlines()
    title: Optional[str] = None
    out: List[str] = []
    code_blocks: List[str] = []
    n = len(lines)
    i = 0
    while i < n:
        ln = lines[i]
        stripped = ln.strip()
        # Heading: text line followed by an underline of punctuation.
        if (stripped and i + 1 < n and _UNDERLINE_RE.match(lines[i + 1].strip())
                and len(lines[i + 1].strip()) >= len(stripped) - 2):
            heading = _clean_inline(stripped)
            if title is None:
                title = heading
            else:
                out.append('\n## ' + heading + '\n')
            i += 2
            continue
        m = re.match(r'^(\s*)\.\.\s+([A-Za-z0-9_-]+)::\s*(.*)$', ln)
        if m:
            indent = len(m.group(1))
            directive, arg = m.group(2), m.group(3)
            body, nxt = _block_body(lines, i + 1, indent)
            if directive in ('code-block', 'code', 'sourcecode') and 'python' in arg:
                code = _dedent(body)
                if code:
                    code_blocks.append(code)
                    out.append('\n```python\n' + code + '\n```\n')
            # other directives (blockdiag, list-table, only, seealso, ...) skipped
            i = nxt
            continue
        if stripped.startswith('..'):  # comments / orphan directives
            i += 1
            continue
        out.append(_clean_inline(ln))
        i += 1
    text = re.sub(r'\n{3,}', '\n\n', '\n'.join(out)).strip()
    return (title or os.path.basename(path)), text, code_blocks


def extract_docs() -> List[dict]:
    out: List[dict] = []
    base = os.path.join(DOCS_DIR, 'en')
    for section in PROSE_SECTIONS:
        d = os.path.join(base, section)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith('.rst') or fn == 'index.rst':
                continue
            title, text, code = _rst_prose(os.path.join(d, fn))
            doc_id = '{}/{}'.format(section, fn[:-4])
            out.append({
                'id': doc_id, 'kind': section, 'title': title,
                'bodyMd': text, 'codeBlocks': code,
                'page': doc_id, 'targets': list(TARGETS),
            })
    return out


# --------------------------------------------------------------------------- #
# Pack assembly
# --------------------------------------------------------------------------- #


def build_capabilities() -> List[dict]:
    return [{
        'target': t,
        'modules': list(TARGET_CAPABILITIES[t]['modules']),
        'boards': list(TARGET_CAPABILITIES[t]['boards']),
        'features': {'barcode': bool(TARGET_CAPABILITIES[t]['barcode_boards'])},
    } for t in TARGETS]


def build_pack(version: str) -> dict:
    try:
        sha = subprocess.check_output(
            ['git', '-C', REPO_ROOT, 'rev-parse', '--short', 'HEAD']
        ).decode().strip()
    except Exception:
        sha = 'unknown'
    espv = extract_espvision()
    mpy = extract_micropython()
    docs = extract_docs()
    examples = extract_examples()
    return {
        'schemaVersion': SCHEMA_VERSION,
        'meta': {
            'version': version,
            'sourceCommit': sha,
            'langs': ['en'],
            'targets': list(TARGETS),
            'micropythonVersion': _mp_version(),
            'counts': {
                'espVisionModules': len(espv),
                'espVisionSymbols': sum(len(m['symbols']) for m in espv),
                'micropythonModules': len(mpy),
                'micropythonSymbols': sum(len(m['symbols']) for m in mpy),
                'docs': len(docs),
                'examples': len(examples),
            },
        },
        'capabilities': build_capabilities(),
        'modules': espv + mpy,
        'docs': docs,
        'examples': examples,
    }


def main() -> None:
    ap = argparse.ArgumentParser(description='Extract the ESP-VISION API MCP pack.')
    ap.add_argument('version', nargs='?', default='v0.0.0-dev')
    ap.add_argument('--out', default=os.path.join(REPO_ROOT, 'mcp-pack.json'))
    ap.add_argument('--dry-run', action='store_true',
                    help='Validate extraction without requiring optional sources '
                         '(micropython submodule); used by the CI check gate.')
    args = ap.parse_args()
    pack = build_pack(args.version)
    with open(args.out, 'w', encoding='utf-8') as f:
        json.dump(pack, f, ensure_ascii=False, indent=2, sort_keys=True)
    c = pack['meta']['counts']
    print('Wrote {}'.format(args.out))
    print('  ESP-VISION : {} modules, {} symbols'.format(
        c['espVisionModules'], c['espVisionSymbols']))
    print('  MicroPython: {} modules, {} symbols (v{})'.format(
        c['micropythonModules'], c['micropythonSymbols'], pack['meta']['micropythonVersion']))
    print('  Prose docs : {}   Examples: {}'.format(c['docs'], c['examples']))


if __name__ == '__main__':
    main()
