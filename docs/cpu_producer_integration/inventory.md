# CPU producer integration inventory and order I1

2026-09-07. Preparation authorized by the current request to bring the accepted CPU producer into main. This candidate preserves both live checkouts; main merge and default promotion follow a tested candidate and user runtime observations. Single-agent execution.

## Verified sources

- Destination restir-development: 5a58d58a2f5a8626ae55186b3110fd13b0b9c46e.
- Producer branch HEAD: 7625a4edf9f1f05c0cf701e3b02a097b94dd4da3 (incomplete).
- Accepted source: 794929bf0af70ab80e7764776a20240560891fb4, tree 76e666bbc079397a5ff12e4a0e79de80fd67e697.
- Common base: c742a1abd2e7948fcf7de4389d880ea1f462af39.
- 2209 neo file hashes verified against reviewed.json; zero mismatches.
- Protected Discovery source hash EC769D35F156DD21184CBBA4A7E149FEF46E3FB7628DD53F69806DD82806C404. Import intact. Main has no independent change to this file since the common base.
- Original status manifests and R4-039 records exported alongside this inventory.

## Transfer boundary

Exact initial source boundary: producer-source-delta.tsv (158 files: 87 added, 71 modified). This is the common-base to accepted-checkpoint neo delta, not a whole-checkpoint import. It contains no shader changes. Retain source/build/test dependencies of the functioning legacy route for the first baseline. DeviceManager_VK.cpp adds accepted Vulkan exception reporting, with no renderer algorithm change.

| Family | Source and required dependencies |
| --- | --- |
| Capture and lifecycle | common_frame.cpp idle boundary; tr_frontend_main.cpp root hooks; tr_frontend_addmodels.cpp admission; RenderSystem/RenderWorld/Model and vertex-cache identity/lifetime changes |
| Resident geometry and jobs | PathTraceCpuProducerRewrite.cpp/.h; immutable source owners, frozen products, exact root overlays; geometry/instance universes and lifecycle identities |
| Material jobs and binding | SceneCapture, TextureRegistry, DynamicMaterialState, Material metadata, pure material/evaluation/binding/record kernels, protected Discovery; Image resource-generation support |
| Lighting | DoomLights owned snapshots, RemixLightManager prepare/apply, EmissiveCandidates and existing light/unified consumers |
| GPU owner commit | SmokeSceneBuild, CpuProducerRewriteSkinned, PrimaryPass state, Skinning, SkinnedHitRoute, rigid preparation, acceleration allocation/pack/resident helpers and resource retirement |
| Legacy producer dependencies | ProducerLanes, capture/committed products, semantic pipeline, apply/publish/packer, associated kernels and harnesses; reachable in the rewrite-disabled route, retain initially |
| Build and tests | neo/CMakeLists.txt registrations plus all added/changed neo/tests in manifest; retain existing main shader build files |

Native chain verified: root begin -> PrepareResidentEntities and admitted model capture -> root seal -> geometry worker -> TryBuildCpuProducerRewriteScene -> current material membership and worker jobs -> early analytic capture plus dependent light/manager preparation -> owner uploads/skinning/AS -> final committed scene/history publication. Successful rewrite return skips the subsequent legacy scene-build body. Four workers retain their accepted job channels and join sequencing. ReleaseConsumedOverlay(true) occurs only at successful final publication; other releases discard. Frame-boundary route transitions wait for existing backend drain acknowledgement.

## Destination and overlap decisions

Main changes 83 neo files since base; 10 overlap producer changes. All destination-only files, including UPT implementation and shaders, stay at destination HEAD.

- CVars.cpp/.h: union producer controls with current main UPT controls; rewrite default remains 0.
- DoomLights.cpp: preserve main radiance scaling, fixed UPT emitter radius and power conventions in both synchronous and owned snapshot paths. Capture cvars on owner; workers read owned values.
- DoomMaterialClassifier.cpp: preserve main opaque diffuse plus additive-emission coverage correction in the producer classifier kernel/cache path.
- DynamicMaterialState.cpp: preserve emissive diagnostic suppression and signature invalidation in material preparation/application.
- GeometryUniverse.cpp and SceneCapture.cpp: retain diagnostic suppression and both mesh/instance material identity publication; inspect equivalent rewrite paths for bypasses.
- SmokeDispatch.cpp and SmokeResources.cpp: retain current main UPT dispatch and sky UAV binding contract while adding producer lifecycle/resource changes.
- SmokeSceneBuild.cpp: retain current main material-table coverage, Lambert diagnostic, emissive diagnostic and light publication behavior while integrating the rewrite early return. Textual merge alone is insufficient evidence for the new route.

## Numbered order I1

1. Create an isolated clone at E:/prog/rbdoom-3-bfg-rt-cpu-integration on codex/cpu-producer-integration-20260907, based on the verified destination HEAD; import accepted source objects and only the manifest delta.
2. Resolve textual overlaps and trace the semantic decisions above. Keep concurrency, ownership, capacity, generation checks and GPU lifetime machinery intact. No shader algorithm changes or custom viewmodel system.
3. Configure a fresh Visual Studio 2022 x64 Release build with Vulkan and Optick, DX12/DXIL disabled. Read main UPT build settings and validate relevant shader closure/compile configuration. Build renderer and affected producer/material/registry/identity harnesses; re-evaluate standing source assertions against actual code.
4. Self-review the combined diff and checkpoint cohesive integration code separately from cleanup. Verify source manifest and destination-only file preservation.
5. Prepare a distinct rollback-safe runtime candidate and record matching shaders/assets/configuration. Obtain compact user observations for demanding scene, membership events, motion/reflections/lights and route/map transitions. Harness/build evidence cannot establish visual/performance acceptance or worker consumption.
6. After baseline validation, remove only proven unused plumbing and document current architecture; extraction/cleanup receives separate commits and focused revalidation.
7. Present concrete commits, build/test/runtime evidence and limitations for main merge decision. Do not promote the rewrite default as an integration side effect.

## Validation contracts and limits

Keep accepted slot ownership, source lifetimes, 65536 instance limit, overlay growth bounds, packed geometry budgets and latched recovery behavior. Preserve existing Optick worker/consume/rejection counters and require consumption plus absence of replaced work in runtime evidence. Recovery is not rewrite success.

Accepted producer build cache: VS17 2022 x64, Release, OPTICK ON, Vulkan ON, DXIL shaders OFF, UPT04 OFF; DX12 backend happened to be enabled. New candidate uses explicit Vulkan-only settings and reports DX12 as untested. Do not reuse its absolute-path CMake cache.

Stop dependent implementation for missing accepted bytes, unresolved ownership/semantic conflict, protected-file alteration or functional regression. No silent fallback/test disabling. Preserve original branches, indexes, dirty files, launchers and rollback builds.

## Bounded cleanup candidates

Potentially unused names are candidates only, not deletions: R1 ledger/cache-gate and superseded semantic APIs. ProducerLanes and capture/committed baseline plumbing are demonstrably called in SmokeSceneBuild when rewrite is disabled. Check callers, CMake registration and harness consumers before removal. Extracting coherent backend commit/material/light responsibilities is secondary to a working integrated baseline.