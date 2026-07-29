#include "precompiled.h"
#pragma hdrstop

#include "PathTraceGeometryLifecycle.h"
#include "PathTraceCVars.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceGeometryAttributeSurvey.h"
#include "PathTraceGeometryIdentityTransport.h"
#include "PathTraceGeometrySourceRegistry.h"
#include "PathTraceGeometrySourceTransport.h"
#include "../Material.h"
#include "../Model.h"
#include "../Model_local.h"
#include "../RenderCommon.h"
#include "../RenderWorld_local.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

const int PT_GEOMETRY_LIFECYCLE_MAX_EVENT_SAMPLES = 16;
const int PT_GEOMETRY_SHADOW_MAX_DUMP_SAMPLES = 16;
const int PT_GEOMETRY_SOURCE_MAX_DUMP_SAMPLES = 8;
const int PT_GEOMETRY_ATTRIBUTE_SURVEY_MAX_DUMP_SAMPLES = 32;
const std::uint32_t PT_GEOMETRY_SHADOW_VERTEX_FORMAT_ID_DRAW_VERT = 1;

std::atomic<std::uint64_t> g_nextWorldGeneration(1);
std::atomic<std::uint64_t> g_nextHistoryOwnerGeneration(1);
std::atomic<std::uint64_t> g_nextShadowAssetId(1);
std::mutex g_liveWorldRegistriesMutex;
std::unordered_map<const void*, PtGeometryLifecycleWorldRegistry*> g_liveWorldRegistries;

std::uint64_t AllocateNonZeroGeneration(std::atomic<std::uint64_t>& counter)
{
    std::uint64_t value = counter.fetch_add(1, std::memory_order_relaxed);
    if (value == 0)
    {
        value = counter.fetch_add(1, std::memory_order_relaxed);
    }
    return value;
}

struct PtGeometryLifecycleSlotState
{
    std::uint32_t generation = 1;
    std::uint32_t modelEpoch = 1;
    bool alive = false;
    PtGeometryLifecycleClass geometryClass = PtGeometryLifecycleClass::Unknown;
};

struct PtGeometryLifecycleEventSample
{
    PtGeometryLifecycleEventKind eventKind = PtGeometryLifecycleEventKind::Add;
    PtGeometryLifecycleDefKind defKind = PtGeometryLifecycleDefKind::Entity;
    PtRenderDefKey key;
    int entityNum = -1;
    int lastModifiedFrameNum = 0;
    std::uint32_t modelEpoch = 0;
    PtGeometryLifecycleClass geometryClass = PtGeometryLifecycleClass::Unknown;
    bool modelChanged = false;
    std::uintptr_t oldModelIdentity = 0;
    std::uintptr_t newModelIdentity = 0;
};

struct PtGeometryLifecycleStats
{
    int entityAdds = 0;
    int entityUpdates = 0;
    int entityUnchanged = 0;
    int entityFrees = 0;
    int entityModelSwaps = 0;
    int lightAdds = 0;
    int lightUpdates = 0;
    int lightFrees = 0;
    int classWorld = 0;
    int classRigidAtRest = 0;
    int classRigidMoving = 0;
    int classDeforming = 0;
    int classTransient = 0;
    int classUnknown = 0;
    PtGeometryLifecycleEventSample samples[PT_GEOMETRY_LIFECYCLE_MAX_EVENT_SAMPLES];
    int sampleCount = 0;
};

struct PtGeometryShadowAssetRecord
{
    std::uint64_t sourceAssetId = 0;
    std::uint64_t sourceAssetGeneration = 1;
    PtCanonicalMeshSourceDomain sourceDomain = PtCanonicalMeshSourceDomain::Invalid;
    idStr modelName;
};

struct PtGeometryShadowMeshRecord
{
    bool valid = false;
    PtCanonicalMeshKey key;
    std::uint64_t hash = 0;
    std::uint32_t liveReferences = 0;
    bool evictionDeferred = false;
    idStr modelName;
};

struct PtGeometryShadowInstanceRecord
{
    bool valid = false;
    PtCanonicalInstanceKey key;
    std::uint64_t hash = 0;
    PtCanonicalMeshKey meshKey;
    std::uint64_t meshHash = 0;
    std::uint32_t materialId = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t triangleCount = 0;
    int entityNum = -1;
    int lastModifiedFrameNum = 0;
    PtGeometryLifecycleClass geometryClass = PtGeometryLifecycleClass::Unknown;
    float origin[3] = {};
    float axis[9] = {};
    idStr modelName;
    idStr materialName;
};

struct PtGeometryShadowStats
{
    std::uint64_t previousWorldGeneration = 0;
    std::uint64_t worldResets = 0;
    std::uint64_t resetDiscardedLiveInstances = 0;
    std::uint64_t resetDiscardedMeshRecords = 0;
    std::uint64_t added = 0;
    std::uint64_t updated = 0;
    std::uint64_t removed = 0;
    std::uint64_t unchanged = 0;
    std::uint64_t keyCollisions = 0;
    std::uint64_t staleGeneration = 0;
    std::uint64_t missingSource = 0;
    std::uint64_t unsupportedTransient = 0;
    std::uint64_t deferredMeshEviction = 0;
    std::uint64_t frontendDeformingDiscovered = 0;
    std::uint64_t frontendDeformingRefreshed = 0;
    std::uint64_t removedVertices = 0;
    std::uint64_t removedIndexes = 0;
    std::uint64_t removedTriangles = 0;
    std::uint64_t removedStaticInstances = 0;
    std::uint64_t removedRigidInstances = 0;
    std::uint64_t removedSkinnedInstances = 0;
    std::uint64_t sourceAdded = 0;
    std::uint64_t sourceReused = 0;
    std::uint64_t sourceRevised = 0;
    std::uint64_t sourceRejected = 0;
    std::uint64_t sourceSkinnedAdmitted = 0;
    std::uint64_t sourceSkinnedNegative = 0;
    std::uint64_t sourcePayloadCopies = 0;
    std::uint64_t sourceCopiedBytes = 0;
};

PtGeometryLifecycleStats g_lifecycleStats;

bool LifecycleDiagnosticsEnabled()
{
    return r_pathTracingGeometryLifecycle.GetInteger() != 0 ||
        r_pathTracingGeometryLifecycleStage.GetInteger() != 0 ||
        r_pathTracingGeometryLifecycleDump.GetInteger() != 0 ||
        r_pathTracingGeometryShadowRegistryDump.GetInteger() != 0;
}

PtGeometryLifecycleSlotState& EnsureSlot(std::vector<PtGeometryLifecycleSlotState>& slots, int index)
{
    if (index < 0)
    {
        static PtGeometryLifecycleSlotState invalidSlot;
        invalidSlot = PtGeometryLifecycleSlotState();
        return invalidSlot;
    }
    if (index >= static_cast<int>(slots.size()))
    {
        slots.resize(static_cast<size_t>(index + 1));
    }
    return slots[static_cast<size_t>(index)];
}

const PtGeometryLifecycleSlotState* FindSlot(const std::vector<PtGeometryLifecycleSlotState>& slots, int index)
{
    if (index < 0 || index >= static_cast<int>(slots.size()))
    {
        return nullptr;
    }
    return &slots[static_cast<size_t>(index)];
}

void AdvanceSlotGeneration(PtGeometryLifecycleSlotState& slot)
{
    ++slot.generation;
    if (slot.generation == 0)
    {
        slot.generation = 1;
    }
}

void AdvanceSlotModelEpoch(PtGeometryLifecycleSlotState& slot)
{
    ++slot.modelEpoch;
    if (slot.modelEpoch == 0)
    {
        slot.modelEpoch = 1;
    }
}

std::uint64_t HashShadowValue(std::uint64_t hash, std::uint64_t value)
{
    for (int byteIndex = 0; byteIndex < 8; ++byteIndex)
    {
        hash ^= static_cast<std::uint8_t>(value & 0xffu);
        hash *= 1099511628211ull;
        value >>= 8u;
    }
    return hash;
}

std::uint64_t BuildShadowTopologySignature(
    const triIndex_t* indexes,
    int vertexCount,
    int indexCount)
{
    if (!indexes || vertexCount <= 0 || indexCount < 3)
    {
        return 0;
    }

    std::uint64_t hash = 1469598103934665603ull;
    hash = HashShadowValue(hash, static_cast<std::uint32_t>(vertexCount));
    hash = HashShadowValue(hash, static_cast<std::uint32_t>(indexCount));
    for (int index = 0; index < indexCount; ++index)
    {
        hash = HashShadowValue(hash, static_cast<std::uint32_t>(indexes[index]));
    }
    return hash != 0 ? hash : 1;
}

std::uint64_t BuildShadowTopologySignature(const srfTriangles_t* tri)
{
    return tri
        ? BuildShadowTopologySignature(
            tri->indexes,
            tri->numVerts,
            tri->numIndexes)
        : 0;
}

const idMaterial* ResolveShadowSurfaceMaterial(const idRenderEntityLocal* entity, const modelSurface_t* surface)
{
    const idMaterial* shader = surface ? surface->shader : nullptr;
    if (!entity || !shader)
    {
        return shader;
    }
    if (entity->parms.customShader != nullptr)
    {
        return shader->Deform() ? shader : entity->parms.customShader;
    }
    if (entity->parms.customSkin != nullptr)
    {
        shader = entity->parms.customSkin->RemapShaderBySkin(shader);
    }
    return shader;
}

void CopyShadowTransform(const idRenderEntityLocal* entity, float origin[3], float axis[9])
{
    if (!entity)
    {
        std::memset(origin, 0, sizeof(float) * 3);
        std::memset(axis, 0, sizeof(float) * 9);
        return;
    }
    for (int component = 0; component < 3; ++component)
    {
        origin[component] = entity->parms.origin[component];
    }
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            axis[row * 3 + column] = entity->parms.axis[row][column];
        }
    }
}

bool ShadowTransformEquals(const PtGeometryShadowInstanceRecord& record, const idRenderEntityLocal* entity)
{
    float origin[3];
    float axis[9];
    CopyShadowTransform(entity, origin, axis);
    return std::memcmp(record.origin, origin, sizeof(origin)) == 0 &&
        std::memcmp(record.axis, axis, sizeof(axis)) == 0;
}

PtGeometrySourceAttribute BuildShadowSourceAttribute(const idDrawVert& drawVert)
{
    idVec3 normal = drawVert.GetNormal();
    if (normal.Normalize() == 0.0f)
    {
        normal.Set(0.0f, 0.0f, 1.0f);
    }
    idVec3 tangent = drawVert.GetTangent();
    if (tangent.Normalize() == 0.0f)
    {
        tangent.Set(1.0f, 0.0f, 0.0f);
    }
    const float bitangentSign = drawVert.GetBiTangentSign();
    idVec3 bitangent = drawVert.GetBiTangent();
    if (bitangent.Normalize() == 0.0f)
    {
        bitangent.Cross(normal, tangent);
        bitangent *= bitangentSign;
        bitangent.Normalize();
    }
    const idVec2 texCoord = drawVert.GetTexCoord();

    PtGeometrySourceAttribute attribute;
    attribute.normal[0] = normal.x;
    attribute.normal[1] = normal.y;
    attribute.normal[2] = normal.z;
    attribute.texCoord[0] = texCoord.x;
    attribute.texCoord[1] = texCoord.y;
    for (int channel = 0; channel < 4; ++channel)
    {
        attribute.color[channel] =
            drawVert.color[channel] * (1.0f / 255.0f);
        attribute.color2[channel] =
            drawVert.color2[channel] * (1.0f / 255.0f);
    }
    attribute.tangent[0] = tangent.x;
    attribute.tangent[1] = tangent.y;
    attribute.tangent[2] = tangent.z;
    attribute.bitangent[0] = bitangent.x;
    attribute.bitangent[1] = bitangent.y;
    attribute.bitangent[2] = bitangent.z;
    attribute.bitangentSign = bitangentSign;
    return attribute;
}

