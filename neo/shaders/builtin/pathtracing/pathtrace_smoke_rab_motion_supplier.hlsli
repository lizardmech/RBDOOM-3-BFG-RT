// Purpose:
//     Supplies rbdoom primary-surface motion and history data for RTXDI/RAB
//     temporal surface contracts.
//
// NVIDIA reference:
//     E:\prog\references\RTXDI-main\Libraries\Rtxdi\Include\Rtxdi\PT\TemporalResampling.hlsli
//     E:\prog\references\RTXDI-main\Samples\FullSample\Shaders\LightingPasses\PT\TemporalResampling.hlsl
//     E:\prog\references\RTXDI-main\Samples\FullSample\Shaders\LightingPasses\RtxdiApplicationBridge\RAB_Surface.hlsli
//
// Current rbdoom supplier:
//     pathtrace_smoke.rt.hlsl primary-surface records, previous static
//     snapshots, skinned previous positions, rigid route transforms, and
//     motion-vector export buffers.
//
// Current deviation:
//     rbdoom reconstructs motion from smoke payload identity, static history,
//     skinned CPU bridge data, and rigid route records instead of the reference
//     sample's G-buffer motion inputs.
//
// Extraction rule:
//     This file must preserve existing behavior until a later correction task.

#ifndef PATHTRACE_SMOKE_RAB_MOTION_SUPPLIER_HLSLI
#define PATHTRACE_SMOKE_RAB_MOTION_SUPPLIER_HLSLI

bool ComputeSmokeTriangleBarycentrics(float3 position, float3 p0, float3 p1, float3 p2, out float3 barycentrics)
{
    barycentrics = float3(1.0, 0.0, 0.0);
    const float3 edge0 = p1 - p0;
    const float3 edge1 = p2 - p0;
    const float3 delta = position - p0;
    const float d00 = dot(edge0, edge0);
    const float d01 = dot(edge0, edge1);
    const float d11 = dot(edge1, edge1);
    const float d20 = dot(delta, edge0);
    const float d21 = dot(delta, edge1);
    const float denominator = d00 * d11 - d01 * d01;
    if (abs(denominator) <= 1.0e-10)
    {
        return false;
    }

    const float invDenominator = 1.0 / denominator;
    const float v = (d11 * d20 - d01 * d21) * invDenominator;
    const float w = (d00 * d21 - d01 * d20) * invDenominator;
    barycentrics = float3(1.0 - v - w, v, w);
    return all(barycentrics == barycentrics);
}

float3 TransformPathTraceRigidRoutePoint(float4 row0, float4 row1, float4 row2, float3 localPoint)
{
    return float3(
        dot(row0, float4(localPoint, 1.0)),
        dot(row1, float4(localPoint, 1.0)),
        dot(row2, float4(localPoint, 1.0)));
}

