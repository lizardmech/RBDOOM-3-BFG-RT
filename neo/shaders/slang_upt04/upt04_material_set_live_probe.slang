import upt04_material_contract;
import upt04_primary_surface_live_contract;
import upt04_material_live_contract;
import upt04_initial_bindings;
import upt04_hit_contract;
import upt04_light_contract;
import upt04_path_event_contract;
import upt04_geometry_contract;
import upt04_material_primary_live;

public static const uint kUpt04MaterialEmissive = 0x00000008u;
public static const uint kUpt04TriangleEmissiveStageOff = 0x00040000u;

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
    const float cosine = saturate(dot(surface.shadingNormal, lightDirection));
    result.weightedContribution = baseColor
        * max(incidentRadiance, float3(0.0f)) * cosine
        * 0.3183098861837907f;
    result.target = max(result.weightedContribution.x,
        max(result.weightedContribution.y, result.weightedContribution.z));
    result.directionalPdf = cosine * 0.3183098861837907f;
    result.lobeMask = kUpt04LobeDiffuse;
    result.valid = all(isfinite(result.weightedContribution)) ? 1u : 0u;
    return result;
}
