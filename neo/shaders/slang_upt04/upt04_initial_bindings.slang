import upt03_reservoir_contract;
import upt04_primary_surface_live_contract;
import upt04_light_contract;
import upt04_light_tile_contract;
import upt04_material_live_contract;
import upt04_initial_control;
import upt04_primary_history_sidecar;
#ifndef UPT04_PACKED_PRIMARY_RECEIVER
#define UPT04_PACKED_PRIMARY_RECEIVER 0
#endif
#ifndef UPT04_PACKED_LIGHT_RECORD
#define UPT04_PACKED_LIGHT_RECORD 0
#endif
#ifndef UPT04_PACKED_MATERIAL_RECORD
#define UPT04_PACKED_MATERIAL_RECORD 0
#endif
#ifndef UPT04_NATIVE_LIGHT_UINT4_VIEW
#define UPT04_NATIVE_LIGHT_UINT4_VIEW 0
#endif
#ifndef UPT04_LIGHT_TILES
#define UPT04_LIGHT_TILES 0
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
#if UPT04_NATIVE_LIGHT_UINT4_VIEW
public [[vk::binding(2, 0)]] StructuredBuffer<uint4> gUpt04CurrentLights;
#elif UPT04_PACKED_LIGHT_RECORD
public [[vk::binding(2, 0)]] StructuredBuffer<Upt04CompactLightRecord> gUpt04CurrentLights;
#else
public [[vk::binding(2, 0)]] StructuredBuffer<Upt04UnifiedLightRecord> gUpt04CurrentLights;
#endif
#if UPT04_PACKED_MATERIAL_RECORD
public [[vk::binding(3, 0)]] StructuredBuffer<Upt04CompactMaterialRecord> gUpt04Materials;
#else
public [[vk::binding(3, 0)]] StructuredBuffer<Upt04SmokeMaterialRecord> gUpt04Materials;
#endif
public [[vk::binding(4, 0)]] RWStructuredBuffer<Upt03PackedReservoir> gUpt04Reservoirs;
public [[vk::binding(5, 0)]] SamplerState gUpt04MaterialSampler;
public [[vk::binding(25, 0)]] StructuredBuffer<Upt04EmissiveDistributionEntry>
    gUpt04EmissiveDistribution;
#if UPT04_PACKED_PRIMARY_RECEIVER == 1
public [[vk::binding(26, 0)]] StructuredBuffer<Upt04PackedPrimarySurfaceRecord>
    gUpt04PreviousPrimarySurfaces;
#elif UPT04_PACKED_PRIMARY_RECEIVER == 2
public [[vk::binding(26, 0)]] StructuredBuffer<Upt04PackedPrimarySurfaceRecord32>
    gUpt04PreviousPrimarySurfaces;
#else
public [[vk::binding(26, 0)]] StructuredBuffer<Upt04PrimarySurfaceRecord>
    gUpt04PreviousPrimarySurfaces;
#endif
public [[vk::binding(27, 0)]] StructuredBuffer<Upt04PrimaryHistorySidecar>
    gUpt04PreviousPrimarySidecars;
public [[vk::binding(28, 0)]] StructuredBuffer<Upt03PackedReservoir>
    gUpt04PreviousReservoirs;
public [[vk::binding(29, 0)]] StructuredBuffer<uint>
    gUpt04PreviousToCurrentLights;
#if UPT04_LIGHT_TILES
public [[vk::binding(30, 0)]] StructuredBuffer<Upt04LightTileEntry>
    gUpt04LightTiles;
#endif
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
#if UPT04_NATIVE_LIGHT_UINT4_VIEW
    const uint chunkBase = index * 7u;
    const uint4 chunk0 = gUpt04CurrentLights[chunkBase + 0u];
    const uint4 chunk1 = gUpt04CurrentLights[chunkBase + 1u];
    const uint4 chunk2 = gUpt04CurrentLights[chunkBase + 2u];
    const uint4 chunk3 = gUpt04CurrentLights[chunkBase + 3u];
    const uint4 chunk4 = gUpt04CurrentLights[chunkBase + 4u];
    const uint4 chunk5 = gUpt04CurrentLights[chunkBase + 5u];
    const uint4 chunk6 = gUpt04CurrentLights[chunkBase + 6u];
    Upt04UnifiedLightRecord light = (Upt04UnifiedLightRecord)0;
    light.positionAndRadius = asfloat(chunk0);
    light.normalAndArea = asfloat(chunk1);
    light.radianceAndLuminance = asfloat(chunk2);
    light.uvOrDoomParams = asfloat(chunk3);
    light.type = chunk4.x;
    light.sourceIndex = chunk4.y;
    light.flags = chunk4.z;
    light.materialOrLightId = chunk4.w;
    light.instanceId = chunk5.x;
    light.primitiveIndex = chunk5.y;
    light.identityA = chunk5.z;
    light.identityB = chunk5.w;
    light.sourcePdf = asfloat(chunk6.x);
    light.sourceWeight = asfloat(chunk6.y);
    light.previousIndex = chunk6.z;
    light.padding0 = chunk6.w;
    return light;
#elif UPT04_PACKED_LIGHT_RECORD
    return Upt04UnpackCompactLight(gUpt04CurrentLights[index]);
#else
    return gUpt04CurrentLights[index];
#endif
}