PtCanonicalMeshSourceDomain ShadowSourceDomain(
    const idRenderEntityLocal* entity,
    const idRenderModel* model,
    bool allowCallbackSource)
{
    if (!entity || !model)
    {
        return PtCanonicalMeshSourceDomain::Invalid;
    }
    // Callback/generated entities may carry a source model pointer whose virtual
    // surface interface is not part of this lifecycle hook's lifetime contract.
    if (entity->parms.callback != nullptr && !allowCallbackSource)
    {
        return PtCanonicalMeshSourceDomain::Invalid;
    }
    if (model->IsStaticWorldModel())
    {
        return PtCanonicalMeshSourceDomain::StaticWorldMap;
    }
    if (model->IsDynamicModel() == DM_CONTINUOUS)
    {
        return PtCanonicalMeshSourceDomain::UnsupportedTransient;
    }
    if (model->IsDynamicModel() == DM_CACHED ||
        entity->parms.joints != nullptr ||
        entity->parms.numJoints > 0)
    {
        return PtCanonicalMeshSourceDomain::SkinnedBindSource;
    }
    return PtCanonicalMeshSourceDomain::RegisteredRenderModel;
}

PtCanonicalDeformationClass ShadowDeformationClass(PtCanonicalMeshSourceDomain sourceDomain)
{
    switch (sourceDomain)
    {
        case PtCanonicalMeshSourceDomain::StaticWorldMap:
            return PtCanonicalDeformationClass::Static;
        case PtCanonicalMeshSourceDomain::RegisteredRenderModel:
            return PtCanonicalDeformationClass::Rigid;
        case PtCanonicalMeshSourceDomain::SkinnedBindSource:
            return PtCanonicalDeformationClass::Skinned;
        default:
            return PtCanonicalDeformationClass::Invalid;
    }
}

PtCanonicalSubInstanceKind ShadowSubInstanceKind(PtCanonicalMeshSourceDomain sourceDomain)
{
    switch (sourceDomain)
    {
        case PtCanonicalMeshSourceDomain::StaticWorldMap:
            return PtCanonicalSubInstanceKind::StaticSurface;
        case PtCanonicalMeshSourceDomain::RegisteredRenderModel:
            return PtCanonicalSubInstanceKind::RigidSurface;
        case PtCanonicalMeshSourceDomain::SkinnedBindSource:
            return PtCanonicalSubInstanceKind::SkinnedSurface;
        default:
            return PtCanonicalSubInstanceKind::Invalid;
    }
}

void AccumulateClass(PtGeometryLifecycleClass geometryClass)
{
    switch (geometryClass)
    {
        case PtGeometryLifecycleClass::World:
            ++g_lifecycleStats.classWorld;
            break;
        case PtGeometryLifecycleClass::RigidAtRest:
            ++g_lifecycleStats.classRigidAtRest;
            break;
        case PtGeometryLifecycleClass::RigidMoving:
            ++g_lifecycleStats.classRigidMoving;
            break;
        case PtGeometryLifecycleClass::Deforming:
            ++g_lifecycleStats.classDeforming;
            break;
        case PtGeometryLifecycleClass::Transient:
            ++g_lifecycleStats.classTransient;
            break;
        default:
            ++g_lifecycleStats.classUnknown;
            break;
    }
}

void AddEventSample(const PtGeometryLifecycleEventSample& sample)
{
    if (!LifecycleDiagnosticsEnabled() ||
        g_lifecycleStats.sampleCount >= PT_GEOMETRY_LIFECYCLE_MAX_EVENT_SAMPLES)
    {
        return;
    }
    g_lifecycleStats.samples[g_lifecycleStats.sampleCount++] = sample;
}

const char* EventName(PtGeometryLifecycleEventKind kind)
{
    switch (kind)
    {
        case PtGeometryLifecycleEventKind::Add:
            return "add";
        case PtGeometryLifecycleEventKind::Update:
            return "update";
        case PtGeometryLifecycleEventKind::Free:
            return "free";
        default:
            return "unknown";
    }
}

const char* DefKindName(PtGeometryLifecycleDefKind kind)
{
    switch (kind)
    {
        case PtGeometryLifecycleDefKind::Entity:
            return "entity";
        case PtGeometryLifecycleDefKind::Light:
            return "light";
        default:
            return "unknown";
    }
}

}

class PtGeometryLifecycleWorldRegistry
{
public:
    PtGeometryLifecycleWorldRegistry()
    {
        worldGeneration = AllocateNonZeroGeneration(g_nextWorldGeneration);
        primaryHistoryOwnerGeneration = AllocateNonZeroGeneration(g_nextHistoryOwnerGeneration);
    }

    void BeginMap(std::uint64_t newMapLoadSerial)
    {
        std::uint64_t liveInstances = 0;
        for (const PtGeometryShadowInstanceRecord& record : instances)
        {
            liveInstances += record.valid ? 1u : 0u;
        }

        PtGeometryShadowStats newStats;
        newStats.previousWorldGeneration = worldGeneration;
        newStats.worldResets = stats.worldResets + 1;
        newStats.resetDiscardedLiveInstances = liveInstances;
        newStats.resetDiscardedMeshRecords = static_cast<std::uint64_t>(meshes.size());
        stats = newStats;

        worldGeneration = AllocateNonZeroGeneration(g_nextWorldGeneration);
        primaryHistoryOwnerGeneration = AllocateNonZeroGeneration(g_nextHistoryOwnerGeneration);
        mapLoadSerial = newMapLoadSerial;
        entitySlots.clear();
        lightSlots.clear();
        assets.clear();
        meshes.clear();
        meshLookup.clear();
        instances.clear();
        instanceLookup.clear();
        sourceRegistry.Clear();
        sourcePublishedRecordCount = 0;
        ++sourcePublicationGeneration;
        if (sourcePublicationGeneration == 0)
        {
            sourcePublicationGeneration = 1;
        }
        sourcePublicationSequence = 0;
        ResetIdentityPublication();
        shadowTracking = false;
    }

    PtRenderDefKey MakeKey(const void* world, int index, bool light) const
    {
        PtRenderDefKey key;
        key.world = world;
        key.worldGeneration = worldGeneration;
        key.index = index;
        const std::vector<PtGeometryLifecycleSlotState>& slots = light ? lightSlots : entitySlots;
        const PtGeometryLifecycleSlotState* slot = FindSlot(slots, index);
        key.generation = slot ? slot->generation : 1u;
        return key;
    }

    void SetShadowTracking(const idRenderWorldLocal* world, bool enabled)
    {
        if (!enabled)
        {
            if (shadowTracking)
            {
                assets.clear();
                meshes.clear();
                meshLookup.clear();
                instances.clear();
                instanceLookup.clear();
                sourceRegistry.Clear();
                sourcePublishedRecordCount = 0;
                ++sourcePublicationGeneration;
                if (sourcePublicationGeneration == 0)
                {
                    sourcePublicationGeneration = 1;
                }
                sourcePublicationSequence = 0;
                ResetIdentityPublication();
                shadowTracking = false;
            }
            return;
        }
        if (shadowTracking || !world)
        {
            return;
        }

        // Do not bootstrap by walking every existing entityDef here. Some
        // fast-path defs intentionally retain opaque model pointers that the
        // renderer does not dereference on an unchanged update. Shadow
        // observation begins with subsequent stable add/update notifications;
        // enabling before mapRestart gives a complete map population.
        shadowTracking = true;
    }

    void ObserveEntity(
        const idRenderEntityLocal* entity,
        PtGeometryLifecycleClass geometryClass,
        bool allowCallbackSource = false,
        const idRenderModel* resolvedSurfaceModel = nullptr)
    {
        if (!shadowTracking || !entity || worldGeneration == 0)
        {
            return;
        }
        if (entity->parms.callback != nullptr && !allowCallbackSource)
        {
            // Callback entities are only surface-safe at the post-R_AddModels
            // frontend boundary. Lifecycle updates preserve any records that
            // boundary discovered; explicit Free remains removal authority.
            return;
        }

        const idRenderModel* sourceModel = entity->parms.hModel;
        const PtCanonicalMeshSourceDomain sourceDomain =
            ShadowSourceDomain(entity, sourceModel, allowCallbackSource);
        if (sourceDomain == PtCanonicalMeshSourceDomain::UnsupportedTransient)
        {
            ++stats.unsupportedTransient;
            RemoveUnobservedEntitySurfaces(entity, std::vector<PtCanonicalInstanceKey>());
            return;
        }
        if (sourceDomain == PtCanonicalMeshSourceDomain::Invalid || !sourceModel)
        {
            ++stats.missingSource;
            RemoveUnobservedEntitySurfaces(entity, std::vector<PtCanonicalInstanceKey>());
            return;
        }
        if (sourceDomain == PtCanonicalMeshSourceDomain::SkinnedBindSource &&
            resolvedSurfaceModel == nullptr)
        {
            // MD5/other cached source models do not own their instantiated
            // render surfaces. Add/update hooks own liveness only; the
            // post-R_AddModels frontend observation owns surface discovery.
            return;
        }

        const idRenderModel* surfaceModel =
            resolvedSurfaceModel != nullptr ? resolvedSurfaceModel : sourceModel;
        PtGeometryLifecycleSlotState& slot = EnsureSlot(entitySlots, entity->index);
        slot.alive = true;
        const PtGeometryShadowAssetRecord& asset = FindOrCreateAsset(sourceModel, sourceDomain);
        std::vector<PtCanonicalInstanceKey> observedKeys;
        if (surfaceModel->NumSurfaces() > 0)
        {
            observedKeys.reserve(static_cast<size_t>(surfaceModel->NumSurfaces()));
        }

        for (int surfaceIndex = 0; surfaceIndex < surfaceModel->NumSurfaces(); ++surfaceIndex)
        {
            const modelSurface_t* surface = surfaceModel->Surface(surfaceIndex);
            const srfTriangles_t* tri = surface ? surface->geometry : nullptr;
            const std::uint64_t topologySignature = BuildShadowTopologySignature(tri);
            if (!surface || !tri || topologySignature == 0)
            {
                ++stats.missingSource;
                continue;
            }
            const std::uint32_t sourceSurfaceIndex =
                resolvedSurfaceModel != nullptr && surface->id >= 0
                    ? static_cast<std::uint32_t>(surface->id)
                    : static_cast<std::uint32_t>(surfaceIndex);

            PtCanonicalMeshKey meshKey;
            meshKey.sourceAssetId = asset.sourceAssetId;
            meshKey.sourceAssetGeneration = asset.sourceAssetGeneration;
            meshKey.topologySignature = topologySignature;
            meshKey.sourceDomain = sourceDomain;
            meshKey.modelSurfaceIndex = sourceSurfaceIndex;
            meshKey.vertexFormat = PT_GEOMETRY_SHADOW_VERTEX_FORMAT_ID_DRAW_VERT;
            meshKey.deformationClass = ShadowDeformationClass(sourceDomain);
            meshKey.vertexCount = static_cast<std::uint32_t>(tri->numVerts);
            meshKey.indexCount = static_cast<std::uint32_t>(tri->numIndexes);
            if (!PtCanonicalMeshKeyIsValid(meshKey))
            {
                ++stats.missingSource;
                continue;
            }

            PtCanonicalInstanceKey instanceKey;
            instanceKey.worldGeneration = worldGeneration;
            instanceKey.renderDefIndex = static_cast<std::uint32_t>(entity->index);
            instanceKey.renderDefGeneration = slot.generation;
            instanceKey.subInstanceKind = ShadowSubInstanceKind(sourceDomain);
            instanceKey.modelSurfaceIndex = sourceSurfaceIndex;
            if (!PtCanonicalInstanceKeyIsValid(instanceKey))
            {
                ++stats.missingSource;
                continue;
            }
            observedKeys.push_back(instanceKey);

            const idMaterial* material = ResolveShadowSurfaceMaterial(entity, surface);
            ObserveImmutableSource(
                meshKey,
                asset.sourceAssetGeneration,
                tri,
                sourceSurfaceIndex,
                sourceModel);
            TouchInstance(
                entity,
                geometryClass,
                sourceModel,
                material,
                meshKey,
                instanceKey,
                static_cast<std::uint32_t>(tri->numVerts),
                static_cast<std::uint32_t>(tri->numIndexes));
        }
        RemoveUnobservedEntitySurfaces(entity, observedKeys);
    }

