#ifndef RB_PATH_TRACE_PRIMARY_SURFACE_HLSLI
#define RB_PATH_TRACE_PRIMARY_SURFACE_HLSLI

static const uint RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION = 2u;
static const uint RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_STRIDE = 176u;

static const uint RT_PRIMARY_SURFACE_VALID = 0x00000001u;
static const uint RT_PRIMARY_SURFACE_HAS_CAMERA_REPROJECTION = 0x00000002u;
static const uint RT_PRIMARY_SURFACE_HAS_OBJECT_MOTION = 0x00000004u;
static const uint RT_PRIMARY_SURFACE_HAS_PREVIOUS_POSITION = 0x00000008u;

static const uint RT_PRIMARY_SURFACE_DEBUG_OK = 0u;
static const uint RT_PRIMARY_SURFACE_DEBUG_VALID_VECTOR = 0u;
static const uint RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT = 1u;
static const uint RT_PRIMARY_SURFACE_DEBUG_MISSING_PREVIOUS = 2u;
static const uint RT_PRIMARY_SURFACE_DEBUG_REJECTED_PREVIOUS = 3u;
static const uint RT_PRIMARY_SURFACE_DEBUG_MATERIAL_MISMATCH = 4u;
static const uint RT_PRIMARY_SURFACE_DEBUG_NORMAL_MISMATCH = 5u;
static const uint RT_PRIMARY_SURFACE_DEBUG_ROUGHNESS_MISMATCH = 6u;
static const uint RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION = 7u;
static const uint RT_PRIMARY_SURFACE_DEBUG_SKINNED_MISSING_PREVIOUS = 8u;
static const uint RT_PRIMARY_SURFACE_DEBUG_SKINNED_RANGE_MISMATCH = 9u;
static const uint RT_PRIMARY_SURFACE_DEBUG_SKINNED_PREVIOUS_OUT_OF_RANGE = 10u;
static const uint RT_PRIMARY_SURFACE_DEBUG_RIGID_MISSING_PREVIOUS = 11u;
static const uint RT_PRIMARY_SURFACE_DEBUG_RIGID_RANGE_MISMATCH = 12u;
static const uint RT_PRIMARY_SURFACE_DEBUG_MISSING_PREVIOUS_CAMERA = 13u;
static const uint RT_PRIMARY_SURFACE_DEBUG_PREVIOUS_BEHIND_CAMERA = 14u;
static const uint RT_PRIMARY_SURFACE_DEBUG_PREVIOUS_XY_OUTSIDE_FRAME = 15u;
static const uint RT_PRIMARY_SURFACE_DEBUG_STATIC_RANGE_MISMATCH = 16u;
static const uint RT_PRIMARY_SURFACE_DEBUG_STATIC_MATERIAL_CLASS_MISMATCH = 17u;
static const uint RT_PRIMARY_SURFACE_DEBUG_PACKED_OBJECT_MISSING_PREVIOUS_MOTION = 18u;

struct PathTracePrimarySurfaceRecord
{
    uint4 header;                       // version, valid flags, debug/status flags, triangle/class flags
    float4 worldPositionAndViewDepth;   // xyz = world position, w = view depth
    float4 geometricNormalAndRoughness; // xyz = geometric normal, w = roughness
    float4 shadingNormalAndOpacity;     // xyz = shading normal, w = opacity
    float4 viewDirectionAndReserved;    // xyz = view direction, w = reserved
    float4 albedoAndAlphaCutoff;        // xyz = diffuse albedo, w = alpha cutoff
    float4 specularF0AndReserved;       // xyz = F0/specular, w = reserved
    float4 emissiveAndHeight;           // xyz = emissive radiance, w = height/parallax/displacement placeholder
    float4 previousPositionOrMotion;    // xyz = previous world position or motion placeholder, w = valid selector
    uint4 materialAndSurface;           // material id, material index, material flags, surface class
    uint4 instancePrimitiveObject;      // instance id, primitive id, object/entity id placeholder, emissive texture index
};

#endif

#if defined(RB_PATH_TRACE_PRIMARY_SURFACE_ENABLE_PROJECTION_HELPERS) || defined(RB_PATH_TRACE_PRIMARY_SURFACE_ENABLE_HELPERS)
#ifndef RB_PATH_TRACE_PRIMARY_SURFACE_PROJECTION_HELPERS
#define RB_PATH_TRACE_PRIMARY_SURFACE_PROJECTION_HELPERS

