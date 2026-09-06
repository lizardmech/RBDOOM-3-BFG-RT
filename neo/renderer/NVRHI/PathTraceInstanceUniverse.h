#pragma once

// Diagnostics-only live scene instance mirror.
//
// This records the final visible drawSurf stream as reusable mesh identity plus
// per-frame instance transforms. It is intentionally not a BLAS/TLAS producer
// yet; the existing smoke capture path remains the renderable fallback.

#include "PathTraceSceneCapture.h"
#include "PathTraceGeometryLifecycle.h"
#include "PathTraceUniversePlanningSnapshot.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <type_traits>

struct viewDef_t;
struct RtPathTraceCommittedBaselineEpoch;
class idMaterial;
class idRenderEntityLocal;

const int RT_PT_INSTANCE_UNIVERSE_SAMPLES = 8;
const int RT_PT_INSTANCE_UNIVERSE_MOVED_RIGID_SAMPLES = 8;

enum RtPathTraceInstanceSourceFlags : uint32_t
{
    RT_PT_INSTANCE_SOURCE_STATIC_WORLD = 1u << 0,
    RT_PT_INSTANCE_SOURCE_RIGID = 1u << 1,
    RT_PT_INSTANCE_SOURCE_SKINNED_OR_DEFORMING = 1u << 2,
    RT_PT_INSTANCE_SOURCE_PARTICLE_OR_TRANSIENT = 1u << 3,
    RT_PT_INSTANCE_SOURCE_GUI = 1u << 4,
    RT_PT_INSTANCE_SOURCE_CALLBACK_OR_GENERATED = 1u << 5,
    RT_PT_INSTANCE_SOURCE_STATIC_UNIVERSE_MATCH = 1u << 6,
    RT_PT_INSTANCE_SOURCE_STATIC_CACHE_MATCH = 1u << 7,
    RT_PT_INSTANCE_SOURCE_MATERIAL_OVERRIDE = 1u << 8,
    RT_PT_INSTANCE_SOURCE_ENTITY_FEED = 1u << 9
};

enum class RtPathTraceResidencyClass : uint32_t
{
    Unknown = 0,
    StaticWorld,
    DurableRigid,
    DynamicFrame,
    TransientEffect
};

inline RtPathTraceResidencyClass RtPathTraceResidencyClassForSourceFlags(uint32_t sourceFlags)
{
    if ((sourceFlags & (RT_PT_INSTANCE_SOURCE_PARTICLE_OR_TRANSIENT | RT_PT_INSTANCE_SOURCE_GUI)) != 0)
    {
        return RtPathTraceResidencyClass::TransientEffect;
    }
    if ((sourceFlags & (RT_PT_INSTANCE_SOURCE_SKINNED_OR_DEFORMING | RT_PT_INSTANCE_SOURCE_CALLBACK_OR_GENERATED)) != 0)
    {
        return RtPathTraceResidencyClass::DynamicFrame;
    }
    if ((sourceFlags & (RT_PT_INSTANCE_SOURCE_STATIC_WORLD | RT_PT_INSTANCE_SOURCE_STATIC_UNIVERSE_MATCH | RT_PT_INSTANCE_SOURCE_STATIC_CACHE_MATCH)) != 0)
    {
        return RtPathTraceResidencyClass::StaticWorld;
    }
    if ((sourceFlags & RT_PT_INSTANCE_SOURCE_RIGID) != 0)
    {
        return RtPathTraceResidencyClass::DurableRigid;
    }
    return RtPathTraceResidencyClass::Unknown;
}

inline bool RtPathTraceSourceFlagsAreDurableRigid(uint32_t sourceFlags)
{
    return RtPathTraceResidencyClassForSourceFlags(sourceFlags) == RtPathTraceResidencyClass::DurableRigid;
}

