#include "precompiled.h"
#pragma hdrstop

#include "PathTraceDoomLights.h"
#include "PathTraceCVars.h"
#include "PathTraceGeometryLifecycle.h"
#include "../RenderCommon.h"
#include "../RenderWorld_local.h"
#include "../../d3xp/Game_local.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

extern idCVar r_lightScale;

const uint32_t RT_DOOM_ANALYTIC_LIGHT_CASTS_SHADOWS = 0x00000001u;
const uint32_t RT_DOOM_ANALYTIC_LIGHT_GAME_LINKED = 0x00000002u;
const uint32_t RT_DOOM_ANALYTIC_LIGHT_BEHIND_CAMERA = 0x00000004u;

namespace {

const int RT_PT_DOOM_LIGHT_MAX_AREA_REFS = 32;
const int RT_PT_DOOM_DYNAMIC_LIGHT_PRUNE_MISSING_FRAMES = 300;
const uint32_t RT_PT_DOOM_LIGHT_INVALID_INDEX = 0xffffffffu;
const uint32_t RT_PT_DOOM_LIGHT_INVALID_ENTITY_NUMBER = 0xffffffffu;

float FinitePositiveOrZero(float value)
{
    return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

enum DoomAnalyticLightUniverseTemporalState : uint32_t
{
    DOOM_LIGHT_UNIVERSE_STATE_ACTIVE = 0x00000001u,
    DOOM_LIGHT_UNIVERSE_STATE_SAMPLEABLE = 0x00000002u,
    DOOM_LIGHT_UNIVERSE_STATE_SELECTED_AREA = 0x00000004u,
    DOOM_LIGHT_UNIVERSE_STATE_SUPPRESSED = 0x00000008u,
    DOOM_LIGHT_UNIVERSE_STATE_GAME_LINKED = 0x00000010u,
    DOOM_LIGHT_UNIVERSE_STATE_CONTINUITY_PROVEN = 0x00000020u,
};

enum DoomAnalyticLightKind : uint32_t
{
    DOOM_ANALYTIC_LIGHT_KIND_POINT = 1u,
};

struct DoomAnalyticLightUniverseKey
{
    uint32_t renderLightIndex = RT_PT_DOOM_LIGHT_INVALID_INDEX;
    uint32_t entityNumber = RT_PT_DOOM_LIGHT_INVALID_ENTITY_NUMBER;
    uint32_t kind = DOOM_ANALYTIC_LIGHT_KIND_POINT;

    bool operator==(const DoomAnalyticLightUniverseKey& other) const
    {
        return renderLightIndex == other.renderLightIndex &&
            entityNumber == other.entityNumber &&
            kind == other.kind;
    }
};

struct DoomAnalyticLightUniverseKeyHash
{
    size_t operator()(const DoomAnalyticLightUniverseKey& key) const
    {
        uint64_t hash = 1469598103934665603ull;
        auto mix = [&](uint32_t value) {
            hash ^= static_cast<uint64_t>(value);
            hash *= 1099511628211ull;
        };
        mix(key.renderLightIndex);
        mix(key.entityNumber);
        mix(key.kind);
        return static_cast<size_t>(hash);
    }
};

struct DoomAnalyticLightUniverseEntry
{
    DoomAnalyticLightUniverseKey key;
    uint32_t universeIndex = RT_PT_DOOM_LIGHT_INVALID_INDEX;
    uint32_t previousUniverseIndex = RT_PT_DOOM_LIGHT_INVALID_INDEX;
    uint32_t currentCandidateIndex = RT_PT_DOOM_LIGHT_INVALID_INDEX;
    bool hasPrevious = false;
    bool remapValid = false;
    bool duplicateKey = false;
    bool active = false;
    bool sampleable = false;
    bool continuityProven = false;
    uint32_t flags = 0;
    uint32_t invalidReasonFlags = 0;
    uint32_t temporalStateFlags = 0;
    idVec3 origin = vec3_zero;
    idVec4 color = idVec4(0.0f, 0.0f, 0.0f, 0.0f);
    float doomRadius = 0.0f;
    float sphereRadius = 0.0f;
    int selectionArea = -1;
    int portalDepth = -1;
};

struct DoomPersistentAuthoredLightEntry
{
    DoomAnalyticLightUniverseKey key;
    uint32_t universeIndex = RT_PT_DOOM_LIGHT_INVALID_INDEX;
    int missingFrames = 0;
    bool staticAuthored = false;
    bool currentSeen = false;
    bool currentActive = false;
    bool currentSampleable = false;
    bool currentSelectedArea = false;
    bool currentConnectedArea = false;
    bool currentSuppressed = false;
    bool currentContinuityProven = false;
    bool currentDuplicateKey = false;
    bool currentPointLight = false;
    bool currentParallel = false;
    bool currentCastsShadows = false;
    bool currentGameLinked = false;
    bool currentCrosshairBehind = false;
    uint32_t currentCandidateIndex = RT_PT_DOOM_LIGHT_INVALID_INDEX;
    uint32_t invalidReasonFlags = 0;
    idVec3 currentOrigin = vec3_zero;
    idVec4 currentColor = idVec4(0.0f, 0.0f, 0.0f, 0.0f);
    float currentDoomRadius = 0.0f;
    float currentSphereRadius = 0.0f;
    int currentSelectionArea = -1;
    int currentPortalDepth = -1;
};

struct DoomPersistentAuthoredLightProposal
{
    uint32_t universeIndex = RT_PT_DOOM_LIGHT_INVALID_INDEX;
    uint32_t candidateIndex = RT_PT_DOOM_LIGHT_INVALID_INDEX;
};

struct DoomAnalyticLightStableKey
{
    DoomAnalyticLightUniverseKey key;
    uint32_t universeIndex = RT_PT_DOOM_LIGHT_INVALID_INDEX;
};

struct DoomAnalyticLightUniverseState
{
    const idRenderWorldLocal* renderWorld = nullptr;
    idStr mapName;
    ID_TIME_T mapTimeStamp = 0;
    uint32_t nextUniverseIndex = 0;
    std::vector<DoomAnalyticLightStableKey> stableKeys;
    std::vector<DoomAnalyticLightUniverseEntry> previousEntries;
    std::vector<DoomAnalyticLightUniverseEntry> currentEntries;
    std::vector<DoomPersistentAuthoredLightEntry> persistentAuthoredLights;
    std::vector<DoomPersistentAuthoredLightProposal> persistentAuthoredProposals;

    void Reset(const idRenderWorldLocal* newRenderWorld, const char* newMapName, ID_TIME_T newMapTimeStamp)
    {
        renderWorld = newRenderWorld;
        mapName = newMapName ? newMapName : "";
        mapTimeStamp = newMapTimeStamp;
        nextUniverseIndex = 0;
        stableKeys.clear();
        previousEntries.clear();
        currentEntries.clear();
        persistentAuthoredLights.clear();
        persistentAuthoredProposals.clear();
    }
};

DoomAnalyticLightUniverseState g_doomAnalyticLightUniverse;
PathTraceDoomAnalyticLightGpuRemap g_doomAnalyticLightGpuRemap;

struct DoomLightPortalSelection
{
    int currentArea = -1;
    int portalSteps = 0;
    int selectedAreaCount = 0;
    int portalEdges = 0;
    int blockedPortalEdges = 0;
    std::vector<int> depthByArea;
};

struct DoomLightRecord
{
    int index = -1;
    int area = -1;
    int originArea = -1;
    int selectionArea = -1;
    int portalDepth = -1;
    int boundsAreaCount = 0;
    int selectedBoundsAreaCount = 0;
    int connectedBoundsAreaCount = 0;
    bool selectedArea = false;
    bool connectedArea = false;
    bool active = false;
    bool suppressed = false;
    bool pointLight = false;
    bool parallel = false;
    bool castsShadows = false;
    bool visibleInView = false;
    bool spriteProxy = false;
    idVec3 origin = vec3_zero;
    idVec3 radius = vec3_zero;
    idBounds bounds;
    idVec4 color = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
    float radiusMax = 0.0f;
    float distance = 0.0f;
    float crosshairT = -1.0f;
    float crosshairDistance = -1.0f;
    float crosshairScore = idMath::INFINITUM;
    bool crosshairBehind = false;
    float sphereRadius = 0.0f;
    const char* shaderName = "<none>";
    const char* entityName = "<unavailable>";
    const char* entityClassname = "<unavailable>";
    const char* entityDefName = "<unavailable>";
    const char* spawnTexture = "<unavailable>";
    const char* spawnModel = "<unavailable>";
    const char* spawnBind = "<none>";
    const char* spawnTarget = "<none>";
    const char* spawnBroken = "<none>";
    bool gameLinked = false;
    bool gameHidden = false;
    bool spawnStartOff = false;
    bool spawnBreak = false;
    bool spawnNoShadows = false;
    bool spawnNoSpecular = false;
    int entityNumber = -1;
    int levels = 0;
    int currentLevel = 0;
    int spawnCount = 0;
    int health = 0;
    idVec3 baseColor = vec3_zero;
    idVec3 currentGameColor = vec3_zero;
};

struct DoomLightGameMetadata
{
    const idLight* light = nullptr;
    const char* entityName = "<unavailable>";
    const char* entityClassname = "<unavailable>";
    const char* entityDefName = "<unavailable>";
    const char* spawnTexture = "<unavailable>";
    const char* spawnModel = "<unavailable>";
    const char* spawnBind = "<none>";
    const char* spawnTarget = "<none>";
    const char* spawnBroken = "<none>";
    bool hidden = false;
    bool startOff = false;
    bool breakOnTrigger = false;
    bool noShadows = false;
    bool noSpecular = false;
    int entityNumber = -1;
    int levels = 0;
    int currentLevel = 0;
    int count = 0;
    int health = 0;
    idVec3 baseColor = vec3_zero;
    idVec3 currentColor = vec3_zero;
};

struct DoomResidentLightRecord
{
    bool valid = false;
    PtRenderDefKey key;
    uint32_t lightGeneration = 0;
    DoomLightRecord record;
    int uniqueAreas[RT_PT_DOOM_LIGHT_MAX_AREA_REFS + 2] = {};
    int uniqueAreaCount = 0;
    int boundsAreas[RT_PT_DOOM_LIGHT_MAX_AREA_REFS] = {};
    int boundsAreaCount = 0;
    int lastSeenFrame = 0;
};

struct DoomResidentLightStats
{
    int visited = 0;
    int derived = 0;
    int hits = 0;
    int misses = 0;
    int dirty = 0;
    int residents = 0;
};

struct DoomResidentLightCache
{
    const idRenderWorldLocal* renderWorld = nullptr;
    idStr mapName;
    ID_TIME_T mapTimeStamp = 0;
    std::vector<DoomResidentLightRecord> records;
    DoomResidentLightStats stats;

    void Reset(const idRenderWorldLocal* newRenderWorld, const char* newMapName, ID_TIME_T newMapTimeStamp)
    {
        renderWorld = newRenderWorld;
        mapName = newMapName ? newMapName : "";
        mapTimeStamp = newMapTimeStamp;
        records.clear();
        stats = DoomResidentLightStats();
    }
};

DoomResidentLightCache g_doomResidentLightCache;

bool IsDoomLightGameStateActive()
{
    return gameLocal.GameState() == GAMESTATE_ACTIVE;
}

std::unordered_map<int, DoomLightGameMetadata> BuildDoomLightGameMetadataByHandle()
{
    std::unordered_map<int, DoomLightGameMetadata> metadataByHandle;
    if (!IsDoomLightGameStateActive())
    {
        return metadataByHandle;
    }

    metadataByHandle.reserve(128);
    for (int entityIndex = 0; entityIndex < gameLocal.num_entities; ++entityIndex)
    {
        idEntity* entity = gameLocal.entities[entityIndex];
        if (!entity || !entity->IsType(idLight::Type))
        {
            continue;
        }

        const idLight* light = static_cast<const idLight*>(entity);
        const qhandle_t lightHandle = light->GetLightDefHandle();
        if (lightHandle < 0)
        {
            continue;
        }

        DoomLightGameMetadata metadata;
        metadata.light = light;
        metadata.entityName = light->GetName();
        metadata.entityClassname = light->spawnArgs.GetString("classname", "<none>");
        metadata.entityDefName = light->GetEntityDefName();
        metadata.spawnTexture = light->spawnArgs.GetString("texture", "lights/squarelight1");
        metadata.spawnModel = light->spawnArgs.GetString("model", "<none>");
        metadata.spawnBind = light->spawnArgs.GetString("bind", "<none>");
        metadata.spawnTarget = light->spawnArgs.GetString("target", "<none>");
        metadata.spawnBroken = light->spawnArgs.GetString("broken", "<none>");
        metadata.hidden = light->fl.hidden;
        metadata.startOff = light->spawnArgs.GetBool("start_off", "0");
        metadata.breakOnTrigger = light->spawnArgs.GetBool("break", "0");
        metadata.noShadows = light->spawnArgs.GetBool("noshadows", "0");
        metadata.noSpecular = light->spawnArgs.GetBool("nospecular", "0");
        metadata.entityNumber = light->GetEntityNumber();
        metadata.levels = light->GetLightLevels();
        metadata.currentLevel = light->GetCurrentLightLevel();
        metadata.count = light->spawnArgs.GetInt("count", "1");
        metadata.health = light->health;
        metadata.baseColor = light->GetBaseColor();
        light->GetColor(metadata.currentColor);
        metadataByHandle[lightHandle] = metadata;
    }
    return metadataByHandle;
}

int ResolveCurrentDoomLightArea(const viewDef_t* viewDef)
{
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        return -1;
    }

    int area = viewDef->areaNum;
    if (area < 0)
    {
        area = renderWorld->PointInArea(viewDef->initialViewAreaOrigin);
    }
    if (area < 0)
    {
        area = renderWorld->PointInArea(viewDef->renderView.vieworg);
    }
    return area;
}

std::vector<int> ResolveDoomLightSeedAreas(const viewDef_t* viewDef)
{
    std::vector<int> seedAreas;
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        return seedAreas;
    }

