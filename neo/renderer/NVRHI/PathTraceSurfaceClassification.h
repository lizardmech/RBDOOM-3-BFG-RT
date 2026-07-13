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

uint32_t SmokeSurfaceClassId(RtSmokeSurfaceClass surfaceClass);
const char* SmokeSurfaceClassName(RtSmokeSurfaceClass surfaceClass);
const char* SmokeSurfaceClassNameByIndex(int classIndex);
const char* RtPtFeedClassName(RtPtFeedClass feedClass);
uint32_t SmokeTranslucentSubtypeId(RtSmokeTranslucentSubtype subtype);
const char* SmokeTranslucentSubtypeName(RtSmokeTranslucentSubtype subtype);
const char* SmokeTranslucentSubtypeNameByIndex(int subtypeIndex);
RtSmokeSurfaceClass ClassifySmokeSurface(const viewDef_t* viewDef, const drawSurf_t* drawSurf, const srfTriangles_t* tri);
RtPtFeedClass ClassifyEntityFeedSurface(const idRenderEntityLocal* entity, const idRenderModel* model, const modelSurface_t* surface);
bool IsEntityFeedSingleBoneSurface(const srfTriangles_t* tri);
bool SmokeMaterialUsesOpaqueSwinglightCompatibility(const idMaterial* material);
bool SmokeMaterialCanPromoteRigidEmissiveCard(const idMaterial* material);
bool SmokeMaterialCanPromoteEntityFeedRigidEmissiveCard(const idMaterial* material);
bool SmokeMaterialCanPromoteEntityFeedRigidEmissiveCard(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier);
RtSmokeTranslucentSubtype ClassifySmokeTranslucentSubtype(const drawSurf_t* drawSurf);
uint32_t SmokeSurfaceClassAndSubtypeId(RtSmokeSurfaceClass surfaceClass, RtSmokeTranslucentSubtype subtype);
uint32_t SmokeMaterialRouteClassSignature(const idMaterial* material, RtSmokeSurfaceClass surfaceClass, RtSmokeTranslucentSubtype subtype);
uint32_t SmokeMaterialRouteClassSignature(const idMaterial* material, RtSmokeSurfaceClass surfaceClass, RtSmokeTranslucentSubtype subtype, const RtSmokeTranslucentClassifierInfo& classifier);
