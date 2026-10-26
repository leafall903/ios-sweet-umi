# iOS 16.5 comparison and v6 checkpoint

**Updated 2026-10-26. Current device-tested checkpoint: iOS 16.5 / 20F66 v6.** Earlier versions stalled during first boot; that is historical, not the current desktop status. The separately retained 16.7.16/v23 image remains a local fallback.

## Why build a separate 16.5 root

The owner requested a complete upstream-style 16.5 comparison without overwriting the retained working fallback. The local root was prepared from the iPhone X OS/SystemOS/AppOS inputs, including staged applications, rather than merely changing the version string of the 16.7.16 root. The source publication contains none of those proprietary files or images.

The user-space build is **20F66**; the ported kernel remains based on **XNU 8796.141.3**. These version identifiers describe different layers.

## First-boot failures resolved during bring-up

The first failure stopped at root discovery because the generated root policy still accepted the previous HFSX volume identity. Rebuilding that policy for the new local root resolved the mismatch without weakening geometry or dual-header checks. `tools/make_root_volume_policy.py` publishes the reusable generator, while actual volume identities remain private.

A later shutdown-notification timeout followed a failed startup/restart path. Investigation found missing experimental first-start state in the new root, including the placeholder system keybag and log migration markers used by the older research image. `tools/umi_startup_seed.py` restores only the public experimental placeholders, rejects unknown existing state and preserves required ownership. It is not a SEP or real data-protection implementation. The tool and its failure cases are covered by the bootstrap tests.

## Device-test progression

| Checkpoint | Owner observation | Interpretation |
| --- | --- | --- |
| 16.5 v1/v2 | Text/panic before usable UI | Root identity and later first-start failures required separate fixes. |
| v3/v4 diagnostics | UI and wallpaper appeared; touch/visual effects still faulty | Reaching the interface did not establish a finished graphics/input path. |
| v5 | Widgets reported normal; broken wallpaper effects and occasional rendering errors remained | FTS idle-event handling and early Mesa sysmem selection retained for the next test. |
| **v6** | **Owner reported no remaining display errors and requested further performance/features work** | Last device-tested checkpoint selected for this source publication. No sustained 60 fps result or exhaustive app audit. |
| v7 | Further feature/performance work exists locally | Untested and excluded from the v6 release. |

v6 combines a minimum valid non-planar client-memory span calculation in SweetSurface with GL image-table capacity/lifetime fixes. Owned/planar surface policy is retained. The source snapshots, hashes, dependency pins and production-function host tests are under `devices/umi/`.

Maps and Settings → Date & Time remain owner-reported failures. Keyboard functionality is not fully revalidated after the first-run-introduction workaround. Do not interpret a complete donor-file input as complete hardware or app compatibility.

Read [the current feature table](../README.md), [build limitations](reproduction.md), and [source provenance](provenance.md) for the published scope. Earlier detailed CPU-era measurements remain in the historical bring-up notes and are not v6 frame-rate measurements.