    const int areaCount = renderWorld->NumAreas();
    auto addSeedArea = [&](const int area) {
        if (area < 0 || area >= areaCount)
        {
            return;
        }
        if (std::find(seedAreas.begin(), seedAreas.end(), area) == seedAreas.end())
        {
            seedAreas.push_back(area);
        }
    };

    addSeedArea(viewDef->areaNum);
    addSeedArea(renderWorld->PointInArea(viewDef->initialViewAreaOrigin));
    addSeedArea(renderWorld->PointInArea(viewDef->renderView.vieworg));

    const idVec3& viewOrigin = viewDef->renderView.vieworg;
    const float probeDistance = 8.0f;
    addSeedArea(renderWorld->PointInArea(viewOrigin + viewDef->renderView.viewaxis[0] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin - viewDef->renderView.viewaxis[0] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin + viewDef->renderView.viewaxis[1] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin - viewDef->renderView.viewaxis[1] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin + viewDef->renderView.viewaxis[2] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin - viewDef->renderView.viewaxis[2] * probeDistance));
    return seedAreas;
}

DoomLightPortalSelection BuildDoomLightPortalSelection(const viewDef_t* viewDef, int portalSteps)
{
    DoomLightPortalSelection selection;
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    const std::vector<int> seedAreas = ResolveDoomLightSeedAreas(viewDef);
    selection.currentArea = !seedAreas.empty() ? seedAreas[0] : ResolveCurrentDoomLightArea(viewDef);
    selection.portalSteps = idMath::ClampInt(0, 8, portalSteps);
    if (!renderWorld || seedAreas.empty())
    {
        return selection;
    }

    const int areaCount = renderWorld->NumAreas();

    selection.depthByArea.assign(areaCount, -1);
    if (r_pathTracingPortalBruteforceFullMap.GetInteger() != 0)
    {
        std::fill(selection.depthByArea.begin(), selection.depthByArea.end(), 0);
        selection.selectedAreaCount = areaCount;
        return selection;
    }

    std::vector<int> queue;
    queue.reserve(areaCount);
    for (int seedArea : seedAreas)
    {
        selection.depthByArea[seedArea] = 0;
        queue.push_back(seedArea);
    }

    for (size_t queueIndex = 0; queueIndex < queue.size(); ++queueIndex)
    {
        const int area = queue[queueIndex];
        const int depth = selection.depthByArea[area];
        if (depth >= selection.portalSteps)
        {
            continue;
        }

        const int portalCount = renderWorld->NumPortalsInArea(area);
        for (int portalIndex = 0; portalIndex < portalCount; ++portalIndex)
        {
            const exitPortal_t portal = renderWorld->GetPortal(area, portalIndex);
            if ((portal.blockingBits & PS_BLOCK_VIEW) != 0)
            {
                ++selection.blockedPortalEdges;
            }

            int nextArea = -1;
            if (portal.areas[0] == area)
            {
                nextArea = portal.areas[1];
            }
            else if (portal.areas[1] == area)
            {
                nextArea = portal.areas[0];
            }
            if (nextArea < 0 || nextArea >= areaCount)
            {
                continue;
            }

            ++selection.portalEdges;
            if (selection.depthByArea[nextArea] < 0)
            {
                selection.depthByArea[nextArea] = depth + 1;
                queue.push_back(nextArea);
            }
        }
    }

    for (int depth : selection.depthByArea)
    {
        if (depth >= 0)
        {
            ++selection.selectedAreaCount;
        }
    }
    return selection;
}

idVec4 EvaluateDoomLightColor(const viewDef_t* viewDef, const idRenderLightLocal* light, bool& active)
{
    active = false;
    if (!viewDef || !light || !light->lightShader)
    {
        return idVec4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    const idMaterial* lightShader = light->lightShader;
    std::vector<float> registers;
    const float* lightRegs = lightShader->ConstantRegisters();
    if (!lightRegs)
    {
        registers.resize(lightShader->GetNumRegisters());
        lightShader->EvaluateRegisters(
            registers.data(),
            light->parms.shaderParms,
            viewDef->renderView.shaderParms,
            viewDef->renderView.time[0] * 0.001f,
            light->parms.referenceSound);
        lightRegs = registers.data();
    }

    const float lightScale = r_lightScale.GetFloat();
    idVec4 bestColor(0.0f, 0.0f, 0.0f, 0.0f);
    float bestIntensity = 0.0f;
    for (int lightStageNum = 0; lightStageNum < lightShader->GetNumStages(); ++lightStageNum)
    {
        const shaderStage_t* lightStage = lightShader->GetStage(lightStageNum);
        if (!lightStage || !lightRegs[lightStage->conditionRegister])
        {
            continue;
        }

        const int* colorRegisters = lightStage->color.registers;
        const idVec4 color(
            lightScale * lightRegs[colorRegisters[0]],
            lightScale * lightRegs[colorRegisters[1]],
            lightScale * lightRegs[colorRegisters[2]],
            lightRegs[colorRegisters[3]]);
        const float intensity = Max(color.x, Max(color.y, color.z));
        if (intensity > bestIntensity)
        {
            bestIntensity = intensity;
            bestColor = color;
            bestColor.w = intensity;
        }
    }

    active = bestIntensity > 0.0f;
    return bestColor;
}

idVec4 DoomAnalyticColorFromVec3(const idVec3& color)
{
    const float lightScale = r_lightScale.GetFloat();
    const idVec4 result(
        lightScale * Max(color.x, 0.0f),
        lightScale * Max(color.y, 0.0f),
        lightScale * Max(color.z, 0.0f),
        0.0f);
    return idVec4(result.x, result.y, result.z, Max(result.x, Max(result.y, result.z)));
}

bool IsDoomLightSuppressedForView(const viewDef_t* viewDef, const idRenderLightLocal* light)
{
    if (!viewDef || !light)
    {
        return true;
    }
    return (light->parms.suppressLightInViewID != 0 && light->parms.suppressLightInViewID == viewDef->renderView.viewID) ||
        (light->parms.allowLightInViewID != 0 && light->parms.allowLightInViewID != viewDef->renderView.viewID);
}

void FillDoomLightCrosshairMetrics(const viewDef_t* viewDef, DoomLightRecord& record)
{
    if (!viewDef)
    {
        return;
    }

    idVec3 forward = viewDef->renderView.viewaxis[0];
    forward.Normalize();
    const idVec3 toLight = record.origin - viewDef->renderView.vieworg;
    record.crosshairT = toLight * forward;
    if (record.crosshairT < 0.0f)
    {
        record.crosshairBehind = true;
        return;
    }

    const idVec3 closestPoint = viewDef->renderView.vieworg + forward * record.crosshairT;
    record.crosshairDistance = (record.origin - closestPoint).Length();
    record.crosshairScore = record.crosshairDistance / Max(record.radiusMax, 1.0f);
}

void ClearDoomLightGameMetadataFields(DoomLightRecord& record)
{
    record.entityName = "<unavailable>";
    record.entityClassname = "<unavailable>";
    record.entityDefName = "<unavailable>";
    record.spawnTexture = "<unavailable>";
    record.spawnModel = "<unavailable>";
    record.spawnBind = "<none>";
    record.spawnTarget = "<none>";
    record.spawnBroken = "<none>";
    record.gameLinked = false;
    record.gameHidden = false;
    record.spawnStartOff = false;
    record.spawnBreak = false;
    record.spawnNoShadows = false;
    record.spawnNoSpecular = false;
    record.entityNumber = -1;
    record.levels = 0;
    record.currentLevel = 0;
    record.spawnCount = 0;
    record.health = 0;
    record.baseColor = vec3_zero;
    record.currentGameColor = vec3_zero;
}

void ApplyDoomLightGameMetadataToRecord(const viewDef_t* viewDef, const DoomLightGameMetadata& metadata, DoomLightRecord& record)
{
    record.gameLinked = true;
    record.entityName = metadata.entityName;
    record.entityClassname = metadata.entityClassname;
    record.entityDefName = metadata.entityDefName;
    record.spawnTexture = metadata.spawnTexture;
    record.spawnModel = metadata.spawnModel;
    record.spawnBind = metadata.spawnBind;
    record.spawnTarget = metadata.spawnTarget;
    record.spawnBroken = metadata.spawnBroken;
    record.gameHidden = metadata.hidden;
    record.spawnStartOff = metadata.startOff;
    record.spawnBreak = metadata.breakOnTrigger;
    record.spawnNoShadows = metadata.noShadows;
    record.spawnNoSpecular = metadata.noSpecular;
    record.entityNumber = metadata.entityNumber;
    record.levels = metadata.levels;
    record.currentLevel = metadata.currentLevel;
    record.spawnCount = metadata.count;
    record.health = metadata.health;
    record.baseColor = metadata.baseColor;
    record.currentGameColor = metadata.currentColor;
    const bool cleanDoomColorRoute = r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0;
    const bool remixLightUniverseColorRoute =
        r_pathTracingRemixLightUniverseEnable.GetInteger() != 0 ||
        (r_pathTracingReGIREnable.GetInteger() != 0 && r_pathTracingReGIRMode.GetInteger() != 0);
    const int doomColorSource = cleanDoomColorRoute
        ? idMath::ClampInt(0, 2, r_pathTracingCleanRtxdiDiDoomColorSource.GetInteger())
        : (remixLightUniverseColorRoute
            ? idMath::ClampInt(0, 2, r_pathTracingRemixLightUniverseDoomColorSource.GetInteger())
            : 0);
    if (doomColorSource == 1)
    {
        record.color = DoomAnalyticColorFromVec3(record.currentGameColor);
        record.active = record.color.w > 0.0f && !record.suppressed;
    }
    else if (doomColorSource == 2)
    {
        record.color = DoomAnalyticColorFromVec3(record.baseColor);
        record.active = record.color.w > 0.0f && !record.suppressed && !record.gameHidden && record.currentLevel > 0;
    }

    if (record.gameHidden || record.currentLevel <= 0)
    {
        record.color = idVec4(0.0f, 0.0f, 0.0f, 0.0f);
        record.active = false;
    }
}

DoomLightGameMetadata BuildDoomLightGameMetadataFromLight(const idLight* light)
{
    DoomLightGameMetadata metadata;
    if (!light)
    {
        return metadata;
    }

    metadata.light = light;
    metadata.entityName = light->GetName();
    metadata.entityClassname = light->spawnArgs.GetString("classname", "<none>");
    metadata.entityDefName = light->GetEntityDefName();
    metadata.spawnTexture = light->spawnArgs.GetString("texture", "lights/squarelight1");
    metadata.spawnModel = light->spawnArgs.GetString("model", "<none>");
    metadata.spawnBind = light->spawnArgs.GetString("bind", "<none>");
    metadata.spawnTarget = light->spawnArgs.GetString("target", "<none>");
    metadata.spawnBroken = light->spawnArgs.GetString("broken", "<none>");
    metadata.hidden = light->fl.hidden;
    metadata.startOff = light->spawnArgs.GetBool("start_off", "0");
    metadata.breakOnTrigger = light->spawnArgs.GetBool("break", "0");
    metadata.noShadows = light->spawnArgs.GetBool("noshadows", "0");
    metadata.noSpecular = light->spawnArgs.GetBool("nospecular", "0");
    metadata.entityNumber = light->GetEntityNumber();
    metadata.levels = light->GetLightLevels();
    metadata.currentLevel = light->GetCurrentLightLevel();
    metadata.count = light->spawnArgs.GetInt("count", "1");
    metadata.health = light->health;
    metadata.baseColor = light->GetBaseColor();
    light->GetColor(metadata.currentColor);
    return metadata;
}

bool TryBuildDoomLightGameMetadataFromEntityNumber(int entityNumber, int expectedLightHandle, DoomLightGameMetadata& metadata)
{
    if (!IsDoomLightGameStateActive() || entityNumber < 0 || entityNumber >= gameLocal.num_entities)
    {
        return false;
    }

    idEntity* entity = gameLocal.entities[entityNumber];
    if (!entity || !entity->IsType(idLight::Type))
    {
        return false;
    }

    const idLight* light = static_cast<const idLight*>(entity);
    if (light->GetLightDefHandle() != expectedLightHandle)
    {
        return false;
    }

    metadata = BuildDoomLightGameMetadataFromLight(light);
    return true;
}

DoomLightRecord BuildDoomLightRecord(
    const viewDef_t* viewDef,
    const idRenderLightLocal* light,
    const DoomLightPortalSelection& selection,
    const std::unordered_map<int, DoomLightGameMetadata>& gameMetadataByHandle)
{
    DoomLightRecord record;
    if (!viewDef || !light)
    {
        return record;
    }

    idRenderWorldLocal* renderWorld = viewDef->renderWorld;
    record.index = light->index;
    record.origin = light->globalLightOrigin;
    record.radius = light->parms.lightRadius;
    record.radiusMax = Max(record.radius.x, Max(record.radius.y, record.radius.z));
    record.bounds = light->globalLightBounds;
    record.area = light->areaNum;
    record.originArea = renderWorld ? renderWorld->PointInArea(record.origin) : -1;
    if (record.area < 0)
    {
        record.area = record.originArea;
    }

    int uniqueAreas[RT_PT_DOOM_LIGHT_MAX_AREA_REFS + 2];
    int uniqueAreaCount = 0;
    auto addUniqueArea = [&](const int area) {
        if (area < 0)
        {
            return;
        }
        for (int i = 0; i < uniqueAreaCount; ++i)
        {
            if (uniqueAreas[i] == area)
            {
                return;
            }
        }
        if (uniqueAreaCount < static_cast<int>(sizeof(uniqueAreas) / sizeof(uniqueAreas[0])))
        {
            uniqueAreas[uniqueAreaCount++] = area;
        }
    };

    addUniqueArea(record.area);
    addUniqueArea(record.originArea);

    int boundsAreas[RT_PT_DOOM_LIGHT_MAX_AREA_REFS];
    const int boundsAreaCount = renderWorld ? renderWorld->BoundsInAreas(record.bounds, boundsAreas, RT_PT_DOOM_LIGHT_MAX_AREA_REFS) : 0;
    record.boundsAreaCount = boundsAreaCount;
    for (int i = 0; i < boundsAreaCount; ++i)
    {
        const int boundsArea = boundsAreas[i];
        addUniqueArea(boundsArea);
        if (boundsArea >= 0 && boundsArea < static_cast<int>(selection.depthByArea.size()) && selection.depthByArea[boundsArea] >= 0)
        {
            ++record.selectedBoundsAreaCount;
        }
        if (viewDef->connectedAreas && renderWorld && boundsArea >= 0 && boundsArea < renderWorld->NumAreas() && viewDef->connectedAreas[boundsArea])
        {
            ++record.connectedBoundsAreaCount;
        }
    }

    for (int i = 0; i < uniqueAreaCount; ++i)
    {
        const int area = uniqueAreas[i];
        if (area >= 0 && area < static_cast<int>(selection.depthByArea.size()))
        {
            const int depth = selection.depthByArea[area];
            if (depth >= 0 && (!record.selectedArea || depth < record.portalDepth))
            {
                record.portalDepth = depth;
                record.selectionArea = area;
                record.selectedArea = true;
            }
        }
        if (viewDef->connectedAreas && renderWorld && area >= 0 && area < renderWorld->NumAreas() && viewDef->connectedAreas[area])
        {
            record.connectedArea = true;
        }
    }

    record.suppressed = IsDoomLightSuppressedForView(viewDef, light);
    record.color = EvaluateDoomLightColor(viewDef, light, record.active);
    record.active = record.active && !record.suppressed;
    record.pointLight = light->parms.pointLight;
    record.parallel = light->parms.parallel;
    record.castsShadows = light->lightShader ? light->LightCastsShadows() : false;
    record.visibleInView = light->viewCount == tr.viewCount && light->viewLight && !light->viewLight->removeFromList;
    record.shaderName = light->lightShader ? light->lightShader->GetName() : "<none>";
    record.distance = (record.origin - viewDef->renderView.vieworg).Length();
    const auto gameMetadataIt = gameMetadataByHandle.find(light->index);
    if (gameMetadataIt != gameMetadataByHandle.end())
    {
        ApplyDoomLightGameMetadataToRecord(viewDef, gameMetadataIt->second, record);
    }
    FillDoomLightCrosshairMetrics(viewDef, record);
    return record;
}

std::vector<DoomLightRecord> CollectDoomLightRecords(
    const viewDef_t* viewDef,
    const DoomLightPortalSelection& selection,
    const std::unordered_map<int, DoomLightGameMetadata>& gameMetadataByHandle)
{
    std::vector<DoomLightRecord> records;
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        return records;
    }

    records.reserve(renderWorld->lightDefs.Num());
    for (int lightIndex = 0; lightIndex < renderWorld->lightDefs.Num(); ++lightIndex)
    {
        const idRenderLightLocal* light = renderWorld->lightDefs[lightIndex];
        if (!light)
        {
            continue;
        }
        records.push_back(BuildDoomLightRecord(viewDef, light, selection, gameMetadataByHandle));
    }
    return records;
}