bool TryPathTracePrimarySurfaceSkinnedObjectMotion(RAB_Surface surface, out float3 previousWorldPosition, out uint debugStatus)
{
    previousWorldPosition = float3(0.0, 0.0, 0.0);
    debugStatus = RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION;
    if (surface.surfaceClass != RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED)
    {
        return false;
    }

    debugStatus = RT_PRIMARY_SURFACE_DEBUG_SKINNED_MISSING_PREVIOUS;
    if (PathTraceIsSkinnedHitRouteInstance(surface.instanceId))
    {
        PathTraceSkinnedHitRouteGpuRecord route;
        PathTraceSkinnedHitRouteGpuTriangle routeTriangle;
        uint vertexIndex0;
        uint vertexIndex1;
        uint vertexIndex2;
        if (!PathTraceLoadSkinnedHitRouteTriangleData(
                surface.instanceId,
                surface.primitiveIndex,
                route,
                routeTriangle,
                vertexIndex0,
                vertexIndex1,
                vertexIndex2) ||
            (route.flags & PT_SKINNED_HIT_ROUTE_HAS_PREVIOUS) == 0u ||
            route.previousPositionOffset ==
                PT_SKINNED_HIT_ROUTE_INVALID_INDEX)
        {
            return false;
        }

        debugStatus = RT_PRIMARY_SURFACE_DEBUG_SKINNED_RANGE_MISMATCH;
        const uint localIndex0 = vertexIndex0 - route.outputVertexOffset;
        const uint localIndex1 = vertexIndex1 - route.outputVertexOffset;
        const uint localIndex2 = vertexIndex2 - route.outputVertexOffset;
        const uint previous0 = route.previousPositionOffset + localIndex0;
        const uint previous1 = route.previousPositionOffset + localIndex1;
        const uint previous2 = route.previousPositionOffset + localIndex2;
        if (previous0 >= PathTraceSkinnedPreviousPositionCount() ||
            previous1 >= PathTraceSkinnedPreviousPositionCount() ||
            previous2 >= PathTraceSkinnedPreviousPositionCount())
        {
            debugStatus =
                RT_PRIMARY_SURFACE_DEBUG_SKINNED_PREVIOUS_OUT_OF_RANGE;
            return false;
        }

        const float3 current0 =
            SmokeSkinnedCurrentVertices[vertexIndex0].position.xyz;
        const float3 current1 =
            SmokeSkinnedCurrentVertices[vertexIndex1].position.xyz;
        const float3 current2 =
            SmokeSkinnedCurrentVertices[vertexIndex2].position.xyz;
        float3 barycentrics;
        if (!ComputeSmokeTriangleBarycentrics(
                surface.worldPos,
                current0,
                current1,
                current2,
                barycentrics))
        {
            return false;
        }

        const float3 prev0 =
            SmokeSkinnedPreviousPositions[previous0].
                previousPosition.xyz;
        const float3 prev1 =
            SmokeSkinnedPreviousPositions[previous1].
                previousPosition.xyz;
        const float3 prev2 =
            SmokeSkinnedPreviousPositions[previous2].
                previousPosition.xyz;
        previousWorldPosition =
            prev0 * barycentrics.x +
            prev1 * barycentrics.y +
            prev2 * barycentrics.z;
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_OK;
        return all(previousWorldPosition == previousWorldPosition);
    }

    if (surface.instanceId != 1u)
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION;
        return false;
    }

    const uint dispatchCount = PathTraceSkinnedSurfaceDispatchCount();
    const uint dispatchIndexCount = PathTraceSkinnedTriangleDispatchIndexCount();
    if (dispatchCount == 0u || dispatchIndexCount == 0u || PathTraceSkinnedPreviousPositionCount() == 0u)
    {
        return false;
    }

    if (surface.primitiveIndex >= dispatchIndexCount)
    {
        return false;
    }

    const uint dispatchIndex = SmokeSkinnedTriangleDispatchIndexes[surface.primitiveIndex];
    if (dispatchIndex == 0xffffffffu || dispatchIndex >= dispatchCount)
    {
        return false;
    }

    const PathTraceSkinnedSurfaceDispatchRecord dispatch = SmokeSkinnedSurfaceDispatch[dispatchIndex];
    if (surface.primitiveIndex < dispatch.dynamicTriangleOffset ||
        surface.primitiveIndex >= dispatch.dynamicTriangleOffset + dispatch.triangleCount)
    {
        return false;
    }

    if ((dispatch.flags & (PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS | PT_SKINNED_DISPATCH_RT_CPU_SKINNED)) !=
        (PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS | PT_SKINNED_DISPATCH_RT_CPU_SKINNED) ||
        dispatch.previousPositionOffset == 0xffffffffu)
    {
        return false;
    }

    debugStatus = RT_PRIMARY_SURFACE_DEBUG_SKINNED_RANGE_MISMATCH;
    const uint localTriangleIndex = surface.primitiveIndex - dispatch.dynamicTriangleOffset;
    const uint indexOffset = dispatch.dynamicIndexOffset + localTriangleIndex * 3u;
    if (indexOffset + 2u >= PathTraceDynamicIndexCount() ||
        dispatch.vertexCount == 0u ||
        dispatch.triangleCount == 0u)
    {
        return false;
    }

    const uint i0 = SmokeDynamicIndices[indexOffset + 0u];
    const uint i1 = SmokeDynamicIndices[indexOffset + 1u];
    const uint i2 = SmokeDynamicIndices[indexOffset + 2u];
    const uint vertexEnd = dispatch.dynamicVertexOffset + dispatch.vertexCount;
    if (i0 < dispatch.dynamicVertexOffset || i0 >= vertexEnd ||
        i1 < dispatch.dynamicVertexOffset || i1 >= vertexEnd ||
        i2 < dispatch.dynamicVertexOffset || i2 >= vertexEnd ||
        i0 >= PathTraceDynamicVertexCount() ||
        i1 >= PathTraceDynamicVertexCount() ||
        i2 >= PathTraceDynamicVertexCount())
    {
        return false;
    }

    const float3 p0 = SmokeDynamicVertices[i0].position.xyz;
    const float3 p1 = SmokeDynamicVertices[i1].position.xyz;
    const float3 p2 = SmokeDynamicVertices[i2].position.xyz;
    float3 barycentrics;
    if (!ComputeSmokeTriangleBarycentrics(surface.worldPos, p0, p1, p2, barycentrics))
    {
        return false;
    }

    debugStatus = RT_PRIMARY_SURFACE_DEBUG_SKINNED_PREVIOUS_OUT_OF_RANGE;
    const uint previous0 = dispatch.previousPositionOffset + (i0 - dispatch.dynamicVertexOffset);
    const uint previous1 = dispatch.previousPositionOffset + (i1 - dispatch.dynamicVertexOffset);
    const uint previous2 = dispatch.previousPositionOffset + (i2 - dispatch.dynamicVertexOffset);
    if (previous0 >= PathTraceSkinnedPreviousPositionCount() ||
        previous1 >= PathTraceSkinnedPreviousPositionCount() ||
        previous2 >= PathTraceSkinnedPreviousPositionCount())
    {
        return false;
    }

    const float3 prev0 = SmokeSkinnedPreviousPositions[previous0].previousPosition.xyz;
    const float3 prev1 = SmokeSkinnedPreviousPositions[previous1].previousPosition.xyz;
    const float3 prev2 = SmokeSkinnedPreviousPositions[previous2].previousPosition.xyz;
    previousWorldPosition = prev0 * barycentrics.x + prev1 * barycentrics.y + prev2 * barycentrics.z;
    debugStatus = RT_PRIMARY_SURFACE_DEBUG_OK;
    return all(previousWorldPosition == previousWorldPosition);
}

