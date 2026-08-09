import upt04_material_contract;
import upt04_primary_surface_live_contract;
import upt04_material_live_contract;
import upt04_material_emission;
import upt04_initial_bindings;
import upt04_initial_control;
import upt04_hit_contract;
import upt04_light_contract;
import upt04_path_event_contract;
import upt04_geometry_contract;
import upt04_material_primary_live;
import upt05_material_openpbr;

public static const uint kUpt04MaterialDiffuseYCoCg = 0x00000002u;
public static const uint kUpt04MaterialForceDebugAlbedo = 0x00000080u;
public static const uint kUpt04SurfaceClassMask = 0x000000ffu;
public static const uint kUpt04SurfaceClassTranslucent = 3u;
public static const uint kUpt04TranslucentSubtypeShift = 24u;
public static const uint kUpt04TranslucentSubtypeMask = 0x0f000000u;
public static const uint kUpt04TranslucentSubtypeGuiScreen = 5u;

public float Upt04MaterialSrgbToLinear(float encoded)
{
    return encoded <= 0.04045f
        ? encoded / 12.92f
        : pow((encoded + 0.055f) / 1.055f, 2.4f);
}

public float3 Upt04MaterialSrgbToLinear3(float3 encoded)
{
    return float3(
        Upt04MaterialSrgbToLinear(encoded.x),
        Upt04MaterialSrgbToLinear(encoded.y),
        Upt04MaterialSrgbToLinear(encoded.z));
}

public float3 Upt04MaterialYCoCgToRgb(float4 ycocg)
{
    ycocg.z = ycocg.z * 31.875f + 1.0f;
    ycocg.z = 1.0f / ycocg.z;
    ycocg.xy *= ycocg.z;
    return saturate(float3(
        dot(ycocg, float4(1.0f, -1.0f, 0.0f, 1.0f)),
        dot(ycocg, float4(0.0f, 1.0f, -0.50196078f, 1.0f)),
        dot(ycocg, float4(-1.0f, -1.0f, 1.00392156f, 1.0f))));
}

// Binding-closure probe retained for static verification. Primary shading uses
// the already evaluated receiver values; the production secondary-hit path
// below consumes the same set-1 material texture table through decoded hit UVs.
public float3 Upt04MaterialBindingProbe(uint materialIndex, float2 uv)
{
    if ((gUpt04Control.shaderProofMode & 0x80000000u) == 0u
        || materialIndex >= gUpt04Control.materialCount)
        return float3(1.0f);
    const Upt04SmokeMaterialRecord material = Upt04LoadMaterialRecord(materialIndex);
    if (material.diffuseTextureIndex == kUpt04InvalidIdentity
        || material.diffuseTextureIndex >= gUpt04Control.logicalTextureCount)
        return max(material.debugAlbedo.xyz, float3(0.0f));
    return max(gUpt04MaterialTextures[NonUniformResourceIndex(material.diffuseTextureIndex)]
        .SampleLevel(gUpt04MaterialSampler, frac(uv), 0.0f).xyz, float3(0.0f));
}

public Upt04MaterialEmissionEvaluation Upt04EvaluateEmissiveEndpoint(
    uint materialIndex,
    uint triangleClassAndFlags,
    float2 uv,
    float2 uvGradient0,
    float2 uvGradient1)
{
    Upt04MaterialEmissionEvaluation result = (Upt04MaterialEmissionEvaluation)0;
    if (materialIndex >= gUpt04Control.materialCount
        || !all(isfinite(uv))
        || !all(isfinite(uvGradient0))
        || !all(isfinite(uvGradient1)))
        return result;

    const Upt04SmokeMaterialRecord material = Upt04LoadMaterialRecord(materialIndex);
    float3 emissiveTexel = float3(1.0f);
    uint emissiveTexelValid = 1u;
    if (material.emissiveTextureIndex != kUpt04InvalidIdentity)
    {
        if (material.emissiveTextureIndex >= gUpt04Control.logicalTextureCount)
            emissiveTexelValid = 0u;
        else
            emissiveTexel =
                gUpt04MaterialTextures[NonUniformResourceIndex(
                    material.emissiveTextureIndex)]
                    .SampleGrad(
                        gUpt04MaterialSampler,
                        frac(uv),
                        uvGradient0,
                        uvGradient1).xyz;
    }
    return Upt04FinalizeEmissiveEndpoint(
        material,
        triangleClassAndFlags,
        emissiveTexel,
        emissiveTexelValid,
        gUpt04Control.emissiveScale);
}

