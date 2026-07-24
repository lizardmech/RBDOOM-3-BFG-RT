#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCVars.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceEntityFeed.h"
#include "PathTraceGeometryUniverse.h"
#include "PathTraceMaterialUniverse.h"
#include "PathTraceMaterialTextureDiscovery.h"
#include "PathTraceRigidIdentity.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceSceneUniverse.h"
#include "PathTraceSurfaceClassification.h"
#include "PathTraceTextureRegistry.h"
#include "../RenderCommon.h"
#include "../RenderWorld_local.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace {

void BuildEntityFeedNormalTexMatrix(const idMaterial* material, float matrix[6])
{
    const float identity[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    memcpy(matrix, identity, sizeof(identity));
    const float* registers = material ? material->ConstantRegisters() : nullptr;
    if (!material || !registers)
    {
        return;
    }

    const int registerCount = material->GetNumRegisters();
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || stage->lighting != SL_BUMP || !stage->texture.hasMatrix)
        {
            continue;
        }
        for (int row = 0; row < 2; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                const int registerIndex = stage->texture.matrix[row][column];
                if (registerIndex >= 0 && registerIndex < registerCount)
                {
                    matrix[row * 3 + column] = registers[registerIndex];
                }
            }
        }
        return;
    }
}

bool SurfaceUsesStaticModelWithJoints(const idRenderModel* model, int surfaceIndex)
{
    const modelSurface_t* surface = model ? model->Surface(surfaceIndex) : nullptr;
    const srfTriangles_t* tri = surface ? surface->geometry : nullptr;
    return tri && tri->staticModelWithJoints != nullptr;
}

bool EntityFeedModelHasJointData(const idRenderEntityLocal* entity, const idRenderModel* model)
{
    const renderEntity_t& renderEntity = entity->parms;
    if (renderEntity.numJoints > 0 || renderEntity.joints != nullptr || (model && model->NumJoints() > 0))
    {
        return true;
    }

    const int surfaceCount = model ? model->NumSurfaces() : 0;
    for (int surfaceIndex = 0; surfaceIndex < surfaceCount; ++surfaceIndex)
    {
        if (SurfaceUsesStaticModelWithJoints(model, surfaceIndex))
        {
            return true;
        }
    }

    return false;
}

unsigned int HashEntityFeedJoints(const renderEntity_t& renderEntity)
{
    if (!renderEntity.joints || renderEntity.numJoints <= 0)
    {
        return 0;
    }

    return MD5_BlockChecksum(renderEntity.joints, renderEntity.numJoints * sizeof(idJointMat));
}

void AccumulateEntityFeedSurfaceClass(
    RtPathTraceEntityFeedStats& stats,
    RtPtFeedClass feedClass)
{
    switch (feedClass)
    {
        case RtPtFeedClass::StaticWorld:
            ++stats.candidatesS0;
            break;
        case RtPtFeedClass::RigidEntity:
            ++stats.candidatesS1;
            break;
        case RtPtFeedClass::RigidSkinned:
            ++stats.candidatesS2;
            break;
        case RtPtFeedClass::TrueDeform:
            ++stats.candidatesS3;
            break;
        default:
            ++stats.candidatesS4;
            break;
    }
}

bool EntityFeedSurfaceUsableForRigidRoute(const modelSurface_t* surface, const srfTriangles_t* tri, const idMaterial* material)
{
    return
        surface != nullptr &&
        tri != nullptr &&
        tri->verts != nullptr &&
        tri->indexes != nullptr &&
        tri->numVerts > 0 &&
        tri->numIndexes > 0 &&
        material != nullptr &&
        material->IsDrawn();
}

bool EntityFeedCanPromoteRigidEmissiveCard(const idRenderEntityLocal* entity, const idRenderModel* model, const srfTriangles_t* tri, const idMaterial* material)
{
    if (r_pathTracingRigidRouteEmissiveCards.GetInteger() == 0 ||
        !entity ||
        !model ||
        !tri ||
        !material ||
        model->IsStaticWorldModel() ||
        model->IsDynamicModel() != DM_STATIC)
    {
        return false;
    }

    const renderEntity_t& renderEntity = entity->parms;
    if (renderEntity.joints != nullptr ||
        renderEntity.numJoints > 0 ||
        renderEntity.callback != nullptr ||
        renderEntity.forceUpdate != 0 ||
        renderEntity.weaponDepthHack ||
        renderEntity.modelDepthHack != 0.0f ||
        entity->dynamicModel != nullptr ||
        entity->cachedDynamicModel != nullptr ||
        tri->staticModelWithJoints != nullptr)
    {
        return false;
    }

    return SmokeMaterialCanPromoteEntityFeedRigidEmissiveCard(material);
}

struct EntityFeedVisibleDrawSurfSet
{
    std::unordered_set<int> modelSurfaceIndexes;
    std::unordered_set<const srfTriangles_t*> geometries;
    std::unordered_set<const idMaterial*> materials;
};

using EntityFeedVisibleDrawSurfMap = std::unordered_map<const idRenderEntityLocal*, EntityFeedVisibleDrawSurfSet>;

EntityFeedVisibleDrawSurfMap BuildEntityFeedVisibleDrawSurfMap(const viewDef_t* viewDef)
{
    EntityFeedVisibleDrawSurfMap visibleDrawSurfs;
    if (!viewDef || !viewDef->drawSurfs)
    {
        return visibleDrawSurfs;
    }

    visibleDrawSurfs.reserve(viewDef->numDrawSurfs);
    for (int drawSurfIndex = 0; drawSurfIndex < viewDef->numDrawSurfs; ++drawSurfIndex)
    {
        const drawSurf_t* drawSurf = viewDef->drawSurfs[drawSurfIndex];
        const idRenderEntityLocal* entity =
            (drawSurf && drawSurf->space) ? drawSurf->space->entityDef : nullptr;
        if (!entity)
        {
            continue;
        }

        EntityFeedVisibleDrawSurfSet& visibleSet = visibleDrawSurfs[entity];
        if (drawSurf->modelSurfaceIndex >= 0)
        {
            visibleSet.modelSurfaceIndexes.insert(drawSurf->modelSurfaceIndex);
        }
        if (drawSurf->frontEndGeo)
        {
            visibleSet.geometries.insert(drawSurf->frontEndGeo);
        }
        if (drawSurf->material)
        {
            visibleSet.materials.insert(drawSurf->material);
        }
    }

    return visibleDrawSurfs;
}

bool EntityFeedSurfaceHasVisibleDrawSurf(
    const EntityFeedVisibleDrawSurfMap& visibleDrawSurfs,
    const idRenderEntityLocal* entity,
    int modelSurfaceIndex,
    const srfTriangles_t* tri,
    const idMaterial* material)
{
    if (!entity)
    {
        return false;
    }

    const auto visibleIt = visibleDrawSurfs.find(entity);
    if (visibleIt == visibleDrawSurfs.end())
    {
        return false;
    }

    const EntityFeedVisibleDrawSurfSet& visibleSet = visibleIt->second;
    if (modelSurfaceIndex >= 0 &&
        visibleSet.modelSurfaceIndexes.find(modelSurfaceIndex) != visibleSet.modelSurfaceIndexes.end())
    {
        return true;
    }
    if (tri && visibleSet.geometries.find(tri) != visibleSet.geometries.end())
    {
        return true;
    }
    if (material && visibleSet.materials.find(material) != visibleSet.materials.end())
    {
        return true;
    }

    return false;
}

struct EntityFeedRigidCandidate
{
    RtPathTraceMeshKey meshKey;
    RtPathTraceRigidInstanceSnapshot rigidSnapshot;
    RtPathTraceMeshObservation meshObservation;
    RtPathTraceInstanceObservation instanceObservation;
    RtPathTraceRigidMeshCandidateObservation candidateObservation;
    float priority = 0.0f;
    float distance = 0.0f;
    bool onScreen = false;
    bool emissive = false;
};