bool TryPathTracePrimarySurfaceRigidObjectMotion(RAB_Surface surface, out float3 previousWorldPosition, out uint debugStatus)
{
    previousWorldPosition = float3(0.0, 0.0, 0.0);
    debugStatus = RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION;
    if (!PathTraceIsRigidHitRouteInstance(surface.instanceId) ||
        surface.surfaceClass != RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY)
    {
        return false;
    }

    debugStatus = RT_PRIMARY_SURFACE_DEBUG_RIGID_MISSING_PREVIOUS;
    const uint routeInstanceIndex = surface.instanceId - 2u;
    if (routeInstanceIndex >= PathTraceRigidRouteInstanceCount())
    {
        return false;
    }

    const PathTraceRigidRouteInstance routeInstance = SmokeRigidRouteInstances[routeInstanceIndex];
    if ((routeInstance.flags & (PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM | PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS)) !=
        (PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM | PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS))
    {
        return false;
    }

    debugStatus = RT_PRIMARY_SURFACE_DEBUG_RIGID_RANGE_MISMATCH;
    if (surface.primitiveIndex >= routeInstance.triangleCount ||
        routeInstance.triangleOffset + surface.primitiveIndex >= PathTraceRigidRouteTriangleCount())
    {
        return false;
    }

    const uint indexOffset = routeInstance.indexOffset + surface.primitiveIndex * 3u;
    if (indexOffset + 2u >= PathTraceRigidRouteIndexCount())
    {
        return false;
    }

    const uint i0 = SmokeRigidRouteIndices[indexOffset + 0u];
    const uint i1 = SmokeRigidRouteIndices[indexOffset + 1u];
    const uint i2 = SmokeRigidRouteIndices[indexOffset + 2u];
    if (i0 >= routeInstance.vertexCount || i1 >= routeInstance.vertexCount || i2 >= routeInstance.vertexCount ||
        routeInstance.vertexOffset + i0 >= PathTraceRigidRouteVertexCount() ||
        routeInstance.vertexOffset + i1 >= PathTraceRigidRouteVertexCount() ||
        routeInstance.vertexOffset + i2 >= PathTraceRigidRouteVertexCount())
    {
        return false;
    }

    const float3 local0 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i0].position.xyz;
    const float3 local1 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i1].position.xyz;
    const float3 local2 = SmokeRigidRouteVertices[routeInstance.vertexOffset + i2].position.xyz;
    const float3 current0 = TransformPathTraceRigidRoutePoint(routeInstance.currentObjectToWorld0, routeInstance.currentObjectToWorld1, routeInstance.currentObjectToWorld2, local0);
    const float3 current1 = TransformPathTraceRigidRoutePoint(routeInstance.currentObjectToWorld0, routeInstance.currentObjectToWorld1, routeInstance.currentObjectToWorld2, local1);
    const float3 current2 = TransformPathTraceRigidRoutePoint(routeInstance.currentObjectToWorld0, routeInstance.currentObjectToWorld1, routeInstance.currentObjectToWorld2, local2);

    float3 barycentrics;
    if (!ComputeSmokeTriangleBarycentrics(surface.worldPos, current0, current1, current2, barycentrics))
    {
        return false;
    }

    const float3 prev0 = TransformPathTraceRigidRoutePoint(routeInstance.previousObjectToWorld0, routeInstance.previousObjectToWorld1, routeInstance.previousObjectToWorld2, local0);
    const float3 prev1 = TransformPathTraceRigidRoutePoint(routeInstance.previousObjectToWorld0, routeInstance.previousObjectToWorld1, routeInstance.previousObjectToWorld2, local1);
    const float3 prev2 = TransformPathTraceRigidRoutePoint(routeInstance.previousObjectToWorld0, routeInstance.previousObjectToWorld1, routeInstance.previousObjectToWorld2, local2);
    previousWorldPosition = prev0 * barycentrics.x + prev1 * barycentrics.y + prev2 * barycentrics.z;
    debugStatus = RT_PRIMARY_SURFACE_DEBUG_OK;
    return all(previousWorldPosition == previousWorldPosition);
}

