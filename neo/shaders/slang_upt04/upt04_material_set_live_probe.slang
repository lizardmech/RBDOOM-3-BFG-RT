import upt04_material_contract;
import upt04_primary_surface_live_contract;
import upt04_material_live_contract;
import upt04_initial_bindings;
import upt04_hit_contract;
import upt04_path_event_contract;

// Binding-closure probe for the future secondary-hit MaterialSet.  Primary
// shading uses the already evaluated 176-byte surface values.  The optional
// texture path exists only to prove the shared set-1 table/sampler ABI; it is
// disabled in the admitted production specialization until hit UV decoding exists.
public float3 Upt04MaterialBindingProbe(uint materialIndex, float2 uv)
{
    if (gUpt04Control.textureBindingProbeEnabled == 0u
        || materialIndex >= gUpt04Control.materialCount)
        return float3(1.0f);
    const Upt04SmokeMaterialRecord material = gUpt04Materials[materialIndex];
    if (material.diffuseTextureIndex == kUpt04InvalidIdentity
        || material.diffuseTextureIndex >= gUpt04Control.logicalTextureCount)
        return max(material.debugAlbedo.xyz, float3(0.0f));
    return max(gUpt04MaterialTextures[NonUniformResourceIndex(material.diffuseTextureIndex)]
        .SampleLevel(gUpt04MaterialSampler, frac(uv), 0.0f).xyz, float3(0.0f));
}

public Upt04MaterialEvaluation Upt04EvaluatePrimaryDirectProbe(
    Upt04PrimarySurfaceRecord surface,
    float3 lightDirection,
    float3 incidentRadiance,
    float3 bindingProbe)
{
    Upt04MaterialEvaluation result = (Upt04MaterialEvaluation)0;
    if (!Upt04PrimarySurfaceRecordValid(surface)
        || !all(isfinite(lightDirection))
        || !all(isfinite(incidentRadiance)))
        return result;

    const float cosine = saturate(dot(normalize(surface.shadingNormalAndOpacity.xyz), lightDirection));
    result.weightedContribution = max(surface.albedoAndAlphaCutoff.xyz, float3(0.0f))
        * max(incidentRadiance, float3(0.0f)) * cosine * bindingProbe;
    result.target = max(result.weightedContribution.x,
        max(result.weightedContribution.y, result.weightedContribution.z));
    result.lobeMask = kUpt04LobeDiffuse;
    result.valid = all(isfinite(result.weightedContribution)) ? 1u : 0u;
    return result;
}