struct EntityFeedCapturedRigidSurface
{
    RtPathTraceMeshKey meshKey;
    RtPathTraceRigidInstanceSnapshot rigidSnapshot;
    const idRenderEntityLocal* entity = nullptr;
    const idMaterial* baseMaterial = nullptr;
    uint32_t surfaceClassId = 0;
    uint32_t surfaceClassAndFlags = 0;
    int currentArea = -1;
    float objectToWorld[16] = {};
    float distance = 0.0f;
    float projectedSize = 0.0f;
    bool onScreen = false;
    bool emissive = false;
    idStr materialName;
    idStr modelName;
};

struct EntityFeedEntityMaterialKey
{
    const idRenderEntityLocal* entity = nullptr;
    const idMaterial* material = nullptr;

    bool operator==(const EntityFeedEntityMaterialKey& rhs) const
    {
        return entity == rhs.entity && material == rhs.material;
    }
};

struct EntityFeedEntityMaterialKeyHash
{
    size_t operator()(const EntityFeedEntityMaterialKey& key) const
    {
        const uintptr_t entityBits = reinterpret_cast<uintptr_t>(key.entity);
        const uintptr_t materialBits = reinterpret_cast<uintptr_t>(key.material);
        return static_cast<size_t>((entityBits >> 4) ^ (materialBits << 1) ^ (materialBits >> 9));
    }
};

struct EntityFeedMaterialCacheRecord
{
    bool classifierValid = false;
    RtSmokeTranslucentClassifierInfo classifier;
    bool routeSignatureValid = false;
    uint32_t routeSignature = 0;
    bool promoteRigidEmissiveCardValid = false;
    bool promoteRigidEmissiveCard = false;
};

struct EntityFeedCaptureMaterialCache
{
    std::unordered_map<const idMaterial*, EntityFeedMaterialCacheRecord> materialRecords;
    std::unordered_map<uint32_t, bool> materialEmissive;
    std::unordered_map<EntityFeedEntityMaterialKey, bool, EntityFeedEntityMaterialKeyHash> activeEmissiveStage;
};

struct EntityFeedResidentSurfaceKey
{
    PtRenderDefKey renderDefKey;
    int surfaceIndex = -1;

    bool operator==(const EntityFeedResidentSurfaceKey& rhs) const
    {
        return
            renderDefKey.world == rhs.renderDefKey.world &&
            renderDefKey.index == rhs.renderDefKey.index &&
            renderDefKey.generation == rhs.renderDefKey.generation &&
            surfaceIndex == rhs.surfaceIndex;
    }
};

struct RtEntityFeedResidentSurface
{
    EntityFeedResidentSurfaceKey key;
    uint32_t modelEpoch = 0;
    uint64 materialToken = 0;
    int lastSeenFrame = 0;
    bool baseValid = false;
    bool routeValid = false;
    const idMaterial* remappedMaterial = nullptr;
    RtPtFeedClass feedClass = RtPtFeedClass::Transient;
    bool promotedEmissiveCard = false;
    uint32_t baseMaterialId = 0;
    uint32_t materialId = 0;
    uint32_t materialClassSignature = 0;
    uint32_t surfaceClassId = 0;
    uint32_t surfaceClassAndFlags = 0;
    bool activeEmissiveStage = false;
    bool activeEmissiveStageDynamic = false;
    RtPathTraceMeshKey meshKey;
    RtPathTraceRigidInstanceSnapshot rigidSnapshot;
    bool emissive = false;
    idStr materialName;
    idStr modelName;
};

struct EntityFeedResidentEntitySlot
{
    PtRenderDefKey renderDefKey;
    int lastScannedFrame = -1;
    std::vector<RtEntityFeedResidentSurface> surfaceRecords;
};

struct EntityFeedResidentSurfaceStore
{
    const idRenderWorldLocal* renderWorld = nullptr;
    idStr mapName;
    ID_TIME_T mapTimeStamp = 0;
    uint64 mapLoadSerial = 0;
    int lastGcFrame = -1;
    std::vector<EntityFeedResidentEntitySlot> entitySlots;

    void ResetForWorld(const idRenderWorldLocal* world)
    {
        renderWorld = world;
        mapName = world ? world->mapName : "";
        mapTimeStamp = world ? world->mapTimeStamp : 0;
        mapLoadSerial = world ? world->mapLoadSerial : 0;
        lastGcFrame = -1;
        entitySlots.clear();
    }
};

EntityFeedResidentSurfaceStore& EntityFeedResidentStoreForWorld(const idRenderWorldLocal* renderWorld)
{
    static EntityFeedResidentSurfaceStore store;
    const char* mapName = renderWorld ? renderWorld->mapName.c_str() : "";
    const ID_TIME_T mapTimeStamp = renderWorld ? renderWorld->mapTimeStamp : 0;
    const uint64 mapLoadSerial = renderWorld ? renderWorld->mapLoadSerial : 0;
    if (store.renderWorld != renderWorld ||
        store.mapName.Icmp(mapName) != 0 ||
        store.mapTimeStamp != mapTimeStamp ||
        store.mapLoadSerial != mapLoadSerial)
    {
        store.ResetForWorld(renderWorld);
    }
    return store;
}

bool EntityFeedResidentRecordValid(const RtEntityFeedResidentSurface& record)
{
    return record.baseValid;
}

int CountEntityFeedResidentRecords(const EntityFeedResidentSurfaceStore& store)
{
    int count = 0;
    for (const EntityFeedResidentEntitySlot& entitySlot : store.entitySlots)
    {
        for (const RtEntityFeedResidentSurface& record : entitySlot.surfaceRecords)
        {
            if (EntityFeedResidentRecordValid(record))
            {
                ++count;
            }
        }
    }
    return count;
}

int PruneEntityFeedResidentStore(EntityFeedResidentSurfaceStore& store, int currentFrame)
{
    if (store.lastGcFrame == currentFrame)
    {
        return 0;
    }
    store.lastGcFrame = currentFrame;

    const int ttlFrames = Max(0, r_pathTracingResidencyTtlFrames.GetInteger());
    int evicted = 0;
    for (EntityFeedResidentEntitySlot& entitySlot : store.entitySlots)
    {
        if (entitySlot.renderDefKey.world == nullptr)
        {
            continue;
        }
        for (RtEntityFeedResidentSurface& record : entitySlot.surfaceRecords)
        {
            if (!EntityFeedResidentRecordValid(record))
            {
                continue;
            }
            if (currentFrame - record.lastSeenFrame > ttlFrames)
            {
                record = RtEntityFeedResidentSurface();
                ++evicted;
            }
        }
    }
    return evicted;
}

bool EntityFeedResidencyEnabled()
{
    return
        r_pathTracingResidency.GetInteger() != 0 &&
        r_pathTracingResidencyEntityFeed.GetInteger() != 0;
}

uint64 EntityFeedHashTokenValue(uint64 hash, uint64 value)
{
    hash ^= value;
    hash *= 1099511628211ull;
    return hash;
}

uint64 EntityFeedEntityMaterialTokenBase(const renderEntity_t& renderEntity)
{
    // EntityFeed derived facts are valid until identity/material/geometry inputs change.
    // MUST bump: modelEpoch for hModel/model-geometry swaps; customSkin/customShader
    // pointer changes; the base surface material pointer; renderEntity.entityNum because
    // runtime material ids include it; and material decl-level emissive capability changes
    // when represented by a different material pointer. Runtime material ids also include
    // entity index and surface index, which are already covered by the resident key.
    // MUST NOT bump: objectToWorld, distance, projected size, viewCount/on-screen state,
    // and visible drawSurf presence. Active emissive stage can depend on shader parms/time,
    // so dynamic materials resample that stage every frame on a residency hit instead of
    // folding it into this token.
    uint64 hash = 14695981039346656037ull;
    hash = EntityFeedHashTokenValue(hash, static_cast<uint64>(reinterpret_cast<uintptr_t>(renderEntity.customSkin)));
    hash = EntityFeedHashTokenValue(hash, static_cast<uint64>(reinterpret_cast<uintptr_t>(renderEntity.customShader)));
    hash = EntityFeedHashTokenValue(hash, static_cast<uint64>(static_cast<uint32_t>(renderEntity.entityNum)));
    return hash;
}

uint64 EntityFeedSurfaceMaterialToken(uint64 entityMaterialTokenBase, const idMaterial* surfaceMaterial)
{
    return EntityFeedHashTokenValue(
        entityMaterialTokenBase,
        static_cast<uint64>(reinterpret_cast<uintptr_t>(surfaceMaterial)));
}

