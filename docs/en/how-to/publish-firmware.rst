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
#. Before tagging, separately bump the Plugin and Knowledge MCP npm versions and set matching Core/CLI minor ranges and resolved versions in both package manifests. The resolved CLI must already be published with the resolved Core bundled; the Plugin bundles that published CLI. Patch releases only fix bugs compatibly, only minor releases may add compatible features, and only major releases may break compatibility. Patch and major releases do not add unrelated new features. Core and CLI versions publish independently from protected default-branch pipelines.
#. Wait for the exact source commit's GitHub mirror before creating a new firmware tag through the normal protected release process; commit-pinned model links must already resolve. The uploader rejects an unavailable source commit before advancing the config. Wait for build, HIL, SDK and package qualification; an npm publication does not publish firmware.
#. Verify the root config's publication hash against the versioned manifest, its exact board membership and website/MCP snapshots, and all selected image hashes. A dry-run config or a passed branch pipeline does not prove a production upload.

Historical binaries and immutable manifests remain available. Do not manufacture metadata for old images, replace an existing version, or guess compatibility from git tags. Offline tests and merging these changes do not publish a tag, npm package or CDN data.

Iris firmware files
-------------------

``ESP_VISION_ENABLE_IRISFW`` is an opt-in CMake option and C compilation macro, supported only on ``ESP32_S31_MOSAICO``. Enabling it for another board fails configuration. The default remains OFF: ordinary Launchpad BIN builds keep their existing factory layout. The separate ``firmware_irisfw_mosaico`` job enables it with IDF ``release-v6.1`` and stores artifacts in ``build/ESP32_S31_MOSAICO/idf6.1-irisfw``. Firmware tags package only this job through ``pack_irisfw``; no other board produces an Iris package.

Use a new build directory when changing this option. The build checks the effective partition selection, QIO Flash mode and rollback setting, including when reusing a configuration:

.. code-block:: sh

   idf.py --board ESP32_S31_MOSAICO -B "$ESP_VISION_WORK_DIR/build/irisfw" \
     -D ESP_VISION_ENABLE_IRISFW=ON build

Retained layout
~~~~~~~~~~~~~~~

The 16 MiB layout follows the fixed system prefix of Mosaico Recovery ABI 2, contract ``esp-mosaico/v2`` and layout ID ``mosaico-retained-test-2m-v2``. The partition table remains at ``0x8000``. Application NVS keeps its address. The 7 MiB application is followed by one FAT partition using every remaining byte through the 16 MiB Flash boundary; there is no separate ``ui_apps`` reservation:

The map below includes the areas before the named partitions. The bootloader row is its reserved address window, not the actual image size; neither it nor the partition-table sector is an application partition. The Flash ends at ``0x1000000`` (exclusive).

.. list-table:: Complete Iris Flash map
   :header-rows: 1

   * - Region
     - Type/subtype
     - Offset
     - Size
   * - Reserved prefix
     - Outside named partitions
     - 0x0
     - 0x2000 (8 KiB)
   * - Retained bootloader window
     - Outside named partitions
     - 0x2000
     - 0x6000 (24 KiB)
   * - Partition table
     - Outside named partitions
     - 0x8000
     - 0x1000 (4 KiB)
   * - otadata
     - data/ota
     - 0x9000
     - 0x2000 (8 KiB)
   * - phy_init
     - data/phy
     - 0xB000
     - 0x1000 (4 KiB)
   * - sysmeta
     - data/nvs
     - 0xC000
     - 0x14000 (80 KiB)
   * - vibe_mode
     - app/test
     - 0x20000
     - 0x1C0000 (1.75 MiB)
   * - coredump
     - data/coredump
     - 0x1E0000
     - 0x20000 (128 KiB)
   * - nvs
     - data/nvs
     - 0x200000
     - 0x10000 (64 KiB)
   * - main_app
     - app/ota_0
     - 0x210000
     - 0x700000 (7 MiB)
   * - ffat
     - data/fat
     - 0x910000
     - 0x6F0000 (6.9375 MiB)

The fixed ``0x210000`` application start leaves only 13.9375 MiB for application and FAT combined. Two 7 MiB partitions would exceed 16 MiB Flash by 64 KiB, so the default uses 7 MiB for the application and 6.9375 MiB for FAT.

The packer rejects applications larger than the 7 MiB slot. This layout reclaims the former UI resource area for FAT. Changing an existing FAT partition's offset or size does not migrate or resize its filesystem; preserve required files before installing a different layout.

