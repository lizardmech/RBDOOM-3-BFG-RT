# Current integration status

Runtime acceptance recorded 2026-09-07: the user reported approximately 30 minutes of extensive play across multiple levels with everything else appearing fine. I4 fixed the major acceleration-structure GPU regression. The remaining small upload difference was explicitly accepted; further light-domain or performance tuning is closed. The user then authorized continuing with main-branch integration. See acceptance.md for scope and receipts.

The accepted integration includes I1 through I5, ending at code checkpoint 9f2c36704. The distinct I5 EXE is EF03F967B9E3D8DBEC082A043725D8EDC584AFC567A06523E39753BB6FC4AB5E, with matching MAP and shaders in E:/prog/rbdoom-3-BFG-prebuilt_cpu_integration. Use launch-rewrite-I5-uploads.cmd for that candidate. The generic launch-rewrite.cmd still selects I3; prior candidates remain rollback artifacts. Source defaults remain opt-in (rewrite=0); the tested launcher explicitly requests rewrite=1 and fail-on-recovery=1. This integration does not promote the original runtime launcher or change those defaults.

The earlier persistent rejection was not localized to a root cause. I3 added the user-requested fatal-on-recovery policy and rejection-site diagnostics; subsequent reported gameplay did not expose another issue. This is practical runtime acceptance, not proof of exhaustive stress coverage or a claim that the earlier rejection was repaired. See I3_recovery_failure.md.

Launcher correction after user feedback matched the original launchUTP.bat arguments and saved settings, with zero shared renderer CVar differences in live probes. Earlier automated smoke evidence below used the broader upt.cfg configuration. Keep effective settings equal for comparisons; the user subsequently tested UPT and UPTX and accepted their minor remaining disparity. See launcher_baseline.md.

Candidate: codex/cpu-producer-integration-20260907, based on restir-development 5a58d58a.
Accepted producer source: 794929bf (R4-039 complete-source checkpoint).

The first integration imports the 158 neo source/build/test paths in source-manifest.tsv. It retains the legacy route and its production dependencies. It does not import historical checkpoint documents, external-agent machinery or producer shader files. Main's 73 destination-only neo files have identical content after newline normalization; all existing main CVar defaults are unchanged. The rewrite default remains 0.

Local integration review resolved two textual conflicts and these semantic boundaries:

- Analytic-light fixed UPT emitter policy is captured with immutable worker inputs; current main radiance packing applies to snapshot publication as well as the synchronous path and remap records.
- The retained material-row configuration signature includes main's emissive proposal/surface suppression controls, so changed diagnostics rebuild the rows.
- Both scene routes use the same upload-only Lambert material transformation; authored CPU rows and light preparation remain intact.
- Rewrite geometry suppression uses owner-captured values and the existing idle-boundary lifecycle invalidation/drain protocol; cached geometry is rediscovered when the setting changes.
- Main's opaque diffuse/additive-emission coverage fix, both mesh/instance material identities, current dispatch code and sky UAV layout survive the merge.
- Protected Discovery is copied byte-for-byte from the verified accepted source.

Evidence directory: E:/prog/cpu-producer-integration-20260907. It contains original checkout status manifests, accepted build/deployment records, runtime shader/config manifests and build/test logs. The isolated preparation preserved both original checkouts. The approved final transfer advances restir-development while preserving its unrelated working files; main-merge contains before/after receipts.

Code checkpoints: d4bfbfd7d (integration), f0879461e (lookup allocation), aea3f841c (fail-on-recovery), 4ef597cb7 (batched skinned update barriers), and 9f2c36704 (exact lighting-upload reuse). The latest Vulkan Release build passes and all 36 registered harnesses pass on I5 (22.54 seconds). Native I2 smoke and later user gameplay observations are recorded separately, with their limitations.

Runtime pairing matters: 190 of main's 432 Vulkan shader files differ from the accepted CPU runtime. The combined executable must use a matching main shader set. Keep the accepted CPU EXE/MAP and shaders together as the comparison baseline.
## Historical I1/I2 validation record

The remaining sections record the earlier baseline validation and deployment. Their pending-acceptance statements and candidate names are superseded by the current status above.

## Re-evaluated aggregate harness assertions

The first candidate CTest run passed CPU planning and the rewrite harness (19.76 seconds), while the aggregate reported four source pins. Source inspection established:

- TLAS commit ordering: the first whole-file CommitRayTracingSmokeSceneResources occurrence now belongs to TryBuildCpuProducerRewriteScene. The legacy descriptor assignment and later commit remain ordered. The assertion now searches the final commit after the candidate descriptor assignment.
- Rigid cache snapshot and commit: Refresh calls SnapshotRigidCpuMeshPointers, derives localVerts/localIndexes, invokes RtPathTraceBuildRigidOwnedCpuCache into a temporary, then swaps vertices/indexes and installs bounds/signature/valid state. The harness now follows that extracted helper as well as the refresh caller; it checks copied inputs, allocation/conversion, bounds, early reuse and publication ordering.
- Skinned history: accepted R4-039 uses RtCpuRewriteCommittedPoseValid plus per-instance identity and service->LastCommittedRootFrame(), replacing the older RtCpuRewriteSkinnedPreviousPoseValid predicate. The assertion now requires those current provenance guards.

No functional checks or features were disabled. Native implementation for the rigid-cache helper and committed skinned pose is unchanged from the accepted checkpoint. The final aggregate run passes; see tests-final.log.

## Build registration and protected-byte follow-up