    void ObserveFrontendDeformingEntity(
        const idRenderEntityLocal* entity,
        const idRenderModel* resolvedSurfaceModel)
    {
        if (!shadowTracking || !entity || !resolvedSurfaceModel || worldGeneration == 0)
        {
            return;
        }

        const PtGeometryLifecycleSlotState* slot = FindSlot(entitySlots, entity->index);
        if (!slot || !slot->alive)
        {
            return;
        }

        bool foundExisting = false;
        for (PtGeometryShadowInstanceRecord& record : instances)
        {
            if (!record.valid ||
                record.key.worldGeneration != worldGeneration ||
                record.key.renderDefIndex != static_cast<std::uint32_t>(entity->index) ||
                record.key.renderDefGeneration != slot->generation ||
                record.key.subInstanceKind != PtCanonicalSubInstanceKind::SkinnedSurface)
            {
                continue;
            }

            foundExisting = true;
            record.entityNum = entity->parms.entityNum;
            record.lastModifiedFrameNum = entity->lastModifiedFrameNum;
            record.geometryClass = PtGeometryLifecycleClass::Deforming;
            CopyShadowTransform(entity, record.origin, record.axis);
        }

        if (foundExisting)
        {
            ++stats.frontendDeformingRefreshed;
            return;
        }

        const std::uint64_t addedBefore = stats.added;
        ObserveEntity(
            entity,
            PtGeometryLifecycleClass::Deforming,
            true,
            resolvedSurfaceModel);
        if (stats.added > addedBefore)
        {
            ++stats.frontendDeformingDiscovered;
        }
    }

    void CaptureSourceDelta(viewDef_t* viewDef)
    {
        if (viewDef == nullptr || viewDef->isSubview || !shadowTracking ||
            worldGeneration == 0)
        {
            return;
        }

        const int budgetMB = idMath::ClampInt(
            1,
            32,
            r_pathTracingGeometrySourceDeltaBudgetMB.GetInteger());
        const std::uint64_t byteBudget =
            static_cast<std::uint64_t>(budgetMB) * 1024ull * 1024ull;
        PtGeometrySourceTransportPlan plan;
        const PtGeometrySourceTransportResult result =
            PtPlanGeometrySourceTransport(
                sourceRegistry,
                sourcePublishedRecordCount,
                byteBudget,
                plan);
        if (result == PtGeometrySourceTransportResult::EmptyDelta)
        {
            return;
        }
        if (result != PtGeometrySourceTransportResult::Success ||
            plan.records.empty() ||
            plan.packedBytes > static_cast<std::uint64_t>(
                std::numeric_limits<int>::max()))
        {
            common->Printf(
                "PathTracePrimaryPass: GEO06 source transport producer rejected result=%s cursor=%llu records=%llu budgetBytes=%llu\n",
                PtGeometrySourceTransportResultName(result),
                static_cast<unsigned long long>(sourcePublishedRecordCount),
                static_cast<unsigned long long>(sourceRegistry.RecordCount()),
                static_cast<unsigned long long>(byteBudget));
            return;
        }

        PtGeometrySourceTransportSnapshot* snapshot =
            new (R_ClearedFrameAlloc(
                sizeof(PtGeometrySourceTransportSnapshot),
                FRAME_ALLOC_VIEW_DEF))
                PtGeometrySourceTransportSnapshot();
        snapshot->worldGeneration = worldGeneration;
        snapshot->publicationGeneration = sourcePublicationGeneration;
        snapshot->publicationSequence = ++sourcePublicationSequence;
        snapshot->firstRecordIndex = plan.firstRecordIndex;
        snapshot->nextRecordIndex = plan.nextRecordIndex;
        snapshot->packedBytes = plan.packedBytes;
        snapshot->recordCount = plan.records.size();
        snapshot->streams.positionCount = plan.positionCount;
        snapshot->streams.attributeCount = plan.attributeCount;
        snapshot->streams.indexCount = plan.indexCount;
        snapshot->streams.triangleCount = plan.triangleCount;

        PtGeometrySourceTransportRecord* records =
            static_cast<PtGeometrySourceTransportRecord*>(R_FrameAlloc(
                static_cast<int>(
                    plan.records.size() *
                    sizeof(PtGeometrySourceTransportRecord)),
                FRAME_ALLOC_VIEW_ENTITY));
        PtGeometrySourcePosition* positions =
            static_cast<PtGeometrySourcePosition*>(R_FrameAlloc(
                static_cast<int>(
                    plan.positionCount *
                    sizeof(PtGeometrySourcePosition)),
                FRAME_ALLOC_VIEW_ENTITY));
        PtGeometrySourceAttribute* attributes =
            static_cast<PtGeometrySourceAttribute*>(R_FrameAlloc(
                static_cast<int>(
                    plan.attributeCount *
                    sizeof(PtGeometrySourceAttribute)),
                FRAME_ALLOC_VIEW_ENTITY));
        std::uint32_t* indexes =
            static_cast<std::uint32_t*>(R_FrameAlloc(
                static_cast<int>(
                    plan.indexCount * sizeof(std::uint32_t)),
                FRAME_ALLOC_VIEW_ENTITY));
        PtGeometrySourceTriangle* triangles =
            static_cast<PtGeometrySourceTriangle*>(R_FrameAlloc(
                static_cast<int>(
                    plan.triangleCount *
                    sizeof(PtGeometrySourceTriangle)),
                FRAME_ALLOC_VIEW_ENTITY));

        std::memcpy(
            records,
            plan.records.data(),
            plan.records.size() * sizeof(plan.records[0]));
        for (std::size_t localRecordIndex = 0;
            localRecordIndex < plan.records.size();
            ++localRecordIndex)
        {
            const PtGeometrySourceRecord* source = sourceRegistry.RecordAt(
                plan.firstRecordIndex + localRecordIndex);
            const PtGeometrySourceTransportRecord& transport =
                plan.records[localRecordIndex];
            if (source == nullptr)
            {
                continue;
            }
            std::memcpy(
                positions + transport.positionOffset,
                source->payload.positions.data(),
                source->payload.positions.size() *
                    sizeof(PtGeometrySourcePosition));
            source->payload.CopyDecodedAttributes(
                attributes + transport.attributeOffset,
                source->payload.AttributeCount());
            std::memcpy(
                indexes + transport.indexOffset,
                source->payload.indexes.data(),
                source->payload.indexes.size() * sizeof(std::uint32_t));
            std::memcpy(
                triangles + transport.triangleOffset,
                source->payload.triangles.data(),
                source->payload.triangles.size() *
                    sizeof(PtGeometrySourceTriangle));
        }

        snapshot->records = records;
        snapshot->streams.positions = positions;
        snapshot->streams.attributes = attributes;
        snapshot->streams.indexes = indexes;
        snapshot->streams.triangles = triangles;
        viewDef->pathTraceGeometrySourceSnapshot = snapshot;
        sourcePublishedRecordCount = plan.nextRecordIndex;
    }

    void CaptureIdentityDelta(viewDef_t* viewDef)
    {
        if (viewDef == nullptr || viewDef->isSubview || !shadowTracking ||
            worldGeneration == 0)
        {
            return;
        }

        MaybeCompactIdentityJournal();
        const int budgetMB = idMath::ClampInt(
            1,
            32,
            r_pathTracingGeometrySourceDeltaBudgetMB.GetInteger());
        const std::uint64_t byteBudget =
            static_cast<std::uint64_t>(budgetMB) * 1024ull * 1024ull;
        PtGeometryIdentityTransportPlan plan;
        const PtGeometryIdentityTransportResult result =
            PtPlanGeometryIdentityTransport(
                identityJournal,
                identityPublishedRecordCount,
                byteBudget,
                plan);
        if (result == PtGeometryIdentityTransportResult::EmptyDelta)
        {
            return;
        }
        if (result != PtGeometryIdentityTransportResult::Success ||
            plan.records.empty() ||
            plan.packedBytes > static_cast<std::uint64_t>(
                std::numeric_limits<int>::max()))
        {
            common->Printf(
                "PathTracePrimaryPass: GEO06 identity transport producer rejected result=%s cursor=%llu records=%llu budgetBytes=%llu\n",
                PtGeometryIdentityTransportResultName(result),
                static_cast<unsigned long long>(
                    identityPublishedRecordCount),
                static_cast<unsigned long long>(identityJournal.size()),
                static_cast<unsigned long long>(byteBudget));
            return;
        }

        PtGeometryIdentityTransportSnapshot* snapshot =
            new (R_ClearedFrameAlloc(
                sizeof(PtGeometryIdentityTransportSnapshot),
                FRAME_ALLOC_VIEW_DEF))
                PtGeometryIdentityTransportSnapshot();
        snapshot->worldGeneration = worldGeneration;
        snapshot->publicationGeneration = identityPublicationGeneration;
        snapshot->publicationSequence = ++identityPublicationSequence;
        snapshot->firstRecordIndex = plan.firstRecordIndex;
        snapshot->nextRecordIndex = plan.nextRecordIndex;
        snapshot->packedBytes = plan.packedBytes;
        snapshot->recordCount = plan.records.size();

        PtGeometryIdentityTransportRecord* records =
            static_cast<PtGeometryIdentityTransportRecord*>(R_FrameAlloc(
                static_cast<int>(
                    plan.records.size() *
                    sizeof(PtGeometryIdentityTransportRecord)),
                FRAME_ALLOC_VIEW_ENTITY));
        std::memcpy(
            records,
            plan.records.data(),
            plan.records.size() * sizeof(plan.records[0]));
        snapshot->records = records;
        viewDef->pathTraceGeometryIdentitySnapshot = snapshot;
        identityPublishedRecordCount = plan.nextRecordIndex;
    }

    void RemoveEntity(const idRenderEntityLocal* entity, std::uint32_t generation)
    {
        if (!shadowTracking || !entity)
        {
            return;
        }

        bool removedAny = false;
        for (PtGeometryShadowInstanceRecord& record : instances)
        {
            if (!record.valid ||
                record.key.worldGeneration != worldGeneration ||
                record.key.renderDefIndex != static_cast<std::uint32_t>(entity->index) ||
                record.key.renderDefGeneration != generation)
            {
                continue;
            }
            RemoveInstance(record);
            removedAny = true;
        }
        (void)removedAny;
    }

