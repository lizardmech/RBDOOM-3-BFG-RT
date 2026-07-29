#pragma once

// Live drawSurf mirror for the future PT scene producer.
//
// The mirror observes the final raster-submitted surfaces as mesh identities
// plus per-frame instances. Source mode 3 can also append non-static mirror
// records into the existing shader-compatible dynamic frame bucket.

#include "PathTraceInstanceUniverse.h"

#include <vector>

class RtSmokeGeometryUniverse;
class RtPathTraceSceneUniverse;
struct PathTraceSmokeVertex;
struct RtSmokeSkinnedSurfaceRecord;
struct RtSmokeCapturedSurfaceRecord;
struct PtSkinnedHitRouteRecord;
struct viewDef_t;

const int RT_PT_BOUNDS_OVERLAY_MAX_LINES = 4096;

struct RtPathTraceBoundsOverlayLine
{
    idVec4 startAndPad = idVec4(0.0f, 0.0f, 0.0f, 0.0f);
    idVec4 endAndPad = idVec4(0.0f, 0.0f, 0.0f, 0.0f);
    idVec4 color = idVec4(1.0f, 1.0f, 1.0f, 1.0f);
};

struct RtPathTraceDrawSurfMirrorSurfaceCache
{
    bool valid = false;
    const drawSurf_t* drawSurf = nullptr;
    const srfTriangles_t* tri = nullptr;
    RtSmokeSurfaceSkipStats skipStats;
    RtSmokeSurfaceClass classifiedSurfaceClass = RtSmokeSurfaceClass::Unknown;
    RtSmokeSurfaceClass surfaceClass = RtSmokeSurfaceClass::Unknown;
    RtSmokeTranslucentSubtype translucentSubtype = RtSmokeTranslucentSubtype::Unknown;
    uint64 legacyStaticKey = 0;
    uint32_t baseMaterialId = 0;
    uint32_t materialId = 0;
    uint32_t sourceKind = 0;
    uint32_t sourceFlags = 0;
    uint32_t surfaceClassId = 0;
    uint32_t surfaceClassAndFlags = 0;
    uint32_t materialClassSignature = 0;
};

void CapturePathTraceDrawSurfMirror(
    const viewDef_t* viewDef,
    const RtPathTraceSceneUniverse* sceneUniverse,
    RtSmokeGeometryUniverse* geometryUniverse,
    RtPathTraceInstanceUniverse& instanceUniverse,
    std::vector<RtPathTraceBoundsOverlayLine>* boundsOverlayLines = nullptr,
    const std::vector<RtPathTraceDrawSurfMirrorSurfaceCache>* surfaceCache = nullptr);

uint64 BuildPathTraceSkinnedCaptureViewSignature(
    const viewDef_t* viewDef);

bool CapturePathTraceDynamicFrameFromDrawSurfMirror(
    const viewDef_t* viewDef,
    const RtPathTraceSceneUniverse* sceneUniverse,
    RtSmokeGeometryUniverse* geometryUniverse,
    std::vector<PathTraceSmokeVertex>& vertexData,
    std::vector<uint32_t>& indexData,
    std::vector<uint32_t>& triangleClassData,
    std::vector<uint32_t>& triangleMaterialData,
    std::vector<uint32_t>* triangleInstanceData,
    std::vector<uint32_t>* triangleIdentityData,
    int& sourceSurfaces,
    int& sourceVerts,
    int& sourceIndexes,
    RtSmokeSurfaceClassStats& classStats,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeDynamicGeometryStats& dynamicStats,
    RtSmokeAttributeStats& attributeStats,
    RtSmokeMaterialStats& materialStats,
    RtSmokeBucketRanges& bucketRanges,
    RtSmokeSceneCaptureTiming& captureTiming,
    std::vector<RtSmokeSkinnedSurfaceRecord>* skinnedSurfaceRecords = nullptr,
    std::vector<RtSmokeCapturedSurfaceRecord>* capturedSurfaceRecords = nullptr,
    std::vector<RtPathTraceDrawSurfMirrorSurfaceCache>* surfaceCache = nullptr,
    RtPathTraceInstanceUniverse* instanceUniverse = nullptr,
    std::vector<RtPathTraceBoundsOverlayLine>* boundsOverlayLines = nullptr,
    bool recordAllInstanceClasses = false,
    const std::vector<PtSkinnedHitRouteRecord>*
        skinnedCaptureAdmissionRoutes = nullptr);
