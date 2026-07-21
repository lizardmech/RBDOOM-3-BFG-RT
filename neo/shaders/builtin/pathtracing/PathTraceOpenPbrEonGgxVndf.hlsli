#ifndef RB_PATH_TRACE_OPENPBR_EON_GGX_VNDF_HLSLI
#define RB_PATH_TRACE_OPENPBR_EON_GGX_VNDF_HLSLI

// Small rbdoom-owned HLSL rewrite of the OpenPBR EON diffuse and isotropic
// GGX Smith VNDF formulas. Keep this include opt-in so clean DI sentinel builds
// do not pay for the heavier opaque BRDF path before their split gate.

static const float RT_OPENPBR_PI = 3.14159265358979323846;
static const float RT_OPENPBR_RCP_PI = 0.31830988618379067154;
static const float RT_OPENPBR_TWO_PI = 6.28318530717958647692;
static const float RT_OPENPBR_FON_A = 0.5 - 2.0 / (3.0 * RT_OPENPBR_PI);
static const float RT_OPENPBR_FON_B = 2.0 / 3.0 - 28.0 / (15.0 * RT_OPENPBR_PI);


float PathTraceOpenPbrLuminance(float3 value)
{
    return dot(max(value, float3(0.0, 0.0, 0.0)), float3(0.2126, 0.7152, 0.0722));
}

float PathTraceOpenPbrMaxComponent(float3 value)
{
    return max(value.r, max(value.g, value.b));
}

float3 PathTraceOpenPbrLocalToWorld(float3 normal, float3 localDir)
{
    const float3 tangent = RAB_BuildPerpendicular(normal);
    const float3 bitangent = RAB_SafeNormalize(cross(normal, tangent), float3(0.0, 1.0, 0.0));
    return RAB_SafeNormalize(tangent * localDir.x + bitangent * localDir.y + normal * localDir.z, normal);
}

float3 PathTraceOpenPbrWorldToLocal(float3 normal, float3 worldDir)
{
    const float3 tangent = RAB_BuildPerpendicular(normal);
    const float3 bitangent = RAB_SafeNormalize(cross(normal, tangent), float3(0.0, 1.0, 0.0));
    return float3(dot(worldDir, tangent), dot(worldDir, bitangent), dot(worldDir, normal));
}

float PathTraceOpenPbrFonDirectionalAlbedoApprox(float mu, float roughness)
{
    const float muComp = 1.0 - saturate(mu);
    const float g =
        muComp * (0.0571085289 +
        muComp * (0.491881867 +
        muComp * (-0.332181442 +
        muComp * 0.0714429953)));
    return (1.0 + saturate(roughness) * g) / max(1.0 + RT_OPENPBR_FON_A * saturate(roughness), 1.0e-5);
}

