# Adreno 650 platform profile (Xiaomi Mi 10 / umi)

This folder contains a small, non-operational platform profile for research and review. It documents resource inputs; it does not map MMIO, change clocks, load firmware, or start the GPU.

- `umi-platform.json`: redacted device platform profile and source references.
- `UmiA650Platform.h`: matching C structure with address/size constants.

The values were copied from the local merged-device-tree research profile and are not a substitute for validating the active device tree, access permissions, power dependencies, or register semantics before any driver work. Firmware binaries and device backups are deliberately omitted.