uint PathTracePrimarySurfacePreviousProjectionStatus(float3 worldPosition, uint2 dimensions, out float2 projectedPixelFloat, out float previousLinearDepth)
{
    projectedPixelFloat = float2(-1.0, -1.0);
    previousLinearDepth = 0.0;
    if (PrevCameraOriginAndValid.w < 0.5)
    {
        return RT_PRIMARY_SURFACE_DEBUG_MISSING_PREVIOUS_CAMERA;
    }

    const float3 delta = worldPosition - PrevCameraOriginAndValid.xyz;
    const float forwardDistance = dot(delta, PrevCameraForwardAndTanX.xyz);
    if (forwardDistance <= 0.05)
    {
        return RT_PRIMARY_SURFACE_DEBUG_PREVIOUS_BEHIND_CAMERA;
    }
    const bool projectedDepthEnabled = RayReconstructionInfo.w > 0.0;
    if (projectedDepthEnabled)
    {
        const float zNear = max(RayReconstructionInfo.w, 1.0e-4);
        previousLinearDepth = saturate(0.999 - zNear / max(forwardDistance, zNear));
    }
    else
    {
        previousLinearDepth = length(delta);
    }

    const float ndcX = -dot(delta, PrevCameraLeftAndTanY.xyz) / max(forwardDistance * PrevCameraForwardAndTanX.w, 1.0e-5);
    const float ndcY = -dot(delta, PrevCameraUpAndTanY.xyz) / max(forwardDistance * PrevCameraLeftAndTanY.w, 1.0e-5);
    projectedPixelFloat = (float2(ndcX, ndcY) * 0.5 + 0.5) * float2(dimensions);
    if (!all(projectedPixelFloat == projectedPixelFloat))
    {
        return RT_PRIMARY_SURFACE_DEBUG_REJECTED_PREVIOUS;
    }
    if (abs(ndcX) > 1.0 || abs(ndcY) > 1.0)
    {
        return RT_PRIMARY_SURFACE_DEBUG_PREVIOUS_XY_OUTSIDE_FRAME;
    }
    if (projectedPixelFloat.x < 0.0 || projectedPixelFloat.y < 0.0 ||
        projectedPixelFloat.x >= (float)dimensions.x || projectedPixelFloat.y >= (float)dimensions.y)
    {
        return RT_PRIMARY_SURFACE_DEBUG_PREVIOUS_XY_OUTSIDE_FRAME;
    }

    return RT_PRIMARY_SURFACE_DEBUG_VALID_VECTOR;
}

bool ProjectPathTracePrimarySurfaceToPreviousPixelFloatAndDepthWithStatus(float3 worldPosition, uint2 dimensions, out float2 previousPixelFloat, out float previousLinearDepth, out uint debugStatus)
{
    debugStatus = PathTracePrimarySurfacePreviousProjectionStatus(worldPosition, dimensions, previousPixelFloat, previousLinearDepth);
    if (debugStatus != RT_PRIMARY_SURFACE_DEBUG_VALID_VECTOR &&
        debugStatus != RT_PRIMARY_SURFACE_DEBUG_PREVIOUS_XY_OUTSIDE_FRAME)
    {
        previousPixelFloat = float2(-1.0, -1.0);
        return false;
    }

    return true;
}

bool ProjectPathTracePrimarySurfaceToPreviousPixelFloatAndDepth(float3 worldPosition, uint2 dimensions, out float2 previousPixelFloat, out float previousLinearDepth)
{
    uint debugStatus;
    return ProjectPathTracePrimarySurfaceToPreviousPixelFloatAndDepthWithStatus(worldPosition, dimensions, previousPixelFloat, previousLinearDepth, debugStatus);
}

bool ProjectPathTracePrimarySurfaceToPreviousPixelFloat(float3 worldPosition, uint2 dimensions, out float2 previousPixelFloat)
{
    float previousLinearDepth;
    return ProjectPathTracePrimarySurfaceToPreviousPixelFloatAndDepth(worldPosition, dimensions, previousPixelFloat, previousLinearDepth);
}

float PathTracePrimarySurfaceCurrentViewZ(float3 worldPosition)
{
    return dot(worldPosition - CameraOriginAndTMax.xyz, CameraForwardAndTanX.xyz);
}

float PathTracePrimarySurfacePreviousViewZ(float3 worldPosition)
{
    return dot(worldPosition - PrevCameraOriginAndValid.xyz, PrevCameraForwardAndTanX.xyz);
}