    void DumpAttributeSurvey(std::uint64_t frameIndex, int requestedPage)
    {
        PtGeometryAttributeSurvey survey;
        PtSurveyGeometrySourceRegistry(sourceRegistry, survey);
        const PtGeometryAttributeSurveyStats& totals = survey.totals;
        const std::uint64_t vertexCount = totals.vertexCount;
        std::uint64_t packedColorRecords = 0;
        std::uint64_t fullColorRecords = 0;
        std::uint64_t storedAttributeBytes = 0;
        std::uint64_t fallbackNonFinite = 0;
        std::uint64_t fallbackOutOfRange = 0;
        std::uint64_t fallbackNonExact = 0;
        for (std::size_t recordIndex = 0;
            recordIndex < sourceRegistry.RecordCount();
            ++recordIndex)
        {
            const PtGeometrySourceRecord* source =
                sourceRegistry.RecordAt(recordIndex);
            if (source == nullptr)
            {
                continue;
            }
            storedAttributeBytes +=
                source->payload.StoredAttributeBytes();
            if (source->payload.attributeEncoding ==
                PtGeometrySourceAttributeEncoding::ColorUnorm8)
            {
                ++packedColorRecords;
            }
            else
            {
                ++fullColorRecords;
            }
            switch (source->colorUnorm8FallbackReason)
            {
                case PtGeometrySourceColorUnorm8FallbackReason::NonFinite:
                    ++fallbackNonFinite;
                    break;
                case PtGeometrySourceColorUnorm8FallbackReason::OutOfRange:
                    ++fallbackOutOfRange;
                    break;
                case PtGeometrySourceColorUnorm8FallbackReason::NonExact:
                    ++fallbackNonExact;
                    break;
                default:
                    break;
            }
        }
        const std::uint64_t halfUvSavings = vertexCount * 4ull;
        const std::uint64_t octNormalSavings = vertexCount * 8ull;
        const std::uint64_t octTangentSavings = vertexCount * 8ull;
        const std::uint64_t deriveBitangentSavings = vertexCount * 12ull;
        const std::uint64_t unormColorSavings = vertexCount * 12ull;
        const std::uint64_t unormColor2Savings = vertexCount * 12ull;
        const std::size_t pageSize = static_cast<std::size_t>(
            PT_GEOMETRY_ATTRIBUTE_SURVEY_MAX_DUMP_SAMPLES);
        const std::size_t pageCount = survey.records.empty()
            ? 1
            : (survey.records.size() + pageSize - 1) / pageSize;
        const std::size_t page = static_cast<std::size_t>(
            idMath::ClampInt(
                1,
                static_cast<int>(pageCount),
                requestedPage > 0 ? requestedPage : 1) -
            1);
        const std::size_t firstRecord = page * pageSize;
        const std::size_t endRecord = Min(
            survey.records.size(),
            firstRecord + pageSize);

        common->Printf(
            "PathTracePrimaryPass: GEO12 attribute survey frame=%llu route=measurement-only page=%llu/%llu emitted=%llu records(total/rigid/skinned)=%llu/%llu/%llu vertices=%llu currentBytes(position/attribute)=%llu/%llu candidateSavingsSeparate(halfUV/octNormal/octTangent/deriveBitangent/unormColor/unormColor2)=%llu/%llu/%llu/%llu/%llu/%llu\n",
            static_cast<unsigned long long>(frameIndex),
            static_cast<unsigned long long>(page + 1),
            static_cast<unsigned long long>(pageCount),
            static_cast<unsigned long long>(endRecord - firstRecord),
            static_cast<unsigned long long>(totals.recordCount),
            static_cast<unsigned long long>(totals.rigidRecordCount),
            static_cast<unsigned long long>(totals.skinnedRecordCount),
            static_cast<unsigned long long>(totals.vertexCount),
            static_cast<unsigned long long>(totals.currentPositionBytes),
            static_cast<unsigned long long>(totals.currentAttributeBytes),
            static_cast<unsigned long long>(halfUvSavings),
            static_cast<unsigned long long>(octNormalSavings),
            static_cast<unsigned long long>(octTangentSavings),
            static_cast<unsigned long long>(deriveBitangentSavings),
            static_cast<unsigned long long>(unormColorSavings),
            static_cast<unsigned long long>(unormColor2Savings));
        common->Printf(
            "PathTracePrimaryPass: GEO12 color storage gate=%d records(packed/full)=%llu/%llu bytes(logical/stored/saved)=%llu/%llu/%llu fallback(nonFinite/outOfRange/nonExact)=%llu/%llu/%llu transport=decoded-full gpuAttributeAbi=full-float\n",
            r_pathTracingGeometrySourceColorUnorm8.GetInteger(),
            static_cast<unsigned long long>(packedColorRecords),
            static_cast<unsigned long long>(fullColorRecords),
            static_cast<unsigned long long>(totals.currentAttributeBytes),
            static_cast<unsigned long long>(storedAttributeBytes),
            static_cast<unsigned long long>(
                totals.currentAttributeBytes >= storedAttributeBytes
                    ? totals.currentAttributeBytes - storedAttributeBytes
                    : 0),
            static_cast<unsigned long long>(fallbackNonFinite),
            static_cast<unsigned long long>(fallbackOutOfRange),
            static_cast<unsigned long long>(fallbackNonExact));
        common->Printf(
            "PathTracePrimaryPass: GEO12 attribute ranges positionMin=(%.9g,%.9g,%.9g) positionMax=(%.9g,%.9g,%.9g) uvMin=(%.9g,%.9g) uvMax=(%.9g,%.9g) nonFinite(position/uv/basis/color)=%llu/%llu/%llu/%llu\n",
            totals.positionMin[0],
            totals.positionMin[1],
            totals.positionMin[2],
            totals.positionMax[0],
            totals.positionMax[1],
            totals.positionMax[2],
            totals.texCoordMin[0],
            totals.texCoordMin[1],
            totals.texCoordMax[0],
            totals.texCoordMax[1],
            static_cast<unsigned long long>(
                totals.nonFinitePositionComponents),
            static_cast<unsigned long long>(
                totals.nonFiniteTexCoordComponents),
            static_cast<unsigned long long>(
                totals.nonFiniteBasisComponents),
            static_cast<unsigned long long>(
                totals.nonFiniteColorComponents));
        common->Printf(
            "PathTracePrimaryPass: GEO12 candidate measurements halfUV(samples/overflow/underflow/maxAbs/maxRelative)=%llu/%llu/%llu/%.9g/%.9g oct16(normalSamples/maxDegrees/tangentSamples/maxDegrees)=%llu/%.9g/%llu/%.9g basis(degenerateN/T/B/reconstructSamples/reconstructInvalid/maxDegrees)=%llu/%llu/%llu/%llu/%llu/%.9g unorm8(colorExact/total/outOfRange/maxAbs/color2Exact/total/outOfRange/maxAbs)=%llu/%llu/%llu/%.9g/%llu/%llu/%llu/%.9g\n",
            static_cast<unsigned long long>(totals.halfTexCoordComponents),
            static_cast<unsigned long long>(
                totals.halfTexCoordOverflowComponents),
            static_cast<unsigned long long>(
                totals.halfTexCoordUnderflowToZeroComponents),
            totals.halfTexCoordMaxAbsError,
            totals.halfTexCoordMaxRelativeError,
            static_cast<unsigned long long>(totals.normalOct16Samples),
            totals.normalOct16MaxAngularErrorDegrees,
            static_cast<unsigned long long>(totals.tangentOct16Samples),
            totals.tangentOct16MaxAngularErrorDegrees,
            static_cast<unsigned long long>(totals.normalDegenerateVertices),
            static_cast<unsigned long long>(totals.tangentDegenerateVertices),
            static_cast<unsigned long long>(
                totals.bitangentDegenerateVertices),
            static_cast<unsigned long long>(
                totals.bitangentReconstructionSamples),
            static_cast<unsigned long long>(
                totals.bitangentReconstructionInvalid),
            totals.bitangentReconstructionMaxAngularErrorDegrees,
            static_cast<unsigned long long>(
                totals.colorUnorm8ExactComponents),
            static_cast<unsigned long long>(totals.colorComponents),
            static_cast<unsigned long long>(
                totals.colorOutOfUnormRangeComponents),
            totals.colorUnorm8MaxAbsError,
            static_cast<unsigned long long>(
                totals.color2Unorm8ExactComponents),
            static_cast<unsigned long long>(totals.color2Components),
            static_cast<unsigned long long>(
                totals.color2OutOfUnormRangeComponents),
            totals.color2Unorm8MaxAbsError);
        common->Printf(
            "PathTracePrimaryPass: GEO12 skinned measurements vertices=%llu joints(components/nonIntegral/outOfByte/min/max)=%llu/%llu/%llu/%u/%u weights(nonFinite/min/max/sumMin/sumMax)= %llu/%.9g/%.9g/%.9g/%.9g\n",
            static_cast<unsigned long long>(totals.skinnedVertexCount),
            static_cast<unsigned long long>(totals.skinnedJointComponents),
            static_cast<unsigned long long>(
                totals.skinnedJointNonIntegralComponents),
            static_cast<unsigned long long>(
                totals.skinnedJointOutOfByteRangeComponents),
            totals.skinnedJointIndexMin,
            totals.skinnedJointIndexMax,
            static_cast<unsigned long long>(
                totals.skinnedWeightNonFiniteComponents),
            totals.skinnedWeightMin,
            totals.skinnedWeightMax,
            totals.skinnedWeightSumMin,
            totals.skinnedWeightSumMax);

        for (std::size_t recordIndex = firstRecord;
            recordIndex < endRecord;
            ++recordIndex)
        {
            const PtGeometryAttributeSurveyRecord& record =
                survey.records[recordIndex];
            const PtGeometryAttributeSurveyStats& stats = record.stats;
            const char* modelName = "<unresolved>";
            for (const PtGeometryShadowMeshRecord& mesh : meshes)
            {
                if (mesh.valid && mesh.hash == record.meshHash)
                {
                    modelName = mesh.modelName.c_str();
                    break;
                }
            }
            common->Printf(
                "PathTracePrimaryPass: GEO12 attribute record index=%llu mesh=%llu domain=%u deformation=%u surface=%u model='%s' vertices=%llu bytes(position/attribute)=%llu/%llu positionMin=(%.9g,%.9g,%.9g) positionMax=(%.9g,%.9g,%.9g) uvMin=(%.9g,%.9g) uvMax=(%.9g,%.9g) halfUV(overflow/underflow/maxAbs/maxRelative)=%llu/%llu/%.9g/%.9g oct16(maxNormalDegrees/maxTangentDegrees)=%.9g/%.9g basis(degenerateN/T/B/reconstructInvalid/maxDegrees)=%llu/%llu/%llu/%llu/%.9g unorm8(colorExact/total/outOfRange/color2Exact/total/outOfRange)=%llu/%llu/%llu/%llu/%llu/%llu\n",
                static_cast<unsigned long long>(recordIndex),
                static_cast<unsigned long long>(record.meshHash),
                static_cast<unsigned int>(record.sourceDomain),
                static_cast<unsigned int>(record.deformationClass),
                record.modelSurfaceIndex,
                modelName,
                static_cast<unsigned long long>(stats.vertexCount),
                static_cast<unsigned long long>(stats.currentPositionBytes),
                static_cast<unsigned long long>(stats.currentAttributeBytes),
                stats.positionMin[0],
                stats.positionMin[1],
                stats.positionMin[2],
                stats.positionMax[0],
                stats.positionMax[1],
                stats.positionMax[2],
                stats.texCoordMin[0],
                stats.texCoordMin[1],
                stats.texCoordMax[0],
                stats.texCoordMax[1],
                static_cast<unsigned long long>(
                    stats.halfTexCoordOverflowComponents),
                static_cast<unsigned long long>(
                    stats.halfTexCoordUnderflowToZeroComponents),
                stats.halfTexCoordMaxAbsError,
                stats.halfTexCoordMaxRelativeError,
                stats.normalOct16MaxAngularErrorDegrees,
                stats.tangentOct16MaxAngularErrorDegrees,
                static_cast<unsigned long long>(
                    stats.normalDegenerateVertices),
                static_cast<unsigned long long>(
                    stats.tangentDegenerateVertices),
                static_cast<unsigned long long>(
                    stats.bitangentDegenerateVertices),
                static_cast<unsigned long long>(
                    stats.bitangentReconstructionInvalid),
                stats.bitangentReconstructionMaxAngularErrorDegrees,
                static_cast<unsigned long long>(
                    stats.colorUnorm8ExactComponents),
                static_cast<unsigned long long>(stats.colorComponents),
                static_cast<unsigned long long>(
                    stats.colorOutOfUnormRangeComponents),
                static_cast<unsigned long long>(
                    stats.color2Unorm8ExactComponents),
                static_cast<unsigned long long>(stats.color2Components),
                static_cast<unsigned long long>(
                    stats.color2OutOfUnormRangeComponents));
            if (stats.skinnedVertexCount != 0)
            {
                common->Printf(
                    "PathTracePrimaryPass: GEO12 skinned record index=%llu mesh=%llu joints(components/nonIntegral/outOfByte/min/max)=%llu/%llu/%llu/%u/%u weights(nonFinite/min/max/sumMin/sumMax)=%llu/%.9g/%.9g/%.9g/%.9g\n",
                    static_cast<unsigned long long>(recordIndex),
                    static_cast<unsigned long long>(record.meshHash),
                    static_cast<unsigned long long>(
                        stats.skinnedJointComponents),
                    static_cast<unsigned long long>(
                        stats.skinnedJointNonIntegralComponents),
                    static_cast<unsigned long long>(
                        stats.skinnedJointOutOfByteRangeComponents),
                    stats.skinnedJointIndexMin,
                    stats.skinnedJointIndexMax,
                    static_cast<unsigned long long>(
                        stats.skinnedWeightNonFiniteComponents),
                    stats.skinnedWeightMin,
                    stats.skinnedWeightMax,
                    stats.skinnedWeightSumMin,
                    stats.skinnedWeightSumMax);
            }
        }
    }

