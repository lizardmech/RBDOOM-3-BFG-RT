import upt03_reservoir_contract;
import upt04_primary_surface_live_contract;
import upt04_light_contract;
import upt04_material_live_contract;
import upt04_initial_control;
#ifndef UPT04_PACKED_PRIMARY_RECEIVER
#define UPT04_PACKED_PRIMARY_RECEIVER 0
#endif
#ifndef UPT04_PACKED_LIGHT_RECORD
#define UPT04_PACKED_LIGHT_RECORD 0
#endif
#if UPT04_PACKED_PRIMARY_RECEIVER == 1
import upt04_packed_primary_surface;
#elif UPT04_PACKED_PRIMARY_RECEIVER == 2
import upt04_packed_primary_surface32;
#endif

public [[vk::binding(0, 0)]] RaytracingAccelerationStructure gUpt04Scene;
#if UPT04_PACKED_PRIMARY_RECEIVER == 1
public [[vk::binding(1, 0)]] StructuredBuffer<Upt04PackedPrimarySurfaceRecord> gUpt04PrimarySurfaces;
#elif UPT04_PACKED_PRIMARY_RECEIVER == 2
public [[vk::binding(1, 0)]] StructuredBuffer<Upt04PackedPrimarySurfaceRecord32> gUpt04PrimarySurfaces;
#else
public [[vk::binding(1, 0)]] StructuredBuffer<Upt04PrimarySurfaceRecord> gUpt04PrimarySurfaces;
#endif
#if UPT04_PACKED_LIGHT_RECORD
public [[vk::binding(2, 0)]] StructuredBuffer<Upt04CompactLightRecord> gUpt04CurrentLights;
#else
public [[vk::binding(2, 0)]] StructuredBuffer<Upt04UnifiedLightRecord> gUpt04CurrentLights;
#endif
public [[vk::binding(3, 0)]] StructuredBuffer<Upt04SmokeMaterialRecord> gUpt04Materials;
public [[vk::binding(4, 0)]] RWStructuredBuffer<Upt03PackedReservoir> gUpt04Reservoirs;
public [[vk::binding(5, 0)]] SamplerState gUpt04MaterialSampler;
public [[vk::binding(0, 1)]] Texture2D<float4> gUpt04MaterialTextures[];
public [[vk::push_constant]] ConstantBuffer<Upt04InitialControl> gUpt04Control;

public Upt04PrimarySurfaceRecord Upt04LoadPrimarySurfaceRecord(uint index)
{
#if UPT04_PACKED_PRIMARY_RECEIVER == 1
    return Upt04UnpackPrimarySurface(gUpt04PrimarySurfaces[index]);
#elif UPT04_PACKED_PRIMARY_RECEIVER == 2
    return Upt04UnpackPrimarySurface32(
        gUpt04PrimarySurfaces[index],
        float3(
            gUpt04Control.primaryCameraOriginX,
            gUpt04Control.primaryCameraOriginY,
            gUpt04Control.primaryCameraOriginZ));
#else
    return gUpt04PrimarySurfaces[index];
#endif
}

public Upt04UnifiedLightRecord Upt04LoadCurrentLightRecord(uint index)
{
#if UPT04_PACKED_LIGHT_RECORD
    return Upt04UnpackCompactLight(gUpt04CurrentLights[index]);
#else
    return gUpt04CurrentLights[index];
#endif
}
