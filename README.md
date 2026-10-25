# iOS 16 on Xiaomi Mi 10 (umi): community research

This repository contains an experimental, independent porting effort for running an iOS 16 research build on the Xiaomi Mi 10 (`umi`, Qualcomm SM8250). It is not an official HackiOS release and does not imply support or endorsement by Apple, Xiaomi, or the HackiOS maintainers.

## Current status

- A research build has reached the iOS lock screen and UI on the Mi 10.
- The current UI uses a CPU framebuffer path. Adreno 650 hardware acceleration is **not implemented**.
- GPUCC/CX experiments were diagnostic probes only. They did not establish working GMU firmware, SMMU mappings, command submission, or GPU rendering.
- A wallpaper/widget compatibility candidate was installed and tested on-device; the owner reported no meaningful improvement. Wallpaper and widget behavior remain unresolved.
- There is no supported installer or public device image in this repository. Do not treat the patches as a ready-to-install distribution.

See [the Mi 10 status report](research/umi10-a650-status.md) and [A650 platform notes](research/a650/README.md) for evidence, known gaps, and safe next steps.

## Safety and scope

This is experimental low-level software. Use only on a device you own, keep a verified recovery path and backups, and understand that boot experiments can leave the device temporarily unable to start its normal OS. This repository intentionally excludes device backups, firmware binaries, raw crash logs, and identifying device data.