bool EntityFeedActiveEmissiveStageIsDynamic(const idMaterial* material)
{
    return material && material->ConstantRegisters() == nullptr;
}

EntityFeedResidentEntitySlot* FindOrCreateEntityFeedResidentEntitySlot(
    EntityFeedResidentSurfaceStore* store,
    const PtRenderDefKey& key)
{
    if (!store ||
        key.index < 0 ||
        key.generation == 0)
    {
        return nullptr;
    }

    if (key.index >= static_cast<int>(store->entitySlots.size()))
    {
        store->entitySlots.resize(static_cast<size_t>(key.index + 1));
    }

    EntityFeedResidentEntitySlot& slot = store->entitySlots[static_cast<size_t>(key.index)];
    if (slot.renderDefKey.world != key.world ||
        slot.renderDefKey.worldGeneration != key.worldGeneration ||
        slot.renderDefKey.index != key.index ||
        slot.renderDefKey.generation != key.generation)
    {
        slot = EntityFeedResidentEntitySlot();
        slot.renderDefKey = key;
    }

    return &slot;
}

RtEntityFeedResidentSurface* FindOrCreateEntityFeedResidentSurface(
    EntityFeedResidentEntitySlot* entitySlot,
    const EntityFeedResidentSurfaceKey& key)
{
    if (!entitySlot || key.surfaceIndex < 0)
    {
        return nullptr;
    }

    if (key.surfaceIndex >= static_cast<int>(entitySlot->surfaceRecords.size()))
    {
        entitySlot->surfaceRecords.resize(static_cast<size_t>(key.surfaceIndex + 1));
    }

    RtEntityFeedResidentSurface& record = entitySlot->surfaceRecords[static_cast<size_t>(key.surfaceIndex)];
    if (record.key == key)
    {
        return &record;
    }

    record = RtEntityFeedResidentSurface();
    record.key = key;
    return &record;
}

void UpdateEntityFeedResidentSurfaceBase(
    RtEntityFeedResidentSurface& record,
    const EntityFeedResidentSurfaceKey& key,
    uint32_t modelEpoch,
    uint64 materialToken,
    const idMaterial* material,
    RtPtFeedClass feedClass,
    bool promotedEmissiveCard,
    const idRenderModel* model)
{
    record.key = key;
    record.modelEpoch = modelEpoch;
    record.materialToken = materialToken;
    record.lastSeenFrame = tr.frameCount;
    record.baseValid = true;
    record.routeValid = false;
    record.remappedMaterial = material;
    record.feedClass = feedClass;
    record.promotedEmissiveCard = promotedEmissiveCard;
    record.materialName = material ? material->GetName() : "<none>";
    record.modelName = model ? model->Name() : "<none>";
}

void UpdateEntityFeedResidentSurfaceRoute(
    RtEntityFeedResidentSurface& record,
    uint32_t baseMaterialId,
    uint32_t materialId,
    uint32_t materialClassSignature,
    uint32_t surfaceClassId,
    uint32_t surfaceClassAndFlags,
    bool activeEmissiveStage,
    bool activeEmissiveStageDynamic,
    const RtPathTraceMeshKey& meshKey,
    const RtPathTraceRigidInstanceSnapshot& rigidSnapshot,
    bool emissive)
{
    record.routeValid = true;
    record.baseMaterialId = baseMaterialId;
    record.materialId = materialId;
    record.materialClassSignature = materialClassSignature;
    record.surfaceClassId = surfaceClassId;
    record.surfaceClassAndFlags = surfaceClassAndFlags;
    record.activeEmissiveStage = activeEmissiveStage;
    record.activeEmissiveStageDynamic = activeEmissiveStageDynamic;
    record.meshKey = meshKey;
    record.rigidSnapshot = rigidSnapshot;
    record.emissive = emissive;
}

idVec3 EntityFeedEntityOrigin(const idRenderEntityLocal* entity)
{
    return idVec3(entity->modelMatrix[12], entity->modelMatrix[13], entity->modelMatrix[14]);
}

float EntityFeedEntityDistance(const idRenderEntityLocal* entity, const idVec3& viewOrigin)
{
    return (EntityFeedEntityOrigin(entity) - viewOrigin).Length();
}

float EntityFeedProjectedSizeProxy(const idRenderEntityLocal* entity, float distance)
{
    if (!entity || entity->localReferenceBounds.IsCleared())
    {
        return 0.0f;
    }

    const idVec3 extent = entity->localReferenceBounds[1] - entity->localReferenceBounds[0];
    const float radius = extent.Length() * 0.5f;
    const float safeDistance = distance > 1.0f ? distance : 1.0f;
    return radius / safeDistance;
}

bool EntityFeedMaterialIsEmissive(uint32_t materialId)
{
    const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(materialId, -1);
    const RtSmokeMaterialUniverseFacts& facts = GetSmokeMaterialUniverseFacts(materialId, info);
    return facts.emissive;
}

const RtSmokeTranslucentClassifierInfo& EntityFeedMaterialClassifier(
    EntityFeedCaptureMaterialCache& cache,
    const idMaterial* material)
{
    EntityFeedMaterialCacheRecord& record = cache.materialRecords[material];
    if (!record.classifierValid)
    {
        OPTICK_EVENT("PT EntityFeed Capture Classifier Miss");
        record.classifier = BuildSmokeTranslucentClassifierInfo(material);
        record.classifierValid = true;
    }
    return record.classifier;
}

bool EntityFeedCanPromoteRigidEmissiveCardCached(
    EntityFeedCaptureMaterialCache& cache,
    const idMaterial* material)
{
    EntityFeedMaterialCacheRecord& record = cache.materialRecords[material];
    if (!record.promoteRigidEmissiveCardValid)
    {
        const RtSmokeTranslucentClassifierInfo& classifier = EntityFeedMaterialClassifier(cache, material);
        record.promoteRigidEmissiveCard = SmokeMaterialCanPromoteEntityFeedRigidEmissiveCard(material, classifier);
        record.promoteRigidEmissiveCardValid = true;
    }
    return record.promoteRigidEmissiveCard;
}

uint32_t EntityFeedMaterialRouteClassSignatureCached(
    EntityFeedCaptureMaterialCache& cache,
    const idMaterial* material)
{
    EntityFeedMaterialCacheRecord& record = cache.materialRecords[material];
    if (!record.routeSignatureValid)
    {
        const RtSmokeTranslucentClassifierInfo& classifier = EntityFeedMaterialClassifier(cache, material);
        record.routeSignature = SmokeMaterialRouteClassSignature(
            material,
            RtSmokeSurfaceClass::RigidEntity,
            RtSmokeTranslucentSubtype::Unknown,
            classifier);
        record.routeSignatureValid = true;
    }
    return record.routeSignature;
}

bool EntityFeedMaterialIsEmissiveCached(
    EntityFeedCaptureMaterialCache& cache,
    uint32_t materialId)
{
    const auto existing = cache.materialEmissive.find(materialId);
    if (existing != cache.materialEmissive.end())
    {
        return existing->second;
    }

    const bool emissive = EntityFeedMaterialIsEmissive(materialId);
    cache.materialEmissive.emplace(materialId, emissive);
    return emissive;
}

bool EntityFeedActiveEmissiveStageCached(
    EntityFeedCaptureMaterialCache& cache,
    const viewDef_t* viewDef,
    const idRenderEntityLocal* entity,
    const idMaterial* material)
{
    const EntityFeedEntityMaterialKey key{ entity, material };
    const auto existing = cache.activeEmissiveStage.find(key);
    if (existing != cache.activeEmissiveStage.end())
    {
        return existing->second;
    }

    const RtSmokeTranslucentClassifierInfo& classifier = EntityFeedMaterialClassifier(cache, material);
    const bool active = SmokeEntitySurfaceHasActiveEmissiveStage(viewDef, entity, material, classifier);
    cache.activeEmissiveStage.emplace(key, active);
    return active;
}

