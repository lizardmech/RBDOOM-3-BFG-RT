// Clean-room ReSTIR GI temporal reuse compute entrypoint.
//
// First optimization slice for the specular-on GI path: run the temporal
// reservoir contract as compute instead of DXR raygen when the normal beauty
// path has spatial reuse enabled. Other views stay on the RT fallback.

// Root dependency marker: temporal reservoir selection stays white-noise.
#include "pathtrace_clean_restir_gi.rt.hlsl"

static const uint CLEAN_GI_TEMPORAL_GROUP_SIZE_X = 16u;
static const uint CLEAN_GI_TEMPORAL_GROUP_SIZE_Y = 8u;

[numthreads(CLEAN_GI_TEMPORAL_GROUP_SIZE_X, CLEAN_GI_TEMPORAL_GROUP_SIZE_Y, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint2 dimensions = uint2(CleanRtxdiDiWidth, CleanRtxdiDiHeight);
    if (dimensions.x == 0u || dimensions.y == 0u ||
        pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    if (CleanRestirGiView != 0u || CleanRestirGiPhase != 0u ||
        CleanRestirGiSpatialEnabled == 0u)
    {
        return;
    }

    PathTracePrimarySurfaceRecord record;
    const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);
    CleanGiRunTemporalContract(pixel, dimensions, surfaceValid, record);
}
