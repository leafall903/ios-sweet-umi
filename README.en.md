# Xiaomi Mi 10 / umi: iOS 16 research

**Maintained by [leafall903](https://github.com/leafall903)** · [中文](README.md) · Updated 2026-10-25

This project researches booting an iPhone-style iOS 16 environment on the Xiaomi Mi 10, adapting storage, input, CPU bring-up and display presentation. The device has reached the lock screen, Settings and SpringBoard UI. It still uses a CPU framebuffer. **Adreno 650 acceleration and sustained 60 fps are not demonstrated.**

## Target

| Component | Tested configuration |
| --- | --- |
| Phone | Xiaomi Mi 10, `umi`, 8 GB RAM / 256 GB storage, unlocked bootloader |
| SoC / GPU | Snapdragon 865 / SM8250; Adreno 650 family, Recovery KGSL reports `Adreno650v3` |
| CPU | 4 Cortex-A55 + 4 Cortex-A77; eight-core initialization and SGI1 tests passed on-device |
| Panel / logical canvas | 1080 × 2340 physical; 1125 × 2436 iPhone reference canvas |
| Touch | ST FTM5, I²C `0x49`; device-specific identity and event handling required |
| User space | iPhone X / `iPhone10,3`, iOS 16.7.16 / `20H392` |
| Kernel | XNU 8796 / ios-sweet-derived research route; donor and ported kernel versions differ |
| Root filesystem | A 16 GiB HFSX root volume placed in userdata on the test phone |
| Boot route | Fastboot temporary boot → umi UEFI / loader → XNU → iOS user space |

The tested layout replaced Android userdata. Temporary booting of the loader does not mean the root filesystem lives entirely in RAM or that the original Android data is preserved.

## Device results

| Area | Observed status |
| --- | --- |
| Lock screen, Settings, SpringBoard | Reached and operated on-device |
| UFS / SMMU | Takeover/read checks, GPT CRC, repeat reads and independent readback; later HFSX root mount and launchd PID 1 |
| Touch | Working in tested revisions; previous regressions and special-event handling required fixes; long-term stability unproven |
| Hardware keys | Working; early multi-second screen-off latency substantially improved according to the tester |
| Eight CPUs | Initialization/SGI1 passed; this does not validate all DVFS, thermal or sustained-load behavior |
| Display | Early wrong colors / visible line-by-line refresh substantially improved; composition issues remain |
| 60 Hz mode | Experimental mode/pacing implemented; actual animation is not sustained 60 fps |
| Wallpapers and widgets | Unresolved; installed compatibility candidate produced no meaningful improvement in the tester's report |
| Blur / transparency / vibrancy | Unresolved, including incorrect blur orientation and disappearing transparency |
| A650 | v18 register preflight and v19 short CX power-domain probe only; no GPU rendering |
| Metal / GPU composition | Not implemented for this device |
| Wi-Fi, cellular, Bluetooth, audio, camera, charging, Apple ID, App Store | No successful device validation in this research; upstream results from other devices do not establish support here |

## Performance and stability evidence

The v13 path had a 10 Hz cap and median frame intervals around 109 ms. v14 removed that cap, added a 60 Hz mode and corrected presentation timing/notification handling; the tester reported a visible improvement.

A selected v16 1015 ms window contained 53 submissions and 41 presentations (~52.22 / 40.39 calls per second). A selected v17 1016 ms window had 34 of each (~33.46 calls per second). These used different interactions and are not a controlled version comparison, sustained averages, or visual counts of unique animation frames. Copy-time medians around 3 ms cover only the last framebuffer copy, not complete rendering or input latency.

v15 exposed `IOGMD: not wired for the IODMACommand`. The descriptor prepare / transfer / complete lifecycle was fixed in v16; the next recovered test did not reproduce that panic. v17 was reported stable but still noticeably unresponsive. See [the bring-up record](docs/bringup-history.md) for milestones and measurement limits.

## A650 work still required

Validate umi-specific clocks, resets, power and GMU firmware/HFI requirements; implement SMMU routing/page tables, coherency, command rings and fault recovery; then connect a suitable XNU/userspace rendering path and test CoreAnimation and Metal-dependent offscreen behavior. Redwood's 642L sequence is not interchangeable with umi A650.

The [A650 profile](research/a650/) is a non-operational resource description. The [status report](research/umi10-a650-status.md) explicitly distinguishes CX power observations from working GPU acceleration.

## What is published

- `xnu/patches/`: inherited base patches and helper scripts, retaining original history.
- `docs/`: device milestones, provenance, reproduction limits and a proposed community-listing introduction.
- `research/`: A650 result summaries and address/size constants.

**This is not yet a complete reproducible source release of the latest v17–v19 device build.** The complete integrated changes, pinned dependencies and image-packaging steps are not all published here. Cloning this repository alone does not rebuild the tested system. There is no public installer, Apple system image, GPU firmware binary, device backup or raw private crash-log bundle. See [reproduction conditions](docs/reproduction.md).

## Maintainer and attribution

[leafall903](https://github.com/leafall903) owns and maintains this repository. GitHub lists previous authors because the base patch history was imported; contributor counts do not grant administrative access. Existing authorship and license notices remain valid. See [provenance](docs/provenance.md).

Sources and references include [ios-sweet-umi](https://github.com/myh982516/ios-sweet-umi), [UniXNU-umi](https://github.com/reAAAq/UniXNU-umi), [ios-sweet](https://github.com/dipa-studio/ios-sweet), [HackiOS XNU](https://github.com/oosexgang/hackios-xnu-kernel), and [HackiOS userland](https://github.com/oosexgang/hackios-xnu-userland). This repository does not relicense all inherited code under a new blanket license.

This is independent community research and is not currently an official HackiOS supported-device entry. For a proposed community link and the public contact route, see [listing notes](docs/hackios-listing.md). Bug reports and reproducible, redacted observations are welcome in [this repository's Issues](https://github.com/leafall903/ios-sweet-umi/issues).
