#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Check and optionally normalize ESP-VISION documentation source formatting."""

import argparse
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DOCS_DIR = REPO_ROOT / 'docs'
PROTECTED_RST_DIRECTIVES = {
    'blockdiag',
    'code-block',
    'csv-table',
    'literalinclude',
    'mermaid',
    'raw',
    'seqdiag',
    'toctree',
}
RST_DIRECTIVE_RE = re.compile(r'^(\s*)\.\. ([A-Za-z0-9_-]+)::')
RST_ADORNMENT_RE = re.compile(r'^[=\-~^"+`:#*]{3,}$')
RST_LIST_RE = re.compile(r'^(\s*)(?:[-+*]|#\.|\d+\.)\s+')
RST_CELL_RE = re.compile(r'^\s+(?:\* -|- )')
RST_OPTION_RE = re.compile(r'^\s+:[A-Za-z0-9_-]+:')
MARKDOWN_LIST_RE = re.compile(r'^(\s*)(?:[-+*]|\d+\.)\s+')


def _indent(line):
    return len(line) - len(line.lstrip(' '))


def _protected_rst_lines(lines):
    protected = set()
    index = 0
    while index < len(lines):
        match = RST_DIRECTIVE_RE.match(lines[index])
        if match is None:
            index += 1
            continue
        directive_indent = len(match.group(1))
        cursor = index + 1
        while cursor < len(lines) and RST_OPTION_RE.match(lines[cursor]):
            protected.add(cursor)
            cursor += 1
        if match.group(2) not in PROTECTED_RST_DIRECTIVES:
            index += 1
            continue
        while cursor < len(lines):
            line = lines[cursor]
            if not line.strip() or _indent(line) > directive_indent:
                protected.add(cursor)
                cursor += 1
                continue
            break
        index = cursor
    return protected


def _is_rst_special(line, index, lines, protected):
    stripped = line.strip()
    if index in protected or not stripped:
        return True
    if RST_ADORNMENT_RE.fullmatch(stripped):
        return True
    if RST_DIRECTIVE_RE.match(line):
        return True
    if stripped.startswith(':link_to_translation:'):
        return True
    return False


def _can_join_rst(current, following, current_index, following_index, lines, protected):
    if _is_rst_special(current, current_index, lines, protected):
        return False
    if _is_rst_special(following, following_index, lines, protected):
        return False
    current_indent = _indent(current)
    following_indent = _indent(following)
    current_list = RST_LIST_RE.match(current)
    if current_list:
        if RST_LIST_RE.match(following) or RST_CELL_RE.match(following):
            return False
        return following_indent > len(current_list.group(1))
    if RST_LIST_RE.match(following) or RST_CELL_RE.match(following):
        return False
    return following_indent == current_indent


def format_rst(text):
    lines = text.splitlines()
    protected = _protected_rst_lines(lines)
    output = []
    index = 0
    while index < len(lines):
        line = lines[index]
        while (
            index + 1 < len(lines)
            and _can_join_rst(line, lines[index + 1], index, index + 1, lines, protected)
        ):
            line = '{} {}'.format(line.rstrip(), lines[index + 1].strip())
            index += 1
        output.append(line.rstrip())
        index += 1
    return '\n'.join(output).rstrip() + '\n'


def _protected_markdown_lines(lines):
    protected = set()
    fence = None
    html_block = False
    for index, line in enumerate(lines):
        stripped = line.strip()
        if fence:
            protected.add(index)
            if stripped.startswith(fence):
                fence = None
            continue
        if stripped.startswith(('```', '~~~')):
            fence = stripped[:3]
            protected.add(index)
            continue
        if stripped.startswith('<') and not stripped.startswith(('<!--', '<http')):
            html_block = True
        if html_block:
            protected.add(index)
            if stripped.endswith('>') and stripped.startswith('</'):
                html_block = False
    return protected


def _is_markdown_special(line, index, protected):
    stripped = line.strip()
    if index in protected or not stripped:
        return True
    if stripped.startswith(('#', '|', '<!--', '-->')):
        return True
    if re.fullmatch(r'[-=_]{3,}', stripped):
        return True
    return False


def _can_join_markdown(current, following, current_index, following_index, protected):
    if _is_markdown_special(current, current_index, protected):
        return False
    if _is_markdown_special(following, following_index, protected):
        return False
    current_indent = _indent(current)
    following_indent = _indent(following)
    current_list = MARKDOWN_LIST_RE.match(current)
    if current_list:
        if MARKDOWN_LIST_RE.match(following):
            return False
        return following_indent > len(current_list.group(1))
    if MARKDOWN_LIST_RE.match(following):
        return False
    return following_indent == current_indent