bool TryPathTracePrimarySurfaceObjectMotion(RAB_Surface surface, out float3 previousWorldPosition, out uint debugStatus)
{
    if (TryPathTracePrimarySurfaceSkinnedObjectMotion(surface, previousWorldPosition, debugStatus))
    {
        return true;
    }
    if (debugStatus != RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION)
    {
        return false;
    }
    return TryPathTracePrimarySurfaceRigidObjectMotion(surface, previousWorldPosition, debugStatus);
}

#define RB_PATH_TRACE_PRIMARY_SURFACE_ENABLE_HELPERS
#define RB_PATH_TRACE_PRIMARY_SURFACE_ENABLE_OBJECT_MOTION
#include "PathTracePrimarySurface.hlsli"

bool TryPathTracePreviousStaticSnapshotPosition(RAB_Surface currentSurface, out float3 previousPosition, out uint debugStatus)
{
    previousPosition = float3(0.0, 0.0, 0.0);
    debugStatus = RT_PRIMARY_SURFACE_DEBUG_OK;
    if (!RAB_IsSurfaceValid(currentSurface))
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT;
        return false;
    }

    if (currentSurface.instanceId != 0u || currentSurface.surfaceClass != 0u)
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION;
        return false;
    }

    if (PathTracePreviousStaticVertexCount() == 0u ||
        PathTracePreviousStaticIndexCount() == 0u ||
        PathTracePreviousStaticTriangleCount() == 0u ||
        PathTracePreviousStaticMaterialIndexCount() == 0u)
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_MISSING_PREVIOUS;
        return false;
    }

    const uint primitiveIndex = currentSurface.primitiveIndex;
    const uint indexOffset = primitiveIndex * 3u;
    if (primitiveIndex >= PathTraceStaticTriangleCount() ||
        primitiveIndex >= PathTracePreviousStaticTriangleCount() ||
        indexOffset + 2u >= PathTraceStaticIndexCount() ||
        indexOffset + 2u >= PathTracePreviousStaticIndexCount() ||
        primitiveIndex >= PathTracePreviousStaticMaterialIndexCount())
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_STATIC_RANGE_MISMATCH;
        return false;
    }

    const uint currentClass = SmokeStaticTriangleClasses[primitiveIndex];
    const uint previousClass = SmokePreviousStaticTriangleClasses[primitiveIndex];
    const uint currentMaterialId = SmokeStaticTriangleMaterials[primitiveIndex];
    const uint previousMaterialId = SmokePreviousStaticTriangleMaterials[primitiveIndex];
    const uint currentMaterialIndex = SmokeStaticTriangleMaterialIndexes[primitiveIndex];
    const uint previousMaterialIndex = SmokePreviousStaticTriangleMaterialIndexes[primitiveIndex];
    if ((currentClass & RT_SMOKE_TRIANGLE_CLASS_MASK) != (previousClass & RT_SMOKE_TRIANGLE_CLASS_MASK) ||
        currentMaterialId != previousMaterialId ||
        currentMaterialIndex != previousMaterialIndex)
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_STATIC_MATERIAL_CLASS_MISMATCH;
        return false;
    }

    const uint ci0 = SmokeStaticIndices[indexOffset + 0u];
    const uint ci1 = SmokeStaticIndices[indexOffset + 1u];
    const uint ci2 = SmokeStaticIndices[indexOffset + 2u];
    const uint pi0 = SmokePreviousStaticIndices[indexOffset + 0u];
    const uint pi1 = SmokePreviousStaticIndices[indexOffset + 1u];
    const uint pi2 = SmokePreviousStaticIndices[indexOffset + 2u];
    if (ci0 >= PathTraceStaticVertexCount() ||
        ci1 >= PathTraceStaticVertexCount() ||
        ci2 >= PathTraceStaticVertexCount() ||
        pi0 >= PathTracePreviousStaticVertexCount() ||
        pi1 >= PathTracePreviousStaticVertexCount() ||
        pi2 >= PathTracePreviousStaticVertexCount())
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_STATIC_RANGE_MISMATCH;
        return false;
    }

    const float3 c0 = SmokeStaticVertices[ci0].position.xyz;
    const float3 c1 = SmokeStaticVertices[ci1].position.xyz;
    const float3 c2 = SmokeStaticVertices[ci2].position.xyz;
    float3 barycentrics;
    if (!ComputeSmokeTriangleBarycentrics(currentSurface.worldPos, c0, c1, c2, barycentrics))
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_STATIC_RANGE_MISMATCH;
        return false;
    }

    const float3 p0 = SmokePreviousStaticVertices[pi0].position.xyz;
    const float3 p1 = SmokePreviousStaticVertices[pi1].position.xyz;
    const float3 p2 = SmokePreviousStaticVertices[pi2].position.xyz;
    previousPosition = p0 * barycentrics.x + p1 * barycentrics.y + p2 * barycentrics.z;
    if (!all(previousPosition == previousPosition))
    {
        debugStatus = RT_PRIMARY_SURFACE_DEBUG_NORMAL_MISMATCH;
        return false;
    }

    return true;
}

