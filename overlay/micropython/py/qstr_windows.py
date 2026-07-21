# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Small Windows adaptations for MicroPython's qstr CMake rules."""

from __future__ import annotations

import re
import runpy
import subprocess
import sys
from pathlib import Path


def replace_command(source: str, prefix: str, replacement: str) -> str:
    lines = source.splitlines(keepends=True)
    matches = [index for index, line in enumerate(lines) if line.strip().startswith(prefix)]
    if len(matches) != 1:
        raise SystemExit(f"expected one mkrules.cmake command starting with: {prefix}")

    index = matches[0]
    indentation = lines[index][: len(lines[index]) - len(lines[index].lstrip())]
    newline = "\r\n" if lines[index].endswith("\r\n") else "\n"
    lines[index] = "".join(indentation + line + newline for line in replacement.splitlines())
    return "".join(lines)


def prepare_mkrules(arguments: list[str]) -> None:
    if len(arguments) != 2:
        raise SystemExit("usage: qstr_windows.py prepare-mkrules <input> <output>")

    source_path, output_path = map(Path, arguments)
    source = source_path.read_text(encoding="utf-8")
    source = replace_command(
        source,
        "COMMAND ${Python3_EXECUTABLE} ${MICROPY_PY_DIR}/makeqstrdefs.py pp ",
        """COMMAND ${Python3_EXECUTABLE} ${_esp_vision_qstr_windows} makeqstrdefs
    ${MICROPY_PY_DIR}/makeqstrdefs.py pp ${CMAKE_C_COMPILER} -E
    output ${MICROPY_GENHDR_DIR}/qstr.i.last
    cflags ${MICROPY_CPP_FLAGS} -DNO_QSTR
    cxxflags ${MICROPY_CPP_FLAGS} -DNO_QSTR
    sources ${MICROPY_SOURCE_QSTR}""",
    )
    source = replace_command(
        source,
        "COMMAND cat ${MICROPY_QSTRDEFS_PY} ",
        """COMMAND ${Python3_EXECUTABLE} ${_esp_vision_qstr_windows} preprocess
    ${CMAKE_C_COMPILER}
    ${MICROPY_QSTRDEFS_PREPROCESSED}
    ${MICROPY_CPP_FLAGS}
    --
    ${MICROPY_QSTRDEFS_PY}
    ${MICROPY_QSTRDEFS_PORT}
    ${MICROPY_QSTRDEFS_COLLECTED}""",
    )
    Path(output_path).write_text(source, encoding="utf-8", newline="")


def makeqstrdefs(arguments: list[str]) -> None:
    if len(arguments) < 2:
        raise SystemExit("usage: qstr_windows.py makeqstrdefs <script> [arguments ...]")

    script = Path(arguments[0])
    forwarded = arguments[1:]
    try:
        sources_index = forwarded.index("sources") + 1
    except ValueError as error:
        raise SystemExit("makeqstrdefs arguments are missing sources") from error

    expanded_sources: list[str] = []
    for source in forwarded[sources_index:]:
        if source.endswith(".rsp"):
            expanded_sources.extend(
                line.rstrip("\r\n")
                for line in Path(source).read_text(encoding="utf-8").splitlines(keepends=True)
                if line.strip()
            )
        else:
            expanded_sources.append(source)

    sys.argv = [str(script), *forwarded[:sources_index], *expanded_sources]
    runpy.run_path(str(script), run_name="__main__")


def quote_qstr_lines(data: bytes) -> bytes:
    output = bytearray()
    for line in data.splitlines(keepends=True):
        content = line.rstrip(b"\r\n")
        newline = line[len(content) :]
        if content.startswith(b"Q(") and content.endswith(b")"):
            output.extend(b'"' + content + b'"' + newline)
        else:
            output.extend(line)
    return bytes(output)


def unquote_preprocessed_qstrs(data: bytes) -> bytes:
    pattern = re.compile(rb'^"(Q\(.*\))"')
    return b"".join(pattern.sub(rb"\1", line) for line in data.splitlines(keepends=True))


def preprocess(arguments: list[str]) -> None:
    try:
        separator = arguments.index("--")
    except ValueError as error:
        raise SystemExit("qstr preprocess arguments are missing --") from error

    if separator < 3 or separator == len(arguments) - 1:
        raise SystemExit(
            "usage: qstr_windows.py preprocess <compiler> <output> [flags ...] -- <inputs ...>"
        )

    compiler, output = arguments[:2]
    flags = arguments[2:separator]
    inputs = arguments[separator + 1 :]
    source = quote_qstr_lines(b"".join(Path(path).read_bytes() for path in inputs))
    result = subprocess.run(
        [compiler, "-E", *flags, "-"],
        check=True,
        input=source,
        stdout=subprocess.PIPE,
    )
    Path(output).write_bytes(unquote_preprocessed_qstrs(result.stdout))


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit("usage: qstr_windows.py <command> [arguments ...]")

    if sys.argv[1] == "prepare-mkrules":
        prepare_mkrules(sys.argv[2:])
    elif sys.argv[1] == "makeqstrdefs":
        makeqstrdefs(sys.argv[2:])
    elif sys.argv[1] == "preprocess":
        preprocess(sys.argv[2:])
    else:
        raise SystemExit("unsupported qstr Windows command")


if __name__ == "__main__":
    main()
