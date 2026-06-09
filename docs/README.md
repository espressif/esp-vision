# ESP-VISION Documentation

This folder holds the [ESP-Docs](https://github.com/espressif/esp-docs) source for the ESP-VISION Programming Guide. ESP-Docs is Espressif's Sphinx-based documentation toolchain, so the layout mirrors ESP-IDF.

## Layout

| Path | Purpose |
| --- | --- |
| `conf_common.py` | Shared Sphinx configuration imported by every language. |
| `en/`, `zh_CN/` | Per-language content trees, each rooted at `index.rst`. |
| `en/conf.py`, `zh_CN/conf.py` | Language-specific configuration. |
| `gen_api.py` | Generates the API reference fragments from `stubs/*.pyi`. |
| `targets.py` | Target/board capability data used by the docs and API generator. |
| `_static/versions.js` | Theme manifest that populates the version and target selectors. |
| `requirements.txt` | Python build dependencies (`esp-docs`). |
| `*/api-reference/_generated/` | Auto-generated API fragments (git-ignored). |
| `_build/` | Generated output (git-ignored). |

Both language trees share the same file names, so every page should exist in
`en/` and `zh_CN/`.

## Auto-generated API reference

The `image`, `sensor`, `display`, `espdl`, `image.ImageIO`, `h264`, and `rtsp`
signatures and descriptions are generated from the type stubs in `stubs/*.pyi`,
which are the single source of truth. `docs/gen_api.py` parses the stubs and
writes reStructuredText into `docs/<lang>/api-reference/_generated/<module>.rst`;
the curated module pages pull these in with `.. include::`. The generator runs
automatically on every Sphinx build (wired through `conf_common.py`), so to
update the reference, **edit the stub, not the generated file**. You can also run
it by hand:

```bash
python docs/gen_api.py
```

To document a new symbol, add its signature and a `#:` comment block in the
stub. If availability differs by target, add the module or symbol condition to
`targets.py`; do not duplicate the condition in generated RST. The conceptual
background lives in the hand-written `concepts/` section.

## Build

The docs support `esp32p4`, `esp32s3`, and `esp32s31`. The `-t` option accepts
one or more targets and defaults to the build action, so do not append `build`
after a multi-value `-t`.

```bash
pip install -r requirements.txt

# Build a single language and target (HTML):
cd docs
build-docs -l en -t esp32p4
build-docs -l zh_CN -t esp32s3

# Build all languages for all supported targets:
build-docs -t esp32p4 esp32s3 esp32s31
```

The rendered site is written to `docs/_build/<lang>/<target>/html/`.

Whole target-specific pages are excluded in `conf_common.py`. Use Sphinx
`.. only:: <target>` blocks for target-specific paragraphs and references.
Keep target conditions aligned with `micropython.cmake`, board `board.cmake`
files, and `imlib_config.h`.

## Live preview

```bash
cd docs
build-docs -l en -t esp32p4 && python -m http.server -d _build/en/esp32p4/html
```
