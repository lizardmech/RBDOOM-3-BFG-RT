#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_MATH_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_MATH_HLSLI

// Thin-glass helpers for the clean RTXDI DI material-feature path.
//
// The shape follows the RTX Remix translucent-material audit, but this file is
// an rbdoom-owned HLSL slice: dirac thin glass, no roughness, no PSR resolver,
// no thick/nested medium tracking.

static const float RT_CLEAN_RTXDI_DI_GLASS_EPSILON = 1.0e-4;
static const float RT_CLEAN_RTXDI_DI_GLASS_MIN_TRANSMITTANCE = 1.0e-4;

float PathTraceCleanRtxdiDiGlassPow5(float value)
{
    const float value2 = value * value;
    return value2 * value2 * value;
}

float PathTraceCleanRtxdiDiGlassIorToF0(float incidentIor, float transmittedIor)
{
    const float denominator = max(incidentIor + transmittedIor, RT_CLEAN_RTXDI_DI_GLASS_EPSILON);
    const float reflectance = (incidentIor - transmittedIor) / denominator;
    return saturate(reflectance * reflectance);
}

float PathTraceCleanRtxdiDiGlassSchlickFresnel(float f0, float viewDotNormal)
{
    const float oneMinusCos = 1.0 - saturate(viewDotNormal);
    return saturate(f0 + (1.0 - f0) * PathTraceCleanRtxdiDiGlassPow5(oneMinusCos));
}

bool PathTraceCleanRtxdiDiGlassRefractionCosine(
    float relativeIor,
    float viewDotNormal,
    out float refractionDotNormal)
{
    const float sinSquared = relativeIor * relativeIor * max(0.0, 1.0 - viewDotNormal * viewDotNormal);
    if (sinSquared > 1.0)
    {
        refractionDotNormal = 0.0;
        return false;
    }

    refractionDotNormal = sqrt(max(0.0, 1.0 - sinSquared));
    return true;
}

float PathTraceCleanRtxdiDiGlassSchlickFresnelTir(
    float f0,
    float relativeIor,
    float viewDotNormal)
{
    float modifiedDot = saturate(viewDotNormal);
    if (relativeIor > 1.0 &&
        !PathTraceCleanRtxdiDiGlassRefractionCosine(relativeIor, modifiedDot, modifiedDot))
    {
        return 1.0;
    }
    return PathTraceCleanRtxdiDiGlassSchlickFresnel(f0, modifiedDot);
}

float3 PathTraceCleanRtxdiDiGlassTransmittanceToAttenuation(
    float3 transmittanceColor,
    float measurementDistance)
{
    const float3 safeTransmittance = max(saturate(transmittanceColor), float3(
        RT_CLEAN_RTXDI_DI_GLASS_MIN_TRANSMITTANCE,
        RT_CLEAN_RTXDI_DI_GLASS_MIN_TRANSMITTANCE,
        RT_CLEAN_RTXDI_DI_GLASS_MIN_TRANSMITTANCE));
    return -log(safeTransmittance) / max(measurementDistance, RT_CLEAN_RTXDI_DI_GLASS_EPSILON);
}

float3 PathTraceCleanRtxdiDiGlassBeerLambert(float3 attenuationCoefficient, float distance)
{
    return exp(-attenuationCoefficient * max(distance, 0.0));
}

float PathTraceCleanRtxdiDiGlassThinAttenuationDistance(
    float thickness,
    float3 normal,
    float3 viewDirection)
{
    return max(thickness, 0.0) / max(abs(dot(normal, viewDirection)), 0.05);
}

float3 PathTraceCleanRtxdiDiGlassThinGeometricSeries(float3 value)
{
    return 1.0 / max(1.0 - value * value, float3(
        RT_CLEAN_RTXDI_DI_GLASS_EPSILON,
        RT_CLEAN_RTXDI_DI_GLASS_EPSILON,
        RT_CLEAN_RTXDI_DI_GLASS_EPSILON));
}

float3 PathTraceCleanRtxdiDiGlassThinReflectionThroughput(
    float outsideFresnel,
    float insideFresnel,
    float3 attenuation)
{
    return outsideFresnel +
        insideFresnel * attenuation * attenuation *
        (1.0 - outsideFresnel) * (1.0 - insideFresnel) *
        PathTraceCleanRtxdiDiGlassThinGeometricSeries(attenuation * insideFresnel);
}

float3 PathTraceCleanRtxdiDiGlassThinTransmissionThroughput(
    float outsideFresnel,
    float insideFresnel,
    float3 attenuation)
{
    return attenuation * (1.0 - outsideFresnel) * (1.0 - insideFresnel) *
        PathTraceCleanRtxdiDiGlassThinGeometricSeries(attenuation * insideFresnel);
}

struct PathTraceCleanRtxdiDiGlassThinPayload
{
    float3 transmission;
    float weight;
    float3 reflection;
    float fresnel;
    float attenuationDistance;
};

PathTraceCleanRtxdiDiGlassThinPayload PathTraceCleanRtxdiDiBuildGlassThinPayload(
    RAB_Surface surface,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    PathTraceCleanRtxdiDiGlassThinPayload payload;

    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 viewDirection = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    const float ndotv = saturate(abs(dot(normal, viewDirection)));

    const float3 transmittanceColor = saturate(runtimeParams.params0.xyz);
    const float thickness = max(runtimeParams.params0.w, 0.0);
    const float glassIor = max(runtimeParams.params1.x, 1.0001);
    const float strength = saturate(runtimeParams.params1.y);

    const float f0 = PathTraceCleanRtxdiDiGlassIorToF0(1.0, glassIor);
    const float outsideFresnel = PathTraceCleanRtxdiDiGlassSchlickFresnel(f0, ndotv);
    const float insideFresnel = PathTraceCleanRtxdiDiGlassSchlickFresnelTir(f0, glassIor, ndotv);
    const float attenuationDistance = PathTraceCleanRtxdiDiGlassThinAttenuationDistance(
        thickness,
        normal,
        viewDirection);
    const float3 attenuationCoefficient = PathTraceCleanRtxdiDiGlassTransmittanceToAttenuation(
        transmittanceColor,
        1.0);
    const float3 attenuation = PathTraceCleanRtxdiDiGlassBeerLambert(
        attenuationCoefficient,
        attenuationDistance);

    payload.transmission = saturate(PathTraceCleanRtxdiDiGlassThinTransmissionThroughput(
        outsideFresnel,
        insideFresnel,
        attenuation));
    payload.weight = strength;
    payload.reflection = saturate(PathTraceCleanRtxdiDiGlassThinReflectionThroughput(
        outsideFresnel,
        insideFresnel,
        attenuation));
    payload.fresnel = outsideFresnel;
    payload.attenuationDistance = attenuationDistance;
    return payload;
}

#endif
