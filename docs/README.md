# ESP-VISION Documentation

This folder holds the [ESP-Docs](https://github.com/espressif/esp-docs) source for
the ESP-VISION Programming Guide. ESP-Docs is Espressif's Sphinx-based
documentation toolchain, so the layout mirrors ESP-IDF.

## Layout

| Path | Purpose |
| --- | --- |
| `conf_common.py` | Shared Sphinx configuration imported by every language. |
| `en/`, `zh_CN/` | Per-language content trees, each rooted at `index.rst`. |
| `en/conf.py`, `zh_CN/conf.py` | Language-specific configuration. |
| `requirements.txt` | Python build dependencies (`esp-docs`). |
| `_build/` | Generated output (git-ignored). |

Both language trees share the same file names, so every page should exist in
`en/` and `zh_CN/`.

## Build

The docs are chip-agnostic (no per-target slug), so no `-t` is needed.

```bash
pip install -r requirements.txt

# Build a single language (HTML):
cd docs
build-docs -l en build
build-docs -l zh_CN build

# Build everything (all languages):
build-docs build
```

The rendered site is written to `docs/_build/<lang>/generic/html/`.

## Live preview

```bash
cd docs
build-docs -l en build && python -m http.server -d _build/en/generic/html
```