    void Dump(std::uint64_t frameIndex)
    {
        std::uint64_t liveInstances = 0;
        std::uint64_t liveVertices = 0;
        std::uint64_t liveIndexes = 0;
        std::uint64_t liveTriangles = 0;
        std::uint64_t referencedMeshes = 0;
        std::uint64_t zeroReferenceMeshes = 0;
        std::uint64_t sharedMeshes = 0;
        std::uint64_t maxMeshReferences = 0;
        std::uint64_t staticInstances = 0;
        std::uint64_t rigidInstances = 0;
        std::uint64_t skinnedInstances = 0;
        for (const PtGeometryShadowInstanceRecord& record : instances)
        {
            if (!record.valid)
            {
                continue;
            }
            ++liveInstances;
            liveVertices += record.vertexCount;
            liveIndexes += record.indexCount;
            liveTriangles += record.triangleCount;
            switch (record.key.subInstanceKind)
            {
                case PtCanonicalSubInstanceKind::StaticSurface:
                    ++staticInstances;
                    break;
                case PtCanonicalSubInstanceKind::RigidSurface:
                    ++rigidInstances;
                    break;
                case PtCanonicalSubInstanceKind::SkinnedSurface:
                    ++skinnedInstances;
                    break;
                default:
                    break;
            }
        }
        for (const PtGeometryShadowMeshRecord& record : meshes)
        {
            if (!record.valid)
            {
                continue;
            }
            if (record.liveReferences > 0)
            {
                ++referencedMeshes;
                if (record.liveReferences > 1)
                {
                    ++sharedMeshes;
                }
                if (record.liveReferences > maxMeshReferences)
                {
                    maxMeshReferences = record.liveReferences;
                }
            }
            else
            {
                ++zeroReferenceMeshes;
            }
        }

        common->Printf(
            "PathTracePrimaryPass: GEO05 shadow registry frame=%llu enabled=%d tracking=%d worldGeneration=%llu previousWorldGeneration=%llu mapLoadSerial=%llu primaryHistoryOwner=%llu resets=%llu resetDiscarded(liveInstances/meshes)=%llu/%llu funnelInterval(add/update/remove/unchanged/collision/stale/missing/transient/deferredMesh)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu frontendDeforming(discovered/refreshed)=%llu/%llu records(instances/meshesReferenced/meshesZeroRef/assets/shared/maxRefs)=%llu/%llu/%llu/%d/%llu/%llu instanceClass(static/rigid/skinned)=%llu/%llu/%llu liveGeometry(v/i/t)=%llu/%llu/%llu removedGeometryInterval(v/i/t)=%llu/%llu/%llu removedClassInterval(static/rigid/skinned)=%llu/%llu/%llu route=observation-only\n",
            static_cast<unsigned long long>(frameIndex),
            r_pathTracingGeometryShadowRegistry.GetInteger(),
            shadowTracking ? 1 : 0,
            static_cast<unsigned long long>(worldGeneration),
            static_cast<unsigned long long>(stats.previousWorldGeneration),
            static_cast<unsigned long long>(mapLoadSerial),
            static_cast<unsigned long long>(primaryHistoryOwnerGeneration),
            static_cast<unsigned long long>(stats.worldResets),
            static_cast<unsigned long long>(stats.resetDiscardedLiveInstances),
            static_cast<unsigned long long>(stats.resetDiscardedMeshRecords),
            static_cast<unsigned long long>(stats.added),
            static_cast<unsigned long long>(stats.updated),
            static_cast<unsigned long long>(stats.removed),
            static_cast<unsigned long long>(stats.unchanged),
            static_cast<unsigned long long>(stats.keyCollisions),
            static_cast<unsigned long long>(stats.staleGeneration),
            static_cast<unsigned long long>(stats.missingSource),
            static_cast<unsigned long long>(stats.unsupportedTransient),
            static_cast<unsigned long long>(stats.deferredMeshEviction),
            static_cast<unsigned long long>(stats.frontendDeformingDiscovered),
            static_cast<unsigned long long>(stats.frontendDeformingRefreshed),
            static_cast<unsigned long long>(liveInstances),
            static_cast<unsigned long long>(referencedMeshes),
            static_cast<unsigned long long>(zeroReferenceMeshes),
            static_cast<int>(assets.size()),
            static_cast<unsigned long long>(sharedMeshes),
            static_cast<unsigned long long>(maxMeshReferences),
            static_cast<unsigned long long>(staticInstances),
            static_cast<unsigned long long>(rigidInstances),
            static_cast<unsigned long long>(skinnedInstances),
            static_cast<unsigned long long>(liveVertices),
            static_cast<unsigned long long>(liveIndexes),
            static_cast<unsigned long long>(liveTriangles),
            static_cast<unsigned long long>(stats.removedVertices),
            static_cast<unsigned long long>(stats.removedIndexes),
            static_cast<unsigned long long>(stats.removedTriangles),
            static_cast<unsigned long long>(stats.removedStaticInstances),
            static_cast<unsigned long long>(stats.removedRigidInstances),
            static_cast<unsigned long long>(stats.removedSkinnedInstances));

        const PtGeometrySourceRegistryStats& sourceStats =
            sourceRegistry.Stats();
        common->Printf(
            "PathTracePrimaryPass: GEO06 source registry frame=%llu records=%llu retainedBytes=%llu cumulative(collisions)=%llu interval(add/reuse/revise/reject/skinnedAdmitted/skinnedNegative/copies/copiedBytes)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu route=observation-only\n",
            static_cast<unsigned long long>(frameIndex),
            static_cast<unsigned long long>(sourceRegistry.RecordCount()),
            static_cast<unsigned long long>(sourceStats.retainedBytes),
            static_cast<unsigned long long>(sourceStats.hashCollisions),
            static_cast<unsigned long long>(stats.sourceAdded),
            static_cast<unsigned long long>(stats.sourceReused),
            static_cast<unsigned long long>(stats.sourceRevised),
            static_cast<unsigned long long>(stats.sourceRejected),
            static_cast<unsigned long long>(stats.sourceSkinnedAdmitted),
            static_cast<unsigned long long>(stats.sourceSkinnedNegative),
            static_cast<unsigned long long>(stats.sourcePayloadCopies),
            static_cast<unsigned long long>(stats.sourceCopiedBytes));

        const std::size_t sourceSampleCount = Min(
            sourceRegistry.RecordCount(),
            static_cast<std::size_t>(PT_GEOMETRY_SOURCE_MAX_DUMP_SAMPLES));
        for (std::size_t sampleIndex = 0;
            sampleIndex < sourceSampleCount;
            ++sampleIndex)
        {
            const PtGeometrySourceRecord* source =
                sourceRegistry.RecordAt(sampleIndex);
            if (source == nullptr)
            {
                continue;
            }
            const std::uint32_t firstIndex =
                source->payload.indexes.empty()
                    ? UINT32_MAX
                    : source->payload.indexes.front();
            const std::uint32_t lastIndex =
                source->payload.indexes.empty()
                    ? UINT32_MAX
                    : source->payload.indexes.back();
            const std::uint32_t firstMaterialSlot =
                source->payload.triangles.empty()
                    ? UINT32_MAX
                    : source->payload.triangles.front().sourceMaterialSlot;
            const std::uint32_t lastMaterialSlot =
                source->payload.triangles.empty()
                    ? UINT32_MAX
                    : source->payload.triangles.back().sourceMaterialSlot;
            common->Printf(
                "PathTracePrimaryPass: GEO06 source sample %llu mesh=%llu asset=%llu:%llu surface=%u revision=%llu checksum=%llu verts/indexes/tris=%u/%u/%llu retainedBytes=%llu firstLastIndex=%u/%u firstLastSourceMaterialSlot=%u/%u\n",
                static_cast<unsigned long long>(sampleIndex),
                static_cast<unsigned long long>(source->meshHash),
                static_cast<unsigned long long>(source->key.sourceAssetId),
                static_cast<unsigned long long>(source->key.sourceAssetGeneration),
                source->key.modelSurfaceIndex,
                static_cast<unsigned long long>(source->sourceContentRevision),
                static_cast<unsigned long long>(source->sourceChecksum),
                source->key.vertexCount,
                source->key.indexCount,
                static_cast<unsigned long long>(source->payload.triangles.size()),
                static_cast<unsigned long long>(source->retainedBytes),
                firstIndex,
                lastIndex,
                firstMaterialSlot,
                lastMaterialSlot);
        }

        int sampleCount = 0;
        for (int samplePass = 0; samplePass < 3 && sampleCount < PT_GEOMETRY_SHADOW_MAX_DUMP_SAMPLES; ++samplePass)
        {
            for (const PtGeometryShadowInstanceRecord& record : instances)
            {
                const bool sampleThisPass =
                    (samplePass == 0 && record.key.subInstanceKind == PtCanonicalSubInstanceKind::SkinnedSurface) ||
                    (samplePass == 1 && record.key.subInstanceKind == PtCanonicalSubInstanceKind::RigidSurface) ||
                    (samplePass == 2 && record.key.subInstanceKind == PtCanonicalSubInstanceKind::StaticSurface);
                if (!record.valid ||
                    !sampleThisPass ||
                    sampleCount >= PT_GEOMETRY_SHADOW_MAX_DUMP_SAMPLES)
                {
                    continue;
                }
                common->Printf(
                    "PathTracePrimaryPass: GEO05 shadow sample %d instance=%llu mesh=%llu world=%llu slot=%u generation=%u surface=%u kind=%u source=%u asset=%llu:%llu topology=%llu verts/indexes/tris=%u/%u/%u material=%u entityNum=%d modified=%d class=%s model='%s' materialName='%s'\n",
                    sampleCount,
                    static_cast<unsigned long long>(record.hash),
                    static_cast<unsigned long long>(record.meshHash),
                    static_cast<unsigned long long>(record.key.worldGeneration),
                    record.key.renderDefIndex,
                    record.key.renderDefGeneration,
                    record.key.modelSurfaceIndex,
                    static_cast<std::uint32_t>(record.key.subInstanceKind),
                    static_cast<std::uint32_t>(record.meshKey.sourceDomain),
                    static_cast<unsigned long long>(record.meshKey.sourceAssetId),
                    static_cast<unsigned long long>(record.meshKey.sourceAssetGeneration),
                    static_cast<unsigned long long>(record.meshKey.topologySignature),
                    record.vertexCount,
                    record.indexCount,
                    record.triangleCount,
                    record.materialId,
                    record.entityNum,
                    record.lastModifiedFrameNum,
                    PtGeometryLifecycle::ClassName(record.geometryClass),
                    record.modelName.c_str(),
                    record.materialName.c_str());
                ++sampleCount;
            }
        }

        stats.resetDiscardedLiveInstances = 0;
        stats.resetDiscardedMeshRecords = 0;
        stats.added = 0;
        stats.updated = 0;
        stats.removed = 0;
        stats.unchanged = 0;
        stats.keyCollisions = 0;
        stats.staleGeneration = 0;
        stats.missingSource = 0;
        stats.unsupportedTransient = 0;
        stats.deferredMeshEviction = 0;
        stats.frontendDeformingDiscovered = 0;
        stats.frontendDeformingRefreshed = 0;
        stats.removedVertices = 0;
        stats.removedIndexes = 0;
        stats.removedTriangles = 0;
        stats.removedStaticInstances = 0;
        stats.removedRigidInstances = 0;
        stats.removedSkinnedInstances = 0;
        stats.sourceAdded = 0;
        stats.sourceReused = 0;
        stats.sourceRevised = 0;
        stats.sourceRejected = 0;
        stats.sourceSkinnedAdmitted = 0;
        stats.sourceSkinnedNegative = 0;
        stats.sourcePayloadCopies = 0;
        stats.sourceCopiedBytes = 0;
    }