float PathTracePrimarySurfaceProjectionDepthFromViewZ(float viewZ)
{
    const float zNear = max(RayReconstructionInfo.w, 1.0e-4);
    const float safeViewZ = max(viewZ, zNear);
#if defined(RB_PATH_TRACE_PRIMARY_SURFACE_HAS_RR_PROJECTION_DEPTH_INFO)
    const float cameraZ = -safeViewZ;
    const float clipZ = RRProjectionDepthInfo.x * cameraZ + RRProjectionDepthInfo.y;
    const float clipW = RRProjectionDepthInfo.z * cameraZ + RRProjectionDepthInfo.w;
    if (abs(clipW) > 1.0e-6 && all(RRProjectionDepthInfo == RRProjectionDepthInfo))
    {
        return saturate(clipZ / clipW);
    }
#endif
    return saturate(0.999 - zNear / safeViewZ);
}

float PathTracePrimarySurfaceCurrentProjectionDepth(float3 worldPosition)
{
    if (RayReconstructionInfo.w <= 0.0)
    {
        return length(worldPosition - CameraOriginAndTMax.xyz);
    }
    return PathTracePrimarySurfaceProjectionDepthFromViewZ(PathTracePrimarySurfaceCurrentViewZ(worldPosition));
}

float PathTracePrimarySurfacePreviousProjectionDepth(float3 worldPosition)
{
    if (RayReconstructionInfo.w <= 0.0)
    {
        return length(worldPosition - PrevCameraOriginAndValid.xyz);
    }
    return PathTracePrimarySurfaceProjectionDepthFromViewZ(PathTracePrimarySurfacePreviousViewZ(worldPosition));
}

#endif
#endif

#ifdef RB_PATH_TRACE_PRIMARY_SURFACE_ENABLE_HELPERS
#ifndef RB_PATH_TRACE_PRIMARY_SURFACE_HELPERS
#define RB_PATH_TRACE_PRIMARY_SURFACE_HELPERS

#include "PathTraceMaterialFeature.hlsli"

PathTracePrimarySurfaceRecord PackPathTracePrimarySurfaceRecord(RAB_Surface surface)
{
    PathTracePrimarySurfaceRecord record = (PathTracePrimarySurfaceRecord)0;
    record.header.x = RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION;
    if (!RAB_IsSurfaceValid(surface))
    {
        record.header.z = RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT;
        return record;
    }

    uint validFlags = RT_PRIMARY_SURFACE_VALID;
    if (PrevCameraOriginAndValid.w >= 0.5)
    {
        validFlags |= RT_PRIMARY_SURFACE_HAS_CAMERA_REPROJECTION;
    }

    uint debugStatus = RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION;
    float3 previousWorldPosition = float3(0.0, 0.0, 0.0);
#ifdef RB_PATH_TRACE_PRIMARY_SURFACE_ENABLE_OBJECT_MOTION
    uint objectMotionDebugStatus = RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION;
    if (TryPathTracePrimarySurfaceObjectMotion(surface, previousWorldPosition, objectMotionDebugStatus))
    {
        validFlags |= RT_PRIMARY_SURFACE_HAS_OBJECT_MOTION | RT_PRIMARY_SURFACE_HAS_PREVIOUS_POSITION;
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_OK;
    }
    else
    {
        debugStatus = objectMotionDebugStatus;
    }
#endif

    record.header = uint4(RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION, validFlags, debugStatus, surface.flags);
    record.worldPositionAndViewDepth = float4(surface.worldPos, surface.linearDepth);
    record.geometricNormalAndRoughness = float4(surface.geometryNormal, surface.material.roughness);
    record.shadingNormalAndOpacity = float4(surface.shadingNormal, surface.material.opacity);
    record.viewDirectionAndReserved = float4(surface.viewDir, 0.0);
    record.albedoAndAlphaCutoff = float4(surface.material.diffuseAlbedo, surface.material.alphaCutoff);
    record.specularF0AndReserved = float4(surface.material.specularF0, 0.0);
    record.emissiveAndHeight = float4(surface.material.emissiveRadiance, 0.0);
    record.previousPositionOrMotion = (validFlags & RT_PRIMARY_SURFACE_HAS_PREVIOUS_POSITION) != 0u
        ? float4(previousWorldPosition, 1.0)
        : float4(0.0, 0.0, 0.0, 0.0);
    record.materialAndSurface = uint4(surface.materialId, surface.materialIndex, surface.material.flags, surface.surfaceClass);
    // TODO: replace the object/entity placeholder with the chosen Doom renderer/game identity
    // once the long-lived ID source is confirmed for map transitions, entity updates, and save/load.
    record.instancePrimitiveObject = uint4(surface.instanceId, surface.primitiveIndex, 0u, surface.material.emissiveTextureIndex);
    return record;
}

