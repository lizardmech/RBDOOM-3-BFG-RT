#pragma once

#include "PathTraceCanonicalGeometryIdentity.h"
#include "PathTraceRigidInstanceRecord.h"

#include <stdint.h>
#include <vector>

class idRenderEntityLocal;
class idRenderLightLocal;
class idRenderModel;
class idRenderWorldLocal;
class PtGeometryLifecycleWorldRegistry;
struct viewDef_t;

// Pointer-free facts used by both lifecycle observation and frontend capture.
// Surface-instantiation state and GPU-skinning policy deliberately do not
// participate in stable source-domain identity.
struct PtSourceDomainFacts
{
    bool sourcePresent = false;
    bool staticWorld = false;
    bool continuous = false;
    bool cached = false;
    bool entityJointed = false;
};

inline PtCanonicalMeshSourceDomain PtClassifySourceDomain(const PtSourceDomainFacts& facts)
{
    if (!facts.sourcePresent) return PtCanonicalMeshSourceDomain::Invalid;
    if (facts.staticWorld) return PtCanonicalMeshSourceDomain::StaticWorldMap;
    if (facts.continuous) return PtCanonicalMeshSourceDomain::UnsupportedTransient;
    if (facts.cached || facts.entityJointed) return PtCanonicalMeshSourceDomain::SkinnedBindSource;
    return PtCanonicalMeshSourceDomain::RegisteredRenderModel;
}

struct PtRenderDefKey
{
    const void* world = nullptr;
    uint64_t worldGeneration = 0;
    int index = -1;
    uint32_t generation = 0;
};

enum class PtGeometryLifecycleEventKind : uint32_t
{
    Add = 0,
    Update,
    Free
};

enum class PtGeometryLifecycleDefKind : uint32_t
{
    Entity = 0,
    Light
};

enum class PtGeometryLifecycleClass : uint32_t
{
    Unknown = 0,
    World,
    RigidAtRest,
    RigidMoving,
    Deforming,
    Transient
};

namespace PtGeometryLifecycle
{
    PtGeometryLifecycleWorldRegistry* CreateWorldRegistry(const void* world);
    void DestroyWorldRegistry(const void* world, PtGeometryLifecycleWorldRegistry* registry);
    void BeginWorldMap(PtGeometryLifecycleWorldRegistry* registry, uint64_t mapLoadSerial);

    PtCanonicalWorldKey CanonicalWorldKey(const void* world);
    PtCanonicalHistoryOwnerKey PrimaryHistoryOwnerKey(const void* world);

    PtRenderDefKey MakeEntityKey(const idRenderEntityLocal* entity);
    PtRenderDefKey MakeLightKey(const idRenderLightLocal* light);

    struct PtFrontendCanonicalAuthority {
        uint64_t sourceAssetId = 0;
        uint64_t sourceAssetGeneration = 0;
        uint64_t worldGeneration = 0;
        uint32_t renderDefGeneration = 0;
    };

    bool ResolveFrontendCanonicalAuthority(
        const void* world,
        int renderDefIndex,
        const idRenderModel* model,
        PtCanonicalMeshSourceDomain sourceDomain,
        PtFrontendCanonicalAuthority& outAuthority);

    uint32_t EntityGeneration(const void* world, int index);
    uint32_t EntityModelEpoch(const void* world, int index);
    uint32_t LightGeneration(const void* world, int index);

    bool IsEntityKeyAlive(const PtRenderDefKey& key);
    bool IsLightKeyAlive(const PtRenderDefKey& key);

    PtGeometryLifecycleClass ClassifyEntity(const idRenderEntityLocal* entity);
    const char* ClassName(PtGeometryLifecycleClass geometryClass);

    void NotifyEntityAdded(const idRenderEntityLocal* entity);
    void NotifyEntityUpdated(const idRenderEntityLocal* entity, const idRenderModel* oldModel, bool modelChanged, bool sourceStable = true);
    void PersistRigidMeshFromPresent(const idRenderEntityLocal* entity);
    void PersistRigidMeshFromPresent(const idRenderModel* model);
    void NotifyEntityUnchanged(const idRenderEntityLocal* entity);
    void NotifyEntityFreed(const idRenderEntityLocal* entity);
    void ObserveFrontendDeformingEntities(const viewDef_t* viewDef);
    void CaptureSourceDelta(viewDef_t* viewDef);

    void NotifyLightAdded(const idRenderLightLocal* light);
    void NotifyLightUpdated(const idRenderLightLocal* light);
    void NotifyLightFreed(const idRenderLightLocal* light);

    void MaybeDumpLifecycleStats(uint64_t frameIndex, const idRenderWorldLocal* renderWorld);

    // Phase B packer: per-frame dirty tokens + id-only snapshots.
    // instanceId/lightId are integer keys, never pointers.
    struct PackedInstanceId
    {
        uint64_t instanceId = 0;
        uint64_t meshId = 0;
        uint32_t dirty = 0;
        uint32_t generation = 0;
        uint32_t live = 0;
    };
    struct PackedLightId
    {
        uint64_t lightId = 0;
        uint32_t dirty = 0;
        uint32_t generation = 0;
        uint32_t live = 0;
    };
    struct FrameCounters
    {
        int entityAdds = 0;
        int entityUpdates = 0;
        int entityUnchanged = 0;
        int entityFrees = 0;
        int entityModelSwaps = 0;
        int lightAdds = 0;
        int lightUpdates = 0;
        int lightFrees = 0;
    };

    void BeginProducerPackFrame();
    FrameCounters PeekFrameCounters();
    void SnapshotPackedIds(
        const idRenderWorldLocal* world,
        std::vector<PackedInstanceId>& instances,
        std::vector<PackedLightId>& lights);

    struct PresentedEntityRecord
    {
        PtRenderDefKey key;
        PtGeometryLifecycleClass geometryClass = PtGeometryLifecycleClass::Unknown;
        bool alive = false;
    };
    void SnapshotPresentedEntities(
        const idRenderWorldLocal* world,
        std::vector<PresentedEntityRecord>& entities);

    void SnapshotLiveRigidRegistryInstances(
        const idRenderWorldLocal* world,
        std::vector<cpu_producer_publish::RigidRegistryInstanceRecord>& instances);
}
