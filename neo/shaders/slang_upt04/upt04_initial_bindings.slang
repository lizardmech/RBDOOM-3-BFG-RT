import upt03_reservoir_contract;
import upt04_primary_surface_live_contract;
import upt04_light_contract;
import upt04_material_live_contract;
import upt04_initial_control;
#ifndef UPT04_PACKED_PRIMARY_RECEIVER
#define UPT04_PACKED_PRIMARY_RECEIVER 0
#endif
#if UPT04_PACKED_PRIMARY_RECEIVER
import upt04_packed_primary_surface;
#endif

public [[vk::binding(0, 0)]] RaytracingAccelerationStructure gUpt04Scene;
#if UPT04_PACKED_PRIMARY_RECEIVER
public [[vk::binding(1, 0)]] StructuredBuffer<Upt04PackedPrimarySurfaceRecord> gUpt04PrimarySurfaces;
#else
public [[vk::binding(1, 0)]] StructuredBuffer<Upt04PrimarySurfaceRecord> gUpt04PrimarySurfaces;
#endif
public [[vk::binding(2, 0)]] StructuredBuffer<Upt04UnifiedLightRecord> gUpt04CurrentLights;
public [[vk::binding(3, 0)]] StructuredBuffer<Upt04SmokeMaterialRecord> gUpt04Materials;
public [[vk::binding(4, 0)]] RWStructuredBuffer<Upt03PackedReservoir> gUpt04Reservoirs;
public [[vk::binding(5, 0)]] SamplerState gUpt04MaterialSampler;
public [[vk::binding(0, 1)]] Texture2D<float4> gUpt04MaterialTextures[];
public [[vk::push_constant]] ConstantBuffer<Upt04InitialControl> gUpt04Control;

public Upt04PrimarySurfaceRecord Upt04LoadPrimarySurfaceRecord(uint index)
{
#if UPT04_PACKED_PRIMARY_RECEIVER
    return Upt04UnpackPrimarySurface(gUpt04PrimarySurfaces[index]);
#else
    return gUpt04PrimarySurfaces[index];
#endif
}
