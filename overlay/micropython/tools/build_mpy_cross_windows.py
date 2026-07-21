# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Build MicroPython's host-side mpy-cross executable on Windows."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


def normalised_environment() -> dict[str, str]:
    """Return a case-normalised Windows environment without duplicate keys."""
    return {key.upper(): value for key, value in os.environ.items()}


def enable_msvc_utf8(environment: dict[str, str]) -> None:
    """Compile UTF-8 MicroPython sources without changing the parent shell."""
    options = environment.get("CL", "").split()
    if not any(option.lower() == "/utf-8" for option in options):
        options.append("/utf-8")
    environment["CL"] = " ".join(options)


def find_msbuild(environment: dict[str, str]) -> str:
    msbuild = shutil.which("MSBuild.exe", path=environment.get("PATH"))
    if msbuild:
        return msbuild

    program_files_x86 = environment.get("PROGRAMFILES(X86)", r"C:\Program Files (x86)")
    vswhere = Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.is_file():
        result = subprocess.run(
            [
                str(vswhere),
                "-latest",
                "-products",
                "*",
                "-requires",
                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-find",
                r"MSBuild\**\Bin\MSBuild.exe",
            ],
            check=True,
            capture_output=True,
            env=environment,
            text=True,
        )
        for line in result.stdout.splitlines():
            candidate = line.strip()
            if candidate and Path(candidate).is_file():
                return candidate

    raise SystemExit(
        "MSBuild with the Visual Studio C++ tools was not found; "
        "install the Visual Studio Build Tools 'Desktop development with C++' workload"
    )


def micropython_version_tag(project: Path) -> str:
    names = {
        "MICROPY_VERSION_MAJOR",
        "MICROPY_VERSION_MINOR",
        "MICROPY_VERSION_MICRO",
        "MICROPY_VERSION_PRERELEASE",
    }
    values: dict[str, int] = {}
    mpconfig = project.parent.parent / "py" / "mpconfig.h"
    for line in mpconfig.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[0] == "#define" and fields[1] in names:
            values[fields[1]] = int(fields[2].strip("()"))

    if values.keys() != names:
        raise SystemExit(f"could not read the MicroPython version from {mpconfig}")
    suffix = "-preview" if values["MICROPY_VERSION_PRERELEASE"] else ""
    return "v{}.{}.{}{}".format(
        values["MICROPY_VERSION_MAJOR"],
        values["MICROPY_VERSION_MINOR"],
        values["MICROPY_VERSION_MICRO"],
        suffix,
    )


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: build_mpy_cross_windows.py <mpy-cross.vcxproj>")

    project = Path(sys.argv[1]).resolve()
    environment = normalised_environment()
    enable_msvc_utf8(environment)
    environment.setdefault("MICROPY_GIT_TAG", micropython_version_tag(project))
    msbuild = find_msbuild(environment)
    subprocess.run(
        [
            msbuild,
            str(project),
            "/m",
            "/p:Configuration=Release",
            "/p:Platform=x64",
            "/p:PyVariant=standard",
        ],
        check=True,
        cwd=project.parent,
        env=environment,
    )

    output = project.parent / "build" / "mpy-cross.exe"
    if not output.is_file():
        raise SystemExit(f"MSBuild succeeded but did not create {output}")


if __name__ == "__main__":
    main()