Native link exposed an existing Windows CMake condition: all NVRHI renderer sources were appended only when USE_DX12 was enabled. The Vulkan-only candidate omitted them and failed with unresolved renderer symbols. I1 now changes that registration to USE_DX12 OR USE_VULKAN; backend selection and DX12 compilation remain disabled. The corrected native Vulkan-only build passes; see renderer-build.log.

The protected Discovery file has mixed line endings (2777 LF, including 1988 CRLF). Git checkout conversion changed its raw hash during initial transfer. The source has been restored byte-for-byte, and one .gitattributes -text entry preserves those bytes in future checkouts. This supporting repository file is added to I1's boundary; no protected implementation edit is made.

## Build and test receipt

Fresh build directory: build-integration-vulkan. Visual Studio 2022 x64 Release, Vulkan/Optick/BUILD_TESTING/RETAIL enabled; DX12, DXIL shaders, OpenAL and FFmpeg disabled. Slang UPT artifacts enabled and compiled with Vulkan SDK 1.4.341.1. DX12 execution is not validated. The existing code-page and shader warnings remain in the logs.

- Producer/planning/rewrite, light-manager, binding, registry, rigid, acceleration-pack and identity: 9/9 passed, 21.35 seconds; rewrite harness 19.88 seconds.
- Capture transaction, semantic/committed/proposal/referenced-set, instance observation and R1: 9/9 passed, 0.63 seconds.
- Current I2 EXE SHA256: 75D3AC6C9031A69E4CB6161E6CCF23E494ACB9DD9CE696B41A508F52D791973E.
- Matching I2 MAP SHA256: 3F001C614A26119E95C9B7764A9E6FBF9A08714E314F3E05A762040C96AF5D0F.
- Separate deployment: E:/prog/rbdoom-3-BFG-prebuilt_cpu_integration. Uses original game assets through fs_basepath and its own writable fs_savepath. Original launchers, settings and binaries remain intact.
- Shaders: original current-main Vulkan set seeded, then 303 freshly built candidate artifacts overlaid. This avoids pairing the new executable with the older accepted producer shader set.

## Native runtime evidence

The initial mars_city1 run with main's upt.cfg, rewrite=1, GPU skinning=1 and parallel AddModels=1 loaded and exited normally. Its 60-frame Optick capture contains 60 frozen geometry builds, resident reuse, material/light worker activity, skinned consumption and geometry/skinned finalization. This proves native worker/commit activity; it does not prove complete UPT consumption or GPU speed. The automatic capture uses Optick INSTRUMENTATION without TAGS or GPU, so tag counters and GPU timings are not present.

After initial UPT dispatch, the console reported "UPT-04 live input closure is incomplete; initial dispatch skipped". Diagnostics established all required handles were present, while the emissive lookup was inexact. The 4096 native emissive identities were unique; the existing 2x and 4x hash-table capacities required 29 and 20 probes, exceeding the unchanged shader maximum of 16. The 8x capacity fits the same records within 16 probes.

I2 adds bounded 8x and 16x allocation retries to the CPU builder. It changes no shader source, hash, probe limit, identity/PDF rejection or GPU resource ABI. Its new compact native fixture fails against the prior production implementation and passes against the fix, checking all 4096 shader-style lookups plus duplicate-identity and malformed-CDF rejection. The existing 48 complete light-manager output comparisons also pass. See I2_lookup_retry.md and I2-regression-before/after.log.

The final native Vulkan build passes (I2-native-final-build.log). Both restored diagnostic translation units were explicitly recompiled, and the deployed EXE was checked for absence of temporary diagnostic strings. I1 and diagnostic artifacts are retained only as evidence/rollback; launch-rewrite.cmd and launch-legacy.cmd point to I2.

Final native results:

- mars_city1: settled rewrite, legacy switch, rewrite re-entry and map reload all reached their completion markers; orderly shutdown, no incomplete UPT input closure or announced recovery. Main UPT three-vertex split, temporal and spatial routes were effective.
- alphalabs1: give-all/NPC-spawn commands and dynamic test point light completed; gameplay image saved and orderly shutdown. No UPT input rejection or announced recovery. This exercises dynamic membership and skinning consumption, not exhaustive animation/reflection correctness.
- Each 60-frame capture records 60 frozen geometry builds, GPU geometry commits and skinned finalizations. Gameplay also records 60 skinned consumes, light-manager jobs, light preparations and material jobs. New worker products reach committed native scenes. Automatic instrumentation excludes tag counters/GPU timings; no timing comparison is claimed.
- The saved gameplay image shows room geometry, player hands and the spawned NPC; it is noisy with AA=None. The initial mars_city1 captures were cinematic views. These images do not establish settled visual equivalence, RR behavior or performance. User checks of weapon changes, moving/skinned reflections and dynamic/emissive appearance remain necessary.
- Full final CTest: 36/36 passed, 22.10 seconds (I2-complete-tests.log). The first full sweep found 18 targets not yet built; all were then built and the complete sweep passed. No tests were disabled.
- Original checkout statuses still match their starting snapshots. Original runtime shaders/config hashes remain unchanged; task-created screenshots were moved from the engine's fs_basepath screenshot directory into the evidence directory. The isolated user settings were restored after scripted checks.

Current candidate launchers are E:/prog/rbdoom-3-BFG-prebuilt_cpu_integration/launch-rewrite.cmd and launch-legacy.cmd. Source defaults are unchanged (rewrite=0). The manually selected rewrite launcher requests 1. The accepted R4-039 runtime and both original working checkouts remain available.

The production path is documented in architecture.md. No structural refactoring or deletion of still-reachable legacy plumbing was performed. Main promotion requires the remaining user image/event observations and the final merge decision; default promotion is separate.