    std::uint64_t worldGeneration = 0;
    std::uint64_t primaryHistoryOwnerGeneration = 0;
    std::uint64_t mapLoadSerial = 0;
    std::vector<PtGeometryLifecycleSlotState> entitySlots;
    std::vector<PtGeometryLifecycleSlotState> lightSlots;

private:
    void ObserveImmutableSource(
        const PtCanonicalMeshKey& meshKey,
        std::uint64_t sourceContentRevision,
        const srfTriangles_t* tri,
        std::uint32_t sourceMaterialSlot,
        const idRenderModel* sourceModel)
    {
        const bool skinnedBindSource =
            meshKey.sourceDomain ==
                PtCanonicalMeshSourceDomain::SkinnedBindSource &&
            meshKey.deformationClass ==
                PtCanonicalDeformationClass::Skinned;
        const bool rigidSource =
            meshKey.sourceDomain ==
                PtCanonicalMeshSourceDomain::RegisteredRenderModel &&
            meshKey.deformationClass ==
                PtCanonicalDeformationClass::Rigid;
        if (!skinnedBindSource && !rigidSource)
        {
            return;
        }

        const idDrawVert* sourceVerts =
            tri ? tri->verts : nullptr;
        const triIndex_t* sourceIndexes =
            tri ? tri->indexes : nullptr;
        int sourceVertexCount =
            tri ? tri->numVerts : 0;
        int sourceIndexCount =
            tri ? tri->numIndexes : 0;
        if (skinnedBindSource)
        {
            const idRenderModelMD5* md5 =
                dynamic_cast<const idRenderModelMD5*>(sourceModel);
            if (md5 == nullptr ||
                !md5->GetBindPoseGeometry(
                    static_cast<int>(meshKey.modelSurfaceIndex),
                    sourceVerts,
                    sourceVertexCount,
                    sourceIndexes,
                    sourceIndexCount) ||
                sourceVertexCount !=
                    static_cast<int>(meshKey.vertexCount) ||
                sourceIndexCount !=
                    static_cast<int>(meshKey.indexCount) ||
                BuildShadowTopologySignature(
                    sourceIndexes,
                    sourceVertexCount,
                    sourceIndexCount) !=
                    meshKey.topologySignature)
            {
                ++stats.sourceSkinnedNegative;
                ++stats.sourceRejected;
                return;
            }
            ++stats.sourceSkinnedAdmitted;
        }

        const PtGeometrySourceRegistryStats before = sourceRegistry.Stats();
        PtGeometrySourceObserveResult result =
            PtGeometrySourceObserveResult::MissingPayload;
        if (sourceRegistry.Find(meshKey) != nullptr)
        {
            result = sourceRegistry.Observe(
                meshKey,
                sourceContentRevision,
                nullptr);
        }
        else if (sourceVerts == nullptr ||
            sourceIndexes == nullptr ||
            sourceVertexCount <= 0 ||
            sourceIndexCount < 3)
        {
            ++stats.sourceRejected;
            return;
        }
        else
        {
            std::vector<PtGeometrySourcePosition> positions(
                static_cast<size_t>(sourceVertexCount));
            std::vector<PtGeometrySourceAttribute> attributes(
                static_cast<size_t>(sourceVertexCount));
            std::vector<std::uint32_t> indexes(
                static_cast<size_t>(sourceIndexCount));
            std::vector<PtGeometrySourceTriangle> triangles(
                static_cast<size_t>(sourceIndexCount / 3));

            for (int vertexIndex = 0;
                vertexIndex < sourceVertexCount;
                ++vertexIndex)
            {
                const idDrawVert& drawVert = sourceVerts[vertexIndex];
                PtGeometrySourcePosition& position =
                    positions[static_cast<size_t>(vertexIndex)];
                position.xyz[0] = drawVert.xyz.x;
                position.xyz[1] = drawVert.xyz.y;
                position.xyz[2] = drawVert.xyz.z;
                attributes[static_cast<size_t>(vertexIndex)] =
                    BuildShadowSourceAttribute(drawVert);
            }
            for (int index = 0; index < sourceIndexCount; ++index)
            {
                indexes[static_cast<size_t>(index)] =
                    static_cast<std::uint32_t>(sourceIndexes[index]);
            }
            for (PtGeometrySourceTriangle& triangle : triangles)
            {
                triangle.sourceMaterialSlot = sourceMaterialSlot;
            }

            PtGeometrySourcePayloadView payload;
            payload.positions = positions.data();
            payload.positionCount = positions.size();
            payload.attributes = attributes.data();
            payload.attributeCount = attributes.size();
            payload.indexes = indexes.data();
            payload.indexCount = indexes.size();
            payload.triangles = triangles.data();
            payload.triangleCount = triangles.size();
            result = sourceRegistry.Observe(
                meshKey,
                sourceContentRevision,
                &payload,
                r_pathTracingGeometrySourceColorUnorm8.GetBool());
        }

        switch (result)
        {
            case PtGeometrySourceObserveResult::Added:
                ++stats.sourceAdded;
                break;
            case PtGeometrySourceObserveResult::Reused:
                ++stats.sourceReused;
                break;
            case PtGeometrySourceObserveResult::Revised:
                ++stats.sourceRevised;
                break;
            default:
                ++stats.sourceRejected;
                break;
        }
        const PtGeometrySourceRegistryStats after = sourceRegistry.Stats();
        stats.sourcePayloadCopies +=
            after.payloadCopies - before.payloadCopies;
        stats.sourceCopiedBytes +=
            after.copiedBytes - before.copiedBytes;
    }

    const PtGeometryShadowAssetRecord& FindOrCreateAsset(
        const idRenderModel* model,
        PtCanonicalMeshSourceDomain sourceDomain)
    {
        const char* modelName = model ? model->Name() : "<none>";
        for (const PtGeometryShadowAssetRecord& asset : assets)
        {
            if (asset.sourceDomain == sourceDomain && asset.modelName.Icmp(modelName) == 0)
            {
                return asset;
            }
        }

        PtGeometryShadowAssetRecord asset;
        asset.sourceAssetId = AllocateNonZeroGeneration(g_nextShadowAssetId);
        asset.sourceAssetGeneration = 1;
        asset.sourceDomain = sourceDomain;
        asset.modelName = modelName;
        assets.push_back(asset);
        return assets.back();
    }

    PtGeometryShadowMeshRecord* FindMesh(const PtCanonicalMeshKey& key, std::uint64_t hash)
    {
        const std::pair<
            std::unordered_multimap<std::uint64_t, size_t>::iterator,
            std::unordered_multimap<std::uint64_t, size_t>::iterator> range = meshLookup.equal_range(hash);
        bool collision = false;
        for (std::unordered_multimap<std::uint64_t, size_t>::iterator it = range.first; it != range.second; ++it)
        {
            if (it->second >= meshes.size() || !meshes[it->second].valid)
            {
                continue;
            }
            if (meshes[it->second].key == key)
            {
                return &meshes[it->second];
            }
            collision = true;
        }
        if (collision)
        {
            ++stats.keyCollisions;
        }
        return nullptr;
    }

    PtGeometryShadowMeshRecord& AddMesh(
        const PtCanonicalMeshKey& key,
        std::uint64_t hash,
        const idRenderModel* model)
    {
        PtGeometryShadowMeshRecord record;
        record.valid = true;
        record.key = key;
        record.hash = hash;
        record.modelName = model ? model->Name() : "<none>";
        const size_t recordIndex = meshes.size();
        meshes.push_back(record);
        meshLookup.emplace(hash, recordIndex);
        return meshes.back();
    }

    PtGeometryShadowInstanceRecord* FindInstance(const PtCanonicalInstanceKey& key, std::uint64_t hash)
    {
        const std::pair<
            std::unordered_multimap<std::uint64_t, size_t>::iterator,
            std::unordered_multimap<std::uint64_t, size_t>::iterator> range = instanceLookup.equal_range(hash);
        bool collision = false;
        for (std::unordered_multimap<std::uint64_t, size_t>::iterator it = range.first; it != range.second; ++it)
        {
            if (it->second >= instances.size() || !instances[it->second].valid)
            {
                continue;
            }
            if (instances[it->second].key == key)
            {
                return &instances[it->second];
            }
            collision = true;
        }
        if (collision)
        {
            ++stats.keyCollisions;
        }
        return nullptr;
    }

    void TouchInstance(
        const idRenderEntityLocal* entity,
        PtGeometryLifecycleClass geometryClass,
        const idRenderModel* model,
        const idMaterial* material,
        const PtCanonicalMeshKey& meshKey,
        const PtCanonicalInstanceKey& instanceKey,
        std::uint32_t vertexCount,
        std::uint32_t indexCount)
    {
        const std::uint64_t meshHash = PtHashCanonicalMeshKey(meshKey);
        PtGeometryShadowMeshRecord* mesh = FindMesh(meshKey, meshHash);
        if (!mesh)
        {
            mesh = &AddMesh(meshKey, meshHash, model);
        }

        const std::uint64_t instanceHash = PtHashCanonicalInstanceKey(instanceKey);
        PtGeometryShadowInstanceRecord* record = FindInstance(instanceKey, instanceHash);
        const std::uint32_t materialId = SmokeMaterialId(material);
        if (!record)
        {
            PtGeometryShadowInstanceRecord added;
            added.valid = true;
            added.key = instanceKey;
            added.hash = instanceHash;
            added.meshKey = meshKey;
            added.meshHash = meshHash;
            added.materialId = materialId;
            added.vertexCount = vertexCount;
            added.indexCount = indexCount;
            added.triangleCount = indexCount / 3u;
            added.entityNum = entity ? entity->parms.entityNum : -1;
            added.lastModifiedFrameNum = entity ? entity->lastModifiedFrameNum : 0;
            added.geometryClass = geometryClass;
            added.modelName = model ? model->Name() : "<none>";
            added.materialName = material ? material->GetName() : "<none>";
            CopyShadowTransform(entity, added.origin, added.axis);
            const size_t recordIndex = instances.size();
            instances.push_back(added);
            instanceLookup.emplace(instanceHash, recordIndex);
            AppendIdentityEvent(
                PtGeometryIdentityOperation::Upsert,
                added);
            ++mesh->liveReferences;
            mesh->evictionDeferred = false;
            ++stats.added;
            return;
        }

        const bool meshChanged = record->meshKey != meshKey;
        const bool changed =
            meshChanged ||
            record->materialId != materialId ||
            record->vertexCount != vertexCount ||
            record->indexCount != indexCount ||
            record->entityNum != (entity ? entity->parms.entityNum : -1) ||
            record->geometryClass != geometryClass ||
            !ShadowTransformEquals(*record, entity);
        if (meshChanged)
        {
            ReleaseMeshReference(record->meshKey, record->meshHash);
            ++mesh->liveReferences;
            mesh->evictionDeferred = false;
        }

        record->meshKey = meshKey;
        record->meshHash = meshHash;
        record->materialId = materialId;
        record->vertexCount = vertexCount;
        record->indexCount = indexCount;
        record->triangleCount = indexCount / 3u;
        record->entityNum = entity ? entity->parms.entityNum : -1;
        record->lastModifiedFrameNum = entity ? entity->lastModifiedFrameNum : 0;
        record->geometryClass = geometryClass;
        record->modelName = model ? model->Name() : "<none>";
        record->materialName = material ? material->GetName() : "<none>";
        CopyShadowTransform(entity, record->origin, record->axis);
        if (meshChanged)
        {
            AppendIdentityEvent(
                PtGeometryIdentityOperation::Upsert,
                *record);
        }
        if (changed)
        {
            ++stats.updated;
        }
        else
        {
            ++stats.unchanged;
        }
    }

    void ReleaseMeshReference(const PtCanonicalMeshKey& meshKey, std::uint64_t meshHash)
    {
        PtGeometryShadowMeshRecord* mesh = FindMesh(meshKey, meshHash);
        if (!mesh)
        {
            ++stats.staleGeneration;
            return;
        }
        if (mesh->liveReferences == 0)
        {
            ++stats.staleGeneration;
            return;
        }
        --mesh->liveReferences;
        if (mesh->liveReferences == 0 && !mesh->evictionDeferred)
        {
            mesh->evictionDeferred = true;
            ++stats.deferredMeshEviction;
        }
    }

