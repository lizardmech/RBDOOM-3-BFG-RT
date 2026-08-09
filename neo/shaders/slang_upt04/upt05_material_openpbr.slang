public static const float kUpt05PbrPi = 3.14159265358979323846f;
public static const float kUpt05PbrInvPi = 0.31830988618379067154f;
public static const float kUpt05PbrTwoPi = 6.28318530717958647692f;
public static const float kUpt05PbrFonA =
    0.5f - 2.0f / (3.0f * kUpt05PbrPi);
public static const float kUpt05PbrFonB =
    2.0f / 3.0f - 28.0f / (15.0f * kUpt05PbrPi);

public struct Upt05OpenPbrParameters
{
    public float3 baseColor;
    public float roughness;
    public float3 specularF0;
};

public struct Upt05OpenPbrEvaluation
{
    public float3 brdf;
    public float directionalPdf;
    public uint lobeMask;
    public uint valid;
};

public struct Upt05OpenPbrSample
{
    public float3 direction;
    public float directionalPdf;
    public float3 throughput;
    public uint lobeMask;
    public uint valid;
};

public float Upt05OpenPbrMaxComponent(float3 value)
{
    return max(value.x, max(value.y, value.z));
}

public float Upt05OpenPbrLuminance(float3 value)
{
    return dot(max(value, float3(0.0f)), float3(0.2126f, 0.7152f, 0.0722f));
}

public float3 Upt05OpenPbrSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return all(isfinite(value)) && isfinite(lengthSquared)
            && lengthSquared > 1.0e-12f
        ? value * rsqrt(lengthSquared)
        : fallback;
}

public float3 Upt05OpenPbrBuildPerpendicular(float3 normal)
{
    return Upt05OpenPbrSafeNormalize(
        abs(normal.z) < 0.999f
            ? cross(float3(0.0f, 0.0f, 1.0f), normal)
            : cross(float3(0.0f, 1.0f, 0.0f), normal),
        float3(1.0f, 0.0f, 0.0f));
}

public float3 Upt05OpenPbrLocalToWorld(float3 normal, float3 localDirection)
{
    const float3 tangent = Upt05OpenPbrBuildPerpendicular(normal);
    const float3 bitangent = Upt05OpenPbrSafeNormalize(
        cross(normal, tangent), float3(0.0f, 1.0f, 0.0f));
    return Upt05OpenPbrSafeNormalize(
        tangent * localDirection.x + bitangent * localDirection.y
            + normal * localDirection.z,
        normal);
}

public float3 Upt05OpenPbrWorldToLocal(float3 normal, float3 worldDirection)
{
    const float3 tangent = Upt05OpenPbrBuildPerpendicular(normal);
    const float3 bitangent = Upt05OpenPbrSafeNormalize(
        cross(normal, tangent), float3(0.0f, 1.0f, 0.0f));
    return float3(
        dot(worldDirection, tangent),
        dot(worldDirection, bitangent),
        dot(worldDirection, normal));
}

public float Upt05OpenPbrDirectionalAlbedoApprox(float cosine, float roughness)
{
    const float x = 1.0f - saturate(cosine);
    const float g = x * (0.0571085289f
        + x * (0.491881867f
        + x * (-0.332181442f + x * 0.0714429953f)));
    return (1.0f + saturate(roughness) * g)
        / max(1.0f + kUpt05PbrFonA * saturate(roughness), 1.0e-5f);
}

