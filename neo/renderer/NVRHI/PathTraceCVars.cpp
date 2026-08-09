#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCVars.h"

idCVar r_pathTracingDebugMode(
    "r_pathTracingDebugMode",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "RT smoke debug output mode: 0 = hit/miss, 1 = depth, 2 = interpolated normal, 3 = surface class, 4 = UV, 5 = geometric normal, 6 = material ID, 7 = material table, 8 = sampled diffuse texture, 9 = alpha test preview, 10 = albedo, 11 = translucent overlay inspection, 12 = translucent subtype, 13 = fixed Lambert lighting, 14 = selected point-light shadows, 15 = selected light influence, 16 = normal map, 17 = specular map, 18 = toy one-bounce path trace, 21 = solid drawSurf bounds boxes, 22 = wireframe drawSurf bounds boxes, 23 = experimental routed rigid TLAS instances, 24 = fallback-vs-rigid-route overlap validation, 25 = routed rigid lighting validation, 38 = skinned object-motion vector diagnostic, 39 = routed-rigid object-motion eligibility, 40 = routed-rigid object-motion vector diagnostic, 41 = combined skinned/routed-rigid object-motion vector diagnostic, 42 = packed primary object-motion flags, 43 = packed object-motion reprojection match, 44 = previous static snapshot binding, 45 = previous static reprojection match, 46 = previous static motion-vector diagnostic, 47 = combined geometry motion-vector diagnostic, 48 = combined geometry reprojection-match diagnostic, 49 = combined geometry motion-source diagnostic, 52 = routed-rigid transform parity, 57 = material classifier GPU route/class/BSDF, 58 = GEO-09 paired legacy/canonical skinned primary-hit audit. Retired values select production mode 0" );

idCVar r_pathTracingMode18TestPreset(
    "r_pathTracingMode18TestPreset",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "One-shot mode 18 toy PT test preset: 1 = source3 routed rigid mode18, 4 = depth-4 static/rigid residency validation stack" );

idCVar r_pathTracingDebugWidth(
    "r_pathTracingDebugWidth",
    "320",
    CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
    "RT smoke debug output width" );

idCVar r_pathTracingDebugHeight(
    "r_pathTracingDebugHeight",
    "180",
    CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
    "RT smoke debug output height" );

idCVar r_pathTracingTextureProbeIndex(
    "r_pathTracingTextureProbeIndex",
    "-1",
    CVAR_RENDERER | CVAR_INTEGER,
    "RT smoke debug mode 8 material table index to focus in texture probe logging; -1 = first safe texture" );

