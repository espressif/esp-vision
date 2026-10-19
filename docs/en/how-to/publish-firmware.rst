Publish qualified firmware
==========================

:link_to_translation:`zh_CN:[中文]`

Firmware and Device Core have independent release schedules. Publishing an npm package, including Device Core 0.3, does not run the firmware upload job. Only a firmware tag matching the release CI rules runs ``pack_launchpad``, ``qualify_firmware_release`` and ``upload-dl-trigger``. Existing releases such as ``2026.08.27`` predate metadata generation: their binary downloads can work while ``firmware/<board>/<version>.json`` returns HTTP 404.

Package and URL contract
------------------------

The packer reuses the release matrix's IDF artifacts; it does not rebuild firmware. Its ``binaries/`` artifact contains:

.. code-block:: text

   config.toml
   releases/<tag>/manifest.json
   esp-vision-<board>-<tag>.bin
   esp-vision-<board>-<tag>-app.bin
   firmware/<board>/<tag>.json

The uploader must keep ``config.toml``, ``releases/`` and ``firmware/`` at ``https://dl.espressif.com/AE/esp-vision/`` and put both binary files in ``bin/``. Core resolves metadata relative to the root config URL. Putting metadata in ``bin/firmware/`` causes HTTP 404 even though generation succeeded. The uploader accepts only the declared release objects and optional MCP files; unrelated artifacts are rejected.

For eight published boards, the default layout contains 16 BINs, eight per-board JSONs, one immutable manifest and one root config: 26 objects. The optional versioned MCP pack and compatibility ``bin/mcp/latest.json`` alias make 28. Single-bin mode removes eight application BINs and eight per-board JSONs: eight BINs plus manifest/config, or 10 objects (12 with MCP). Source configuration remains one TOML; the server keeps one canonical mutable firmware pointer inside that root config and immutable versioned manifests. The MCP latest alias is a separate compatibility convenience.

After generation, ``tools/ci/launchpad/validate_release.py <tag>`` checks every published board, including boards hidden from the website. It rejects absent application/install metadata, wrong board/version/build identity, incorrect URLs, corrupt binaries, incorrect bootloader/application/partition hashes, invalid silicon revision intersections and invalid partition bounds. It requires metadata for the current release only; older entries in the release list need not have metadata. The release job fails before handing an incomplete package to the uploader.

Immutable manifest and current pointer
--------------------------------------

Each version has one immutable ``releases/<tag>/manifest.json`` containing ``schemaVersion = 1``, the version, full 40-character ``sourceCommit``, positive CI ``pipelineId``, ``layout``, all board release descriptors, that version's complete ``website`` snapshot and an explicit ``mcp`` descriptor or JSON ``null``. Model download URLs use the source commit, not a moving branch. Root ``config.toml`` keeps the launchpad fields and compatibility website snapshot, plus ``[publication]`` with the same identity, ``manifestUrl``, SHA-256 of the exact manifest bytes, ``previousConfigSha256`` and ``releases`` records. Each record names only its versioned manifest/hash and actual nonempty unique ``boards`` set.

The packer downloads the previous public config and verifies every referenced historical manifest's hash, identity and board membership before carrying its record forward unchanged. Fetch or validation errors stop packaging. Git tags are not evidence of a successful publication and are never promoted into records. Legacy website version labels remain download-only; they do not acquire invented manifests or board/version combinations. A published version cannot be repackaged under the same name.

``manifest.website`` is the authoritative versioned board/model snapshot. Root ``config.website`` mirrors the newest snapshot for compatibility. Consumers must follow the publication record for historical versions instead of combining today's board list with every version label.

Build-derived Wi-Fi capability
------------------------------

Every new release entry includes ``capabilities.wifi`` from ``release-build-identity.json``. CI runs the real network-service compiler command in preprocessing-only mode after the build, using the actual include paths and definitions. Wi-Fi provisioning requires ``MICROPY_PY_NETWORK_WLAN && ESP_VISION_ENABLE_BACKGROUND_NETWORK && (CONFIG_ESP_WIFI_ENABLED || CONFIG_ESP_WIFI_REMOTE_ENABLED)``. The identity also binds the board, firmware version, full source commit and application SHA-256. Missing or mismatched identity prevents qualification; presentation data and a board-name allowlist cannot claim Wi-Fi support.

Optional single-bin layout
--------------------------

``ESP_VISION_PUBLICATION_LAYOUT=legacy`` remains the default. Enable ``single-bin`` explicitly only after the manifest/slicing-capable Core version and website consumer are deployed. It emits the eight complete binaries and central manifest, without application-only binaries or per-board JSON. Each application's ``image.source`` carries the complete container's ``url``, ``bytes`` and ``sha256`` plus the application's byte ``offset`` within it. ``image.url`` equals the container URL; ``image.bytes`` and ``image.sha256`` still describe only the application. The source container must exactly match ``install`` and its slice offset must equal the application flash offset. Core verifies the complete container and application slice before writing only that application for an update. Complete installation retains its separate layout/security checks.

MCP and release qualification
-----------------------------

MCP generation and schema validation precede manifest assembly. Its versioned descriptor binds the current pack's bytes/hash, version and full source commit. Failure blocks publication by default. ``ESP_VISION_ALLOW_MISSING_MCP=1`` explicitly permits ``mcp: null``; the manifest must never point to an old ``latest.json``. Compatibility alias maintenance is an uploader operation after the canonical publication has succeeded, not an input to qualification.

Packaging reuses successful firmware artifacts and can overlap hardware checks. ``qualify_firmware_release`` waits for the complete required firmware matrix, all three Function-board HIL variants, SDK checks and host/config/MCP checks, then validates the entire package with the actual commit and pipeline ID. The upload trigger needs both package and qualification success. The uploader independently verifies those jobs, stages and verifies immutable assets/manifests, and updates the mutable config pointer last with a compare-and-swap against ``previousConfigSha256``. A stale/concurrent candidate must not overwrite newer history.

Release procedure
-----------------

#. Merge the new uploader and Core contracts before enabling the new publication flow. Keep the companion root-directory fix as a prerequisite. Do not enable single-bin mode for legacy consumers.
#. Wait for the exact source commit's GitHub mirror before creating a new firmware tag through the normal protected release process; commit-pinned model links must already resolve. The uploader rejects an unavailable source commit before advancing the config. Wait for build, HIL, SDK and package qualification; an npm publication does not publish firmware.
#. Verify the root config's publication hash against the versioned manifest, its exact board membership and website/MCP snapshots, and all selected image hashes. A dry-run config or a passed branch pipeline does not prove a production upload.

Historical binaries and immutable manifests remain available. Do not manufacture metadata for old images, replace an existing version, or guess compatibility from git tags. Offline tests and merging these changes do not publish a tag, npm package or CDN data.
