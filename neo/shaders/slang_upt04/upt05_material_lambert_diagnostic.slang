import upt04_geometry_contract;
import upt04_hit_contract;
import upt04_initial_bindings;
import upt04_light_contract;
import upt04_material_contract;
import upt04_material_emission;
import upt04_material_live_contract;
import upt04_path_event_contract;
import upt04_primary_surface_live_contract;

public static const float kUpt05LambertInvPi = 0.3183098861837907f;
public static const float3 kUpt05LambertDiagnosticAlbedo = float3(0.8f);

public float3 Upt05LambertSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return all(isfinite(value)) && lengthSquared > 1.0e-12f
        ? value * rsqrt(lengthSquared) : fallback;
}

public float3 Upt05LambertBuildPerpendicular(float3 normal)
{
    return Upt05LambertSafeNormalize(
        abs(normal.z) < 0.999f
            ? cross(float3(0.0f, 0.0f, 1.0f), normal)
            : cross(float3(0.0f, 1.0f, 0.0f), normal),
        float3(1.0f, 0.0f, 0.0f));
}

public Upt04PreparedDirectMaterial Upt05LambertPreparePrimary(
    Upt04PrimarySurfaceRecord surface)
{
    Upt04PreparedDirectMaterial result = (Upt04PreparedDirectMaterial)0;
    if (!Upt04PrimarySurfaceRecordValid(surface))
        return result;
    result.normal = Upt05LambertSafeNormalize(
        surface.shadingNormalAndOpacity.xyz,
        Upt05LambertSafeNormalize(
            surface.geometricNormalAndRoughness.xyz,
            float3(0.0f, 0.0f, 1.0f)));
    result.viewDirection = Upt05LambertSafeNormalize(
        surface.viewDirectionAndReserved.xyz, result.normal);
    result.baseColor = kUpt05LambertDiagnosticAlbedo;
    result.roughness = 1.0f;
    result.specularF0 = float3(0.0f);
    result.valid = 1u;
    return result;
}

public Upt04PreparedDirectMaterial Upt05LambertPrepareSecondary(
    Upt04GeometryHitSurface surface,
    float3 viewDirection)
{
    Upt04PreparedDirectMaterial result = (Upt04PreparedDirectMaterial)0;
    if (surface.valid == 0u)
        return result;
    result.normal = Upt05LambertSafeNormalize(
        surface.shadingNormal,
        Upt05LambertSafeNormalize(
            surface.geometricNormal, float3(0.0f, 0.0f, 1.0f)));
    result.viewDirection = Upt05LambertSafeNormalize(viewDirection, result.normal);
    result.baseColor = kUpt05LambertDiagnosticAlbedo;
    result.roughness = 1.0f;
    result.specularF0 = float3(0.0f);
    result.valid = 1u;
    return result;
}

public Upt04MaterialEvaluation Upt05LambertEvaluatePrepared(
    Upt04PreparedDirectMaterial prepared,
    float3 lightDirection,
    float3 incidentRadiance)
{
    Upt04MaterialEvaluation result = (Upt04MaterialEvaluation)0;
    if (prepared.valid == 0u || !all(isfinite(lightDirection))
        || !all(isfinite(incidentRadiance)))
        return result;
    const float3 direction = Upt05LambertSafeNormalize(
        lightDirection, prepared.normal);
    const float cosine = saturate(dot(prepared.normal, direction));
    result.weightedContribution = prepared.baseColor
        * max(incidentRadiance, float3(0.0f)) * cosine * kUpt05LambertInvPi;
    result.target = max(result.weightedContribution.x,
        max(result.weightedContribution.y, result.weightedContribution.z));
    result.directionalPdf = cosine * kUpt05LambertInvPi;
    result.lobeMask = kUpt04LobeDiffuse;
    result.valid = all(isfinite(result.weightedContribution)) ? 1u : 0u;
    return result;
}

public Upt04PathEvent Upt05LambertSamplePrimary(
    Upt04PrimarySurfaceRecord surface,
    float lobeRandom,
    float2 directionRandom)
{
    Upt04PathEvent result = (Upt04PathEvent)0;
    const Upt04PreparedDirectMaterial prepared =
        Upt05LambertPreparePrimary(surface);
    if (prepared.valid == 0u || !isfinite(lobeRandom)
        || lobeRandom < 0.0f || lobeRandom >= 1.0f
        || !all(isfinite(directionRandom)) || any(directionRandom < 0.0f)
        || any(directionRandom >= 1.0f))
        return result;
    const float radius = sqrt(directionRandom.x);
    const float phi = 6.283185307179586f * directionRandom.y;
    const float3 tangent = Upt05LambertBuildPerpendicular(prepared.normal);
    const float3 bitangent = cross(prepared.normal, tangent);
    result.direction = Upt05LambertSafeNormalize(
        tangent * (radius * cos(phi))
            + bitangent * (radius * sin(phi))
            + prepared.normal * sqrt(max(0.0f, 1.0f - directionRandom.x)),
        prepared.normal);
    result.pdf = saturate(dot(prepared.normal, result.direction))
        * kUpt05LambertInvPi;
    if (!Upt04FinitePositive(result.pdf))
        return (Upt04PathEvent)0;
    result.throughput = kUpt05LambertDiagnosticAlbedo;
    result.lobeMask = kUpt04LobeDiffuse;
    result.valid = 1u;
    return result;
}

// Retain real emitter identity, color, masking, and scale. The diagnostic
// replaces receiver/secondary BSDFs only; turning emitters into white constants
// would make the performance comparison exercise a different light universe.
public Upt04MaterialEmissionEvaluation Upt05LambertEvaluateEmissiveEndpoint(
    uint materialIndex,
    uint triangleClassAndFlags,
    float2 uv,
    float2 uvGradient0,
    float2 uvGradient1)
{
    Upt04MaterialEmissionEvaluation result = (Upt04MaterialEmissionEvaluation)0;
    if (materialIndex >= gUpt04Control.materialCount
        || !all(isfinite(uv)) || !all(isfinite(uvGradient0))
        || !all(isfinite(uvGradient1)))
        return result;
    const Upt04SmokeMaterialRecord material =
        Upt04LoadMaterialRecord(materialIndex);
    float3 emissiveTexel = float3(1.0f);
    uint emissiveTexelValid = 1u;
    if (material.emissiveTextureIndex != kUpt04InvalidIdentity)
    {
        if (material.emissiveTextureIndex >= gUpt04Control.logicalTextureCount)
            emissiveTexelValid = 0u;
        else
            emissiveTexel = gUpt04MaterialTextures[NonUniformResourceIndex(
                material.emissiveTextureIndex)].SampleGrad(
                    gUpt04MaterialSampler, frac(uv), uvGradient0, uvGradient1).xyz;
    }
    return Upt04FinalizeEmissiveEndpoint(
        material, triangleClassAndFlags, emissiveTexel, emissiveTexelValid,
        gUpt04Control.emissiveScale);
}