struct RtPathTraceSourceFlagInput
{
    RtSmokeSurfaceClass surfaceClass = RtSmokeSurfaceClass::Unknown;
    bool guiSurface = false;
    bool hasJointCache = false;
    bool hasStaticModelWithJoints = false;
    bool hasRenderEntityJoints = false;
    bool entityCallbackPresent = false;
    bool entityForceUpdate = false;
    bool dynamicModelPresent = false;
    bool cachedDynamicModelPresent = false;
    bool materialDeformed = false;
    bool customShaderPresent = false;
    bool customSkinPresent = false;
};

inline uint32_t RtPathTraceSourceFlagsFromPod(
    const RtPathTraceSourceFlagInput& input)
{
    uint32_t flags = 0;
    switch (input.surfaceClass)
    {
        case RtSmokeSurfaceClass::StaticWorld:
            flags |= RT_PT_INSTANCE_SOURCE_STATIC_WORLD;
            break;
        case RtSmokeSurfaceClass::RigidEntity:
            flags |= RT_PT_INSTANCE_SOURCE_RIGID;
            break;
        case RtSmokeSurfaceClass::SkinnedDeformed:
            flags |= RT_PT_INSTANCE_SOURCE_SKINNED_OR_DEFORMING;
            break;
        case RtSmokeSurfaceClass::ParticleAlpha:
            flags |= RT_PT_INSTANCE_SOURCE_PARTICLE_OR_TRANSIENT;
            break;
        default:
            break;
    }
    if (input.guiSurface)
    {
        flags |= RT_PT_INSTANCE_SOURCE_GUI;
    }
    if (input.hasJointCache || input.hasStaticModelWithJoints ||
        input.hasRenderEntityJoints)
    {
        flags |= RT_PT_INSTANCE_SOURCE_SKINNED_OR_DEFORMING;
    }
    if (input.entityCallbackPresent || input.entityForceUpdate ||
        input.dynamicModelPresent || input.cachedDynamicModelPresent ||
        input.materialDeformed)
    {
        flags |= RT_PT_INSTANCE_SOURCE_CALLBACK_OR_GENERATED;
    }
    if (input.customShaderPresent || input.customSkinPresent)
    {
        flags |= RT_PT_INSTANCE_SOURCE_MATERIAL_OVERRIDE;
    }
    return flags;
}

struct RtPathTraceMeshKey
{
    const srfTriangles_t* tri = nullptr;
    uintptr_t vertexBufferIdentity = 0;
    uintptr_t indexBufferIdentity = 0;
    int numVerts = 0;
    int numIndexes = 0;
    uint32_t vertexFormat = 0;
    uint32_t materialId = 0;
    uint32_t materialClassSignature = 0;
    uint32_t sourceKind = 0;
};

struct RtPathTraceMeshObservation
{
    RtPathTraceMeshKey key;
    uint64 stableHash = 0;
    const idMaterial* baseMaterial = nullptr;
    uint32_t surfaceClassId = 0;
    int jointIndex = -1;
    idStr materialName;
    idStr modelName;
    bool localSpaceValid = false;
};

struct RtPathTraceInstanceObservation
{
    uint64 instanceId = 0;
    uint64 meshHash = 0;
    const idRenderEntityLocal* entity = nullptr;
    int entityIndex = -1;
    int renderEntityNum = -1;
    int drawSurfIndex = -1;
    int modelSurfaceIndex = -1;
    int jointIndex = -1;
    int currentArea = -1;
    PtRenderDefKey renderDefKey;
    uint32_t modelEpoch = 0;
    uint32_t materialOverrideId = 0;
    uint32_t surfaceClassId = 0;
    uint32_t triangleClassAndFlags = 0;
    uint32_t sourceFlags = 0;
    uint32_t trustFlags = 0;
    float objectToWorld[16] = {};
    bool hasPreviousObjectToWorld = false;
    bool transformContinuous = false;
    float previousObjectToWorld[16] = {};
    idStr materialName;
    idStr modelName;
};