float3 PathTraceOpenPbrEvaluateEonDiffuse(float3 albedo, float roughness, float3 normal, float3 lightDir, float3 viewDir)
{
    const float r = saturate(roughness);
    const float3 wiLocal = PathTraceOpenPbrWorldToLocal(normal, viewDir);
    const float3 woLocal = PathTraceOpenPbrWorldToLocal(normal, lightDir);
    const float muI = saturate(wiLocal.z);
    const float muO = saturate(woLocal.z);
    if (muI <= 0.0 || muO <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float s = dot(wiLocal, woLocal) - muI * muO;
    const float sOverT = s > 0.0 ? s / max(max(muI, muO), 1.0e-5) : s;
    const float af = 1.0 / max(1.0 + RT_OPENPBR_FON_A * r, 1.0e-5);
    const float3 singleScatter = albedo * RT_OPENPBR_RCP_PI * af * max(0.0, 1.0 + r * sOverT);

    const float efI = PathTraceOpenPbrFonDirectionalAlbedoApprox(muI, r);
    const float efO = PathTraceOpenPbrFonDirectionalAlbedoApprox(muO, r);
    const float avgEf = af * (1.0 + RT_OPENPBR_FON_B * r);
    const float3 denominator = max(float3(1.0e-5, 1.0e-5, 1.0e-5), float3(1.0, 1.0, 1.0) - albedo * (1.0 - avgEf));
    const float3 rhoMs = (albedo * albedo) * (avgEf / denominator);
    const float multiScatterScale =
        max(1.0e-5, 1.0 - efI) *
        max(1.0e-5, 1.0 - efO) /
        max(1.0e-5, 1.0 - avgEf);
    return singleScatter + rhoMs * (RT_OPENPBR_RCP_PI * multiScatterScale);
}

float3 PathTraceOpenPbrFresnelSchlick(float3 f0, float cosine)
{
    const float x = 1.0 - saturate(cosine);
    const float x2 = x * x;
    const float x5 = x2 * x2 * x;
    return saturate(f0) + (float3(1.0, 1.0, 1.0) - saturate(f0)) * x5;
}

float PathTraceOpenPbrGgxD(float noH, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = noH * noH * (a2 - 1.0) + 1.0;
    return a2 / max(RT_OPENPBR_PI * d * d, 1.0e-6);
}

float PathTraceOpenPbrSmithG1(float noV, float alpha)
{
    const float noV2 = noV * noV;
    if (noV2 <= 0.0)
    {
        return 0.0;
    }
    const float tan2 = max(0.0, (1.0 - noV2) / noV2);
    return 2.0 / (1.0 + sqrt(1.0 + alpha * alpha * tan2));
}

float3 PathTraceOpenPbrEvaluateGgxSpecular(float3 f0, float roughness, float3 normal, float3 lightDir, float3 viewDir)
{
    const float noL = saturate(dot(normal, lightDir));
    const float noV = saturate(dot(normal, viewDir));
    if (noL <= 0.0 || noV <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float alpha = max(saturate(roughness) * saturate(roughness), 1.0e-3);
    const float3 halfVector = RAB_SafeNormalize(lightDir + viewDir, normal);
    const float noH = saturate(dot(normal, halfVector));
    const float voH = saturate(dot(viewDir, halfVector));
    const float d = PathTraceOpenPbrGgxD(noH, alpha);
    const float g = PathTraceOpenPbrSmithG1(noL, alpha) * PathTraceOpenPbrSmithG1(noV, alpha);
    const float3 f = PathTraceOpenPbrFresnelSchlick(f0, voH);
    return f * (d * g / max(4.0 * noL * noV, 1.0e-5));
}

float PathTraceOpenPbrRoughMetalCompensationWeight(float3 f0, float roughness)
{
    const float f0Max = PathTraceOpenPbrMaxComponent(saturate(f0));
    const float metalProxy = saturate((f0Max - 0.08) / 0.92);
    const float rough = saturate((roughness - 0.18) / 0.82);
    return metalProxy * rough * rough;
}

float3 PathTraceOpenPbrEvaluateScalarMmsApprox(float3 f0, float roughness, float3 normal, float3 lightDir, float3 viewDir)
{
    const float noL = saturate(dot(normal, lightDir));
    const float noV = saturate(dot(normal, viewDir));
    if (noL <= 0.0 || noV <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    // Small table-free rough-metal energy recovery. This is not OpenPBR's
    // table-based MMS lobe; it is an rbdoom scalar approximation until the
    // energy tables are deliberately added.
    const float weight = PathTraceOpenPbrRoughMetalCompensationWeight(f0, roughness);
    const float grazingFade = lerp(0.55, 1.0, sqrt(saturate(noV * noL)));
    return saturate(f0) * (0.22 * weight * grazingFade * RT_OPENPBR_RCP_PI);
}

float PathTraceOpenPbrGgxReflectionPdf(float roughness, float3 normal, float3 lightDir, float3 viewDir)
{
    const float noL = saturate(dot(normal, lightDir));
    const float noV = saturate(dot(normal, viewDir));
    if (noL <= 0.0 || noV <= 0.0)
    {
        return 0.0;
    }
    const float alpha = max(saturate(roughness) * saturate(roughness), 1.0e-3);
    const float3 halfVector = RAB_SafeNormalize(lightDir + viewDir, normal);
    const float noH = saturate(dot(normal, halfVector));
    const float d = PathTraceOpenPbrGgxD(noH, alpha);
    const float g1 = PathTraceOpenPbrSmithG1(noV, alpha);
    return d * g1 / max(4.0 * noV, 1.0e-5);
}

float3 PathTraceOpenPbrSampleGgxVndfLocal(float alpha, float3 viewLocal, float2 randomValues)
{
    const float3 stretchedView = RAB_SafeNormalize(float3(alpha * viewLocal.x, alpha * viewLocal.y, viewLocal.z), float3(0.0, 0.0, 1.0));
    const float lensq = stretchedView.x * stretchedView.x + stretchedView.y * stretchedView.y;
    const float3 tangent = lensq > 0.0
        ? float3(-stretchedView.y, stretchedView.x, 0.0) * rsqrt(lensq)
        : float3(1.0, 0.0, 0.0);
    const float3 bitangent = cross(stretchedView, tangent);
    const float radius = sqrt(saturate(randomValues.x));
    const float phi = RT_OPENPBR_TWO_PI * saturate(randomValues.y);
    const float t1 = radius * cos(phi);
    const float t2raw = radius * sin(phi);
    const float s = 0.5 * (1.0 + stretchedView.z);
    const float t2 = lerp(sqrt(max(0.0, 1.0 - t1 * t1)), t2raw, s);
    const float nh = sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2));
    const float3 halfStretched = tangent * t1 + bitangent * t2 + stretchedView * nh;
    return RAB_SafeNormalize(float3(alpha * halfStretched.x, alpha * halfStretched.y, max(0.0, halfStretched.z)), float3(0.0, 0.0, 1.0));
}

bool PathTraceOpenPbrSampleGgxVndf(float roughness, float3 normal, float3 viewDir, float2 randomValues, out float3 lightDir)
{
    lightDir = float3(0.0, 0.0, 0.0);
    const float alpha = max(saturate(roughness) * saturate(roughness), 1.0e-3);
    const float3 viewLocal = PathTraceOpenPbrWorldToLocal(normal, viewDir);
    if (viewLocal.z <= 0.0)
    {
        return false;
    }
    const float3 halfLocal = PathTraceOpenPbrSampleGgxVndfLocal(alpha, viewLocal, randomValues);
    const float3 halfVector = PathTraceOpenPbrLocalToWorld(normal, halfLocal);
    lightDir = RAB_SafeNormalize(reflect(-viewDir, halfVector), normal);
    return dot(normal, lightDir) > 0.0;
}

float PathTraceOpenPbrSpecularSampleProbability(float3 f0, float roughness)
{
    const float specular = PathTraceOpenPbrLuminance(saturate(f0));
    const float compensation = PathTraceOpenPbrRoughMetalCompensationWeight(f0, roughness) * 0.25;
    const float diffuse = saturate(1.0 - specular) * lerp(1.0, 0.35, saturate(1.0 - roughness));
    return clamp((specular + compensation) / max(specular + compensation + diffuse, 1.0e-4), 0.05, 0.95);
}

#endif