float4 EvaluatePathTracePreviousStaticSnapshotDebug(RAB_Surface currentSurface)
{
    float3 previousPosition;
    uint debugStatus;
    if (!TryPathTracePreviousStaticSnapshotPosition(currentSurface, previousPosition, debugStatus))
    {
        return PathTracePrimarySurfaceDebugColor(debugStatus, currentSurface);
    }
    if (length(previousPosition - currentSurface.worldPos) > 1.0)
    {
        return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_NORMAL_MISMATCH, currentSurface);
    }
    return float4(0.03, 0.42, 0.16, 1.0);
}

float4 EvaluatePathTracePreviousStaticSnapshotReprojectionDebug(RAB_Surface currentSurface)
{
    float3 previousPosition;
    uint debugStatus;
    if (!TryPathTracePreviousStaticSnapshotPosition(currentSurface, previousPosition, debugStatus))
    {
        return PathTracePrimarySurfaceDebugColor(debugStatus, currentSurface);
    }

    int2 previousPixel;
    float2 previousPixelFloat;
    float previousLinearDepth;
    uint projectionDebugStatus;
    if (!ProjectPathTracePrimarySurfaceToPreviousPixelFloatAndDepthWithStatus(previousPosition, PathTraceFullOutputSize(), previousPixelFloat, previousLinearDepth, projectionDebugStatus))
    {
        return PathTracePrimarySurfaceDebugColor(projectionDebugStatus, currentSurface);
    }
    previousPixel = int2(floor(previousPixelFloat));

    const RAB_Surface previousSurface = LoadPathTracePrimarySurfaceRecord(previousPixel, true);
    if (!PathTracePrimarySurfacesAreSimilar(currentSurface, previousSurface, debugStatus))
    {
        return PathTracePrimarySurfaceDebugColor(debugStatus, currentSurface);
    }

    return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_OK, currentSurface);
}

