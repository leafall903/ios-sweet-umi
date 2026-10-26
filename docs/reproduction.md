# Reproduction and build boundaries

## Scope of the v6 source checkpoint

Clone the independent repository or select the release tag:

```sh
git clone --branch umi-ios16.5-v6 https://github.com/leafall903/ios-sweet-umi.git
cd ios-sweet-umi
python3 tools/check_publication.py
python3 -m unittest discover -s tests -v
python3 devices/umi/tests/run_host_tests.py --sanitize
```

The default branch remains `leafall903/umi-research-2026-10-25`. The tag fixes the publication snapshot. Python 3 and Clang/Clang++ are sufficient for the host checks; no phone, iOS image or SDK is needed for those checks.

This is a **source checkpoint of an incremental research build**, not a turnkey or bit-for-bit complete system build. It includes the kernel changes and principal GL/Mesa/user-space compatibility sources that were missing from the earlier notes-only repository. The source manifest explicitly sets `bit_for_bit_kernel_rebuild_claimed` to false. No prebuilt ROM or boot image is offered.

## Pinned open-source inputs

| Input | Pin |
| --- | --- |
| XNU | Apple `xnu-8796.141.3`, commit `1b191cb58250d0705d8a51287127505aa4bc0789` |
| HFS | `hfs-627.100.6`, commit `31d5ec238dfab485e78a9a30b8b92d13e8914f03` |
| libpthread | `libpthread-514.100.2`, commit `a6c70ca0e544347a0377babf748f831577ec24f9` |
| libdispatch support | `libdispatch-1415.140.2` open-source firehose files |
| BearSSL | `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f` |
| Historical build infrastructure | blacktop/darwin-xnu-build `663906528d1cd5ea8fde7de6f16de73db9511549` |
| Mesa | Official `25.0.7` archive; SHA256 `592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6` |
| HakiOS compatibility reference | `5fbd7fd9b5e7b73965ff570aba671e15e596d7f2`, plus the 20F66 reference recorded in the license inventory |

See [third-party sources and licenses](../THIRD_PARTY_LICENSES.md) for origins and terms.

## Inspect the kernel patch

From the repository root, set aside an empty working directory for the public Apple base:

```sh
git clone --depth 1 --branch xnu-8796.141.3 https://github.com/apple-oss-distributions/xnu.git /tmp/umi-xnu-base
git -C /tmp/umi-xnu-base rev-parse HEAD
git -C /tmp/umi-xnu-base apply --check "$PWD/devices/umi/kernel/umi-v6.patch"
```

The printed commit must match the pin above. Apply the patch in that separate tree only when ready to inspect/build it. **Do not apply the old `xnu/patches/` series on top of this consolidated patch.** Those files preserve historical staging, not a second current patch layer. The current HakiOS upstream target `8796.121.2` is different; this checkpoint has not been rebased to it.

`kernel/units/` preserves 18 separately compiled translation units whose object SHA256 values matched the tested kernel archive. Some local headers differ across compile times. Treat each unit's adjacent headers as authoritative for that incremental object; do not assume the consolidated tree by itself recreates the entire tested archive. The manifest records canonical overrides and individual file hashes.

`manifests/v6-build-commands.json` preserves the recorded compiler argument arrays with host paths replaced by named placeholders. It is a review aid, **not an executable build driver**. It identifies include roots, architecture, configuration, ABI and optimization flags without publishing SDK contents.

## What an actual kernel rebuild still requires

The tested route generated a `DEVELOPMENT` / `ARM64` / `SWEET` XNU build tree, then compiled selected umi objects and incrementally replaced/added archive members before linking. Some extra units (`SweetGPU`, `sweet_gpu`, `UmiKeys`, `UmiSweetBridge`, `UmiFtsTouch`) were integrated outside the original make-file list. Merely running `make` against the consolidated patch does not perform that integration.

A contributor must provide compatible generated export/configuration/MIG/IIG headers, the staged open-source dependency layout and their own SDK platform headers. The original generated IIG side used the Xcode 15-era XNU IORPC ABI; later isolated compiles used Apple Clang from the installed Xcode beta. Arbitrarily regenerating IIG output with a newer ABI is not a verified substitute. Header generation, the complete archive/link orchestration and an independent clean rebuild remain to be packaged and validated.

The original tested kernel's SHA256 is `278e1b83a242983eb7f4753be8307d5478eae08dad3f58ee624211450e0d52a7`. This identifies a retained private artifact; the binary itself is not published. Matching source/object hashes records provenance but does not prove that a new build is byte-identical.

The kernel's root-volume guard requires a generated private `UmiRootVolume.h`. The public `.in` templates contain a zero identity and are deliberately unusable as a real phone policy. `tools/make_root_volume_policy.py` generates a policy from a locally supplied root image and its explicitly qualified SHA256; consult its `--help`. Keep generated output outside Git. It is not a permission to flash a partition.

## Mesa and user-space sources

The code-only Mesa patch applies to the official 25.0.7 archive. Its URL, archive hash and patch hash are in `devices/umi/userland/mesa/source.json`. Empty non-runtime trimming and caches from the private working copy are omitted. After extracting the verified archive into a separate directory, use `git apply --no-index --check` with the absolute patch path before applying it.

The GL bridge, generated slot wrappers and compatibility sources live next to the Mesa patch. The v6 GL table uses 2048 heap-backed entries with stable addresses and balanced resource ownership; the backboard wrapper retains the v5 early `FD_MESA_DEBUG=sysmem` initialization. The 20F66 guards, snapshot and media adaptations are version-specific. A library built for the older 20H392 user space is not interchangeable.

A full user-space build additionally needs the owner's iOS SDK, compatible compiler/linker, dependency libraries and a locally prepared 20F66 root. Shared-cache identities, injection positions, launch service configuration, signing and exact installation destinations must be checked against that local build. Complete clean build/install/loader orchestration is not included or independently reproduced by this release.

## Private inputs and installation boundary

The tested phone is an unlocked 8 GB / 256 GB Mi 10 with ST FTM5 touch. The owner supplies iPhone X / iPhone10,3 iOS 16.5 / 20F66 software, SDK/toolchain, phone firmware and loader/device-tree inputs themselves. None is redistributed. No UDID, activation data, private root identity or proprietary SDK header dump belongs in this repository.

The phone uses a 16 GiB HFSX research root in **Android userdata**. That installation replaced Android user data; temporary `fastboot boot` does not mean a RAM-only or non-destructive dual boot. Installation and rollback are device-specific and are not performed by any public source check. This guide deliberately offers no ready-to-run userdata-flashing command for an incomplete build recipe.

The first-start seed tool uses public experimental placeholder data, not a real SEP keybag. It does not implement real device security. The local 16.7.16/v23 fallback and personal backups remain separate from this source publication.

## Validation limits

Host tests cover production FTS no-op parsing, client-memory span checks, GL image ownership, bootstrap path handling and root policy generation. CI additionally checks publication boundaries and patch application. These checks establish neither a booted phone nor wallpaper/widget correctness, app compatibility, sustained FPS or hardware security. Hardware claims in the README are explicitly scoped to the recorded owner-operated tests.