bool DoomLightResidencyEnabled()
{
    return r_pathTracingResidency.GetInteger() != 0 && r_pathTracingResidencyLights.GetInteger() != 0;
}

void EnsureDoomResidentLightCacheForWorld(const viewDef_t* viewDef)
{
    const idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    const char* mapName = renderWorld ? renderWorld->mapName.c_str() : "";
    const ID_TIME_T mapTimeStamp = renderWorld ? renderWorld->mapTimeStamp : 0;
    if (g_doomResidentLightCache.renderWorld != renderWorld ||
        g_doomResidentLightCache.mapName.Icmp(mapName) != 0 ||
        g_doomResidentLightCache.mapTimeStamp != mapTimeStamp)
    {
        g_doomResidentLightCache.Reset(renderWorld, mapName, mapTimeStamp);
    }
    g_doomResidentLightCache.stats = DoomResidentLightStats();
}

void DumpDoomResidentLightStatsIfNeeded()
{
    if (r_pathTracingResidencyDump.GetInteger() == 0)
    {
        return;
    }

    static int lastDumpFrame = -120;
    if (tr.frameCount - lastDumpFrame < 120)
    {
        return;
    }
    lastDumpFrame = tr.frameCount;
    common->Printf(
        "PathTracePrimaryPass: RES doomLights visited=%d derived=%d hits=%d misses=%d dirty=%d residents=%d\n",
        g_doomResidentLightCache.stats.visited,
        g_doomResidentLightCache.stats.derived,
        g_doomResidentLightCache.stats.hits,
        g_doomResidentLightCache.stats.misses,
        g_doomResidentLightCache.stats.dirty,
        g_doomResidentLightCache.stats.residents);
}

void AddDoomResidentLightArea(DoomResidentLightRecord& resident, int area)
{
    if (area < 0)
    {
        return;
    }
    for (int i = 0; i < resident.uniqueAreaCount; ++i)
    {
        if (resident.uniqueAreas[i] == area)
        {
            return;
        }
    }
    if (resident.uniqueAreaCount < static_cast<int>(sizeof(resident.uniqueAreas) / sizeof(resident.uniqueAreas[0])))
    {
        resident.uniqueAreas[resident.uniqueAreaCount++] = area;
    }
}

void PopulateDoomResidentLightAreaFacts(const viewDef_t* viewDef, const DoomLightRecord& record, DoomResidentLightRecord& resident)
{
    resident.uniqueAreaCount = 0;
    resident.boundsAreaCount = 0;
    AddDoomResidentLightArea(resident, record.area);
    AddDoomResidentLightArea(resident, record.originArea);

    const idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    resident.boundsAreaCount = renderWorld ? renderWorld->BoundsInAreas(record.bounds, resident.boundsAreas, RT_PT_DOOM_LIGHT_MAX_AREA_REFS) : 0;
    for (int i = 0; i < resident.boundsAreaCount; ++i)
    {
        AddDoomResidentLightArea(resident, resident.boundsAreas[i]);
    }
}

void RefreshDoomRecordAreaSelectionFromResident(
    const viewDef_t* viewDef,
    const DoomLightPortalSelection& selection,
    const DoomResidentLightRecord& resident,
    DoomLightRecord& record)
{
    record.selectedArea = false;
    record.connectedArea = false;
    record.selectionArea = -1;
    record.portalDepth = -1;
    record.boundsAreaCount = resident.boundsAreaCount;
    record.selectedBoundsAreaCount = 0;
    record.connectedBoundsAreaCount = 0;

    const idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    for (int i = 0; i < resident.boundsAreaCount; ++i)
    {
        const int area = resident.boundsAreas[i];
        if (area >= 0 && area < static_cast<int>(selection.depthByArea.size()) && selection.depthByArea[area] >= 0)
        {
            ++record.selectedBoundsAreaCount;
        }
        if (viewDef && viewDef->connectedAreas && renderWorld && area >= 0 && area < renderWorld->NumAreas() && viewDef->connectedAreas[area])
        {
            ++record.connectedBoundsAreaCount;
        }
    }

    for (int i = 0; i < resident.uniqueAreaCount; ++i)
    {
        const int area = resident.uniqueAreas[i];
        if (area >= 0 && area < static_cast<int>(selection.depthByArea.size()))
        {
            const int depth = selection.depthByArea[area];
            if (depth >= 0 && (!record.selectedArea || depth < record.portalDepth))
            {
                record.portalDepth = depth;
                record.selectionArea = area;
                record.selectedArea = true;
            }
        }
        if (viewDef && viewDef->connectedAreas && renderWorld && area >= 0 && area < renderWorld->NumAreas() && viewDef->connectedAreas[area])
        {
            record.connectedArea = true;
        }
    }
}

