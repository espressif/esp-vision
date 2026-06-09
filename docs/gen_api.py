# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Generate reStructuredText API reference fragments from the type stubs in
# ``stubs/*.pyi``. The stubs are the single source of truth for the C-implemented
# MicroPython modules, so this keeps the API reference signatures permanently in
# sync with the code.
#
# Output goes to ``docs/<lang>/api-reference/_generated/<module>.rst`` and is
# meant to be pulled into the curated module pages with ``.. include::``. The
# generated files are build artifacts (git-ignored); they are regenerated on
# every Sphinx build through the ``config-inited`` hook wired in conf_common.py,
# and can also be produced manually with ``python docs/gen_api.py``.

import ast
import os
from typing import List, Optional, Tuple

DOCS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(DOCS_DIR)
STUBS_DIR = os.path.join(REPO_ROOT, 'stubs')

# Target languages. zh_CN reuses the English prose for now (translated later).
LANGUAGES = ['en', 'zh_CN']

# Modules to document, in the order they appear in the api-reference toctree.
MODULES = ['sensor', 'image', 'display', 'espdl', 'imageio', 'h264', 'rtsp']

# Dunder methods that are implementation detail and not documented.
SKIP_METHODS = {'__del__', '__init__'}


def _heading(text: str, char: str) -> List[str]:
    return [text, char * len(text), '']


def _collect_arg_names(args: ast.arguments) -> List[str]:
    names = []
    for a in args.posonlyargs + args.args + args.kwonlyargs:
        names.append(a.arg)
    if args.vararg:
        names.append(args.vararg.arg)
    if args.kwarg:
        names.append(args.kwarg.arg)
    return names


def _format_default(node: ast.AST) -> str:
    try:
        return ast.unparse(node)
    except Exception:  # pragma: no cover - defensive
        return '...'


def _build_signature(name: str, args: ast.arguments) -> str:
    """Reconstruct a call signature without type annotations (MicroPython style)."""
    parts: List[str] = []

    posonly = list(args.posonlyargs)
    normal = list(args.args)
    positional = posonly + normal
    defaults = list(args.defaults)
    # Defaults align to the tail of the combined positional list.
    default_offset = len(positional) - len(defaults)

    for i, a in enumerate(positional):
        if a.arg == 'self':
            continue
        if i >= default_offset:
            parts.append('{}={}'.format(a.arg, _format_default(defaults[i - default_offset])))
        else:
            parts.append(a.arg)
        if posonly and i == len(posonly) - 1:
            parts.append('/')

    if args.vararg:
        parts.append('*' + args.vararg.arg)
    elif args.kwonlyargs:
        parts.append('*')

    for a, d in zip(args.kwonlyargs, args.kw_defaults):
        if d is not None:
            parts.append('{}={}'.format(a.arg, _format_default(d)))
        else:
            parts.append(a.arg)

    if args.kwarg:
        parts.append('**' + args.kwarg.arg)

    return '{}({})'.format(name, ', '.join(parts))


def _leading_comments(lines: List[str], node: ast.AST) -> List[str]:
    """Collect the contiguous ``#:`` comment block directly above ``node``."""
    start = node.lineno
    if getattr(node, 'decorator_list', None):
        start = min(start, min(d.lineno for d in node.decorator_list))
    out: List[str] = []
    i = start - 2  # line above the node (0-based index)
    while i >= 0:
        stripped = lines[i].strip()
        if stripped.startswith('#:'):
            out.append(stripped[2:].strip())
            i -= 1
        else:
            break
    out.reverse()
    return out


def _split_doc(comments: List[str], arg_names: List[str]) -> Tuple[List[str], List[str]]:
    """Split a ``#:`` block into prose paragraphs and ``:param:`` lines."""
    prose: List[str] = []
    params: List[str] = []
    for line in comments:
        if ':' in line:
            head, _, tail = line.partition(':')
            if head.strip() in arg_names and tail.strip():
                params.append(':param {}: {}'.format(head.strip(), tail.strip()))
                continue
        prose.append(line)
    return prose, params


def _emit_block(lines_out: List[str], directive: str, signatures: List[str],
                paragraphs: List[str], params: List[str], indent: str = '') -> None:
    cont = ' ' * len('{}.. {}:: '.format(indent, directive))
    first = signatures[0] if signatures else ''
    lines_out.append('{}.. {}:: {}'.format(indent, directive, first))
    for sig in signatures[1:]:
        lines_out.append('{}{}'.format(cont, sig))
    lines_out.append('')
    body_indent = indent + '   '
    for para in paragraphs:
        if not para:
            continue
        lines_out.append('{}{}'.format(body_indent, para))
        lines_out.append('')
    for p in params:
        lines_out.append('{}{}'.format(body_indent, p))
    if params:
        lines_out.append('')


