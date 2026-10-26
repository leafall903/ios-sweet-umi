# Contributing

Please keep changes scoped to the tested Xiaomi Mi 10 checkpoint and state what you observed on hardware. A source check, a successful build, a sent image, a booted kernel and a usable UI are distinct results.

- Put new umi-only code under `devices/umi/`. Keep unrelated and untested candidates separate from the published v6 checkpoint.
- Supply source and exact dependency pins. New XNU changes must be exportable against the declared Apple base; v6 uses `8796.141.3`. An upstream HakiOS submission requires a separate rebase and its current naming rules.
- Preserve original authors, commit history, copyright notices and applicable licenses. XNU-derived code stays APSL; third-party files retain their own terms. Record new components in `NOTICE` and `THIRD_PARTY_LICENSES.md`. Mark uncertain provenance **to verify**.
- Do not attach iOS files, IPSWs, SDK or firmware-derived header dumps, screenshots, vendor firmware, disk/boot images, device IDs, activation material, keys or raw private logs. Describe the symptom and share only sanitized error text.
- Explain the trigger, observed before/after behavior, exact phone/touch variant, source revision and remaining limits. Do not report 60 fps from a display-mode setting or equate a successful GPU probe with full app acceleration.
- Run the source checks in the default README. CI checks publication boundaries, host regressions and patch application; on-device claims require separate evidence.

The current release intentionally preserves tested historical `Sweet*`/`Umi*` identifiers. Renaming them and rebasing to upstream HakiOS's different kernel base would be new engineering work and must not silently replace a tested checkpoint.

This independent repository is not an upstream merge request or an official supported-device listing. Follow [HakiOS contributing guidance](https://github.com/oosexgang/hackios-xnu-kernel/blob/main/CONTRIBUTING.md) when preparing a separate upstream contribution.
