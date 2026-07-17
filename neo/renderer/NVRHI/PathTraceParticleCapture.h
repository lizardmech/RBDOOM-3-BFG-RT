#pragma once

// CPU records for the screen-space particle-card lane.
//
// PC-T01 fills these records from current draw surfaces but deliberately has no
// GPU upload, BVH cutover, or rendering side effect.

#include <stdint.h>
#include <vector>

struct viewDef_t;
struct drawSurf_t;
struct srfTriangles_t;
class idImage;

enum class RtPathTraceParticleSurfaceRoute : uint32_t
{
    LegacyBvh = 0,
    CompositeOnly = 1
};

enum class RtPathTraceParticleBlendClass : uint32_t
{
    AlphaLit = 0,
    AlphaEmissive = 1,
    PureAdditiveEmissive = 2,
    MultiplicativeDarken = 3,
    Count = 4
};

enum class RtPathTraceParticleDepthPolicy : uint32_t
{
    World = 0,
    WeaponProjection = 1,
    ModelProjection = 2,
    WorldMuzzleNearPlane = 3
};

enum class RtPathTraceParticleSourceClass : uint32_t
{
    World = 0,
    LocalWeapon = 1,
    AttachedWeaponEmitter = 2,
    ProjectileTrail = 3,
    Impact = 4,
    Unknown = 5
};

enum RtPathTraceParticleBatchFlags : uint32_t
{
    RT_PATH_TRACE_PARTICLE_BATCH_TEXTURE_MATRIX = 1u << 0,
    RT_PATH_TRACE_PARTICLE_BATCH_INVERSE_VERTEX_COLOR = 1u << 1,
    RT_PATH_TRACE_PARTICLE_BATCH_CURRENT_PARTICLE_ALPHA_BVH = 1u << 2,
    RT_PATH_TRACE_PARTICLE_BATCH_AMBIGUOUS_MATERIAL = 1u << 3,
    RT_PATH_TRACE_PARTICLE_BATCH_STABLE_ID_UNAVAILABLE = 1u << 4,
    RT_PATH_TRACE_PARTICLE_BATCH_WEAPON_DEPTH_HACK = 1u << 5,
    RT_PATH_TRACE_PARTICLE_BATCH_MODEL_DEPTH_HACK = 1u << 6
};

struct ParticleCompositeVertex
{
    float worldPosition[3] = {};
    float worldPositionPadding = 0.0f;
    float texCoord[2] = {};
    uint32_t packedColor = 0xffffffffu;
    uint32_t particleIdLow = 0;
};

struct ParticleCompositeBatch
{
    uint32_t textureIndex = UINT32_MAX;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    RtPathTraceParticleBlendClass blendClass = RtPathTraceParticleBlendClass::AlphaLit;
    RtPathTraceParticleDepthPolicy depthPolicy = RtPathTraceParticleDepthPolicy::World;
    RtPathTraceParticleSourceClass sourceClass = RtPathTraceParticleSourceClass::Unknown;
    int sourceEntityId = -1;
    int allowSurfaceInViewId = 0;
    float modelDepthHack = 0.0f;
    float emissiveScale = 1.0f;
    float softDepth = 8.0f;
    uint32_t flags = RT_PATH_TRACE_PARTICLE_BATCH_STABLE_ID_UNAVAILABLE;
    uint32_t materialId = 0;
    int surfaceIndex = -1;
    int stageIndex = -1;
};

struct ParticleCompositeQuad
{
    float centerWorld[3] = {};
    uint32_t stableParticleId = 0;
    uint32_t batchIndex = 0;
    uint32_t firstIndex = 0;
    float viewDepth = 0.0f;
};

struct ParticleCompositePrimitive
{
    float centerWorld[3] = {};
    uint32_t stableParticleId = 0;
    uint32_t batchIndex = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    float viewDepth = 0.0f;
};

struct RtPathTraceParticleCaptureStats
{
    int candidateSurfaces = 0;
    int candidateQuads = 0;
    int candidateTriangles = 0;
    int cardOnlySurfaces = 0;
    int mixedStageSurfaces = 0;
    int routedCompositeOnlySurfaces = 0;
    int retainedBvhCandidateSurfaces = 0;
    int capturedSurfaces = 0;
    int capturedBatches = 0;
    int capturedDrawQuads = 0;
    int capturedTrianglePrimitives = 0;
    int droppedGeometrySurfaces = 0;
    int droppedNonQuadSurfaces = 0;
    int alphaLitBatches = 0;
    int alphaEmissiveBatches = 0;
    int pureAdditiveBatches = 0;
    int multiplicativeDarkenBatches = 0;
};

struct RtPathTraceParticleCapture
{
    std::vector<ParticleCompositeVertex> vertices;
    std::vector<uint32_t> indexes;
    std::vector<ParticleCompositeBatch> batches;
    std::vector<ParticleCompositeQuad> quads;
    std::vector<ParticleCompositePrimitive> primitives;
    std::vector<const idImage*> textures;
    RtPathTraceParticleCaptureStats stats;
    bool enabled = false;
    bool debugTint = false;

    void Clear();
};

void BuildPathTraceParticleCompositeCapture(const viewDef_t* viewDef, RtPathTraceParticleCapture& capture);
RtPathTraceParticleSurfaceRoute PathTraceParticleCompositeSurfaceRoute(
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri);
