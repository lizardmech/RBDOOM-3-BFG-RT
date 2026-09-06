#pragma once

// Surface bucket classification for captured Doom draw surfaces.
//
// Maps draw surfaces into the static, rigid, skinned, particle, or unknown
// triangle buckets consumed by the RT smoke scene builder. Doom material traits
// are classified separately in PathTraceDoomMaterialClassifier.

#include <stdint.h>

struct drawSurf_t;
class idRenderEntityLocal;
class idMaterial;
class idRenderModel;
struct RtSmokeTranslucentClassifierInfo;
struct modelSurface_t;
struct srfTriangles_t;
struct viewDef_t;

constexpr int RT_SMOKE_CLASS_COUNT = 5;
constexpr int RT_SMOKE_TRANSLUCENT_SUBTYPE_COUNT = 7;
constexpr uint32_t RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT = 24u;
constexpr uint32_t RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK = 0x0f000000u;

enum class RtSmokeSurfaceClass
{
    StaticWorld,
    RigidEntity,
    SkinnedDeformed,
    ParticleAlpha,
    Unknown
};

enum class RtSmokeTranslucentSubtype
{
    DecalGrime,
    ObjectGlass,
    SmokeParticle,
    SignageGlow,
    PortalWindow,
    GuiScreen,
    Unknown
};

enum class RtPtFeedClass
{
    StaticWorld,
    RigidEntity,
    RigidSkinned,
    TrueDeform,
    Transient
};

struct RtSmokeMaterialRouteInput
{
    char materialName[1024] = {};
    int coverage = 0;
    int deform = 0;
    int stageCount = 0;
    float sort = 0.0f;
    bool materialPresent = false;
    bool guiSurface = false;
    bool polygonOffset = false;
    bool hasAlphaTest = false;
    bool singleStageAmbientAlphaBlend = false;
};

struct RtSmokeSurfaceClassifyInput
{
    RtSmokeMaterialRouteInput material;
    bool hasJointCache = false;
    bool hasStaticModelWithJoints = false;
    bool hasRenderEntityJoints = false;
    bool isWorldSpace = false;
    bool ambientCacheIsStatic = false;
    bool indexCacheIsStatic = false;
    bool hasEntityDef = false;
    float modelDepthHack = 0.0f;
};

uint32_t SmokeSurfaceClassId(RtSmokeSurfaceClass surfaceClass);
const char* SmokeSurfaceClassName(RtSmokeSurfaceClass surfaceClass);
const char* SmokeSurfaceClassNameByIndex(int classIndex);
const char* RtPtFeedClassName(RtPtFeedClass feedClass);
uint32_t SmokeTranslucentSubtypeId(RtSmokeTranslucentSubtype subtype);
const char* SmokeTranslucentSubtypeName(RtSmokeTranslucentSubtype subtype);
const char* SmokeTranslucentSubtypeNameByIndex(int subtypeIndex);
RtSmokeSurfaceClass ClassifySmokeSurface(const viewDef_t* viewDef, const drawSurf_t* drawSurf, const srfTriangles_t* tri);
RtSmokeSurfaceClass ClassifySmokeSurfaceFromPod(
    const RtSmokeSurfaceClassifyInput& input,
    const RtSmokeTranslucentClassifierInfo& classifier);
RtPtFeedClass ClassifyEntityFeedSurface(const idRenderEntityLocal* entity, const idRenderModel* model, const modelSurface_t* surface);
bool IsEntityFeedSingleBoneSurface(const srfTriangles_t* tri);
bool SmokeMaterialUsesOpaqueSwinglightCompatibility(const idMaterial* material);
bool SmokeMaterialUsesOpaqueSwinglightCompatibilityFromPod(
    const RtSmokeMaterialRouteInput& input,
    const RtSmokeTranslucentClassifierInfo& classifier);
bool UnifiedPtDiagnosticRemovesAlphaClipSurface(const idMaterial* material);
bool SmokeMaterialCanPromoteRigidEmissiveCard(const idMaterial* material);
bool SmokeMaterialCanPromoteRigidEmissiveCard(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier);
bool SmokeMaterialCanPromoteRigidEmissiveCardFromPod(
    const RtSmokeMaterialRouteInput& material,
    bool allowSwinglightRuntimeState,
    const RtSmokeTranslucentClassifierInfo& classifier);
bool SmokeMaterialCanPromoteEntityFeedRigidEmissiveCard(const idMaterial* material);
bool SmokeMaterialCanPromoteEntityFeedRigidEmissiveCard(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier);
RtSmokeTranslucentSubtype ClassifySmokeTranslucentSubtype(const drawSurf_t* drawSurf);
RtSmokeTranslucentSubtype ClassifySmokeTranslucentSubtype(const drawSurf_t* drawSurf, const RtSmokeTranslucentClassifierInfo& classifier);
RtSmokeTranslucentSubtype ClassifySmokeTranslucentSubtypeFromPod(
    const RtSmokeMaterialRouteInput& input,
    const RtSmokeTranslucentClassifierInfo& classifier);
uint32_t SmokeSurfaceClassAndSubtypeId(RtSmokeSurfaceClass surfaceClass, RtSmokeTranslucentSubtype subtype);
uint32_t SmokeMaterialRouteClassSignature(const idMaterial* material, RtSmokeSurfaceClass surfaceClass, RtSmokeTranslucentSubtype subtype);
uint32_t SmokeMaterialRouteClassSignature(const idMaterial* material, RtSmokeSurfaceClass surfaceClass, RtSmokeTranslucentSubtype subtype, const RtSmokeTranslucentClassifierInfo& classifier);
uint32_t SmokeMaterialRouteClassSignatureFromPod(
    const RtSmokeMaterialRouteInput& input,
    RtSmokeSurfaceClass surfaceClass,
    RtSmokeTranslucentSubtype subtype,
    const RtSmokeTranslucentClassifierInfo& classifier);
