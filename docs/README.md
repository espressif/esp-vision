# ESP-VISION Documentation

This folder holds the [ESP-Docs](https://github.com/espressif/esp-docs) source for the ESP-VISION Programming Guide. ESP-Docs is Espressif's Sphinx-based documentation toolchain, so the layout mirrors ESP-IDF.

## Layout

| Path | Purpose |
| --- | --- |
| `conf_common.py` | Shared Sphinx configuration imported by every language. |
| `en/`, `zh_CN/` | Per-language content trees, each rooted at `index.rst`. |
| `en/conf.py`, `zh_CN/conf.py` | Language-specific configuration. |
| `gen_api.py` | Generates the API reference fragments from `stubs/*.pyi`. |
| `gen_board_support.py` | Generates the board table from firmware configuration. |
| `targets.py` | Chip/board capability data used by the docs and API generator. |
| `_static/versions.js` | Theme manifest that populates the version and chip selectors. |
| `requirements.txt` | Python build dependencies (`esp-docs`). |
| `*/**/_generated/` | Auto-generated documentation fragments (git-ignored). |
| `_build/` | Generated output (git-ignored). |

Both language trees share the same file names, so every page should exist in `en/` and `zh_CN/`.

Natural-language paragraphs in Markdown and reStructuredText must stay on one source line. Matching English and Chinese RST pages must also keep corresponding content on the same line numbers. Check both constraints with:

```bash
python tools/ci/check_docs_format.py
```

Architecture and process explanations must use the diagram directives supported by ESP-Docs (`blockdiag`, `seqdiag`, `actdiag`, or `nwdiag`) to show structure, order, branches, or interactions; prose may explain the diagram but must not be the only representation of the flow.

MicroPython-related documentation must describe the configuration actually built by this repository, not only upstream MicroPython capabilities. Verify claims against the pinned version in `lib/micropython`, `overlay/micropython/ports/esp32/mpconfigport.h`, board configuration and manifests under `boards/`, `micropython.cmake`, and, when relevant, generated build configuration or compile commands. Clearly distinguish features enabled in every maintained firmware, board- or chip-dependent features, and upstream features that are currently disabled or not integrated.

## Auto-generated API reference

The `image`, `sensor`, `display`, `espdl`, `tflite`, `image.ImageIO`, `h264`, and `rtsp` signatures and descriptions are generated from the type stubs in `stubs/*.pyi`, which are the single source of truth. `docs/gen_api.py` parses the stubs and writes reStructuredText into `docs/<lang>/api-reference/_generated/<module>.rst`; the curated module pages pull these in with `.. include::`. The generator runs automatically on every Sphinx build (wired through `conf_common.py`), so to update the reference, **edit the stub, not the generated file**. You can also run it by hand:

```bash
python docs/gen_api.py
python docs/gen_board_support.py
```

To document a new symbol, add its signature and a `#:` comment block in the stub. If availability differs by chip, add the module or symbol condition to `targets.py`; do not duplicate the condition in generated RST. The conceptual background lives in the hand-written `concepts/` section.

## Build

The docs support the `esp32p4`, `esp32s3`, and `esp32s31` chips. The `-t` option accepts one or more chip identifiers and defaults to the build action, so do not append `build` after a multi-value `-t`.

```bash
pip install -r requirements.txt

# Build a single language and chip (HTML):
cd docs
build-docs -l en -t esp32p4
build-docs -l zh_CN -t esp32s3

# Build all languages for all supported chips:
build-docs -t esp32p4 esp32s3 esp32s31
```

The rendered site is written to `docs/_build/<lang>/<chip>/html/`.

Whole chip-specific pages are excluded in `conf_common.py`. Use Sphinx `.. only:: <chip>` blocks for chip-specific paragraphs and references. Keep chip conditions aligned with `micropython.cmake`, board `board.cmake` files, and `imlib_config.h`.

## Live preview

```bash
cd docs
build-docs -l en -t esp32p4 && python -m http.server -d _build/en/esp32p4/html
```

## GitLab CI

Merge requests that change documentation or public-facing firmware sources build HTML and PDF output for both languages and all supported chips. The result is deployed to the preview documentation server. CI exposes the Chinese entry URL, which redirects to the default ``esp32p4`` build; readers can switch both language and chip from the selectors in the rendered documentation. A source-only change also runs an advisory check that lists files which may require a corresponding ``docs/`` or ``stubs/`` update.

Successful ``master`` pipelines publish the same matrix as ``latest`` on the production documentation server. Deployment uses these protected GitLab CI variables:

- ``DOCS_PREVIEW_DEPLOY_KEY``, ``DOCS_PREVIEW_SERVER``, ``DOCS_PREVIEW_SERVER_USER``, ``DOCS_PREVIEW_PATH``, and ``DOCS_PREVIEW_URL_BASE``
- ``DOCS_PROD_DEPLOY_KEY``, ``DOCS_PROD_SERVER``, ``DOCS_PROD_SERVER_USER``, ``DOCS_PROD_PATH``, and ``DOCS_PROD_URL_BASE``
- ``GITLAB_MR_NOTE_TOKEN`` to post preview links to merge requests; posting is skipped without this optional token

The preview and production SSH accounts must be provisioned for the ``esp-vision`` project web root. Reusing another project's deployment user may allow SSH uploads while every public URL still returns HTTP 404. The deployment job verifies both language entry URLs after upload and fails when they are not publicly reachable.