float EntityFeedCandidatePriority(bool onScreen, bool emissive, float projectedSize, float distance)
{
    // These weights intentionally make current-frame and emissive entities dominate, then use
    // apparent size and distance to keep the bounded offscreen set stable and nearby.
    constexpr float onScreenWeight = 1000000.0f;
    constexpr float emissiveWeight = 10000.0f;
    constexpr float projectedSizeWeight = 1000.0f;
    constexpr float distanceWeight = 0.01f;
    return
        (onScreen ? onScreenWeight : 0.0f) +
        (emissive ? emissiveWeight : 0.0f) +
        projectedSize * projectedSizeWeight -
        distance * distanceWeight;
}

EntityFeedRigidCandidate BuildEntityFeedRigidCandidate(const EntityFeedCapturedRigidSurface& captured)
{
    EntityFeedRigidCandidate candidate;
    candidate.meshKey = captured.meshKey;
    candidate.rigidSnapshot = captured.rigidSnapshot;
    candidate.distance = captured.distance;
    candidate.onScreen = captured.onScreen;
    candidate.emissive = captured.emissive;
    candidate.priority = EntityFeedCandidatePriority(
        captured.onScreen,
        captured.emissive,
        captured.projectedSize,
        captured.distance);

    candidate.meshObservation.key = captured.rigidSnapshot.meshKey;
    candidate.meshObservation.stableHash = captured.rigidSnapshot.meshHash;
    candidate.meshObservation.baseMaterial = captured.baseMaterial;
    candidate.meshObservation.surfaceClassId = captured.surfaceClassId;
    candidate.meshObservation.jointIndex = captured.rigidSnapshot.jointIndex;
    candidate.meshObservation.materialName = captured.materialName;
    candidate.meshObservation.modelName = captured.modelName;
    candidate.meshObservation.localSpaceValid = true;

    candidate.instanceObservation.meshHash = captured.rigidSnapshot.meshHash;
    candidate.instanceObservation.entity = captured.entity;
    candidate.instanceObservation.entityIndex = captured.rigidSnapshot.entityIndex;
    candidate.instanceObservation.renderEntityNum = captured.rigidSnapshot.renderEntityNum;
    candidate.instanceObservation.drawSurfIndex = -1;
    candidate.instanceObservation.modelSurfaceIndex = captured.rigidSnapshot.modelSurfaceIndex;
    candidate.instanceObservation.jointIndex = captured.rigidSnapshot.jointIndex;
    candidate.instanceObservation.currentArea = captured.currentArea;
    candidate.instanceObservation.renderDefKey = captured.rigidSnapshot.renderDefKey;
    candidate.instanceObservation.modelEpoch = captured.rigidSnapshot.modelEpoch;
    candidate.instanceObservation.materialOverrideId = captured.rigidSnapshot.materialId;
    candidate.instanceObservation.surfaceClassId = captured.surfaceClassId;
    candidate.instanceObservation.triangleClassAndFlags = captured.surfaceClassAndFlags;
    candidate.instanceObservation.sourceFlags = captured.rigidSnapshot.sourceFlags;
    memcpy(candidate.instanceObservation.objectToWorld, captured.objectToWorld, sizeof(candidate.instanceObservation.objectToWorld));
    candidate.instanceObservation.instanceId = captured.rigidSnapshot.instanceId;
    candidate.instanceObservation.materialName = captured.materialName;
    candidate.instanceObservation.modelName = captured.modelName;

    candidate.candidateObservation.tri = captured.meshKey.tri;
    candidate.candidateObservation.meshHash = captured.rigidSnapshot.meshHash;
    candidate.candidateObservation.instanceId = captured.rigidSnapshot.instanceId;
    candidate.candidateObservation.vertexBufferIdentity = captured.meshKey.vertexBufferIdentity;
    candidate.candidateObservation.indexBufferIdentity = captured.meshKey.indexBufferIdentity;
    candidate.candidateObservation.sourceFlags = captured.rigidSnapshot.sourceFlags;
    candidate.candidateObservation.materialId = captured.rigidSnapshot.materialId;
    candidate.candidateObservation.materialClassSignature = captured.rigidSnapshot.materialClassSignature;
    candidate.candidateObservation.surfaceClassId = captured.surfaceClassId;
    candidate.candidateObservation.triangleClassAndFlags = captured.surfaceClassAndFlags;
    candidate.candidateObservation.vertexFormat = captured.meshKey.vertexFormat;
    candidate.candidateObservation.drawSurfIndex = -1;
    candidate.candidateObservation.entityIndex = captured.rigidSnapshot.entityIndex;
    candidate.candidateObservation.renderEntityNum = captured.rigidSnapshot.renderEntityNum;
    candidate.candidateObservation.modelEpoch = captured.rigidSnapshot.modelEpoch;
    candidate.candidateObservation.jointIndex = captured.rigidSnapshot.jointIndex;
    candidate.candidateObservation.numVerts = captured.meshKey.numVerts;
    candidate.candidateObservation.numIndexes = captured.meshKey.numIndexes;
    candidate.candidateObservation.localSpaceValid = candidate.meshObservation.localSpaceValid;
    BuildEntityFeedNormalTexMatrix(captured.baseMaterial, candidate.candidateObservation.normalTexMatrix);
    candidate.candidateObservation.materialName = captured.materialName;
    candidate.candidateObservation.modelName = captured.modelName;
    return candidate;
}

std::vector<EntityFeedRigidCandidate> ProcessEntityFeedCapturedRigidSurfaces(
    const std::vector<EntityFeedCapturedRigidSurface>& capturedSurfaces,
    int rigidRouteMaxInstances)
{
    OPTICK_EVENT("PT EntityFeed Process Captured Candidates");

    std::vector<EntityFeedRigidCandidate> candidates;
    candidates.reserve(rigidRouteMaxInstances);

    for (const EntityFeedCapturedRigidSurface& captured : capturedSurfaces)
    {
        candidates.push_back(BuildEntityFeedRigidCandidate(captured));
    }

    {
        OPTICK_EVENT("PT EntityFeed Sort Candidates");
        std::stable_sort(
            candidates.begin(),
            candidates.end(),
            [](const EntityFeedRigidCandidate& lhs, const EntityFeedRigidCandidate& rhs) {
                if (lhs.onScreen != rhs.onScreen)
                {
                    return lhs.onScreen;
                }
                if (lhs.priority != rhs.priority)
                {
                    return lhs.priority > rhs.priority;
                }
                if (lhs.distance != rhs.distance)
                {
                    return lhs.distance < rhs.distance;
                }
                return lhs.rigidSnapshot.instanceId < rhs.rigidSnapshot.instanceId;
            });
    }

    return candidates;
}

struct EntityFeedReachableAreaSet
{
    std::vector<bool> reachableAreas;
    std::vector<int> areaDepth;
};

int EntityFeedAreaDepth(const std::vector<int>& areaDepth, int areaIndex)
{
    if (areaIndex < 0 || areaIndex >= static_cast<int>(areaDepth.size()))
    {
        return -1;
    }
    return areaDepth[areaIndex];
}