void RefreshDoomResidentLightDynamicFields(
    const viewDef_t* viewDef,
    const idRenderLightLocal* light,
    const DoomLightPortalSelection& selection,
    const DoomResidentLightRecord& resident,
    DoomLightRecord& record)
{
    record = resident.record;
    if (!viewDef || !light)
    {
        return;
    }

    record.index = light->index;
    record.origin = light->globalLightOrigin;
    record.radius = light->parms.lightRadius;
    record.radiusMax = Max(record.radius.x, Max(record.radius.y, record.radius.z));
    record.bounds = light->globalLightBounds;
    record.area = light->areaNum;
    record.originArea = viewDef->renderWorld ? viewDef->renderWorld->PointInArea(record.origin) : -1;
    if (record.area < 0)
    {
        record.area = record.originArea;
    }
    RefreshDoomRecordAreaSelectionFromResident(viewDef, selection, resident, record);

    record.suppressed = IsDoomLightSuppressedForView(viewDef, light);
    record.color = EvaluateDoomLightColor(viewDef, light, record.active);
    record.active = record.active && !record.suppressed;
    record.visibleInView = light->viewCount == tr.viewCount && light->viewLight && !light->viewLight->removeFromList;
    record.distance = (record.origin - viewDef->renderView.vieworg).Length();
    record.crosshairT = -1.0f;
    record.crosshairDistance = -1.0f;
    record.crosshairScore = idMath::INFINITUM;
    record.crosshairBehind = false;
    ClearDoomLightGameMetadataFields(record);

    DoomLightGameMetadata metadata;
    if (TryBuildDoomLightGameMetadataFromEntityNumber(resident.record.entityNumber, light->index, metadata))
    {
        ApplyDoomLightGameMetadataToRecord(viewDef, metadata, record);
    }
    FillDoomLightCrosshairMetrics(viewDef, record);
}

std::vector<DoomLightRecord> CollectDoomLightRecordsResident(
    const viewDef_t* viewDef,
    const DoomLightPortalSelection& selection)
{
    std::vector<DoomLightRecord> records;
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        g_doomResidentLightCache.Reset(nullptr, "", 0);
        return records;
    }

    EnsureDoomResidentLightCacheForWorld(viewDef);
    g_doomResidentLightCache.records.resize(renderWorld->lightDefs.Num());
    records.reserve(renderWorld->lightDefs.Num());

    bool needsMetadataMap = false;
    for (int lightIndex = 0; lightIndex < renderWorld->lightDefs.Num(); ++lightIndex)
    {
        const idRenderLightLocal* light = renderWorld->lightDefs[lightIndex];
        if (!light)
        {
            continue;
        }

        const PtRenderDefKey key = PtGeometryLifecycle::MakeLightKey(light);
        const uint32_t lightGeneration = PtGeometryLifecycle::LightGeneration(light->world, light->index);
        DoomResidentLightRecord& resident = g_doomResidentLightCache.records[lightIndex];
        const bool cacheHit =
            resident.valid &&
            resident.key.world == key.world &&
            resident.key.index == key.index &&
            resident.key.generation == key.generation &&
            resident.lightGeneration == lightGeneration;
        if (!cacheHit)
        {
            needsMetadataMap = true;
            break;
        }
    }

    std::unordered_map<int, DoomLightGameMetadata> gameMetadataByHandle;
    if (needsMetadataMap)
    {
        gameMetadataByHandle = BuildDoomLightGameMetadataByHandle();
    }

    for (int lightIndex = 0; lightIndex < renderWorld->lightDefs.Num(); ++lightIndex)
    {
        const idRenderLightLocal* light = renderWorld->lightDefs[lightIndex];
        if (!light)
        {
            continue;
        }

        ++g_doomResidentLightCache.stats.visited;
        const PtRenderDefKey key = PtGeometryLifecycle::MakeLightKey(light);
        const uint32_t lightGeneration = PtGeometryLifecycle::LightGeneration(light->world, light->index);
        DoomResidentLightRecord& resident = g_doomResidentLightCache.records[lightIndex];
        const bool cacheHit =
            resident.valid &&
            resident.key.world == key.world &&
            resident.key.index == key.index &&
            resident.key.generation == key.generation &&
            resident.lightGeneration == lightGeneration;

        DoomLightRecord record;
        if (cacheHit)
        {
            ++g_doomResidentLightCache.stats.hits;
            RefreshDoomResidentLightDynamicFields(viewDef, light, selection, resident, record);
        }
        else
        {
            ++g_doomResidentLightCache.stats.derived;
            if (resident.valid)
            {
                ++g_doomResidentLightCache.stats.dirty;
            }
            else
            {
                ++g_doomResidentLightCache.stats.misses;
            }

            record = BuildDoomLightRecord(viewDef, light, selection, gameMetadataByHandle);
            resident.valid = true;
            resident.key = key;
            resident.lightGeneration = lightGeneration;
            resident.record = record;
            PopulateDoomResidentLightAreaFacts(viewDef, record, resident);
        }

        resident.lastSeenFrame = tr.frameCount;
        records.push_back(record);
    }

    for (const DoomResidentLightRecord& resident : g_doomResidentLightCache.records)
    {
        g_doomResidentLightCache.stats.residents += resident.valid ? 1 : 0;
    }
    DumpDoomResidentLightStatsIfNeeded();
    return records;
}

void PrintDoomLightRecord(const char* prefix, int sampleIndex, const DoomLightRecord& light, const char* mapName)
{
    common->Printf("%s[%d]: map='%s' renderLight=%d linked=%d entity='%s' entNum=%d classname='%s' entityDef='%s' spawnTexture='%s' shader='%s' type=%s origin=(%.2f %.2f %.2f) radius=(%.2f %.2f %.2f) radiusMax=%.2f bounds=(%.2f %.2f %.2f)-(%.2f %.2f %.2f) color=(%.3f %.3f %.3f) intensity=%.3f gameColor=(%.3f %.3f %.3f) baseColor=(%.3f %.3f %.3f) level=%d/%d hidden=%d startOff=%d break=%d count=%d health=%d model='%s' bind='%s' target='%s' broken='%s' area=%d originArea=%d selectionArea=%d portalDepth=%d boundsAreas=%d selectedBounds=%d connectedBounds=%d selected=%d connected=%d active=%d suppressed=%d shadows=%d visible=%d distance=%.2f crosshairBehind=%d crosshairT=%.2f crosshairDist=%.2f\n",
        prefix,
        sampleIndex,
        mapName ? mapName : "<unknown>",
        light.index,
        light.gameLinked ? 1 : 0,
        light.entityName,
        light.entityNumber,
        light.entityClassname,
        light.entityDefName,
        light.spawnTexture,
        light.shaderName,
        light.parallel ? "parallel" : (light.pointLight ? "point" : "projected"),
        light.origin.x,
        light.origin.y,
        light.origin.z,
        light.radius.x,
        light.radius.y,
        light.radius.z,
        light.radiusMax,
        light.bounds[0].x,
        light.bounds[0].y,
        light.bounds[0].z,
        light.bounds[1].x,
        light.bounds[1].y,
        light.bounds[1].z,
        light.color.x,
        light.color.y,
        light.color.z,
        light.color.w,
        light.currentGameColor.x,
        light.currentGameColor.y,
        light.currentGameColor.z,
        light.baseColor.x,
        light.baseColor.y,
        light.baseColor.z,
        light.currentLevel,
        light.levels,
        light.gameHidden ? 1 : 0,
        light.spawnStartOff ? 1 : 0,
        light.spawnBreak ? 1 : 0,
        light.spawnCount,
        light.health,
        light.spawnModel,
        light.spawnBind,
        light.spawnTarget,
        light.spawnBroken,
        light.area,
        light.originArea,
        light.selectionArea,
        light.portalDepth,
        light.boundsAreaCount,
        light.selectedBoundsAreaCount,
        light.connectedBoundsAreaCount,
        light.selectedArea ? 1 : 0,
        light.connectedArea ? 1 : 0,
        light.active ? 1 : 0,
        light.suppressed ? 1 : 0,
        light.castsShadows ? 1 : 0,
        light.visibleInView ? 1 : 0,
        light.distance,
        light.crosshairBehind ? 1 : 0,
        light.crosshairT,
        light.crosshairDistance);
}

void RunDoomLightIdentityDump(const viewDef_t* viewDef, const DoomLightPortalSelection& selection, const std::vector<DoomLightRecord>& records)
{
    const int dumpMode = r_pathTracingDoomLightDump.GetInteger();
    if (dumpMode == 0)
    {
        return;
    }

    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    const char* mapName = renderWorld ? renderWorld->mapName.c_str() : "<unknown>";
    int active = 0;
    int selected = 0;
    int visible = 0;
    int point = 0;
    int projected = 0;
    int parallel = 0;
    int unknownArea = 0;
    int gameLinked = 0;
    int gameHidden = 0;
    int gameLevelOff = 0;
    int connectedUnselected = 0;
    int selectedViaBounds = 0;
    int boundsAreaRefs = 0;
    int selectedBoundsAreaRefs = 0;
    int depthBins[6] = {};

    for (const DoomLightRecord& record : records)
    {
        active += record.active ? 1 : 0;
        selected += record.selectedArea ? 1 : 0;
        visible += record.visibleInView ? 1 : 0;
        point += record.pointLight ? 1 : 0;
        projected += (!record.pointLight && !record.parallel) ? 1 : 0;
        parallel += record.parallel ? 1 : 0;
        unknownArea += record.area < 0 ? 1 : 0;
        gameLinked += record.gameLinked ? 1 : 0;
        gameHidden += record.gameHidden ? 1 : 0;
        gameLevelOff += record.gameLinked && record.currentLevel <= 0 ? 1 : 0;
        connectedUnselected += (record.connectedArea && !record.selectedArea) ? 1 : 0;
        selectedViaBounds += (record.selectedArea && record.selectionArea >= 0 && record.selectionArea != record.area) ? 1 : 0;
        boundsAreaRefs += record.boundsAreaCount;
        selectedBoundsAreaRefs += record.selectedBoundsAreaCount;
        if (record.portalDepth >= 0 && record.portalDepth < 5)
        {
            ++depthBins[record.portalDepth];
        }
        else
        {
            ++depthBins[5];
        }
    }

    common->Printf("PathTracePrimaryPass: Doom light dump map='%s' lights=%d active=%d visibleView=%d point/projected/parallel=%d/%d/%d gameLinked=%d hidden=%d levelOff=%d currentArea=%d portalSteps=%d selectedAreas=%d portalEdges/blocked=%d/%d selectedLights=%d selectedViaBounds=%d boundsAreaRefs=%d selectedBoundsAreaRefs=%d connectedUnselected=%d unknownArea=%d depthBins 0/1/2/3/4/other=%d/%d/%d/%d/%d/%d entityNameStatus=linked-by-idLight-lightDefHandle\n",
        mapName,
        static_cast<int>(records.size()),
        active,
        visible,
        point,
        projected,
        parallel,
        gameLinked,
        gameHidden,
        gameLevelOff,
        selection.currentArea,
        selection.portalSteps,
        selection.selectedAreaCount,
        selection.portalEdges,
        selection.blockedPortalEdges,
        selected,
        selectedViaBounds,
        boundsAreaRefs,
        selectedBoundsAreaRefs,
        connectedUnselected,
        unknownArea,
        depthBins[0],
        depthBins[1],
        depthBins[2],
        depthBins[3],
        depthBins[4],
        depthBins[5]);

    std::vector<DoomLightRecord> samples = records;
    std::stable_sort(samples.begin(), samples.end(), [](const DoomLightRecord& a, const DoomLightRecord& b) {
        if (a.selectedArea != b.selectedArea)
        {
            return a.selectedArea && !b.selectedArea;
        }
        if (a.active != b.active)
        {
            return a.active && !b.active;
        }
        if (a.portalDepth != b.portalDepth)
        {
            return a.portalDepth >= 0 && (b.portalDepth < 0 || a.portalDepth < b.portalDepth);
        }
        if (a.distance != b.distance)
        {
            return a.distance < b.distance;
        }
        return a.index < b.index;
    });

    const int maxSamples = idMath::ClampInt(0, 1024, r_pathTracingDoomLightDumpMax.GetInteger());
    const int printCount = dumpMode >= 2 ? Min(maxSamples, static_cast<int>(samples.size())) : Min(maxSamples, Min(16, static_cast<int>(samples.size())));
    for (int i = 0; i < printCount; ++i)
    {
        PrintDoomLightRecord("PathTracePrimaryPass: Doom light", i, samples[i], mapName);
    }

    r_pathTracingDoomLightDump.SetInteger(0);
}

