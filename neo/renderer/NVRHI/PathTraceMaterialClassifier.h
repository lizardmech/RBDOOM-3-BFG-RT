#pragma once

// New path-tracing material classifier.
//
// This stack is intentionally separate from PathTraceDoomMaterialClassifier and
// RtSmokeMaterialUniverseFacts. It owns route/surface/BSDF metadata for the new
// material-classifier path while the legacy renderer remains the default path.

#include "PathTraceTextureRegistry.h"

#include <vector>

class idMaterial;

enum class RtMaterialSurfaceClass : uint8_t
{
    Unknown = 0,
    Metal,
    Stone,
    Flesh,
    Wood,
    Cardboard,
    Liquid,
    Glass,
    Plastic,
    Ricochet,
    Special
};

enum class RtMaterialClassConfidence : uint8_t
{
    Authoritative = 0,
    Flag,
    Heuristic,
    FallbackNone
};

enum class RtMaterialBsdfRoute : uint8_t
{
    Unknown = 0,
    RealPbrRmao,
    LegacySpecGloss,
    SurfaceTypeFallback
};

enum class RtMaterialSurfaceClassReason : uint8_t
{
    Unknown = 0,
    MaterialSort,
    SurfaceType,
    StageKind,
    NameToken,
    ImageNameToken,
    FallbackMetal,
    FallbackUnknown
};

enum class RtMaterialBsdfRouteReason : uint8_t
{
    Unknown = 0,
    PbrRmaoStage,
    LegacySpecularStage,
    DiscoveredLegacySpecularImage,
    RmaoDisabled,
    NoCompatibleSpecularStage
};

enum class RtMaterialNormalDecodeMode : uint8_t
{
    None = 0,
    Rgb8Rg,
    CompressedWy
};

enum class RtMaterialCompositingOp : uint8_t
{
    Unknown = 0,
    OpaqueReplace,
    Additive,
    MultiplyFilter,
    InvertedFilterBlackKey,
    SourceAlphaOver,
    AuthoredAlphaClip,
    InteractionInput
};

struct RtMaterialCompositingStageFact
{
    int stageIndex = -1;
    stageLighting_t lighting = SL_AMBIENT;
    RtMaterialCompositingOp operation = RtMaterialCompositingOp::Unknown;
    uint64 srcBlendBits = 0;
    uint64 dstBlendBits = 0;
    bool hasAlphaTest = false;
    bool ignoreAlphaTest = false;
    int alphaTestRegister = -1;
    int conditionRegister = -1;
    bool conditionCanBeActive = false;
    bool conditionIsDynamic = false;
    int colorRegisters[4] = { -1, -1, -1, -1 };
    bool hasTextureMatrix = false;
    int textureMatrixRegisters[2][3] = { { -1, -1, -1 }, { -1, -1, -1 } };
    texgen_t texgen = TG_EXPLICIT;
    dynamicidImage_t dynamicImage = DI_STATIC;
    int dynamicFrameCount = 0;
    stageVertexColor_t vertexColor = SVC_IGNORE;
    int vertexProgram = -1;
    int fragmentProgram = -1;
    int glslProgram = -1;
    idStr imageName;
};

struct RtMaterialInteractionPacket
{
    int firstStageIndex = -1;
    int lastStageIndex = -1;
    int bumpStageIndex = -1;
    int diffuseStageIndex = -1;
    int specularStageIndex = -1;
};

struct RtMaterialStageFacts
{
    int stageCount = 0;
    int bumpStages = 0;
    int diffuseStages = 0;
    int specularStages = 0;
    int pbrRmaoStages = 0;
    int legacySpecStages = 0;
    int ambientStages = 0;
    int additiveBlendStages = 0;
    int filterBlendStages = 0;
    int alphaBlendStages = 0;
    int opaqueReplaceStages = 0;
    int invertedFilterStages = 0;
    int authoredAlphaClipStages = 0;
    int unknownCompositingStages = 0;
    int coverageStages = 0;
    int guiOrScreenStages = 0;
    int dynamicImageStages = 0;
    int cinematicStages = 0;
    int cubeMapStages = 0;
    int reflectCube2Stages = 0;
    int customProgramStages = 0;
    int effectStages = 0;
    int firstDiffuseStage = -1;
    int firstBumpStage = -1;
    int firstSpecularStage = -1;
    int routeStage = -1;
};

struct RtMaterialDynamicFacts
{
    bool materialUsesRuntimeRegisters = false;
    bool projectedDecal = false;
    int conditionRegisterStages = 0;
    int colorRegisterStages = 0;
    int alphaRegisterStages = 0;
    int alphaTestRegisterStages = 0;
    int textureMatrixStages = 0;
    int textureMatrixRegisterStages = 0;
    int dynamicImageStages = 0;
    int cinematicStages = 0;
    int guiRenderTargetStages = 0;
    int customProgramStages = 0;
    int flipbookAtlasStages = 0;
    int flipbookFrames = 0;
    int flipbookAxis = -1;
};

struct RtMaterialBsdfParams
{
    float roughness = 0.7f;
    float metallic = 0.0f;
    float ior = 1.5f;
    float transmission = 0.0f;
    float specularF0 = 0.04f;
    float ao = 1.0f;
    uint8_t subsurfaceHint = 0;
    uint8_t twoSidedBsdf = 0;
};