std::vector<EntityFeedCapturedRigidSurface> CaptureEntityFeedRigidSurfaces(
    const viewDef_t* viewDef,
    idRenderWorldLocal* renderWorld,
    const std::vector<bool>& reachableAreas,
    const std::vector<int>& areaDepth,
    const EntityFeedVisibleDrawSurfMap& visibleDrawSurfs,
    const idVec3& viewOrigin,
    int rigidRouteMaxInstances,
    float maxDistance,
    RtPathTraceEntityFeedStats& stats)
{
    OPTICK_EVENT("PT EntityFeed Capture Candidates");

    std::vector<EntityFeedCapturedRigidSurface> capturedSurfaces;
    std::unordered_set<uint64> candidateInstanceIds;
    std::unordered_set<uint32_t> registeredMaterialIds;
    std::unordered_set<const idRenderEntityLocal*> scannedEntities;
    std::unordered_set<const idRenderEntityLocal*> liveEntityDefs;
    EntityFeedCaptureMaterialCache materialCache;
    const bool residencyEnabled = EntityFeedResidencyEnabled();
    EntityFeedResidentSurfaceStore* residentStore = nullptr;
    capturedSurfaces.reserve(rigidRouteMaxInstances);
    candidateInstanceIds.reserve(rigidRouteMaxInstances * 2);
    registeredMaterialIds.reserve(128);
    scannedEntities.reserve(renderWorld ? renderWorld->entityDefs.Num() : 0);
    liveEntityDefs.reserve(renderWorld ? renderWorld->entityDefs.Num() : 0);
    materialCache.materialRecords.reserve(256);
    materialCache.materialEmissive.reserve(256);
    materialCache.activeEmissiveStage.reserve(256);

    if (!renderWorld)
    {
        return capturedSurfaces;
    }
    for (int entityIndex = 0; entityIndex < renderWorld->entityDefs.Num(); ++entityIndex)
    {
        const idRenderEntityLocal* liveEntity = renderWorld->entityDefs[entityIndex];
        if (liveEntity)
        {
            liveEntityDefs.insert(liveEntity);
        }
    }
    if (residencyEnabled)
    {
        residentStore = &EntityFeedResidentStoreForWorld(renderWorld);
        stats.residencyEvictedRecords += PruneEntityFeedResidentStore(*residentStore, tr.frameCount);
    }

    for (int areaIndex = 0; areaIndex < static_cast<int>(reachableAreas.size()); ++areaIndex)
    {
        if (!reachableAreas[areaIndex])
        {
            continue;
        }
        ++stats.reachableAreas;
        if (areaIndex < 0 || areaIndex >= renderWorld->numPortalAreas)
        {
            continue;
        }

        const bool beyondViewArea = EntityFeedAreaDepth(areaDepth, areaIndex) > 0;
        if (beyondViewArea)
        {
            ++stats.residencyBeyondViewAreas;
        }
        else
        {
            ++stats.residencyPlayerAreaAreas;
        }
        const auto AddVisitedSurface = [&]() {
            ++stats.residencyVisited;
            if (beyondViewArea)
            {
                ++stats.residencyBeyondViewVisited;
            }
            else
            {
                ++stats.residencyPlayerAreaVisited;
            }
        };
        const auto AddDerivedSurface = [&]() {
            ++stats.residencyDerived;
            if (beyondViewArea)
            {
                ++stats.residencyBeyondViewDerived;
            }
            else
            {
                ++stats.residencyPlayerAreaDerived;
            }
        };
        const int areaVisitStartMs = Sys_Milliseconds();
        OPTICK_EVENT_DYNAMIC(beyondViewArea ? "PT EntityFeed Visit BeyondView" : "PT EntityFeed Visit PlayerArea");
        portalArea_t* area = &renderWorld->portalAreas[areaIndex];
        for (areaReference_t* ref = area->entityRefs.areaNext; ref != &area->entityRefs; ref = ref->areaNext)
        {
            if (beyondViewArea)
            {
                ++stats.residencyBeyondViewRefs;
            }
            else
            {
                ++stats.residencyPlayerAreaRefs;
            }
            idRenderEntityLocal* entity = ref ? ref->entity : nullptr;
            if (!entity || liveEntityDefs.find(entity) == liveEntityDefs.end())
            {
                continue;
            }
            if (entity->world != renderWorld ||
                entity->index < 0 ||
                entity->index >= renderWorld->entityDefs.Num() ||
                renderWorld->entityDefs[entity->index] != entity)
            {
                continue;
            }

            const renderEntity_t& renderEntity = entity->parms;
            // The V2 feed only admits rigid entities (plus the narrow rigid
            // emissive-card promotion below). Known callback/skinned/generated
            // entities are rejected by classification and promotion anyway, so
            // reject them before touching their retained hModel vtable.
            if (renderEntity.callback != nullptr ||
                renderEntity.forceUpdate != 0 ||
                renderEntity.joints != nullptr ||
                renderEntity.numJoints > 0 ||
                renderEntity.weaponDepthHack ||
                renderEntity.modelDepthHack != 0.0f ||
                entity->dynamicModel != nullptr ||
                entity->cachedDynamicModel != nullptr)
            {
                continue;
            }

            idRenderModel* model = renderEntity.hModel;
            if (!model)
            {
                continue;
            }
            PtRenderDefKey entityRenderDefKey;
            uint32_t entityModelEpoch = 0;
            uint64 entityMaterialTokenBase = 0;
            EntityFeedResidentEntitySlot* residentEntitySlot = nullptr;
            if (residentStore)
            {
                entityRenderDefKey = PtGeometryLifecycle::MakeEntityKey(entity);
                residentEntitySlot = FindOrCreateEntityFeedResidentEntitySlot(residentStore, entityRenderDefKey);
                if (residentEntitySlot)
                {
                    if (residentEntitySlot->lastScannedFrame == tr.frameCount)
                    {
                        continue;
                    }
                    residentEntitySlot->lastScannedFrame = tr.frameCount;
                    entityModelEpoch = PtGeometryLifecycle::EntityModelEpoch(entityRenderDefKey.world, entityRenderDefKey.index);
                    entityMaterialTokenBase = EntityFeedEntityMaterialTokenBase(renderEntity);
                }
            }
            if (!residentEntitySlot && !scannedEntities.insert(entity).second)
            {
                continue;
            }
            for (int surfaceIndex = 0; surfaceIndex < model->NumSurfaces(); ++surfaceIndex)
            {
                AddVisitedSurface();
                EntityFeedResidentSurfaceKey residentKey;
                RtEntityFeedResidentSurface* residentRecord = nullptr;
                if (residentEntitySlot)
                {
                    residentKey.renderDefKey = entityRenderDefKey;
                    residentKey.surfaceIndex = surfaceIndex;
                    residentRecord = FindOrCreateEntityFeedResidentSurface(residentEntitySlot, residentKey);
                }
                const modelSurface_t* surface = model->Surface(surfaceIndex);
                const srfTriangles_t* tri = surface ? surface->geometry : nullptr;
                const idMaterial* surfaceMaterial = surface ? surface->shader : nullptr;
                const uint64 surfaceMaterialToken = residentEntitySlot
                    ? EntityFeedSurfaceMaterialToken(entityMaterialTokenBase, surfaceMaterial)
                    : 0;
                const bool residentBaseHit =
                    residentRecord &&
                    residentRecord->baseValid &&
                    residentRecord->modelEpoch == entityModelEpoch &&
                    residentRecord->materialToken == surfaceMaterialToken;

                const idMaterial* material = residentBaseHit ? residentRecord->remappedMaterial : nullptr;
                RtPtFeedClass feedClass = residentBaseHit ? residentRecord->feedClass : RtPtFeedClass::Transient;
                bool promotedEmissiveCard = residentBaseHit ? residentRecord->promotedEmissiveCard : false;

                if (residentBaseHit)
                {
                    residentRecord->lastSeenFrame = tr.frameCount;
                }
                else
                {
                    ++stats.residencyCacheMisses;
                    AddDerivedSurface();
                    {
                        OPTICK_EVENT("PT EntityFeed Capture Material Remap");
                        material = R_RemapShaderBySkin(surfaceMaterial, renderEntity.customSkin, renderEntity.customShader);
                    }

                    {
                        OPTICK_EVENT("PT EntityFeed Capture Classify");
                        feedClass = ClassifyEntityFeedSurface(entity, model, surface);
                    }

                    if (feedClass == RtPtFeedClass::Transient)
                    {
                        OPTICK_EVENT("PT EntityFeed Capture Emissive Card Test");
                        promotedEmissiveCard =
                            r_pathTracingRigidRouteEmissiveCards.GetInteger() != 0 &&
                            entity &&
                            model &&
                            tri &&
                            material &&
                            !model->IsStaticWorldModel() &&
                            model->IsDynamicModel() == DM_STATIC &&
                            renderEntity.joints == nullptr &&
                            renderEntity.numJoints <= 0 &&
                            renderEntity.callback == nullptr &&
                            renderEntity.forceUpdate == 0 &&
                            !renderEntity.weaponDepthHack &&
                            renderEntity.modelDepthHack == 0.0f &&
                            entity->dynamicModel == nullptr &&
                            entity->cachedDynamicModel == nullptr &&
                            tri->staticModelWithJoints == nullptr &&
                            EntityFeedCanPromoteRigidEmissiveCardCached(materialCache, material);
                    }
                    if (residentRecord)
                    {
                        UpdateEntityFeedResidentSurfaceBase(
                            *residentRecord,
                            residentKey,
                            entityModelEpoch,
                            surfaceMaterialToken,
                            material,
                            feedClass,
                            promotedEmissiveCard,
                            model);
                    }
                }

                bool visibleDrawSurf = false;
                {
                    OPTICK_EVENT("PT EntityFeed Capture Visible Test");
                    visibleDrawSurf = EntityFeedSurfaceHasVisibleDrawSurf(visibleDrawSurfs, entity, surfaceIndex, tri, material);
                }
                if (promotedEmissiveCard && visibleDrawSurf)
                {
                    if (residentBaseHit)
                    {
                        ++stats.residencyCacheHits;
                    }
                    continue;
                }
                if (feedClass == RtPtFeedClass::RigidEntity && visibleDrawSurf)
                {
                    if (residentBaseHit)
                    {
                        ++stats.residencyCacheHits;
                    }
                    continue;
                }
                if (feedClass != RtPtFeedClass::RigidEntity && !promotedEmissiveCard)
                {
                    if (residentBaseHit)
                    {
                        ++stats.residencyCacheHits;
                    }
                    continue;
                }
                ++stats.candidatesS1;

                if (!EntityFeedSurfaceUsableForRigidRoute(surface, tri, material))
                {
                    if (residentBaseHit)
                    {
                        ++stats.residencyCacheHits;
                    }
                    continue;
                }

                if (residentBaseHit && residentRecord->routeValid)
                {
                    ++stats.residencyCacheHits;
                    const RtPathTraceRigidInstanceSnapshot& rigidSnapshot = residentRecord->rigidSnapshot;
                    uint32_t surfaceClassAndFlags = residentRecord->surfaceClassAndFlags;
                    if (residentRecord->activeEmissiveStageDynamic)
                    {
                        OPTICK_EVENT("PT EntityFeed Capture Active Emissive Stage");
                        const bool activeEmissiveStage =
                            EntityFeedActiveEmissiveStageCached(materialCache, viewDef, entity, material);
                        surfaceClassAndFlags = residentRecord->surfaceClassId |
                            (activeEmissiveStage ? 0u : RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF);
                        residentRecord->activeEmissiveStage = activeEmissiveStage;
                        residentRecord->surfaceClassAndFlags = surfaceClassAndFlags;
                    }
                    if (!candidateInstanceIds.insert(rigidSnapshot.instanceId).second)
                    {
                        continue;
                    }

                    float distance = 0.0f;
                    {
                        OPTICK_EVENT("PT EntityFeed Capture Distance");
                        distance = EntityFeedEntityDistance(entity, viewOrigin);
                    }
                    if (maxDistance > 0.0f && distance > maxDistance)
                    {
                        ++stats.droppedBudget;
                        continue;
                    }

                    {
                        OPTICK_EVENT("PT EntityFeed Capture Surface Snapshot");
                        EntityFeedCapturedRigidSurface captured;
                        captured.meshKey = residentRecord->meshKey;
                        captured.rigidSnapshot = rigidSnapshot;
                        captured.entity = entity;
                        captured.baseMaterial = residentRecord->remappedMaterial;
                        captured.surfaceClassId = residentRecord->surfaceClassId;
                        captured.surfaceClassAndFlags = surfaceClassAndFlags;
                        captured.currentArea = areaIndex;
                        memcpy(captured.objectToWorld, entity->modelMatrix, sizeof(captured.objectToWorld));
                        captured.distance = distance;
                        captured.projectedSize = EntityFeedProjectedSizeProxy(entity, distance);
                        captured.onScreen = entity->viewCount == tr.viewCount;
                        captured.emissive = residentRecord->emissive;
                        captured.materialName = residentRecord->materialName;
                        captured.modelName = residentRecord->modelName;
                        capturedSurfaces.push_back(captured);
                    }
                    continue;
                }

                if (residentBaseHit)
                {
                    ++stats.residencyCacheMisses;
                    AddDerivedSurface();
                }

                uint32_t baseMaterialId = 0;
                uint32_t materialId = 0;
                {
                    OPTICK_EVENT("PT EntityFeed Capture Material Id");
                    baseMaterialId = SmokeMaterialId(material);
                    materialId = SmokeRuntimeMaterialTableIdForEntitySurface(entity, surfaceIndex, material, baseMaterialId);
                }
                if (registeredMaterialIds.insert(materialId).second)
                {
                    if (materialId == baseMaterialId)
                    {
                        OPTICK_EVENT("PT EntityFeed Capture Register Material");
                        RegisterSmokeMaterialTextureInfo(material);
                    }
                }
                uint32_t materialClassSignature = 0;
                {
                    OPTICK_EVENT("PT EntityFeed Capture Material Class Signature");
                    materialClassSignature = EntityFeedMaterialRouteClassSignatureCached(materialCache, material);
                }
                const uint32_t surfaceClassId = SmokeSurfaceClassId(RtSmokeSurfaceClass::RigidEntity);
                bool activeEmissiveStage = false;
                {
                    OPTICK_EVENT("PT EntityFeed Capture Active Emissive Stage");
                    activeEmissiveStage = EntityFeedActiveEmissiveStageCached(materialCache, viewDef, entity, material);
                }
                const uint32_t surfaceClassAndFlags = surfaceClassId |
                    (activeEmissiveStage ? 0u : RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF);
                const bool activeEmissiveStageDynamic = EntityFeedActiveEmissiveStageIsDynamic(material);
                RtPathTraceMeshKey meshKey;
                meshKey.tri = tri;
                meshKey.vertexBufferIdentity = static_cast<uintptr_t>(tri->ambientCache);
                meshKey.indexBufferIdentity = static_cast<uintptr_t>(tri->indexCache);
                meshKey.numVerts = tri->numVerts;
                meshKey.numIndexes = tri->numIndexes;
                meshKey.vertexFormat = static_cast<uint32_t>(RtSmokeGeometryBufferFormat::LegacySmokeVertex);
                meshKey.materialId = materialId;
                meshKey.materialClassSignature = materialClassSignature;
                meshKey.sourceKind = surfaceClassId;

                PtRenderDefKey renderDefKey = entityRenderDefKey;
                uint32_t modelEpoch = entityModelEpoch;
                if (!residentEntitySlot)
                {
                    renderDefKey = PtGeometryLifecycle::MakeEntityKey(entity);
                    modelEpoch = PtGeometryLifecycle::EntityModelEpoch(renderDefKey.world, renderDefKey.index);
                }
                uint32_t sourceFlags = RT_PT_INSTANCE_SOURCE_RIGID | RT_PT_INSTANCE_SOURCE_ENTITY_FEED;
                if (renderEntity.customShader != nullptr || renderEntity.customSkin != nullptr)
                {
                    sourceFlags |= RT_PT_INSTANCE_SOURCE_MATERIAL_OVERRIDE;
                }

                RtPathTraceRigidInstanceSnapshot rigidSnapshot;
                {
                    OPTICK_EVENT("PT EntityFeed Capture Rigid Identity");
                    rigidSnapshot = BuildPathTraceRigidInstanceSnapshot(
                        meshKey,
                        model,
                        tri,
                        renderDefKey,
                        modelEpoch,
                        entity->index,
                        renderEntity.entityNum,
                        surfaceIndex,
                        sourceFlags);
                }
                if (!candidateInstanceIds.insert(rigidSnapshot.instanceId).second)
                {
                    continue;
                }

                float distance = 0.0f;
                {
                    OPTICK_EVENT("PT EntityFeed Capture Distance");
                    distance = EntityFeedEntityDistance(entity, viewOrigin);
                }
                if (maxDistance > 0.0f && distance > maxDistance)
                {
                    ++stats.droppedBudget;
                    continue;
                }

                {
                    OPTICK_EVENT("PT EntityFeed Capture Surface Snapshot");
                    EntityFeedCapturedRigidSurface captured;
                    captured.meshKey = meshKey;
                    captured.rigidSnapshot = rigidSnapshot;
                    captured.entity = entity;
                    captured.baseMaterial = material;
                    captured.surfaceClassId = surfaceClassId;
                    captured.surfaceClassAndFlags = surfaceClassAndFlags;
                    captured.currentArea = areaIndex;
                    memcpy(captured.objectToWorld, entity->modelMatrix, sizeof(captured.objectToWorld));
                    captured.distance = distance;
                    captured.projectedSize = EntityFeedProjectedSizeProxy(entity, distance);
                    captured.onScreen = entity->viewCount == tr.viewCount;
                    {
                        OPTICK_EVENT("PT EntityFeed Capture Material Facts");
                        captured.emissive = EntityFeedMaterialIsEmissiveCached(materialCache, materialId);
                    }
                    if (residentRecord)
                    {
                        UpdateEntityFeedResidentSurfaceRoute(
                            *residentRecord,
                            baseMaterialId,
                            materialId,
                            materialClassSignature,
                            surfaceClassId,
                            surfaceClassAndFlags,
                            activeEmissiveStage,
                            activeEmissiveStageDynamic,
                            meshKey,
                            rigidSnapshot,
                            captured.emissive);
                    }
                    captured.materialName = material ? material->GetName() : "<none>";
                    captured.modelName = model ? model->Name() : "<none>";
                    capturedSurfaces.push_back(captured);
                }
            }
        }
        const int areaVisitMs = Sys_Milliseconds() - areaVisitStartMs;
        if (beyondViewArea)
        {
            stats.residencyBeyondViewVisitMs += areaVisitMs;
        }
        else
        {
            stats.residencyPlayerAreaVisitMs += areaVisitMs;
        }
    }

    if (residentStore && r_pathTracingResidencyDump.GetInteger() != 0)
    {
        stats.residencyResidentRecords = CountEntityFeedResidentRecords(*residentStore);
    }
    return capturedSurfaces;
}