void RunDoomLightProbeDump(const viewDef_t* viewDef, const std::vector<DoomLightRecord>& records)
{
    if (r_pathTracingDoomLightProbeDump.GetInteger() == 0)
    {
        return;
    }

    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    const char* mapName = renderWorld ? renderWorld->mapName.c_str() : "<unknown>";
    const int maxSamples = idMath::ClampInt(1, 64, r_pathTracingDoomLightProbeMax.GetInteger());

    std::vector<DoomLightRecord> nearest = records;
    std::stable_sort(nearest.begin(), nearest.end(), [](const DoomLightRecord& a, const DoomLightRecord& b) {
        if (a.distance != b.distance)
        {
            return a.distance < b.distance;
        }
        return a.index < b.index;
    });

    std::vector<DoomLightRecord> crosshair;
    crosshair.reserve(records.size());
    for (const DoomLightRecord& record : records)
    {
        if (record.crosshairT >= 0.0f)
        {
            crosshair.push_back(record);
        }
    }
    std::stable_sort(crosshair.begin(), crosshair.end(), [](const DoomLightRecord& a, const DoomLightRecord& b) {
        if (a.crosshairScore != b.crosshairScore)
        {
            return a.crosshairScore < b.crosshairScore;
        }
        if (a.crosshairT != b.crosshairT)
        {
            return a.crosshairT < b.crosshairT;
        }
        return a.index < b.index;
    });

    common->Printf("PathTracePrimaryPass: Doom light probe map='%s' playerOrigin=(%.2f %.2f %.2f) forward=(%.3f %.3f %.3f) nearest=%d crosshair=%d max=%d\n",
        mapName,
        viewDef->renderView.vieworg.x,
        viewDef->renderView.vieworg.y,
        viewDef->renderView.vieworg.z,
        viewDef->renderView.viewaxis[0].x,
        viewDef->renderView.viewaxis[0].y,
        viewDef->renderView.viewaxis[0].z,
        static_cast<int>(nearest.size()),
        static_cast<int>(crosshair.size()),
        maxSamples);

    for (int i = 0; i < Min(maxSamples, static_cast<int>(nearest.size())); ++i)
    {
        PrintDoomLightRecord("PathTracePrimaryPass: Doom nearest light", i, nearest[i], mapName);
    }
    for (int i = 0; i < Min(maxSamples, static_cast<int>(crosshair.size())); ++i)
    {
        PrintDoomLightRecord("PathTracePrimaryPass: Doom crosshair light", i, crosshair[i], mapName);
    }

    r_pathTracingDoomLightProbeDump.SetInteger(0);
}

bool DoomLightContinuityProvenForTask01(const DoomLightRecord& record);

std::vector<DoomLightRecord> BuildAnalyticDoomLightRecords(
    const std::vector<DoomLightRecord>& records,
    bool preserveZeroRadianceSlots,
    bool stableReservoirOrder,
    bool includeOutOfSelectedArea,
    bool requireProvenContinuity)
{
    std::vector<DoomLightRecord> candidates;
    candidates.reserve(records.size());
    const float radiusScale = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingAnalyticSphereLightRadiusScale.GetFloat());
    const float radiusMin = Max(0.0f, r_pathTracingAnalyticSphereLightRadiusMin.GetFloat());
    const float radiusMax = Max(radiusMin, r_pathTracingAnalyticSphereLightRadiusMax.GetFloat());
    for (DoomLightRecord record : records)
    {
        const bool areaEligible = includeOutOfSelectedArea || record.selectedArea;
        const bool continuityEligible = !requireProvenContinuity || DoomLightContinuityProvenForTask01(record);
        const bool eligibleLight = record.pointLight && !record.parallel && !record.suppressed && areaEligible && continuityEligible && record.radiusMax > 1.0f;
        if (!eligibleLight || (!preserveZeroRadianceSlots && !record.active))
        {
            continue;
        }
        record.sphereRadius = idMath::ClampFloat(radiusMin, radiusMax, record.radiusMax * radiusScale);
        candidates.push_back(record);
    }

    if (stableReservoirOrder)
    {
        std::stable_sort(candidates.begin(), candidates.end(), [](const DoomLightRecord& a, const DoomLightRecord& b) {
            if (a.index != b.index)
            {
                return a.index < b.index;
            }
            return a.entityNumber < b.entityNumber;
        });
        return candidates;
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const DoomLightRecord& a, const DoomLightRecord& b) {
        if (a.portalDepth != b.portalDepth)
        {
            return a.portalDepth < b.portalDepth;
        }
        if (a.distance != b.distance)
        {
            return a.distance < b.distance;
        }
        return a.index < b.index;
    });

    return candidates;
}

uint32_t DoomLightEntityNumberForUniverse(const DoomLightRecord& record)
{
    return record.entityNumber >= 0 ? static_cast<uint32_t>(record.entityNumber) : RT_PT_DOOM_LIGHT_INVALID_ENTITY_NUMBER;
}

DoomAnalyticLightUniverseKey MakeDoomAnalyticLightUniverseKey(const DoomLightRecord& record)
{
    DoomAnalyticLightUniverseKey key;
    key.renderLightIndex = record.index >= 0 ? static_cast<uint32_t>(record.index) : RT_PT_DOOM_LIGHT_INVALID_INDEX;
    key.entityNumber = DoomLightEntityNumberForUniverse(record);
    key.kind = DOOM_ANALYTIC_LIGHT_KIND_POINT;
    return key;
}

bool IsLikelyTemporaryDoomLightEntity(const DoomLightRecord& record)
{
    const char* text[] = {
        record.entityName,
        record.entityClassname,
        record.entityDefName,
        record.spawnModel,
        record.spawnTexture,
    };
    const char* tokens[] = {
        "projectile",
        "fireball",
        "rocket",
        "grenade",
        "muzzle",
        "flash",
        "explosion",
        "particle",
        "fx_",
    };
    for (const char* value : text)
    {
        if (!value)
        {
            continue;
        }
        for (const char* token : tokens)
        {
            if (idStr::FindText(value, token, false) >= 0)
            {
                return true;
            }
        }
    }
    return false;
}

bool DoomLightContinuityProvenForTask01(const DoomLightRecord& record)
{
    if (!record.gameLinked || record.entityNumber < 0)
    {
        return false;
    }
    if (IsLikelyTemporaryDoomLightEntity(record))
    {
        return false;
    }
    return true;
}

bool IsDoomAnalyticUniverseStructuralLight(const DoomLightRecord& record)
{
    return record.pointLight && !record.parallel && record.radiusMax > 1.0f;
}




uint32_t GetStableDoomAnalyticUniverseIndex(DoomAnalyticLightUniverseState& state, const DoomAnalyticLightUniverseKey& key)
{
    for (const DoomAnalyticLightStableKey& stableKey : state.stableKeys)
    {
        if (stableKey.key == key)
        {
            return stableKey.universeIndex;
        }
    }

    DoomAnalyticLightStableKey stableKey;
    stableKey.key = key;
    stableKey.universeIndex = state.nextUniverseIndex++;
    state.stableKeys.push_back(stableKey);
    return stableKey.universeIndex;
}


uint32_t BuildDoomAnalyticIdentityFlags(const DoomAnalyticLightUniverseEntry& entry)
{
    uint32_t flags = PATH_TRACE_DOOM_ANALYTIC_IDENTITY_VALID;
    if (entry.sampleable)
    {
        flags |= PATH_TRACE_DOOM_ANALYTIC_IDENTITY_SAMPLEABLE;
    }
    if (entry.remapValid)
    {
        flags |= PATH_TRACE_DOOM_ANALYTIC_IDENTITY_REMAP_VALID;
    }
    return flags;
}


bool DoomRawFrameKeyIsDuplicate(const std::vector<DoomLightRecord>& records, const DoomAnalyticLightUniverseKey& key)
{
    int keyCount = 0;
    for (const DoomLightRecord& record : records)
    {
        if (MakeDoomAnalyticLightUniverseKey(record) == key)
        {
            ++keyCount;
            if (keyCount > 1)
            {
                return true;
            }
        }
    }
    return false;
}

bool DoomPersistentAuthoredLightKeyReferenced(const std::vector<DoomPersistentAuthoredLightEntry>& entries, const DoomAnalyticLightUniverseKey& key)
{
    for (const DoomPersistentAuthoredLightEntry& entry : entries)
    {
        if (entry.key == key)
        {
            return true;
        }
    }
    return false;
}

void PruneDoomPersistentDynamicAuthoredLights(DoomAnalyticLightUniverseState& state)
{
    int removedCount = 0;
    state.persistentAuthoredLights.erase(
        std::remove_if(
            state.persistentAuthoredLights.begin(),
            state.persistentAuthoredLights.end(),
            [&](const DoomPersistentAuthoredLightEntry& entry) {
                const bool shouldRemove =
                    !entry.staticAuthored &&
                    !entry.currentSeen &&
                    entry.missingFrames > RT_PT_DOOM_DYNAMIC_LIGHT_PRUNE_MISSING_FRAMES;
                removedCount += shouldRemove ? 1 : 0;
                return shouldRemove;
            }),
        state.persistentAuthoredLights.end());

    if (removedCount <= 0)
    {
        return;
    }

    state.stableKeys.erase(
        std::remove_if(
            state.stableKeys.begin(),
            state.stableKeys.end(),
            [&](const DoomAnalyticLightStableKey& stableKey) {
                return !DoomPersistentAuthoredLightKeyReferenced(state.persistentAuthoredLights, stableKey.key);
            }),
        state.stableKeys.end());
}