Iris builds load Wi-Fi directly from NVS partition ``sysmeta``, namespace ``shared``, string keys ``wifi_ssid`` and ``wifi_password``. A valid profile is enabled by default. The legacy ``sysmeta/wifi`` keys ``ssid``/``password`` are read only when both shared keys are absent; startup does not copy credentials or write NVS. ESP-VISION's ordinary ``nvs/ev_network`` record and factory credentials are not used in this build. Missing credentials leave Wi-Fi unconfigured; incomplete, oversized or unreadable data produces a configuration error and is left untouched.

ESP-VISION network configuration writes the same shared credential keys. Its ``ev_enabled`` (u8) and ``ev_revision`` (u32) keys preserve the service's enable/revision state; Vibe Mode continues to use the shared SSID/password. Forget stores an empty shared pair to prevent either firmware from reviving legacy credentials. No operation erases ``sysmeta`` or unrelated shared settings.

These packages require an existing compatible Vibe Mode installation, version 0.2.0 or later. Install the ``.irisfw`` through its native System Update path. This build's raw IDF flashing manifest is not a Vibe Mode installation package: its bootloader and blank OTA metadata must not replace the retained system. Changing from the ordinary factory layout does not migrate existing files. The new ``ffat`` follows the board's filesystem initialization policy; it is never supplied as an empty image in the update package.

.. _iris-app-flash:

Direct application flashing
~~~~~~~~~~~~~~~~~~~~~~~~~~~

The root ``app`` and ``app-flash`` commands also support ordinary firmware on all supported boards; see :ref:`idf-app-flash`. This section describes the Iris-specific procedure for a device already using the Iris layout and booting ``main_app``. First confirm the board, running application and partition layout; the source of truth is ``boards/ESP32_S31_MOSAICO/port/partitions-16MiB-irisfw.csv``. App-only flashing is not a conversion from the ordinary factory layout.

#. Select a supported ESP-IDF environment and set ``ESP_VISION_WORK_DIR`` to a writable workspace outside the checkout. Keep this Iris build separate from ordinary builds and explicitly enable ``ESP_VISION_ENABLE_IRISFW``. The application must come from this Iris configuration; an ordinary factory build or a complete ``.irisfw`` archive is not an application image for direct writing.
#. Release other clients and serial monitors. For a running ESP-VISION application, the installed CLI's ``control --action bootloader`` enters ROM download mode; see :doc:`../device-core/cli`. Wait for USB to re-enumerate, identify the same board and set ``PORT`` to its download port. Do not assume the application port name stays the same. This operation does not enter Vibe Mode.
#. From the repository root, run the command below. IDF builds the application as needed and flashes only that application.
#. After reset, reconnect through the application's port and verify its board/version and running ``main_app`` address. For camera changes, also capture a fresh camera frame. A completed write alone does not prove the expected application booted.

.. code-block:: console

   idf.py --board ESP32_S31_MOSAICO -B "$ESP_VISION_WORK_DIR/build/irisfw" \
     -D ESP_VISION_ENABLE_IRISFW=ON -p "$PORT" app-flash

The IDF target obtains the application offset from this build's partition table: ``0x210000`` for ``main_app``. It writes ``esp-vision.bin`` only, retaining the installed bootloader, partition table, OTA metadata, Vibe Mode, NVS and FAT. With the Iris option enabled, ``flash`` (including ``build flash monitor``) selects the same application-only target and prints this choice. Subsequent invocations recognize the option saved in that build directory's CMake cache; an explicit ``-D`` value takes precedence. Use separate build directories for ordinary and Iris firmware. Ordinary builds retain the usual full ``flash`` behavior.

Direct flashing uses the chip's ROM download interface, which is distinct from Vibe Mode. ESP-VISION's USB bootloader control can enter ROM download mode; select the re-enumerated download port before flashing. Direct app flashing neither installs this layout on a factory-layout board nor changes which application is selected to boot. It is suitable for runtime development tests, but does not exercise the native Iris update transaction, HEALTHY acceptance or installation receipt. Validate those separately by manually entering Vibe Mode and installing an ``.irisfw`` package.

Native Iris installation acceptance
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After MicroPython and EV-MUX become ready, the Iris-enabled application accepts its pending OTA image and records its address in the existing ABI 2 recovery metadata. The same macro enables a minimal Iris 0.2 console acceptance subset on the existing USB CDC interface: hardware/firmware identity, authentication binding, BOOT/LINK_READY/HEALTHY events, status, ping/time and read-only system inventory. The native updater can check the new boot, exact ELF identity, partition-table hash and retained operation receipt without a host modification. HEALTHY requires confirmed startup, VM readiness and a non-crash reset; it does not prove peripheral behavior.

