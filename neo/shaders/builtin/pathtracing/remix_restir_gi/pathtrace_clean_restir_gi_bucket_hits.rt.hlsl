// GEO-10 compact checked static-bucket hit library for clean ReSTIR GI.
//
// Compile the accepted compact GI hit graph with bucket-local reconstruction
// and distinct exports. The large split raygen libraries remain unchanged.
#define CLEAN_GI_STATIC_BUCKET_HITS 1
#define CLEAN_GI_HIT_ANY_EXPORT CleanGiBucketAnyHit
#define CLEAN_GI_HIT_SHADOW_ANY_EXPORT CleanGiBucketShadowAnyHit
#define CLEAN_GI_HIT_CLOSEST_EXPORT CleanGiBucketClosestHit
#define CLEAN_GI_HIT_SHADOW_CLOSEST_EXPORT CleanGiBucketShadowClosestHit
#include "pathtrace_clean_restir_gi_skinned_hits.rt.hlsl"
