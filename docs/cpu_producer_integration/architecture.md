# CPU producer: current architecture

The default CPU producer builds geometry, material and light inputs for the existing Vulkan path tracer. The backend still owns GPU resources, command recording and final scene publication. r_pathTracingCpuProducerRewrite defaults to 1, requesting warmup and the rewrite route; explicitly setting it to 0 selects the retained legacy route for troubleshooting. Route transitions occur after rendering and Game/Draw have joined.

## Follow one frame

1. framework/common_frame.cpp calls RtCpuProducerRewrite_OnFrameBoundary at the idle boundary. Pending recovery or user route changes use the existing worker drain and backend acknowledgement. Geometry diagnostic changes invalidate retained membership at this same boundary.
2. renderer/tr_frontend_main.cpp begins a root view. PathTraceCpuProducerRewrite.cpp prepares resident entities; tr_frontend_addmodels.cpp admits current model surfaces. Capture retains owned geometry bytes and copies current transforms/joints. Resident static discovery is separate from dirty mesh preparation.
3. Root seal finishes retained entity/static capture and copies analytic light inputs. The geometry worker reads sealed inputs and publishes a complete frozen product. Capture slots and frozen products carry lifecycle, world/map, configuration and identity witnesses.
4. PathTraceSmokeSceneBuild.cpp::TryBuildCpuProducerRewriteScene acquires compatible geometry and a current root overlay. It checks the join, resolves current material membership, prepares bindings/numeric state and starts rigid/material-record planning. Early analytic work can overlap material preparation.
5. Light preparation assembles current emissive and analytic inputs, then joins the dependent light-manager product. Owner code applies texture/resource snapshots and prepared records. Main's UPT light radiance convention applies to both synchronous and snapshot publication.
6. The backend prepares/uploads changed buffers, commits rigid/skinned geometry and records acceleration work. PathTraceCpuProducerRewriteSkinned.cpp owns the skinned commit transaction. Resources are published only through the final scene commit, and prior resources remain alive through the existing retirement policy.
7. Successful final publication advances committed pose history and releases the consumed overlay with committed=true. Rejection releases discard history. Returning from the rewrite function can also mean reusing a previous GPU scene; that is distinct from consuming a new producer result.

## Ownership and scheduling

| Owner | Responsibility and lifetime |
| --- | --- |
| Frontend | Native model/material/entity access, resident membership, source identity, current pose and material/light capture. Renderer pointers remain on this side of the capture boundary. |
| Geometry worker | Sealed geometry inputs -> immutable frozen geometry products. Unchanged mesh conversion can be reused across membership edits; products do not retain unbounded predecessor chains. |
| Shading worker | Numeric/current material preparation and light preparation share one accepted job channel. |
| Light-manager worker | Material-binding preparation and light-manager preparation share one accepted job channel. |
| Planning worker | Rigid preparation and material-record construction share one accepted job channel; finish one before reusing it. |
| Backend owner | Compatibility decisions, current resource resolution/loading, bounded application, uploads, skinning/AS commands, scene publication and GPU retirement. |

A retained geometry frame number is not a latency policy for every field. Current root transforms, joints, material values and light inputs require their own exact-frame witnesses. Previous skinned poses must match instance/source identity and the last successfully committed scene. Failed attempts cannot advance that history.

## Source map

All paths below are relative to neo/renderer/NVRHI unless otherwise stated.

- PathTraceCpuProducerRewrite.h/.cpp: states, slots, owned carriers, resident capture, workers, publication, joins, recovery and counters.
- PathTraceCpuProducerRewriteSkinned.cpp: skinned GPU preparation/commit/finalization and committed motion history.
- PathTraceSmokeSceneBuild.cpp: backend orchestration, material/light job sequencing, uploads and final commit.
- PathTraceSceneCapture.cpp: current membership, captured material inputs and worker integration.
- PathTraceTextureRegistry.cpp: owned binding definitions, revisions, resource freshness and first-use hydration.
- PathTraceDynamicMaterialState.cpp: material rows, cache signatures, texture slots and resident table refresh.
- PathTraceRuntimeMaterialEvalKernel.h, PathTraceMaterialFrameKernel.h, PathTraceMaterialBindingKernel.h and PathTraceMaterialRecordKernel.h: copied CPU input transformations.
- PathTraceDoomLights.cpp and PathTraceRemixLightManager.cpp: analytic snapshots, prepared light products and owner publication.
- PathTraceRigidCandidatePreparedDelta.cpp and PathTraceRigidPreparedPayload.h: revision-checked CPU cache preparation and owned mesh conversion.
- PathTraceAcceleration*, PathTraceSkinning*, PathTraceSkinnedHitRoute* and PathTracePrimaryPass.h: GPU plans/resources and retained backend state.
- renderer/RenderSystem_init.cpp, renderer/RenderWorld_load.cpp and PathTraceGeometryLifecycle.cpp: startup, shutdown, video/map reset and identity lifetime.

PathTraceMaterialTextureDiscovery.cpp is the protected accepted implementation; see inventory.md for its exact hash. Do not alter it as incidental integration cleanup.

## Failure and diagnostics

Capacity pressure and persistent scene rejection request recovery. r_pathTracingCpuProducerRewriteFailOnRecovery defaults to 1: the idle boundary reports the reason and raises an engine fatal error before switching to legacy. Explicitly setting it to 0 permits the existing announced, latched legacy recovery. Temporary keep-last reuse is still permitted. The accepted instance limit is 65536; overlay rows/joints grow within fixed byte bounds. Rigid packing and GPU retirement retain their byte/count checks. A fallback that renders correctly is not proof of rewrite consumption.

For runtime evidence, inspect the PT CPU worker scopes, GPU commit/reuse counters, material/light consumption, rejection reasons and rewriteConsecutiveRejectedFrames/rewriteRecoveryReason. Require the replaced legacy owner work to be skipped on successful rewrite frames. CPU harnesses validate contracts and deterministic behavior; they do not establish native GPU appearance or frame-time improvement.

The older ProducerLanes/capture/committed/semantic modules are still reachable through the legacy route or diagnostic consumers. R1 ledger/cache-gate code is referenced by SceneCapture and DrawSurfCapture. Historical names alone are not dead-code evidence.

## Working on this code

Use the source map and integration status as the operational entry point. Keep the original numbered producer reports in the producer checkout as history. Preserve the accepted algorithms and ownership until a measured defect justifies changing them. Keep cleanup separate from integration changes; runtime acceptance of this combined build remains required before structural refactoring or main promotion.