struct RtPathTraceInstanceUniverseSample
{
    bool valid = false;
    int drawSurfIndex = -1;
    int entityIndex = -1;
    int renderEntityNum = -1;
    int verts = 0;
    int indexes = 0;
    uint64 meshHash = 0;
    uint64 instanceId = 0;
    RtSmokeSurfaceClass surfaceClass = RtSmokeSurfaceClass::Unknown;
    uint32_t sourceFlags = 0;
    idVec3 origin = vec3_origin;
    idStr materialName;
    idStr modelName;
};

struct RtPathTraceMovedRigidInstanceSample
{
    bool valid = false;
    int drawSurfIndex = -1;
    int entityIndex = -1;
    int renderEntityNum = -1;
    uint64 meshHash = 0;
    uint64 instanceId = 0;
    int transformChangeCount = 0;
    float maxFirstToCurrentMatrixDelta = 0.0f;
    float maxObservedMatrixDelta = 0.0f;
    float maxObservedOriginDelta = 0.0f;
    idVec3 firstOrigin = vec3_origin;
    idVec3 currentOrigin = vec3_origin;
    idStr materialName;
    idStr modelName;
};

struct RtPathTraceInstanceUniverseStats
{
    int drawSurfCount = 0;
    int usableDrawSurfs = 0;
    int skippedDrawSurfs = 0;
    int uniqueMeshCount = 0;
    int instanceCount = 0;
    int meshCacheHits = 0;
    int meshCacheMisses = 0;
    int staticWorldSurfaces = 0;
    int rigidSurfaces = 0;
    int skinnedOrDeformingSurfaces = 0;
    int particleOrTransientSurfaces = 0;
    int unknownSurfaces = 0;
    int staticUniverseMatches = 0;
    int staticGeometryCacheMatches = 0;
    int sameTransformObservations = 0;
    int changedTransformObservations = 0;
    int changingTransformRigidObservations = 0;
    int everChangedTransformObservations = 0;
    int everChangedRigidTransformObservations = 0;
    int materialOverrideObservations = 0;
    int missingMaterialOrSkinOverrideMetadata = 0;
    int residencyStaticWorldInstances = 0;
    int residencyDurableRigidInstances = 0;
    int residencyDynamicFrameInstances = 0;
    int residencyTransientEffectInstances = 0;
    int residencyUnknownInstances = 0;
    int dynamicSkinnedDeformingCandidates = 0;
    int callbackOrGeneratedCandidates = 0;
    int guiCandidates = 0;
    int particlesOrTransientCandidates = 0;
    int dynamicFramePreviousMatches = 0;
    int transientEffectPreviousMatches = 0;
    int nullSurfaceSkips = 0;
    int missingGeometrySkips = 0;
    int nullMaterialSkips = 0;
    int nullSpaceSkips = 0;
    int nullModelSkips = 0;
    int invalidIndexSkips = 0;
    int nonCurrentCacheSkips = 0;
    int guiSurfaceSkips = 0;
    int callbackEntitySkips = 0;
    uint64 frameIndex = 0;
    uint64 generation = 1;
    RtPathTraceInstanceUniverseSample samples[RT_PT_INSTANCE_UNIVERSE_SAMPLES];
    int sampleCount = 0;
    RtPathTraceMovedRigidInstanceSample movedRigidSamples[RT_PT_INSTANCE_UNIVERSE_MOVED_RIGID_SAMPLES];
    int movedRigidSampleCount = 0;
};

struct RtPathTraceInstanceUniverseDiagnosticDesc
{
    bool dumpRequested = false;
    int sceneSource = 0;
    int legacySourceSurfaces = 0;
    const RtSmokeSurfaceClassStats* legacyClassStats = nullptr;
    const RtSmokeSurfaceSkipStats* legacySkipStats = nullptr;
};

class RtPathTraceInstanceUniverse
{
private:
    struct MeshRecord
    {
        RtPathTraceMeshKey key;
        uint64 stableHash = 0;
        const idMaterial* baseMaterial = nullptr;
        idStr materialName;
        idStr modelName;
        int firstSeenFrame = 0;
        int lastSeenFrame = 0;
        int seenCount = 0;
        bool localSpaceValid = false;
    };