void RecordEntityFeedRigidCandidate(
    const EntityFeedRigidCandidate& candidate,
    RtSmokeGeometryUniverse& geometryUniverse,
    RtPathTraceInstanceUniverse& instanceUniverse)
{
    instanceUniverse.RecordObservation(
        candidate.meshObservation,
        candidate.instanceObservation,
        RtSmokeSurfaceClass::RigidEntity,
        candidate.meshKey.numVerts,
        candidate.meshKey.numIndexes);
    geometryUniverse.RecordRigidMeshCandidate(candidate.candidateObservation);
}

}

void DumpEntityFeedStats(const RtPathTraceEntityFeedStats& s)
{
    common->Printf(
        "PathTracePrimaryPass: PT entityFeed frame=%d reachableAreas=%d candidatesS0=%d candidatesS1=%d candidatesS2=%d candidatesS3=%d candidatesS4=%d admitted=%d droppedBudget=%d\n",
        s.frameIndex,
        s.reachableAreas,
        s.candidatesS0,
        s.candidatesS1,
        s.candidatesS2,
        s.candidatesS3,
        s.candidatesS4,
        s.admitted,
        s.droppedBudget);
}

void DumpEntityFeedResidencyStatsIfNeeded(const RtPathTraceEntityFeedStats& s)
{
    if (r_pathTracingResidencyDump.GetInteger() == 0)
    {
        return;
    }

    static int lastDumpFrame = -120;
    if (s.frameIndex - lastDumpFrame < 120)
    {
        return;
    }
    lastDumpFrame = s.frameIndex;
    common->Printf(
        "PathTracePrimaryPass: RES entityFeed visited=%d derived=%d hits=%d misses=%d residents=%d evicted=%d player(areas/refs/visited/derived/ms)=%d/%d/%d/%d/%d beyond(areas/refs/visited/derived/ms)=%d/%d/%d/%d/%d\n",
        s.residencyVisited,
        s.residencyDerived,
        s.residencyCacheHits,
        s.residencyCacheMisses,
        s.residencyResidentRecords,
        s.residencyEvictedRecords,
        s.residencyPlayerAreaAreas,
        s.residencyPlayerAreaRefs,
        s.residencyPlayerAreaVisited,
        s.residencyPlayerAreaDerived,
        s.residencyPlayerAreaVisitMs,
        s.residencyBeyondViewAreas,
        s.residencyBeyondViewRefs,
        s.residencyBeyondViewVisited,
        s.residencyBeyondViewDerived,
        s.residencyBeyondViewVisitMs);
}