def _join(prose_lines: List[str]) -> str:
    """Join wrapped ``#:`` lines into a single paragraph."""
    return ' '.join(line for line in prose_lines if line)


def _group_overloads(body: List[ast.AST]):
    """Yield (name, [nodes]) groups, merging consecutive same-named defs."""
    groups = []
    index = {}
    for node in body:
        if isinstance(node, ast.FunctionDef):
            if node.name in index:
                groups[index[node.name]][1].append(node)
            else:
                index[node.name] = len(groups)
                groups.append((node.name, [node]))
    return groups


def _render_function(lines: List[str], src_lines: List[str], nodes: List[ast.FunctionDef],
                     directive: str, indent: str = '') -> None:
    signatures = [_build_signature(n.name, n.args) for n in nodes]
    arg_names: List[str] = []
    for n in nodes:
        arg_names.extend(_collect_arg_names(n.args))
    comments: List[str] = []
    for n in nodes:
        c = _leading_comments(src_lines, n)
        if c:
            comments = c
            break
    prose, params = _split_doc(comments, arg_names)
    _emit_block(lines, directive, signatures, [_join(prose)], params, indent)


def _render_class(lines: List[str], src_lines: List[str], node: ast.ClassDef) -> None:
    # Build the class signature from __init__ overloads.
    init_nodes = [n for n in node.body
                  if isinstance(n, ast.FunctionDef) and n.name == '__init__']
    if init_nodes:
        signatures = [_build_signature(node.name, n.args) for n in init_nodes]
    else:
        signatures = [node.name]

    class_comments = _leading_comments(src_lines, node)
    init_arg_names: List[str] = []
    init_comments: List[str] = []
    for n in init_nodes:
        init_arg_names.extend(_collect_arg_names(n.args))
        c = _leading_comments(src_lines, n)
        if c and not init_comments:
            init_comments = c
    class_prose, _ = _split_doc(class_comments, [])
    init_prose, params = _split_doc(init_comments, init_arg_names)
    paragraphs = [p for p in (_join(class_prose), _join(init_prose)) if p]
    _emit_block(lines, 'py:class', signatures, paragraphs, params)

    for name, nodes in _group_overloads(node.body):
        if name in SKIP_METHODS:
            continue
        _render_function(lines, src_lines, nodes, 'py:method', indent='   ')


def _render_module(module: str) -> str:
    path = os.path.join(STUBS_DIR, module + '.pyi')
    with open(path, 'r', encoding='utf-8') as f:
        source = f.read()
    src_lines = source.splitlines()
    tree = ast.parse(source)

    out: List[str] = []
    out.append('.. py:module:: {}'.format(module))
    out.append('')

    # Constants: annotated module-level assignments (e.g. ``RGB565: int``).
    consts = [n for n in tree.body if isinstance(n, ast.AnnAssign)
              and isinstance(n.target, ast.Name)]
    if consts:
        out += _heading('Constants', '-')
        for n in consts:
            comments = _leading_comments(src_lines, n)
            prose, _ = _split_doc(comments, [])
            out.append('.. py:data:: {}'.format(n.target.id))
            out.append('')
            para = _join(prose)
            if para:
                out.append('   {}'.format(para))
                out.append('')
        if out[-1] != '':
            out.append('')

    # Module-level functions.
    func_nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef)]
    if func_nodes:
        out += _heading('Functions', '-')
        for name, nodes in _group_overloads(func_nodes):
            _render_function(out, src_lines, nodes, 'py:function')

    # Classes.
    class_nodes = [n for n in tree.body if isinstance(n, ast.ClassDef)]
    if class_nodes:
        out += _heading('Classes', '-')
        for n in class_nodes:
            _render_class(out, src_lines, n)

    # Collapse trailing blank lines to a single newline.
    text = '\n'.join(out).rstrip() + '\n'
    return text


def generate(app=None):  # noqa: ARG001 - signature required by Sphinx event
    """Regenerate every module fragment for every language."""
    header = ('.. This file is generated by docs/gen_api.py from stubs/{0}.pyi.\n'
              '.. Do not edit by hand; edit the stub instead.\n\n')
    for module in MODULES:
        body = _render_module(module)
        for lang in LANGUAGES:
            out_dir = os.path.join(DOCS_DIR, lang, 'api-reference', '_generated')
            os.makedirs(out_dir, exist_ok=True)
            out_path = os.path.join(out_dir, module + '.rst')
            with open(out_path, 'w', encoding='utf-8') as f:
                f.write(header.format(module))
                f.write(body)


if __name__ == '__main__':
    generate()
    print('Generated API reference fragments for: ' + ', '.join(MODULES))