    struct InstanceHistory
    {
        uint64 instanceId = 0;
        uint64 lastSeenFrame = 0;
        float firstObjectToWorld[16] = {};
        float lastObjectToWorld[16] = {};
        float maxObservedMatrixDelta = 0.0f;
        float maxObservedOriginDelta = 0.0f;
        int sameTransformCount = 0;
        int changedTransformCount = 0;
    };

public:
    struct ObservationTxn
    {
    private:
        friend class RtPathTraceInstanceUniverse;
        std::vector<MeshRecord> meshRecords;
        std::unordered_map<uint64, size_t> meshLookup;
        std::vector<InstanceHistory> instanceHistories;
        std::unordered_map<uint64, size_t> instanceHistoryLookup;
        uint64 generation = 1;
        const RtPathTraceInstanceUniverse* owner = nullptr;
        uint64 lifecycleSerial = 0;
        uint64 frameBeginSerial = 0;
        uint64 frameIndex = 0;
        uint64 baseGeneration = 0;
        uint32_t stagedObservationCount = 0;
        bool complete = false;
    };

    struct ObservationTxnAllocationTestSeam
    {
        size_t failAtClone = static_cast<size_t>(-1);
        size_t cloneCalls = 0;
        bool throwLengthError = false;
    };

    enum class ObservationReplayAllocationPhase : uint32_t
    {
        BeforePersistentMutation = 0,
        AfterPersistentMutation,
        AfterLiveFrameMeshMutation,
        AfterLiveFrameInstanceMutation
    };

    struct ObservationReplayAllocationTestSeam
    {
        ObservationReplayAllocationPhase failAt =
            ObservationReplayAllocationPhase::BeforePersistentMutation;
        size_t phaseCalls = 0;
        bool armed = false;
        bool throwLengthError = false;
    };