void UpdateDoomPersistentAuthoredLightRegistry(
    DoomAnalyticLightUniverseState& state,
    const std::vector<DoomLightRecord>& records)
{
    OPTICK_EVENT("PT Doom Light Registry");
    int maxLightIndex = -1;
    for (const DoomPersistentAuthoredLightEntry& entry : state.persistentAuthoredLights)
    {
        if (entry.key.renderLightIndex != RT_PT_DOOM_LIGHT_INVALID_INDEX)
        {
            maxLightIndex = Max(maxLightIndex, static_cast<int>(entry.key.renderLightIndex));
        }
    }
    for (const DoomLightRecord& record : records)
    {
        maxLightIndex = Max(maxLightIndex, record.index);
    }

    std::vector<int> persistentEntryByLightIndex;
    if (maxLightIndex >= 0)
    {
        persistentEntryByLightIndex.assign(static_cast<size_t>(maxLightIndex + 1), -1);
    }
    for (int entryIndex = 0; entryIndex < static_cast<int>(state.persistentAuthoredLights.size()); ++entryIndex)
    {
        const DoomAnalyticLightUniverseKey& key = state.persistentAuthoredLights[entryIndex].key;
        if (key.renderLightIndex != RT_PT_DOOM_LIGHT_INVALID_INDEX &&
            key.renderLightIndex < static_cast<uint32_t>(persistentEntryByLightIndex.size()))
        {
            // Newer entries are appended later; let them win after render-light slot reuse.
            persistentEntryByLightIndex[key.renderLightIndex] = entryIndex;
        }
    }

    for (DoomPersistentAuthoredLightEntry& entry : state.persistentAuthoredLights)
    {
        entry.currentSeen = false;
        entry.currentActive = false;
        entry.currentSampleable = false;
        entry.currentSelectedArea = false;
        entry.currentConnectedArea = false;
        entry.currentSuppressed = false;
        entry.currentContinuityProven = false;
        entry.currentDuplicateKey = false;
        entry.currentPointLight = false;
        entry.currentParallel = false;
        entry.currentCastsShadows = false;
        entry.currentGameLinked = false;
        entry.currentCrosshairBehind = false;
        entry.currentCandidateIndex = RT_PT_DOOM_LIGHT_INVALID_INDEX;
        entry.invalidReasonFlags = 0;
    }

    for (const DoomLightRecord& record : records)
    {
        const DoomAnalyticLightUniverseKey key = MakeDoomAnalyticLightUniverseKey(record);
        const bool duplicateKeyInRawFrame =
            key.renderLightIndex == RT_PT_DOOM_LIGHT_INVALID_INDEX &&
            DoomRawFrameKeyIsDuplicate(records, key);
        const bool continuityProven = DoomLightContinuityProvenForTask01(record);
        const bool structuralLight = IsDoomAnalyticUniverseStructuralLight(record);
        const bool staticAuthored = continuityProven && structuralLight;
        int entryIndex = -1;
        if (key.renderLightIndex != RT_PT_DOOM_LIGHT_INVALID_INDEX &&
            key.renderLightIndex < static_cast<uint32_t>(persistentEntryByLightIndex.size()))
        {
            const int candidateEntryIndex = persistentEntryByLightIndex[key.renderLightIndex];
            if (candidateEntryIndex >= 0 &&
                candidateEntryIndex < static_cast<int>(state.persistentAuthoredLights.size()) &&
                state.persistentAuthoredLights[candidateEntryIndex].key == key)
            {
                entryIndex = candidateEntryIndex;
            }
        }
        const bool isNewEntry = entryIndex < 0;
        if (isNewEntry)
        {
            DoomPersistentAuthoredLightEntry entry;
            entry.key = key;
            entry.universeIndex = GetStableDoomAnalyticUniverseIndex(state, key);
            entry.staticAuthored = staticAuthored;
            state.persistentAuthoredLights.push_back(entry);
            entryIndex = static_cast<int>(state.persistentAuthoredLights.size()) - 1;
            if (key.renderLightIndex != RT_PT_DOOM_LIGHT_INVALID_INDEX &&
                key.renderLightIndex < static_cast<uint32_t>(persistentEntryByLightIndex.size()))
            {
                persistentEntryByLightIndex[key.renderLightIndex] = entryIndex;
            }
        }

        DoomPersistentAuthoredLightEntry& entry = state.persistentAuthoredLights[entryIndex];
        entry.staticAuthored = entry.staticAuthored || staticAuthored;
        entry.currentSeen = true;
        entry.currentActive = record.active;
        entry.currentSampleable = structuralLight && !record.suppressed && record.active;
        entry.currentSelectedArea = record.selectedArea;
        entry.currentConnectedArea = record.connectedArea;
        entry.currentSuppressed = record.suppressed;
        entry.currentContinuityProven = continuityProven;
        entry.currentDuplicateKey = entry.currentDuplicateKey || duplicateKeyInRawFrame;
        entry.currentPointLight = record.pointLight;
        entry.currentParallel = record.parallel;
        entry.currentCastsShadows = record.castsShadows;
        entry.currentGameLinked = record.gameLinked;
        entry.currentCrosshairBehind = record.crosshairBehind;
        entry.currentOrigin = record.origin;
        entry.currentColor = record.color;
        entry.currentDoomRadius = record.radiusMax;
        entry.currentSphereRadius = record.sphereRadius;
        entry.currentSelectionArea = record.selectionArea;
        entry.currentPortalDepth = record.portalDepth;

        if (!record.active || record.color.w <= 0.0f)
        {
            entry.invalidReasonFlags |= DOOM_LIGHT_UNIVERSE_INVALID_ZERO_RADIANCE;
        }
        if (record.suppressed)
        {
            entry.invalidReasonFlags |= DOOM_LIGHT_UNIVERSE_INVALID_SUPPRESSED;
        }
        if (!record.selectedArea)
        {
            entry.invalidReasonFlags |= DOOM_LIGHT_UNIVERSE_INVALID_OUT_OF_SELECTED_AREA;
            if (!record.connectedArea)
            {
                entry.invalidReasonFlags |= DOOM_LIGHT_UNIVERSE_INVALID_DISCONNECTED_OR_PORTAL;
            }
        }
        if (key.entityNumber == RT_PT_DOOM_LIGHT_INVALID_ENTITY_NUMBER)
        {
            entry.invalidReasonFlags |= DOOM_LIGHT_UNIVERSE_INVALID_UNKNOWN_ENTITY;
        }
        if (!entry.currentContinuityProven)
        {
            entry.invalidReasonFlags |= DOOM_LIGHT_UNIVERSE_INVALID_UNPROVEN_CONTINUITY;
        }
        if (entry.currentDuplicateKey)
        {
            entry.invalidReasonFlags |= DOOM_LIGHT_UNIVERSE_INVALID_DUPLICATE_KEY;
        }
    }

    for (DoomPersistentAuthoredLightEntry& entry : state.persistentAuthoredLights)
    {
        if (!entry.currentSeen)
        {
            entry.missingFrames += 1;
            entry.invalidReasonFlags |= DOOM_LIGHT_UNIVERSE_INVALID_MISSING_CURRENT;
        }
    }
    PruneDoomPersistentDynamicAuthoredLights(state);
}

void BuildDoomPersistentAuthoredProposalList(
    DoomAnalyticLightUniverseState& state,
    const std::vector<DoomLightRecord>& candidates)
{
    OPTICK_EVENT("PT Doom Light Proposals");
    state.persistentAuthoredProposals.clear();
    state.persistentAuthoredProposals.reserve(candidates.size());

    std::unordered_map<DoomAnalyticLightUniverseKey, int, DoomAnalyticLightUniverseKeyHash> persistentEntryByKey;
    persistentEntryByKey.reserve(state.persistentAuthoredLights.size());
    for (int entryIndex = 0; entryIndex < static_cast<int>(state.persistentAuthoredLights.size()); ++entryIndex)
    {
        persistentEntryByKey.emplace(state.persistentAuthoredLights[entryIndex].key, entryIndex);
    }

    for (int candidateIndex = 0; candidateIndex < static_cast<int>(candidates.size()); ++candidateIndex)
    {
        const DoomLightRecord& candidate = candidates[candidateIndex];
        const DoomAnalyticLightUniverseKey key = MakeDoomAnalyticLightUniverseKey(candidate);
        const auto entryIt = persistentEntryByKey.find(key);
        if (entryIt == persistentEntryByKey.end())
        {
            continue;
        }

        DoomPersistentAuthoredLightEntry& entry = state.persistentAuthoredLights[entryIt->second];
        entry.currentCandidateIndex = static_cast<uint32_t>(candidateIndex);

        DoomPersistentAuthoredLightProposal proposal;
        proposal.universeIndex = entry.universeIndex;
        proposal.candidateIndex = static_cast<uint32_t>(candidateIndex);
        state.persistentAuthoredProposals.push_back(proposal);

    }
}

DoomAnalyticLightUniverseEntry MakeDoomAnalyticLightUniverseEntryFromPersistent(
    const DoomPersistentAuthoredLightEntry& persistentEntry,
    const std::vector<DoomAnalyticLightUniverseEntry>& previousEntries,
    const std::unordered_map<DoomAnalyticLightUniverseKey, int, DoomAnalyticLightUniverseKeyHash>& previousEntryByKey,
    int maxGpuCandidates)
{
    DoomAnalyticLightUniverseEntry entry;
    entry.key = persistentEntry.key;
    entry.universeIndex = persistentEntry.universeIndex;
    entry.currentCandidateIndex = persistentEntry.currentCandidateIndex;
    entry.duplicateKey = persistentEntry.currentDuplicateKey;
    entry.active = persistentEntry.currentSeen && persistentEntry.currentActive;
    entry.sampleable = persistentEntry.currentSeen && persistentEntry.currentSampleable;
    entry.continuityProven = persistentEntry.staticAuthored && persistentEntry.currentContinuityProven;
    entry.flags = 0;
    if (persistentEntry.currentCastsShadows)
    {
        entry.flags |= RT_DOOM_ANALYTIC_LIGHT_CASTS_SHADOWS;
    }
    if (persistentEntry.currentGameLinked)
    {
        entry.flags |= RT_DOOM_ANALYTIC_LIGHT_GAME_LINKED;
    }
    if (persistentEntry.currentCrosshairBehind)
    {
        entry.flags |= RT_DOOM_ANALYTIC_LIGHT_BEHIND_CAMERA;
    }
    entry.origin = persistentEntry.currentOrigin;
    entry.color = persistentEntry.currentSeen ? persistentEntry.currentColor : idVec4(0.0f, 0.0f, 0.0f, 0.0f);
    entry.doomRadius = persistentEntry.currentDoomRadius;
    entry.sphereRadius = persistentEntry.currentSphereRadius;
    entry.selectionArea = persistentEntry.currentSelectionArea;
    entry.portalDepth = persistentEntry.currentPortalDepth;

    if (entry.active)
    {
        entry.temporalStateFlags |= DOOM_LIGHT_UNIVERSE_STATE_ACTIVE;
    }
    if (entry.sampleable)
    {
        entry.temporalStateFlags |= DOOM_LIGHT_UNIVERSE_STATE_SAMPLEABLE;
    }
    if (persistentEntry.currentSelectedArea)
    {
        entry.temporalStateFlags |= DOOM_LIGHT_UNIVERSE_STATE_SELECTED_AREA;
    }
    if (persistentEntry.currentSuppressed)
    {
        entry.temporalStateFlags |= DOOM_LIGHT_UNIVERSE_STATE_SUPPRESSED;
    }
    if (persistentEntry.currentGameLinked)
    {
        entry.temporalStateFlags |= DOOM_LIGHT_UNIVERSE_STATE_GAME_LINKED;
    }
    if (entry.continuityProven)
    {
        entry.temporalStateFlags |= DOOM_LIGHT_UNIVERSE_STATE_CONTINUITY_PROVEN;
    }

    if (persistentEntry.currentSeen && (!persistentEntry.currentPointLight || persistentEntry.currentParallel))
    {
        entry.invalidReasonFlags |= DOOM_LIGHT_UNIVERSE_INVALID_NON_POINT_OR_PARALLEL;
    }
    if (persistentEntry.currentSeen && persistentEntry.currentDoomRadius <= 1.0f)
    {
        entry.invalidReasonFlags |= DOOM_LIGHT_UNIVERSE_INVALID_RADIUS_INVALID;
    }
    entry.invalidReasonFlags |= persistentEntry.invalidReasonFlags;
    if (entry.currentCandidateIndex != RT_PT_DOOM_LIGHT_INVALID_INDEX && entry.currentCandidateIndex >= static_cast<uint32_t>(Max(maxGpuCandidates, 0)))
    {
        entry.invalidReasonFlags |= DOOM_LIGHT_UNIVERSE_INVALID_CANDIDATE_CAP_DROPPED;
    }

    const auto previousEntryIt = previousEntryByKey.find(entry.key);
    if (previousEntryIt != previousEntryByKey.end())
    {
        entry.hasPrevious = true;
        entry.previousUniverseIndex = previousEntries[previousEntryIt->second].universeIndex;
    }
    else
    {
        entry.invalidReasonFlags |= DOOM_LIGHT_UNIVERSE_INVALID_MISSING_PREVIOUS;
    }
    return entry;
}

void UpdateDoomAnalyticLightUniverse(
    const viewDef_t* viewDef,
    const std::vector<DoomLightRecord>& records,
    const std::vector<DoomLightRecord>& candidates,
    int maxGpuCandidates)
{
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    const char* mapName = renderWorld ? renderWorld->mapName.c_str() : "";
    const ID_TIME_T mapTimeStamp = renderWorld ? renderWorld->mapTimeStamp : 0;
    if (g_doomAnalyticLightUniverse.renderWorld != renderWorld ||
        g_doomAnalyticLightUniverse.mapName.Icmp(mapName) != 0 ||
        g_doomAnalyticLightUniverse.mapTimeStamp != mapTimeStamp)
    {
        g_doomAnalyticLightUniverse.Reset(renderWorld, mapName, mapTimeStamp);
    }

    DoomAnalyticLightUniverseState& state = g_doomAnalyticLightUniverse;
    std::vector<DoomAnalyticLightUniverseEntry> previousEntries = state.currentEntries;
    state.previousEntries = previousEntries;
    state.currentEntries.clear();
    std::unordered_map<DoomAnalyticLightUniverseKey, int, DoomAnalyticLightUniverseKeyHash> previousEntryByKey;
    previousEntryByKey.reserve(previousEntries.size());
    std::unordered_map<DoomAnalyticLightUniverseKey, int, DoomAnalyticLightUniverseKeyHash> previousKeyCounts;
    previousKeyCounts.reserve(previousEntries.size());
    for (int previousIndex = 0; previousIndex < static_cast<int>(previousEntries.size()); ++previousIndex)
    {
        previousEntryByKey.emplace(previousEntries[previousIndex].key, previousIndex);
        ++previousKeyCounts[previousEntries[previousIndex].key];
    }
    {
        OPTICK_EVENT("PT Doom Light Universe Registry");
        UpdateDoomPersistentAuthoredLightRegistry(state, records);
    }
    {
        OPTICK_EVENT("PT Doom Light Universe Proposals");
        BuildDoomPersistentAuthoredProposalList(state, candidates);
    }
    state.currentEntries.reserve(state.persistentAuthoredLights.size());

    {
        OPTICK_EVENT("PT Doom Light Universe Entries");
        for (const DoomPersistentAuthoredLightEntry& persistentEntry : state.persistentAuthoredLights)
        {
            if (!persistentEntry.currentSeen && !persistentEntry.staticAuthored)
            {
                continue;
            }

            state.currentEntries.push_back(MakeDoomAnalyticLightUniverseEntryFromPersistent(persistentEntry, previousEntries, previousEntryByKey, maxGpuCandidates));
        }
    }

    std::unordered_map<DoomAnalyticLightUniverseKey, int, DoomAnalyticLightUniverseKeyHash> currentKeyCounts;
    currentKeyCounts.reserve(state.currentEntries.size());
    for (int currentIndex = 0; currentIndex < static_cast<int>(state.currentEntries.size()); ++currentIndex)
    {
        ++currentKeyCounts[state.currentEntries[currentIndex].key];
    }

    {
        OPTICK_EVENT("PT Doom Light Universe Remap Flags");
        for (DoomAnalyticLightUniverseEntry& entry : state.currentEntries)
        {
            const auto previousEntryIt = previousEntryByKey.find(entry.key);
            const bool previousDuplicateKey = previousEntryIt != previousEntryByKey.end() && previousEntries[previousEntryIt->second].duplicateKey;
            const auto currentCountIt = currentKeyCounts.find(entry.key);
            const auto previousCountIt = previousKeyCounts.find(entry.key);
            entry.duplicateKey = entry.duplicateKey ||
                previousDuplicateKey ||
                (currentCountIt != currentKeyCounts.end() && currentCountIt->second > 1) ||
                (previousCountIt != previousKeyCounts.end() && previousCountIt->second > 1);
            if (entry.duplicateKey)
            {
                entry.invalidReasonFlags |= DOOM_LIGHT_UNIVERSE_INVALID_DUPLICATE_KEY;
            }
            entry.remapValid = entry.hasPrevious && !entry.duplicateKey && entry.continuityProven;
        }
    }

}

