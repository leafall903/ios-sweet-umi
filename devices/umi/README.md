# Xiaomi Mi 10 device source checkpoint

**Checkpoint:** `umi-ios16.5-v6` · **Maintainer:** leafall903 · **Device test:** owner-operated Mi 10, 2026-10-26.

Read the [current feature table](../../README.md) and [reproduction guide](../../docs/reproduction.md) first. This directory is the current device-specific source publication. The root `xnu/patches/` directory preserves earlier history and is not an alternative v6 patch stack.

## Contents and correspondence

- `kernel/umi-v6.patch`: consolidated changes against Apple XNU `8796.141.3`, commit `1b191cb58250d0705d8a51287127505aa4bc0789`.
- `kernel/units/`: 18 translation-unit snapshots selected by object-hash matches against the tested kernel archive. Keep each unit's local headers when reviewing its incremental build; shared headers were not all built at the same time.
- `kernel/root-volume-policy.h.in`: zero-identity template. Generate a private policy from your own qualified root image; the template is not a working phone policy.
- `kernel/support/`: open-source libdispatch firehose source used by the local link, under its original Apache 2.0 terms.
- `userland/gl/`: GL bridge with the v6 heap-backed, stable-address image table and balanced lifetime handling.
- `userland/compat/`: iOS 16.5-specific version guards, wrappers, container and snapshot/media compatibility. These are not interchangeable with the old 16.7.16 binaries.
- `userland/mesa/`: code-only patch against the official Mesa 25.0.7 archive; source pin and checksums in `source.json`.
- `manifests/v6-source.json`: source and object hashes, exact kernel base, owner feedback and exclusions. A private artifact hash identifies the tested kernel without distributing it.
- `tests/`: host regressions for FTS idle events, client-memory spans and GL image ownership. The runner extracts the production GL functions before compiling the fixture.

The two v6 changes relative to the v5 checkpoint are a bounded non-planar client-memory span calculation and GL image-table/lifetime fixes. The v5 FTS no-op event handling and early `FD_MESA_DEBUG=sysmem` wrapper remain in use. v7 feature/performance work is excluded.

## Review without device access

```sh
python3 tools/check_publication.py
python3 -m unittest discover -s tests -v
python3 devices/umi/tests/run_host_tests.py --sanitize
```

Run these commands at the repository root. They make no phone connection. Patch-application checks and full external-input requirements are documented in the [reproduction guide](../../docs/reproduction.md).

## Distribution boundary

This is a source checkpoint, not a ROM, boot image or independently verified complete build recipe. No Apple system files, SDK headers, firmware blobs, donor device tree, personal logs or per-device volume identity are included. License scope and preserved third-party notices are recorded in [LICENSE](../../LICENSE), [NOTICE](../../NOTICE) and [THIRD_PARTY_LICENSES.md](../../THIRD_PARTY_LICENSES.md).
