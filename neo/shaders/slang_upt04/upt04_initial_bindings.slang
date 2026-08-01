import upt03_reservoir_contract;
import upt04_primary_surface_live_contract;
import upt04_light_contract;
import upt04_material_live_contract;
import upt04_initial_control;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure gUpt04Scene;
[[vk::binding(1, 0)]] StructuredBuffer<Upt04PrimarySurfaceRecord> gUpt04PrimarySurfaces;
[[vk::binding(2, 0)]] StructuredBuffer<Upt04UnifiedLightRecord> gUpt04CurrentLights;
[[vk::binding(3, 0)]] StructuredBuffer<Upt04SmokeMaterialRecord> gUpt04Materials;
[[vk::binding(4, 0)]] RWStructuredBuffer<Upt03PackedReservoir> gUpt04Reservoirs;
[[vk::binding(5, 0)]] SamplerState gUpt04MaterialSampler;
[[vk::binding(0, 1)]] Texture2D<float4> gUpt04MaterialTextures[];
[[vk::push_constant]] ConstantBuffer<Upt04InitialControl> gUpt04Control;