    void RemoveInstance(PtGeometryShadowInstanceRecord& record)
    {
        AppendIdentityEvent(
            PtGeometryIdentityOperation::Remove,
            record);
        ReleaseMeshReference(record.meshKey, record.meshHash);
        stats.removedVertices += record.vertexCount;
        stats.removedIndexes += record.indexCount;
        stats.removedTriangles += record.triangleCount;
        switch (record.key.subInstanceKind)
        {
            case PtCanonicalSubInstanceKind::StaticSurface:
                ++stats.removedStaticInstances;
                break;
            case PtCanonicalSubInstanceKind::RigidSurface:
                ++stats.removedRigidInstances;
                break;
            case PtCanonicalSubInstanceKind::SkinnedSurface:
                ++stats.removedSkinnedInstances;
                break;
            default:
                break;
        }
        ++stats.removed;
        record.valid = false;
    }

    void ResetIdentityPublication()
    {
        identityJournal.clear();
        identityPublishedRecordCount = 0;
        ++identityPublicationGeneration;
        if (identityPublicationGeneration == 0)
        {
            identityPublicationGeneration = 1;
        }
        identityPublicationSequence = 0;
    }

    void MaybeCompactIdentityJournal()
    {
        constexpr std::size_t kIdentityJournalCompactThreshold = 16384;
        if (identityJournal.size() < kIdentityJournalCompactThreshold ||
            identityPublishedRecordCount != identityJournal.size())
        {
            return;
        }

        identityJournal.clear();
        identityPublishedRecordCount = 0;
        ++identityPublicationGeneration;
        if (identityPublicationGeneration == 0)
        {
            identityPublicationGeneration = 1;
        }
        identityPublicationSequence = 0;
        for (const PtGeometryShadowInstanceRecord& instance : instances)
        {
            if (instance.valid)
            {
                AppendIdentityEvent(
                    PtGeometryIdentityOperation::Upsert,
                    instance);
            }
        }
    }

    void AppendIdentityEvent(
        PtGeometryIdentityOperation operation,
        const PtGeometryShadowInstanceRecord& instance)
    {
        if (!PtCanonicalInstanceKeyIsValid(instance.key) ||
            !PtCanonicalMeshKeyIsValid(instance.meshKey))
        {
            return;
        }
        PtGeometryIdentityTransportRecord event;
        event.operation = operation;
        event.eventSequence =
            static_cast<std::uint64_t>(identityJournal.size()) + 1;
        event.instanceKey = instance.key;
        event.instanceHash = instance.hash;
        event.meshKey = instance.meshKey;
        event.meshHash = instance.meshHash;
        identityJournal.push_back(event);
    }

    void RemoveUnobservedEntitySurfaces(
        const idRenderEntityLocal* entity,
        const std::vector<PtCanonicalInstanceKey>& observedKeys)
    {
        if (!entity)
        {
            return;
        }
        const PtGeometryLifecycleSlotState* slot = FindSlot(entitySlots, entity->index);
        if (!slot)
        {
            return;
        }
        for (PtGeometryShadowInstanceRecord& record : instances)
        {
            if (!record.valid ||
                record.key.worldGeneration != worldGeneration ||
                record.key.renderDefIndex != static_cast<std::uint32_t>(entity->index) ||
                record.key.renderDefGeneration != slot->generation)
            {
                continue;
            }
            bool observed = false;
            for (const PtCanonicalInstanceKey& key : observedKeys)
            {
                if (record.key == key)
                {
                    observed = true;
                    break;
                }
            }
            if (!observed)
            {
                RemoveInstance(record);
            }
        }
    }

    bool shadowTracking = false;
    PtGeometryShadowStats stats;
    std::vector<PtGeometryShadowAssetRecord> assets;
    std::vector<PtGeometryShadowMeshRecord> meshes;
    std::unordered_multimap<std::uint64_t, size_t> meshLookup;
    std::vector<PtGeometryShadowInstanceRecord> instances;
    std::unordered_multimap<std::uint64_t, size_t> instanceLookup;
    PtGeometrySourceRegistry sourceRegistry;
    std::size_t sourcePublishedRecordCount = 0;
    std::uint64_t sourcePublicationGeneration = 1;
    std::uint64_t sourcePublicationSequence = 0;
    std::vector<PtGeometryIdentityTransportRecord> identityJournal;
    std::size_t identityPublishedRecordCount = 0;
    std::uint64_t identityPublicationGeneration = 1;
    std::uint64_t identityPublicationSequence = 0;
};

namespace {

PtGeometryLifecycleWorldRegistry* RegistryForWorld(const void* world)
{
    if (!world)
    {
        return nullptr;
    }

    // A PtRenderDefKey can legitimately outlive the idRenderWorldLocal pointer
    // value it records. Never dereference that opaque value to find its owner.
    // This directory validates live owners only; lifecycle data remains owned
    // exclusively by the per-world registry.
    std::lock_guard<std::mutex> lock(g_liveWorldRegistriesMutex);
    const std::unordered_map<const void*, PtGeometryLifecycleWorldRegistry*>::const_iterator it =
        g_liveWorldRegistries.find(world);
    return it != g_liveWorldRegistries.end() ? it->second : nullptr;
}

void SyncShadowTracking(idRenderWorldLocal* world)
{
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(world);
    if (!registry)
    {
        return;
    }
    registry->SetShadowTracking(world, r_pathTracingGeometryShadowRegistry.GetInteger() != 0);
}

}