bool TryPathTracePreviousStaticSnapshotMotionPixels(RAB_Surface currentSurface, uint2 pixel, out int2 previousPixel, out float2 previousPixelFloat, out float2 motionPixels, out uint debugStatus);
bool TryPathTraceCombinedGeometryMotionPixels(RAB_Surface currentSurface, uint2 pixel, out int2 previousPixel, out float2 previousPixelFloat, out float2 motionPixels, out uint debugStatus, out uint sourceKind);
bool TryPathTraceCombinedGeometryMotionPixelsAndDepth(RAB_Surface currentSurface, uint2 pixel, out int2 previousPixel, out float2 previousPixelFloat, out float2 motionPixels, out float expectedPrevDepth, out uint debugStatus, out uint sourceKind);

float4 EvaluatePathTracePreviousStaticSnapshotMotionVectorDebug(RAB_Surface currentSurface, uint2 pixel)
{
    int2 previousPixel;
    float2 previousPixelFloat;
    float2 motionPixels;
    uint debugStatus;
    uint sourceKind;
    if (!TryPathTraceCombinedGeometryMotionPixels(currentSurface, pixel, previousPixel, previousPixelFloat, motionPixels, debugStatus, sourceKind))
    {
        return PathTracePrimarySurfaceDebugColor(debugStatus, currentSurface);
    }

    if (sourceKind != 1u)
    {
        return PathTracePrimarySurfaceDebugColor(RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION, currentSurface);
    }

    return PathTracePrimarySurfaceMotionVectorColor(motionPixels);
}

bool TryPathTracePreviousStaticSnapshotMotionPixelsAndDepth(RAB_Surface currentSurface, uint2 pixel, out int2 previousPixel, out float2 previousPixelFloat, out float2 motionPixels, out float expectedPrevDepth, out uint debugStatus)
{
    previousPixel = int2(-1, -1);
    previousPixelFloat = float2(-1.0, -1.0);
    motionPixels = float2(0.0, 0.0);
    expectedPrevDepth = 0.0;
    float3 previousPosition;
    if (!TryPathTracePreviousStaticSnapshotPosition(currentSurface, previousPosition, debugStatus))
    {
        return false;
    }

    if (!ProjectPathTracePrimarySurfaceToPreviousPixelFloatAndDepthWithStatus(previousPosition, PathTraceFullOutputSize(), previousPixelFloat, expectedPrevDepth, debugStatus))
    {
        return false;
    }

    expectedPrevDepth = PathTracePrimarySurfacePreviousProjectionDepth(previousPosition);
    previousPixel = int2(floor(previousPixelFloat));
    motionPixels = previousPixelFloat - (float2(pixel) + 0.5);
    return true;
}

bool TryPathTracePreviousStaticSnapshotMotionPixels(RAB_Surface currentSurface, uint2 pixel, out int2 previousPixel, out float2 previousPixelFloat, out float2 motionPixels, out uint debugStatus)
{
    float expectedPrevDepth;
    return TryPathTracePreviousStaticSnapshotMotionPixelsAndDepth(currentSurface, pixel, previousPixel, previousPixelFloat, motionPixels, expectedPrevDepth, debugStatus);
}

bool TryPathTracePackedObjectMotionPixelsAndDepth(RAB_Surface currentSurface, uint2 pixel, out int2 previousPixel, out float2 previousPixelFloat, out float2 motionPixels, out float expectedPrevDepth, out uint debugStatus)
{
    previousPixel = int2(-1, -1);
    previousPixelFloat = float2(-1.0, -1.0);
    motionPixels = float2(0.0, 0.0);
    expectedPrevDepth = 0.0;
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

    if (!ProjectPathTracePrimarySurfaceToPreviousPixelFloatAndDepthWithStatus(currentRecord.previousPositionOrMotion.xyz, PathTraceFullOutputSize(), previousPixelFloat, expectedPrevDepth, debugStatus))
    {
        return false;
    }

    expectedPrevDepth = PathTracePrimarySurfacePreviousProjectionDepth(currentRecord.previousPositionOrMotion.xyz);
    previousPixel = int2(floor(previousPixelFloat));
    motionPixels = previousPixelFloat - (float2(pixel) + 0.5);
    return true;
}