const DoomAnalyticLightUniverseEntry* FindDoomAnalyticEntryByUniverseIndex(
    const std::vector<DoomAnalyticLightUniverseEntry>& entries,
    uint32_t universeIndex)
{
    for (const DoomAnalyticLightUniverseEntry& entry : entries)
    {
        if (entry.universeIndex == universeIndex)
        {
            return &entry;
        }
    }
    return nullptr;
}

PathTraceDoomAnalyticLightCandidateIdentity MakeInvalidDoomAnalyticCandidateIdentity()
{
    PathTraceDoomAnalyticLightCandidateIdentity identity;
    identity.universeIndex = PATH_TRACE_DOOM_ANALYTIC_LIGHT_INVALID_INDEX;
    identity.remapIndex = PATH_TRACE_DOOM_ANALYTIC_LIGHT_INVALID_INDEX;
    return identity;
}



PathTraceDoomAnalyticLightCandidate MakeDoomAnalyticLightCandidateFromUniverseEntry(const DoomAnalyticLightUniverseEntry& entry)
{
    PathTraceDoomAnalyticLightCandidate gpuLight = {};
    const float sphereRadius = FinitePositiveOrZero(entry.sphereRadius);
    const float doomRadius = FinitePositiveOrZero(entry.doomRadius);
    gpuLight.originAndRadius[0] = entry.origin.x;
    gpuLight.originAndRadius[1] = entry.origin.y;
    gpuLight.originAndRadius[2] = entry.origin.z;
    gpuLight.originAndRadius[3] = sphereRadius;
    gpuLight.colorAndIntensity[0] = Max(entry.color.x, 0.0f);
    gpuLight.colorAndIntensity[1] = Max(entry.color.y, 0.0f);
    gpuLight.colorAndIntensity[2] = Max(entry.color.z, 0.0f);
    gpuLight.colorAndIntensity[3] = Max(entry.color.w, 0.0f);
    gpuLight.doomRadiusAndArea[0] = doomRadius;
    gpuLight.doomRadiusAndArea[1] = 12.56637061f * sphereRadius * sphereRadius;
    gpuLight.doomRadiusAndArea[2] = static_cast<float>(entry.portalDepth);
    gpuLight.doomRadiusAndArea[3] = static_cast<float>(entry.selectionArea);
    gpuLight.flags = entry.flags;
    gpuLight.renderLightIndex = entry.key.renderLightIndex;
    gpuLight.entityNumber = entry.key.entityNumber;
    return gpuLight;
}

void BuildDoomAnalyticLightGpuRemap(DoomAnalyticLightUniverseState& state, int uploadedCandidateCount)
{
    OPTICK_EVENT("PT Doom Light GPU Remap");
    PathTraceDoomAnalyticLightGpuRemap gpuRemap;
    const int currentIdentityCount = Max(uploadedCandidateCount, 0);
    gpuRemap.currentCandidateIdentities.assign(currentIdentityCount, MakeInvalidDoomAnalyticCandidateIdentity());

    int previousIdentityCount = 0;
    for (const DoomAnalyticLightUniverseEntry& previousEntry : state.previousEntries)
    {
        if (previousEntry.currentCandidateIndex != RT_PT_DOOM_LIGHT_INVALID_INDEX)
        {
            previousIdentityCount = Max(previousIdentityCount, static_cast<int>(previousEntry.currentCandidateIndex) + 1);
        }
    }
    gpuRemap.previousCandidates.assign(previousIdentityCount, PathTraceDoomAnalyticLightCandidate());
    gpuRemap.previousCandidateIdentities.assign(previousIdentityCount, MakeInvalidDoomAnalyticCandidateIdentity());

    std::vector<uint32_t> remapUniverseIndices;
    remapUniverseIndices.reserve(state.currentEntries.size() + state.previousEntries.size());
    std::unordered_map<uint32_t, uint32_t> remapSlotByUniverse;
    remapSlotByUniverse.reserve(state.currentEntries.size() + state.previousEntries.size());
    auto findOrAddRemapSlot = [&](uint32_t universeIndex) -> uint32_t {
        if (universeIndex == RT_PT_DOOM_LIGHT_INVALID_INDEX)
        {
            return RT_PT_DOOM_LIGHT_INVALID_INDEX;
        }
        const auto slotIt = remapSlotByUniverse.find(universeIndex);
        if (slotIt != remapSlotByUniverse.end())
        {
            return slotIt->second;
        }
        const uint32_t slot = static_cast<uint32_t>(remapUniverseIndices.size());
        remapUniverseIndices.push_back(universeIndex);
        remapSlotByUniverse.emplace(universeIndex, slot);
        return slot;
    };
    auto findRemapSlot = [&](uint32_t universeIndex) -> uint32_t {
        if (universeIndex == RT_PT_DOOM_LIGHT_INVALID_INDEX)
        {
            return RT_PT_DOOM_LIGHT_INVALID_INDEX;
        }
        const auto slotIt = remapSlotByUniverse.find(universeIndex);
        return slotIt != remapSlotByUniverse.end() ? slotIt->second : RT_PT_DOOM_LIGHT_INVALID_INDEX;
    };
    for (const DoomPersistentAuthoredLightProposal& proposal : state.persistentAuthoredProposals)
    {
        if (proposal.candidateIndex < static_cast<uint32_t>(currentIdentityCount))
        {
            findOrAddRemapSlot(proposal.universeIndex);
        }
    }
    for (const DoomAnalyticLightUniverseEntry& previousEntry : state.previousEntries)
    {
        if (previousEntry.currentCandidateIndex != RT_PT_DOOM_LIGHT_INVALID_INDEX &&
            previousEntry.currentCandidateIndex < static_cast<uint32_t>(previousIdentityCount) &&
            previousEntry.universeIndex != RT_PT_DOOM_LIGHT_INVALID_INDEX)
        {
            findOrAddRemapSlot(previousEntry.universeIndex);
        }
    }
    gpuRemap.universeRemap.assign(remapUniverseIndices.size(), PathTraceDoomAnalyticLightRemap());

    std::unordered_map<uint32_t, const DoomAnalyticLightUniverseEntry*> currentEntryByUniverse;
    currentEntryByUniverse.reserve(state.currentEntries.size());
    for (const DoomAnalyticLightUniverseEntry& entry : state.currentEntries)
    {
        currentEntryByUniverse.emplace(entry.universeIndex, &entry);
    }
    std::unordered_map<uint32_t, const DoomAnalyticLightUniverseEntry*> previousEntryByUniverse;
    previousEntryByUniverse.reserve(state.previousEntries.size());
    for (const DoomAnalyticLightUniverseEntry& entry : state.previousEntries)
    {
        previousEntryByUniverse.emplace(entry.universeIndex, &entry);
    }

    for (const DoomPersistentAuthoredLightProposal& proposal : state.persistentAuthoredProposals)
    {
        if (proposal.candidateIndex >= static_cast<uint32_t>(currentIdentityCount))
        {
            continue;
        }

        const auto currentEntryIt = currentEntryByUniverse.find(proposal.universeIndex);
        const DoomAnalyticLightUniverseEntry* currentEntry = currentEntryIt != currentEntryByUniverse.end() ? currentEntryIt->second : nullptr;
        PathTraceDoomAnalyticLightCandidateIdentity& identity = gpuRemap.currentCandidateIdentities[proposal.candidateIndex];
        identity.universeIndex = proposal.universeIndex;
        identity.remapIndex = findRemapSlot(proposal.universeIndex);
        if (currentEntry)
        {
            identity.flags = BuildDoomAnalyticIdentityFlags(*currentEntry);
            identity.invalidReasonFlags = currentEntry->invalidReasonFlags;
        }
        else
        {
            identity.invalidReasonFlags = DOOM_LIGHT_UNIVERSE_INVALID_MISSING_CURRENT;
        }
    }

    for (const DoomAnalyticLightUniverseEntry& previousEntry : state.previousEntries)
    {
        if (previousEntry.currentCandidateIndex == RT_PT_DOOM_LIGHT_INVALID_INDEX ||
            previousEntry.currentCandidateIndex >= static_cast<uint32_t>(previousIdentityCount))
        {
            continue;
        }

        PathTraceDoomAnalyticLightCandidateIdentity& identity = gpuRemap.previousCandidateIdentities[previousEntry.currentCandidateIndex];
        identity.universeIndex = previousEntry.universeIndex;
        identity.remapIndex = findRemapSlot(previousEntry.universeIndex);
        identity.flags = BuildDoomAnalyticIdentityFlags(previousEntry);
        identity.invalidReasonFlags = previousEntry.invalidReasonFlags;
        gpuRemap.previousCandidates[previousEntry.currentCandidateIndex] = MakeDoomAnalyticLightCandidateFromUniverseEntry(previousEntry);
    }

    for (int remapSlot = 0; remapSlot < static_cast<int>(remapUniverseIndices.size()); ++remapSlot)
    {
        const uint32_t universeIndex = remapUniverseIndices[remapSlot];
        const auto currentEntryIt = currentEntryByUniverse.find(universeIndex);
        const auto previousEntryIt = previousEntryByUniverse.find(universeIndex);
        const DoomAnalyticLightUniverseEntry* currentEntry = currentEntryIt != currentEntryByUniverse.end() ? currentEntryIt->second : nullptr;
        const DoomAnalyticLightUniverseEntry* previousEntry = previousEntryIt != previousEntryByUniverse.end() ? previousEntryIt->second : nullptr;
        if (!currentEntry && !previousEntry)
        {
            ++gpuRemap.invalidRemapCount;
            continue;
        }

        PathTraceDoomAnalyticLightRemap& remap = gpuRemap.universeRemap[remapSlot];
        const DoomAnalyticLightUniverseEntry& remapSource = currentEntry ? *currentEntry : *previousEntry;
        remap.flags = BuildDoomAnalyticIdentityFlags(remapSource);
        remap.invalidReasonFlags = remapSource.invalidReasonFlags;

        const bool currentCandidateValid =
            currentEntry &&
            currentEntry->currentCandidateIndex != RT_PT_DOOM_LIGHT_INVALID_INDEX &&
            currentEntry->currentCandidateIndex < static_cast<uint32_t>(currentIdentityCount);
        const bool previousCandidateValid =
            previousEntry &&
            previousEntry->currentCandidateIndex != RT_PT_DOOM_LIGHT_INVALID_INDEX &&
            previousEntry->currentCandidateIndex < static_cast<uint32_t>(previousIdentityCount);

        if (currentEntry && currentEntry->remapValid && currentCandidateValid)
        {
            remap.previousToCurrentCandidateIndex = static_cast<int32_t>(currentEntry->currentCandidateIndex);
        }
        if (currentEntry && currentEntry->remapValid && previousCandidateValid)
        {
            remap.currentToPreviousCandidateIndex = static_cast<int32_t>(previousEntry->currentCandidateIndex);
        }
        if (remap.previousToCurrentCandidateIndex < 0 || remap.currentToPreviousCandidateIndex < 0)
        {
            ++gpuRemap.invalidRemapCount;
        }
    }

    g_doomAnalyticLightGpuRemap = gpuRemap;
}

