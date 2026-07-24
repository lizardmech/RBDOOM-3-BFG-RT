#pragma once

#include "PathTraceCanonicalGeometryIdentity.h"

#include <stdint.h>

class idRenderEntityLocal;
class idRenderLightLocal;
class idRenderModel;
class idRenderWorldLocal;
class PtGeometryLifecycleWorldRegistry;

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

    uint32_t EntityGeneration(const void* world, int index);
    uint32_t EntityModelEpoch(const void* world, int index);
    uint32_t LightGeneration(const void* world, int index);

    bool IsEntityKeyAlive(const PtRenderDefKey& key);
    bool IsLightKeyAlive(const PtRenderDefKey& key);

    PtGeometryLifecycleClass ClassifyEntity(const idRenderEntityLocal* entity);
    const char* ClassName(PtGeometryLifecycleClass geometryClass);

    void NotifyEntityAdded(const idRenderEntityLocal* entity);
    void NotifyEntityUpdated(const idRenderEntityLocal* entity, const idRenderModel* oldModel, bool modelChanged, bool sourceStable = true);
    void NotifyEntityUnchanged(const idRenderEntityLocal* entity);
    void NotifyEntityFreed(const idRenderEntityLocal* entity);

    void NotifyLightAdded(const idRenderLightLocal* light);
    void NotifyLightUpdated(const idRenderLightLocal* light);
    void NotifyLightFreed(const idRenderLightLocal* light);

    void MaybeDumpLifecycleStats(uint64_t frameIndex, const idRenderWorldLocal* renderWorld);
}