bool PathTracePrimarySurfaceRecordHasObjectMotion(PathTracePrimarySurfaceRecord record)
{
    const uint requiredFlags = RT_PRIMARY_SURFACE_HAS_OBJECT_MOTION | RT_PRIMARY_SURFACE_HAS_PREVIOUS_POSITION;
    return record.header.x == RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION &&
        (record.header.y & requiredFlags) == requiredFlags &&
        record.previousPositionOrMotion.w >= 0.5;
}

uint PathTracePrimarySurfaceMissingPackedObjectMotionStatus(RAB_Surface surface, uint packedStatus)
{
    if (packedStatus != RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION)
    {
        return packedStatus;
    }
    if (surface.surfaceClass == RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED)
    {
        return RT_PRIMARY_SURFACE_DEBUG_SKINNED_MISSING_PREVIOUS;
    }
    if (surface.surfaceClass == RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY)
    {
        return RT_PRIMARY_SURFACE_DEBUG_RIGID_MISSING_PREVIOUS;
    }
    return RT_PRIMARY_SURFACE_DEBUG_PACKED_OBJECT_MISSING_PREVIOUS_MOTION;
}

RAB_Surface UnpackPathTracePrimarySurfaceRecord(PathTracePrimarySurfaceRecord record)
{
    RAB_Surface surface = RAB_EmptySurface();
    if (record.header.x != RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION ||
        (record.header.y & RT_PRIMARY_SURFACE_VALID) == 0u)
    {
        return surface;
    }

    surface.valid = 1u;
    surface.worldPos = record.worldPositionAndViewDepth.xyz;
    surface.linearDepth = record.worldPositionAndViewDepth.w;
    surface.geometryNormal = SafeNormalize(record.geometricNormalAndRoughness.xyz, float3(0.0, 0.0, 1.0));
    surface.shadingNormal = ConstrainSmokeShadingNormal(record.shadingNormalAndOpacity.xyz, surface.geometryNormal);
    surface.viewDir = SafeNormalize(record.viewDirectionAndReserved.xyz, -surface.shadingNormal);
    surface.materialId = record.materialAndSurface.x;
    surface.materialIndex = record.materialAndSurface.y;
    surface.flags = record.header.w;
    surface.surfaceClass = record.materialAndSurface.w;
    surface.instanceId = record.instancePrimitiveObject.x;
    surface.primitiveIndex = record.instancePrimitiveObject.y;

    RAB_Material material = RAB_EmptyMaterial();
    material.materialId = surface.materialId;
    material.materialIndex = surface.materialIndex;
    material.flags = record.materialAndSurface.z;
    material.alphaCutoff = record.albedoAndAlphaCutoff.w;
    material.diffuseAlbedo = record.albedoAndAlphaCutoff.xyz;
    material.roughness = saturate(record.geometricNormalAndRoughness.w);
    material.specularF0 = max(record.specularF0AndReserved.xyz, float3(0.0, 0.0, 0.0));
    material.opacity = saturate(record.shadingNormalAndOpacity.w);
    material.emissiveRadiance = max(record.emissiveAndHeight.xyz, float3(0.0, 0.0, 0.0));
    material.emissiveTextureIndex = record.instancePrimitiveObject.w;
    surface.material = material;
    return surface;
}

void StorePathTracePrimarySurfaceRecord(uint2 pixel, RAB_Surface surface)
{
    if (PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_PRIMARY_SURFACE_HISTORY))
    {
        return;
    }

    pixel = PathTracePrimarySurfaceStorePixel(pixel);
    const uint2 dimensions = PathTraceFullOutputSize();
    if (pixel.x >= dimensions.x || pixel.y >= dimensions.y)
    {
        return;
    }

    const uint index = pixel.y * dimensions.x + pixel.x;
    if (index >= PathTracePrimarySurfaceHistoryCount())
    {
        return;
    }

    PrimarySurfaceHistoryCurrent[index] = PackPathTracePrimarySurfaceRecord(surface);
}

RAB_Surface LoadPathTracePrimarySurfaceRecord(int2 pixelPosition, bool previousFrame)
{
    if (PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_PRIMARY_SURFACE_HISTORY))
    {
        return RAB_EmptySurface();
    }

    pixelPosition = PathTracePrimarySurfaceLoadPixel(pixelPosition, previousFrame);
    const uint2 dimensions = PathTraceFullOutputSize();
    if (pixelPosition.x < 0 || pixelPosition.y < 0 ||
        (uint)pixelPosition.x >= dimensions.x || (uint)pixelPosition.y >= dimensions.y)
    {
        return RAB_EmptySurface();
    }

    const uint index = (uint)pixelPosition.y * dimensions.x + (uint)pixelPosition.x;
    if (index >= PathTracePrimarySurfaceHistoryCount())
    {
        return RAB_EmptySurface();
    }

    if (previousFrame)
    {
        return UnpackPathTracePrimarySurfaceRecord(PrimarySurfaceHistoryPrevious[index]);
    }
    return UnpackPathTracePrimarySurfaceRecord(PrimarySurfaceHistoryCurrent[index]);
}

