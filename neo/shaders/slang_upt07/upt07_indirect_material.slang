import upt04_geometry_contract;
import upt04_geometry_decode;
import upt04_hit_contract;
import upt04_initial_control;
import upt04_material_contract;
import upt04_material_emission;
import upt04_material_live_contract;
#ifndef UPT_DIAGNOSTIC_LAMBERT
#define UPT_DIAGNOSTIC_LAMBERT 0
#endif
#if UPT_DIAGNOSTIC_LAMBERT
import upt07_material_lambert_diagnostic;
#else
import upt04_material_primary_live;
#endif
import upt04_path_event_contract;
#if !UPT_DIAGNOSTIC_LAMBERT
import upt05_material_openpbr;
#endif
import upt07_emissive_material;
#ifndef UPT09_SPATIAL_INDIRECT_REPLAY
#define UPT09_SPATIAL_INDIRECT_REPLAY 0
#endif
#if UPT09_SPATIAL_INDIRECT_REPLAY
import upt09_spatial_direct_bindings;
#define gUpt07Control gUpt09Control
#else
import upt07_temporal_direct_bindings;
#endif
import upt07_work_budget;

#ifndef UPT07_THREE_VERTEX_REPLAY
#define UPT07_THREE_VERTEX_REPLAY 0
#endif

public static const uint kUpt07MaterialAlphaClipTest = 0x00000001u;
public static const uint kUpt07MaterialDiffuseYCoCg = 0x00000002u;
public static const uint kUpt07MaterialAlphaClipFromDiffuseLuma = 0x00000040u;
public static const uint kUpt07MaterialForceDebugAlbedo = 0x00000080u;
public static const uint kUpt07MaterialAlphaClipFromDiffuseDarkKey = 0x00000100u;
public static const uint kUpt07MaterialAlphaClipFromDiffuseMagentaKey = 0x00001000u;
public static const uint kUpt07SurfaceClassMask = 0x000000ffu;
public static const uint kUpt07SurfaceClassTranslucent = 3u;
public static const uint kUpt07TranslucentSubtypeShift = 24u;
public static const uint kUpt07TranslucentSubtypeMask = 0x0f000000u;
public static const uint kUpt07TranslucentSubtypeGuiScreen = 5u;
public static const uint kUpt07MaterialPortalWindowFallback = 0x00000200u;
public static const uint kUpt07MaterialObjectGlassFallback = 0x00000400u;

public float3 Upt07MaterialYCoCgToRgb(float4 ycocg)
{
    ycocg.z = ycocg.z * 31.875f + 1.0f;
    ycocg.z = 1.0f / ycocg.z;
    ycocg.xy *= ycocg.z;
    return saturate(float3(
        dot(ycocg, float4(1.0f, -1.0f, 0.0f, 1.0f)),
        dot(ycocg, float4(0.0f, 1.0f, -0.50196078f, 1.0f)),
        dot(ycocg, float4(-1.0f, -1.0f, 1.00392156f, 1.0f))));
}

public float Upt07MaterialSrgbToLinear(float encoded)
{
    return encoded <= 0.04045f
        ? encoded / 12.92f
        : pow((encoded + 0.055f) / 1.055f, 2.4f);
}

public float3 Upt07MaterialSrgbToLinear3(float3 encoded)
{
    return float3(
        Upt07MaterialSrgbToLinear(encoded.x),
        Upt07MaterialSrgbToLinear(encoded.y),
        Upt07MaterialSrgbToLinear(encoded.z));
}

public bool Upt07MaterialTexture(
    uint textureIndex,
    float2 texCoord,
    out float4 texel)
{
    texel = float4(1.0f);
    if (textureIndex == kUpt04InvalidIdentity
        || textureIndex >= gUpt07Control.logicalTextureCount
        || !all(isfinite(texCoord)) || any(abs(texCoord) > 65536.0f))
        return false;
    texel = gUptDirectEmitterTextures[NonUniformResourceIndex(textureIndex)]
        .SampleLevel(gUptDirectEmitterSampler, frac(texCoord), 0.0f);
    return all(isfinite(texel)) && !any(abs(texel) > 65504.0f);
}

