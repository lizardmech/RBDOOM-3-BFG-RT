# CPU producer integration acceptance

2026-09-07. The user reported extensive gameplay across multiple levels for approximately 30 minutes with everything else appearing fine. They confirmed I4 fixed the large AS build/update GPU regression, observed little change from I5 on UPT, and reported that UPTX did not show the minor residual issue. They explicitly accepted that difference and asked to stop tuning it. Following the proposed move to main-branch integration, the user replied "yes continue".

This authorizes transferring the validated integration into the active restir-development branch. It does not request another renderer redesign, light-domain change, default-route change, runtime replacement, or remote push. Keep the unrelated dirty main files and original producer checkout intact. Legacy/default selection and named rollback executables remain available.

## Candidate and validation

- Integration branch: codex/cpu-producer-integration-20260907, based on main 5a58d58a2f5a8626ae55186b3110fd13b0b9c46e.
- Complete accepted producer source: 794929bf0af70ab80e7764776a20240560891fb4, selectively reconciled with main in d4bfbfd7d.
- Final code checkpoint: 9f2c36704, including I2 lookup retry, I3 fail-on-recovery, I4 batched skinned barriers and I5 exact lighting-upload reuse. Acceptance documentation changes no executable source.
- Latest Vulkan-only Release build passed. All 36 CTests passed in 22.54 seconds; no checks disabled. Evidence: E:/prog/cpu-producer-integration-20260907/I5-light-uploads.
- Candidate EXE: RBDoom3BFG.cpu-integration-I5-EF03F967-20260907.exe, SHA256 EF03F967B9E3D8DBEC082A043725D8EDC584AFC567A06523E39753BB6FC4AB5E.
- Matching MAP SHA256: 558BA277560741ABA34B1854C6E60A407BEAFC694DBC41A646D1B83ADE9371D1.
- Runtime: E:/prog/rbdoom-3-BFG-prebuilt_cpu_integration; launch-rewrite-I5-uploads.cmd selects the I5 candidate with rewrite=1 and fail-on-recovery=1. The generic launch-rewrite.cmd is the older I3 launcher.
- The user's final 30-minute report did not enumerate exact maps or re-state the executable hash. Record it as acceptance of the integration under discussion, not a hash-attested benchmark or exhaustive stress test.
- Protected PathTraceMaterialTextureDiscovery.cpp SHA256: EC769D35F156DD21184CBBA4A7E149FEF46E3FB7628DD53F69806DD82806C404.

## Remaining limits

The earlier reason-4 persistent rejection was not localized; I3 makes requested recovery fatal and records its source. Later user testing exposed no further issue, but is not proof of a root-cause repair. The residual upload difference is accepted, not a pending optimization gate. No exact GPU savings are claimed beyond the user's confirmation that the major AS regression is fixed. DX12 was not built or validated.

The final transfer uses a fast-forward from the unchanged main base, preserving the reviewable integration/fix commits and tested source. E:/prog/cpu-producer-integration-20260907/main-merge stores local preflight/transfer verification. Source default remains r_pathTracingCpuProducerRewrite=0; use the explicit rewrite setting to activate it. Structural cleanup is deferred to separately scoped work after this accepted baseline is landed.
