# Xiaomi Mi 10 (umi) — iOS 16 research

**Maintained by [leafall903](https://github.com/leafall903)** · English (default) · [简体中文](README.zh-CN.md) · Updated 2026-10-26

An independent, experimental port of XNU and an iPhone-style iOS 16 user space to the **Xiaomi Mi 10 / Snapdragon 865 / Adreno 650**. The current published checkpoint is **iOS 16.5 v6**, the last version tested on the research phone before further feature work. It reaches the iPhone lock screen and SpringBoard, renders wallpaper and widgets, and uses an experimental Mesa/freedreno GL path. The owner reported no remaining display corruption in the v6 test. Performance and app compatibility still need work.

**[v6 source release](https://github.com/leafall903/ios-sweet-umi/releases/tag/umi-ios16.5-v6)** · [Device sources](devices/umi/) · [Build and reproduction limits](docs/reproduction.md) · [Version history](docs/bringup-history.md)

This is a **device-tested research checkpoint, not a daily-use ROM or a verified 60 fps release**. The experimental root volume on this phone replaced Android `userdata`. Temporary `fastboot boot` loading does not make the installation non-destructive or preserve Android app data.

## Current device status

| Component | v6 result | Evidence and limits |
| --- | --- | --- |
| iPhone lock screen, Settings, SpringBoard | Reached and operated | Owner-observed UI on the research phone; app support is incomplete. |
| UFS / SMMU / HFSX root | Working for boot | Storage takeover, root mounting and user-space startup exercised. Uses a generated root-volume identity policy. |
| CPU | Eight cores brought up | 4 × Cortex-A55 + 4 × Cortex-A77 initialized; SGI1 diagnostic passed. Sustained clocks, thermal behavior and power management are not fully validated. |
| ST FTM5 touch and physical keys | Working in recent tests | Device-specific touch qualification and event handling; other controller/firmware batches are untested. No formal latency or long-duration reliability result. |
| Display | No corruption reported in v6 test | v6 fixes short client-memory surface spans and GL image-table lifetime/capacity. This is an observation from the test, not an exhaustive rendering guarantee. |
| Wallpaper | Rendering observed | Appeared in 16.5; broken effects in v4/v5 were no longer reported in v6. The wallpaper catalog and all styles have not been audited. |
| Clock / Calendar widgets | Owner reported normal in v5 | Retained in v6; no exhaustive test of every widget/provider or refresh condition. |
| Blur / transparency | Improved; earlier test reported correct blur | GL composition and Mesa changes included. Every effect and app has not been revalidated separately in v6. |
| Adreno 650 | Experimental GL rendering path | Earlier on-device GMU/HFI/CP/fence and FD650 clear/readback tests passed. The window-server GL path is connected; final framebuffer presentation still includes a CPU copy. |
| Metal / native display scanout | Incomplete | Do not equate working GL with complete Metal, zero-copy scanout, or GPU acceleration in every app. |
| Smoothness / 60 fps | Still unresolved | Owner requested further improvements. No sustained, reproducible 60 fps measurement for v6. |
| Maps; Settings → Date & Time | Known failures | Owner reports that these cannot open or be used; exact failure paths remain to be diagnosed. |
| Keyboard | Partial / not fully retested | A stuck introductory Continue screen was reported earlier. A first-run-state workaround exists; complete keyboard functionality is not established. |
| Wi-Fi, cellular, Bluetooth, audio, camera, charging, real battery telemetry | Not verified working on this port | Other phones' support tables do not establish support on this Mi 10. |
| Apple ID, App Store, Face ID, SEP-backed security | Not verified / unsupported | No service activation material is distributed. Experimental startup seeds do not implement a real SEP or production data protection. |

## Exact tested configuration

| Item | Configuration |
| --- | --- |
| Phone | Xiaomi Mi 10, `umi`, 8 GB RAM / 256 GB storage, unlocked bootloader |
| SoC / GPU | Qualcomm SM8250 / Snapdragon 865; Adreno 650 v3 reported by Recovery KGSL |
| Touch | ST FTM5, I²C `0x49`; a distinct protocol from reference phones with FocalTech controllers |
| Display | Physical 1080 × 2340; experimental logical iPhone canvas 1125 × 2436 |
| User space | iPhone X / `iPhone10,3`, **iOS 16.5 / 20F66**; supplied locally, not included |
| Kernel | Port based on **Apple XNU `8796.141.3`**, commit `1b191cb58250d0705d8a51287127505aa4bc0789` |
| Graphics | Mesa **25.0.7**, HakiOS compatibility layer and umi-specific adaptations |
| Storage / boot | 16 GiB HFSX root in `userdata`; temporary UEFI/loader boot handoff to XNU |

The user-space version and ported kernel version are separate. This checkpoint's kernel patch targets **8796.141.3**, not upstream HakiOS's current 8796.121.2 patch target. The local 16.7.16/v23 fallback is retained separately. **Untested v7 work is excluded from this release.**

## What this publication contains

```text
devices/umi/kernel/umi-v6.patch       Consolidated patch against the pinned Apple XNU base
devices/umi/kernel/units/            18 source-unit snapshots matched to the tested archive
devices/umi/kernel/support/          Open-source firehose support used by the port
devices/umi/userland/gl/             v6 GL bridge and image lifetime changes
devices/umi/userland/compat/         20F66-specific wrapper, container, snapshot and media code
devices/umi/userland/mesa/           Source patch against Mesa 25.0.7 and dependency manifest
devices/umi/manifests/v6-source.json Source hashes, object correspondence and test boundaries
devices/umi/tests/                   Host regressions for touch, spans and GL image ownership
xnu/patches/                        Inherited historical patch series; not the current v6 bundle
tools/                              Root-volume policy, startup seed and publication checks
docs/                               Reproduction, provenance, history and listing information
```

This release substantially expands the previous notes-only publication with the v6 kernel and user-space source checkpoint. It is **not a turnkey, bit-for-bit full-system rebuild**: the tested kernel was assembled through incremental object compilation and relinking. The consolidated patch is reviewable and checked for clean application; per-unit snapshots preserve the source/header combinations from those builds. Generated build headers, SDKs, firmware, loader identity inputs and the iOS root image remain external. See the [reproduction guide](docs/reproduction.md) before trying to build or install anything.

No iOS system files, IPSWs, SDK headers, screenshots, device firmware, personal logs, disk images or ready-to-flash boot packages are published. Release assets contain source and checksums only.

## Source checks

From a clone, with Python 3 and Clang available:

```sh
python3 tools/check_publication.py
python3 -m unittest discover -s tests -v
python3 devices/umi/tests/run_host_tests.py --sanitize
```

CI also checks that the consolidated kernel patch applies to the pinned Apple base. Host tests exercise production parser, memory-span and image-ownership functions; they do not simulate the phone, prove a complete OS build, or measure FPS.

## Ownership, contribution and licensing

This repository is independently maintained by **leafall903**. Original commit authors and source notices are preserved; GitHub's contributor list reflects inherited history as well as new work. See [provenance](docs/provenance.md), [NOTICE](NOTICE), [license index](LICENSE) and [third-party licenses](THIRD_PARTY_LICENSES.md). XNU-derived changes retain APSL 2.0 and applicable per-file notices; original non-kernel additions use MIT. Third-party code retains its own terms.

The publication follows the community's device-directory, source-only, provenance and licensing approach. It preserves tested historical symbol names and its actual XNU base rather than claiming to be an upstream-ready patch. It is **not officially listed or endorsed by HakiOS/HackiOS**, Apple, Xiaomi or Qualcomm. [Listing preparation](docs/hackios-listing.md) describes the public route; no request has been sent on the maintainer's behalf.

Please report reproducible issues in [this repository](https://github.com/leafall903/ios-sweet-umi/issues), including the checkpoint, hardware variant, action and sanitized error text. Read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting sources or logs.