idCVar r_pathTracingTextureProbeReset(
    "r_pathTracingTextureProbeReset",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to release the latched RT smoke texture probe and select a new one" );

idCVar r_pathTracingPostProcess(
    "r_pathTracingPostProcess",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Enable PT HDR postprocess presentation path: 0 direct blit, 1 TonemapPass preview" );

idCVar r_pathTracingPostExposure(
    "r_pathTracingPostExposure",
    "-0.5",
    CVAR_RENDERER | CVAR_FLOAT,
    "PT postprocess exposure bias in stops, independent of raster r_exposure" );

idCVar r_pathTracingPostMinLuminance(
    "r_pathTracingPostMinLuminance",
    "0.02",
    CVAR_RENDERER | CVAR_FLOAT,
    "PT postprocess minimum adapted luminance clamp" );

idCVar r_pathTracingPostMaxLuminance(
    "r_pathTracingPostMaxLuminance",
    "0.5",
    CVAR_RENDERER | CVAR_FLOAT,
    "PT postprocess maximum adapted luminance clamp" );

idCVar r_pathTracingPostWhitePoint(
    "r_pathTracingPostWhitePoint",
    "3.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "PT postprocess tonemap white point" );

idCVar r_pathTracingPostACES(
    "r_pathTracingPostACES",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "PT postprocess ACES curve toggle for A/B comparison" );

idCVar r_pathTracingPostContrast(
    "r_pathTracingPostContrast",
    "1.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "PT postprocess contrast multiplier after tone mapping; 1.0 = neutral" );

idCVar r_pathTracingPostSaturation(
    "r_pathTracingPostSaturation",
    "1.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "PT postprocess saturation multiplier after tone mapping; 1.0 = neutral" );

idCVar r_pathTracingPostLUT(
    "r_pathTracingPostLUT",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Enable PT postprocess PNG strip LUT: image width must equal height*height" );

idCVar r_pathTracingPostLUTImage(
    "r_pathTracingPostLUTImage",
    "",
    CVAR_RENDERER,
    "PT postprocess LUT image path, e.g. textures/color_grading/my_lut.png" );

idCVar r_pathTracingPostLUTReload(
    "r_pathTracingPostLUTReload",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Reload the current PT postprocess LUT image once" );

idCVar r_pathTracingPostLUTDebug(
    "r_pathTracingPostLUTDebug",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Debug PT post LUT path: 0 normal, 1 bypass LUT sample, 2 show LUT white corner" );

idCVar r_pathTracingEmissiveInventoryMaxTriangles(
    "r_pathTracingEmissiveInventoryMaxTriangles",
    "4096",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum emissive triangles captured into the RT smoke inventory buffer" );

idCVar r_pathTracingSceneSource(
    "r_pathTracingSceneSource",
    "3",
    CVAR_RENDERER | CVAR_INTEGER,
    "PT scene producer source: 0 = legacy drawSurf producer only, 1 = scene-universe diagnostics only, 2 = full static scene-universe geometry plus dynamic drawSurf fallback, 3 = source3 portal-resident scene producer" );

idCVar r_pathTracingSceneSource2RigidEntities(
    "r_pathTracingSceneSource2RigidEntities",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Experimental source 2 rigid entity promotion: 0 = off, 1 = whole eligible rigid entities that contain emissive-capable surfaces, 2 = all eligible non-skinned non-callback static entity model surfaces" );

idCVar r_pathTracingAsyncBvh(
    "r_pathTracingAsyncBvh",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Master gate for async/buffered off-screen BVH. 0 = synchronous fallback." );

idCVar r_pathTracingAsyncBvhJobs(
    "r_pathTracingAsyncBvhJobs",
    "2",
    CVAR_RENDERER | CVAR_INTEGER,
    "Requested async BVH CPU planning worker count; values <= 0 disable the background snapshot worker path." );

idCVar r_pathTracingCpuPlanningAsync(
    "r_pathTracingCpuPlanningAsync",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic opt-in: run PT CPU acceleration planning from an owned snapshot on a background worker when possible; late or stale work falls back synchronously on the render thread" );

idCVar r_pathTracingInstanceUniverseDump(
    "r_pathTracingInstanceUniverseDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump diagnostics-only PT drawSurf mesh/instance mirror observations once" );

idCVar r_pathTracingRigidMeshUniverseDump(
    "r_pathTracingRigidMeshUniverseDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump diagnostics-only PT rigid drawSurf mesh reuse eligibility once" );

idCVar r_pathTracingRigidMeshValidate(
    "r_pathTracingRigidMeshValidate",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump a one-shot validation comparing rigid local-source records against the baked dynamic rigid payload" );

idCVar r_pathTracingRigidBlasPlanDump(
    "r_pathTracingRigidBlasPlanDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump a diagnostics-only reusable rigid BLAS/TLAS plan from local-source mesh records" );

idCVar r_pathTracingRigidBlasInputDump(
    "r_pathTracingRigidBlasInputDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump diagnostics-only CPU rigid BLAS build-input descriptors and validation" );

idCVar r_pathTracingRigidBlasGpuScaffold(
    "r_pathTracingRigidBlasGpuScaffold",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Source3 rigid BLAS GPU scaffold gate; set 0 to disable reusable rigid BLAS GPU resources" );

idCVar r_pathTracingRigidBlasGpuBuild(
    "r_pathTracingRigidBlasGpuBuild",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Source3 rigid BLAS build-submit gate; requires r_pathTracingRigidBlasGpuScaffold 1" );

idCVar r_pathTracingRigidBlasGpuBuildLimit(
    "r_pathTracingRigidBlasGpuBuildLimit",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum source3 rigid BLAS package builds per frame; 0 is unlimited" );

idCVar r_pathTracingRigidBlasGpuResultBudgetKB(
    "r_pathTracingRigidBlasGpuResultBudgetKB",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum admitted source3 rigid BLAS result bytes per frame in KiB; 0 is unlimited" );

idCVar r_pathTracingRigidBlasGpuForceRebuild(
    "r_pathTracingRigidBlasGpuForceRebuild",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Force source3 rigid BLAS GPU rebuild submission every frame for diagnostics" );

idCVar r_pathTracingRigidBlasGpuDump(
    "r_pathTracingRigidBlasGpuDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump one-shot rigid BLAS GPU scaffold buffer/build stats" );

idCVar r_pathTracingRigidTlasPlanDump(
    "r_pathTracingRigidTlasPlanDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump a diagnostics-only rigid TLAS instance plan from source3 visible rigid instances" );

idCVar r_pathTracingRigidTlasRoute(
    "r_pathTracingRigidTlasRoute",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Source3 rigid TLAS route gate; active in debug modes 23/24/25 and mode 18 unless its per-mode gate is disabled" );

idCVar r_pathTracingRigidRouteMode18(
    "r_pathTracingRigidRouteMode18",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Mode 18 routed rigid integration gate; set 0 to force legacy dynamic fallback behavior" );

idCVar r_pathTracingRigidRouteDump(
    "r_pathTracingRigidRouteDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump one-shot rigid route buffer build and upload stats" );

idCVar r_pathTracingRigidRouteOverlapDump(
    "r_pathTracingRigidRouteOverlapDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 in debug mode 24/39 to read back overlap/eligibility validation, including mode 24 material/class mismatch buckets, and dump routed dynamic-removal stats once" );

idCVar r_pathTracingRigidRouteRemoveDynamic(
    "r_pathTracingRigidRouteRemoveDynamic",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Remove routed-ready rigid candidates from the source3 dynamic fallback; set 0 for legacy overlap/emergency testing" );

idCVar r_pathTracingRigidRouteEmissiveCards(
    "r_pathTracingRigidRouteEmissiveCards",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Promote safe entity-attached translucent signage/glow cards into source3 rigid residency so off-camera emissive panels remain routable" );

idCVar r_pathTracingRigidRouteMaxInstances(
    "r_pathTracingRigidRouteMaxInstances",
    "510",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum source3 rigid instances routed into material IDs, route buffers, and TLAS descriptors; clamped to 510 to fit the 512-instance TLAS with static/dynamic base entries" );

idCVar r_pathTracingRigidResidency(
    "r_pathTracingRigidResidency",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Keep source3 rigid instances resident through the geometry residency system instead of only raster-visible drawSurfs" );

idCVar r_pathTracingRigidResidencyPortalSteps(
    "r_pathTracingRigidResidencyPortalSteps",
    "4",
    CVAR_RENDERER | CVAR_INTEGER,
    "Portal traversal depth for source3 rigid geometry residency; default depth 4 is the current source3 standard" );

idCVar r_pathTracingRigidResidencyDump(
    "r_pathTracingRigidResidencyDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump source3 rigid geometry portal residency stats once" );

idCVar r_pathTracingGeometryLifecycle(
    "r_pathTracingGeometryLifecycle",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Legacy PT geometry lifecycle diagnostic sampling gate; residency behavior is controlled by r_pathTracingGeometryResidencyV2" );

idCVar r_pathTracingGeometryLifecycleStage(
    "r_pathTracingGeometryLifecycleStage",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Legacy PT geometry lifecycle diagnostic stage tag; records/dumps only, no longer selects geometry residency behavior" );

idCVar r_pathTracingGeometryLifecycleDump(
    "r_pathTracingGeometryLifecycleDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump one-shot render-def lifecycle counters, generation keys, and classification samples" );

idCVar r_pathTracingGeometryShadowRegistry(
    "r_pathTracingGeometryShadowRegistry",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Canonical per-render-world instance/source registry required by production geometry routes; set to 0 for legacy-only fallback" );

idCVar r_pathTracingGeometryShadowRegistryDump(
    "r_pathTracingGeometryShadowRegistryDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump the GEO-05 per-world lifecycle shadow registry once" );

idCVar r_pathTracingGeometryOffsetBlasTiming(
    "r_pathTracingGeometryOffsetBlasTiming",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1..120 for one GEO-06/Q3 paired Vulkan GPU timing run comparing identical index data at zero and non-zero offsets; measurement only" );

idCVar r_pathTracingGeometrySourceDeltaBudgetMB(
    "r_pathTracingGeometrySourceDeltaBudgetMB",
    "8",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-06 primary-view immutable source delta budget in MiB per frame" );

idCVar r_pathTracingGeometryDynamicFallbackBudgetMB(
    "r_pathTracingGeometryDynamicFallbackBudgetMB",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-13 per-frame merged dynamic capture byte budget in MiB; 0 is unlimited" );

idCVar r_pathTracingGeometryDynamicFallbackSurfaceBudget(
    "r_pathTracingGeometryDynamicFallbackSurfaceBudget",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-13 per-frame merged dynamic capture surface budget; 0 is unlimited" );

idCVar r_pathTracingGeometryStaticResidentBudgetMB(
    "r_pathTracingGeometryStaticResidentBudgetMB",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-13 persistent monolithic/static-source capture byte budget in MiB; 0 is unlimited" );

idCVar r_pathTracingGeometryStaticResidentSurfaceBudget(
    "r_pathTracingGeometryStaticResidentSurfaceBudget",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-13 persistent monolithic/static-source capture surface budget; 0 is unlimited" );

idCVar r_pathTracingGeometryAdmissionDump(
    "r_pathTracingGeometryAdmissionDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 for one bounded GEO-13 dynamic admission budget/result summary" );

idCVar r_pathTracingGeometryAttributeSurveyDump(
    "r_pathTracingGeometryAttributeSurveyDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to a 1-based page for one bounded GEO-12 full-fidelity canonical-source attribute survey; measurement only" );

idCVar r_pathTracingGeometryRenderedAttributeSurveyDump(
    "r_pathTracingGeometryRenderedAttributeSurveyDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to a 1-based page for one bounded GEO-12 final rendered static/dynamic attribute survey; measurement only" );

idCVar r_pathTracingGeometrySkinnedAttributeDeriveDump(
    "r_pathTracingGeometrySkinnedAttributeDeriveDump",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "One-shot GEO-12/Q2 exact posed-frame survey of geometric-frame transported skinned normal/tangent reconstruction; measurement only" );

idCVar r_pathTracingGeometrySourceColorUnorm8(
    "r_pathTracingGeometrySourceColorUnorm8",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "GEO-12 lossless UNORM8 color storage for newly captured canonical CPU source records; set 0 and reload the map for full-float rollback" );

idCVar r_pathTracingGeometryCanonicalRigidBlas(
    "r_pathTracingGeometryCanonicalRigidBlas",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-13 default-on canonical pooled rigid BLAS provider; live selection requires the separate traversal gate" );

idCVar r_pathTracingGeometryCanonicalRigidBlasBuildsPerFrame(
    "r_pathTracingGeometryCanonicalRigidBlasBuildsPerFrame",
    "16",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum canonical pooled rigid BLAS builds submitted per frame" );

idCVar r_pathTracingGeometryCanonicalRigidBlasResultBudgetKB(
    "r_pathTracingGeometryCanonicalRigidBlasResultBudgetKB",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum canonical pooled rigid BLAS result bytes admitted per frame in KiB; 0 is unlimited" );

idCVar r_pathTracingGeometryCanonicalRigidTraversal(
    "r_pathTracingGeometryCanonicalRigidTraversal",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-13 default-on canonical rigid TLAS traversal; requires exact descriptor parity and fails closed to legacy" );

idCVar r_pathTracingGeometryCanonicalRigidHitDump(
    "r_pathTracingGeometryCanonicalRigidHitDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "One-shot GEO-06 center-pixel primary-hit tuple readback for legacy/canonical rigid traversal comparison" );

idCVar r_pathTracingGeometryAuthoritativeGpuSkinning(
    "r_pathTracingGeometryAuthoritativeGpuSkinning",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Authoritative PT GPU-skinning source gate; 0 keeps legacy CPU capture, 1 stages renderer jointCache bytes and retains the exact frame-owned upload pose for history, 2 is a compatibility alias for 1" );

idCVar r_pathTracingGeometrySkinnedTlasCompare(
    "r_pathTracingGeometrySkinnedTlasCompare",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Per-instance canonical skinned TLAS route: 0 merged-dynamic rollback, 1 synchronized in-place PerformUpdate production route, 2 logical updates as full BUILD diagnostic controls" );

idCVar r_pathTracingGeometrySkinnedTlasCompareDump(
    "r_pathTracingGeometrySkinnedTlasCompareDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump the GEO-08 per-instance skinned TLAS comparison plan once" );

idCVar r_pathTracingGeometrySkinnedCaptureSplit(
    "r_pathTracingGeometrySkinnedCaptureSplit",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "CPU-capture cutover for the per-instance skinned route; only prior-frame exact routes omit CPU-skinned merged-dynamic geometry, and 0 restores the merged-dynamic rollback" );

idCVar r_pathTracingGeometrySkinnedCaptureRouteSetLimit(
    "r_pathTracingGeometrySkinnedCaptureRouteSetLimit",
    "8",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-08 bounded exact-visible-set cache size; values 1..8 support lifecycle eviction validation" );

idCVar r_pathTracingGeometrySkinnedTiming(
    "r_pathTracingGeometrySkinnedTiming",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Capture 1..240 GEO-08 frames of delayed GPU timer-query results for per-instance skinned BLAS work and the merged dynamic BLAS, with microsecond CPU skin accounting" );

idCVar r_pathTracingGeometrySkinnedConsumerAudit(
    "r_pathTracingGeometrySkinnedConsumerAudit",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "One-shot GEO-09 audit of both the live accepted skinned route and same-frame legacy-backed shadow across upload/TLAS, material/class, motion-range, and primitive/emissive identity contracts" );

idCVar r_pathTracingGeometrySkinnedHitAudit(
    "r_pathTracingGeometrySkinnedHitAudit",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-09 paired same-ray comparison of legacy merged-dynamic and canonical per-instance skinned hit/motion tuples; value 1 arms on the next eligible frame, N > 1 counts down eligible frames before arming; requires debug mode 58 and the comparison route" );

idCVar r_pathTracingGeometrySkinnedEmissiveAudit(
    "r_pathTracingGeometrySkinnedEmissiveAudit",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "One-shot GEO-09 canonical skinned-emissive audit; value 1 also injects at most 24 validation-only light records for unsafe authored emissive materials and verifies the same-frame GPU publication readback, N > 1 counts down eligible frames before arming" );

idCVar r_pathTracingGeometryStaticBucketAudit(
    "r_pathTracingGeometryStaticBucketAudit",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "One-shot GEO-10 shadow audit of deterministic portal-area static bucket assignment; does not change BLAS or TLAS routing" );

idCVar r_pathTracingGeometryStaticBucketMaxVertices(
    "r_pathTracingGeometryStaticBucketMaxVertices",
    "65536",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-10 shadow planner maximum vertices per static portal-area bucket; oversized surfaces remain explicitly assigned" );

idCVar r_pathTracingGeometryStaticBucketMaxIndexes(
    "r_pathTracingGeometryStaticBucketMaxIndexes",
    "196608",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-10 shadow planner maximum indexes per static portal-area bucket; splits rather than drops" );

idCVar r_pathTracingGeometryStaticBucketMaxTriangles(
    "r_pathTracingGeometryStaticBucketMaxTriangles",
    "65536",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-10 shadow planner maximum triangles per static portal-area bucket; splits rather than drops" );

idCVar r_pathTracingGeometryStaticBucketBlas(
    "r_pathTracingGeometryStaticBucketBlas",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-10 full-map resident bucket storage and per-bucket BLAS resource gate; default on for the accepted production route, set 0 for monolithic rollback" );

idCVar r_pathTracingGeometryStaticBucketBlasBuild(
    "r_pathTracingGeometryStaticBucketBlasBuild",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Submit GEO-10 per-bucket BLAS builds when the bucket resource gate is enabled; default on with bounded per-frame admission, set 0 to retain storage without new builds" );

idCVar r_pathTracingGeometryStaticBucketBlasBuildLimit(
    "r_pathTracingGeometryStaticBucketBlasBuildLimit",
    "2",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum GEO-10 shadow-only static bucket BLAS builds submitted per frame; 0 means no limit" );

idCVar r_pathTracingGeometryStaticBucketBlasResultBudgetKB(
    "r_pathTracingGeometryStaticBucketBlasResultBudgetKB",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum static bucket BLAS result bytes admitted per frame in KiB; 0 is unlimited" );

idCVar r_pathTracingGeometryStaticBucketBlasForceRebuild(
    "r_pathTracingGeometryStaticBucketBlasForceRebuild",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Force already-built GEO-10 shadow bucket BLAS records to rebuild for validation" );

idCVar r_pathTracingGeometryStaticBucketPortalSteps(
    "r_pathTracingGeometryStaticBucketPortalSteps",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Portal-neighbor expansion around the immutable frontend-visible area snapshot used by the GEO-10 static bucket active mask" );

idCVar r_pathTracingGeometryStaticBucketReflectionPortalSteps(
    "r_pathTracingGeometryStaticBucketReflectionPortalSteps",
    "4",
    CVAR_RENDERER | CVAR_INTEGER,
    "Secondary optical GEO-10 portal-neighbor expansion; effective steps are max(primary, secondary) while bounded glass reflection, opaque-mirror reflection, or refracted PSR is active" );

idCVar r_pathTracingGeometryStaticBucketRoute(
    "r_pathTracingGeometryStaticBucketRoute",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "GEO-10 route mode: 0 monolithic rollback, 1 default production request (fail-closed until resources and consumers agree), 2 isolated primary probe in view 2 status, view 17 motion, view 19 post-composite albedo, or view 24 material classification" );

idCVar r_pathTracingGeometryStaticBucketSecondaryProbeStage(
    "r_pathTracingGeometryStaticBucketSecondaryProbeStage",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Default-off GEO-10 view-16 probe: stages 1-9 isolate setup/DI/bindings; 10 raygen without trace; 11 traversal without hit shaders; 12 rejected full any-hit baseline; 13 any-hit entry only; 14 one complete any-hit invocation; 15 rejected bounded repeated any-hit; 16 one decode-free IgnoreHit then accept; 17 bucket-resident bounded iterative resolve; 18 route-0 monolithic bounded iterative control; 19 initial only; 20 temporal diagnostic; 21 initial plus production presentation with temporal and spatial reuse off; 22 bucket hardware-hit versus packed-replay tuple diagnostic; 23 closest-hit versus raygen/replay position split; 24 admits material-feature composition then blocks later consumers" );

idCVar r_pathTracingGeometryResidencyV2(
    "r_pathTracingGeometryResidencyV2",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Master gate for the Remix-style frame-age + frustum residency model. Set 0 for legacy behavior." );

idCVar r_pathTracingEntityFeed(
    "r_pathTracingEntityFeed",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Master gate for the entityDef-feed BVH producer. 0 = legacy fallback behavior." );

idCVar r_pathTracingEntityFeedDump(
    "r_pathTracingEntityFeedDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Print entityDef-feed diagnostics when != 0." );

idCVar r_pathTracingEntityFeedMaxDepth(
    "r_pathTracingEntityFeedMaxDepth",
    "4",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum portal flood depth for entityDef-feed diagnostics and producers; default 4 matches the routed residency portal budget." );

idCVar r_pathTracingEntityFeedMaxDistance(
    "r_pathTracingEntityFeedMaxDistance",
    "4096",
    CVAR_RENDERER | CVAR_FLOAT,
    "Maximum area-center distance for entityDef-feed diagnostics and producers." );

idCVar r_pathTracingResidency(
    "r_pathTracingResidency",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Master gate for path-trace render-thread residency caches. 0 = legacy per-frame derive. Subsystem residency paths require this and their subsystem gate." );

idCVar r_pathTracingResidencyEntityFeed(
    "r_pathTracingResidencyEntityFeed",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Gate for entity-feed residency caches. Active only when r_pathTracingResidency is also non-zero." );

idCVar r_pathTracingResidencyStatic(
    "r_pathTracingResidencyStatic",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Gate for static-area residency caches. Active only when r_pathTracingResidency is also non-zero." );

idCVar r_pathTracingResidencyMaterial(
    "r_pathTracingResidencyMaterial",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Gate for material residency caches. Active only when r_pathTracingResidency is also non-zero." );

idCVar r_pathTracingResidencyLights(
    "r_pathTracingResidencyLights",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Gate for light residency caches. Active only when r_pathTracingResidency is also non-zero." );

idCVar r_pathTracingResidencyTtlFrames(
    "r_pathTracingResidencyTtlFrames",
    "120",
    CVAR_RENDERER | CVAR_INTEGER,
    "Frames a path-trace resident record survives unseen before residency GC." );

idCVar r_pathTracingResidencyDump(
    "r_pathTracingResidencyDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Print path-trace residency cache instrumentation when non-zero." );

idCVar r_pathTracingResidencyFramesToKeep(
    "r_pathTracingResidencyFramesToKeep",
    "100000",
    CVAR_RENDERER | CVAR_INTEGER,
    "Frames a rigid resident instance survives after it was last seen before frame-age GC removes it; default is long-lived Remix-style residency" );

idCVar r_pathTracingResidencyMeshFramesToKeep(
    "r_pathTracingResidencyMeshFramesToKeep",
    "900",
    CVAR_RENDERER | CVAR_INTEGER,
    "Frames an unreferenced rigid mesh/BLAS cache record survives after it was last seen; resident instances keep their mesh regardless of this window" );

idCVar r_pathTracingResidencyRouteCached(
    "r_pathTracingResidencyRouteCached",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Debug disable for retained off-screen rigid route emission; default 1 emits retained V2 residents when cached GPU route data is ready." );

idCVar r_pathTracingResidencyRouteCachedTlas(
    "r_pathTracingResidencyRouteCachedTlas",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic gate for cached route emission: 1 allows cached off-screen rigid records into TLAS, 0 keeps cached route buffers but prevents cached BLAS hits." );

idCVar r_pathTracingResidencyRouteCachedTraceMask(
    "r_pathTracingResidencyRouteCachedTraceMask",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic gate for cached rigid TLAS descriptors: 1 uses the normal trace mask, 0 emits cached descriptors with mask 0 so TLAS build still references them but rays cannot hit them." );

idCVar r_pathTracingResidencyMovingGraceFrames(
    "r_pathTracingResidencyMovingGraceFrames",
    "6",
    CVAR_RENDERER | CVAR_INTEGER,
    "Frames after an entity transform update during which rigid residency treats the instance as moving and refuses off-screen freeze retention" );

idCVar r_pathTracingResidencyDebug(
    "r_pathTracingResidencyDebug",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Dump residency V2 counts (live / retained-offscreen / aged-out)" );

idCVar r_pathTracingStaticAreaPreload(
    "r_pathTracingStaticAreaPreload",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Preload source3 static-world geometry for the current Doom area plus portal-adjacent areas instead of waiting for raster drawSurfs" );

idCVar r_pathTracingStaticAreaPreloadPortalSteps(
    "r_pathTracingStaticAreaPreloadPortalSteps",
    "4",
    CVAR_RENDERER | CVAR_INTEGER,
    "Portal traversal depth for source3 static-world area preload; default depth 4 is the current source3 standard" );

idCVar r_pathTracingStaticGeometryPruneMissing(
    "r_pathTracingStaticGeometryPruneMissing",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Source3 static geometry cache policy: 0 keeps discovered static surfaces resident; 1 prunes static drawSurf records not seen this frame for diagnostic active-BLAS high-water testing" );

idCVar r_pathTracingStaticBlasForceRebuild(
    "r_pathTracingStaticBlasForceRebuild",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic A/B: 0=cached static BLAS, 1=normal static cache-miss upload policy plus BLAS rebuild, 2=BLAS rebuild from existing GPU buffers without uploading them" );

idCVar r_pathTracingStaticBlasGenerationGuard(
    "r_pathTracingStaticBlasGenerationGuard",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Rejected GEO-09 diagnostic: require cached static BLAS generation to match persistent static generation; default off because generation-matched geometry still disappeared" );

idCVar r_pathTracingStaticVertexFullUploadOnCacheMiss(
    "r_pathTracingStaticVertexFullUploadOnCacheMiss",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Fully initialize static vertex buffers on cache misses when runtime texture matrices would otherwise select only a partial upload range; accepted GEO-12 fix, set to 0 for legacy rollback" );

idCVar r_pathTracingSceneBoundsOverlay(
    "r_pathTracingSceneBoundsOverlay",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "PT bounds overlay: 0 = off, 1 = eligible rigid drawSurf candidates plus resident rigid, 2 = all mirrored categories plus resident rigid, 3 = resident rigid only, 4 = current static cache first, 5 = static cache plus resident rigid, 6 = cache-only static first" );

idCVar r_pathTracingSceneBoundsOverlayMax(
    "r_pathTracingSceneBoundsOverlayMax",
    "128",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum PT drawSurf mirror bounds boxes drawn per frame" );

idCVar r_pathTracingSceneBoundsOverlayGpu(
    "r_pathTracingSceneBoundsOverlayGpu",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Experimental PT shader composited bounds overlay gate; default off after device-removal risk in per-pixel overlay loop" );

idCVar r_pathTracingPortalBruteforceFullMap(
    "r_pathTracingPortalBruteforceFullMap",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic override: 1 treats every render-world area as selected for PT scene, rigid-residency, emissive-light, and analytic-light portal selectors; very slow" );

idCVar r_pathTracingWorldStaticEmissives(
    "r_pathTracingWorldStaticEmissives",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Experimental: supplement mode 19/20 emissive candidates from full-level static world models; off by default due current GPU/device-removal risk" );

idCVar r_pathTracingWorldStaticEmissiveMaxTriangles(
    "r_pathTracingWorldStaticEmissiveMaxTriangles",
    "128",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum full-level static world emissive triangles to append when r_pathTracingWorldStaticEmissives is enabled" );

idCVar r_pathTracingDynamicOccluderRadius(
    "r_pathTracingDynamicOccluderRadius",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Radius around the camera that keeps rigid entity model surfaces in the RT dynamic BLAS even when raster visibility culls them; 0 disables" );

idCVar r_pathTracingDynamicOccluderMaxSurfaces(
    "r_pathTracingDynamicOccluderMaxSurfaces",
    "64",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum nearby rigid entity surfaces appended as dynamic RT occluder safety geometry per frame" );

idCVar r_pathTracingLightAreaPortalSteps(
    "r_pathTracingLightAreaPortalSteps",
    "4",
    CVAR_RENDERER | CVAR_INTEGER,
    "Portal traversal depth for RT smoke emissive light-area selection diagnostics; default depth 4 matches current source3 residency baseline" );

idCVar r_pathTracingRemixLightUniverseEnable(
    "r_pathTracingRemixLightUniverseEnable",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Default-off Remix-shaped light-universe CPU shell: owns dense current/previous light domains and maps without active shader consume" );

idCVar r_pathTracingRemixLightUniverseUseForCleanRtxdiDi(
    "r_pathTracingRemixLightUniverseUseForCleanRtxdiDi",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Allow clean RTXDI DI real-light routes to request and consume the Remix-shaped dense Doom analytic light domain" );

idCVar r_pathTracingRemixLightUniverseDomain(
    "r_pathTracingRemixLightUniverseDomain",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "RLU dense-domain source: 0 Doom analytic only, 1 emissive only, 2 analytic plus emissive unified" );

idCVar r_pathTracingRemixLightUniverseDoomColorSource(
    "r_pathTracingRemixLightUniverseDoomColorSource",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Reserved RLU Doom analytic payload diagnostic color source: 0 material register, 1 current renderLight color, 2 authored/base color" );

idCVar r_pathTracingRemixLightUniverseStrictRemixMapping(
    "r_pathTracingRemixLightUniverseStrictRemixMapping",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "RLU mapping policy: 1 bases validity only on current/previous dense-domain identity; 0 allows diagnostic compatibility mode" );

idCVar r_pathTracingRemixLightManagerRAB(
    "r_pathTracingRemixLightManagerRAB",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Active RRX RAB light source: Remix Light Universe only; legacy ReSTIR manager fallback has been purged" );

idCVar r_pathTracingReGIREnable(
    "r_pathTracingReGIREnable",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Enable the standalone clean-room ReGIR resource shell; does not route PDFNEE, temporal, spatial, RRX, or best-light" );

idCVar r_pathTracingReGIRDebugView(
    "r_pathTracingReGIRDebugView",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Standalone ReGIR debug view selector: 0 disabled, 1 status, 2 cells, 3 validity, 4 occupancy, 5 analytic identity, 6 emissive identity, 7 sourcePdf, 8 empty reason, 9 deterministic slot, 10 cell mean" );

idCVar r_pathTracingReGIRMode(
    "r_pathTracingReGIRMode",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Standalone ReGIR layout mode: 0 disabled, 1 grid, 2 onion" );

idCVar r_pathTracingReGIRCellSize(
    "r_pathTracingReGIRCellSize",
    "256",
    CVAR_RENDERER | CVAR_FLOAT,
    "Standalone ReGIR smallest cell size in Doom world units" );

idCVar r_pathTracingReGIRGridX(
    "r_pathTracingReGIRGridX",
    "32",
    CVAR_RENDERER | CVAR_INTEGER,
    "Standalone ReGIR grid cells along world X for the initial debug shell" );

idCVar r_pathTracingReGIRGridY(
    "r_pathTracingReGIRGridY",
    "32",
    CVAR_RENDERER | CVAR_INTEGER,
    "Standalone ReGIR grid cells along world Y for the initial debug shell" );

idCVar r_pathTracingReGIRGridZ(
    "r_pathTracingReGIRGridZ",
    "16",
    CVAR_RENDERER | CVAR_INTEGER,
    "Standalone ReGIR grid cells along world Z for the initial debug shell" );

idCVar r_pathTracingReGIRLightsPerCell(
    "r_pathTracingReGIRLightsPerCell",
    "128",
    CVAR_RENDERER | CVAR_INTEGER,
    "Standalone ReGIR candidate slots stored per cell" );

idCVar r_pathTracingReGIRBuildSamples(
    "r_pathTracingReGIRBuildSamples",
    "16",
    CVAR_RENDERER | CVAR_INTEGER,
    "Standalone ReGIR light proposals streamed per cell candidate slot; clamped to 1..64" );

idCVar r_pathTracingReGIRLightDomain(
    "r_pathTracingReGIRLightDomain",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Standalone ReGIR light domain: 0 analytic only, 1 emissive only, 2 analytic plus emissive split domains" );

idCVar r_pathTracingReGIRCenterMode(
    "r_pathTracingReGIRCenterMode",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Standalone ReGIR center mode: 0 camera-centered, 1 map/static bounds centered, 2 manager-provided manual center" );

idCVar r_pathTracingReGIRManualCenterX(
    "r_pathTracingReGIRManualCenterX",
    "0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Standalone ReGIR manual center X used when r_pathTracingReGIRCenterMode is 2" );

idCVar r_pathTracingReGIRManualCenterY(
    "r_pathTracingReGIRManualCenterY",
    "0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Standalone ReGIR manual center Y used when r_pathTracingReGIRCenterMode is 2" );

idCVar r_pathTracingReGIRManualCenterZ(
    "r_pathTracingReGIRManualCenterZ",
    "0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Standalone ReGIR manual center Z used when r_pathTracingReGIRCenterMode is 2" );

idCVar r_pathTracingNeeCacheEnable(
    "r_pathTracingNeeCacheEnable",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Enable the Remix-style NEE cache proposal-provider resource shell; does not route PDFNEE, temporal, spatial, RRX, or best-light" );

idCVar r_pathTracingNeeCacheMode(
    "r_pathTracingNeeCacheMode",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "NEE cache provider mode: 0 disabled, 1 Remix-style log/hash learned cache, 2 ReGIR/onion fallback provider, 3 bounded grid diagnostic" );

idCVar r_pathTracingNeeCacheDebugView(
    "r_pathTracingNeeCacheDebugView",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "NEE cache debug view selector: 0 disabled, 1 status, 2 cell id, 3 occupancy, 4 task accumulation, 5 emissive candidates, 6 analytic candidates, 7 sourcePdf, 8 mixture source, 9 fallback reason, 10 RAB replay, 11 flat consumed candidates, 12 flat full current RLU" );

idCVar r_pathTracingNeeCacheCellResolution(
    "r_pathTracingNeeCacheCellResolution",
    "8",
    CVAR_RENDERER | CVAR_INTEGER,
    "NEE cache log/hash cell resolution parameter; higher values mean smaller cells; default 8 gives 32-world-unit base cells at the default minRange 256; use 16 for sharper diagnostics" );

idCVar r_pathTracingNeeCacheMinRange(
    "r_pathTracingNeeCacheMinRange",
    "256",
    CVAR_RENDERER | CVAR_FLOAT,
    "NEE cache minimum range for lowest-level cells in Doom world units" );

idCVar r_pathTracingNeeCacheCellCount(
    "r_pathTracingNeeCacheCellCount",
    "65536",
    CVAR_RENDERER | CVAR_INTEGER,
    "NEE cache fixed physical cell count for the log/hash provider" );

idCVar r_pathTracingNeeCacheCandidateSlots(
    "r_pathTracingNeeCacheCandidateSlots",
    "8",
    CVAR_RENDERER | CVAR_INTEGER,
    "NEE cache candidate slots per cell for the fixed-budget provider shell" );

idCVar r_pathTracingNeeCacheTaskSlots(
    "r_pathTracingNeeCacheTaskSlots",
    "8",
    CVAR_RENDERER | CVAR_INTEGER,
    "NEE cache task slots per cell for learned source feedback" );

idCVar r_pathTracingNeeCacheFallbackProbability(
    "r_pathTracingNeeCacheFallbackProbability",
    "0.25",
    CVAR_RENDERER | CVAR_FLOAT,
    "NEE cache probability of sampling full-RLU fallback instead of the cache; 0 is diagnostic/cache-only" );

idCVar r_pathTracingNeeCacheSourceDomain(
    "r_pathTracingNeeCacheSourceDomain",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "NEE cache source domain: 0 all current RLU, 1 emissive range only, 2 analytic range only, 3 typed candidate lists with mixture probabilities" );

idCVar r_pathTracingAnalyticLightCandidates(
    "r_pathTracingAnalyticLightCandidates",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Build, upload, and shade analytic sphere-light candidates from active Doom lights" );

idCVar r_pathTracingAnalyticLightMaxGpu(
    "r_pathTracingAnalyticLightMaxGpu",
    "256",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum Doom analytic sphere-light candidates uploaded and sampled by the PT shader" );

idCVar r_pathTracingAnalyticLightIntensityScale(
    "r_pathTracingAnalyticLightIntensityScale",
    "4.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Radiance calibration scale for Doom analytic sphere lights in PT lighting" );

idCVar r_pathTracingAnalyticLightReplaceSelected(
    "r_pathTracingAnalyticLightReplaceSelected",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "When analytic Doom lights are enabled, suppress the legacy selected point-light array so analytic contribution can be inspected alone" );

idCVar r_pathTracingAnalyticSphereLightRadiusScale(
    "r_pathTracingAnalyticSphereLightRadiusScale",
    "0.08",
    CVAR_RENDERER | CVAR_FLOAT,
    "Analytic sphere-light radius as a fraction of Doom point-light radius" );

idCVar r_pathTracingAnalyticSphereLightRadiusMin(
    "r_pathTracingAnalyticSphereLightRadiusMin",
    "4.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Minimum analytic sphere-light radius" );

idCVar r_pathTracingAnalyticSphereLightRadiusMax(
    "r_pathTracingAnalyticSphereLightRadiusMax",
    "64.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Maximum analytic sphere-light radius" );

idCVar r_pathTracingAnalyticLightDoomRadiusCutoff(
    "r_pathTracingAnalyticLightDoomRadiusCutoff",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Debug-only: apply Doom authored radius as a hard analytic-light shading cutoff in the RAB sphere sampler" );

idCVar r_pathTracingLightCount(
    "r_pathTracingLightCount",
    "4",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum selected visible point lights for RT smoke debug modes 14/15; 0 = fixed directional fallback, max 32" );

idCVar r_pathTracingLightSelection(
    "r_pathTracingLightSelection",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "RT smoke point-light selection: 0 = nearest camera lights, 1 = strongest estimated camera influence" );

idCVar r_pathTracingLightSpriteProxies(
    "r_pathTracingLightSpriteProxies",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Draw debug-only emissive sprite proxies for selected RT smoke lights in mode 14" );

idCVar r_pathTracingLightSpriteRadiusScale(
    "r_pathTracingLightSpriteRadiusScale",
    "0.04",
    CVAR_RENDERER | CVAR_FLOAT,
    "World-space radius scale for RT smoke selected-light sprite proxies" );

idCVar r_pathTracingLightSpriteIntensity(
    "r_pathTracingLightSpriteIntensity",
    "2.5",
    CVAR_RENDERER | CVAR_FLOAT,
    "Intensity multiplier for RT smoke selected-light sprite proxies" );

idCVar r_pathTracingTextureTableLimit(
    "r_pathTracingTextureTableLimit",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum safe captured material textures to bind for RT smoke texture debug modes; 0 = discovery/logging only, max 2048" );

idCVar r_pathTracingTextureTableStart(
    "r_pathTracingTextureTableStart",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "First safe captured diffuse texture candidate to bind for RT smoke debug mode 8" );

idCVar r_pathTracingTextureForceFallback(
    "r_pathTracingTextureForceFallback",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "RT smoke debug mode 8: bind fallback white texture into active bindless slots instead of captured diffuse textures" );

idCVar r_pathTracingTextureSampleEnable(
    "r_pathTracingTextureSampleEnable",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "RT smoke debug mode 8: enable actual bindless texture sampling; 0 keeps the descriptor table active but shades from material fallback colors" );

idCVar r_pathTracingTextureBindlessEnable(
    "r_pathTracingTextureBindlessEnable",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "RT smoke debug mode 8: sample from the bindless texture table; 0 samples a regular fallback texture SRV for diagnostics" );

idCVar r_pathTracingSkyCubeProbe(
    "r_pathTracingSkyCubeProbe",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "One-shot isolated sky TextureCube probe: 0 off, 1 upload a renderer-owned cube and sample its six axes in a compute pass; never enables the live RT sky path" );

idCVar r_pathTracingSkyCubeEnvironment(
    "r_pathTracingSkyCubeEnvironment",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Use the renderer-owned TextureCube for primary terminal sky surfaces; 0 keeps the stable white fallback, 1 enables cube color sampling" );

idCVar r_pathTracingSkyCubeBrightness(
    "r_pathTracingSkyCubeBrightness",
    "20.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Linear brightness multiplier for renderer-owned sky-cube radiance" );

idCVar r_pathTracingTextureSampleMethod(
    "r_pathTracingTextureSampleMethod",
    "2",
    CVAR_RENDERER | CVAR_INTEGER,
    "RT smoke debug mode 8 texture fetch method: 0 = disabled/fallback color, 1 = SampleLevel diagnostic, 2 = Texture.Load stable default" );

idCVar r_pathTracingTextureFilter(
    "r_pathTracingTextureFilter",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "RT smoke Texture.Load filtering: 0 = point, 1 = manual bilinear" );

idCVar r_pathTracingTextureDecode(
    "r_pathTracingTextureDecode",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Decode RT smoke material texture encodings such as Doom diffuse YCoCg" );

idCVar r_pathTracingForceTextureCodeUse(
    "r_pathTracingForceTextureCodeUse",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Use four-digit Doom texture filename codes as RT smoke material image discovery hints" );

idCVar r_pathTracingMatClassEnable(
    "r_pathTracingMatClassEnable",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Enable the new material-classifier path; set 0 to roll back to the legacy classifier/table path" );

idCVar r_pathTracingMatClassUseRmao(
    "r_pathTracingMatClassUseRmao",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Material classifier Route A: use TD_SPECULAR_PBR_RMAO/RMAOD specular images as real RMAO PBR inputs" );

idCVar r_pathTracingMatClassDriveLegacySpec(
    "r_pathTracingMatClassDriveLegacySpec",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Material classifier Route B opt-in: promote classified Ricochet legacy spec materials to metallic F0 while preserving specmap roughness" );

idCVar r_pathTracingMatClassNormalDecodeMode(
    "r_pathTracingMatClassNormalDecodeMode",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Material classifier normal decode: 0=image-format swizzle, 1=force RGB8 rg, 2=force compressed wy" );

idCVar r_pathTracingMatClassDebugList(
    "r_pathTracingMatClassDebugList",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Dump new material-classifier records; 1=registered materials, 2=also one-shot all-decl surfaceType distribution, 3=also per-stage .mtr evidence" );

idCVar r_pathTracingCrosshairMaterialDump(
    "r_pathTracingCrosshairMaterialDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump the PT material and evaluated stage state under the crosshair on the next path-traced frame; resets to 0" );

idCVar r_pathTracingMatClassDebugMax(
    "r_pathTracingMatClassDebugMax",
    "64",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum material-classifier record lines printed per frame when r_pathTracingMatClassDebugList is enabled" );

idCVar r_pathTracingUseNormalMaps(
    "r_pathTracingUseNormalMaps",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Use sampled normal maps for RT smoke debug mode 14 direct lighting" );

idCVar r_pathTracingNormalMapFlipGreen(
    "r_pathTracingNormalMapFlipGreen",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Flip sampled RT smoke normal-map green/Y before tangent-space reconstruction; diagnostic for Doom 3 legacy normal-map convention" );

idCVar r_pathTracingUseSpecularMaps(
    "r_pathTracingUseSpecularMaps",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Use sampled specular maps for RT smoke debug mode 14 direct lighting" );

idCVar r_pathTracingUseEmissiveMaps(
    "r_pathTracingUseEmissiveMaps",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Use sampled emissive/glow material stages for RT smoke debug mode 14 direct lighting" );

idCVar r_pathTracingEmissiveFallbackWithoutTexture(
    "r_pathTracingEmissiveFallbackWithoutTexture",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Keep material-universe emissive color active when an emissive texture cannot be assigned a bindless descriptor slot" );

idCVar r_pathTracingToyMaxRayDistance(
    "r_pathTracingToyMaxRayDistance",
    "1024",
    CVAR_RENDERER | CVAR_FLOAT,
    "Maximum mode 18 toy path-tracing secondary/direct ray distance; reduces leaks through incomplete visible-surface TLAS geometry" );

idCVar r_pathTracingToyLightScale(
    "r_pathTracingToyLightScale",
    "0.3",
    CVAR_RENDERER | CVAR_FLOAT,
    "Scale selected point-light contribution in mode 18 toy path tracing and ReSTIR PT analytic-light preview intensity" );

idCVar r_pathTracingToyEmissiveScale(
    "r_pathTracingToyEmissiveScale",
    "4.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Scale emissive material contribution in mode 18 toy path tracing and ReSTIR PT emissive preview intensity" );

idCVar r_pathTracingToyLightTraceCap(
    "r_pathTracingToyLightTraceCap",
    "8",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum selected point lights traced per mode 18 direct-light evaluation; clamps expensive shadow rays after light selection, max 32" );

idCVar r_pathTracingToyFakePBRSpecular(
    "r_pathTracingToyFakePBRSpecular",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Use rbdoom3 legacy specmap-to-PBR roughness/F0 shading for mode 18 and related path-tracing visualizers; set 0 to opt out" );

idCVar r_pathTracingOpenPbrBrdfMode(
    "r_pathTracingOpenPbrBrdfMode",
    "4",
    CVAR_RENDERER | CVAR_INTEGER,
    "Opaque direct BRDF mode for OpenPBR test consumers: 0 Lambert, 1 EON diffuse, 2 EON+GGX eval, 3 GGX VNDF sampling, 4 mode 3 plus scalar rough-metal compensation" );

idCVar r_pathTracingToyAccumulation(
    "r_pathTracingToyAccumulation",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Accumulate mode 18 toy path tracing across stable camera frames" );

idCVar r_pathTracingToyAccumMaxFrames(
    "r_pathTracingToyAccumMaxFrames",
    "64",
    CVAR_RENDERER | CVAR_INTEGER,
    "Maximum accumulated frames for mode 18 toy path tracing" );

idCVar r_pathTracingSamplesPerPixel(
    "r_pathTracingSamplesPerPixel",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Mode 18 path tracer samples per pixel; default 1, clamp 1..4 for the current monolithic dispatch" );

idCVar r_pathTracingMaxPathDepth(
    "r_pathTracingMaxPathDepth",
    "2",
    CVAR_RENDERER | CVAR_INTEGER,
    "Mode 18 maximum path depth including the primary hit; default 2 preserves the current one-bounce toy path" );

idCVar r_pathTracingDiffuseBounceLimit(
    "r_pathTracingDiffuseBounceLimit",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Mode 18 diffuse secondary-bounce limit; default 1 preserves the current toy bounce" );

idCVar r_pathTracingSpecularBounceLimit(
    "r_pathTracingSpecularBounceLimit",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Mode 18 specular/reflection secondary-bounce limit; default 0 keeps reflections disabled" );

idCVar r_pathTracingTransmissionBounceLimit(
    "r_pathTracingTransmissionBounceLimit",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Mode 18 glass/transmission secondary-bounce limit; default 0 keeps glass transmission fail-closed, clamp 0..1" );

idCVar r_pathTracingReflectionMode(
    "r_pathTracingReflectionMode",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Mode 18 reflection mode: 0 off, 1 mirror-ish low-roughness validation bounce, 2 rougher validation bounce" );

idCVar r_pathTracingRussianRouletteDepth(
    "r_pathTracingRussianRouletteDepth",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Path depth where Russian roulette may start; 0 disables it in the current conservative shader path" );

idCVar r_pathTracingNextEventEstimation(
    "r_pathTracingNextEventEstimation",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Enable direct-light next-event-estimation style work in the mode 18 path tracer core" );

idCVar r_pathTracingSecondaryNeeMode(
    "r_pathTracingSecondaryNeeMode",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Mode 18 secondary selected-light NEE mode: 0 off, 1 one sampled selected light, 2 legacy full selected-light loop" );

idCVar r_pathTracingSecondaryNeeVisibility(
    "r_pathTracingSecondaryNeeVisibility",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic gate for sampled secondary selected-light NEE visibility rays: 0 shades selected samples as visible, 1 traces shadow visibility" );

idCVar r_pathTracingSecondaryAnalyticNeeMode(
    "r_pathTracingSecondaryAnalyticNeeMode",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Mode 18 secondary/reflection Doom analytic-light NEE: 0 off, 1 sampled/bounded proposals, 2 legacy full analytic-light loop" );

idCVar r_pathTracingSecondaryAnalyticNeeSamples(
    "r_pathTracingSecondaryAnalyticNeeSamples",
    "4",
    CVAR_RENDERER | CVAR_INTEGER,
    "Number of uniformly sampled Doom analytic-light proposals evaluated per secondary/reflection NEE vertex when r_pathTracingSecondaryAnalyticNeeMode is 1" );

idCVar r_pathTracingNeeCacheSecondaryEnable(
    "r_pathTracingNeeCacheSecondaryEnable",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Enable opt-in ReSTIR PT path-tracer NEE cache consumption through current RLU/RAB replay" );

idCVar r_pathTracingNeeCacheSecondaryVisualRefresh(
    "r_pathTracingNeeCacheSecondaryVisualRefresh",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "NEECACHE-10 view 8 band 10 diagnostic refresh: 0 read existing cache, 1 one-shot refresh, 2 refresh every frame" );

idCVar r_pathTracingReservoirTwoSidedEmissives(
    "r_pathTracingReservoirTwoSidedEmissives",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Treat clean DI and PDFNEE lightmode 7 emissive triangle samples as two-sided for Doom panel winding compatibility" );

idCVar r_pathTracingReservoirCandidateTrials(
    "r_pathTracingReservoirCandidateTrials",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Emissive reservoir candidate trials per pixel for clean DI, PDFNEE, and UPT parity sampling; higher values improve small-light reachability at extra shader cost" );

idCVar r_pathTracingEmissiveUniformMixture(
    "r_pathTracingEmissiveUniformMixture",
    "0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Uniform full-support fraction mixed into the shared emissive power CDF; 0 is power-only, 1 is uniform triangle selection" );

idCVar r_pathTracingRestirPTTemporalAnalyticNeeReuse(
    "r_pathTracingRestirPTTemporalAnalyticNeeReuse",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Allow previous-frame NEE light reservoirs to be reused by ReSTIR PT temporal/spatial modes; set 0 to isolate current-frame NEE from previous light history" );

idCVar r_pathTracingRestirPTTemporalAnalyticLightChangeTolerance(
    "r_pathTracingRestirPTTemporalAnalyticLightChangeTolerance",
    "0.10",
    CVAR_RENDERER | CVAR_FLOAT,
    "Relative current/previous Doom analytic light color tolerance for ReSTIR PT temporal NEE remap; lower rejects animated light phases more aggressively" );

idCVar r_pathTracingRestirPTMaterialSimilarityMode(
    "r_pathTracingRestirPTMaterialSimilarityMode",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "ReSTIR PT temporal material similarity: 0 = legacy Doom material id/flags, 1 = RTXDI roughness/specular/diffuse, 2 = ignore diffuse, 3 = ignore specular, 4 = ignore roughness, 5 = accept all materials" );

idCVar r_pathTracingRestirPTUnifiedPrevToCurrentScan(
    "r_pathTracingRestirPTUnifiedPrevToCurrentScan",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Debug-only ReSTIR PT unified-light temporal remap proof: invert the current-to-previous unified remap in shader for previous-to-current translation" );

idCVar r_pathTracingRestirPTUnifiedLightLoad(
    "r_pathTracingRestirPTUnifiedLightLoad",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Legacy/debug RAB_LoadLightInfo comparison switch: 0 split buffers, 1 unified current/previous buffers; the active Remix-manager RRX route does not require this opt-in" );

idCVar r_pathTracingRestirPTUnifiedLightSample(
    "r_pathTracingRestirPTUnifiedLightSample",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Legacy/debug RAB_SamplePolymorphicLight comparison switch: 0 split light-type switch, 1 unified light-record type switch; the active Remix-manager RRX route uses the unified sample path internally" );

idCVar r_pathTracingRestirPTUnifiedNee(
    "r_pathTracingRestirPTUnifiedNee",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "ReSTIR PT NEE producer: 1 = RTXDI-shaped local-light DI initial producer for unified or manager RAB domains, 0 = explicit legacy rbdoom RIS fallback outside manager RAB" );

idCVar r_pathTracingRestirPTAnalyticLightTrials(
    "r_pathTracingRestirPTAnalyticLightTrials",
    "32",
    CVAR_RENDERER | CVAR_INTEGER,
    "Current-frame analytic Doom light proposal trials for ReSTIR PT NEE; higher values reduce sparse-light flicker at additional GPU cost" );

idCVar r_pathTracingRestirPTVisibilityPolicy(
    "r_pathTracingRestirPTVisibilityPolicy",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "ReSTIR PT NEE visibility policy: 0 = final/preview visibility only, 1 = selected NEE sample producer visibility, 2 = strict proposal-stream visibility" );

idCVar r_pathTracingRestirPdfNeeVerifierEnable(
    "r_pathTracingRestirPdfNeeVerifierEnable",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Replacement ReSTIR PDF + NEE RLU current producer: one-CVar current-frame direct lighting and clean current-reservoir output; diagnostics are optional" );

idCVar r_pathTracingRestirPdfNeeVerifierSamples(
    "r_pathTracingRestirPdfNeeVerifierSamples",
    "32",
    CVAR_RENDERER | CVAR_INTEGER,
    "Replacement RLU current producer candidate proposals per pixel; default 32 for the one-CVar RLU path, clamped 1..64; use 64 for complex-scene diagnostics" );

idCVar r_pathTracingRestirPdfNeeVerifierVisibility(
    "r_pathTracingRestirPdfNeeVerifierVisibility",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Replacement RLU current producer visibility: 0 forced visible diagnostic, 1 use RAB NEE visibility policy; r_pathTracingRestirPTVisibilityPolicy controls selected/strict shadow rays" );

idCVar r_pathTracingRestirPdfNeeVerifierSourcePolicy(
    "r_pathTracingRestirPdfNeeVerifierSourcePolicy",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Replacement RLU current producer source policy: 0 full dense RLU uniform baseline, 1 RLU-04 range-stratified typed ranges using rangeSampleCount/(rangeCount*totalProposalSamples)" );

idCVar r_pathTracingUnifiedPtEnable(
    "r_pathTracingUnifiedPtEnable",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean unified Slang ReSTIR PT route request; overrides legacy Clean-DI/GI execution; UPT-06 owns two private 64-byte reservoir pages while no-reuse D0/R0 still use page 0 only; UPT-05 presentation is separately admitted by r_pathTracingUnifiedPtResolve" );

idCVar r_pathTracingUnifiedPtLeanPrimary(
    "r_pathTracingUnifiedPtLeanPrimary",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT-only primary payload A/B: 0 uses the existing 336-byte shared P0 payload; 1 uses a Vulkan-only 172-byte payload with decal/liquid candidate bins omitted; receiver ABI is selected independently by r_pathTracingUnifiedPtCompactReceiver" );

idCVar r_pathTracingUnifiedPtCompactReceiver(
    "r_pathTracingUnifiedPtCompactReceiver",
    "2",
    CVAR_RENDERER | CVAR_INTEGER,
    "UPT-only P0-to-D0 receiver A/B for production shader proofs 1..6: 0 legacy 176-byte history record; 1 UPT-owned 48-byte current-frame receiver without reuse; 2 production 32-byte distance/direction receiver with a separate 32-byte cold temporal/spatial history sidecar" );

idCVar r_pathTracingUnifiedPtCompactGeometry(
    "r_pathTracingUnifiedPtCompactGeometry",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT D0 geometry replay A/B for Vulkan RayQuery production with compact receiver 2: 0 binds the scene's 112-byte vertices; 1 GPU-packs four UPT-only 48-byte sidecars before D0 and binds those at the existing vertex slots" );

idCVar r_pathTracingUnifiedPtCompactLights(
    "r_pathTracingUnifiedPtCompactLights",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT D0 current-light A/B after compact geometry: 0 binds the manager's 112-byte records; 1 GPU-packs a UPT-only 64-byte current-light sidecar before D0 while preserving all replay identities" );

idCVar r_pathTracingUnifiedPtCompactMaterials(
    "r_pathTracingUnifiedPtCompactMaterials",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT D0 material A/B after compact lights: 0 binds the 112-byte material table; 1 GPU-packs a lossless-for-D0 48-byte sidecar; direct-only family suppresses the pack because it does not read secondary material records" );

idCVar r_pathTracingUnifiedPtSplitInitial(
    "r_pathTracingUnifiedPtSplitInitial",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT unified D0 A/B: 0 runs the three-query RayQuery megakernel; 1 splits direct and indirect work into two 8x8 dispatches using one exact intermediate/final 64-byte reservoir page; requires unified RayQuery production with compact32 receiver and supports either native geometry/lights or the paired compact geometry+light path" );

idCVar r_pathTracingUnifiedPtLightTiles(
    "r_pathTracingUnifiedPtLightTiles",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT split native-light D0 A/B: presample exact-PDF emissive and ordinal-stratified analytic light identities into coherent 8x8 screen-tile domains before D0; requires proposal parity and adds no visibility candidates" );

idCVar r_pathTracingUnifiedPtSplitContinuation(
    "r_pathTracingUnifiedPtSplitContinuation",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT unified D0 A/B: trace the deterministic primary continuation into one 32-byte hit-facts sidecar, then run direct plus secondary shading with one final reservoir write; mutually exclusive with the rejected direct/indirect split" );

idCVar r_pathTracingUnifiedPtDirectProposalParity(
    "r_pathTracingUnifiedPtDirectProposalParity",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT D0 direct proposal: 1 uses one uniform emissive trial plus up to 32 range-stratified analytic trials while retaining one selected visibility ray; 0 selects the fixed 8-trial split fallback" );

idCVar r_pathTracingUnifiedPtD0PreviousBest(
    "r_pathTracingUnifiedPtD0PreviousBest",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT D0 legacy-shaped previous-best emissive reseed: reproject one prior receiver, remap the stable light identity, replay at the current receiver, exclude duplicate fresh proposals, and retain one winner-only visibility ray" );

idCVar r_pathTracingUnifiedPtTemporal(
    "r_pathTracingUnifiedPtTemporal",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT-07 bounded temporal reuse over two fully written 64-byte reservoir pages; direct mode retains one winner-only visibility RayQuery and opt-in indirect replay has its separately verified four-ray ceiling; default off" );

idCVar r_pathTracingUnifiedPtTemporalSearch(
    "r_pathTracingUnifiedPtTemporalSearch",
    "2",
    CVAR_RENDERER | CVAR_INTEGER,
    "UPT-07 reprojection A/B: 0 exact floor single tap, 1 sub-pixel-dithered single tap, 2 dither plus up to eight randomized radius-4 compatible-surface probes (default); reservoir contents never steer the search" );

idCVar r_pathTracingUnifiedPtTemporalMaxHistoryM(
    "r_pathTracingUnifiedPtTemporalMaxHistoryM",
    "32",
    CVAR_RENDERER | CVAR_INTEGER,
    "UPT-07 maximum effective M admitted from the previous reservoir; clamps to 1..1024, default 32" );

idCVar r_pathTracingUnifiedPtTemporalMaxAge(
    "r_pathTracingUnifiedPtTemporalMaxAge",
    "32",
    CVAR_RENDERER | CVAR_INTEGER,
    "UPT-07 maximum selected-history age; clamps to 1..63, default 32 after temporal+spatial correlation acceptance; 63 remains the packed saturation/no-expiry stress value" );

idCVar r_pathTracingUnifiedPtTemporalPreviousBest(
    "r_pathTracingUnifiedPtTemporalPreviousBest",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT-07 previous-best current-domain seed A/B with single-admission duplicate exclusion and winner-only current visibility; default off pending runtime validation" );

idCVar r_pathTracingUnifiedPtTemporalPairwise(
    "r_pathTracingUnifiedPtTemporalPairwise",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT-07 estimator A/B: 1 replaces the cross-frame global basic-correction denominator with two-domain pairwise MIS using fresh targets at both receivers; 0 retains the existing temporal merge" );

idCVar r_pathTracingUnifiedPtTemporalIndirect(
    "r_pathTracingUnifiedPtTemporalIndirect",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT-07 bounded one-continuation temporal replay: reconstructs global endpoints and force-reconnects stored secondary-NEE light vertices without rerunning light selection; rejects any identity, replay, target, or visibility mismatch; default off during temporal-only validation" );

idCVar r_pathTracingUnifiedPtTemporalEarlyReconnect(
    "r_pathTracingUnifiedPtTemporalEarlyReconnect",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT-17 static secondary-vertex hybrid shift A/B: apply the paper footprint gate, reconnect the current receiver directly to stored x2, and use exact random replay for unsupported paths; pairwise MIS remains on replay" );

idCVar r_pathTracingUnifiedPtTemporalReconnectDiagnostics(
    "r_pathTracingUnifiedPtTemporalReconnectDiagnostics",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT-17 one-frame-delayed aggregate counters for static locator, footprint, reconnect visibility, Jacobian, replay fallback, and admission stages" );

idCVar r_pathTracingUnifiedPtTemporalRouteDiagnostics(
    "r_pathTracingUnifiedPtTemporalRouteDiagnostics",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT temporal workload diagnostic specialization: one-frame-delayed counters for current/history event mix, indirect replay attempts, BASIC selection, merge winner, age, and failure route; valid with early reconnect disabled" );

idCVar r_pathTracingUnifiedPtDuplication(
    "r_pathTracingUnifiedPtDuplication",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT-08 correlation control: publish a 31-bit random-sample identity, count matching copies in a 17x17 neighborhood, and adapt the next temporal M cap from default to 1; allocates no resources and dispatches no work when disabled" );

idCVar r_pathTracingUnifiedPtSpatial(
    "r_pathTracingUnifiedPtSpatial",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT-09 bounded direct spatial rescue: reads the completed D0/T0 page and fully writes the other existing 64-byte page; 3 regular or 12 empty-center attempts, 30-pixel radius, at most one visibility RayQuery; default off" );

idCVar r_pathTracingUnifiedPtSpatialProofMode(
    "r_pathTracingUnifiedPtSpatialProofMode",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "UPT-09 spatial estimator: 0 production unique-neighbor basic correction with non-recursive rescue, 1 exact reservoir pass-through, 2 reproduce old recursive with-replacement rescue, 3 non-recursive unique neighbors with standard 1/M normalization, 4 strict selected-center/selected-neighbor legacy admission, 5 mode 0 with every source-side target freshly replayed at its actual source surface, 6 selected-center pairwise MIS with fresh cross-domain targets and no empty-center rescue; clamps to 0..6" );

idCVar r_pathTracingUnifiedPtDirectTargetPdfParity(
    "r_pathTracingUnifiedPtDirectTargetPdfParity",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Use the RTXDI direct-light reservoir measure: luminance(BRDF*Li*cos)/solidAnglePdf with technique MIS in the scalar RIS weight. 0 restores the original UPT max-channel target with MIS embedded in the stored sample" );

idCVar r_pathTracingUnifiedPtAnalyticPortalDomain(
    "r_pathTracingUnifiedPtAnalyticPortalDomain",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "UPT analytic-light domain A/B: 1 admits only lights selected by the current Doom portal region while retaining the global stable light universe for identity/remap; 0 samples the full global analytic range" );

idCVar r_pathTracingUnifiedPtBackend(
    "r_pathTracingUnifiedPtBackend",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "UPT-04 static backend: 0 RayQuery compute (default), 1 ray-generation; changing it releases the old pipeline and creates only the selected replacement" );

idCVar r_pathTracingUnifiedPtFamily(
    "r_pathTracingUnifiedPtFamily",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "UPT-04 static family specialization: 0 unified direct+indirect, 1 direct-only (default bring-up), 2 indirect-only; only one pipeline exists at a time" );

idCVar r_pathTracingUnifiedPtProofStage(
    "r_pathTracingUnifiedPtProofStage",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "UPT live admission ladder after the full-frame GPU watchdog: 1 input closure only (safe default), 2 two-page allocation, 3 selected pipeline creation, 4 descriptor creation, 5 barriers+page-0 allocation clear, 6 state+push binding, 7 one 8x8 group, 8 one full-width 8-pixel row, 9 full-frame dispatch" );

idCVar r_pathTracingUnifiedPtShaderProof(
    "r_pathTracingUnifiedPtShaderProof",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "UPT-04 RayQuery execution ladder: 1 UAV write, 2 primary read, 3 proposal/material without tracing, 4 fixed valid RayQuery with status only, 5 proposed visibility RayQuery with status only, 6 full committed-hit metadata and reservoir completion, 7 compact one-invocation live-TLAS Slang probe, 8 byte-equivalent DXC-to-SPIR-V probe, 9 production reservoir/push shader ABI through the full host layout, 10 pure b0/b4 fixed query with status only, 11 same pure query with complete set 0 but bindless set 1 omitted, 12 only production TLAS b0 plus uint UAV b4, 13 mode 12 plus the 208-byte production push range, 14 full-frame one-ray traversal isolation using only TLAS, receiver position/normal, and one uint output" );

idCVar r_pathTracingUnifiedPtDiagnostics(
    "r_pathTracingUnifiedPtDiagnostics",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "One-shot UPT-04 RayQuery diagnostic specialization; clears and reads back the fixed 107-counter direct/indirect/material/continuation-route tuple plus crosshair D0/C0 probes and a 128-bit order-independent reservoir signature, then returns to the selected production specialization" );

idCVar r_pathTracingUnifiedPtFixedSampleIndex(
    "r_pathTracingUnifiedPtFixedSampleIndex",
    "-1",
    CVAR_RENDERER | CVAR_INTEGER,
    "UPT fixed RNG sample index for repeatability tests: -1 uses the live frame index, values >= 0 hold the UPT random streams at that sample without freezing unrelated renderer state" );

idCVar r_pathTracingUnifiedPtResolve(
    "r_pathTracingUnifiedPtResolve",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Admit the UPT-05 trace-free RGBA16F resolve and present its output; requires production shaderProof 1..6 and full-frame proofStage 9" );

idCVar r_pathTracingUnifiedPtResolveView(
    "r_pathTracingUnifiedPtResolveView",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "UPT-05 resolve view: 0 estimator, 1 direct/global classification, 2 diffuse/specular classification, 3 selected contribution, 4 normalization, 5 temporal state/rejection (selected blue-to-green; yellow no compatible history; red history empty; magenta unsupported event; cyan shift invalid; white shifted zero; green merge math), 6 emissive-triangle cast estimate only, 7 Doom-analytic estimate only, 8 emissive-triangle cast plus primary self-emission, 9 primary self-emission only" );

idCVar r_pathTracingCleanRtxdiDiEnable(
    "r_pathTracingCleanRtxdiDiEnable",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI path: default-on production view-16 route; set 0 to opt out for diagnostics; independent of existing RRX debug views" );

idCVar r_pathTracingCleanRtxdiDiView(
    "r_pathTracingCleanRtxdiDiView",
    "16",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI view: default 16 production material resolve from clean current/temporal/spatial reservoirs; 0 disabled, 1 route sentinel, 2 primary status, 3 analytic status, 4 raw flat current, 5 raw flat temporal, 6 raw flat split, 7 identity/M/history, 8 weight/targetPdf/rejection, 9 synthetic temporal, 10 synthetic analytic temporal, 11 synthetic overlap temporal, 12 clean material-classifier live-material proof, 13 real analytic one-sample scalar diagnostic, 14 real analytic target-factor diagnostic, 15 real analytic binary gate diagnostic, 17 RR motion-vector diagnostic, 18 RR input/guide mosaic, 19 RR guide albedo, 20 RR guide specular albedo, 21 RR depth contract bands, 22 primary hit reprojection error, 23 previous hit/motion reprojection error, 24 material classifier consumed-surface debug, 25 transmission PSR replacement mask" );

idCVar r_pathTracingCleanRtxdiDiTemporal(
    "r_pathTracingCleanRtxdiDiTemporal",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI path: enable temporal reuse; set 0 for current-only initial reservoir diagnostics" );

idCVar r_pathTracingCleanRtxdiDiSpatial(
    "r_pathTracingCleanRtxdiDiSpatial",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI path: default-on basic spatial reservoir reuse after the temporal producer pass; set 0 for temporal-only diagnostics" );

idCVar r_pathTracingCleanRtxdiDiStopAfterSpatial(
    "r_pathTracingCleanRtxdiDiStopAfterSpatial",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI view 16 diagnostic: stop after optional transmission PSR plus initial, temporal, and spatial DispatchRays, before material-feature composition, GI, RR, and later consumers; disable the transmission producer or compose CVar for a DI-only boundary" );

idCVar r_pathTracingCleanRtxdiDiBlueNoise(
    "r_pathTracingCleanRtxdiDiBlueNoise",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room RTXDI DI: allow eligible low-dimension DI and material-feature sampler sites to use the shared STBN blue-noise mask. Default off; requires shaders compiled with RBPT_ENABLE_BLUE_NOISE and a valid textures/bluenoise/stbn_scalar_128x128x64.raw mask." );

idCVar r_pathTracingCleanRtxdiDiTransmissionProducer(
    "r_pathTracingCleanRtxdiDiTransmissionProducer",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean RTXDI DI view 16 standalone transmission producer: 0 off, 1 write thin-glass transmission payload from the clean primary surface" );

idCVar r_pathTracingCleanRtxdiDiTransmissionCompose(
    "r_pathTracingCleanRtxdiDiTransmissionCompose",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean RTXDI DI view 16 glass sidecar compose: 0 off, 1 let the glass pass copy shaded output through the transmission sidecar path" );

idCVar r_pathTracingCleanRtxdiDiTransmissionIsolationStage(
    "r_pathTracingCleanRtxdiDiTransmissionIsolationStage",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean RTXDI DI view 16 transmission isolation: 0 iterative production, 1 source decode/no trace, 2 iterative trace/no resolve, 3 iterative resolved tuple/no publish, 4 iterative history record only, 5 iterative record plus guides/no sidecar, 6 iterative full control, 7 unstable legacy any-hit diagnostic" );

idCVar r_pathTracingCleanRtxdiDiTransmissionDebugView(
    "r_pathTracingCleanRtxdiDiTransmissionDebugView",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean RTXDI DI view 16 transmission producer debug output: 0 off, 1 thin-glass payload, 2 source validity red=strict green=relaxed blue=current-source energy, 3 PSR status green=replaced yellow=miss red=still-glass gray=not-glass, 4 reflection sidecar radiance, 5 cosmetic distortion sidecar, 6 cosmetic distorted source preview, 7 cosmetic distortion source difference, 8 normal-map diagnostic, 9 procedural warped checker, 10 reflection PSR lane/trace mask green=reflection-hit blue=transmission-selected cyan=candidate magenta=rejected yellow=reflection-miss gray=empty, 11 reflection PSR candidate class green=hit/selected cyan=transmission red=rejected yellow=miss gray=empty" );

idCVar r_pathTracingCleanRtxdiDiGlassReflectionPsr(
    "r_pathTracingCleanRtxdiDiGlassReflectionPsr",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean RTXDI DI view 16 glass reflection system (default on): deterministic/sticky PSR transport, one dense mirror hit, exact hit emissive, and bounded analytic RIS sidecar lighting" );

idCVar r_pathTracingReflectionOpaqueMirror(
    "r_pathTracingReflectionOpaqueMirror",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Experimental opaque exact-mirror reflections: 0 off, 1 reuse the dense reflection secondary path on opaque surfaces whose resolved roughness is effectively zero" );

idCVar r_pathTracingReflectionSecondarySamples(
    "r_pathTracingReflectionSecondarySamples",
    "8",
    CVAR_RENDERER | CVAR_INTEGER,
    "Glass reflection analytic RIS candidate count M (1-16, default 8), committing at most one shade and one shadow ray. Not multi-SPP" );

idCVar r_pathTracingReflectionSecondaryShadows(
    "r_pathTracingReflectionSecondaryShadows",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Dedicated reflection secondary: 0 skip visibility rays on simplified shade, 1 cast shadow rays (default)" );

idCVar r_pathTracingCleanRtxdiDiGlassDistortion(
    "r_pathTracingCleanRtxdiDiGlassDistortion",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean RTXDI DI view 16 cosmetic glass distortion sidecar: 0 off, 1 diagnostic final-color warp only; does not affect DI/GI/RR guides or PSR continuation direction" );

idCVar r_pathTracingCleanRtxdiDiGlassRefractedPsr(
    "r_pathTracingCleanRtxdiDiGlassRefractedPsr",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean RTXDI DI view 16 transmission PSR: 0 thin straight-through continuation, 1 use refracted PSR continuation ray for supported glass" );

idCVar r_pathTracingCleanRtxdiDiGlassRefractedPsrStrength(
    "r_pathTracingCleanRtxdiDiGlassRefractedPsrStrength",
    "0.25",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean RTXDI DI view 16 refracted transmission PSR direction strength: 0 straight-through thin glass, 1 full single-interface refraction" );

idCVar r_pathTracingCleanRtxdiDiGlassShader(
    "r_pathTracingCleanRtxdiDiGlassShader",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean RTXDI DI view 16 glass material-feature pass: 1 applies PSR sidecar compose when available, otherwise opaque glass overlay for incidence-response testing" );

idCVar r_pathTracingCleanRtxdiDiGlassDebugView(
    "r_pathTracingCleanRtxdiDiGlassDebugView",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean RTXDI DI view 16 glass material-feature debug output: 0 off, 1 sidecar status, 2 transmission RGB, 3 overlay energy; producer-only no-ops here, use transmission debug view for PSR producer/reflection status" );

idCVar r_pathTracingCleanRtxdiDiGlassReflectionBoost(
    "r_pathTracingCleanRtxdiDiGlassReflectionBoost",
    "1.5",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean RTXDI DI view 16 glass late-compose reflection boost; default matches object-glass material feature params" );

idCVar r_pathTracingCleanRtxdiDiGlassTransmissionFloor(
    "r_pathTracingCleanRtxdiDiGlassTransmissionFloor",
    "0.02",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean RTXDI DI view 16 glass late-compose minimum transmission floor; default matches object-glass material feature params" );

idCVar r_cleanDiSpatial(
    "r_cleanDiSpatial",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Short alias for clean-room Remix DI basic spatial reuse; set 0 for temporal-only diagnostics" );

idCVar r_cleanSpatial(
    "r_cleanSpatial",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Short alias for clean-room Remix DI basic spatial reuse; set 0 for temporal-only diagnostics" );

idCVar r_cleanDiSpatialSamples(
    "r_cleanDiSpatialSamples",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Short alias for clean DI spatial neighbor sample count; clamps to 1..16" );

idCVar r_cleanDiSpatialDisocclusionSamples(
    "r_cleanDiSpatialDisocclusionSamples",
    "4",
    CVAR_RENDERER | CVAR_INTEGER,
    "Short alias for clean DI spatial short-history/disocclusion neighbor sample count; clamps to 1..16" );

idCVar r_cleanDiSpatialRadius(
    "r_cleanDiSpatialRadius",
    "30",
    CVAR_RENDERER | CVAR_FLOAT,
    "Short alias for clean DI spatial screen-space neighbor radius in pixels" );

idCVar r_cleanDiBoilingFilter(
    "r_cleanDiBoilingFilter",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean DI DLSS RR input: enable post-resolve direct-light boiling clamp on the RR input color" );

idCVar r_cleanDiBoilingThreshold(
    "r_cleanDiBoilingThreshold",
    "5.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean DI DLSS RR input: clamp pixels whose 8x8 tile luminance exceeds tile average times this threshold" );

idCVar r_pathTracingCleanRtxdiDiBestLights(
    "r_pathTracingCleanRtxdiDiBestLights",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI RLU-13 previous-best seed switch; default-on projected previous temporal reservoir approximation, not full Remix best-light parity; set 0 for RLU-12 random-only testing" );

idCVar r_pathTracingCleanRtxdiDiLightMode(
    "r_pathTracingCleanRtxdiDiLightMode",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI light mode: 0 no lights negative test, 1 Doom analytic only, 2 emissive only deferred, 3 analytic plus emissive deferred" );

idCVar r_pathTracingCleanRtxdiDiExternalPdfNeeCurrent(
    "r_pathTracingCleanRtxdiDiExternalPdfNeeCurrent",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI consumes the PDFNEE-produced current reservoir page at u69 instead of rebuilding its own initial reservoir; first slice supports analytic split-domain indices" );

idCVar r_pathTracingCleanRtxdiDiNeeCacheProvider(
    "r_pathTracingCleanRtxdiDiNeeCacheProvider",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI initial sampling may draw dense current RLU proposals from the NEE cache provider/fallback mixture; does not edit temporal or spatial reuse" );

idCVar r_pathTracingCleanRtxdiDiDump(
    "r_pathTracingCleanRtxdiDiDump",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI one-shot route/resource dump for enable/view/features/light mode/page roles and dispatch dimensions" );

idCVar r_pathTracingCleanRtxdiDiGui(
    "r_pathTracingCleanRtxdiDiGui",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Show clean-room RTXDI DI route/domain/history diagnostics in the ImGui overlay; requires com_showFPS 2 or another active ImGui window" );

idCVar r_pathTracingCleanRtxdiDiCandidateCount(
    "r_pathTracingCleanRtxdiDiCandidateCount",
    "8",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI diagnostic: RTXDI initial local-light candidate samples for real analytic views; default 8 matches the existing view-12 baseline" );

idCVar r_pathTracingCleanRtxdiDiView8Band(
    "r_pathTracingCleanRtxdiDiView8Band",
    "-1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI diagnostic: force view 8 to show one diagnostic band full-screen; -1 keeps stacked bands, valid forced range is 0..17; band 8 classifies selected-sample black output cause, band 9 splits RLU selected/mapped replay causes, band 10 shows the NEE cache secondary emissive candidate field, bands 11..14 show previous-best source/translation/candidate/selection, band 15 shows selected light type, band 16 shows basic spatial reuse output when enabled, band 17 shows the final temporal reservoir state using the UPT resolve-view-5 age palette" );

idCVar r_pathTracingCleanRtxdiDiView18Tile(
    "r_pathTracingCleanRtxdiDiView18Tile",
    "-1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI diagnostic: force view 18 to show one DLSS RR input full-screen; -1 keeps the 2x3 mosaic, 0 albedo, 1 normal/roughness, 2 specular albedo, 3 input color, 4 depth/hit distance, 5 motion/reset, 6 max(input color, specular guide), 7 specular hit distance only using Remix linear grayscale hitDistance/1000" );

idCVar r_pathTracingCleanRtxdiDiResolveVisibilityReuse(
    "r_pathTracingCleanRtxdiDiResolveVisibilityReuse",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI spatial resolve visibility diagnostic: 0 trace final selected-light visibility, 1 reuse packed reservoir visibility when valid before tracing, 2 force selected sample visible with no final visibility trace, 3 checkerboard skip about half of final visibility traces" );

idCVar r_pathTracingCleanRtxdiDiResolveSolidAnglePdf(
    "r_pathTracingCleanRtxdiDiResolveSolidAnglePdf",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI diagnostic: divide final resolve by selected light solidAnglePdf; default 1 matches the RTXDI DI target/source convention used by this renderer" );

idCVar r_pathTracingCleanRtxdiDiInitialVisibility(
    "r_pathTracingCleanRtxdiDiInitialVisibility",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI diagnostic: trace selected initial DI sample visibility and discard invisible selected samples before temporal/spatial reuse" );

idCVar r_pathTracingCleanRtxdiDiResolveBrdfTarget(
    "r_pathTracingCleanRtxdiDiResolveBrdfTarget",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI diagnostic: make final resolve use the same RAB BRDF reflected-radiance function used by the target PDF instead of the flat-diffuse display resolve" );

idCVar r_pathTracingCleanRtxdiDiReferenceRab(
    "r_pathTracingCleanRtxdiDiReferenceRab",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI diagnostic: 1 strict reference-shaped RAB; 2 altered radiance+PDF prove-use; 3 altered radiance only; 4 altered target/PDF only; 5 stable candidate-index radiance; 6 constant white radiance; 7 stable universe-identity radiance; 8 stable payload-key radiance; 9 constant white with resolve visibility forced on; 10 constant white with resolve visibility and reservoir throughput forced on" );

idCVar r_pathTracingCleanRtxdiDiView10LightCount(
    "r_pathTracingCleanRtxdiDiView10LightCount",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI diagnostic: number of real Doom analytic payloads exposed to view 10 synthetic temporal sampling" );

idCVar r_pathTracingCleanRtxdiDiView10LightStart(
    "r_pathTracingCleanRtxdiDiView10LightStart",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI diagnostic: first sampleable Doom analytic payload index exposed to view 10 synthetic temporal sampling" );

idCVar r_pathTracingCleanRtxdiDiView10PortalDomain(
    "r_pathTracingCleanRtxdiDiView10PortalDomain",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI diagnostic: force view 10 synthetic single-Doom-payload proof to pick from the same portal-domain analytic count as view 12" );

idCVar r_pathTracingCleanRtxdiDiSubviewDispatch(
    "r_pathTracingCleanRtxdiDiSubviewDispatch",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI mirror/subview diagnostic: allow clean dispatch during subviews; set 0 to block the mirror-glitch route at entry" );

idCVar r_pathTracingCleanRtxdiDiSubviewReservoirPromote(
    "r_pathTracingCleanRtxdiDiSubviewReservoirPromote",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI mirror/subview diagnostic: allow subview temporal reservoir pages to promote into previous history" );

idCVar r_pathTracingCleanRtxdiDiSubviewSurfacePromote(
    "r_pathTracingCleanRtxdiDiSubviewSurfacePromote",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI mirror/subview diagnostic: allow subview primary-surface pages to promote into previous history" );

idCVar r_pathTracingCleanRtxdiDiView12FullAnalyticDomain(
    "r_pathTracingCleanRtxdiDiView12FullAnalyticDomain",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI view 12 diagnostic: use full current Doom analytic domain instead of portal-region analytic domain" );

idCVar r_pathTracingCleanRtxdiDiRelaxBrdfGates(
    "r_pathTracingCleanRtxdiDiRelaxBrdfGates",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI diagnostic: relax RAB opaque/geometric/view BRDF rejection gates; default off for baseline validation" );

idCVar r_pathTracingCleanRtxdiDiDoomTargetFloor(
    "r_pathTracingCleanRtxdiDiDoomTargetFloor",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI diagnostic: force valid Doom analytic light samples to have a nonzero target PDF floor; default off for baseline validation" );

idCVar r_pathTracingCleanRtxdiDiDummyEmissiveNormals(
    "r_pathTracingCleanRtxdiDiDummyEmissiveNormals",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI diagnostic: make RLU emissive triangle samples use orientation-independent source facing; isolates bad emissive source normals/facing without changing RTXDI reservoir math" );

idCVar r_pathTracingCleanRtxdiDiForceEmissiveVisibility(
    "r_pathTracingCleanRtxdiDiForceEmissiveVisibility",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI diagnostic: force selected RLU emissive triangle samples visible; isolates clean shadow/ignore rejection without changing RTXDI reservoir math" );

idCVar r_pathTracingCleanRtxdiDiTemporalRigidEmissives(
    "r_pathTracingCleanRtxdiDiTemporalRigidEmissives",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI diagnostic: allow temporal reuse of routed rigid emissive lights; set 0 to isolate routed rigid emissive temporal strobe regressions" );

idCVar r_pathTracingCleanRtxdiDiFrameFreeze(
    "r_pathTracingCleanRtxdiDiFrameFreeze",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI diagnostic: freeze the clean RTXDI frame index used by initial and temporal random streams; default off" );

idCVar r_pathTracingCleanRtxdiDiAnalyticDomainFreezeMs(
    "r_pathTracingCleanRtxdiDiAnalyticDomainFreezeMs",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI diagnostic: freeze uploaded Doom analytic current/previous/remap buffers for this many milliseconds between refreshes; 0 disables the freeze" );

idCVar r_pathTracingCleanRtxdiDiBypassLightUniverse(
    "r_pathTracingCleanRtxdiDiBypassLightUniverse",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI diagnostic: bypass the persistent Doom analytic light universe and synthesize compact current/previous/remap buffers from uploaded active candidates; default off" );

idCVar r_pathTracingCleanRtxdiDiDoomColorSource(
    "r_pathTracingCleanRtxdiDiDoomColorSource",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI diagnostic: Doom analytic payload color source; 0 material shader registers, 1 game current renderLight color, 2 game authored base color" );

idCVar r_pathTracingCleanRtxdiDiRequireProvenDoomLights(
    "r_pathTracingCleanRtxdiDiRequireProvenDoomLights",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix DI diagnostic: require game-linked non-temporary Doom analytic lights in the uploaded clean RTXDI analytic domain; default off" );

idCVar r_pathTracingCleanRtxdiDiTemporalBiasCorrection(
    "r_pathTracingCleanRtxdiDiTemporalBiasCorrection",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI diagnostic: RTXDI temporal bias correction mode; 0 off, 1 basic, 2+ ray traced. Default matches the current clean view-12 path" );

idCVar r_pathTracingCleanRtxdiDiTemporalMaxHistory(
    "r_pathTracingCleanRtxdiDiTemporalMaxHistory",
    "5",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI diagnostic: RTXDI temporal maxHistoryLength parameter; default 5 limits stale history during movement; use 0 to run temporal while suppressing previous-reservoir history contribution" );

idCVar r_pathTracingCleanRtxdiDiTemporalFireflyClamp(
    "r_pathTracingCleanRtxdiDiTemporalFireflyClamp",
    "32",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room Remix DI temporal history outlier guard: 0 off, otherwise reject reused history older than one frame when its targetPdf*W exceeds the current reservoir by this ratio" );

idCVar r_pathTracingCleanRtxdiDiTemporalAudit(
    "r_pathTracingCleanRtxdiDiTemporalAudit",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room RTXDI DI temporal audit: 0 off, 1 encode per-pixel accumulator gates into the clean output and aggregate them into the ImGui panel via readback" );

idCVar r_pathTracingCleanRestirGiEnable(
    "r_pathTracingCleanRestirGiEnable",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room Remix ReSTIR GI lane: 0 = no GI producer/reservoir/resolve work dispatches at all; 1 = the GI lane runs through its own explicit route only" );

idCVar r_pathTracingCleanRestirGiPipelineWarmupLimit(
    "r_pathTracingCleanRestirGiPipelineWarmupLimit",
    "18",
    CVAR_RENDERER | CVAR_INTEGER,
    "Vulkan clean-GI split-pipeline warmup cap: 0 builds none, 1..17 stop after that many modules for bounded validation, 18 builds the complete production lane (default)" );

idCVar r_pathTracingCleanRestirGiView(
    "r_pathTracingCleanRestirGiView",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI debug view: 0 off, 1 producer radiance, 2 producer hit geometry, 3 initial reservoir radiance*W, 4 temporal output radiance*W, 5 spatial output radiance*W, 6 final shaded indirect GI (diffuse+specular isolated), 7 reservoir M/age diagnostics, 8 route sentinel, 9 secondary material albedo, 10 secondary material texture-source flags, 11 final diffuse lobe, 12 final specular lobe, 13 specular producer radiance, 14 specular producer hit geometry, 15 specular producer PDF health, 16 specular lobe hit distance, 17 specular producer eligibility, 18 specular reuse state, 19 stored specular output, 20 NEE-cache provider state, 21 producer shade gate, 22 producer ray-query vs TraceRay compare, 23 producer-to-reservoir path classifier, 24 transmission PSR primary-surface mask, 25 spatial authority (R similar candidates, G accepted reservoirs, B neighbor selected), 26 Remix-comparable raw spatial radiance*W clamped to 0..1. Reads GI lane resources only" );

idCVar r_pathTracingCleanRestirGiTemporal(
    "r_pathTracingCleanRestirGiTemporal",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI temporal resampling: 0 = initial reservoir passes through to the temporal output page unchanged" );

idCVar r_pathTracingCleanRestirGiPermutationSampling(
    "r_pathTracingCleanRestirGiPermutationSampling",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI temporal permutation sampling: perturb the previous-frame reservoir address inside Remix-default 4x4 pixel blocks to produce denoiser-friendly temporal variation" );

idCVar r_pathTracingCleanRestirGiDlssRrCompatibility(
    "r_pathTracingCleanRestirGiDlssRrCompatibility",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI DLSS-RR compatibility A/B: while DLSS-RR evaluation is active, decorrelate diffuse temporal history with a broad randomized reprojection and increase the fresh sample's temporal authority" );

idCVar r_pathTracingCleanRestirGiDlssRrCompatibilityRadius(
    "r_pathTracingCleanRestirGiDlssRrCompatibilityRadius",
    "80",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI DLSS-RR compatibility temporal randomization radius at 960-pixel render width; scales with the active render width" );

idCVar r_pathTracingCleanRestirGiSpatial(
    "r_pathTracingCleanRestirGiSpatial",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI spatial resampling: 0 = spatial input passes through to the spatial output page unchanged" );

idCVar r_pathTracingCleanRestirGiSpatialRemixProfile(
    "r_pathTracingCleanRestirGiSpatialRemixProfile",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI spatial A/B: use broad alternating searches, history-starved four-neighbor recovery, grazing-aware gates, and pairwise central-sample suppression" );

idCVar r_pathTracingCleanRestirGiSpatialCentralWeight(
    "r_pathTracingCleanRestirGiSpatialCentralWeight",
    "0.1",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean-room ReSTIR GI Remix-profile pairwise importance of the central temporal reservoir; lower values let current-frame spatial neighbors replace coherent temporal artifacts more aggressively" );

idCVar r_pathTracingCleanRestirGiSpatialVisibility(
    "r_pathTracingCleanRestirGiSpatialVisibility",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI spatial visibility: 0 off, 1 validate every neighbor, 2 validate alternating half of neighbors" );

idCVar r_pathTracingCleanRestirGiTemporalBiasCorrection(
    "r_pathTracingCleanRestirGiTemporalBiasCorrection",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI temporal normalization: 0 off/count-M, 1 local BASIC support-count, 2 Remix-shaped target-PDF MIS normalization" );

idCVar r_pathTracingCleanRestirGiJacobian(
    "r_pathTracingCleanRestirGiJacobian",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI jacobian validation of reused samples. Turning this off is a diagnostic, not a shipping mode" );

idCVar r_pathTracingCleanRestirGiMaxHistoryLength(
    "r_pathTracingCleanRestirGiMaxHistoryLength",
    "4",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI temporal maxHistoryLength parameter; default 4 matches deployed-game Remix tuning; 0 bypasses previous-reservoir temporal history" );

idCVar r_pathTracingCleanRestirGiMaxReservoirAge(
    "r_pathTracingCleanRestirGiMaxReservoirAge",
    "12",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI max reservoir age in frames; default is conservative until lighting-change validation exists" );

idCVar r_pathTracingCleanRestirGiFireflyThreshold(
    "r_pathTracingCleanRestirGiFireflyThreshold",
    "20",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean-room ReSTIR GI firefly filtering luminance threshold applied to initial-sample radiance only (never to reused reservoirs); 0 disables" );

idCVar r_pathTracingCleanRestirGiContributionFireflyThreshold(
    "r_pathTracingCleanRestirGiContributionFireflyThreshold",
    "2",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean-room ReSTIR GI final weighted contribution firefly clamp: clamps luminance(radiance * reservoir W) before final lobe shading; 0 disables" );

idCVar r_pathTracingCleanRestirGiBlueNoise(
    "r_pathTracingCleanRestirGiBlueNoise",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI: feed spatiotemporal blue noise to eligible producer, initial-sample, and spatial-reuse RNG dimensions instead of white noise. Temporal reservoir selection remains white-noise to avoid imprinting structured history. Requires the STBN mask at textures/bluenoise/stbn_scalar_128x128x64.raw; falls back to white noise if absent." );

idCVar r_pathTracingCleanRestirGiMaxBounces(
    "r_pathTracingCleanRestirGiMaxBounces",
    "2",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI producer path depth: 1 keeps the current primary->secondary path, 2 adds one secondary->tertiary continuation bounce" );

idCVar r_pathTracingCleanRestirGiContinuationRoulette(
    "r_pathTracingCleanRestirGiContinuationRoulette",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI: apply Russian roulette only to the secondary->tertiary continuation segment when max bounces is 2" );

idCVar r_pathTracingCleanRestirGiContinuationRouletteMin(
    "r_pathTracingCleanRestirGiContinuationRouletteMin",
    "0.1",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean-room ReSTIR GI 2nd+ bounce diffuse continuation probability (legacy Min name); 0.1 matches Remix specular-based shipping configuration" );

idCVar r_pathTracingCleanRestirGiContinuationRouletteMax(
    "r_pathTracingCleanRestirGiContinuationRouletteMax",
    "0.98",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean-room ReSTIR GI 2nd+ bounce specular continuation probability (legacy Max name); blended by roughness and a 0.1 segment-distance factor like Remix" );

idCVar r_pathTracingCleanRestirGiContinuationDirectProbability(
    "r_pathTracingCleanRestirGiContinuationDirectProbability",
    "0.5",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean-room ReSTIR GI probability for terminal direct-light sampling on secondary->tertiary continuation hits; compensated when below 1" );

idCVar r_pathTracingCleanRestirGiSecondaryDirectProbability(
    "r_pathTracingCleanRestirGiSecondaryDirectProbability",
    "1",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean-room ReSTIR GI probability for direct-light sampling at the first secondary hit; compensated when below 1 to reduce one-bounce shadow-ray cost" );

idCVar r_pathTracingCleanRestirGiSecondaryDirectSamples(
    "r_pathTracingCleanRestirGiSecondaryDirectSamples",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI direct-light proposal count at the first secondary hit; raise above 1 only for explicit producer-density diagnostics" );

idCVar r_pathTracingCleanRestirGiSecondaryRluCandidates(
    "r_pathTracingCleanRestirGiSecondaryRluCandidates",
    "2",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI RLU RIS candidate count for each first-secondary direct-light proposal; 8 matches the DI candidate-count legacy behavior" );

idCVar r_pathTracingCleanRestirGiDiSampleStealing(
    "r_pathTracingCleanRestirGiDiSampleStealing",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI A/B: project compatible first-secondary hits into the current primary surface and replay its finalized DI reservoir sample before falling back to RLU RIS" );

idCVar r_pathTracingCleanRestirGiTypedStridedRis(
    "r_pathTracingCleanRestirGiTypedStridedRis",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI A/B: stratify the fixed secondary RLU RIS budget across the emissive and analytic light ranges instead of sampling the combined RLU uniformly" );

idCVar r_pathTracingCleanRestirGiLocalityRis(
    "r_pathTracingCleanRestirGiLocalityRis",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI A/B: make typed secondary RIS resist large indoor light-set dilution using the emissive power distribution and a PDF-correct portal-local/global analytic mixture" );

idCVar r_pathTracingCleanRestirGiContinuationOpaqueTrace(
    "r_pathTracingCleanRestirGiContinuationOpaqueTrace",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI: trace the optional secondary->tertiary continuation as opaque to avoid alpha any-hit cost on second bounces" );

idCVar r_pathTracingCleanRestirGiProducerOpaqueTrace(
    "r_pathTracingCleanRestirGiProducerOpaqueTrace",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI diagnostic: trace producer bounce rays as opaque to bypass any-hit alpha/material rejection. Off preserves alpha-aware GI producer behavior" );

idCVar r_pathTracingCleanRestirGiProducerConsumeProof(
    "r_pathTracingCleanRestirGiProducerConsumeProof",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI producer workload proof: 0 normal, 1 trace immediate invalid candidate, 2 shade immediate magenta return, 3 shade full surface load/unpack/store only, 4 full shade with visibility accepted before shadow TraceRay, 5 shade scalar valid-field read plus near-identical magenta return, 6 trace with normal any-hit/closest-hit payload but no reconstruction, 7 mode 6 plus full three-vertex geometry loads but no material evaluation or candidate packing" );

idCVar r_pathTracingCleanRestirGiForceFullShade(
    "r_pathTracingCleanRestirGiForceFullShade",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI diagnostic: at the otherwise identical one-bounce production settings, select the full producer shade pipeline instead of ShadeFast so shader-shape cost can be separated from continuation-bounce cost" );

idCVar r_pathTracingCleanRestirGiBindingSetCache(
    "r_pathTracingCleanRestirGiBindingSetCache",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI Vulkan performance A/B: retain identical binding sets instead of creating and destroying a dedicated Vulkan descriptor pool for every set every frame" );

idCVar r_pathTracingCleanRestirGiSplitSpatialFinal(
    "r_pathTracingCleanRestirGiSplitSpatialFinal",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI Vulkan performance A/B: production view 0 with spatial visibility disabled runs trace-free spatial reservoir reuse and final shading/visibility in separate raygen modules and Nsight markers instead of the fused reuse entry" );

idCVar r_pathTracingCleanRestirGiSpatialCompute(
    "r_pathTracingCleanRestirGiSpatialCompute",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI Vulkan performance A/B: with split spatial/final active, execute the same trace-free spatial contract as compute instead of an RT raygen pipeline" );

idCVar r_pathTracingCleanRestirGiProducerSimple(
    "r_pathTracingCleanRestirGiProducerSimple",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI experiment: use a stripped one-dispatch TraceRay initial producer that writes the raw GI sample directly, bypassing the split trace/shade producer" );

idCVar r_pathTracingCleanRestirGiProducerLeanSplit(
    "r_pathTracingCleanRestirGiProducerLeanSplit",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI experiment: use a lean split TraceRay producer with constrained-normal trace plus stripped one-sample shade" );

idCVar r_pathTracingCleanRestirGiProducerRayQuery(
    "r_pathTracingCleanRestirGiProducerRayQuery",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI diagnostic: dispatch CleanGI.0a producer trace as inline ray-query compute; alpha/material rejection is handled in the ray-query loop" );

idCVar r_pathTracingCleanRestirGiProducerRayQueryRoughFallback(
    "r_pathTracingCleanRestirGiProducerRayQueryRoughFallback",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI diagnostic: after the inline ray-query producer, retrace rough/non-specular surfaces with the known-good RT producer path" );

idCVar r_pathTracingCleanRestirGiProducerRayQueryHitIdMode(
    "r_pathTracingCleanRestirGiProducerRayQueryHitIdMode",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI diagnostic: inline ray-query hit instance source, 0=InstanceID/custom index, 1=InstanceIndex, 2=InstanceID with out-of-range InstanceIndex fallback" );

idCVar r_pathTracingCleanRestirGiNeeCacheSeed(
    "r_pathTracingCleanRestirGiNeeCacheSeed",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI: merge the NEE-cache-seeded reservoir at the temporal pass initial-sample step (Remix neeCache.enableOnFirstBounce equivalent). Off for bring-up" );

idCVar r_pathTracingCleanRestirGiNeeCacheSecondary(
    "r_pathTracingCleanRestirGiNeeCacheSecondary",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI: allow secondary-bounce NEE-cache provider queries. Off by default because the current cache is primary-surface populated; primary GI seed remains controlled by r_pathTracingCleanRestirGiNeeCacheSeed" );

idCVar r_pathTracingCleanRestirGiNeeCacheSecondaryMode(
    "r_pathTracingCleanRestirGiNeeCacheSecondaryMode",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI secondary NEE-cache policy when r_pathTracingCleanRestirGiNeeCacheSecondary is enabled: 0 off, 1 specular/glossy producer rays only, 2 all secondary producer rays" );

idCVar r_pathTracingCleanRestirGiNeeCacheSecondaryRoughness(
    "r_pathTracingCleanRestirGiNeeCacheSecondaryRoughness",
    "0.1",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean-room ReSTIR GI secondary NEE-cache max roughness for specular-only mode; mirrors Remix's after-first-bounce cache gating shape" );

idCVar r_pathTracingCleanRestirGiNeeCacheSecondaryProbability(
    "r_pathTracingCleanRestirGiNeeCacheSecondaryProbability",
    "1",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean-room ReSTIR GI secondary NEE-cache Bernoulli attempt probability after mode/roughness gating; lower values trade cache reuse for performance" );

idCVar r_pathTracingCleanRestirGiSpecularProducer(
    "r_pathTracingCleanRestirGiSpecularProducer",
    "2",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI specular mode: 0 off, 1 full specular/glossy seed producer reference, 2 one-ray mixed producer default that keeps specular output eligibility but skips the extra full-screen specular seed trace/shade" );

idCVar r_pathTracingCleanRestirGiGlossySecondRay(
    "r_pathTracingCleanRestirGiGlossySecondRay",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI experiment: in mixed specular mode, add one extra specular first-indirect seed for glossy primary surfaces; default off after high-cost/low-quality A/B" );

idCVar r_pathTracingCleanRestirGiGlossySecondRayRoughness(
    "r_pathTracingCleanRestirGiGlossySecondRayRoughness",
    "0.65",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean-room ReSTIR GI glossy second-ray max primary roughness; generous default tests whether extra RT traversal can replace candidate-loop pressure" );

idCVar r_pathTracingCleanRestirGiRrHitDistance(
    "r_pathTracingCleanRestirGiRrHitDistance",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI: write eligible reflective GI specular hit distances into the RR/DLSS specular hit-distance guide. Requires the specular producer and is off by default for A/B validation" );

idCVar r_pathTracingCleanRestirGiRrSpecularInput(
    "r_pathTracingCleanRestirGiRrSpecularInput",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI: add eligible stored specular GI into PathTraceRRInputColor only when the full GI resolve is not already feeding RR. Off by default; the old path double-added specular when both routes were active" );

idCVar r_pathTracingCleanRestirGiResolve(
    "r_pathTracingCleanRestirGiResolve",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Clean-room ReSTIR GI: add the final-shaded GI output into the combined resolve (T path / behind-glass). Default 1. Glass hybrid R also needs LinkedGi so mirror GI is written into the reflection sidecar" );

idCVar r_pathTracingCleanRestirGiResolveGain(
    "r_pathTracingCleanRestirGiResolveGain",
    "1.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean-room ReSTIR GI gain for the final view-0 resolve add and matching RR input resolve; default 1 uses raw indirect energy after source-PDF reservoir weighting" );

idCVar r_pathTracingCleanRestirGiFinalMix(
    "r_pathTracingCleanRestirGiFinalMix",
    "13",
    CVAR_RENDERER | CVAR_INTEGER,
    "Clean-room ReSTIR GI final shading mix: 0 reservoir-only, 1 raw initial-only, 2 fixed raw/reservoir blend, 3 adaptive raw fallback. Add 10 to final-visibility-test reused reservoir samples before shading" );

idCVar r_pathTracingCleanRestirGiBoilingFilter(
    "r_pathTracingCleanRestirGiBoilingFilter",
    "15",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean-room ReSTIR GI boiling filter min threshold: clamp shaded indirect diffuse/specular above each lobe's group average luminance * threshold, Remix final-shading style. 0 disables. Deployed-game reference (HL2 RTX) uses 15" );

idCVar r_pathTracingCleanRestirGiBoilingFilterMax(
    "r_pathTracingCleanRestirGiBoilingFilterMax",
    "20",
    CVAR_RENDERER | CVAR_FLOAT,
    "Clean-room ReSTIR GI boiling filter max threshold when surface normal faces the view; deployed-game reference (HL2 RTX) uses 20" );

idCVar r_pathTracingDispatchTileEnable(
    "r_pathTracingDispatchTileEnable",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic mode: split path tracing DispatchRays into screen-space tiles while preserving the same scene and resources" );

idCVar r_pathTracingDispatchTileWidth(
    "r_pathTracingDispatchTileWidth",
    "512",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic tiled DispatchRays tile width in pixels when r_pathTracingDispatchTileEnable is set" );

idCVar r_pathTracingDispatchTileHeight(
    "r_pathTracingDispatchTileHeight",
    "512",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic tiled DispatchRays tile height in pixels when r_pathTracingDispatchTileEnable is set" );

idCVar r_pathTracingDisableAnyHitAlpha(
    "r_pathTracingDisableAnyHitAlpha",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic kill switch: disable any-hit alpha, translucent, glass, and particle rejection while preserving shadow self-ignore" );

idCVar r_pathTracingDisableSelectedLightLoop(
    "r_pathTracingDisableSelectedLightLoop",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic kill switch: disable the legacy selected point-light loops in the smoke/path tracer shader" );

idCVar r_pathTracingDisableAnalyticLightLoop(
    "r_pathTracingDisableAnalyticLightLoop",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic kill switch: disable Doom analytic light loops and RAB analytic light sampling in the smoke/path tracer shader" );

idCVar r_pathTracingDisableEmissiveTriangleSampling(
    "r_pathTracingDisableEmissiveTriangleSampling",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic kill switch: disable emissive triangle sampling loops for clean DI and PDFNEE validation paths" );

idCVar r_pathTracingEmissiveDistribution(
    "r_pathTracingEmissiveDistribution",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Use the CPU-built emissive CDF proposal table when available; 0 forces the legacy shader linear scan" );

idCVar r_pathTracingDisableDiffuseSecondaryRay(
    "r_pathTracingDisableDiffuseSecondaryRay",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic kill switch: disable diffuse secondary continuation rays in the toy path tracer and RAB path tracer bridge" );

idCVar r_pathTracingDisableReflectionRay(
    "r_pathTracingDisableReflectionRay",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic kill switch: disable reflection/specular secondary rays in the toy path tracer" );

idCVar r_pathTracingDisablePrimarySurfaceHistory(
    "r_pathTracingDisablePrimarySurfaceHistory",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic kill switch: disable primary surface history writes, clears, copies, and previous-camera history validity" );

idCVar r_pathTracingCleanRtxdiDiPrimarySurfaceHistorySwap(
    "r_pathTracingCleanRtxdiDiPrimarySurfaceHistorySwap",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Promote clean DI primary-surface history by swapping current/previous buffer handles instead of copying the full 176-byte-per-pixel buffer; 0 restores the legacy copy for comparison" );

idCVar r_pathTracingDisableRestirVisibilityRay(
    "r_pathTracingDisableRestirVisibilityRay",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic kill switch: disable the ReSTIR PT preview visibility ray even when the preview visibility CVar or mode requests it" );

idCVar r_pathTracingSmokeParticleDither(
    "r_pathTracingSmokeParticleDither",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Use stable alpha dithering for RT smoke particle cards and ignore them for debug shadow rays" );

idCVar r_pathTracingSmokeParticleAlphaScale(
    "r_pathTracingSmokeParticleAlphaScale",
    "0.25",
    CVAR_RENDERER | CVAR_FLOAT,
    "Opacity scale for RT smoke particle-card alpha dithering" );

idCVar r_pathTracingParticleComposite(
    "r_pathTracingParticleComposite",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Screen-space particle lane: 0=off, 1=capture on, 2=capture with diagnostic tint requested; PC-T01 captures CPU records only" );

idCVar r_pathTracingParticleAmbient(
    "r_pathTracingParticleAmbient",
    "0.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Optional broad fill multiplier for alpha particle cards; 0 preserves visible response to the live light domain" );

idCVar r_pathTracingParticleEmissiveScale(
    "r_pathTracingParticleEmissiveScale",
    "1.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Global scale recorded for emissive particle-card batches" );

idCVar r_pathTracingParticleFireEmissiveScale(
    "r_pathTracingParticleFireEmissiveScale",
    "3.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Additional HDR emissive multiplier for textures/particles/pfiresmall and pfiresmall2; 1 restores the global particle emissive scale" );

idCVar r_pathTracingParticleFlares(
    "r_pathTracingParticleFlares",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Composite legacy deform-flare glow geometry; set 0 to hide screen-space fake volumetric light flares while retaining particle effects" );

idCVar r_pathTracingParticleOpacity(
    "r_pathTracingParticleOpacity",
    "0.7",
    CVAR_RENDERER | CVAR_FLOAT,
    "Global opacity scale for alpha-blended particle cards; pure additive cards are unaffected" );

idCVar r_pathTracingParticleSoftDepth(
    "r_pathTracingParticleSoftDepth",
    "8.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "World-space soft-intersection depth recorded for particle-card batches" );

idCVar r_pathTracingParticleLighting(
    "r_pathTracingParticleLighting",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Enable approximate local lighting for AlphaLit particle cards from the current Remix light universe" );

idCVar r_pathTracingParticleLightingDebug(
    "r_pathTracingParticleLightingDebug",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Particle lighting diagnostic: 0=off, 1=force AlphaLit cards red without reading the lighting buffer" );

idCVar r_pathTracingParticleLightCandidates(
    "r_pathTracingParticleLightCandidates",
    "4096",
    CVAR_RENDERER | CVAR_INTEGER,
    "Unified-light records evaluated for deterministic particle local fill; the default covers the active domain" );

idCVar r_pathTracingParticleShadowRays(
    "r_pathTracingParticleShadowRays",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Particle-card visibility diagnostic: 0=soft unshadowed local fill, 1 or greater=one hard selected-light visibility ray" );

idCVar r_pathTracingParticleTemporalLighting(
    "r_pathTracingParticleTemporalLighting",
    "1",
    CVAR_RENDERER | CVAR_BOOL,
    "Reuse compatible previous-frame particle irradiance by stable smoke-particle identity" );

idCVar r_pathTracingParticleTemporalWeight(
    "r_pathTracingParticleTemporalWeight",
    "0.85",
    CVAR_RENDERER | CVAR_FLOAT,
    "Previous-frame weight for compatible particle irradiance history; large lighting changes reject history" );

idCVar r_pathTracingParticleSortMode(
    "r_pathTracingParticleSortMode",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Alpha particle ordering mode reserved for the composite: 0=none, 1=CPU back-to-front" );

idCVar r_pathTracingParticleDump(
    "r_pathTracingParticleDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Bounded particle-composite diagnostics: 0=off, 1=one frame plus batch details, 2=240-frame per-frame trace then auto-off" );

idCVar r_pathTracingParticleGpuTiming(
    "r_pathTracingParticleGpuTiming",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Remaining nonblocking Vulkan GPU timestamp samples around particle upload, lighting, and composite draws; set 1..240, results print only after poll reports ready" );

idCVar r_pathTracingParticleBindingCache(
    "r_pathTracingParticleBindingCache",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Experimental particle graphics binding-set reuse: 0=per-batch creation, 1=cache identical resource tuples for descriptor-stall A/B" );

idCVar r_pathTracingSmokeParticleEdgeFade(
    "r_pathTracingSmokeParticleEdgeFade",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Fade RT smoke particle-card dither opacity near card UV edges" );

idCVar r_pathTracingPortalWindowStochastic(
    "r_pathTracingPortalWindowStochastic",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Use stable stochastic transparency for RT smoke portal/window surfaces" );

idCVar r_pathTracingPortalWindowAlphaScale(
    "r_pathTracingPortalWindowAlphaScale",
    "0.35",
    CVAR_RENDERER | CVAR_FLOAT,
    "Opacity scale for RT smoke portal/window stochastic transparency" );

idCVar r_pathTracingPortalWindowMinOpacity(
    "r_pathTracingPortalWindowMinOpacity",
    "0.05",
    CVAR_RENDERER | CVAR_FLOAT,
    "Minimum primary-ray opacity for RT smoke portal/window stochastic transparency" );

idCVar r_pathTracingPortalWindowShadowOpacity(
    "r_pathTracingPortalWindowShadowOpacity",
    "0.05",
    CVAR_RENDERER | CVAR_FLOAT,
    "Shadow-ray opacity multiplier for RT smoke portal/window stochastic transparency" );

idCVar r_pathTracingAdditiveDecalKey(
    "r_pathTracingAdditiveDecalKey",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Treat additive translucent decal/signage materials as RGB-keyed RT overlays" );

idCVar r_pathTracingAdditiveEmissiveBlendThrough(
    "r_pathTracingAdditiveEmissiveBlendThrough",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Deterministically composite additive emissive signage/glow cards over their receiver instead of stochastic any-hit coverage; enabled by default after door-panel/signage validation" );

idCVar r_pathTracingDecalComposite(
    "r_pathTracingDecalComposite",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Detail-decal blend-through composite stage: 0=legacy stochastic, 1=composite on, 2=normal-offset geometry only, 3=any-hit collect only (no composite), 4=composite diagnostic tint. Changing the offset stages re-captures static geometry on map reload" );

idCVar r_pathTracingDecalOffsetStep(
    "r_pathTracingDecalOffsetStep",
    "0.15",
    CVAR_RENDERER | CVAR_FLOAT,
    "World-unit step per offset index for the detail-decal face-normal lift" );

idCVar r_pathTracingDecalMaxOffsetIndex(
    "r_pathTracingDecalMaxOffsetIndex",
    "8",
    CVAR_RENDERER | CVAR_INTEGER,
    "Cap for the incrementing detail-decal offset index" );

idCVar r_pathTracingDecalModulateFloor(
    "r_pathTracingDecalModulateFloor",
    "0.12",
    CVAR_RENDERER | CVAR_FLOAT,
    "Per-channel floor for the filter/modulate decal multiply factor. Prevents exact-zero albedo, which the DI/RR pipeline treats as an invalid surface (reads as holes)" );

idCVar r_pathTracingLiquidPoolMode(
    "r_pathTracingLiquidPoolMode",
    "3",
    CVAR_RENDERER | CVAR_INTEGER,
    "Liquid-pool receiver modifier: 0=legacy rollback, 1=classification/collection diagnostic, 2=effective smooth coat, 3=effective coat plus generated film-owned normal (default)",
    0, 3, idCmdSystem::ArgCompletion_Integer<0, 3> );

idCVar r_pathTracingLiquidPoolDebug(
    "r_pathTracingLiquidPoolDebug",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Liquid-pool debug: 0=off, 1=classification, 2=raw candidates, 3=validated union, 4=winner/rejection/overflow, 5=resolved optics, 6=secondary route",
    0, 6, idCmdSystem::ArgCompletion_Integer<0, 6> );

idCVar r_pathTracingLiquidPoolDebugPage(
    "r_pathTracingLiquidPoolDebugPage",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Liquid-pool raw diagnostic tuple page; ignored when the selected debug mode has no additional page",
    0, 3, idCmdSystem::ArgCompletion_Integer<0, 3> );

idCVar r_pathTracingAllowGuiTextures(
    "r_pathTracingAllowGuiTextures",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Allow strictly validated GUI/SWF-like material textures into RT smoke bindless texture diagnostics" );

idCVar r_pathTracingAllowGuiSurfaces(
    "r_pathTracingAllowGuiSurfaces",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Allow GUI/SWF draw-surface geometry cards into RT smoke capture diagnostics" );

idCVar r_pathTracingSkipCallbackEntities(
    "r_pathTracingSkipCallbackEntities",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Skip custom-shader deferred callback render entities in RT smoke capture to avoid item/pickup lifetime hazards" );

idCVar r_pathTracingAnchorRaycast(
    "r_pathTracingAnchorRaycast",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Use center-ray scene anchor and dynamic-surface ordering for RT smoke capture; slower but useful when surface caps hide important geometry" );

idCVar r_pathTracingMaterialMetadataCache(
    "r_pathTracingMaterialMetadataCache",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Cache RT smoke per-material texture metadata; frame-local material tables are still rebuilt" );

idCVar r_pathTracingSmokeLog(
    "r_pathTracingSmokeLog",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Enable broad periodic RT smoke debug logging; verbose diagnostic firehose" );

idCVar r_pathTracingTimingLog(
    "r_pathTracingTimingLog",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Enable RT smoke slow-frame CPU timing logs" );

idCVar r_pathTracingTimingThreshold(
    "r_pathTracingTimingThreshold",
    "40",
    CVAR_RENDERER | CVAR_INTEGER,
    "RT smoke CPU timing log threshold in milliseconds" );

idCVar r_pathTracingTimingLogInterval(
    "r_pathTracingTimingLogInterval",
    "1000",
    CVAR_RENDERER | CVAR_INTEGER,
    "Minimum milliseconds between repeated RT smoke timing log lines; 0 logs every threshold hit" );

idCVar r_pathTracingOptickGpuMarkers(
    "r_pathTracingOptickGpuMarkers",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Enable experimental Optick GPU markers inside the RT smoke/path tracing build and dispatch passes" );

idCVar r_pathTracingOptickCaptureDelayFrames(
    "r_pathTracingOptickCaptureDelayFrames",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Optick-only automatic PT capture delay in built scene frames; used with r_pathTracingOptickCaptureFrames" );

idCVar r_pathTracingOptickCaptureFrames(
    "r_pathTracingOptickCaptureFrames",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Optick-only automatic instrumentation capture length in built scene frames; saves pathtrace_geometry(timestamp).opt" );

idCVar r_pathTracingNsightGpuMarkers(
    "r_pathTracingNsightGpuMarkers",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Enable NVRHI GPU debug markers around RT smoke/path tracing dispatches for Nsight captures" );

idCVar r_pathTracingGpuSkinning(
    "r_pathTracingGpuSkinning",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "PT skinned geometry producer: 0 = legacy CPU-skinned bridge only, 1 = canonical per-instance GPU output for production routes, 2 = diagnostic compute overwrite of merged dynamic vertices before BLAS build" );

idCVar r_pathTracingGpuSkinningParityDump(
    "r_pathTracingGpuSkinningParityDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "One-shot PT GPU-skinning funnel and bounded current/previous CPU-vs-GPU vertex parity readback" );

idCVar r_pathTracingMotionVectorExport(
    "r_pathTracingMotionVectorExport",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "PT private motion-vector export writer: 0 disabled, 1 write combined geometry current-to-previous pixel motion plus validity/source mask into private PT UAVs" );

idCVar r_pathTracingMotionVectorDisableRigid(
    "r_pathTracingMotionVectorDisableRigid",
    "0",
    CVAR_RENDERER | CVAR_BOOL,
    "Debug-only PT motion-vector quarantine: mark routed rigid surfaces as invalid motion instead of exporting rigid object-motion vectors" );

idCVar r_pathTracingDLSSRRGuideDebugView(
    "r_pathTracingDLSSRRGuideDebugView",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Mode 56 DLSS RR guide debug view: 0 = off, 1 = albedo, 2 = normal, 3 = roughness, 4 = depth, 5 = actual specular hit distance using Remix linear grayscale hitDistance/1000, 6 = motion-vector mask, 7 = reset/disocclusion mask, 8 = specular albedo/F0, 9 = RR input HDR preview, 10 = RR motion vector" );

idCVar r_pathTracingDLSSRRProbe(
    "r_pathTracingDLSSRRProbe",
    "1",
    CVAR_RENDERER | CVAR_INTEGER | CVAR_INIT,
    "Initialize the experimental Streamline DLSS/RR bridge at renderer startup and dump SDK feature support; set 0 before startup to disable" );

idCVar r_pathTracingDLSSRR(
    "r_pathTracingDLSSRR",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Experimental DLSS Ray Reconstruction evaluation gate for path-traced primary-prepass output" );

idCVar r_pathTracingDLSSRRMode(
    "r_pathTracingDLSSRRMode",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "DLSS RR quality mode: 0 = DLAA/native, 1 = Quality, 2 = Balanced, 3 = Performance, 4 = Ultra Performance, 5 = Ultra Quality" );

idCVar r_pathTracingDLSSRRColorBuffersHDR(
    "r_pathTracingDLSSRRColorBuffersHDR",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "DLSS RR colorBuffersHDR option; default 1 because the current Streamline/NGX RR path rejects the SDR-tagged option" );

idCVar r_pathTracingDLSSRRPreExposure(
    "r_pathTracingDLSSRRPreExposure",
    "1.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "DLSS RR preExposure option; keep 1.0 unless the input color has already been pre-exposed" );

idCVar r_pathTracingDLSSRRExposureScale(
    "r_pathTracingDLSSRRExposureScale",
    "1.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "DLSS RR exposureScale option for diagnostic color/exposure tuning of the experimental RR bridge" );

idCVar r_pathTracingDLSSRRForceReset(
    "r_pathTracingDLSSRRForceReset",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic DLSS RR history isolation: force a Ray Reconstruction reset every evaluated frame" );

idCVar r_pathTracingDLSSRRCameraFar(
    "r_pathTracingDLSSRRCameraFar",
    "2048",
    CVAR_RENDERER | CVAR_FLOAT,
    "DLSS RR cameraFar (world units) for the finite-far RR depth projection. Sets the far plane the depth buffer + cameraViewToClip reach 1.0/0.0 at. <=znear falls back to 100000. 2048 validated for Doom 3 BFG (depth-overlay parity with reference DLSS-RR games)" );

idCVar r_pathTracingDLSSRRDepthMode(
    "r_pathTracingDLSSRRDepthMode",
    "2",
    CVAR_RENDERER | CVAR_INTEGER,
    "DLSS RR clean-path depth contract: 2 = hyperbolic hardware depth [0,1] tagged kBufferTypeDepth (default; the form DLSS-RR actually consumes), 0 = normalized linear view-Z [0,1], 1 = raw linear view-Z (0/1 tagged kBufferTypeLinearDepth). Tune cameraNear/cameraFar to spread the hyperbolic distribution" );

idCVar r_pathTracingDLSSRRCameraNear(
    "r_pathTracingDLSSRRCameraNear",
    "0.2",
    CVAR_RENDERER | CVAR_FLOAT,
    "DLSS RR cameraNear (world units) for the RR depth frustum. <=0 uses r_znear. 0.2 validated for Doom 3 BFG (depth-overlay parity with reference DLSS-RR games). far/near ratio controls the hyperbolic distribution; geometry closer than this clamps to the near plane" );

idCVar r_pathTracingDLSSRRMotionVectorScaleX(
    "r_pathTracingDLSSRRMotionVectorScaleX",
    "1.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Diagnostic DLSS RR motion-vector Streamline scale multiplier for X; use -1 to test sign convention, 0 to remove X motion" );

idCVar r_pathTracingDLSSRRMotionVectorScaleY(
    "r_pathTracingDLSSRRMotionVectorScaleY",
    "1.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Diagnostic DLSS RR motion-vector Streamline scale multiplier for Y; use -1 to test sign convention, 0 to remove Y motion" );

idCVar r_pathTracingDLSSRRClipHistory(
    "r_pathTracingDLSSRRClipHistory",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic DLSS RR clip-history constants gate: 1 sends clipToPrevClip/prevClipToClip, 0 sends identity while keeping RR history active" );

idCVar r_pathTracingDLSSRRJitterScaleX(
    "r_pathTracingDLSSRRJitterScaleX",
    "1.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Diagnostic DLSS RR jitter offset scale for X as reported to Streamline; primary rays are unchanged" );

idCVar r_pathTracingDLSSRRJitterScaleY(
    "r_pathTracingDLSSRRJitterScaleY",
    "1.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "Diagnostic DLSS RR jitter offset scale for Y as reported to Streamline; primary rays are unchanged" );

idCVar r_pathTracingDLSSRRSharpness(
    "r_pathTracingDLSSRRSharpness",
    "0.0",
    CVAR_RENDERER | CVAR_FLOAT,
    "DLSS RR sharpness option, clamped 0..1" );

idCVar r_pathTracingDLSSRRDenoiserPreset(
    "r_pathTracingDLSSRRDenoiserPreset",
    "4",
    CVAR_RENDERER | CVAR_INTEGER,
    "DLSS RR denoiser preset: 0 = SDK default, 4 = preset D/default transformer, 5 = preset E/latest transformer" );

idCVar r_pathTracingDLSSRRVerbose(
    "r_pathTracingDLSSRRVerbose",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Verbose Streamline DLSS/RR logging. 1 = events only (one-time CONTRACT dump, HISTORY RESET on fire, errors/warnings) -- no per-frame spam. 2 = also per-frame evaluate line + Streamline info messages" );

idCVar r_pathTracingWaitForIdleOnPortalChange(
    "r_pathTracingWaitForIdleOnPortalChange",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Diagnostic: wait for GPU idle before replacing committed PT scene resources when portal/scene membership changes" );

idCVar r_pathTracingSceneRetireFrames(
    "r_pathTracingSceneRetireFrames",
    "6",
    CVAR_RENDERER | CVAR_INTEGER,
    "Minimum frames to retain replaced PT scene packages after their exact graphics submission completes; clamped 0..32" );

idCVar r_pathTracingSkipRaster3D(
    "r_pathTracingSkipRaster3D",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Skip the normal 3D raster view after RT smoke/path tracing has produced its debug output; GUI views still render" );

idCVar r_pathTracingReadbackEnable(
    "r_pathTracingReadbackEnable",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Enable RT smoke UAV readback diagnostics; can stall the GPU/CPU while profiling" );

idCVar r_pathTracingMaterialCache(
    "r_pathTracingMaterialCache",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Cache the universe-derived RT smoke active material table when the material/signature inputs are unchanged" );

idCVar r_pathTracingMaterialUniverseTable(
    "r_pathTracingMaterialUniverseTable",
    "1",
    CVAR_RENDERER | CVAR_INTEGER,
    "Build the RT smoke frame material table from stable material-universe records instead of the legacy active order; default on" );

idCVar r_pathTracingGeometryUniverseValidate(
    "r_pathTracingGeometryUniverseValidate",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Run expensive RT smoke geometry-universe static record validation; reports validate=total/range/duplicate/history/keyVector" );

idCVar r_pathTracingGeometryUniverseRangeDump(
    "r_pathTracingGeometryUniverseRangeDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to dump one-shot RT smoke geometry-universe static current/previous range records" );

idCVar r_pathTracingStaticContractDump(
    "r_pathTracingStaticContractDump",
    "0",
    CVAR_RENDERER | CVAR_INTEGER,
    "Set to 1 to trace the center view ray and dump one-shot static producer/storage/upload/BLAS/TLAS contract diagnostics" );