public bool Upt07MaterialTextureCounted(
    uint textureIndex,
    float2 texCoord,
    out float4 texel,
    inout Upt07WorkCounters work)
{
    if (textureIndex != kUpt04InvalidIdentity
        && textureIndex < gUpt07Control.logicalTextureCount
        && all(isfinite(texCoord)) && !any(abs(texCoord) > 65536.0f))
        Upt07WorkAdd(work, kUpt07WorkTextureSamples);
    return Upt07MaterialTexture(textureIndex, texCoord, texel);
}

public bool Upt07MaterialAlwaysTransmitsCandidate(
    Upt04HitFacts candidate,
    inout Upt07WorkCounters work)
{
    uint materialIndex;
    Upt07WorkAdd(work, kUpt07WorkGeometryResolves);
    if (!Upt04ResolveTriangleMaterialIndex(candidate, materialIndex)
        || materialIndex >= gUpt07Control.materialCount)
        return false;
    Upt07WorkAdd(work, kUpt07WorkMaterialRecordLoads);
    const uint flags = gUptDirectEmitterMaterials[materialIndex].flags;
    if ((flags & (kUpt07MaterialPortalWindowFallback
            | kUpt07MaterialObjectGlassFallback)) != 0u)
        return true;
    uint triangleClassAndFlags;
    Upt07WorkAdd(work, kUpt07WorkGeometryResolves);
    return Upt04ResolveTriangleClassAndFlags(
            candidate, triangleClassAndFlags)
        && Upt04TriangleClassIsTransmittingGlass(triangleClassAndFlags);
}

// Reject only a proven discarded texel. Missing route/material/texture data
// remains solid, matching the accepted D0 no-hole traversal rule.
public bool Upt07MaterialRejectsAlphaClipCandidate(
    Upt04HitFacts candidate,
    inout Upt07WorkCounters work)
{
    uint materialIndex;
    Upt07WorkAdd(work, kUpt07WorkGeometryResolves);
    if (!Upt04ResolveTriangleMaterialIndex(candidate, materialIndex)
        || materialIndex >= gUpt07Control.materialCount)
        return false;
    Upt07WorkAdd(work, kUpt07WorkMaterialRecordLoads);
    const uint materialFlags =
        gUptDirectEmitterMaterials[materialIndex].flags;
    if ((materialFlags & kUpt07MaterialAlphaClipTest) == 0u)
        return false;

    Upt04ResolvedTriangle triangle;
    Upt07WorkAdd(work, kUpt07WorkGeometryResolves);
    if (!Upt04ResolveTriangle(candidate, triangle)
        || triangle.materialIndex != materialIndex
        || triangle.materialIndex >= gUpt07Control.materialCount)
        return false;
    Upt07WorkAdd(work, kUpt07WorkMaterialRecordLoads);
    const Upt04SmokeMaterialRecord material =
        gUptDirectEmitterMaterials[triangle.materialIndex];
    if ((material.flags & kUpt07MaterialAlphaClipTest) == 0u
        || !isfinite(material.alphaCutoff))
        return false;
    const float3 barycentrics = float3(
        1.0f - candidate.barycentrics.x - candidate.barycentrics.y,
        candidate.barycentrics.x,
        candidate.barycentrics.y);
    if (!all(isfinite(barycentrics)) || any(barycentrics < -1.0e-4f)
        || any(barycentrics > 1.0001f))
        return false;
    const float2 texCoord =
        triangle.vertex0.texCoord.xy * barycentrics.x
        + triangle.vertex1.texCoord.xy * barycentrics.y
        + triangle.vertex2.texCoord.xy * barycentrics.z;

    float4 diffuse = material.debugAlbedo;
    bool diffuseValid = true;
    if ((material.flags & kUpt07MaterialForceDebugAlbedo) == 0u)
    {
        diffuseValid = Upt07MaterialTextureCounted(
            material.diffuseTextureIndex, texCoord, diffuse, work);
        if (!diffuseValid
            && material.diffuseTextureIndex == kUpt04InvalidIdentity)
        {
            diffuse = material.debugAlbedo;
            diffuseValid = true;
        }
        if (diffuseValid
            && (gUpt07Control.geometryAvailabilityFlags
                & kUpt04MaterialDecodeTextures) != 0u
            && (material.flags & kUpt07MaterialDiffuseYCoCg) != 0u)
            diffuse.xyz = Upt07MaterialYCoCgToRgb(diffuse);
    }

    float coverage = 1.0f;
    if ((material.flags & kUpt07MaterialAlphaClipFromDiffuseDarkKey) != 0u)
    {
        if (!diffuseValid) return false;
        coverage = 1.0f - max(max(diffuse.x, diffuse.y), diffuse.z);
    }
    else if ((material.flags & kUpt07MaterialAlphaClipFromDiffuseLuma) != 0u)
    {
        if (!diffuseValid) return false;
        coverage = max(max(diffuse.x, diffuse.y), diffuse.z);
    }
    else if ((material.flags & kUpt07MaterialAlphaClipFromDiffuseMagentaKey) != 0u)
    {
        if (!diffuseValid) return false;
        const float keyDistance = max(abs(diffuse.x - 1.0f),
            max(abs(diffuse.y), abs(diffuse.z - 1.0f)));
        coverage = keyDistance <= 0.08f ? 0.0f : 1.0f;
    }
    else if (material.alphaTextureIndex != kUpt04InvalidIdentity)
    {
        float4 alphaTexel;
        if (!Upt07MaterialTextureCounted(
                material.alphaTextureIndex, texCoord, alphaTexel, work))
            return false;
        coverage = alphaTexel.w;
    }
    else
    {
        if (!diffuseValid) return false;
        coverage = diffuse.w;
    }
    return isfinite(coverage)
        && saturate(coverage) < saturate(material.alphaCutoff);
}

