#ifndef RB_PATH_TRACE_MATERIAL_CLASSIFIER_HLSLI
#define RB_PATH_TRACE_MATERIAL_CLASSIFIER_HLSLI

static const uint RT_MATCLASS_ROUTE_REAL_PBR_RMAO = 1u;
static const uint RT_MATCLASS_ROUTE_LEGACY_SPEC_GLOSS = 2u;
static const uint RT_MATCLASS_ROUTE_SURFACE_TYPE_FALLBACK = 3u;
static const uint RT_MATCLASS_ROUTE_SHIFT = 10u;
static const uint RT_MATCLASS_ROUTE_MASK = 0x0fu;
static const uint RT_MATCLASS_NORMAL_DECODE_NONE = 0u;
static const uint RT_MATCLASS_NORMAL_DECODE_RGB8_RG = 1u;
static const uint RT_MATCLASS_NORMAL_DECODE_COMPRESSED_WY = 2u;
static const uint RT_MATCLASS_NORMAL_DECODE_SHIFT = 18u;
static const uint RT_MATCLASS_NORMAL_DECODE_MASK = 0x03u;
static const uint RT_MATCLASS_SURFACE_CLASS_METAL = 1u;
static const uint RT_MATCLASS_SURFACE_CLASS_RICOCHET = 9u;
static const uint RT_MATCLASS_SURFACE_CLASS_SPECIAL = 10u;
static const uint RT_MATCLASS_EMISSIVE_INTENT = 0x02000000u;
static const uint RT_SMOKE_MATERIAL_OVERRIDE_FULL_METAL = 0x00001000u;
static const uint RT_SMOKE_MATERIAL_CLASSIFIER_DRIVE_LEGACY_SPEC = 0x00000002u;
static const uint RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_RUNTIME_REGS = 0x00000004u;
static const uint RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_COLOR = 0x00000008u;
static const uint RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_ALPHA = 0x00000010u;
static const uint RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_CONDITION = 0x00000020u;
static const uint RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_TEX_MATRIX = 0x00000040u;
static const uint RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_VIDEO = 0x00000080u;
static const uint RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_DECAL = 0x00000100u;
static const uint RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_GUI_RENDER = 0x00000200u;
static const uint RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_PROGRAM = 0x00000400u;
static const uint RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_FLIPBOOK = 0x00000800u;
static const uint RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_MASK = 0x00000ffcu;

uint SmokeMatClassRoute(PathTraceSmokeMaterial material)
{
    return (material.padding1 >> RT_MATCLASS_ROUTE_SHIFT) & RT_MATCLASS_ROUTE_MASK;
}

uint SmokeMatClassSurfaceClass(PathTraceSmokeMaterial material)
{
    return material.padding1 & 0x0fu;
}

uint SmokeMatClassNormalDecodeMode(PathTraceSmokeMaterial material)
{
    return (material.padding1 >> RT_MATCLASS_NORMAL_DECODE_SHIFT) & RT_MATCLASS_NORMAL_DECODE_MASK;
}

float SmokeMatClassUnpackUnit8(uint packed, uint shift)
{
    return (float((packed >> shift) & 0xffu)) * (1.0 / 255.0);
}

float SmokeMatClassRoughness(PathTraceSmokeMaterial material)
{
    return SmokeMatClassUnpackUnit8(material.padding2, 0u);
}

float SmokeMatClassMetallic(PathTraceSmokeMaterial material)
{
    return SmokeMatClassUnpackUnit8(material.padding2, 8u);
}

float SmokeMatClassTransmission(PathTraceSmokeMaterial material)
{
    return SmokeMatClassUnpackUnit8(material.padding2, 16u);
}

float SmokeMatClassF0(PathTraceSmokeMaterial material)
{
    return SmokeMatClassUnpackUnit8(material.padding2, 24u);
}

bool SmokeMatClassHasPackedBsdf(PathTraceSmokeMaterial material)
{
    const uint route = SmokeMatClassRoute(material);
    const uint surfaceClass = SmokeMatClassSurfaceClass(material);
    return route == RT_MATCLASS_ROUTE_SURFACE_TYPE_FALLBACK &&
        surfaceClass != RT_MATCLASS_SURFACE_CLASS_SPECIAL;
}

bool SmokeMatClassDrivesLegacySpec(PathTraceSmokeMaterial material)
{
    const uint route = SmokeMatClassRoute(material);
    const uint surfaceClass = SmokeMatClassSurfaceClass(material);
    return
        (material.padding0 & RT_SMOKE_MATERIAL_CLASSIFIER_DRIVE_LEGACY_SPEC) != 0u &&
        route == RT_MATCLASS_ROUTE_LEGACY_SPEC_GLOSS &&
        surfaceClass == RT_MATCLASS_SURFACE_CLASS_RICOCHET;
}

uint SmokeMatClassDynamicFlags(PathTraceSmokeMaterial material)
{
    return material.padding0 & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_MASK;
}

bool SmokeMatClassNeedsDynamicInstance(PathTraceSmokeMaterial material)
{
    return SmokeMatClassDynamicFlags(material) != 0u;
}

bool SmokeMaterialHasFullMetalOverride(PathTraceSmokeMaterial material)
{
    return (material.padding0 & RT_SMOKE_MATERIAL_OVERRIDE_FULL_METAL) != 0u;
}