namespace {

EntityFeedReachableAreaSet BuildEntityFeedReachableAreaSet(const viewDef_t* viewDef, int maxDepth, float maxDistance)
{
    EntityFeedReachableAreaSet result;
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        return result;
    }

    const int areaCount = renderWorld->NumAreas();
    if (areaCount <= 0)
    {
        return result;
    }

    int seedArea = renderWorld->PointInArea(viewDef->renderView.vieworg);
    if (seedArea < 0)
    {
        seedArea = viewDef->areaNum;
    }
    if (seedArea < 0 || seedArea >= areaCount)
    {
        return result;
    }

    maxDepth = idMath::ClampInt(0, 128, maxDepth);
    result.reachableAreas.assign(areaCount, false);
    result.areaDepth.assign(areaCount, -1);
    std::vector<int> queue;
    queue.reserve(areaCount);

    result.reachableAreas[seedArea] = true;
    result.areaDepth[seedArea] = 0;
    queue.push_back(seedArea);

    const idVec3& viewOrigin = viewDef->renderView.vieworg;
    for (size_t queueIndex = 0; queueIndex < queue.size(); ++queueIndex)
    {
        const int area = queue[queueIndex];
        const int depth = result.areaDepth[area];
        if (depth >= maxDepth)
        {
            continue;
        }

        const int portalCount = renderWorld->NumPortalsInArea(area);
        for (int portalIndex = 0; portalIndex < portalCount; ++portalIndex)
        {
            const exitPortal_t portal = renderWorld->GetPortal(area, portalIndex);
            int nextArea = -1;
            if (portal.areas[0] == area)
            {
                nextArea = portal.areas[1];
            }
            else if (portal.areas[1] == area)
            {
                nextArea = portal.areas[0];
            }
            if (nextArea < 0 || nextArea >= areaCount || result.reachableAreas[nextArea])
            {
                continue;
            }
            if (!renderWorld->AreasAreConnected(area, nextArea, PS_BLOCK_VIEW))
            {
                continue;
            }

            const portalArea_t& nextPortalArea = renderWorld->portalAreas[nextArea];
            if (maxDistance > 0.0f && !nextPortalArea.globalBounds.IsCleared())
            {
                const float areaDistanceSqr = (nextPortalArea.globalBounds.GetCenter() - viewOrigin).LengthSqr();
                if (areaDistanceSqr > maxDistance * maxDistance)
                {
                    continue;
                }
            }

            result.reachableAreas[nextArea] = true;
            result.areaDepth[nextArea] = depth + 1;
            queue.push_back(nextArea);
        }
    }

    return result;
}

}

std::vector<bool> BuildEntityFeedReachableAreas(const viewDef_t* viewDef, int maxDepth, float maxDistance)
{
    return BuildEntityFeedReachableAreaSet(viewDef, maxDepth, maxDistance).reachableAreas;
}

void DumpEntityFeedSingleBoneDiagnostics(const viewDef_t* viewDef)
{
    static int lastDumpFrame = -120;
    if (r_pathTracingEntityFeedDump.GetInteger() == 0)
    {
        return;
    }

    const int frameIndex = tr.frameCount;
    if (frameIndex - lastDumpFrame < 120)
    {
        return;
    }
    lastDumpFrame = frameIndex;

    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        return;
    }

    for (int entityIndex = 0; entityIndex < renderWorld->entityDefs.Num(); ++entityIndex)
    {
        const idRenderEntityLocal* entity = renderWorld->entityDefs[entityIndex];
        idRenderModel* sourceModel = entity ? entity->parms.hModel : nullptr;
        if (!entity || !sourceModel || !EntityFeedModelHasJointData(entity, sourceModel))
        {
            continue;
        }

        idRenderModel* temporaryModel = nullptr;
        const idRenderModel* diagnosticModel = sourceModel;
        if (sourceModel->IsDynamicModel() != DM_STATIC)
        {
            temporaryModel = sourceModel->InstantiateDynamicModel(&entity->parms, viewDef, nullptr);
            diagnosticModel = temporaryModel;
        }

        if (!diagnosticModel)
        {
            continue;
        }

        const int surfaceCount = diagnosticModel->NumSurfaces();
        for (int surfaceIndex = 0; surfaceIndex < surfaceCount; ++surfaceIndex)
        {
            const modelSurface_t* surface = diagnosticModel->Surface(surfaceIndex);
            const srfTriangles_t* tri = surface ? surface->geometry : nullptr;
            common->Printf(
                "PathTracePrimaryPass: PT entityFeed md5 entity=%d model='%s' surface=%d vertCount=%d singleBone=%d\n",
                entityIndex,
                sourceModel->Name(),
                surfaceIndex,
                tri ? tri->numVerts : 0,
                IsEntityFeedSingleBoneSurface(tri) ? 1 : 0);
        }

        if (temporaryModel && temporaryModel != sourceModel)
        {
            delete temporaryModel;
        }
    }
}