public Upt04PreparedDirectMaterial Upt07PrepareSecondaryDirect(
    Upt04GeometryHitSurface surface,
    float3 viewDirection)
{
#if UPT_DIAGNOSTIC_LAMBERT
    return Upt07LambertPrepareSecondary(surface, viewDirection);
#else
    Upt04PreparedDirectMaterial result = (Upt04PreparedDirectMaterial)0;
    if (surface.valid == 0u
        || surface.materialIndex >= gUpt07Control.materialCount)
        return result;
    const Upt04SmokeMaterialRecord material =
        gUptDirectEmitterMaterials[surface.materialIndex];
    const bool forceDebug =
        (material.flags & kUpt07MaterialForceDebugAlbedo) != 0u;
    const bool hasDiffuse = material.diffuseTextureIndex != kUpt04InvalidIdentity;
    float4 diffuse = saturate(material.debugAlbedo);
    if (!forceDebug && hasDiffuse)
    {
        if (!Upt07MaterialTexture(
                material.diffuseTextureIndex, surface.texCoord, diffuse))
            return result;
        diffuse = saturate(diffuse);
        if ((gUpt07Control.geometryAvailabilityFlags
                & kUpt04MaterialDecodeTextures) != 0u
            && (material.flags & kUpt07MaterialDiffuseYCoCg) != 0u)
            diffuse.xyz = Upt07MaterialYCoCgToRgb(diffuse);
    }
    float3 baseColor = diffuse.xyz;
    const uint surfaceClass =
        surface.triangleClassAndFlags & kUpt07SurfaceClassMask;
    const uint translucentSubtype =
        (surface.triangleClassAndFlags & kUpt07TranslucentSubtypeMask)
            >> kUpt07TranslucentSubtypeShift;
    if (surfaceClass == kUpt07SurfaceClassTranslucent
        && translucentSubtype == kUpt07TranslucentSubtypeGuiScreen)
        baseColor = hasDiffuse && !forceDebug
            ? diffuse.xyz * surface.vertexColor.xyz : surface.vertexColor.xyz;

    float3 specularF0 = float3(0.0f);
    float roughness = 1.0f;
    if ((gUpt07Control.geometryAvailabilityFlags
            & kUpt04MaterialUseSpecularMaps) != 0u
        && material.specularTextureIndex != kUpt04InvalidIdentity)
    {
        float4 sampledSpecular;
        if (!Upt07MaterialTexture(
                material.specularTextureIndex,
                surface.texCoord,
                sampledSpecular))
            return result;
        const float3 specularTexel = saturate(sampledSpecular.xyz);
        specularF0 = specularTexel;
        if ((gUpt07Control.geometryAvailabilityFlags
                & kUpt04MaterialLegacySpecmapToPbr) != 0u)
        {
            const float specularLuminance = dot(
                float3(0.2125f, 0.7154f, 0.0721f), specularTexel);
            specularF0 = float3(0.04f);
            const float contrastMid = 0.214f;
            float contrast = saturate(
                (specularLuminance - contrastMid) / (1.0f - contrastMid));
            contrast += saturate(specularLuminance / contrastMid) - 1.0f;
            contrast = exp2(2.0f * contrast);
            specularF0 *= contrast;
            const float encodedBrightness = saturate(2.0f * specularLuminance);
            const float linearBrightness =
                Upt07MaterialSrgbToLinear(encodedBrightness);
            const float specularPower = max(0.0f,
                (8.0f * linearBrightness) / max(specularF0.y, 1.0e-4f)
                    - 2.0f);
            specularF0 *= min(1.0f,
                linearBrightness / max(specularF0.y * 0.25f, 1.0e-4f));
            roughness = sqrt(2.0f / (specularPower + 2.0f));
            const float metallic = step(0.7f, saturate(1.0f - roughness));
            const float3 linearSpecular =
                Upt07MaterialSrgbToLinear3(specularTexel);
            specularF0 = lerp(specularF0, linearSpecular, metallic);
            roughness = sqrt(roughness);
        }
    }

    // T0 reuses the exact accepted D0 secondary material baseline.
    result.normal = Upt05OpenPbrSafeNormalize(
        surface.shadingNormal,
        Upt05OpenPbrSafeNormalize(
            surface.geometricNormal, float3(0.0f, 0.0f, 1.0f)));
    result.viewDirection = Upt05OpenPbrSafeNormalize(
        viewDirection, result.normal);
    result.baseColor = saturate(baseColor);
    result.roughness = clamp(roughness, 0.02f, 1.0f);
    result.specularF0 = saturate(specularF0);
    result.valid = 1u;
    return result;
#endif
}