public float3 Upt05OpenPbrEvaluateEonDiffuse(
    float3 albedo,
    float roughness,
    float3 normal,
    float3 lightDirection,
    float3 viewDirection)
{
    const float noL = saturate(dot(normal, lightDirection));
    const float noV = saturate(dot(normal, viewDirection));
    if (noL <= 0.0f || noV <= 0.0f)
        return float3(0.0f);

    const float r = saturate(roughness);
    const float s = dot(viewDirection, lightDirection) - noV * noL;
    const float sOverT = s > 0.0f
        ? s / max(max(noV, noL), 1.0e-5f)
        : s;
    const float af = 1.0f / max(1.0f + kUpt05PbrFonA * r, 1.0e-5f);
    const float3 singleScatter = albedo * kUpt05PbrInvPi * af
        * max(0.0f, 1.0f + r * sOverT);

    const float efV = Upt05OpenPbrDirectionalAlbedoApprox(noV, r);
    const float efL = Upt05OpenPbrDirectionalAlbedoApprox(noL, r);
    const float averageEf = af * (1.0f + kUpt05PbrFonB * r);
    const float3 denominator = max(
        float3(1.0e-5f),
        float3(1.0f) - albedo * (1.0f - averageEf));
    const float3 rhoMs = albedo * albedo * (averageEf / denominator);
    const float multiScatterScale = max(1.0e-5f, 1.0f - efV)
        * max(1.0e-5f, 1.0f - efL)
        / max(1.0e-5f, 1.0f - averageEf);
    return singleScatter
        + rhoMs * (kUpt05PbrInvPi * multiScatterScale);
}

public float3 Upt05OpenPbrFresnelSchlick(float3 f0, float cosine)
{
    const float x = 1.0f - saturate(cosine);
    const float x2 = x * x;
    const float x5 = x2 * x2 * x;
    return saturate(f0) + (float3(1.0f) - saturate(f0)) * x5;
}

public float Upt05OpenPbrGgxD(float noH, float alpha)
{
    const float alphaSquared = alpha * alpha;
    const float denominator = noH * noH * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared
        / max(kUpt05PbrPi * denominator * denominator, 1.0e-6f);
}

public float Upt05OpenPbrSmithG1(float noV, float alpha)
{
    const float noVSquared = noV * noV;
    if (noVSquared <= 0.0f)
        return 0.0f;
    const float tangentSquared = max(
        0.0f, (1.0f - noVSquared) / noVSquared);
    return 2.0f / (1.0f + sqrt(
        1.0f + alpha * alpha * tangentSquared));
}

public float3 Upt05OpenPbrEvaluateGgx(
    float3 f0,
    float roughness,
    float3 normal,
    float3 lightDirection,
    float3 viewDirection)
{
    const float noL = saturate(dot(normal, lightDirection));
    const float noV = saturate(dot(normal, viewDirection));
    if (noL <= 0.0f || noV <= 0.0f)
        return float3(0.0f);
    const float3 halfVector = Upt05OpenPbrSafeNormalize(
        lightDirection + viewDirection, normal);
    const float noH = saturate(dot(normal, halfVector));
    const float voH = saturate(dot(viewDirection, halfVector));
    const float alpha = max(roughness * roughness, 1.0e-3f);
    const float d = Upt05OpenPbrGgxD(noH, alpha);
    const float g = Upt05OpenPbrSmithG1(noL, alpha)
        * Upt05OpenPbrSmithG1(noV, alpha);
    return Upt05OpenPbrFresnelSchlick(f0, voH)
        * (d * g / max(4.0f * noL * noV, 1.0e-5f));
}

public float Upt05OpenPbrGgxReflectionPdf(
    float roughness,
    float3 normal,
    float3 lightDirection,
    float3 viewDirection)
{
    const float noL = saturate(dot(normal, lightDirection));
    const float noV = saturate(dot(normal, viewDirection));
    if (noL <= 0.0f || noV <= 0.0f)
        return 0.0f;
    const float3 halfVector = Upt05OpenPbrSafeNormalize(
        lightDirection + viewDirection, normal);
    const float noH = saturate(dot(normal, halfVector));
    const float alpha = max(roughness * roughness, 1.0e-3f);
    return Upt05OpenPbrGgxD(noH, alpha)
        * Upt05OpenPbrSmithG1(noV, alpha)
        / max(4.0f * noV, 1.0e-5f);
}

public float Upt05OpenPbrSpecularProbability(
    float3 f0,
    float roughness)
{
    const float specular = Upt05OpenPbrLuminance(saturate(f0));
    const float diffuse = saturate(1.0f - specular)
        * lerp(1.0f, 0.35f, saturate(1.0f - roughness));
    return clamp(specular / max(specular + diffuse, 1.0e-4f), 0.05f, 0.95f);
}

