# Xiaomi Mi 10 iOS 16 research status

**Snapshot:** 2026-10-25  
**Device:** Xiaomi Mi 10 (`umi`), Qualcomm SM8250, Adreno 650 v3 (reported by Recovery KGSL)

## Results observed on the device

- The experimental iOS 16 build reached the lock screen and UI. Multi-core bring-up was tested up to eight CPUs; this is separate from GPU acceleration.
- The UI currently uses a CPU framebuffer. The user reported substantial improvement from later CPU/presentation changes, but it remains visibly less responsive than a 60 Hz-class experience. No verified 60 fps result is available.
- A wallpaper/widget compatibility candidate was installed and tested. The user reported no meaningful improvement; widgets may remain blank/black and wallpaper behavior is unresolved.
- Recovered diagnostic summaries showed `PosterBoard` exiting in the `PBFHomeScreenPreviewImageBuilder _imageForView:` path and `ScreenshotServicesService` aborting around Metal compute pipeline `setComputeFunction:`. These are observations, not a demonstrated root cause or proof that a compatibility library loaded correctly.

## Adreno 650 probes

- Device-tree inputs identify GPU chip ID `0x06050002`. The current local platform profile records GPU `0x03d00000/0x40000`, GMU `0x03d6a000/0x30000`, GPUCC `0x03d90000/0x9000`, and GPU SMMU `0x03da0000/0x10000`.
- A read-only v18 probe reached iOS and recorded GPUCC state. A v19 CX-domain probe briefly observed `power_on=1`, restored the collapse-control bit, and then reached iOS.
- Neither probe establishes successful firmware execution, GMU/HFI communication, 3D power-up, SMMU translation, command submission, or rendering. No GPU register bring-up code is presented here as a working driver.
- Recovery Linux reported a maximum GPU clock of 587 MHz. This is Linux-driver metadata and says nothing about iOS performance.

## Difference from the Redwood reference

The HackiOS XNU reference targets a Redwood Adreno 642L configuration, while this device reports an A650 family ID. Local notes identified different chip ID and GMU/SMMU/RSCC resource sizes, and A650 firmware names. Copying the Redwood sequence as-is would be unsafe and does not amount to an A650 port.

## Work still required for acceleration

1. Validate the exact A650 power, reset, clock, and GMU firmware path against the device tree and vendor/Linux implementation.
2. Implement and validate SMMU stream routing and page tables, cache coherency, GMU/HFI, command rings, and fault recovery.
3. Connect a compatible XNU GPU client and userspace stack to CoreAnimation/display composition, including the Metal-dependent offscreen paths implicated by diagnostics.
4. Measure sustained frame times and validate wallpaper, widgets, transparency, and touch behavior on-device.

Until these steps are demonstrated end to end, the project should be described as **booting iOS UI with experimental A650 diagnostics**, not “A650 accelerated”.

## References

- [HackiOS XNU kernel](https://github.com/oosexgang/hackios-xnu-kernel)
- [Xiaomi SM8250 Linux GPU driver sources](https://github.com/xiaomi-sm8250-devs/android_kernel_xiaomi_sm8250/tree/lineage-20/drivers/gpu/msm)
