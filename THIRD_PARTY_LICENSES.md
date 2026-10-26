# Third-party sources and licenses

This inventory applies to the `umi-ios16.5-v6` publication. Keep individual file headers and notices. The root [LICENSE](LICENSE) is a scope index, not a replacement for upstream terms.

| Component | Exact source / pin | Published scope | License / notice |
| --- | --- | --- | --- |
| Apple XNU | [xnu-8796.141.3](https://github.com/apple-oss-distributions/xnu/tree/1b191cb58250d0705d8a51287127505aa4bc0789) | Consolidated kernel diff and selected source units | APSL 2.0 with original per-file exceptions; [full text](LICENSE.APSL) |
| Apple HFS | [hfs-627.100.6](https://github.com/apple-oss-distributions/hfs/tree/31d5ec238dfab485e78a9a30b8b92d13e8914f03) | Staged open-source HFS code inside the kernel patch | Original Apple/APSL and per-file notices retained |
| Apple libpthread | [libpthread-514.100.2](https://github.com/apple-oss-distributions/libpthread/tree/a6c70ca0e544347a0377babf748f831577ec24f9) | Staged open-source pthread support in the kernel patch | Original Apple/APSL and per-file notices retained |
| BearSSL | Commit `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f` from [BearSSL upstream](https://www.bearssl.org/gitweb/?p=BearSSL;a=summary) | Open-source AES support staged in the kernel patch | MIT, [retained notice](LICENSES/BearSSL.txt) |
| Apple libdispatch | [libdispatch-1415.140.2](https://github.com/apple-oss-distributions/libdispatch/tree/libdispatch-1415.140.2) | `devices/umi/kernel/support/firehose_buffer*` | Apache 2.0, [license](LICENSES/Apache-2.0-libdispatch.txt), original file headers retained |
| Inherited ios-sweet-umi | [historical base](https://github.com/myh982516/ios-sweet-umi/tree/debeedb48c21b2622e85a3740160f77437ac403c) | `xnu/patches/` history and kernel lineage | Preserve original terms and authors; not relicensed as leafall903's original work |
| HakiOS kernel/userland compatibility | [pinned kernel repository](https://github.com/oosexgang/hackios-xnu-kernel/tree/5fbd7fd9b5e7b73965ff570aba671e15e596d7f2), with [20F66 compatibility reference](https://github.com/oosexgang/hackios-xnu-kernel/tree/0e6fb0bb83a5c71ce8887b8d66e28d41fef4a7e1) | GL bridge, Mesa compatibility and selected source adaptations | Kernel derivatives APSL; original non-kernel sources MIT; [upstream scope and MIT text](LICENSES/HakiOS.txt) |
| Mesa 25.0.7 | [official archive](https://archive.mesa3d.org/mesa-25.0.7.tar.xz), SHA256 `592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6` | Code-only changes in `devices/umi/userland/mesa/`; full upstream tree fetched separately | Predominantly MIT, with per-file Apache/BSD/other exceptions; [upstream notice](LICENSES/Mesa-notice.rst), [MIT](LICENSES/Mesa-MIT.txt), [Apache](LICENSES/Mesa-Apache-2.0.txt), [BSL](LICENSES/Mesa-BSL-1.0.txt). Consult upstream `licenses/` and file SPDX notices. |
| FTS helpers with explicit SPDX notices | Published source-unit headers | Source helpers, not firmware | [BSD-2-Clause-Patent](LICENSES/BSD-2-Clause-Patent.txt); explicit file notices prevail |
| LineageOS FTS reference | [SM8250 commit 71b13e6](https://github.com/LineageOS/android_kernel_xiaomi_sm8250/tree/71b13e62f057a649b77fe4062feb73ee72ad609c/drivers/input/touchscreen/fts_521) | Protocol/event reference; no wholesale Linux driver or firmware redistribution | Consult upstream file headers for its terms; the reference is not relicensed here |
| darwin-xnu-build | [commit 6639065](https://github.com/blacktop/darwin-xnu-build/tree/663906528d1cd5ea8fde7de6f16de73db9511549) | External historical build infrastructure, not vendored | Upstream terms apply; no SDK/KDK redistribution |
| leafall903 publication additions | This repository's new original non-kernel docs/tools/test harnesses | Original additions only | [MIT](LICENSES/MIT-leafall903.txt) |

## Provenance boundaries to verify

The inherited patch series predates this publication and is preserved as history. Its complete transitive authorship/license audit is **to verify**; it is not asserted to have a single blanket license. Likewise, mixed-source kernel staging and downstream Mesa changes must continue to retain their file-level notices. If a file's origin or license cannot be established, identify it in an issue rather than inventing a grant.

`eagl_gen.h` is generated compatibility source from the open-source HakiOS slot generator/table, not an Apple SDK header dump. `target_guard.h` is author-written build-identity validation code; its build UUIDs are public software-version identifiers, not a phone's private UDID or activation material. `UmiGpuFirmware.h` contains input bounds checks, not firmware bytes.

## External proprietary inputs

The owner supplies the iOS 16.5 root input, their SDK/toolchain and phone firmware locally. They are **not covered** by this repository's licenses and **not distributed** in the source release. Private volume identities and loader device-tree/identity inputs are omitted. No binary boot package is part of this publication.