The compatibility code shares EV-MUX's first-come ownership lease and TX scheduler. Iris records are parsed only outside EV-MUX frames, and never interleave bytes with a frame already being sent. An Iris owner does not enable EV-MUX REPL, logs or preview; disconnect or lease expiry releases ownership. The Iris build adds an ``ESP-Iris`` USB product marker for native discovery, retaining the existing CDC interface. Ordinary builds compile none of this adaptation. No Iris runtime, second USB stack, application updater or remote Vibe Mode command is added.

Enter Vibe Mode manually with AI held while pressing Reset before installing a subsequent package. The application rejects Iris restart and update-write commands. After the native updater releases the device, ESP-VISION clients continue to use EV-MUX. Its ``firmware.status.v1`` capability provides additional read-only evidence, including the exact application BIN hash; see :doc:`../communication/wire-protocol`.


Container and publication
~~~~~~~~~~~~~~~~~~~~~~~~~

The unsigned ZIP uses ``esp-iris-system-update/0.2`` and contains exactly ``manifest.json``, ``partition-table.bin`` padded to 4096 erased bytes, and ``app.bin``. The manifest declares ``minimum_recovery_version=0.2.0``. Bootloader, OTA metadata, Vibe Mode, NVS, coredump, FFAT and UI resource images are excluded. The packer checks the retained partition policy and rejects extra flash inputs except the standard blank OTA metadata generated by IDF. Secure Boot signatures and encrypted images need a separate policy and are rejected.

The Iris build retains the standard ESP-IDF application descriptor's project and version. Tags must fit its 31-byte version and match the compiled descriptor exactly. Build provenance binds the compile profile, board, source commit, pipeline/job IDs, actual IDF commit, configuration/layout hashes and all input hashes. It excludes personal paths and configuration contents. Packaging consumes the exact pipeline artifact without rebuilding or selecting another IDF directory.

The format reference is `esp-mosaico-utils at be151ae047014e79a9c7ae0092dd203844499003 <https://github.com/esp-mosaico/esp-mosaico-utils/blob/be151ae047014e79a9c7ae0092dd203844499003/ESP-Iris/components/esp_iris/protocol/system_update_manifest.schema.json>`_. The packer and independent validator use the standard library plus hash-pinned YAML parsing. ``dist/irisfw/`` contains one board's package, ``.provenance.json`` and ``.validation.json`` sidecars, ``SHA256SUMS`` and ``release.json``.

``publish_irisfw`` waits for ``qualify_firmware_release`` and uses ``CI_JOB_TOKEN`` to publish the frozen files to the project's Generic Package Registry under package ``esp-vision-irisfw``, version ``<tag>``. The download endpoint is ``${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/packages/generic/esp-vision-irisfw/<tag>/<filename>`` and uses existing registry access permissions. Existing Launchpad BIN paths and the external DL uploader are unchanged.

Publication preflights every remote file, reuses identical bytes and rejects conflicting bytes. It compares every downloaded upload before writing ``release.json`` last. Retry with the same frozen artifacts; a different pipeline or rebuilt provenance requires a new version. A CI resource group serializes publication. Checksums establish integrity, not installation authorization. Platform upload, approval and public listing remain separate from registry publication and device testing.

To package downloaded artifacts from one pipeline:

.. code-block:: sh

   python -m pip install --require-hashes -r tools/ci/irisfw/requirements.lock
   python tools/ci/irisfw/package_release.py --tag "$CI_COMMIT_TAG" \
     --source-sha "$CI_COMMIT_SHA" --pipeline-id "$CI_PIPELINE_ID" \
     --build-root build --output dist/irisfw
   python tools/ci/irisfw/validate_release.py --tag "$CI_COMMIT_TAG" \
     --source-sha "$CI_COMMIT_SHA" --pipeline-id "$CI_PIPELINE_ID" --root dist/irisfw

Use a new output directory per candidate. ``ESP_VISION_IRISFW_ONLY=1`` with ``ESP_VISION_TEST_BOARD=ESP32_S31_MOSAICO`` and ``ESP_VISION_TEST_IDF_IMAGE=release-v6.1`` selects only the Iris firmware build in API/web CI; it does not operate hardware. Offline regressions run with ``python -m unittest tools.tests.test_irisfw_release``. Branch/MR pipelines do not publish registry files.
