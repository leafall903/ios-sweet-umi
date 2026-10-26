# Xiaomi Mi 10 / Adreno 650 status

**Updated 2026-10-26 · Current published checkpoint: iOS 16.5 v6.** This replaces the earlier v18/v19 probe-only status. See the [current feature table](../README.md) and [source checkpoint](../devices/umi/README.md).

## What has actually progressed

The initial v18 read-only register probe and v19 CX-domain test were followed by a working experimental A650 path. Later device logs recorded GMU/HFI/CP/fence progress and an FD650 RGBA clear/readback result (64, 128, 191, 255). Subsequent builds connected the Mesa/freedreno GL bridge to the window-server rendering path. Those results are stronger than observing a power-domain bit, but they do not establish complete Metal or acceleration in every app.

The v6 source publication includes the `SweetGPU` client, `sweet_gpu` backend, framebuffer/surface support, GL bridge and a pinned Mesa 25.0.7 code patch. The final display path still includes a CPU framebuffer copy. The owner reports improved usability and no remaining display corruption in the v6 test, but still requests smoother operation. **No sustained, reproducible 60 fps result for v6 is available.** Earlier CPU-era driver call counters are not a current animation-FPS benchmark.

## Device-specific configuration

The phone reports Adreno 650 v3 in Recovery KGSL. The research device-tree profile records chip ID `0x06050002`, GPU `0x03d00000/0x40000`, GMU `0x03d6a000/0x30000`, GPUCC `0x03d90000/0x9000`, and GPU SMMU `0x03da0000/0x10000`. These constants describe this platform; the separate [parameter archive](a650/) is not by itself an executable driver.

Redwood's Adreno 642L configuration is a reference, not a drop-in initialization sequence for A650. Firmware, memory ranges, SMMU context routing and GPU/GMU setup require device-specific treatment. Vendor firmware must be supplied locally and is not included in this repository.

## v6 graphics/input changes

- SweetSurface accepts the actual minimal non-planar client-memory span, including a partial final padded row, while rejecting malformed or undersized geometry. Owned/planar policy is retained.
- The GL image table uses 2048 stable heap entries and balances surface/group ownership through replacement, deletion and teardown.
- The v5 early `FD_MESA_DEBUG=sysmem` wrapper and native FTS no-op handling are retained.
- Widgets were reported normal in v5. Wallpaper had appeared before v6, with broken effects in earlier tests. The owner reported no remaining display errors in v6; every style/effect/provider has not been independently audited.

## Still incomplete

Sustained frame-time and latency measurements, power/thermal behavior, wider resource lifetime/fault recovery tests, Metal-dependent app paths and more direct display presentation remain work items. Maps and Settings → Date & Time are known owner-reported failures. Other hardware and Apple services are not established by this graphics result. Untested local v7 work is excluded from this checkpoint.

Sources, exact dependency pins and test limitations are in [reproduction](../docs/reproduction.md), [provenance](../docs/provenance.md), and the [license inventory](../THIRD_PARTY_LICENSES.md).