void DumpEntityFeedJointAdvanceProbe(const viewDef_t* viewDef)
{
    if (r_pathTracingEntityFeedDump.GetInteger() == 0)
    {
        return;
    }

    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        return;
    }

    static std::unordered_map<int, unsigned int> previousJointHashes;
    int printed = 0;
    for (int entityIndex = 0; entityIndex < renderWorld->entityDefs.Num() && printed < 4; ++entityIndex)
    {
        const idRenderEntityLocal* entity = renderWorld->entityDefs[entityIndex];
        const idRenderModel* model = entity ? entity->parms.hModel : nullptr;
        if (!entity || !model || !EntityFeedModelHasJointData(entity, model))
        {
            continue;
        }

        const unsigned int jointHash = HashEntityFeedJoints(entity->parms);
        const auto previousHash = previousJointHashes.find(entityIndex);
        const bool jointHashChanged = previousHash != previousJointHashes.end() && previousHash->second != jointHash;
        previousJointHashes[entityIndex] = jointHash;

        common->Printf(
            "PathTracePrimaryPass: PT entityFeed jointProbe entity=%d model='%s' jointHashChangedThisFrame=%d onScreenThisFrame=%d\n",
            entityIndex,
            model->Name(),
            jointHashChanged ? 1 : 0,
            entity->viewCount == tr.viewCount ? 1 : 0);
        ++printed;
    }
}

void DumpEntityFeedReachableCandidateStats(const viewDef_t* viewDef)
{
    if (r_pathTracingEntityFeedDump.GetInteger() == 0)
    {
        return;
    }

    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        return;
    }

    const std::vector<bool> reachableAreas = BuildEntityFeedReachableAreas(
        viewDef,
        r_pathTracingEntityFeedMaxDepth.GetInteger(),
        r_pathTracingEntityFeedMaxDistance.GetFloat());
    if (reachableAreas.empty())
    {
        return;
    }

    RtPathTraceEntityFeedStats stats;
    stats.frameIndex = tr.frameCount;
    std::unordered_set<int> visitedEntities;
    for (int areaIndex = 0; areaIndex < static_cast<int>(reachableAreas.size()); ++areaIndex)
    {
        if (!reachableAreas[areaIndex])
        {
            continue;
        }
        ++stats.reachableAreas;
        if (areaIndex < 0 || areaIndex >= renderWorld->numPortalAreas)
        {
            continue;
        }

        portalArea_t* area = &renderWorld->portalAreas[areaIndex];
        for (areaReference_t* ref = area->entityRefs.areaNext; ref != &area->entityRefs; ref = ref->areaNext)
        {
            idRenderEntityLocal* entity = ref ? ref->entity : nullptr;
            const int entityIndex = entity ? entity->index : -1;
            if (!entity || entityIndex < 0 || !visitedEntities.insert(entityIndex).second)
            {
                continue;
            }

            idRenderModel* sourceModel = entity->parms.hModel;
            if (!sourceModel)
            {
                ++stats.candidatesS4;
                continue;
            }

            idRenderModel* temporaryModel = nullptr;
            const idRenderModel* diagnosticModel = sourceModel;
            if (sourceModel->IsDynamicModel() != DM_STATIC && EntityFeedModelHasJointData(entity, sourceModel))
            {
                temporaryModel = sourceModel->InstantiateDynamicModel(&entity->parms, viewDef, nullptr);
                diagnosticModel = temporaryModel;
            }

            const int surfaceCount = diagnosticModel ? diagnosticModel->NumSurfaces() : 0;
            if (surfaceCount <= 0)
            {
                AccumulateEntityFeedSurfaceClass(stats, ClassifyEntityFeedSurface(entity, sourceModel, nullptr));
            }
            for (int surfaceIndex = 0; surfaceIndex < surfaceCount; ++surfaceIndex)
            {
                const modelSurface_t* surface = diagnosticModel->Surface(surfaceIndex);
                AccumulateEntityFeedSurfaceClass(stats, ClassifyEntityFeedSurface(entity, sourceModel, surface));
            }

            if (temporaryModel && temporaryModel != sourceModel)
            {
                delete temporaryModel;
            }
        }
    }

    DumpEntityFeedStats(stats);
}

void ProduceEntityFeedRigidEntities(const viewDef_t* viewDef, RtSmokeGeometryUniverse& geometryUniverse, RtPathTraceInstanceUniverse& instanceUniverse, RtSmokeMaterialStats& materialStats)
{
    OPTICK_EVENT("PT EntityFeed Produce");
    if (r_pathTracingEntityFeed.GetInteger() == 0)
    {
        return;
    }

    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        return;
    }

    EntityFeedReachableAreaSet reachableAreaSet;
    {
        OPTICK_EVENT("PT EntityFeed Reachable Areas");
        reachableAreaSet = BuildEntityFeedReachableAreaSet(
            viewDef,
            r_pathTracingEntityFeedMaxDepth.GetInteger(),
            r_pathTracingEntityFeedMaxDistance.GetFloat());
    }
    if (reachableAreaSet.reachableAreas.empty())
    {
        return;
    }

    const idVec3& viewOrigin = viewDef->renderView.vieworg;
    const float maxDistance = r_pathTracingEntityFeedMaxDistance.GetFloat();
    const int rigidRouteMaxInstances = idMath::ClampInt(1, 510, r_pathTracingRigidRouteMaxInstances.GetInteger());
    int offscreenBudget = rigidRouteMaxInstances;

    RtPathTraceEntityFeedStats stats;
    stats.frameIndex = tr.frameCount;

    EntityFeedVisibleDrawSurfMap visibleDrawSurfs;
    {
        OPTICK_EVENT("PT EntityFeed Visible DrawSurf Map");
        visibleDrawSurfs = BuildEntityFeedVisibleDrawSurfMap(viewDef);
    }

    const std::vector<EntityFeedCapturedRigidSurface> capturedSurfaces = CaptureEntityFeedRigidSurfaces(
        viewDef,
        renderWorld,
        reachableAreaSet.reachableAreas,
        reachableAreaSet.areaDepth,
        visibleDrawSurfs,
        viewOrigin,
        rigidRouteMaxInstances,
        maxDistance,
        stats);
    const std::vector<EntityFeedRigidCandidate> candidates = ProcessEntityFeedCapturedRigidSurfaces(
        capturedSurfaces,
        rigidRouteMaxInstances);

    {
        OPTICK_EVENT("PT EntityFeed Admit Candidates");
        for (const EntityFeedRigidCandidate& candidate : candidates)
        {
            if (!candidate.onScreen && offscreenBudget <= 0)
            {
                ++stats.droppedBudget;
                continue;
            }

            RecordEntityFeedRigidCandidate(candidate, geometryUniverse, instanceUniverse);
            SceneUniverseAddDynamicMaterialEvalStatsForId(
                materialStats,
                viewDef,
                candidate.instanceObservation.entity,
                candidate.meshObservation.baseMaterial,
                static_cast<int>(candidate.meshKey.numIndexes),
                candidate.candidateObservation.materialId);
            ++stats.admitted;
            if (!candidate.onScreen)
            {
                --offscreenBudget;
            }
        }
    }

    if (r_pathTracingEntityFeedDump.GetInteger() != 0)
    {
        DumpEntityFeedStats(stats);
    }
    DumpEntityFeedResidencyStatsIfNeeded(stats);
}
