# Maintenance, authorship and source provenance

**leafall903** owns and independently maintains [this repository](https://github.com/leafall903/ios-sweet-umi). Its current publication is the Xiaomi Mi 10 iOS 16.5 v6 source checkpoint. It is not a merge into someone else's repository or an official HakiOS device-support entry.

## Why inherited authors still appear on GitHub

The original repository was created with the history of `myh982516/ios-sweet-umi`, based on commit `debeedb48c21b2622e85a3740160f77437ac403c`. GitHub's Contributors list attributes those commits to their original authors. That list is not a list of current repository administrators or people who performed this phone's recent tests.

Original authors and notices are retained. New leafall903 changes do not retroactively transfer authorship of inherited work. No history rewriting or attribution removal is part of this release.

## Source layers

| Layer | Source / role | Scope |
| --- | --- | --- |
| Historical base patches | [myh982516/ios-sweet-umi](https://github.com/myh982516/ios-sweet-umi), [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) | Retained in `xnu/patches/`; not a complete current v6 build stack. |
| Mi 10 storage/SMMU reference | [reAAAq/UniXNU-umi](https://github.com/reAAAq/UniXNU-umi) | Device-specific storage takeover reference. |
| Kernel | Apple's open-source XNU 8796.141.3 plus port changes | Exact base, consolidated diff, selected source-unit hashes and normalized compiler arguments published under `devices/umi/`. |
| GL/Mesa and user-space compatibility | [HakiOS kernel](https://github.com/oosexgang/hackios-xnu-kernel), [userland](https://github.com/oosexgang/hackios-xnu-userland), Mesa/freedreno | Pinned compatibility references in the license inventory and Mesa source manifest; not a claim that another phone's GPU settings apply unchanged to A650. |
| Touch protocol | [LineageOS Xiaomi SM8250 FTS driver](https://github.com/LineageOS/android_kernel_xiaomi_sm8250/tree/71b13e62f057a649b77fe4062feb73ee72ad609c/drivers/input/touchscreen/fts_521) | Native event semantics checked against the local ST FTM5 controller; no firmware blob included. |
| Device adaptations and evidence | leafall903's research workspace and owner-operated phone tests | Device-specific source changes, test fixtures, publication tools and status notes. Source checks and hardware observations are distinguished. |

## Test and source correspondence

The v6 kernel SHA256 is recorded as an identifier for the private tested binary. Eighteen selected source units were matched by object hash to its local archive. These matches identify specific compilation inputs; they do not certify a clean full-system rebuild. The consolidated patch is derived from the staged open-source tree with those selected units overlaid; unit-local headers preserve the original incremental combinations.

The source release excludes v7, Apple system/SDK files, screenshots, vendor firmware, donor identity blobs, personal logs and private images. The owner retains the working phone artifacts separately. Root-policy templates contain zero identities. Build UUIDs in compatibility guards identify public software versions, not individual phones.

## Licenses and community scope

See [LICENSE](../LICENSE), [NOTICE](../NOTICE), [third-party inventory](../THIRD_PARTY_LICENSES.md) and [publication scope](../COMPLIANCE.md). Original file terms remain in force. Unknown inherited transitive provenance is marked **to verify**, not silently granted a new license.

The publication adopts HakiOS's source-only and per-device organization while preserving the actual tested XNU base and historical identifiers. An upstream submission would require separate rebasing, naming review and validation. Neither an upstream code merge nor a website listing has been requested by this publication.
