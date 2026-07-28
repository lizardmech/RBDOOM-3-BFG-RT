#define CLEAN_RTXDI_DI_INITIAL_ENTRY 1
// GEO-10 REF-10 focused compile/decode checkpoint. CPU cutover still admits
// only the primary view-2 probe, so clean-DI traversal remains fail-closed.
#define RB_PT_ENABLE_STATIC_BUCKET_SHADER_CONSUMERS 1
#include "pathtrace_clean_rtxdi_di_sentinel.rt.hlsl"
