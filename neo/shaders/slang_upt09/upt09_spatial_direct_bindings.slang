import upt03_reservoir_contract;
import upt04_light_contract;
import upt04_primary_surface_live_contract;
import upt04_packed_primary_surface32;
import upt04_primary_history_sidecar;

#ifndef UPT09_PACKED_LIGHT_RECORD
#define UPT09_PACKED_LIGHT_RECORD 0
#endif
#ifndef UPT42_REUSE_TEXTURE_PAIRING
#define UPT42_REUSE_TEXTURE_PAIRING 0
#endif
#ifndef UPT43_SPATIAL_SHIFT_PREPASS
#define UPT43_SPATIAL_SHIFT_PREPASS 0
#endif
#ifndef UPT43_SPATIAL_RESAMPLE
#define UPT43_SPATIAL_RESAMPLE 0
#endif
#ifndef UPT438_SPATIAL_BOOST_CLASSIFY
#define UPT438_SPATIAL_BOOST_CLASSIFY 0
#endif
#ifndef UPT438_SPATIAL_BOOST_COMPACT
#define UPT438_SPATIAL_BOOST_COMPACT 0
#endif

public struct Upt09SpatialDirectControl
{
    public uint renderWidth;
    public uint renderHeight;
    public uint surfaceCount;
    public uint frameSampleIndex;
    public uint emissiveRangeStart;
    public uint emissiveRangeCount;
    public uint analyticRangeStart;
    public uint analyticRangeCount;
    public uint currentLightCount;
    public uint maximumInputM;
    public uint regularNeighborCount;
    public uint rescueNeighborCount;
    public float neighborRadius;
    public uint geometryAvailabilityFlags;
    public float emissiveScale;
    public uint proofMode;
    public float3 primaryCameraOrigin;
    public uint compactPrimaryHistory;
    public uint materialCount;
    public uint logicalTextureCount;
    public uint emissiveDistributionCountAndValid;
    public uint emissiveLookupCapacityAndValid;
};

public [[vk::binding(0, 0)]] RaytracingAccelerationStructure gUpt09Scene;
public [[vk::binding(1, 0)]] StructuredBuffer<Upt04PackedPrimarySurfaceRecord32> gUpt09Surfaces;
public [[vk::binding(2, 0)]] StructuredBuffer<Upt03PackedReservoir> gUpt09InputReservoirs;
public [[vk::binding(3, 0)]] RWStructuredBuffer<Upt03PackedReservoir> gUpt09OutputReservoirs;
#if UPT09_PACKED_LIGHT_RECORD
public [[vk::binding(4, 0)]] StructuredBuffer<Upt04CompactLightRecord> gUpt09CurrentLights;
#else
public [[vk::binding(4, 0)]] StructuredBuffer<Upt04UnifiedLightRecord> gUpt09CurrentLights;
#endif
public [[vk::binding(5, 0)]] StructuredBuffer<Upt04PrimaryHistorySidecar> gUpt09SurfaceSidecars;
#if UPT42_REUSE_TEXTURE_PAIRING
// Three immutable, concatenated self-inverting delta fields with extents
// 254, 230 and 210, followed by one canonical endpoint per pair. Each delta
// uint packs signed int16 dx/dy. Production UPT-42 consumes the first field;
// the other independent fields and pair lists are ready for a future
// simultaneous three-neighbor resampling pass without regeneration.
public [[vk::binding(29, 0)]] StructuredBuffer<uint> gUpt09ReuseTextureDeltas;
#endif
#if UPT43_SPATIAL_SHIFT_PREPASS || UPT43_SPATIAL_RESAMPLE
// sourceTarget, targetTarget, shiftJacobian, packed status/route. Every pixel
// overwrites its own record before the resampling pass; no clear is required.
public [[vk::binding(30, 0)]] RWStructuredBuffer<float4> gUpt43SpatialShifts;
#endif
#if UPT438_SPATIAL_BOOST_CLASSIFY
public [[vk::binding(33, 0)]] RWStructuredBuffer<uint> gUpt438BoostQueue;
public [[vk::binding(34, 0)]] RWStructuredBuffer<uint> gUpt438BoostMeta;
public [[vk::binding(35, 0)]] RWStructuredBuffer<uint> gUpt438BoostDispatchArgs;
#elif UPT438_SPATIAL_BOOST_COMPACT
public [[vk::binding(33, 0)]] StructuredBuffer<uint> gUpt438BoostQueue;
public [[vk::binding(34, 0)]] StructuredBuffer<uint> gUpt438BoostMeta;
#endif
public [[vk::push_constant]] ConstantBuffer<Upt09SpatialDirectControl> gUpt09Control;

public Upt04UnifiedLightRecord Upt09LoadLight(uint index)
{
#if UPT09_PACKED_LIGHT_RECORD
    return Upt04UnpackCompactLight(gUpt09CurrentLights[index]);
#else
    return gUpt09CurrentLights[index];
#endif
}

public Upt04PrimarySurfaceRecord Upt09LoadSurface(uint index)
{
    return Upt04ApplyPrimaryHistorySidecar(
        Upt04UnpackPrimarySurface32(
            gUpt09Surfaces[index],
            gUpt09Control.primaryCameraOrigin),
        gUpt09SurfaceSidecars[index]);
}