uint PathTraceMotionVectorSourceKind(RAB_Surface currentSurface)
{
    if (!RAB_IsSurfaceValid(currentSurface))
    {
        return PT_MOTION_VECTOR_SOURCE_UNKNOWN;
    }
    if (currentSurface.instanceId == 0u && currentSurface.surfaceClass == 0u)
    {
        return PT_MOTION_VECTOR_SOURCE_STATIC;
    }
    if (currentSurface.surfaceClass == RT_SMOKE_SURFACE_CLASS_SKINNED_DEFORMED)
    {
        return PT_MOTION_VECTOR_SOURCE_SKINNED;
    }
    if (currentSurface.surfaceClass == RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY)
    {
        return PT_MOTION_VECTOR_SOURCE_RIGID;
    }
    return PT_MOTION_VECTOR_SOURCE_OTHER_OBJECT;
}

uint PathTraceMotionVectorMaskFromStatus(bool valid, uint sourceKind, uint debugStatus)
{
    const uint sourceBits = (sourceKind & 0x0fu) << PT_MOTION_VECTOR_MASK_SOURCE_SHIFT;
    if (valid)
    {
        return PT_MOTION_VECTOR_MASK_VALID | sourceBits;
    }
    return sourceBits | ((debugStatus & 0xffu) << PT_MOTION_VECTOR_MASK_INVALID_REASON_SHIFT);
}

bool TryPathTraceCombinedGeometryMotionPixels(RAB_Surface currentSurface, uint2 pixel, out int2 previousPixel, out float2 previousPixelFloat, out float2 motionPixels, out uint debugStatus, out uint sourceKind)
{
    previousPixel = int2(-1, -1);
    previousPixelFloat = float2(-1.0, -1.0);
    motionPixels = float2(0.0, 0.0);
    debugStatus = RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION;
    sourceKind = PathTraceMotionVectorSourceKind(currentSurface);
    if (RAB_IsSurfaceValid(currentSurface) &&
        currentSurface.instanceId == 0u &&
        currentSurface.surfaceClass == 0u)
    {
        return TryPathTracePreviousStaticSnapshotMotionPixels(currentSurface, pixel, previousPixel, previousPixelFloat, motionPixels, debugStatus);
    }

    if (TryPathTracePrimarySurfacePackedObjectMotionPixels(currentSurface, pixel, PathTraceFullOutputSize(), previousPixel, motionPixels, debugStatus))
    {
        previousPixelFloat = motionPixels + (float2(pixel) + 0.5);
        return true;
    }

    return false;
}

bool TryPathTraceCombinedGeometryMotionPixelsAndDepth(RAB_Surface currentSurface, uint2 pixel, out int2 previousPixel, out float2 previousPixelFloat, out float2 motionPixels, out float expectedPrevDepth, out uint debugStatus, out uint sourceKind)
{
    previousPixel = int2(-1, -1);
    previousPixelFloat = float2(-1.0, -1.0);
    motionPixels = float2(0.0, 0.0);
    expectedPrevDepth = 0.0;
    debugStatus = RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION;
    sourceKind = PathTraceMotionVectorSourceKind(currentSurface);
    if (RAB_IsSurfaceValid(currentSurface) &&
        currentSurface.instanceId == 0u &&
        currentSurface.surfaceClass == 0u)
    {
        return TryPathTracePreviousStaticSnapshotMotionPixelsAndDepth(currentSurface, pixel, previousPixel, previousPixelFloat, motionPixels, expectedPrevDepth, debugStatus);
    }

    return TryPathTracePackedObjectMotionPixelsAndDepth(currentSurface, pixel, previousPixel, previousPixelFloat, motionPixels, expectedPrevDepth, debugStatus);
}

bool PathTraceMotionVectorExportEnabled()
{
    return MotionVectorInfo.x >= 0.5;
}

