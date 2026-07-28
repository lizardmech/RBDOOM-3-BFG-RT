#define CLEAN_RTXDI_DI_GLASS_ENTRY 1
// GEO-10 REF-10B view-16 post-hit material consumer. CPU admission remains
// route-2, diagnostic-marker gated, and clean-GI-off.
#define RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS 1
#include "pathtrace_clean_rtxdi_di_shared.hlsli"
#include "pathtrace_clean_rtxdi_di_math.hlsli"
#include "pathtrace_clean_rtxdi_di_material_adapter.hlsli"
#include "pathtrace_clean_rtxdi_di_glass_feature.hlsli"
#include "pathtrace_clean_rtxdi_di_glass_params.hlsli"
#include "pathtrace_clean_rtxdi_di_glass_math.hlsli"
#include "pathtrace_clean_rtxdi_di_glass_compose.hlsli"
#include "pathtrace_clean_rtxdi_di_transmission_sidecar.hlsli"
#include "pathtrace_clean_rtxdi_di_reflection_sidecar.hlsli"
#include "pathtrace_clean_rtxdi_di_smoke_exports.hlsli"
#include "pathtrace_clean_rtxdi_di_glass.hlsli"