public Upt05OpenPbrEvaluation Upt05OpenPbrEvaluate(
    Upt05OpenPbrParameters material,
    float3 normal,
    float3 lightDirection,
    float3 viewDirection)
{
    normal = Upt05OpenPbrSafeNormalize(normal, float3(0.0f, 0.0f, 1.0f));
    viewDirection = Upt05OpenPbrSafeNormalize(viewDirection, normal);
    material.baseColor = saturate(material.baseColor);
    material.specularF0 = saturate(material.specularF0);
    material.roughness = clamp(material.roughness, 0.02f, 1.0f);
    return Upt05OpenPbrEvaluatePrepared(
        material, normal, lightDirection, viewDirection);
}

// Evaluates direction-dependent terms for a receiver whose normal, view, and
// material parameters were already normalized/clamped. This is the inner-loop
// entry used by prepared direct receivers; the public wrapper above retains
// the original defensive contract for all other callers.
public Upt05OpenPbrEvaluation Upt05OpenPbrEvaluatePrepared(
    Upt05OpenPbrParameters material,
    float3 normal,
    float3 lightDirection,
    float3 viewDirection)
{
    Upt05OpenPbrEvaluation result = (Upt05OpenPbrEvaluation)0;
    lightDirection = Upt05OpenPbrSafeNormalize(lightDirection, normal);
    const float noL = saturate(dot(normal, lightDirection));
    const float noV = saturate(dot(normal, viewDirection));
    if (noL <= 0.0f || noV <= 0.0f)
        return result;

    const float diffuseScale = saturate(
        1.0f - Upt05OpenPbrMaxComponent(material.specularF0));
    const float3 diffuse = Upt05OpenPbrEvaluateEonDiffuse(
        material.baseColor * diffuseScale,
        material.roughness,
        normal,
        lightDirection,
        viewDirection);
    const float3 specular = Upt05OpenPbrEvaluateGgx(
        material.specularF0,
        material.roughness,
        normal,
        lightDirection,
        viewDirection);
    const float diffusePdf = noL * kUpt05PbrInvPi;
    const float specularPdf = Upt05OpenPbrGgxReflectionPdf(
        material.roughness,
        normal,
        lightDirection,
        viewDirection);
    const float specularProbability = Upt05OpenPbrSpecularProbability(
        material.specularF0, material.roughness);
    result.brdf = diffuse + specular;
    result.directionalPdf = lerp(
        diffusePdf, specularPdf, specularProbability);
    const bool hasDiffuse = any(material.baseColor * diffuseScale > 0.0f);
    const bool hasSpecular = any(material.specularF0 > 0.0f);
    result.lobeMask = (hasDiffuse ? 1u : 0u) | (hasSpecular ? 2u : 0u);
    if (result.lobeMask == 0u)
        result.lobeMask = 1u;
    result.valid = all(isfinite(result.brdf))
            && all(result.brdf >= 0.0f)
            && isfinite(result.directionalPdf)
            && result.directionalPdf > 0.0f
        ? 1u
        : 0u;
    return result;
}

public float3 Upt05OpenPbrSampleGgxVndfLocal(
    float alpha,
    float3 viewLocal,
    float2 randomValues)
{
    const float3 stretchedView = Upt05OpenPbrSafeNormalize(
        float3(alpha * viewLocal.x, alpha * viewLocal.y, viewLocal.z),
        float3(0.0f, 0.0f, 1.0f));
    const float lensq = dot(stretchedView.xy, stretchedView.xy);
    const float3 tangent = lensq > 0.0f
        ? float3(-stretchedView.y, stretchedView.x, 0.0f) * rsqrt(lensq)
        : float3(1.0f, 0.0f, 0.0f);
    const float3 bitangent = cross(stretchedView, tangent);
    const float radius = sqrt(saturate(randomValues.x));
    const float phi = kUpt05PbrTwoPi * saturate(randomValues.y);
    const float t1 = radius * cos(phi);
    const float t2Disk = radius * sin(phi);
    const float s = 0.5f * (1.0f + stretchedView.z);
    const float t2 = lerp(
        sqrt(max(0.0f, 1.0f - t1 * t1)), t2Disk, s);
    const float nh = sqrt(max(0.0f, 1.0f - t1 * t1 - t2 * t2));
    const float3 halfStretched = tangent * t1
        + bitangent * t2 + stretchedView * nh;
    return Upt05OpenPbrSafeNormalize(
        float3(
            alpha * halfStretched.x,
            alpha * halfStretched.y,
            max(0.0f, halfStretched.z)),
        float3(0.0f, 0.0f, 1.0f));
}

