#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_MATH_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_GLASS_MATH_HLSLI

// Thin-glass helpers for the clean RTXDI DI material-feature path.
//
// The shape follows the RTX Remix translucent-material audit, but this file is
// an rbdoom-owned HLSL slice: dirac thin glass, no roughness, no PSR resolver,
// no thick/nested medium tracking.

static const float RT_CLEAN_RTXDI_DI_GLASS_EPSILON = 1.0e-4;
static const float RT_CLEAN_RTXDI_DI_GLASS_MIN_TRANSMITTANCE = 1.0e-4;
static const float RT_CLEAN_RTXDI_DI_GLASS_REFRACTION_PIXEL_SCALE = 120.0;
static const float RT_CLEAN_RTXDI_DI_GLASS_REFRACTION_MAX_PIXELS = 10.0;
static const float RT_CLEAN_RTXDI_DI_GLASS_REFLECTION_PIXEL_SCALE = 48.0;
static const float RT_CLEAN_RTXDI_DI_GLASS_REFLECTION_MAX_PIXELS = 6.0;

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

float PathTraceCleanRtxdiDiGlassThinRefractedAttenuationDistance(
    float thickness,
    float3 normal,
    float3 viewDirection,
    float glassIor)
{
    const float viewDotNormal = saturate(abs(dot(normal, viewDirection)));
    float refractedDotNormal;
    if (!PathTraceCleanRtxdiDiGlassRefractionCosine(
        rcp(max(glassIor, 1.0001)),
        viewDotNormal,
        refractedDotNormal))
    {
        return PathTraceCleanRtxdiDiGlassThinAttenuationDistance(thickness, normal, viewDirection);
    }

    return max(thickness, 0.0) / max(refractedDotNormal, 0.05);
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

float2 PathTraceCleanRtxdiDiGlassProjectScreenDirection(float3 direction)
{
    const float3 left = PathTraceCleanRoomSafeNormalize(CleanRtxdiDiCameraLeftAndTanY.xyz, float3(0.0, 1.0, 0.0));
    const float3 up = PathTraceCleanRoomSafeNormalize(CleanRtxdiDiCameraUpAndTanY.xyz, float3(0.0, 0.0, 1.0));
    return float2(-dot(direction, left), -dot(direction, up));
}

float2 PathTraceCleanRtxdiDiGlassNormalizeScreenDirection(float2 direction, float2 fallbackDirection)
{
    const float directionLengthSq = dot(direction, direction);
    if (directionLengthSq > RT_CLEAN_RTXDI_DI_GLASS_EPSILON)
    {
        return direction * rsqrt(directionLengthSq);
    }

    const float fallbackLengthSq = dot(fallbackDirection, fallbackDirection);
    if (fallbackLengthSq > RT_CLEAN_RTXDI_DI_GLASS_EPSILON)
    {
        return fallbackDirection * rsqrt(fallbackLengthSq);
    }

    return float2(0.0, 0.0);
}

float2 PathTraceCleanRtxdiDiGlassRefractedScreenDirection(
    float3 normal,
    float3 viewDirection,
    float ior)
{
    const float3 incidentDirection = -viewDirection;
    const float3 entryNormal = dot(normal, viewDirection) >= 0.0 ? normal : -normal;
    const float3 refractedDirection = refract(incidentDirection, entryNormal, rcp(max(ior, 1.0001)));
    const float2 fallbackDirection = PathTraceCleanRtxdiDiGlassProjectScreenDirection(normal);
    return PathTraceCleanRtxdiDiGlassNormalizeScreenDirection(
        PathTraceCleanRtxdiDiGlassProjectScreenDirection(refractedDirection - incidentDirection),
        fallbackDirection);
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
    PathTraceCleanRtxdiDiGlassMaterialParams materialParams)
{
    PathTraceCleanRtxdiDiGlassThinPayload payload;

    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 viewDirection = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    const float ndotv = saturate(abs(dot(normal, viewDirection)));

    const float3 transmittanceColor = saturate(materialParams.transmittanceColor);
    const float thickness = max(materialParams.thickness, 0.0);
    const float glassIor = max(materialParams.ior, 1.0001);
    const float strength = saturate(materialParams.strength);

    const float f0 = PathTraceCleanRtxdiDiGlassIorToF0(1.0, glassIor);
    const float outsideFresnel = PathTraceCleanRtxdiDiGlassSchlickFresnel(f0, ndotv);
    const float insideFresnel = PathTraceCleanRtxdiDiGlassSchlickFresnelTir(f0, glassIor, ndotv);
    const float attenuationDistance = PathTraceCleanRtxdiDiGlassThinRefractedAttenuationDistance(
        thickness,
        normal,
        viewDirection,
        glassIor);
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

PathTraceCleanRtxdiDiGlassThinPayload PathTraceCleanRtxdiDiBuildGlassThinPayload(
    RAB_Surface surface,
    PathTraceCleanRtxdiDiMaterialFeatureRuntimeParams runtimeParams)
{
    return PathTraceCleanRtxdiDiBuildGlassThinPayload(
        surface,
        PathTraceCleanRtxdiDiLoadGlassMaterialParams(surface, runtimeParams));
}

float2 PathTraceCleanRtxdiDiGlassRefractionPixelOffset(
    RAB_Surface surface,
    PathTraceCleanRtxdiDiGlassMaterialParams materialParams,
    PathTraceCleanRtxdiDiGlassThinPayload payload)
{
    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 viewDirection = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    const float ndotv = saturate(abs(dot(normal, viewDirection)));
    const float grazing = 1.0 - ndotv;
    const float glassIor = max(materialParams.ior, 1.0001);
    const float iorBend = saturate((glassIor - 1.0) / 0.7);
    const float pixelMagnitude = min(
        RT_CLEAN_RTXDI_DI_GLASS_REFRACTION_MAX_PIXELS,
        max(materialParams.thickness, 0.0) *
            RT_CLEAN_RTXDI_DI_GLASS_REFRACTION_PIXEL_SCALE *
            iorBend *
            saturate(payload.weight) *
            grazing);

    const float2 screenDirection = PathTraceCleanRtxdiDiGlassRefractedScreenDirection(
        normal,
        viewDirection,
        glassIor);
    if (dot(screenDirection, screenDirection) <= RT_CLEAN_RTXDI_DI_GLASS_EPSILON)
    {
        return float2(0.0, 0.0);
    }

    return screenDirection * pixelMagnitude;
}

float2 PathTraceCleanRtxdiDiGlassRefractionSamplePosition(
    uint2 pixel,
    uint2 dimensions,
    RAB_Surface surface,
    PathTraceCleanRtxdiDiGlassMaterialParams materialParams,
    PathTraceCleanRtxdiDiGlassThinPayload payload)
{
    return float2(pixel) + PathTraceCleanRtxdiDiGlassRefractionPixelOffset(
        surface,
        materialParams,
        payload);
}

float2 PathTraceCleanRtxdiDiGlassReflectionPixelOffset(
    RAB_Surface surface,
    PathTraceCleanRtxdiDiGlassMaterialParams materialParams,
    PathTraceCleanRtxdiDiGlassThinPayload payload)
{
    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float3 viewDirection = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    const float ndotv = saturate(abs(dot(normal, viewDirection)));
    const float grazing = 1.0 - ndotv;
    const float reflectionEnergy = saturate(max(payload.reflection.x, max(payload.reflection.y, payload.reflection.z)));
    const float pixelMagnitude = min(
        RT_CLEAN_RTXDI_DI_GLASS_REFLECTION_MAX_PIXELS,
        max(materialParams.thickness, 0.0) *
            RT_CLEAN_RTXDI_DI_GLASS_REFLECTION_PIXEL_SCALE *
            saturate(payload.weight) *
            reflectionEnergy *
            (0.25 + 0.75 * grazing));

    const float3 reflectedDirection = reflect(-viewDirection, normal);
    const float2 fallbackDirection = PathTraceCleanRtxdiDiGlassProjectScreenDirection(normal);
    const float2 screenDirection = PathTraceCleanRtxdiDiGlassNormalizeScreenDirection(
        PathTraceCleanRtxdiDiGlassProjectScreenDirection(reflectedDirection),
        fallbackDirection);
    if (dot(screenDirection, screenDirection) <= RT_CLEAN_RTXDI_DI_GLASS_EPSILON)
    {
        return float2(0.0, 0.0);
    }

    return screenDirection * pixelMagnitude;
}

float2 PathTraceCleanRtxdiDiGlassReflectionSamplePosition(
    uint2 pixel,
    uint2 dimensions,
    RAB_Surface surface,
    PathTraceCleanRtxdiDiGlassMaterialParams materialParams,
    PathTraceCleanRtxdiDiGlassThinPayload payload)
{
    return float2(pixel) + PathTraceCleanRtxdiDiGlassReflectionPixelOffset(
        surface,
        materialParams,
        payload);
}

float4 PathTraceCleanRtxdiDiComposeThinGlassColor(
    float4 currentColor,
    float4 sourceColor,
    float4 reflectedSourceColor,
    PathTraceCleanRtxdiDiGlassMaterialParams materialParams,
    PathTraceCleanRtxdiDiGlassThinPayload payload)
{
    const float payloadWeight = saturate(payload.weight);
    const float3 transmission = saturate(payload.transmission);
    const float3 reflectedThroughput = saturate(payload.reflection * materialParams.reflectionBoost);
    const float surfaceGlint = saturate(payload.fresnel * payload.fresnel * materialParams.reflectionBoost);
    const float3 transmittedColor = sourceColor.rgb * transmission;
    const float3 reflectedColor = surfaceGlint + reflectedSourceColor.rgb * reflectedThroughput;
    const float3 floorColor = transmission * materialParams.transmissionFloor;
    const float3 composedColor = saturate(transmittedColor + reflectedColor + floorColor);
    return float4(lerp(currentColor.rgb, composedColor, payloadWeight), currentColor.a);
}

#endif
