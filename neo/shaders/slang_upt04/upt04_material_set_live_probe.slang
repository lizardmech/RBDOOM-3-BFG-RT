import upt04_material_contract;
import upt04_primary_surface_live_contract;
import upt04_material_live_contract;
import upt04_initial_bindings;
import upt04_initial_control;
import upt04_hit_contract;
import upt04_light_contract;
import upt04_path_event_contract;
import upt04_geometry_contract;
import upt04_material_primary_live;
import upt05_material_openpbr;

public static const uint kUpt04MaterialEmissive = 0x00000008u;
public static const uint kUpt04TriangleEmissiveStageOff = 0x00040000u;

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

// Binding-closure probe for the future secondary-hit MaterialSet.  Primary
// shading uses the already evaluated 176-byte surface values.  The optional
// texture path exists only to prove the shared set-1 table/sampler ABI; it is
// disabled in the admitted production specialization until hit UV decoding exists.
public float3 Upt04MaterialBindingProbe(uint materialIndex, float2 uv)
{
    if ((gUpt04Control.shaderProofMode & 0x80000000u) == 0u
        || materialIndex >= gUpt04Control.materialCount)
        return float3(1.0f);
    const Upt04SmokeMaterialRecord material = gUpt04Materials[materialIndex];
    if (material.diffuseTextureIndex == kUpt04InvalidIdentity
        || material.diffuseTextureIndex >= gUpt04Control.logicalTextureCount)
        return max(material.debugAlbedo.xyz, float3(0.0f));
    return max(gUpt04MaterialTextures[NonUniformResourceIndex(material.diffuseTextureIndex)]
        .SampleLevel(gUpt04MaterialSampler, frac(uv), 0.0f).xyz, float3(0.0f));
}

public Upt04MaterialEmissionEvaluation Upt04EvaluateEmissiveEndpoint(
    uint materialIndex,
    uint triangleClassAndFlags,
    float2 uv)
{
    Upt04MaterialEmissionEvaluation result = (Upt04MaterialEmissionEvaluation)0;
    if (materialIndex >= gUpt04Control.materialCount
        || !all(isfinite(uv)))
        return result;

    const Upt04SmokeMaterialRecord material = gUpt04Materials[materialIndex];
    if ((material.flags & kUpt04MaterialEmissive) == 0u
        || (triangleClassAndFlags & kUpt04TriangleEmissiveStageOff) != 0u)
    {
        result.valid = 1u;
        return result;
    }

    float3 radiance = max(material.emissiveColor.xyz, float3(0.0f));
    if (material.emissiveTextureIndex != kUpt04InvalidIdentity)
    {
        if (material.emissiveTextureIndex >= gUpt04Control.logicalTextureCount)
            return result;
        radiance *= saturate(
            gUpt04MaterialTextures[NonUniformResourceIndex(material.emissiveTextureIndex)]
                .SampleLevel(gUpt04MaterialSampler, frac(uv), 0.0f).xyz);
    }
    // Doom's baseline emissive scale belongs to this replaceable MaterialSet,
    // never to proposal, replay, transport, or reservoir code.
    radiance *= 1.75f;
    if (!all(isfinite(radiance)))
        return result;
    result.radiance = radiance;
    result.valid = 1u;
    return result;
}

public Upt04MaterialEvaluation Upt04EvaluateSecondaryDirectProbe(
    Upt04GeometryHitSurface surface,
    float3 viewDirection,
    float3 lightDirection,
    float3 incidentRadiance)
{
    Upt04MaterialEvaluation result = (Upt04MaterialEvaluation)0;
    if (surface.valid == 0u
        || surface.materialIndex >= gUpt04Control.materialCount
        || !all(isfinite(lightDirection))
        || !all(isfinite(incidentRadiance)))
        return result;

    const Upt04SmokeMaterialRecord material = gUpt04Materials[surface.materialIndex];
    float3 baseColor = max(material.debugAlbedo.xyz, float3(0.0f));
    if (material.diffuseTextureIndex != kUpt04InvalidIdentity)
    {
        if (material.diffuseTextureIndex >= gUpt04Control.logicalTextureCount)
            return result;
        baseColor *= saturate(
            gUpt04MaterialTextures[NonUniformResourceIndex(material.diffuseTextureIndex)]
                .SampleLevel(gUpt04MaterialSampler, frac(surface.texCoord), 0.0f).xyz);
    }
    baseColor *= max(surface.vertexColor.xyz, float3(0.0f));

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

    const float3 normal = Upt05OpenPbrSafeNormalize(
        surface.shadingNormal,
        Upt05OpenPbrSafeNormalize(
            surface.geometricNormal,
            float3(0.0f, 0.0f, 1.0f)));
    viewDirection = Upt05OpenPbrSafeNormalize(viewDirection, normal);
    Upt05OpenPbrParameters parameters;
    parameters.baseColor = saturate(baseColor);
    parameters.roughness = roughness;
    parameters.specularF0 = saturate(specularF0);
    const Upt05OpenPbrEvaluation evaluated = Upt05OpenPbrEvaluate(
        parameters,
        normal,
        lightDirection,
        viewDirection);
    if (evaluated.valid == 0u)
        return result;
    const float cosine = saturate(dot(normal, lightDirection));
    result.weightedContribution = evaluated.brdf
        * max(incidentRadiance, float3(0.0f)) * cosine;
    result.target = max(result.weightedContribution.x,
        max(result.weightedContribution.y, result.weightedContribution.z));
    result.directionalPdf = evaluated.directionalPdf;
    result.lobeMask = evaluated.lobeMask;
    result.valid = all(isfinite(result.weightedContribution)) ? 1u : 0u;
    return result;
}