void StorePathTraceMotionVectorExport(uint2 pixel, RAB_Surface currentSurface)
{
    if (!PathTraceMotionVectorExportEnabled())
    {
        return;
    }

    int2 previousPixel;
    float2 previousPixelFloat;
    float2 motionPixels;
    float expectedPrevDepth;
    uint debugStatus;
    uint sourceKind;
    if (TryPathTraceCombinedGeometryMotionPixelsAndDepth(currentSurface, pixel, previousPixel, previousPixelFloat, motionPixels, expectedPrevDepth, debugStatus, sourceKind))
    {
        if (RayReconstructionInfo.z >= 0.5)
        {
            motionPixels -= RayReconstructionInfo.xy;
        }
        PathTraceMotionVectors[pixel] = float4(motionPixels, expectedPrevDepth - PathTracePrimarySurfaceCurrentProjectionDepth(currentSurface.worldPos), 0.0);
        PathTraceMotionVectorMask[pixel] = PathTraceMotionVectorMaskFromStatus(true, sourceKind, debugStatus);
    }
    else
    {
        PathTraceMotionVectors[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        PathTraceMotionVectorMask[pixel] = PathTraceMotionVectorMaskFromStatus(false, sourceKind, debugStatus);
    }
}

float4 EvaluatePathTraceCombinedGeometryMotionVectorDebug(RAB_Surface currentSurface, uint2 pixel)
{
    int2 previousPixel;
    float2 previousPixelFloat;
    float2 motionPixels;
    uint debugStatus;
    uint sourceKind;
    if (!TryPathTraceCombinedGeometryMotionPixels(currentSurface, pixel, previousPixel, previousPixelFloat, motionPixels, debugStatus, sourceKind))
    {
        return PathTracePrimarySurfaceDebugColor(debugStatus, currentSurface);
    }

    return PathTracePrimarySurfaceMotionVectorColor(motionPixels);
}

float4 EvaluatePathTraceCombinedGeometryReprojectionDebug(RAB_Surface currentSurface, uint2 pixel)
{
    int2 previousPixel;
    float2 previousPixelFloat;
    float2 motionPixels;
    uint debugStatus;
    uint sourceKind;
    if (!TryPathTraceCombinedGeometryMotionPixels(currentSurface, pixel, previousPixel, previousPixelFloat, motionPixels, debugStatus, sourceKind))
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

float4 EvaluatePathTraceCombinedGeometryMotionSourceDebug(RAB_Surface currentSurface, uint2 pixel)
{
    int2 previousPixel;
    float2 previousPixelFloat;
    float2 motionPixels;
    uint debugStatus;
    uint sourceKind;
    if (!TryPathTraceCombinedGeometryMotionPixels(currentSurface, pixel, previousPixel, previousPixelFloat, motionPixels, debugStatus, sourceKind))
    {
        return PathTracePrimarySurfaceDebugColor(debugStatus, currentSurface);
    }

    if (sourceKind == PT_MOTION_VECTOR_SOURCE_STATIC)
    {
        return float4(0.03, 0.42, 0.16, 1.0);
    }
    if (sourceKind == PT_MOTION_VECTOR_SOURCE_SKINNED)
    {
        return float4(0.48, 0.10, 0.62, 1.0);
    }
    if (sourceKind == PT_MOTION_VECTOR_SOURCE_RIGID)
    {
        return float4(0.04, 0.36, 0.48, 1.0);
    }

    return float4(0.08, 0.38, 0.18, 1.0);
}

float4 EvaluatePathTraceRigidRouteTransformParityDebug(PathTraceSmokePayload payload)
{
    if (payload.value == 0u)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    if (payload.instanceId < 2u || payload.surfaceClass != RT_SMOKE_SURFACE_CLASS_RIGID_ENTITY)
    {
        return float4(0.04, 0.08, 0.32, 1.0);
    }
    if ((payload.debugFlags & 0x1u) == 0u)
    {
        return float4(0.55, 0.04, 0.04, 1.0);
    }

    const float routeError = payload.debugVector.x;
    const float tlasError = payload.debugVector.y;
    const float routeVsTlasError = payload.debugVector.z;
    if (routeVsTlasError > 0.05)
    {
        return float4(0.75, 0.0, 0.85, 1.0);
    }
    if (routeError <= 0.01 && tlasError <= 0.01)
    {
        return float4(0.02, 0.65, 0.12, 1.0);
    }
    if (routeError <= 0.05)
    {
        return float4(0.75, 0.68, 0.04, 1.0);
    }
    if (routeError <= 0.25)
    {
        return float4(0.95, 0.38, 0.02, 1.0);
    }
    return float4(0.85, 0.02, 0.02, 1.0);
}

#endif
