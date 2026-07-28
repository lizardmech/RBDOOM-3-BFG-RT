#define CLEAN_RTXDI_DI_TRANSMISSION_PRODUCER_ENTRY 1
#define CLEAN_RTXDI_DI_TRACE_HIT_SURFACE_ADAPTER 1
#define CLEAN_RTXDI_DI_TRANSMISSION_PSR_TRANSPORT 1
// GEO-10 REF-10 compile-only validation of the existing 176-byte hit payload.
// CPU cutover remains closed for clean-DI view 16.
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
#include "../cleanroom_common/pathtrace_liquid_pool_modifier.hlsli"
#include "pathtrace_clean_rtxdi_di_transmission_probe.hlsli"
#include "pathtrace_clean_rtxdi_di_smoke_exports.hlsli"
#include "pathtrace_clean_rtxdi_di_hit_surface_adapter.hlsli"
#include "pathtrace_clean_rtxdi_di_transmission_transport.hlsli"
#include "pathtrace_clean_rtxdi_di_transmission_producer.hlsli"
