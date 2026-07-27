// GEO-10 compact extended-payload hit library for clean-DI material features.
//
// Glass and transmission keep their stable large shader blobs. This source
// compiles only the hit/miss export graph required by their ray-mode-3
// payload, including checked bucket identity, alpha/transmission traversal,
// emissive-card passthrough, and liquid-pool candidate transport.

#define CLEAN_RTXDI_DI_TRACE_HIT_SURFACE_ADAPTER 1
#define CLEAN_RTXDI_DI_TRANSMISSION_PSR_TRANSPORT 1
#define RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS 1
#include "pathtrace_clean_rtxdi_di_shared.hlsli"
#undef RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS
#include "pathtrace_clean_rtxdi_di_math.hlsli"
#include "pathtrace_clean_rtxdi_di_material_adapter.hlsli"
#include "pathtrace_clean_rtxdi_di_glass_feature.hlsli"
#include "pathtrace_clean_rtxdi_di_glass_params.hlsli"
#include "pathtrace_clean_rtxdi_di_glass_math.hlsli"
#include "pathtrace_clean_rtxdi_di_glass_compose.hlsli"
#include "pathtrace_clean_rtxdi_di_transmission_sidecar.hlsli"
#include "pathtrace_clean_rtxdi_di_reflection_sidecar.hlsli"
#include "../cleanroom_common/pathtrace_liquid_pool_modifier.hlsli"

#define Miss CleanDiMaterialBucketMiss
#define ShadowMiss CleanDiMaterialBucketShadowMiss
#define AnyHit CleanDiMaterialBucketAnyHit
#define ShadowAnyHit CleanDiMaterialBucketShadowAnyHit
#define ClosestHit CleanDiMaterialBucketClosestHit
#define ShadowClosestHit CleanDiMaterialBucketShadowClosestHit
#include "pathtrace_clean_rtxdi_di_smoke_exports.hlsli"
#undef ShadowClosestHit
#undef ClosestHit
#undef ShadowAnyHit
#undef AnyHit
#undef ShadowMiss
#undef Miss

#include "pathtrace_clean_rtxdi_di_hit_surface_adapter.hlsli"
