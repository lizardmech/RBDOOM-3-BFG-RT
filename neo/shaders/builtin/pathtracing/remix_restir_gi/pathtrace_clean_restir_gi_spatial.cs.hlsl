// Trace-free production GI spatial reuse compute entrypoint.
//
// This executes the same rbdoom-owned spatial reservoir contract as
// SpatialReuseOnlyRayGen without attaching the renderer's RT hit groups or
// ray-pipeline stack. The host selects it only for view 0 with spatial
// visibility disabled.

#define CLEAN_GI_SPATIAL_REUSE_TRACE_FREE 1
#include "pathtrace_clean_restir_gi.rt.hlsl"

static const uint CLEAN_GI_SPATIAL_GROUP_SIZE_X = 16u;
static const uint CLEAN_GI_SPATIAL_GROUP_SIZE_Y = 8u;

[numthreads(CLEAN_GI_SPATIAL_GROUP_SIZE_X, CLEAN_GI_SPATIAL_GROUP_SIZE_Y, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint2 dimensions = uint2(CleanRtxdiDiWidth, CleanRtxdiDiHeight);
    if (dimensions.x == 0u || dimensions.y == 0u ||
        pixel.x >= dimensions.x || pixel.y >= dimensions.y ||
        CleanRestirGiView != 0u ||
        CleanRestirGiSpatialEnabled == 0u)
    {
        return;
    }

    PathTracePrimarySurfaceRecord record;
    const bool surfaceValid = CleanGiLoadSurfaceRecord(pixel, dimensions, record);
    RAB_Surface surface = RAB_EmptySurface();
    if (surfaceValid)
    {
        surface = CleanGiMaterialSurfaceFromCurrentRecord(pixel, record);
    }

    const RTXDI_GIReservoir spatialInput = RAB_LoadGIReservoir(
        int2(pixel),
        int(RemixRAB_GetGITemporalOutputReservoirIndex()));
    uint4 spatialDebugStats;
    const RTXDI_GIReservoir spatialReservoir = CleanGiRunSpatialReuse(
        pixel,
        surface,
        spatialInput,
        spatialDebugStats);
    RAB_StoreGIReservoir(
        spatialReservoir,
        int2(pixel),
        int(RemixRAB_GetGISpatialOutputReservoirIndex()));
}