void RunAnalyticLightCandidateDump(const DoomLightPortalSelection& selection, const std::vector<DoomLightRecord>& records)
{
    if (r_pathTracingAnalyticLightCandidates.GetInteger() == 0)
    {
        if (r_pathTracingAnalyticLightCandidateDump.GetInteger() != 0)
        {
            common->Printf("PathTracePrimaryPass: Doom analytic light candidates disabled; set r_pathTracingAnalyticLightCandidates 1 to build and shade analytic sphere records\n");
            r_pathTracingAnalyticLightCandidateDump.SetInteger(0);
        }
        return;
    }

    std::vector<DoomLightRecord> candidates = BuildAnalyticDoomLightRecords(records, false, false, false, false);

    if (r_pathTracingAnalyticLightCandidateDump.GetInteger() == 0)
    {
        return;
    }

    const float radiusScale = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingAnalyticSphereLightRadiusScale.GetFloat());
    const float radiusMin = Max(0.0f, r_pathTracingAnalyticSphereLightRadiusMin.GetFloat());
    const float radiusMax = Max(radiusMin, r_pathTracingAnalyticSphereLightRadiusMax.GetFloat());

    int linkedCandidates = 0;
    for (const DoomLightRecord& candidate : candidates)
    {
        linkedCandidates += candidate.gameLinked ? 1 : 0;
    }

    int behindCandidates = 0;
    for (const DoomLightRecord& candidate : candidates)
    {
        behindCandidates += candidate.crosshairBehind ? 1 : 0;
    }

    const int gpuMax = idMath::ClampInt(0, 1024, r_pathTracingAnalyticLightMaxGpu.GetInteger());
    const int uploadedCandidates = Min(gpuMax, static_cast<int>(candidates.size()));
    const int droppedCandidates = static_cast<int>(candidates.size()) - uploadedCandidates;

    common->Printf("PathTracePrimaryPass: Doom analytic sphere-light candidates count=%d uploaded=%d droppedByGpuCap=%d linked=%d behindCamera=%d sourceLights=%d currentArea=%d portalSteps=%d gpuMax=%d intensityScale=%.3f radius scale/min/max=%.4f/%.2f/%.2f selection=multi-area-bounds-active-point-not-view-facing outputChange=1 gpuBuffer=separate\n",
        static_cast<int>(candidates.size()),
        uploadedCandidates,
        droppedCandidates,
        linkedCandidates,
        behindCandidates,
        static_cast<int>(records.size()),
        selection.currentArea,
        selection.portalSteps,
        gpuMax,
        idMath::ClampFloat(0.0f, 16.0f, r_pathTracingAnalyticLightIntensityScale.GetFloat()),
        radiusScale,
        radiusMin,
        radiusMax);

    const int maxSamples = idMath::ClampInt(0, 1024, r_pathTracingDoomLightDumpMax.GetInteger());
    for (int i = 0; i < Min(maxSamples, static_cast<int>(candidates.size())); ++i)
    {
        const DoomLightRecord& light = candidates[i];
        common->Printf("PathTracePrimaryPass: Doom analytic sphere candidate[%d] renderLight=%d linked=%d entity='%s' entNum=%d classname='%s' spawnTexture='%s' shader='%s' level=%d/%d hidden=%d behindCamera=%d gameColor=(%.3f %.3f %.3f) baseColor=(%.3f %.3f %.3f) area=%d selectionArea=%d portalDepth=%d boundsAreas=%d selectedBounds=%d origin=(%.2f %.2f %.2f) doomRadius=%.2f sphereRadius=%.2f color=(%.3f %.3f %.3f) intensity=%.3f\n",
            i,
            light.index,
            light.gameLinked ? 1 : 0,
            light.entityName,
            light.entityNumber,
            light.entityClassname,
            light.spawnTexture,
            light.shaderName,
            light.currentLevel,
            light.levels,
            light.gameHidden ? 1 : 0,
            light.crosshairBehind ? 1 : 0,
            light.currentGameColor.x,
            light.currentGameColor.y,
            light.currentGameColor.z,
            light.baseColor.x,
            light.baseColor.y,
            light.baseColor.z,
            light.area,
            light.selectionArea,
            light.portalDepth,
            light.boundsAreaCount,
            light.selectedBoundsAreaCount,
            light.origin.x,
            light.origin.y,
            light.origin.z,
            light.radiusMax,
            light.sphereRadius,
            light.color.x,
            light.color.y,
            light.color.z,
            light.color.w);
    }

    r_pathTracingAnalyticLightCandidateDump.SetInteger(0);
}

}

std::vector<PathTraceDoomAnalyticLightCandidate> BuildPathTraceDoomAnalyticLightCandidates(const viewDef_t* viewDef, bool forceEnable)
{
    PathTraceDoomAnalyticLightBuildOptions options;
    options.forceBuild = forceEnable;
    options.preserveZeroRadianceSlots = forceEnable;
    options.stableReservoirOrder = forceEnable;
    options.includeOutOfSelectedArea = forceEnable;
    options.ignoreConfiguredCandidateCap = forceEnable;
    return BuildPathTraceDoomAnalyticLightCandidates(viewDef, options);
}

std::vector<PathTraceDoomAnalyticLightCandidate> BuildPathTraceDoomAnalyticLightCandidates(const viewDef_t* viewDef, const PathTraceDoomAnalyticLightBuildOptions& options)
{
    std::vector<PathTraceDoomAnalyticLightCandidate> gpuCandidates;
    if (!viewDef || !viewDef->renderWorld || !IsDoomLightGameStateActive() || (!options.forceBuild && r_pathTracingAnalyticLightCandidates.GetInteger() == 0))
    {
        g_doomAnalyticLightGpuRemap = PathTraceDoomAnalyticLightGpuRemap();
        return gpuCandidates;
    }

    const DoomLightPortalSelection selection = [&]() {
        OPTICK_EVENT("PT Doom Light Portal Selection");
        return BuildDoomLightPortalSelection(
            viewDef,
            idMath::ClampInt(0, 8, r_pathTracingLightAreaPortalSteps.GetInteger()));
    }();
    const std::vector<DoomLightRecord> records = [&]() {
        OPTICK_EVENT("PT Doom Light Collect Records");
        return DoomLightResidencyEnabled()
            ? CollectDoomLightRecordsResident(viewDef, selection)
            : CollectDoomLightRecords(viewDef, selection, BuildDoomLightGameMetadataByHandle());
    }();
    const std::vector<DoomLightRecord> candidates = [&]() {
        OPTICK_EVENT("PT Doom Light Build Candidates");
        return BuildAnalyticDoomLightRecords(
            records,
            options.preserveZeroRadianceSlots,
            options.stableReservoirOrder,
            options.includeOutOfSelectedArea,
            options.requireProvenContinuity);
    }();
    const int configuredMaxGpuCandidates = idMath::ClampInt(0, 1024, r_pathTracingAnalyticLightMaxGpu.GetInteger());
    const int maxGpuCandidates = options.ignoreConfiguredCandidateCap ? static_cast<int>(candidates.size()) : configuredMaxGpuCandidates;
    {
        OPTICK_EVENT("PT Doom Light Universe Update");
        UpdateDoomAnalyticLightUniverse(viewDef, records, candidates, maxGpuCandidates);
    }
    const int uploadedCandidateCount = (options.forceBuild || r_pathTracingAnalyticLightCandidates.GetInteger() != 0)
        ? Min(maxGpuCandidates, static_cast<int>(candidates.size()))
        : 0;
    {
        OPTICK_EVENT("PT Doom Light Remap Build");
        BuildDoomAnalyticLightGpuRemap(g_doomAnalyticLightUniverse, uploadedCandidateCount);
    }
    if (!options.forceBuild && r_pathTracingAnalyticLightCandidates.GetInteger() == 0)
    {
        return gpuCandidates;
    }

    {
        OPTICK_EVENT("PT Doom Light GPU Candidate Pack");
        gpuCandidates.reserve(Min(maxGpuCandidates, static_cast<int>(candidates.size())));

        for (int i = 0; i < maxGpuCandidates && i < static_cast<int>(candidates.size()); ++i)
        {
            const DoomLightRecord& light = candidates[i];
            PathTraceDoomAnalyticLightCandidate gpuLight = {};
            const float sphereRadius = FinitePositiveOrZero(light.sphereRadius);
            const float doomRadius = FinitePositiveOrZero(light.radiusMax);
            gpuLight.originAndRadius[0] = light.origin.x;
            gpuLight.originAndRadius[1] = light.origin.y;
            gpuLight.originAndRadius[2] = light.origin.z;
            gpuLight.originAndRadius[3] = sphereRadius;
            gpuLight.colorAndIntensity[0] = Max(light.color.x, 0.0f);
            gpuLight.colorAndIntensity[1] = Max(light.color.y, 0.0f);
            gpuLight.colorAndIntensity[2] = Max(light.color.z, 0.0f);
            gpuLight.colorAndIntensity[3] = Max(light.color.w, 0.0f);
            gpuLight.doomRadiusAndArea[0] = doomRadius;
            gpuLight.doomRadiusAndArea[1] = 12.56637061f * sphereRadius * sphereRadius;
            gpuLight.doomRadiusAndArea[2] = static_cast<float>(light.portalDepth);
            gpuLight.doomRadiusAndArea[3] = static_cast<float>(light.selectionArea >= 0 ? light.selectionArea : light.area);
            if (light.castsShadows)
            {
                gpuLight.flags |= RT_DOOM_ANALYTIC_LIGHT_CASTS_SHADOWS;
            }
            if (light.gameLinked)
            {
                gpuLight.flags |= RT_DOOM_ANALYTIC_LIGHT_GAME_LINKED;
            }
            if (light.crosshairBehind)
            {
                gpuLight.flags |= RT_DOOM_ANALYTIC_LIGHT_BEHIND_CAMERA;
            }
            gpuLight.renderLightIndex = static_cast<uint32_t>(Max(light.index, 0));
            gpuLight.entityNumber = light.entityNumber >= 0 ? static_cast<uint32_t>(light.entityNumber) : RT_PT_DOOM_LIGHT_INVALID_ENTITY_NUMBER;
            gpuCandidates.push_back(gpuLight);
        }
    }

    return gpuCandidates;
}

const PathTraceDoomAnalyticLightGpuRemap& GetPathTraceDoomAnalyticLightGpuRemap()
{
    return g_doomAnalyticLightGpuRemap;
}

void RunPathTraceDoomLightDiagnostics(const viewDef_t* viewDef)
{
    if (!viewDef || !viewDef->renderWorld || !IsDoomLightGameStateActive())
    {
        return;
    }

    const bool wantsDump =
        r_pathTracingDoomLightDump.GetInteger() != 0 ||
        r_pathTracingDoomLightProbeDump.GetInteger() != 0 ||
        r_pathTracingAnalyticLightCandidateDump.GetInteger() != 0;
    if (!wantsDump)
    {
        return;
    }

    const DoomLightPortalSelection selection = BuildDoomLightPortalSelection(
        viewDef,
        idMath::ClampInt(0, 8, r_pathTracingLightAreaPortalSteps.GetInteger()));
    const std::unordered_map<int, DoomLightGameMetadata> gameMetadataByHandle = BuildDoomLightGameMetadataByHandle();
    const std::vector<DoomLightRecord> records = CollectDoomLightRecords(viewDef, selection, gameMetadataByHandle);
    RunDoomLightIdentityDump(viewDef, selection, records);
    RunDoomLightProbeDump(viewDef, records);
    RunAnalyticLightCandidateDump(selection, records);
}