    void Clear();
    void BeginFrame(uint64 frameIndex, const viewDef_t* viewDef);
    void EndFrame();
    void SetObservedDrawSurfCount(int drawSurfCount);
    void RecordSkippedDrawSurf(const RtSmokeSurfaceSkipStats& skipStats);
    void RecordObservation(
        const RtPathTraceMeshObservation& meshObservation,
        const RtPathTraceInstanceObservation& instanceObservation,
        RtSmokeSurfaceClass surfaceClass,
        int numVerts,
        int numIndexes);
    bool BeginObservationTxn(
        ObservationTxn& txn,
        ObservationTxnAllocationTestSeam* allocationTestSeam = nullptr) const;
    bool RecordObservationStaged(
        ObservationTxn& txn,
        const RtPathTraceMeshObservation& meshObservation,
        const RtPathTraceInstanceObservation& instanceObservation,
        RtSmokeSurfaceClass surfaceClass,
        int numVerts,
        int numIndexes,
        ObservationReplayAllocationTestSeam* allocationTestSeam = nullptr);
    bool CommitObservationTxn(ObservationTxn& txn) noexcept;
    void AbortFrameObservations() noexcept;
    bool ObservationApplyMayBeFirstTouch() const noexcept;
    const RtPathTraceInstanceUniverseStats& GetFrameStats() const;
    const std::vector<RtPathTraceInstanceObservation>& FrameInstances() const;
    uint64 Generation() const noexcept { return m_generation; }
    bool HasFrameInstance(uint64 instanceId) const;
    bool CaptureInstanceUniverseSnapshot(
        RtPathTraceInstanceUniverseSnapshot& snapshot,
        const RtPathTracePlanningSnapshotEpoch& epoch,
        size_t& captureProductSlotBytes) const;
    bool CaptureCommittedInstanceUniverseSnapshot(
        RtPathTraceInstanceUniverseSnapshot& snapshot,
        const RtPathTraceCommittedBaselineEpoch& epoch,
        size_t& captureProductSlotBytes) const;
    bool CountInstanceUniverseSnapshot(
        const RtPathTracePlanningSnapshotEpoch& epoch,
        RtPathTraceInstanceUniverseSnapshotCounts& counts) const;
    bool FillInstanceUniverseSnapshotPreReserved(
        RtPathTraceInstanceUniverseSnapshot& snapshot,
        const RtPathTracePlanningSnapshotEpoch& epoch,
        const RtPathTraceInstanceUniverseSnapshotCounts& counts) const;
    void RunDiagnostics(const RtPathTraceInstanceUniverseDiagnosticDesc& desc);

#if defined(RT_PT_INSTANCE_UNIVERSE_HARNESS)
    bool DebugPersistentStateEquals(const RtPathTraceInstanceUniverse& rhs) const;
    bool DebugFrameStateEquals(const RtPathTraceInstanceUniverse& rhs) const;
#endif

private:
    bool CaptureInstanceUniverseSnapshotInternal(
        RtPathTraceInstanceUniverseSnapshot& snapshot,
        const RtPathTracePlanningSnapshotEpoch& epoch,
        size_t& captureProductSlotBytes,
        bool committedAfterEndFrame) const;
    void ResetFrameStats();
    void ResetFrameObservationAuthority() noexcept;
    bool ObservationTxnMatchesLive(const ObservationTxn& txn) const noexcept;
    void InvalidateObservationTxn(ObservationTxn& txn) const noexcept;
    MeshRecord* FindOrCreateMeshRecord(
        std::vector<MeshRecord>& meshRecords,
        std::unordered_map<uint64, size_t>& meshLookup,
        uint64& generation,
        const RtPathTraceMeshObservation& observation,
        bool& cacheHit);
    InstanceHistory* FindOrCreateInstanceHistory(
        std::vector<InstanceHistory>& instanceHistories,
        std::unordered_map<uint64, size_t>& instanceHistoryLookup,
        uint64 instanceId);
    void RecordObservationImpl(
        std::vector<MeshRecord>& meshRecords,
        std::unordered_map<uint64, size_t>& meshLookup,
        std::vector<InstanceHistory>& instanceHistories,
        std::unordered_map<uint64, size_t>& instanceHistoryLookup,
        uint64& generation,
        const RtPathTraceMeshObservation& meshObservation,
        const RtPathTraceInstanceObservation& instanceObservation,
        RtSmokeSurfaceClass surfaceClass,
        int numVerts,
        int numIndexes,
        ObservationReplayAllocationTestSeam* allocationTestSeam);
    void AddSample(
        const RtPathTraceMeshObservation& meshObservation,
        const RtPathTraceInstanceObservation& instanceObservation,
        RtSmokeSurfaceClass surfaceClass,
        int numVerts,
        int numIndexes);
    void AddMovedRigidSample(
        const RtPathTraceMeshObservation& meshObservation,
        const RtPathTraceInstanceObservation& instanceObservation,
        const InstanceHistory& history);

    const void* m_renderWorld = nullptr;
    uint64 m_frameIndex = 0;
    uint64 m_generation = 1;
    bool m_frameActive = false;
    std::vector<MeshRecord> m_meshRecords;
    std::unordered_map<uint64, size_t> m_meshLookup;
    std::unordered_set<uint64> m_frameMeshHashes;
    std::vector<InstanceHistory> m_instanceHistories;
    std::unordered_map<uint64, size_t> m_instanceHistoryLookup;
    std::vector<RtPathTraceInstanceObservation> m_frameInstances;
    RtPathTraceInstanceUniverseStats m_frameStats;
    uint64 m_lifecycleSerial = 1;
    uint64 m_frameBeginSerial = 0;
    uint32_t m_frameObservedDrawSurfCountCalls = 0;
    uint32_t m_frameSkippedDrawSurfCalls = 0;
    uint32_t m_frameObservationCalls = 0;
};
