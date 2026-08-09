import upt04_material_contract;
import upt04_primary_surface_live_contract;
import upt04_path_event_contract;
import upt04_light_contract;
import upt05_material_openpbr;

public Upt04PreparedDirectMaterial Upt04PreparePrimaryDirectProbe(
    Upt04PrimarySurfaceRecord surface)
{
    Upt04PreparedDirectMaterial result = (Upt04PreparedDirectMaterial)0;
    if (!Upt04PrimarySurfaceRecordValid(surface))
        return result;
    result.normal = Upt05OpenPbrSafeNormalize(
        surface.shadingNormalAndOpacity.xyz,
        Upt05OpenPbrSafeNormalize(
            surface.geometricNormalAndRoughness.xyz,
            float3(0.0f, 0.0f, 1.0f)));
    result.viewDirection = Upt05OpenPbrSafeNormalize(
        surface.viewDirectionAndReserved.xyz, result.normal);
    result.baseColor = saturate(surface.albedoAndAlphaCutoff.xyz);
    result.roughness = clamp(
        saturate(surface.geometricNormalAndRoughness.w), 0.02f, 1.0f);
    result.specularF0 = saturate(surface.specularF0AndReserved.xyz);
    result.valid = 1u;
    return result;
}

public Upt04MaterialEvaluation Upt04EvaluatePreparedDirectProbe(
    Upt04PreparedDirectMaterial prepared,
    float3 lightDirection,
    float3 incidentRadiance)
{
    Upt04MaterialEvaluation result = (Upt04MaterialEvaluation)0;
    if (prepared.valid == 0u
        || !all(isfinite(lightDirection))
        || !all(isfinite(incidentRadiance)))
        return result;
    Upt05OpenPbrParameters parameters;
    parameters.baseColor = prepared.baseColor;
    parameters.roughness = prepared.roughness;
    parameters.specularF0 = prepared.specularF0;
    const Upt05OpenPbrEvaluation evaluated = Upt05OpenPbrEvaluatePrepared(
        parameters,
        prepared.normal,
        lightDirection,
        prepared.viewDirection);
    if (evaluated.valid == 0u)
        return result;
    const float cosine = saturate(dot(prepared.normal, lightDirection));
    result.weightedContribution = evaluated.brdf
        * max(incidentRadiance, float3(0.0f)) * cosine;
    result.target = max(result.weightedContribution.x,
        max(result.weightedContribution.y, result.weightedContribution.z));
    result.directionalPdf = evaluated.directionalPdf;
    result.lobeMask = evaluated.lobeMask;
    result.valid = all(isfinite(result.weightedContribution)) ? 1u : 0u;
    return result;
}

public Upt04MaterialEvaluation Upt04EvaluatePrimaryDirectProbe(
    Upt04PrimarySurfaceRecord surface,
    float3 lightDirection,
    float3 incidentRadiance)
{
    return Upt04EvaluatePreparedDirectProbe(
        Upt04PreparePrimaryDirectProbe(surface),
        lightDirection,
        incidentRadiance);
}

public bool Upt04PrimaryContinuationAvailable(Upt04PrimarySurfaceRecord surface)
{
    return Upt04PrimarySurfaceRecordValid(surface)
        && (any(max(surface.albedoAndAlphaCutoff.xyz, float3(0.0f)) > 0.0f)
            || any(max(surface.specularF0AndReserved.xyz, float3(0.0f)) > 0.0f));
}

public Upt04PathEvent Upt04SamplePrimaryContinuation(
    Upt04PrimarySurfaceRecord surface,
    float lobeRandom,
    float2 directionRandom)
{
    Upt04PathEvent result = (Upt04PathEvent)0;
    if (!Upt04PrimaryContinuationAvailable(surface)
        || !isfinite(lobeRandom) || lobeRandom < 0.0f || lobeRandom >= 1.0f
        || !all(isfinite(directionRandom))
        || any(directionRandom < 0.0f) || any(directionRandom >= 1.0f))
        return result;
    const float3 normal = Upt05OpenPbrSafeNormalize(
        surface.shadingNormalAndOpacity.xyz,
        Upt05OpenPbrSafeNormalize(
            surface.geometricNormalAndRoughness.xyz,
            float3(0.0f, 0.0f, 1.0f)));
    const float3 viewDirection = Upt05OpenPbrSafeNormalize(
        surface.viewDirectionAndReserved.xyz, normal);
    Upt05OpenPbrParameters parameters;
    parameters.baseColor = saturate(surface.albedoAndAlphaCutoff.xyz);
    parameters.roughness = saturate(surface.geometricNormalAndRoughness.w);
    parameters.specularF0 = saturate(surface.specularF0AndReserved.xyz);
    const Upt05OpenPbrSample sampled = Upt05SampleOpenPbr(
        parameters,
        normal,
        viewDirection,
        lobeRandom,
        directionRandom);
    if (sampled.valid == 0u || !Upt04FinitePositive(sampled.directionalPdf))
        return result;
    result.direction = sampled.direction;
    result.pdf = sampled.directionalPdf;
    result.throughput = sampled.throughput;
    result.lobeMask = sampled.lobeMask;
    result.valid = all(isfinite(result.throughput)) ? 1u : 0u;
    return result;
}