struct RtMaterialRecord
{
    bool valid = false;
    uint64 signature = 0;
    uint32_t materialId = 0;
    idStr materialName;
    idStr diffuseImageName;
    idStr normalImageName;
    idStr specularImageName;
    idStr emissiveImageName;
    idStr diffuseReason;
    idStr normalReason;
    idStr specularReason;
    idStr emissiveReason;
    idStr liquidFilmCoverageImageName;
    idStr liquidFilmOverrideReason;
    idStr liquidFilmReason;
    idStr surfaceClassEvidence;
    idStr bsdfEvidence;
    int rawSurfaceType = 0;
    int surfaceFlags = 0;
    float sort = 0.0f;
    materialCoverage_t coverage = MC_BAD;
    RtMaterialSurfaceClass surfaceClass = RtMaterialSurfaceClass::Unknown;
    RtMaterialClassConfidence surfaceClassConfidence = RtMaterialClassConfidence::FallbackNone;
    RtMaterialBsdfRoute route = RtMaterialBsdfRoute::Unknown;
    RtMaterialSurfaceClassReason surfaceClassReason = RtMaterialSurfaceClassReason::Unknown;
    RtMaterialBsdfRouteReason routeReason = RtMaterialBsdfRouteReason::Unknown;
    RtMaterialNormalDecodeMode normalDecodeMode = RtMaterialNormalDecodeMode::None;
    RtMaterialStageFacts stageFacts;
    std::vector<RtMaterialCompositingStageFact> compositingStages;
    std::vector<RtMaterialInteractionPacket> interactionPackets;
    RtMaterialDynamicFacts dynamicFacts;
    RtMaterialBsdfParams bsdf;
    textureUsage_t diffuseUsage = TD_DEFAULT;
    textureUsage_t normalUsage = TD_DEFAULT;
    textureUsage_t specularUsage = TD_DEFAULT;
    textureUsage_t emissiveUsage = TD_DEFAULT;
    textureColor_t diffuseColorFormat = CFM_DEFAULT;
    textureColor_t normalColorFormat = CFM_DEFAULT;
    textureColor_t specularColorFormat = CFM_DEFAULT;
    textureColor_t emissiveColorFormat = CFM_DEFAULT;
    bool hasDiffuseImage = false;
    bool hasNormalImage = false;
    bool hasSpecularImage = false;
    bool hasEmissiveImage = false;
    bool alphaTested = false;
    bool emissiveIntent = false;
    bool liquidFilmIsDetailDecal = false;
    bool liquidFilmHasBloodSemantic = false;
    bool liquidFilmHasWetReflectStage = false;
    bool liquidFilmHasCoverageSource = false;
    bool liquidFilmHasWetNormalSource = false;
    bool liquidFilmExactOverride = false;
    bool liquidFilmLegacyPool = false;
    bool liquidFilmCandidate = false;
    bool liquidFilmVariant = false;
    bool liquidFilmDynamic = false;
};

struct RtMaterialClassifierStats
{
    int records = 0;
    int hits = 0;
    int misses = 0;
    int rebuilds = 0;
    int frameHits = 0;
    int frameMisses = 0;
    int frameRebuilds = 0;
    int routeRealPbr = 0;
    int routeLegacySpec = 0;
    int routeFallback = 0;
    int confidenceAuthoritative = 0;
    int confidenceFlag = 0;
    int confidenceHeuristic = 0;
    int confidenceFallbackNone = 0;
    int compositingStages = 0;
    int maxCompositingStages = 0;
    int materialsOverFourStages = 0;
    int materialsOverEightStages = 0;
    int compositingOpaque = 0;
    int compositingAdditive = 0;
    int compositingMultiply = 0;
    int compositingInverted = 0;
    int compositingAlphaOver = 0;
    int compositingAlphaClip = 0;
    int compositingInteraction = 0;
    int compositingUnknown = 0;
};

const char* RtMaterialSurfaceClassName(RtMaterialSurfaceClass surfaceClass);
const char* RtMaterialClassConfidenceName(RtMaterialClassConfidence confidence);
const char* RtMaterialBsdfRouteName(RtMaterialBsdfRoute route);
const char* RtMaterialSurfaceClassReasonName(RtMaterialSurfaceClassReason reason);
const char* RtMaterialBsdfRouteReasonName(RtMaterialBsdfRouteReason reason);
const char* RtMaterialNormalDecodeModeName(RtMaterialNormalDecodeMode mode);
const char* RtMaterialCompositingOpName(RtMaterialCompositingOp operation);

void BeginPathTraceMaterialClassifierFrame();
const RtMaterialRecord& RegisterPathTraceMaterialRecord(const idMaterial* material, const RtSmokeMaterialTextureInfo& info);
const RtMaterialRecord* FindPathTraceMaterialRecord(uint32_t materialId);
int GetPathTraceMaterialRecordCount();
bool PathTraceMaterialRecordLookupIndexSelfTest();
RtMaterialClassifierStats GetPathTraceMaterialClassifierStats();
uint32_t GetPathTraceMaterialClassifierGeneration();
uint32_t PackPathTraceMaterialClassifierFlags(const RtMaterialRecord& record);
uint32_t PackPathTraceMaterialClassifierParams(const RtMaterialRecord& record);
uint32_t PackPathTraceMaterialClassifierDynamicFlags(const RtMaterialRecord& record);
void MaybeDumpPathTraceMaterialDeclSurfaceTypeDistribution();
