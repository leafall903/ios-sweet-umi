# Source publication scope

The v6 publication follows the community's device-directory, source-only, provenance and notice-retention approach. It does not claim an independent legal audit or compliance certification.

- **Corresponding source:** the pinned Apple base, consolidated diff, 18 object-matched source-unit snapshots and normalized compile records are under `devices/umi/`. The incremental build's generated/external inputs and remaining reproduction gaps are documented in `docs/reproduction.md`.
- **Notices and modifications:** original notices remain in source and patches. The v6 changes and test boundaries are identified in the source manifest, release notes and commit history. Explicit third-party file licenses are retained.
- **License scope:** XNU derivatives are not relabeled MIT. The root `LICENSE` indexes APSL, original MIT additions and third-party exceptions. `NOTICE` and `THIRD_PARTY_LICENSES.md` identify origins and unresolved legacy provenance.
- **Distribution:** release assets are source-only. No proprietary Apple system/SDK files, vendor firmware, private identities, raw logs, screenshots, or phone images are uploaded.
- **Community compatibility:** the actual tested base is XNU `8796.141.3`; upstream's present patch target is different. Historical symbols remain unchanged. This release does not claim an upstream rebase, full-system reproducibility or official support status.
- **Automation:** the source-check workflow makes no phone connection and uploads no SDK/KDK build-tree artifacts. The old historical binary/build-tree publication workflow has been removed from the current tree.

Report a concrete provenance or distribution concern through this repository's issues with a file path and source reference. The maintainer will review the affected material.