// Native 112-byte records are authoritative, but returning the complete
// structure makes Slang issue one eager whole-record load before it knows
// whether the candidate is emissive or analytic.  Proposal parity evaluates
// many analytic candidates after one emissive candidate, so build the exact
// type-specific evaluation value from member loads instead.  The compact64
// route already has one deliberately packed record and keeps its old decode.
public Upt04UnifiedLightRecord Upt04LoadCurrentLightEvaluationRecord(
    uint index,
    uint selectedType)
{
#if UPT04_NATIVE_LIGHT_UINT4_VIEW
    const uint chunkBase = index * 7u;
    const uint4 chunk0 = gUpt04CurrentLights[chunkBase + 0u];
    const uint4 chunk2 = gUpt04CurrentLights[chunkBase + 2u];
    const uint4 chunk4 = gUpt04CurrentLights[chunkBase + 4u];
    Upt04UnifiedLightRecord light = (Upt04UnifiedLightRecord)0;
    light.positionAndRadius = asfloat(chunk0);
    light.radianceAndLuminance = asfloat(chunk2);
    light.type = chunk4.x;
    light.sourceIndex = chunk4.y;
    if (selectedType == kUpt04LightEmissiveTriangle)
    {
        const uint4 chunk1 = gUpt04CurrentLights[chunkBase + 1u];
        const uint4 chunk5 = gUpt04CurrentLights[chunkBase + 5u];
        const uint4 chunk6 = gUpt04CurrentLights[chunkBase + 6u];
        light.normalAndArea = asfloat(chunk1);
        light.materialOrLightId = chunk4.w;
        light.instanceId = chunk5.x;
        light.primitiveIndex = chunk5.y;
        light.identityA = chunk5.z;
        light.identityB = chunk5.w;
        light.sourceWeight = asfloat(chunk6.y);
    }
    else
    {
        const uint4 chunk3 = gUpt04CurrentLights[chunkBase + 3u];
        light.uvOrDoomParams = asfloat(chunk3);
        light.flags = chunk4.z;
    }
    return light;
#elif UPT04_PACKED_LIGHT_RECORD
    return Upt04UnpackCompactLight(gUpt04CurrentLights[index]);
#else
    return gUpt04CurrentLights[index];
#endif
}

public Upt04UnifiedLightRecord Upt04LoadCurrentLightIdentityRecord(uint index)
{
#if UPT04_NATIVE_LIGHT_UINT4_VIEW
    const uint chunkBase = index * 7u;
    const uint4 chunk4 = gUpt04CurrentLights[chunkBase + 4u];
    const uint4 chunk5 = gUpt04CurrentLights[chunkBase + 5u];
    Upt04UnifiedLightRecord light = (Upt04UnifiedLightRecord)0;
    light.type = chunk4.x;
    light.materialOrLightId = chunk4.w;
    light.identityA = chunk5.z;
    light.identityB = chunk5.w;
    return light;
#elif UPT04_PACKED_LIGHT_RECORD
    return Upt04UnpackCompactLight(gUpt04CurrentLights[index]);
#else
    return gUpt04CurrentLights[index];
#endif
}

public Upt04UnifiedLightRecord Upt04LoadCurrentLightVisibilityRecord(uint index)
{
#if UPT04_NATIVE_LIGHT_UINT4_VIEW
    const uint chunkBase = index * 7u;
    const uint4 chunk4 = gUpt04CurrentLights[chunkBase + 4u];
    const uint4 chunk5 = gUpt04CurrentLights[chunkBase + 5u];
    Upt04UnifiedLightRecord light = (Upt04UnifiedLightRecord)0;
    light.type = chunk4.x;
    light.instanceId = chunk5.x;
    light.primitiveIndex = chunk5.y;
    return light;
#elif UPT04_PACKED_LIGHT_RECORD
    return Upt04UnpackCompactLight(gUpt04CurrentLights[index]);
#else
    return gUpt04CurrentLights[index];
#endif
}

public Upt04SmokeMaterialRecord Upt04LoadMaterialRecord(uint index)
{
#if UPT04_PACKED_MATERIAL_RECORD
    return Upt04UnpackCompactMaterial(gUpt04Materials[index]);
#else
    return gUpt04Materials[index];
#endif
}

// Alpha traversal usually needs only the classification bit. Keep this as a
// member load so natural records do not inflate into an eager 112-byte read,
// and compact records touch only their flags-containing uint4.
public uint Upt04LoadMaterialFlags(uint index)
{
#if UPT04_PACKED_MATERIAL_RECORD
    return gUpt04Materials[index].textureIndicesAndFlags.w;
#else
    return gUpt04Materials[index].flags;
#endif
}

public Upt04PrimarySurfaceRecord Upt04LoadPreviousPrimarySurface(uint index)
{
#if UPT04_PACKED_PRIMARY_RECEIVER == 1
    // Compact48 is a current-frame-only receiver. Host metadata keeps D0
    // history disabled for this specialization, so this declaration exists
    // only to retain one descriptor layout across production variants.
    return Upt04UnpackPrimarySurface(gUpt04PreviousPrimarySurfaces[index]);
#elif UPT04_PACKED_PRIMARY_RECEIVER == 2
    return Upt04ApplyPrimaryHistorySidecar(
        Upt04UnpackPrimarySurface32(
            gUpt04PreviousPrimarySurfaces[index],
            float3(
                gUpt04Control.previousCameraOriginX,
                gUpt04Control.previousCameraOriginY,
                gUpt04Control.previousCameraOriginZ)),
        gUpt04PreviousPrimarySidecars[index]);
#else
    return gUpt04PreviousPrimarySurfaces[index];
#endif
}