def format_markdown(text):
    lines = text.splitlines()
    protected = _protected_markdown_lines(lines)
    output = []
    index = 0
    while index < len(lines):
        line = lines[index]
        while (
            index + 1 < len(lines)
            and _can_join_markdown(
                line, lines[index + 1], index, index + 1, protected
            )
        ):
            line = '{} {}'.format(line.rstrip(), lines[index + 1].strip())
            index += 1
        output.append(line.rstrip())
        index += 1
    return '\n'.join(output).rstrip() + '\n'


def source_files():
    yield REPO_ROOT / 'README.md'
    yield REPO_ROOT / 'README_ZH.md'
    for path in sorted(DOCS_DIR.rglob('*')):
        if path.suffix not in {'.md', '.rst'}:
            continue
        if '_build' in path.parts or '_generated' in path.parts:
            continue
        yield path


def language_pairs():
    yield REPO_ROOT / 'README.md', REPO_ROOT / 'README_ZH.md', Path('README.md')
    for en_path in sorted((DOCS_DIR / 'en').rglob('*.rst')):
        if '_generated' in en_path.parts:
            continue
        relative = en_path.relative_to(DOCS_DIR / 'en')
        zh_path = DOCS_DIR / 'zh_CN' / relative
        if zh_path.exists():
            yield en_path, zh_path, relative


def normalized_content(path):
    text = path.read_text(encoding='utf-8')
    if path.suffix == '.rst':
        return format_rst(text)
    return format_markdown(text)


def language_line_mismatches():
    mismatches = []
    for en_path, zh_path, relative in language_pairs():
        en_lines = len(en_path.read_text(encoding='utf-8').splitlines())
        zh_lines = len(zh_path.read_text(encoding='utf-8').splitlines())
        if en_lines != zh_lines:
            mismatches.append((relative, en_lines, zh_lines))
    return mismatches


def _rst_line_shape(line):
    stripped = line.strip()
    if not stripped:
        return ('blank',)
    if RST_ADORNMENT_RE.fullmatch(stripped):
        return ('heading', stripped[0])
    directive = RST_DIRECTIVE_RE.match(line)
    if directive:
        return ('directive', len(directive.group(1)), directive.group(2))
    option = RST_OPTION_RE.match(line)
    if option:
        return ('option', _indent(line), option.group(0).strip().split(':')[1])
    list_item = RST_LIST_RE.match(line)
    if list_item:
        marker = line.strip().split(maxsplit=1)[0]
        return ('list', len(list_item.group(1)), marker)
    return ('text', _indent(line))


def _markdown_line_shape(line):
    stripped = line.strip()
    if not stripped:
        return ('blank',)
    if stripped.startswith('#'):
        return ('heading', len(stripped) - len(stripped.lstrip('#')))
    if stripped.startswith('|'):
        return ('table',)
    list_item = MARKDOWN_LIST_RE.match(line)
    if list_item:
        marker = line.strip().split(maxsplit=1)[0]
        return ('list', len(list_item.group(1)), marker)
    if stripped.startswith(('```', '~~~')):
        return ('fence', stripped[:3])
    return ('text', _indent(line))


def language_structure_mismatches():
    mismatches = []
    for en_path, zh_path, relative in language_pairs():
        shape = _rst_line_shape if en_path.suffix == '.rst' else _markdown_line_shape
        en_shapes = [
            shape(line)
            for line in en_path.read_text(encoding='utf-8').splitlines()
        ]
        zh_shapes = [
            shape(line)
            for line in zh_path.read_text(encoding='utf-8').splitlines()
        ]
        if en_shapes == zh_shapes:
            continue
        first_difference = next(
            (
                index
                for index, (en_shape, zh_shape) in enumerate(
                    zip(en_shapes, zh_shapes), start=1
                )
                if en_shape != zh_shape
            ),
            min(len(en_shapes), len(zh_shapes)) + 1,
        )
        mismatches.append((relative, first_difference))
    return mismatches


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--write',
        action='store_true',
        help='rewrite documentation files using the required paragraph format',
    )
    args = parser.parse_args()
    unformatted = []
    for path in source_files():
        expected = normalized_content(path)
        current = path.read_text(encoding='utf-8')
        if current == expected:
            continue
        if args.write:
            path.write_text(expected, encoding='utf-8')
        else:
            unformatted.append(path.relative_to(REPO_ROOT))
    if args.write:
        return 0
    for path in unformatted:
        print('{} contains a wrapped paragraph'.format(path))
    mismatches = language_line_mismatches()
    for relative, en_lines, zh_lines in mismatches:
        print(
            '{} has different line counts: en={}, zh_CN={}'.format(
                relative, en_lines, zh_lines
            )
        )
    structure_mismatches = language_structure_mismatches()
    for relative, line in structure_mismatches:
        print('{} first differs structurally at line {}'.format(relative, line))
    return 1 if unformatted or mismatches or structure_mismatches else 0


if __name__ == '__main__':
    raise SystemExit(main())