RAB_Surface RAB_GetGBufferSurface(int2 pixelPosition, bool previousFrame)
{
    return LoadPathTracePrimarySurfaceRecord(pixelPosition, previousFrame);
}

RAB_Material RAB_GetGBufferMaterial(int2 pixelPosition, bool previousFrame)
{
    return RAB_GetGBufferSurface(pixelPosition, previousFrame).material;
}

bool ProjectPathTracePrimarySurfaceToPreviousPixel(float3 worldPosition, uint2 dimensions, out int2 previousPixel)
{
    previousPixel = int2(-1, -1);
    float2 previousPixelFloat;
    if (!ProjectPathTracePrimarySurfaceToPreviousPixelFloat(worldPosition, dimensions, previousPixelFloat))
    {
        return false;
    }

    previousPixel = int2(floor(previousPixelFloat));
    return previousPixel.x >= 0 && previousPixel.y >= 0 &&
        (uint)previousPixel.x < dimensions.x && (uint)previousPixel.y < dimensions.y;
}

bool RestirPTProjectWorldToPreviousPixel(float3 worldPosition, uint2 dimensions, out int2 previousPixel)
{
    return ProjectPathTracePrimarySurfaceToPreviousPixel(worldPosition, dimensions, previousPixel);
}

float4 PathTracePrimarySurfaceMotionVectorColor(float2 motionPixels)
{
    const float magnitude = length(motionPixels);
    const float maxVisualizedPixels = 32.0;
    const float2 signedDirection = saturate(motionPixels / (maxVisualizedPixels * 2.0) + 0.5);
    return float4(signedDirection.x, signedDirection.y, saturate(magnitude / maxVisualizedPixels), 1.0);
}

bool ProjectPathTracePrimarySurfaceToPreviousPixel(RAB_Surface currentSurface, uint2 dimensions, out int2 previousPixel, out uint debugStatus)
{
    previousPixel = int2(-1, -1);
    debugStatus = RT_PRIMARY_SURFACE_DEBUG_OK;
    if (!RAB_IsSurfaceValid(currentSurface))
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT;
        return false;
    }

    const PathTracePrimarySurfaceRecord currentRecord = PackPathTracePrimarySurfaceRecord(currentSurface);
    float3 previousProjectionPosition = currentRecord.worldPositionAndViewDepth.xyz;
    if (PathTracePrimarySurfaceRecordHasObjectMotion(currentRecord))
    {
        previousProjectionPosition = currentRecord.previousPositionOrMotion.xyz;
    }
    else if (currentRecord.header.z != RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION)
    {
        debugStatus = currentRecord.header.z;
        return false;
    }

    float2 previousPixelFloat;
    float previousLinearDepth;
    if (!ProjectPathTracePrimarySurfaceToPreviousPixelFloatAndDepthWithStatus(previousProjectionPosition, dimensions, previousPixelFloat, previousLinearDepth, debugStatus))
    {
        return false;
    }

    previousPixel = int2(floor(previousPixelFloat));
    return true;
}

bool TryPathTracePrimarySurfacePackedObjectMotionPixels(RAB_Surface currentSurface, uint2 pixel, uint2 dimensions, out int2 previousPixel, out float2 motionPixels, out uint debugStatus)
{
    previousPixel = int2(-1, -1);
    motionPixels = float2(0.0, 0.0);
    debugStatus = RT_PRIMARY_SURFACE_DEBUG_OK;
    if (!RAB_IsSurfaceValid(currentSurface))
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT;
        return false;
    }

    const PathTracePrimarySurfaceRecord currentRecord = PackPathTracePrimarySurfaceRecord(currentSurface);
    if (!PathTracePrimarySurfaceRecordHasObjectMotion(currentRecord))
    {
        debugStatus = PathTracePrimarySurfaceMissingPackedObjectMotionStatus(currentSurface, currentRecord.header.z);
        return false;
    }

    float2 previousPixelFloat;
    float previousLinearDepth;
    if (!ProjectPathTracePrimarySurfaceToPreviousPixelFloatAndDepthWithStatus(currentRecord.previousPositionOrMotion.xyz, dimensions, previousPixelFloat, previousLinearDepth, debugStatus))
    {
        return false;
    }

    previousPixel = int2(floor(previousPixelFloat));
    motionPixels = previousPixelFloat - (float2(pixel) + 0.5);
    return true;
}