void SmokeApplyFullMetalOverride(PathTraceSmokeMaterial material, inout float3 albedo, inout float3 specularF0)
{
    if (!SmokeMaterialHasFullMetalOverride(material))
    {
        return;
    }

    // Runtime full-metal keeps mirror energy high, but uses albedo chroma as
    // a reduced tint so dark legacy diffuse maps do not erase the reflection.
    const float maxAlbedo = max(albedo.r, max(albedo.g, albedo.b));
    const float3 albedoChroma = maxAlbedo > 1.0e-4
        ? saturate(albedo / maxAlbedo)
        : float3(1.0, 1.0, 1.0);
    const float3 neutralMirrorF0 = float3(0.85, 0.85, 0.85);
    const float tintStrength = 0.45;
    specularF0 = max(specularF0, neutralMirrorF0 * lerp(float3(1.0, 1.0, 1.0), albedoChroma, tintStrength));
    albedo = float3(0.0, 0.0, 0.0);
}

float3 SmokeMatClassDynamicDebugColor(PathTraceSmokeMaterial material)
{
    const uint dynamicFlags = SmokeMatClassDynamicFlags(material);
    if (dynamicFlags == 0u)
    {
        return float3(0.03, 0.03, 0.03);
    }

    return float3(
        (dynamicFlags & (RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_COLOR | RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_ALPHA | RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_CONDITION)) != 0u ? 1.0 : 0.0,
        (dynamicFlags & (RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_TEX_MATRIX | RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_FLIPBOOK | RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_VIDEO | RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_GUI_RENDER)) != 0u ? 1.0 : 0.0,
        (dynamicFlags & (RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_RUNTIME_REGS | RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_DECAL | RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_PROGRAM)) != 0u ? 1.0 : 0.0);
}

float2 SmokeMatClassNormalXY(PathTraceSmokeMaterial material, float4 bump, float globalFlipGreen)
{
    const uint normalDecodeMode = SmokeMatClassNormalDecodeMode(material);
    if (normalDecodeMode == RT_MATCLASS_NORMAL_DECODE_RGB8_RG)
    {
        return bump.rg;
    }
    if (normalDecodeMode == RT_MATCLASS_NORMAL_DECODE_COMPRESSED_WY)
    {
        const float normalY = globalFlipGreen > 0.5 ? -bump.y : bump.y;
        return float2(bump.w, normalY);
    }

    const float normalY = globalFlipGreen > 0.5 ? -bump.y : bump.y;
    return float2(bump.w, normalY);
}

float3 SmokeMatClassMetallicF0(PathTraceSmokeMaterial material, float3 albedo)
{
    const float classifiedMetallic = SmokeMatClassMetallic(material);
    const float classifiedF0 = SmokeMatClassF0(material);
    const float3 dielectricF0 = float3(classifiedF0, classifiedF0, classifiedF0);
    return lerp(dielectricF0, max(saturate(albedo), dielectricF0), saturate(classifiedMetallic));
}

void SmokeApplyMaterialClassifierBsdfInternal(
    PathTraceSmokeMaterial material,
    inout float3 albedo,
    float3 specularTexel,
    bool hasSpecularTexel,
    inout float3 specularF0,
    inout float roughness)
{
    const bool canUseSpecularTexel =
        hasSpecularTexel &&
        material.specularTextureIndex != 0xffffffffu;
    if (canUseSpecularTexel && SmokeMatClassRoute(material) == RT_MATCLASS_ROUTE_REAL_PBR_RMAO)
    {
        const float rmaoRoughness = max(0.05, saturate(specularTexel.r));
        const float rmaoMetallic = saturate(specularTexel.g);
        roughness = rmaoRoughness;
        specularF0 = lerp(float3(0.04, 0.04, 0.04), saturate(albedo), rmaoMetallic);
        albedo *= 1.0 - rmaoMetallic;
        return;
    }

    if (SmokeMatClassDrivesLegacySpec(material))
    {
        specularF0 = max(specularF0, SmokeMatClassMetallicF0(material, albedo));
        return;
    }

    if (!SmokeMatClassHasPackedBsdf(material))
    {
        return;
    }

    const float classifiedRoughness = SmokeMatClassRoughness(material);
    const bool hasIncomingTexturedRoughness =
        material.specularTextureIndex != 0xffffffffu &&
        roughness < 0.999;

    if (!hasIncomingTexturedRoughness)
    {
        roughness = saturate(classifiedRoughness);
    }
    const float classifiedMetallic = saturate(SmokeMatClassMetallic(material));
    specularF0 = SmokeMatClassMetallicF0(material, albedo);
    albedo *= 1.0 - classifiedMetallic;
}

void SmokeApplyMaterialClassifierBsdfWithSpecularTexel(
    PathTraceSmokeMaterial material,
    inout float3 albedo,
    float3 specularTexel,
    inout float3 specularF0,
    inout float roughness)
{
    SmokeApplyMaterialClassifierBsdfInternal(material, albedo, specularTexel, true, specularF0, roughness);
}

void SmokeApplyMaterialClassifierBsdf(PathTraceSmokeMaterial material, inout float3 albedo, inout float3 specularF0, inout float roughness)
{
    SmokeApplyMaterialClassifierBsdfInternal(material, albedo, float3(0.0, 0.0, 0.0), false, specularF0, roughness);
}

#endif