namespace PtGeometryLifecycle {

PtGeometryLifecycleWorldRegistry* CreateWorldRegistry(const void* world)
{
    PtGeometryLifecycleWorldRegistry* registry = new PtGeometryLifecycleWorldRegistry();
    if (world)
    {
        std::lock_guard<std::mutex> lock(g_liveWorldRegistriesMutex);
        g_liveWorldRegistries[world] = registry;
    }
    return registry;
}

void DestroyWorldRegistry(const void* world, PtGeometryLifecycleWorldRegistry* registry)
{
    if (world)
    {
        std::lock_guard<std::mutex> lock(g_liveWorldRegistriesMutex);
        const std::unordered_map<const void*, PtGeometryLifecycleWorldRegistry*>::iterator it =
            g_liveWorldRegistries.find(world);
        if (it != g_liveWorldRegistries.end() && it->second == registry)
        {
            g_liveWorldRegistries.erase(it);
        }
    }
    delete registry;
}

void BeginWorldMap(PtGeometryLifecycleWorldRegistry* registry, std::uint64_t mapLoadSerial)
{
    if (registry)
    {
        registry->BeginMap(mapLoadSerial);
    }
}

PtCanonicalWorldKey CanonicalWorldKey(const void* world)
{
    PtCanonicalWorldKey key;
    const PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(world);
    key.worldGeneration = registry ? registry->worldGeneration : 0;
    return key;
}

PtCanonicalHistoryOwnerKey PrimaryHistoryOwnerKey(const void* world)
{
    PtCanonicalHistoryOwnerKey key;
    const PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(world);
    if (registry)
    {
        key.worldGeneration = registry->worldGeneration;
        key.ownerGeneration = registry->primaryHistoryOwnerGeneration;
        key.ownerKind = PtCanonicalHistoryOwnerKind::RenderTarget;
        key.ownerRole = PtCanonicalHistoryOwnerRole::PrimaryGameplay;
    }
    return key;
}

PtRenderDefKey MakeEntityKey(const idRenderEntityLocal* entity)
{
    if (!entity)
    {
        return PtRenderDefKey();
    }
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(entity->world);
    return registry ? registry->MakeKey(entity->world, entity->index, false) : PtRenderDefKey();
}

PtRenderDefKey MakeLightKey(const idRenderLightLocal* light)
{
    if (!light)
    {
        return PtRenderDefKey();
    }
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(light->world);
    return registry ? registry->MakeKey(light->world, light->index, true) : PtRenderDefKey();
}

std::uint32_t EntityGeneration(const void* world, int index)
{
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(world);
    return registry ? EnsureSlot(registry->entitySlots, index).generation : 1u;
}

std::uint32_t EntityModelEpoch(const void* world, int index)
{
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(world);
    return registry ? EnsureSlot(registry->entitySlots, index).modelEpoch : 1u;
}

std::uint32_t LightGeneration(const void* world, int index)
{
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(world);
    return registry ? EnsureSlot(registry->lightSlots, index).generation : 1u;
}

bool IsEntityKeyAlive(const PtRenderDefKey& key)
{
    if (!key.world || key.worldGeneration == 0 || key.index < 0 || key.generation == 0)
    {
        return false;
    }
    const PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(key.world);
    if (!registry || registry->worldGeneration != key.worldGeneration)
    {
        return false;
    }
    const PtGeometryLifecycleSlotState* slot = FindSlot(registry->entitySlots, key.index);
    return slot && slot->alive && slot->generation == key.generation;
}

bool IsLightKeyAlive(const PtRenderDefKey& key)
{
    if (!key.world || key.worldGeneration == 0 || key.index < 0 || key.generation == 0)
    {
        return false;
    }
    const PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(key.world);
    if (!registry || registry->worldGeneration != key.worldGeneration)
    {
        return false;
    }
    const PtGeometryLifecycleSlotState* slot = FindSlot(registry->lightSlots, key.index);
    return slot && slot->alive && slot->generation == key.generation;
}

PtGeometryLifecycleClass ClassifyEntity(const idRenderEntityLocal* entity)
{
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    const idRenderModel* model = renderEntity ? renderEntity->hModel : nullptr;
    if (!entity || !renderEntity || !model)
    {
        if (renderEntity && renderEntity->callback)
        {
            return PtGeometryLifecycleClass::Deforming;
        }
        return PtGeometryLifecycleClass::Unknown;
    }
    if (model->IsStaticWorldModel())
    {
        return PtGeometryLifecycleClass::World;
    }
    const dynamicModel_t dynamicModel = model->IsDynamicModel();
    if (dynamicModel == DM_CONTINUOUS)
    {
        return PtGeometryLifecycleClass::Transient;
    }
    if (dynamicModel == DM_CACHED ||
        renderEntity->callback != nullptr ||
        renderEntity->joints != nullptr ||
        renderEntity->numJoints > 0 ||
        entity->dynamicModel != nullptr ||
        entity->cachedDynamicModel != nullptr)
    {
        return PtGeometryLifecycleClass::Deforming;
    }
    if (dynamicModel == DM_STATIC)
    {
        return entity->lastModifiedFrameNum == tr.frameCount
            ? PtGeometryLifecycleClass::RigidMoving
            : PtGeometryLifecycleClass::RigidAtRest;
    }
    return PtGeometryLifecycleClass::Unknown;
}

const char* ClassName(PtGeometryLifecycleClass geometryClass)
{
    switch (geometryClass)
    {
        case PtGeometryLifecycleClass::World:
            return "world";
        case PtGeometryLifecycleClass::RigidAtRest:
            return "rigid-at-rest";
        case PtGeometryLifecycleClass::RigidMoving:
            return "rigid-moving";
        case PtGeometryLifecycleClass::Deforming:
            return "deforming";
        case PtGeometryLifecycleClass::Transient:
            return "transient";
        default:
            return "unknown";
    }
}

void ObserveFrontendDeformingEntities(const viewDef_t* viewDef)
{
    if (!viewDef || r_pathTracingGeometryShadowRegistry.GetInteger() == 0)
    {
        return;
    }

    idRenderWorldLocal* renderWorld = viewDef->renderWorld;
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(renderWorld);
    if (!renderWorld || !registry)
    {
        return;
    }
    SyncShadowTracking(renderWorld);

    std::unordered_set<const idRenderEntityLocal*> observedEntities;
    for (const viewEntity_t* viewEntity = viewDef->viewEntitys;
         viewEntity != nullptr;
         viewEntity = viewEntity->next)
    {
        const idRenderEntityLocal* entity = viewEntity->entityDef;
        if (!entity ||
            entity->world != renderWorld ||
            !observedEntities.insert(entity).second)
        {
            continue;
        }

        const idRenderModel* model = entity->parms.hModel;
        if (!model)
        {
            continue;
        }
        const bool deformingSource =
            model->IsDynamicModel() == DM_CACHED ||
            entity->parms.joints != nullptr ||
            entity->parms.numJoints > 0;
        const idRenderModel* resolvedSurfaceModel =
            entity->dynamicModelFrameCount == tr.frameCount
                ? entity->dynamicModel
                : nullptr;
        if (!deformingSource || !resolvedSurfaceModel)
        {
            continue;
        }

        // This runs on the frontend after R_AddModels has resolved the view's
        // dynamic entities. It is the safe discovery boundary for callback
        // MD5 bind sources; the backend never receives these live pointers.
        registry->ObserveFrontendDeformingEntity(entity, resolvedSurfaceModel);
    }
}

void CaptureSourceDelta(viewDef_t* viewDef)
{
    if (viewDef == nullptr)
    {
        return;
    }
    viewDef->pathTraceGeometrySourceSnapshot = nullptr;
    viewDef->pathTraceGeometryIdentitySnapshot = nullptr;
    if (r_pathTracingGeometryShadowRegistry.GetInteger() == 0 ||
        viewDef->isSubview)
    {
        return;
    }
    PtGeometryLifecycleWorldRegistry* registry =
        RegistryForWorld(viewDef->renderWorld);
    if (registry != nullptr)
    {
        registry->CaptureSourceDelta(viewDef);
        registry->CaptureIdentityDelta(viewDef);
    }
}

void NotifyEntityAdded(const idRenderEntityLocal* entity)
{
    if (!entity)
    {
        return;
    }
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(entity->world);
    if (!registry)
    {
        return;
    }
    SyncShadowTracking(entity->world);
    PtGeometryLifecycleSlotState& slot = EnsureSlot(registry->entitySlots, entity->index);
    slot.alive = true;
    ++g_lifecycleStats.entityAdds;
    const PtGeometryLifecycleClass geometryClass = ClassifyEntity(entity);
    slot.geometryClass = geometryClass;
    AccumulateClass(geometryClass);
    registry->ObserveEntity(entity, geometryClass);

    PtGeometryLifecycleEventSample sample;
    sample.eventKind = PtGeometryLifecycleEventKind::Add;
    sample.defKind = PtGeometryLifecycleDefKind::Entity;
    sample.key = MakeEntityKey(entity);
    sample.entityNum = entity->parms.entityNum;
    sample.lastModifiedFrameNum = entity->lastModifiedFrameNum;
    sample.modelEpoch = slot.modelEpoch;
    sample.geometryClass = geometryClass;
    sample.newModelIdentity = reinterpret_cast<std::uintptr_t>(entity->parms.hModel);
    AddEventSample(sample);
}

void NotifyEntityUpdated(
    const idRenderEntityLocal* entity,
    const idRenderModel* oldModel,
    bool modelChanged,
    bool sourceStable)
{
    if (!entity)
    {
        return;
    }
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(entity->world);
    if (!registry)
    {
        return;
    }
    SyncShadowTracking(entity->world);
    PtGeometryLifecycleSlotState& slot = EnsureSlot(registry->entitySlots, entity->index);
    slot.alive = true;
    ++g_lifecycleStats.entityUpdates;
    if (modelChanged)
    {
        AdvanceSlotModelEpoch(slot);
        ++g_lifecycleStats.entityModelSwaps;
    }
    PtGeometryLifecycleClass geometryClass = slot.geometryClass;
    if (sourceStable)
    {
        geometryClass = ClassifyEntity(entity);
        slot.geometryClass = geometryClass;
        AccumulateClass(geometryClass);
        registry->ObserveEntity(entity, geometryClass);
    }

    PtGeometryLifecycleEventSample sample;
    sample.eventKind = PtGeometryLifecycleEventKind::Update;
    sample.defKind = PtGeometryLifecycleDefKind::Entity;
    sample.key = MakeEntityKey(entity);
    sample.entityNum = entity->parms.entityNum;
    sample.lastModifiedFrameNum = entity->lastModifiedFrameNum;
    sample.modelEpoch = slot.modelEpoch;
    sample.geometryClass = geometryClass;
    sample.modelChanged = modelChanged;
    sample.oldModelIdentity = reinterpret_cast<std::uintptr_t>(oldModel);
    sample.newModelIdentity = reinterpret_cast<std::uintptr_t>(entity->parms.hModel);
    AddEventSample(sample);
}

void NotifyEntityUnchanged(const idRenderEntityLocal* entity)
{
    if (!entity)
    {
        return;
    }
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(entity->world);
    if (!registry)
    {
        return;
    }
    SyncShadowTracking(entity->world);
    PtGeometryLifecycleSlotState& slot = EnsureSlot(registry->entitySlots, entity->index);
    slot.alive = true;
    ++g_lifecycleStats.entityUnchanged;
}

void NotifyEntityFreed(const idRenderEntityLocal* entity)
{
    if (!entity)
    {
        return;
    }
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(entity->world);
    if (!registry)
    {
        return;
    }
    SyncShadowTracking(entity->world);
    PtGeometryLifecycleSlotState& slot = EnsureSlot(registry->entitySlots, entity->index);
    const PtRenderDefKey oldKey = registry->MakeKey(entity->world, entity->index, false);
    registry->RemoveEntity(entity, slot.generation);
    slot.alive = false;
    AdvanceSlotGeneration(slot);

    ++g_lifecycleStats.entityFrees;
    PtGeometryLifecycleEventSample sample;
    sample.eventKind = PtGeometryLifecycleEventKind::Free;
    sample.defKind = PtGeometryLifecycleDefKind::Entity;
    sample.key = oldKey;
    sample.entityNum = entity->parms.entityNum;
    sample.lastModifiedFrameNum = entity->lastModifiedFrameNum;
    sample.modelEpoch = slot.modelEpoch;
    sample.geometryClass = slot.geometryClass;
    sample.newModelIdentity = reinterpret_cast<std::uintptr_t>(entity->parms.hModel);
    AddEventSample(sample);
}

void NotifyLightAdded(const idRenderLightLocal* light)
{
    if (!light)
    {
        return;
    }
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(light->world);
    if (!registry)
    {
        return;
    }
    EnsureSlot(registry->lightSlots, light->index).alive = true;
    ++g_lifecycleStats.lightAdds;

    PtGeometryLifecycleEventSample sample;
    sample.eventKind = PtGeometryLifecycleEventKind::Add;
    sample.defKind = PtGeometryLifecycleDefKind::Light;
    sample.key = MakeLightKey(light);
    sample.lastModifiedFrameNum = light->lastModifiedFrameNum;
    AddEventSample(sample);
}

void NotifyLightUpdated(const idRenderLightLocal* light)
{
    if (!light)
    {
        return;
    }
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(light->world);
    if (!registry)
    {
        return;
    }
    PtGeometryLifecycleSlotState& slot = EnsureSlot(registry->lightSlots, light->index);
    slot.alive = true;
    AdvanceSlotGeneration(slot);
    ++g_lifecycleStats.lightUpdates;

    PtGeometryLifecycleEventSample sample;
    sample.eventKind = PtGeometryLifecycleEventKind::Update;
    sample.defKind = PtGeometryLifecycleDefKind::Light;
    sample.key = MakeLightKey(light);
    sample.lastModifiedFrameNum = light->lastModifiedFrameNum;
    AddEventSample(sample);
}

void NotifyLightFreed(const idRenderLightLocal* light)
{
    if (!light)
    {
        return;
    }
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(light->world);
    if (!registry)
    {
        return;
    }
    PtGeometryLifecycleSlotState& slot = EnsureSlot(registry->lightSlots, light->index);
    const PtRenderDefKey oldKey = registry->MakeKey(light->world, light->index, true);
    slot.alive = false;
    AdvanceSlotGeneration(slot);

    ++g_lifecycleStats.lightFrees;
    PtGeometryLifecycleEventSample sample;
    sample.eventKind = PtGeometryLifecycleEventKind::Free;
    sample.defKind = PtGeometryLifecycleDefKind::Light;
    sample.key = oldKey;
    sample.lastModifiedFrameNum = light->lastModifiedFrameNum;
    AddEventSample(sample);
}

void MaybeDumpLifecycleStats(std::uint64_t frameIndex, const idRenderWorldLocal* renderWorld)
{
    PtGeometryLifecycleWorldRegistry* registry = RegistryForWorld(renderWorld);
    if (registry)
    {
        registry->SetShadowTracking(
            renderWorld,
            r_pathTracingGeometryShadowRegistry.GetInteger() != 0);
    }

    if (r_pathTracingGeometryShadowRegistryDump.GetInteger() != 0)
    {
        if (registry)
        {
            registry->Dump(frameIndex);
        }
        else
        {
            common->Printf("PathTracePrimaryPass: GEO05 shadow registry frame=%llu enabled=%d tracking=0 missingWorld=1 route=observation-only\n",
                static_cast<unsigned long long>(frameIndex),
                r_pathTracingGeometryShadowRegistry.GetInteger());
        }
        r_pathTracingGeometryShadowRegistryDump.SetInteger(0);
    }

    if (r_pathTracingGeometryAttributeSurveyDump.GetInteger() != 0)
    {
        const int requestedPage =
            r_pathTracingGeometryAttributeSurveyDump.GetInteger();
        if (registry)
        {
            registry->DumpAttributeSurvey(frameIndex, requestedPage);
        }
        else
        {
            common->Printf(
                "PathTracePrimaryPass: GEO12 attribute survey frame=%llu route=measurement-only missingWorld=1 records=0\n",
                static_cast<unsigned long long>(frameIndex));
        }
        r_pathTracingGeometryAttributeSurveyDump.SetInteger(0);
    }

    if (r_pathTracingGeometryLifecycleDump.GetInteger() == 0)
    {
        return;
    }

    common->Printf("PathTracePrimaryPass: geometry lifecycle frame=%llu cvar=%d stage=%d worldGeneration=%llu entity add/update/unchanged/free=%d/%d/%d/%d modelSwaps=%d light add/update/free=%d/%d/%d class world/rigidRest/rigidMoving/deforming/transient/unknown=%d/%d/%d/%d/%d/%d perWorldOwner=%d\n",
        static_cast<unsigned long long>(frameIndex),
        r_pathTracingGeometryLifecycle.GetInteger(),
        r_pathTracingGeometryLifecycleStage.GetInteger(),
        static_cast<unsigned long long>(registry ? registry->worldGeneration : 0),
        g_lifecycleStats.entityAdds,
        g_lifecycleStats.entityUpdates,
        g_lifecycleStats.entityUnchanged,
        g_lifecycleStats.entityFrees,
        g_lifecycleStats.entityModelSwaps,
        g_lifecycleStats.lightAdds,
        g_lifecycleStats.lightUpdates,
        g_lifecycleStats.lightFrees,
        g_lifecycleStats.classWorld,
        g_lifecycleStats.classRigidAtRest,
        g_lifecycleStats.classRigidMoving,
        g_lifecycleStats.classDeforming,
        g_lifecycleStats.classTransient,
        g_lifecycleStats.classUnknown,
        registry ? 1 : 0);

    for (int sampleIndex = 0; sampleIndex < g_lifecycleStats.sampleCount; ++sampleIndex)
    {
        const PtGeometryLifecycleEventSample& sample = g_lifecycleStats.samples[sampleIndex];
        common->Printf("PathTracePrimaryPass: geometry lifecycle sample %d %s %s world=%p worldGeneration=%llu index=%d generation=%u modelEpoch=%u entityNum=%d class=%s modifiedFrame=%d modelChanged=%d oldModel=%p newModel=%p\n",
            sampleIndex,
            DefKindName(sample.defKind),
            EventName(sample.eventKind),
            sample.key.world,
            static_cast<unsigned long long>(sample.key.worldGeneration),
            sample.key.index,
            sample.key.generation,
            sample.modelEpoch,
            sample.entityNum,
            ClassName(sample.geometryClass),
            sample.lastModifiedFrameNum,
            sample.modelChanged ? 1 : 0,
            reinterpret_cast<const void*>(sample.oldModelIdentity),
            reinterpret_cast<const void*>(sample.newModelIdentity));
    }

    g_lifecycleStats = PtGeometryLifecycleStats();
    r_pathTracingGeometryLifecycleDump.SetInteger(0);
}

}