bool PathTracePrimarySurfacesAreSimilar(RAB_Surface a, RAB_Surface b, out uint debugStatus)
{
    debugStatus = RT_PRIMARY_SURFACE_DEBUG_OK;
    const bool currentValid = RAB_IsSurfaceValid(a);
    const bool previousValid = RAB_IsSurfaceValid(b);
    if (!currentValid)
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT;
        return false;
    }
    if (!previousValid)
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_MISSING_PREVIOUS;
        return false;
    }

    const float3 aGeometryNormal = SafeNormalize(a.geometryNormal, a.shadingNormal);
    const float3 bGeometryNormal = SafeNormalize(b.geometryNormal, b.shadingNormal);
    const float normalSimilarity = dot(aGeometryNormal, bGeometryNormal);
    const float roughnessDelta = abs(GetRoughness(a.material) - GetRoughness(b.material));
    const bool materialMatch = a.materialId == b.materialId &&
        a.materialIndex == b.materialIndex &&
        a.surfaceClass == b.surfaceClass;
    if (!materialMatch)
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_MATERIAL_MISMATCH;
        return false;
    }
    if (normalSimilarity < 0.85)
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_NORMAL_MISMATCH;
        return false;
    }
    if (roughnessDelta > 0.20)
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_ROUGHNESS_MISMATCH;
        return false;
    }

    return true;
}

bool RAB_AreMaterialsSimilar(RAB_Surface a, RAB_Surface b)
{
    uint debugStatus;
    return PathTracePrimarySurfacesAreSimilar(a, b, debugStatus);
}

