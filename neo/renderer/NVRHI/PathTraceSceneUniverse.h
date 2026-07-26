#pragma once

// Diagnostics-only PT scene universe scaffold.
//
// This walks renderer world/model data directly so we can inspect static-world
// geometry and area membership without depending on the raster drawSurf list.
// It deliberately does not feed BLAS/TLAS or shader buffers yet.

#include "../BoundsTrack.h"
#include "PathTraceSceneCapture.h"

#include <unordered_map>
#include <vector>

class idRenderWorldLocal;
class RtSmokeGeometryUniverse;
class idRenderEntityLocal;
class idMaterial;
struct srfTriangles_t;
struct viewDef_t;

struct RtPathTraceSceneUniverseSurface
{
    uint64 key = 0;
    uint64 legacyDrawSurfKey = 0;
    const idRenderEntityLocal* entity = nullptr;
    const idMaterial* material = nullptr;
    const srfTriangles_t* tri = nullptr;
    int entityIndex = -1;
    int surfaceIndex = -1;
    uint32_t baseMaterialId = 0;
    uint32_t surfaceClassId = 0;
    uint32_t materialId = 0;
    int numVerts = 0;
    int numIndexes = 0;
    int triangles = 0;
    idBounds bounds;
    int portalArea = -1;
    int centerArea = -1;
    int offCenterArea = -1;
    int areas[8] = {};
    int areaCount = 0;
    int portalCount = 0;
    bool emissiveCapable = false;
    bool validStaticBuild = false;
    bool areaBoundsSkipped = false;
    idStr modelName;
    idStr materialName;
};

struct RtPathTraceSceneUniverseAreaStats
{
    int area = -1;
    int surfaces = 0;
    int triangles = 0;
    int emissiveCapableSurfaces = 0;
    int portals = 0;
};

struct RtPathTraceSceneUniverseStats
{
    bool valid = false;
    uint64 generation = 0;
    int entityDefs = 0;
    int staticWorldEntities = 0;
    int staticSurfaces = 0;
    int staticTriangles = 0;
    int uniqueMaterials = 0;
    int emissiveCapableSurfaces = 0;
    int emissiveCapableTriangles = 0;
    int emissiveCapableMaterials = 0;
    int assignedSurfaces = 0;
    int unassignedSurfaces = 0;
    int multiAreaSurfaces = 0;
    int boundsAreaSkippedSurfaces = 0;
    int centerAreaSurfaces = 0;
    int offCenterAreaSurfaces = 0;
    int distinctAreas = 0;
    int totalAreaRefs = 0;
    int totalPortalsReferenced = 0;
    idBounds bounds;
    idStr mapName;
};

struct RtPathTraceSceneUniverseSelectionStats
{
    bool valid = false;
    int selectedSurfaces = 0;
    int selectedAreaList[16] = {};
    int selectedAreaListCount = 0;
};

struct RtPathTraceSceneUniverseBuildStats
{
    bool built = false;
    bool cacheHit = false;
    int surfaces = 0;
    int vertices = 0;
    int indexes = 0;
    int triangles = 0;
    int skippedInvalid = 0;
    int skippedLimits = 0;
    int skippedZeroArea = 0;
    int emissiveCapableSurfaces = 0;
    int rigidEntitySurfaces = 0;
    int rigidEntityTriangles = 0;
    int residencyVisited = 0;
    int residencyDerived = 0;
    int residencyCacheHits = 0;
    int residencyCacheMisses = 0;
};

void SceneUniverseAddDynamicMaterialEvalStats(
    RtSmokeMaterialStats& stats,
    const viewDef_t* viewDef,
    const idRenderEntityLocal* entity,
    const idMaterial* material,
    int indexes);
void SceneUniverseAddDynamicMaterialEvalStatsForId(
    RtSmokeMaterialStats& stats,
    const viewDef_t* viewDef,
    const idRenderEntityLocal* entity,
    const idMaterial* material,
    int indexes,
    uint32_t materialId);

class RtPathTraceSceneUniverse
{
public:
    void Clear();
    bool EnsureBuilt(const viewDef_t* viewDef);
    RtPathTraceSceneUniverseBuildStats BuildFullStaticGeometry(const viewDef_t* viewDef, RtSmokeGeometryUniverse& geometryUniverse, RtSmokeSurfaceClassStats& classStats, RtSmokeSurfaceSkipStats& skipStats, RtSmokeAttributeStats& attributeStats, RtSmokeMaterialStats& materialStats, RtSmokeBucketRanges& bucketRanges);
    RtPathTraceSceneUniverseBuildStats BuildSelectedStaticGeometry(const viewDef_t* viewDef, RtSmokeGeometryUniverse& geometryUniverse, RtSmokeSurfaceClassStats& classStats, RtSmokeSurfaceSkipStats& skipStats, RtSmokeAttributeStats& attributeStats, RtSmokeMaterialStats& materialStats, RtSmokeBucketRanges& bucketRanges, int portalSteps);

    const RtPathTraceSceneUniverseStats& GetStats() const;
    const std::vector<RtPathTraceSceneUniverseSurface>& Surfaces() const;

private:
    bool Build(const viewDef_t* viewDef);
    RtPathTraceSceneUniverseSelectionStats BuildSelectionStats(const viewDef_t* viewDef, int portalSteps, bool countSelectedSurfaces) const;

    const idRenderWorldLocal* m_renderWorld = nullptr;
    idStr m_renderWorldMapName;
    ID_TIME_T m_renderWorldMapTimeStamp = 0;
    uint64 m_generation = 0;
    uint64 m_fullStaticGeometryGeneration = 0;
    int m_fullStaticGeometryRigidMode = 0;
    RtPathTraceSceneUniverseStats m_stats;
    std::vector<RtPathTraceSceneUniverseSurface> m_surfaces;
    std::vector<RtPathTraceSceneUniverseAreaStats> m_areaStats;
    std::vector<std::vector<int>> m_areaSurfaceIndices;
    std::vector<uint64> m_surfaceSelectionStamps;
    uint64 m_surfaceSelectionStamp = 0;
};