public bool Upt05OpenPbrSampleGgxVndf(
    float roughness,
    float3 normal,
    float3 viewDirection,
    float2 randomValues,
    out float3 lightDirection)
{
    lightDirection = float3(0.0f);
    const float3 viewLocal = Upt05OpenPbrWorldToLocal(normal, viewDirection);
    if (viewLocal.z <= 0.0f)
        return false;
    const float alpha = max(roughness * roughness, 1.0e-3f);
    const float3 halfLocal = Upt05OpenPbrSampleGgxVndfLocal(
        alpha, viewLocal, randomValues);
    const float3 halfVector = Upt05OpenPbrLocalToWorld(normal, halfLocal);
    lightDirection = Upt05OpenPbrSafeNormalize(
        reflect(-viewDirection, halfVector), normal);
    return dot(normal, lightDirection) > 0.0f;
}

public Upt05OpenPbrSample Upt05SampleOpenPbr(
    Upt05OpenPbrParameters material,
    float3 normal,
    float3 viewDirection,
    float lobeRandom,
    float2 directionRandom)
{
    Upt05OpenPbrSample result = (Upt05OpenPbrSample)0;
    if (!isfinite(lobeRandom) || lobeRandom < 0.0f || lobeRandom >= 1.0f
        || !all(isfinite(directionRandom)) || any(directionRandom < 0.0f)
        || any(directionRandom >= 1.0f))
        return result;

    normal = Upt05OpenPbrSafeNormalize(normal, float3(0.0f, 0.0f, 1.0f));
    viewDirection = Upt05OpenPbrSafeNormalize(viewDirection, normal);
    material.baseColor = saturate(material.baseColor);
    material.specularF0 = saturate(material.specularF0);
    material.roughness = clamp(material.roughness, 0.02f, 1.0f);
    if (dot(normal, viewDirection) <= 0.0f)
        return result;

    const float specularProbability = Upt05OpenPbrSpecularProbability(
        material.specularF0, material.roughness);
    const bool selectedSpecular = lobeRandom < specularProbability;
    float3 direction = float3(0.0f);
    if (selectedSpecular)
    {
        if (!Upt05OpenPbrSampleGgxVndf(
                material.roughness,
                normal,
                viewDirection,
                directionRandom,
                direction))
            return result;
    }
    else
    {
        const float radius = sqrt(directionRandom.x);
        const float phi = kUpt05PbrTwoPi * directionRandom.y;
        const float3 localDirection = float3(
            radius * cos(phi),
            radius * sin(phi),
            sqrt(max(0.0f, 1.0f - directionRandom.x)));
        direction = Upt05OpenPbrLocalToWorld(normal, localDirection);
    }

    const Upt05OpenPbrEvaluation evaluated = Upt05OpenPbrEvaluate(
        material, normal, direction, viewDirection);
    const float noL = saturate(dot(normal, direction));
    if (evaluated.valid == 0u || noL <= 0.0f)
        return result;
    result.direction = direction;
    result.directionalPdf = evaluated.directionalPdf;
    result.throughput = evaluated.brdf * noL / evaluated.directionalPdf;
    result.lobeMask = selectedSpecular ? 2u : 1u;
    result.valid = all(isfinite(result.throughput))
            && all(result.throughput >= 0.0f)
        ? 1u
        : 0u;
    return result;
}
