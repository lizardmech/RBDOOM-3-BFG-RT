import upt04_geometry_contract;
import upt04_light_contract;
import upt04_material_contract;
import upt04_path_event_contract;
import upt04_primary_surface_live_contract;
import upt07_material_provider_contract;

public static const float kUpt07LambertInvPi = 0.3183098861837907f;
public static const float3 kUpt07LambertAlbedo = float3(0.8f);

public float3 Upt07LambertSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return all(isfinite(value)) && lengthSquared > 1.0e-12f
        ? value * rsqrt(lengthSquared) : fallback;
}

public float3 Upt07LambertPerpendicular(float3 normal)
{
    return Upt07LambertSafeNormalize(
        abs(normal.z) < 0.999f
            ? cross(float3(0.0f, 0.0f, 1.0f), normal)
            : cross(float3(0.0f, 1.0f, 0.0f), normal),
        float3(1.0f, 0.0f, 0.0f));
}

public Upt04PreparedDirectMaterial Upt04PreparePrimaryDirectProbe(
    Upt04PrimarySurfaceRecord surface)
{
    Upt04PreparedDirectMaterial result = (Upt04PreparedDirectMaterial)0;
    if (!Upt04PrimarySurfaceRecordValid(surface)) return result;
    result.normal = Upt07LambertSafeNormalize(
        surface.shadingNormalAndOpacity.xyz,
        Upt07LambertSafeNormalize(surface.geometricNormalAndRoughness.xyz,
            float3(0.0f, 0.0f, 1.0f)));
    result.viewDirection = Upt07LambertSafeNormalize(
        surface.viewDirectionAndReserved.xyz, result.normal);
    result.baseColor = kUpt07LambertAlbedo;
    result.roughness = 1.0f;
    result.valid = 1u;
    return result;
}

public Upt04PreparedDirectMaterial Upt07LambertPrepareSecondary(
    Upt04GeometryHitSurface surface, float3 viewDirection)
{
    Upt04PreparedDirectMaterial result = (Upt04PreparedDirectMaterial)0;
    if (surface.valid == 0u) return result;
    result.normal = Upt07LambertSafeNormalize(surface.shadingNormal,
        Upt07LambertSafeNormalize(surface.geometricNormal,
            float3(0.0f, 0.0f, 1.0f)));
    result.viewDirection = Upt07LambertSafeNormalize(viewDirection, result.normal);
    result.baseColor = kUpt07LambertAlbedo;
    result.roughness = 1.0f;
    result.valid = 1u;
    return result;
}

public Upt04MaterialEvaluation Upt04EvaluatePreparedDirectProbe(
    Upt04PreparedDirectMaterial prepared, float3 lightDirection,
    float3 incidentRadiance)
{
    Upt04MaterialEvaluation result = (Upt04MaterialEvaluation)0;
    if (prepared.valid == 0u || !all(isfinite(lightDirection))
        || !all(isfinite(incidentRadiance))) return result;
    const float cosine = saturate(dot(prepared.normal,
        Upt07LambertSafeNormalize(lightDirection, prepared.normal)));
    result.weightedContribution = prepared.baseColor
        * max(incidentRadiance, float3(0.0f)) * cosine * kUpt07LambertInvPi;
    result.target = max(result.weightedContribution.x,
        max(result.weightedContribution.y, result.weightedContribution.z));
    result.directionalPdf = cosine * kUpt07LambertInvPi;
    result.lobeMask = kUpt04LobeDiffuse;
    result.valid = all(isfinite(result.weightedContribution)) ? 1u : 0u;
    return result;
}

public Upt04MaterialEvaluation Upt04EvaluatePrimaryDirectProbe(
    Upt04PrimarySurfaceRecord surface, float3 lightDirection,
    float3 incidentRadiance)
{
    return Upt04EvaluatePreparedDirectProbe(
        Upt04PreparePrimaryDirectProbe(surface), lightDirection, incidentRadiance);
}

public bool Upt04PrimaryContinuationAvailable(Upt04PrimarySurfaceRecord surface)
{ return Upt04PreparePrimaryDirectProbe(surface).valid != 0u; }

public Upt04PathEvent Upt04SamplePrimaryContinuation(
    Upt04PrimarySurfaceRecord surface, float lobeRandom, float2 directionRandom)
{
    Upt04PathEvent result = (Upt04PathEvent)0;
    const Upt04PreparedDirectMaterial prepared = Upt04PreparePrimaryDirectProbe(surface);
    if (prepared.valid == 0u || !isfinite(lobeRandom)
        || lobeRandom < 0.0f || lobeRandom >= 1.0f
        || !all(isfinite(directionRandom)) || any(directionRandom < 0.0f)
        || any(directionRandom >= 1.0f)) return result;
    const float radius = sqrt(directionRandom.x);
    const float phi = 6.283185307179586f * directionRandom.y;
    const float3 tangent = Upt07LambertPerpendicular(prepared.normal);
    const float3 bitangent = cross(prepared.normal, tangent);
    result.direction = Upt07LambertSafeNormalize(
        tangent * (radius * cos(phi)) + bitangent * (radius * sin(phi))
            + prepared.normal * sqrt(max(0.0f, 1.0f - directionRandom.x)),
        prepared.normal);
    result.pdf = saturate(dot(prepared.normal, result.direction))
        * kUpt07LambertInvPi;
    if (!Upt04FinitePositive(result.pdf)) return (Upt04PathEvent)0;
    result.throughput = kUpt07LambertAlbedo;
    result.lobeMask = kUpt04LobeDiffuse;
    result.valid = 1u;
    return result;
}

public struct Upt07LambertDiagnosticMaterialSet : IUpt07PrimaryMaterialSet
{
    static Upt04MaterialEvaluation EvaluatePrimaryDirect(
        Upt04PrimarySurfaceRecord surface, float3 lightDirection,
        float3 incidentRadiance)
    { return Upt04EvaluatePrimaryDirectProbe(surface, lightDirection, incidentRadiance); }
}