public Upt04PreparedDirectMaterial Upt04PrepareSecondaryDirectProbe(
    Upt04GeometryHitSurface surface,
    float3 viewDirection)
{
    Upt04PreparedDirectMaterial result = (Upt04PreparedDirectMaterial)0;
    if (surface.valid == 0u
        || surface.materialIndex >= gUpt04Control.materialCount)
        return result;

    const Upt04SmokeMaterialRecord material = Upt04LoadMaterialRecord(surface.materialIndex);
    const bool forceDebugAlbedo =
        (material.flags & kUpt04MaterialForceDebugAlbedo) != 0u;
    const bool hasDiffuseTexture =
        material.diffuseTextureIndex != kUpt04InvalidIdentity;
    float4 diffuse = saturate(material.debugAlbedo);
    if (!forceDebugAlbedo && hasDiffuseTexture)
    {
        if (material.diffuseTextureIndex >= gUpt04Control.logicalTextureCount)
            return result;
        diffuse = saturate(
            gUpt04MaterialTextures[NonUniformResourceIndex(material.diffuseTextureIndex)]
                .SampleLevel(gUpt04MaterialSampler, frac(surface.texCoord), 0.0f));
        if ((gUpt04Control.availabilityFlags
                & kUpt04MaterialDecodeTextures) != 0u
            && (material.flags & kUpt04MaterialDiffuseYCoCg) != 0u)
        {
            diffuse.xyz = Upt04MaterialYCoCgToRgb(diffuse);
        }
    }
    float3 baseColor = diffuse.xyz;
    const uint surfaceClass =
        surface.triangleClassAndFlags & kUpt04SurfaceClassMask;
    const uint translucentSubtype =
        (surface.triangleClassAndFlags & kUpt04TranslucentSubtypeMask)
            >> kUpt04TranslucentSubtypeShift;
    if (surfaceClass == kUpt04SurfaceClassTranslucent
        && translucentSubtype == kUpt04TranslucentSubtypeGuiScreen)
    {
        baseColor = hasDiffuseTexture && !forceDebugAlbedo
            ? diffuse.xyz * surface.vertexColor.xyz
            : surface.vertexColor.xyz;
    }

    float3 specularF0 = float3(0.0f);
    float roughness = 1.0f;
    if ((gUpt04Control.availabilityFlags & kUpt04MaterialUseSpecularMaps) != 0u
        && material.specularTextureIndex != kUpt04InvalidIdentity)
    {
        if (material.specularTextureIndex >= gUpt04Control.logicalTextureCount)
            return result;
        const float3 specularTexel = saturate(
            gUpt04MaterialTextures[NonUniformResourceIndex(
                material.specularTextureIndex)]
                .SampleLevel(gUpt04MaterialSampler, frac(surface.texCoord), 0.0f).xyz);
        specularF0 = specularTexel;
        if ((gUpt04Control.availabilityFlags
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
                Upt04MaterialSrgbToLinear(encodedBrightness);
            const float specularPower = max(
                0.0f,
                (8.0f * linearBrightness) / max(specularF0.y, 1.0e-4f)
                    - 2.0f);
            specularF0 *= min(
                1.0f,
                linearBrightness / max(specularF0.y * 0.25f, 1.0e-4f));
            roughness = sqrt(2.0f / (specularPower + 2.0f));
            const float metallic = step(0.7f, saturate(1.0f - roughness));
            const float3 linearSpecular =
                Upt04MaterialSrgbToLinear3(specularTexel);
            specularF0 = lerp(specularF0, linearSpecular, metallic);
            roughness = sqrt(roughness);
        }
    }

    result.normal = Upt05OpenPbrSafeNormalize(
        surface.shadingNormal,
        Upt05OpenPbrSafeNormalize(
            surface.geometricNormal,
            float3(0.0f, 0.0f, 1.0f)));
    result.viewDirection = Upt05OpenPbrSafeNormalize(
        viewDirection, result.normal);
    result.baseColor = saturate(baseColor);
    result.roughness = clamp(roughness, 0.02f, 1.0f);
    result.specularF0 = saturate(specularF0);
    result.valid = 1u;
    return result;
}

public Upt04MaterialEvaluation Upt04EvaluateSecondaryDirectProbe(
    Upt04GeometryHitSurface surface,
    float3 viewDirection,
    float3 lightDirection,
    float3 incidentRadiance)
{
    return Upt04EvaluatePreparedDirectProbe(
        Upt04PrepareSecondaryDirectProbe(surface, viewDirection),
        lightDirection,
        incidentRadiance);
}