float4 PathTracePrimarySurfaceDebugColor(uint debugStatus, RAB_Surface currentSurface)
{
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT)
    {
        return float4(0.55, 0.05, 0.04, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_MISSING_PREVIOUS)
    {
        return float4(0.05, 0.12, 0.55, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_REJECTED_PREVIOUS)
    {
        return float4(0.55, 0.02, 0.16, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_MATERIAL_MISMATCH)
    {
        return float4(0.55, 0.42, 0.04, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_NORMAL_MISMATCH)
    {
        return float4(0.35, 0.06, 0.55, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_ROUGHNESS_MISMATCH)
    {
        return float4(0.55, 0.22, 0.04, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION)
    {
        return float4(0.02, 0.22, 0.24, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_SKINNED_MISSING_PREVIOUS)
    {
        return float4(0.10, 0.12, 0.30, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_SKINNED_RANGE_MISMATCH)
    {
        return float4(0.45, 0.10, 0.05, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_SKINNED_PREVIOUS_OUT_OF_RANGE)
    {
        return float4(0.55, 0.02, 0.38, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_RIGID_MISSING_PREVIOUS)
    {
        return float4(0.08, 0.16, 0.32, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_RIGID_RANGE_MISMATCH)
    {
        return float4(0.45, 0.18, 0.04, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_MISSING_PREVIOUS_CAMERA)
    {
        return float4(0.04, 0.20, 0.58, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_PREVIOUS_BEHIND_CAMERA)
    {
        return float4(0.62, 0.02, 0.06, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_PREVIOUS_XY_OUTSIDE_FRAME)
    {
        return float4(0.92, 0.22, 0.02, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_STATIC_RANGE_MISMATCH)
    {
        return float4(0.58, 0.30, 0.02, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_STATIC_MATERIAL_CLASS_MISMATCH)
    {
        return float4(0.62, 0.48, 0.02, 1.0);
    }
    if (debugStatus == RT_PRIMARY_SURFACE_DEBUG_PACKED_OBJECT_MISSING_PREVIOUS_MOTION)
    {
        return float4(0.10, 0.28, 0.30, 1.0);
    }

    const float currentRoughness = saturate(GetRoughness(currentSurface.material));
    const float3 albedo = saturate(currentSurface.material.diffuseAlbedo);
    return float4(saturate(float3(0.02, 0.30, 0.08) + albedo * 0.25 + currentRoughness * float3(0.0, 0.18, 0.08)), 1.0);
}

float4 EvaluatePathTracePrimarySurfaceObjectMotionDebug(RAB_Surface currentSurface, uint2 pixel)
{
    if (!RAB_IsSurfaceValid(currentSurface))
    {
        return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT, currentSurface);
    }

#ifdef RB_PATH_TRACE_PRIMARY_SURFACE_ENABLE_OBJECT_MOTION
    float3 previousObjectPosition;
    uint objectMotionDebugStatus = RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION;
    if (!TryPathTracePrimarySurfaceSkinnedObjectMotion(currentSurface, previousObjectPosition, objectMotionDebugStatus))
    {
        return PathTracePrimarySurfaceDebugColor(objectMotionDebugStatus, currentSurface);
    }

    float2 previousPixelFloat;
    float previousLinearDepth;
    uint projectionDebugStatus;
    if (!ProjectPathTracePrimarySurfaceToPreviousPixelFloatAndDepthWithStatus(previousObjectPosition, PathTraceFullOutputSize(), previousPixelFloat, previousLinearDepth, projectionDebugStatus))
    {
        return PathTracePrimarySurfaceDebugColor(projectionDebugStatus, currentSurface);
    }

    return PathTracePrimarySurfaceMotionVectorColor(previousPixelFloat - (float2(pixel) + 0.5));
#else
    return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION, currentSurface);
#endif
}

float4 EvaluatePathTracePrimarySurfaceRigidEligibilityDebug(RAB_Surface currentSurface)
{
    if (!RAB_IsSurfaceValid(currentSurface))
    {
        return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT, currentSurface);
    }

    if (currentSurface.surfaceClass != RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY)
    {
        return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION, currentSurface);
    }

    if (currentSurface.instanceId < 2u)
    {
        return float4(0.02, 0.28, 0.38, 1.0);
    }

#ifdef RB_PATH_TRACE_PRIMARY_SURFACE_ENABLE_OBJECT_MOTION
    float3 previousObjectPosition;
    uint objectMotionDebugStatus = RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION;
    if (TryPathTracePrimarySurfaceRigidObjectMotion(currentSurface, previousObjectPosition, objectMotionDebugStatus))
    {
        return float4(0.04, 0.48, 0.12, 1.0);
    }
    return PathTracePrimarySurfaceDebugColor(objectMotionDebugStatus, currentSurface);
#else
    return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION, currentSurface);
#endif
}

float4 EvaluatePathTracePrimarySurfaceRigidObjectMotionDebug(RAB_Surface currentSurface, uint2 pixel)
{
    if (!RAB_IsSurfaceValid(currentSurface))
    {
        return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT, currentSurface);
    }

#ifdef RB_PATH_TRACE_PRIMARY_SURFACE_ENABLE_OBJECT_MOTION
    float3 previousObjectPosition;
    uint objectMotionDebugStatus = RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION;
    if (!TryPathTracePrimarySurfaceRigidObjectMotion(currentSurface, previousObjectPosition, objectMotionDebugStatus))
    {
        return PathTracePrimarySurfaceDebugColor(objectMotionDebugStatus, currentSurface);
    }

    float2 previousPixelFloat;
    float previousLinearDepth;
    uint projectionDebugStatus;
    if (!ProjectPathTracePrimarySurfaceToPreviousPixelFloatAndDepthWithStatus(previousObjectPosition, PathTraceFullOutputSize(), previousPixelFloat, previousLinearDepth, projectionDebugStatus))
    {
        return PathTracePrimarySurfaceDebugColor(projectionDebugStatus, currentSurface);
    }

    return PathTracePrimarySurfaceMotionVectorColor(previousPixelFloat - (float2(pixel) + 0.5));
#else
    return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION, currentSurface);
#endif
}

float4 EvaluatePathTracePrimarySurfaceCombinedObjectMotionDebug(RAB_Surface currentSurface, uint2 pixel)
{
    int2 previousPixel;
    float2 motionPixels;
    uint debugStatus;
    if (!TryPathTracePrimarySurfacePackedObjectMotionPixels(currentSurface, pixel, PathTraceFullOutputSize(), previousPixel, motionPixels, debugStatus))
    {
        return PathTracePrimarySurfaceDebugColor(debugStatus, currentSurface);
    }

    return PathTracePrimarySurfaceMotionVectorColor(motionPixels);
}

float4 EvaluatePathTracePrimarySurfacePackedObjectMotionDebug(RAB_Surface currentSurface)
{
    const PathTracePrimarySurfaceRecord record = PackPathTracePrimarySurfaceRecord(currentSurface);
    if (record.header.x != RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION ||
        (record.header.y & RT_PRIMARY_SURFACE_VALID) == 0u)
    {
        return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT, currentSurface);
    }

    if (PathTracePrimarySurfaceRecordHasObjectMotion(record))
    {
        if (record.materialAndSurface.w == RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED)
        {
            return float4(0.04, 0.46, 0.14, 1.0);
        }
        if (record.materialAndSurface.w == RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY)
        {
            return float4(0.04, 0.36, 0.48, 1.0);
        }
        return float4(0.08, 0.38, 0.18, 1.0);
    }

    return PathTracePrimarySurfaceDebugColor(record.header.z, currentSurface);
}

float4 EvaluatePathTracePrimarySurfaceObjectMotionReprojectionDebug(RAB_Surface currentSurface, uint2 pixel)
{
    int2 previousPixel;
    float2 motionPixels;
    uint debugStatus;
    if (!TryPathTracePrimarySurfacePackedObjectMotionPixels(currentSurface, pixel, PathTraceFullOutputSize(), previousPixel, motionPixels, debugStatus))
    {
        return PathTracePrimarySurfaceDebugColor(debugStatus, currentSurface);
    }

    const RAB_Surface previousSurface = LoadPathTracePrimarySurfaceRecord(previousPixel, true);
    if (!PathTracePrimarySurfacesAreSimilar(currentSurface, previousSurface, debugStatus))
    {
        return PathTracePrimarySurfaceDebugColor(debugStatus, currentSurface);
    }

    return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_OK, currentSurface);
}

bool TryPathTraceMaterialUnsupportedDebugColor(RAB_Surface currentSurface, uint passKind, out float4 debugColor)
{
    debugColor = float4(0.0, 0.0, 0.0, 1.0);
    if (!RAB_IsSurfaceValid(currentSurface))
    {
        return false;
    }
    if (MaterialSupportedByPass(currentSurface, passKind))
    {
        return false;
    }

    debugColor = MaterialFailClosedDebugColor(currentSurface, passKind);
    return true;
}

float4 EvaluateRestirPTPrimarySurfacePairDebug(RAB_Surface currentSurface, RAB_Surface previousSurface)
{
    if (!RAB_IsSurfaceValid(currentSurface) && !RAB_IsSurfaceValid(previousSurface))
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float4 unsupportedColor;
    if (TryPathTraceMaterialUnsupportedDebugColor(currentSurface, RT_PATH_TRACE_MATERIAL_PASS_DIRECT_RESERVOIR, unsupportedColor))
    {
        return unsupportedColor;
    }

    uint debugStatus;
    if (!PathTracePrimarySurfacesAreSimilar(currentSurface, previousSurface, debugStatus))
    {
        return PathTracePrimarySurfaceDebugColor(debugStatus, currentSurface);
    }

    return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_OK, currentSurface);
}

float4 EvaluateRestirPTPrimarySurfaceHistoryDebug(RAB_Surface currentSurface, uint2 pixel)
{
    const RAB_Surface previousSurface = RAB_GetGBufferSurface(int2((int)pixel.x, (int)pixel.y), true);
    return EvaluateRestirPTPrimarySurfacePairDebug(currentSurface, previousSurface);
}

float4 EvaluateRestirPTPrimarySurfaceReprojectionDebug(RAB_Surface currentSurface)
{
    if (!RAB_IsSurfaceValid(currentSurface))
    {
        return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT, currentSurface);
    }

    float4 unsupportedColor;
    if (TryPathTraceMaterialUnsupportedDebugColor(currentSurface, RT_PATH_TRACE_MATERIAL_PASS_DIRECT_RESERVOIR, unsupportedColor))
    {
        return unsupportedColor;
    }

    int2 previousPixel;
    uint debugStatus;
    const bool projected = ProjectPathTracePrimarySurfaceToPreviousPixel(currentSurface, PathTraceFullOutputSize(), previousPixel, debugStatus);
    if (!projected)
    {
        return PathTracePrimarySurfaceDebugColor(debugStatus, currentSurface);
    }

    const RAB_Surface previousSurface = RAB_GetGBufferSurface(previousPixel, true);
    return EvaluateRestirPTPrimarySurfacePairDebug(currentSurface, previousSurface);
}

// Compatibility wrappers for the existing smoke raygen until task 05 splits
// primary-surface generation out of the monolithic dispatch.
PathTracePrimarySurfaceRecord RestirPTPackPrimarySurfaceHistory(RAB_Surface surface)
{
    return PackPathTracePrimarySurfaceRecord(surface);
}

RAB_Surface RestirPTUnpackPrimarySurfaceHistory(PathTracePrimarySurfaceRecord record)
{
    return UnpackPathTracePrimarySurfaceRecord(record);
}

void StoreRestirPTPrimarySurfaceHistory(uint2 pixel, RAB_Surface surface)
{
    StorePathTracePrimarySurfaceRecord(pixel, surface);
}

#endif
#endif