#if UPT07_THREE_VERTEX_REPLAY && !UPT_DIAGNOSTIC_LAMBERT
// Exact counterpart of UPT-35's secondary continuation sampler.  Keep this
// behind its own temporal specialization so the accepted one-continuation T0
// artifact pays no code-size or register-pressure cost.
public Upt04PathEvent Upt07SampleSecondaryContinuation(
    Upt04PreparedDirectMaterial prepared,
    float lobeRandom,
    float2 directionRandom)
{
    Upt04PathEvent result = (Upt04PathEvent)0;
    if (prepared.valid == 0u)
        return result;
    Upt05OpenPbrParameters parameters;
    parameters.baseColor = prepared.baseColor;
    parameters.roughness = prepared.roughness;
    parameters.specularF0 = prepared.specularF0;
    const Upt05OpenPbrSample sampled = Upt05SampleOpenPbr(
        parameters,
        prepared.normal,
        prepared.viewDirection,
        lobeRandom,
        directionRandom);
    if (sampled.valid == 0u || !isfinite(sampled.directionalPdf)
        || sampled.directionalPdf <= 0.0f)
        return result;
    result.direction = sampled.direction;
    result.pdf = sampled.directionalPdf;
    result.throughput = sampled.throughput;
    result.lobeMask = sampled.lobeMask;
    result.valid = all(isfinite(result.throughput)) ? 1u : 0u;
    return result;
}
#endif

public Upt04MaterialEvaluation Upt07EvaluateSecondaryDirect(
    Upt04GeometryHitSurface surface,
    float3 viewDirection,
    float3 lightDirection,
    float3 incidentRadiance)
{
    return Upt04EvaluatePreparedDirectProbe(
        Upt07PrepareSecondaryDirect(surface, viewDirection),
        lightDirection,
        incidentRadiance);
}

public Upt04MaterialEmissionEvaluation Upt07EvaluateHitEmission(
    Upt04GeometryHitSurface surface)
{
    return UptDirectEvaluateEmissiveEndpoint(
        surface.materialIndex,
        surface.triangleClassAndFlags,
        surface.texCoord,
        float2(0.0f),
        float2(0.0f),
        gUpt07Control.materialCount,
        gUpt07Control.logicalTextureCount,
        gUpt07Control.emissiveScale);
}
