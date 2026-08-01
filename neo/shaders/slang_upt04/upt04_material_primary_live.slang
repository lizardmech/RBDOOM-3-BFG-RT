import upt04_material_contract;
import upt04_primary_surface_live_contract;
import upt04_path_event_contract;
import upt04_light_contract;

public Upt04MaterialEvaluation Upt04EvaluatePrimaryDirectProbe(
    Upt04PrimarySurfaceRecord surface,
    float3 lightDirection,
    float3 incidentRadiance)
{
    Upt04MaterialEvaluation result = (Upt04MaterialEvaluation)0;
    if (!Upt04PrimarySurfaceRecordValid(surface)
        || !all(isfinite(lightDirection))
        || !all(isfinite(incidentRadiance)))
        return result;
    const float cosine = saturate(dot(
        normalize(surface.shadingNormalAndOpacity.xyz), lightDirection));
    result.weightedContribution =
        max(surface.albedoAndAlphaCutoff.xyz, float3(0.0f))
        * max(incidentRadiance, float3(0.0f)) * cosine
        * 0.3183098861837907f;
    result.target = max(result.weightedContribution.x,
        max(result.weightedContribution.y, result.weightedContribution.z));
    result.directionalPdf = cosine * 0.3183098861837907f;
    result.lobeMask = kUpt04LobeDiffuse;
    result.valid = all(isfinite(result.weightedContribution)) ? 1u : 0u;
    return result;
}

public float3 Upt04MaterialBuildPerpendicular(float3 normal)
{
    return normalize(abs(normal.z) < 0.999f
        ? cross(float3(0.0f, 0.0f, 1.0f), normal)
        : cross(float3(0.0f, 1.0f, 0.0f), normal));
}

public bool Upt04PrimaryContinuationAvailable(Upt04PrimarySurfaceRecord surface)
{
    return Upt04PrimarySurfaceRecordValid(surface)
        && any(max(surface.albedoAndAlphaCutoff.xyz, float3(0.0f)) > 0.0f);
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
    const float3 normal = normalize(surface.shadingNormalAndOpacity.xyz);
    const float radius = sqrt(directionRandom.x);
    const float phi = 6.283185307179586f * directionRandom.y;
    const float3 tangent = Upt04MaterialBuildPerpendicular(normal);
    const float3 bitangent = cross(normal, tangent);
    const float3 direction = normalize(
        tangent * (radius * cos(phi))
        + bitangent * (radius * sin(phi))
        + normal * sqrt(max(0.0f, 1.0f - directionRandom.x)));
    const float cosine = saturate(dot(normal, direction));
    const float pdf = cosine * 0.3183098861837907f;
    if (!Upt04FinitePositive(pdf))
        return result;
    result.direction = direction;
    result.pdf = pdf;
    result.throughput = max(surface.albedoAndAlphaCutoff.xyz, float3(0.0f));
    result.lobeMask = kUpt04LobeDiffuse;
    result.valid = all(isfinite(result.throughput)) ? 1u : 0u;
    return result;
}
