import upt03_reservoir_contract;
import upt04_light_contract;
import upt04_primary_surface_live_contract;
import upt04_packed_primary_surface32;
import upt04_primary_history_sidecar;
import upt07_temporal_surface_contract;

#ifndef UPT07_PACKED_LIGHT_RECORD
#define UPT07_PACKED_LIGHT_RECORD 0
#endif
#ifndef UPT07_DUPLICATION_MAP
#define UPT07_DUPLICATION_MAP 0
#endif
#ifndef UPT07_EARLY_RECONNECT
#define UPT07_EARLY_RECONNECT 0
#endif
#ifndef UPT07_INDIRECT_REPLAY
#define UPT07_INDIRECT_REPLAY 0
#endif
#ifndef UPT07_ROUTE_DIAGNOSTICS
#define UPT07_ROUTE_DIAGNOSTICS 0
#endif
#ifndef UPT07_BOTTLENECK_STAGE
#define UPT07_BOTTLENECK_STAGE 0
#endif

public struct Upt07TemporalDirectControl
{
    public uint renderWidth;
    public uint renderHeight;
    public uint surfaceCount;
    public uint historyAvailable;
    public uint emissiveRangeStart;
    public uint emissiveRangeCount;
    public uint analyticRangeStart;
    public uint analyticRangeCount;
    public uint currentLightCount;
    public uint frameSampleIndex;
    public uint maximumHistoryM;
    public uint maximumHistoryAge;
    public Upt07PreviousCamera previousCamera;
    public uint geometryAvailabilityFlags;
    public float emissiveScale;
    public uint previousToCurrentLightCount;
    public uint maximumHistoryContributionRatio;
    public float3 primaryCameraOrigin;
    public uint compactPrimaryHistory;
    public uint materialCount;
    public uint logicalTextureCount;
    public uint emissiveDistributionCountAndValid;
    public uint emissiveLookupCapacityAndValid;
};

public [[vk::binding(0, 0)]] RaytracingAccelerationStructure gUpt07Scene;
public [[vk::binding(1, 0)]] StructuredBuffer<Upt04PackedPrimarySurfaceRecord32> gUpt07CurrentSurfaces;
public [[vk::binding(2, 0)]] StructuredBuffer<Upt04PackedPrimarySurfaceRecord32> gUpt07PreviousSurfaces;
public [[vk::binding(3, 0)]] RWStructuredBuffer<Upt03PackedReservoir> gUpt07CurrentReservoirs;
public [[vk::binding(4, 0)]] StructuredBuffer<Upt03PackedReservoir> gUpt07HistoryReservoirs;
#if UPT07_PACKED_LIGHT_RECORD
public [[vk::binding(5, 0)]] StructuredBuffer<Upt04CompactLightRecord> gUpt07CurrentLights;
#else
public [[vk::binding(5, 0)]] StructuredBuffer<Upt04UnifiedLightRecord> gUpt07CurrentLights;
#endif
public [[vk::binding(23, 0)]] StructuredBuffer<uint> gUpt07PreviousToCurrentLights;
#if UPT07_DUPLICATION_MAP
public [[vk::binding(24, 0)]] StructuredBuffer<uint> gUpt07HistoryDuplication;
#endif
public [[vk::binding(25, 0)]] StructuredBuffer<Upt04PrimaryHistorySidecar> gUpt07CurrentSurfaceSidecars;
public [[vk::binding(26, 0)]] StructuredBuffer<Upt04PrimaryHistorySidecar> gUpt07PreviousSurfaceSidecars;
#if UPT07_INDIRECT_REPLAY && (UPT07_EARLY_RECONNECT || UPT07_ROUTE_DIAGNOSTICS)
public [[vk::binding(30, 0)]] RWStructuredBuffer<uint> gUpt07ReconnectDiagnostics;
#endif
#if UPT07_BOTTLENECK_STAGE > 0
public [[vk::binding(31, 0)]] RWStructuredBuffer<uint> gUpt07BottleneckProbe;
#endif
public [[vk::push_constant]] ConstantBuffer<Upt07TemporalDirectControl> gUpt07Control;

public Upt04UnifiedLightRecord Upt07LoadLight(uint index)
{
#if UPT07_PACKED_LIGHT_RECORD
    return Upt04UnpackCompactLight(gUpt07CurrentLights[index]);
#else
    return gUpt07CurrentLights[index];
#endif
}

public Upt04PrimarySurfaceRecord Upt07LoadCurrentSurface(uint index)
{
    return Upt04ApplyPrimaryHistorySidecar(
        Upt04UnpackPrimarySurface32(
            gUpt07CurrentSurfaces[index],
            gUpt07Control.primaryCameraOrigin),
        gUpt07CurrentSurfaceSidecars[index]);
}

public Upt04PrimarySurfaceRecord Upt07LoadPreviousSurface(uint index)
{
    return Upt04ApplyPrimaryHistorySidecar(
        Upt04UnpackPrimarySurface32(
            gUpt07PreviousSurfaces[index],
            gUpt07Control.previousCamera.origin),
        gUpt07PreviousSurfaceSidecars[index]);
}
