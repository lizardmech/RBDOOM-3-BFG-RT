#pragma once

// Thin frame-level shell for the experimental RT smoke/path tracing path.
//
// PathTracePrimaryPass owns the persistent smoke test state and exposes the
// frame entry/present hooks used by the renderer. Scene build, resource
// lifetime, dispatch, readback, and diagnostics live in PathTrace* modules.

#include "PathTraceGeometryUniverse.h"
#include "PathTraceAccelerationPlan.h"
#include "PathTraceAccelCpuPack.h"
#include "PathTraceDrawSurfCapture.h"
#include "PathTraceDoomLights.h"
#include "PathTraceCpuWork.h"
#include "PathTraceEmissiveCandidates.h"
#include "PathTraceFrameResources.h"
#include "PathTraceInstanceUniverse.h"
#include "PathTraceCleanRestirGi.h"
#include "PathTraceCleanRtxdiDiMaterialFeatures.h"
#include "PathTraceNeeCache.h"
#include "PathTraceParticleCapture.h"
#include "PathTraceReGIR.h"
#include "PathTraceRemixFramePrepare.h"
#include "PathTraceRemixLightManager.h"
#include "PathTraceDebugModes.h"
#include "PathTraceSceneInputs.h"
#include "PathTraceSceneUniverse.h"
#include "PathTraceSkinnedBlasState.h"
#include "PathTraceSkinnedHitRoute.h"
#include "PathTraceSkinnedOutputAllocator.h"
#include "PathTraceSmokeResources.h"
#include "PathTraceCpuProducerRewrite.h"
#include "PathTraceUnifiedPt.h"

#include <nvrhi/nvrhi.h>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <future>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

class idRenderBackend;
class idParallelJobList;
class TonemapPass;
struct viewDef_t;

// Host half of pathtrace_sky_surface_resolve.cs.hlsl b0. Keep the allocation
// tied to sizeof(this type); duplicating a literal byte count caused the UPT
// glass startup crash when the shader ABI grew from one to two registers.
struct PathTraceSkySurfaceResolveConstants
{
    uint32_t width = 0;
    uint32_t height = 0;
    float brightness = 1.0f;
    uint32_t enabled = 0;
    float cameraOrigin[3] = {};
    uint32_t useUnifiedPtCompactReceiver = 0;
};
static_assert(
    sizeof(PathTraceSkySurfaceResolveConstants) == 32u,
    "sky-surface resolve constant ABI must remain two 16-byte registers");

struct RtSmokeSkinnedHistoryState
{
    PtCanonicalHistoryOwnerKey owner;
    std::vector<RtSmokeSkinnedSurfaceRecord> records;
    std::vector<PathTraceSmokeVertex> vertices;
    std::vector<PathTraceSkinnedJointMatrix> joints;
    uint64 updateSerial = 0;
};

struct RtRetiredSmokeScenePackage
{
    uint64 retireFrame = 0;
    uint64 completionToken = 0;
    uint64 skinnedOutputStorageGeneration = 0;
    bool completionArmed = false;
    nvrhi::EventQueryHandle completionQuery;
    RtSmokeSceneBufferHandles buffers;

    nvrhi::rt::AccelStructHandle staticBlas;
    nvrhi::rt::AccelStructHandle dynamicBlas;
    nvrhi::rt::AccelStructHandle tlas;
    nvrhi::BindingSetHandle bindingSet;
    nvrhi::DescriptorTableHandle textureDescriptorTable;
    std::vector<nvrhi::TextureHandle> activeTextureTable;
    nvrhi::TextureHandle skyEnvironmentCube;
    nvrhi::BindingSetHandle skyCubeProbeBindingSet;
};

struct RtRetiredSmokeStaticBucketGpuPackage
{
    uint64 retireFrame = 0;
    uint64 completionToken = 0;
    uint64 bufferBytes = 0;
    uint64 blasResultBytes = 0;
    int blasResultQueryFailures = 0;
    bool completionArmed = false;
    nvrhi::EventQueryHandle completionQuery;
    RtSmokeRetiredStaticBucketGpuResources resources;
};

struct RtRetiredSmokeRigidGpuPackage
{
    uint64 retireFrame = 0;
    uint64 completionToken = 0;
    uint64 bufferBytes = 0;
    uint64 blasResultBytes = 0;
    int blasResultQueryFailures = 0;
    bool completionArmed = false;
    nvrhi::EventQueryHandle completionQuery;
    RtSmokeRetiredRigidGpuResources resources;
};

struct RtSmokeSkinnedComparisonBlasResource
{
    PtCanonicalInstanceKey instanceKey;
    PtCanonicalMeshKey meshKey;
    uint64 sourceChecksum = 0;
    uint64 sourceGpuIndexGeneration = 0;
    uint64 sourceIndexOffsetBytes = 0;
    uint64 sourceIndexBytes = 0;
    uint64 outputStorageGeneration = 0;
    uint64 outputVertexOffsetBytes = 0;
    uint64 outputVertexCount = 0;
    uint64 blasGeneration = 0;
    nvrhi::BufferHandle vertexBuffer;
    nvrhi::BufferHandle indexBuffer;
    nvrhi::rt::AccelStructDesc blasDesc;
    nvrhi::rt::AccelStructHandle blas;
};

struct RtRetiredSmokeSkinnedComparisonBlasPackage
{
    uint64 retireFrame = 0;
    uint64 completionToken = 0;
    uint64 stateRetirementToken = 0;
    bool completionArmed = false;
    nvrhi::EventQueryHandle completionQuery;
    RtSmokeSkinnedComparisonBlasResource resource;
};

static constexpr int RT_SMOKE_RIGID_ROUTE_SIDE_BUFFER_SLOTS = 3;

template< typename Result >
class RtPathTraceAsyncWorker
{
public:
    RtPathTraceAsyncWorker() = default;
    ~RtPathTraceAsyncWorker()
    {
        Stop();
    }

    RtPathTraceAsyncWorker(const RtPathTraceAsyncWorker&) = delete;
    RtPathTraceAsyncWorker& operator=(const RtPathTraceAsyncWorker&) = delete;

    bool valid() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_jobQueued || m_running || m_resultReady;
    }

    template< typename Rep, typename Period >
    std::future_status wait_for(const std::chrono::duration<Rep, Period>&) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_resultReady ? std::future_status::ready : std::future_status::timeout;
    }

    Result get()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_resultReady = false;
        return std::move(m_result);
    }

    template< typename Function >
    bool Start(Function&& function)
    {
        EnsureThreadStarted();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopRequested || m_jobQueued || m_running || m_resultReady)
            {
                return false;
            }

            m_job = std::function<Result()>(std::forward<Function>(function));
            m_jobQueued = true;
        }

        m_condition.notify_one();
        return true;
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopRequested = true;
            m_jobQueued = false;
            m_resultReady = false;
            m_job = std::function<Result()>();
        }
        m_condition.notify_all();

        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    void Reset()
    {
        Stop();

        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopRequested = false;
        m_result = Result();
    }

private:
    void EnsureThreadStarted()
    {
        if (!m_thread.joinable())
        {
            m_thread = std::thread([this]() {
                ThreadMain();
            });
        }
    }

    void ThreadMain()
    {
        for (;;)
        {
            std::function<Result()> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_condition.wait(lock, [this]() {
                    return m_stopRequested || m_jobQueued;
                });
                if (m_stopRequested && !m_jobQueued)
                {
                    return;
                }

                job = std::move(m_job);
                m_jobQueued = false;
                m_running = true;
            }

            Result result = job();

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_running = false;
                if (!m_stopRequested)
                {
                    m_result = std::move(result);
                    m_resultReady = true;
                }
            }
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::thread m_thread;
    std::function<Result()> m_job;
    Result m_result;
    bool m_jobQueued = false;
    bool m_running = false;
    bool m_resultReady = false;
    bool m_stopRequested = false;
};

struct RtSmokeRigidRouteSideBufferSlot
{
    nvrhi::BufferHandle vertexBuffer;
    nvrhi::BufferHandle indexBuffer;
    nvrhi::BufferHandle triangleMaterialBuffer;
    nvrhi::BufferHandle triangleMaterialIndexBuffer;
    nvrhi::BufferHandle instanceBuffer;
    RtPathTraceCpuWorkGeneration generation;
    uint64 geometryUploadSignature = 0;
    uint64 instanceUploadSignature = 0;
    bool generationValid = false;
    bool geometryUploadSignatureValid = false;
    bool instanceUploadSignatureValid = false;
};

struct RtPathTraceRigidTlasBackendJob
{
    idParallelJobList* jobList = nullptr;
    RtSmokeRigidTlasPlanSnapshot snapshot;
    RtSmokeRigidTlasPlanTimedResult result;
};

class PathTracePrimaryPass {
public:
    explicit PathTracePrimaryPass(idRenderBackend* backend);
    ~PathTracePrimaryPass();

    // Called every frame when r_pathTracing >= 1
    void Execute(const viewDef_t* viewDef);
    void OnGraphicsCommandListSubmitted();
    void InvalidateForBackBufferResize();
    void PresentDebugOutput();
    void BlitDebugOutput(nvrhi::IFramebuffer* targetFramebuffer, const nvrhi::Viewport& targetViewport);
    void TonemapDebugOutput(TonemapPass* tonemapPass, const viewDef_t* viewDef, nvrhi::IFramebuffer* targetFramebuffer);
    void DrawBoundsOverlayRaster(nvrhi::IFramebuffer* targetFramebuffer, const nvrhi::Viewport& targetViewport);

private:
    void InitRayTracingSmokeTest();
    bool EnsureSmokeTlasCapacity(uint32_t neededInstances, RtSmokeTlasCapacityCandidate& candidate);
    bool InitRayTracingSmokeRestirPipeline(int restirLibraryKind);
    bool ResizeRayTracingSmokeOutput(int width, int height, int outputWidth, int outputHeight);
    void ResetRayTracingSmokeAsyncCpuWork();
    void ResetRigidTlasBackendJob();
    bool EnsureBackendParallelV1JobList();
    void ResetBackendParallelV1JobList();
    bool EnsureRigidRouteAppendJobList();
    void ResetRigidRouteAppendJobList();
    void ResetRayTracingSmokeSceneResources();
    void CommitRayTracingSmokeSceneResources(const RtSmokeSceneResourceCommitDesc& desc);
    bool HasRetainableRayTracingSmokeScenePackage() const;
    RtRetiredSmokeScenePackage CaptureRetiredRayTracingSmokeScenePackage() const;
    void PushRetiredRayTracingSmokeScenePackage(RtRetiredSmokeScenePackage& package, uint64 currentFrame, int retireFrames);
    int ReleaseExpiredRetiredRayTracingSmokeScenePackages(uint64 currentFrame);
    void PushRetiredStaticBucketGpuResources(
        RtSmokeRetiredStaticBucketGpuResources& resources,
        uint64 currentFrame);
    int ReleaseCompletedRetiredStaticBucketGpuResources(
        uint64 currentFrame);
    void PushRetiredRigidGpuResources(
        RtSmokeRetiredRigidGpuResources& resources,
        uint64 currentFrame);
    int ReleaseCompletedRetiredRigidGpuResources(
        uint64 currentFrame);
    int ReleaseCompletedRetiredSmokeSkinnedComparisonBlases(uint64 currentFrame);
    void BuildRayTracingSmokeTestScene(const viewDef_t* viewDef);
    bool TryBuildCpuProducerRewriteScene(const viewDef_t* viewDef, nvrhi::IDevice* device, nvrhi::ICommandList* commandList);
    struct RtCpuRewriteSkinnedTransaction;
    bool CommitRewriteSkinnedGpuSkinAndHitRoute(
        nvrhi::IDevice* device,
        nvrhi::ICommandList* commandList,
        const RtCpuRewriteFrozenProductView* view,
        const RtCpuRewriteJoinResult& join,
        uint32_t rigidExtraCount,
        int selectedSlot,
        RtCpuRewriteSkinnedTransaction& transaction,
        std::vector<nvrhi::rt::InstanceDesc>& extraTlas);
    void FinalizeRewriteSkinnedTransaction(RtCpuRewriteSkinnedTransaction& transaction);
    void ExecuteRayTracingSmokeTest(const viewDef_t* viewDef);
    void ReadBackRayTracingSmokeTest();
    void ReadBackSkyCubeProbe();
    void ReadBackLiquidPoolStatus();
    void ReadBackStaticContractShaderSample();
    void QueueStaticContractShaderSample(nvrhi::ICommandList* commandList);
    void ReadBackStaticContractGeometrySample();
    void QueueStaticContractGeometrySample(
        nvrhi::ICommandList* commandList,
        nvrhi::IBuffer* staticVertexBuffer,
        nvrhi::IBuffer* staticIndexBuffer,
        const PathTraceSmokeVertex* staticVertices,
        int vertexOffset,
        int vertexCount,
        const uint32_t* staticIndexes,
        int indexOffset,
        int indexCount,
        uint64 frameIndex);
    struct GpuSkinningParitySample
    {
        idStr modelName;
        int entityIndex = -1;
        int drawSurfIndex = -1;
        int surfaceRecordIndex = -1;
        int vertexIndex = -1;
        uint64 currentByteOffset = 0;
        uint64 previousByteOffset = 0;
        uint32_t previousInvalidReasonFlags = 0;
        uint32_t temporalStateFlags = 0;
        uint32_t materialIndex = UINT32_MAX;
        bool hasPrevious = false;
        bool hasDynamicTexMatrix = false;
        PathTraceSkinnedSourceVertex source = {};
        PathTraceSmokeVertex cpuCurrent = {};
        PathTraceSkinnedPreviousPosition cpuPrevious = {};
    };
    struct CanonicalRigidHitRouteContext
    {
        uint32_t instanceId = 0;
        uint64 sourceInstanceId = 0;
        uint64 legacyMeshHash = 0;
        uint64 canonicalMeshHash = 0;
        uint32_t routeRecordIndex = UINT32_MAX;
        uint32_t canonicalBlasRecordIndex = UINT32_MAX;
        uint64 currentTransformHash = 0;
        uint64 previousTransformHash = 0;
        uint32_t flags = 0;
    };
    struct CanonicalRigidHitEmissiveContext
    {
        uint32_t instanceId = 0;
        uint32_t primitiveIndex = 0;
        uint64 identity = 0;
        uint32_t materialId = 0;
        uint32_t materialIndex = UINT32_MAX;
        uint32_t emissiveTextureIndex = UINT32_MAX;
    };
    void QueueGpuSkinningParitySamples(
        nvrhi::ICommandList* commandList,
        nvrhi::IBuffer* currentOutputBuffer,
        nvrhi::IBuffer* previousPositionBuffer,
        nvrhi::ResourceStates currentRestoreState,
        const std::vector<GpuSkinningParitySample>& samples,
        int mode,
        uint64 frameIndex);
    void ReadBackGpuSkinningParitySamples();
    void QueueSkinnedEmissiveAudit(
        nvrhi::ICommandList* commandList,
        nvrhi::IBuffer* currentOutputBuffer,
        nvrhi::IBuffer* previousPositionBuffer,
        nvrhi::ResourceStates currentRestoreState,
        const std::vector<PtSkinnedEmissiveAuditTriangle>& triangles,
        const std::vector<uint32_t>& materialIds,
        const std::vector<PathTraceSmokeMaterial>& materials,
        const std::vector<PathTraceSmokeVertex>& cpuCurrentVertices,
        const std::vector<PathTraceSkinnedPreviousPosition>& cpuPreviousPositions,
        uint64 frameIndex,
        int forcedMaterialCount,
        int productionEligibleMaterialCount);
    void ReadBackSkinnedEmissiveAudit();
    void QueueSkinnedEmissivePublishAudit(
        nvrhi::ICommandList* commandList,
        nvrhi::IBuffer* emissiveTriangleBuffer,
        nvrhi::IBuffer* previousEmissiveTriangleBuffer,
        uint32 firstCurrentRecord,
        uint32 firstPreviousRecord,
        uint32 recordCount);
    void QueueSkinnedHitRouteReadback(
        nvrhi::ICommandList* commandList,
        nvrhi::IBuffer* recordBuffer,
        nvrhi::IBuffer* triangleBuffer,
        const PtSkinnedHitRouteGpuUpload& expected,
        uint64 frameIndex);
    void ReadBackSkinnedHitRoute();
    void QueueSkinnedHitAuditSamples(nvrhi::ICommandList* commandList);
    void ReadBackSkinnedHitAuditSamples();
    void ExecutePathTraceParticleComposite(nvrhi::ICommandList* commandList, const viewDef_t* viewDef);

    idRenderBackend* m_backend;
    bool m_reportedMode;
    bool m_rayTracingSupported;
    bool m_smokeTestInitialized;
    bool m_smokeSceneBuilt;
    bool m_smokeSceneRebuildLogged;
    bool m_smokeTestDispatched;
    int m_unifiedPtFrozenSceneCapturedMode = 0;
    uint64 m_unifiedPtFrozenSceneReplayFrames = 0;
    int m_unifiedPtFrozenSceneReportedMode = -1;
    int m_unifiedPtFrozenSceneReleaseAuditFromMode = 0;
    int m_unifiedPtFrozenSceneReleaseAuditFramesRemaining = 0;
    bool m_smokeWaitingForDoomSurfaceLogged;
    int m_smokeSceneLogCooldownFrames;
    bool m_smokeStaticBlasCacheValid;
    uint64 m_smokeStaticBlasSignature;
    uint64 m_smokeStaticBlasOpacitySignature;
    uint64 m_smokeStaticBlasGeometryGeneration;
    int m_smokeStaticBlasCacheHitCount;
    int m_smokeStaticBlasCacheMissCount;
    uint64 m_smokeGeometryFrameIndex;
    int m_smokeSceneSourceLast;
    int m_smokeSceneSource2RigidEntitiesLast;
    int m_smokeLiquidPoolOffsetEnabledLast;
    int m_unifiedPtRemoveAlphaClipSurfacesLast;
    uint64 m_smokeSceneUniverseStaticBuildGeneration;
    RtPathTraceCpuWorkState m_smokeCpuWorkState;
    RtPathTraceCpuWorkState m_smokeRigidTlasCpuWorkState;
    RtPathTraceCpuWorkState m_smokeBvhFramePlanningCpuWorkState;
    RtSmokeBvhDirtyTokenState m_smokeBvhDirtyPreviousToken;
    RtPathTraceCpuWorkGeneration m_smokeAccelerationPlanAsyncGeneration;
    RtPathTraceCpuWorkGeneration m_smokeAccelerationPlanAsyncCachedGeneration;
    RtPathTraceCpuWorkTiming m_smokeAccelerationPlanAsyncTiming;
    RtSmokeAccelerationPlan m_smokeAccelerationPlanAsyncCachedPlan;
    RtPathTraceAsyncWorker<RtSmokeAccelerationPlanTimedResult> m_smokeAccelerationPlanFuture;
    int m_smokeAccelerationPlanAsyncLaunchMs = 0;
    bool m_smokeAccelerationPlanAsyncGenerationValid = false;
    bool m_smokeAccelerationPlanAsyncCachedPlanValid = false;
    RtPathTraceCpuWorkGeneration m_smokeRigidTlasPlanAsyncGeneration;
    RtPathTraceCpuWorkGeneration m_smokeRigidTlasPlanAsyncCachedGeneration;
    RtPathTraceCpuWorkTiming m_smokeRigidTlasPlanAsyncTiming;
    RtSmokeRigidTlasPlan m_smokeRigidTlasPlanAsyncCachedPlan;
    RtPathTraceAsyncWorker<RtSmokeRigidTlasPlanTimedResult> m_smokeRigidTlasPlanFuture;
    RtPathTraceRigidTlasBackendJob m_smokeRigidTlasBackendJob;
    bool m_smokeRigidTlasDedicatedWorkerDisabledForBackendPool = false;
    idParallelJobList* m_backendParallelV1JobList = nullptr;
    RtPathTraceBackendJobListPhaseState m_backendParallelV1PhaseState =
        RtPathTraceBackendJobListPhaseState::Idle;
    idParallelJobList* m_rigidRouteAppendJobList = nullptr;
    RtPathTraceBackendJobListPhaseState m_rigidRouteAppendJobPhaseState =
        RtPathTraceBackendJobListPhaseState::Idle;
    bool m_backendParallelV1RigidTlasPending = false;
    bool m_backendParallelV1RigidTlasReady = false;
    bool m_smokeRigidRouteDedicatedWorkerDisabledForBackendV1 = false;
    bool m_smokeProducerLaneBOwnershipActive = false;
    RtPathTraceAccelCpuResidentPublisher m_smokeAccelCpuResidentPublisher;
    bool m_smokeProducerLaneBBootstrapComplete = false;
    int m_smokeRigidTlasPlanAsyncLaunchMs = 0;
    bool m_smokeRigidTlasPlanAsyncGenerationValid = false;
    bool m_smokeRigidTlasPlanAsyncCachedPlanValid = false;
    RtPathTraceCpuWorkState m_smokeRigidRouteBuildCpuWorkState;
    RtPathTraceCpuWorkGeneration m_smokeRigidRouteBuildAsyncGeneration;
    RtPathTraceCpuWorkGeneration m_smokeRigidRouteBuildAsyncCachedGeneration;
    RtPathTraceCpuWorkTiming m_smokeRigidRouteBuildAsyncTiming;
    RtPathTraceRigidRouteBuild m_smokeRigidRouteBuildAsyncCachedBuild;
    uint64_t m_smokeRigidRouteBuildAsyncCachedGeometryUploadSignature = 0;
    uint64_t m_smokeRigidRouteBuildAsyncCachedInstanceUploadSignature = 0;
    RtPathTraceAsyncWorker<RtPathTraceRigidRouteBuildTimedResult> m_smokeRigidRouteBuildFuture;
    int m_smokeRigidRouteBuildAsyncLaunchMs = 0;
    bool m_smokeRigidRouteBuildAsyncGenerationValid = false;
    bool m_smokeRigidRouteBuildAsyncCachedBuildValid = false;
    bool m_smokeRigidRouteBuildAsyncCachedGeometryUploadSignatureValid = false;
    bool m_smokeRigidRouteBuildAsyncCachedInstanceUploadSignatureValid = false;
    RtPathTraceCpuWorkGeneration m_smokeBvhFramePlanningAsyncGeneration;
    RtPathTraceCpuWorkGeneration m_smokeBvhFramePlanningAsyncCachedGeneration;
    RtPathTraceCpuWorkTiming m_smokeBvhFramePlanningAsyncTiming;
    RtSmokeBvhFramePlanningResult m_smokeBvhFramePlanningAsyncCachedResult;
    RtPathTraceAsyncWorker<RtSmokeBvhFramePlanningTimedResult> m_smokeBvhFramePlanningFuture;
    int m_smokeBvhFramePlanningAsyncLaunchMs = 0;
    bool m_smokeBvhFramePlanningAsyncGenerationValid = false;
    bool m_smokeBvhFramePlanningAsyncCachedResultValid = false;
    bool m_smokeBvhDirtyPreviousTokenValid = false;
    const void* m_smokeSceneRenderWorld = nullptr;
    idStr m_smokeSceneMapName;
	ID_TIME_T m_smokeSceneMapTimeStamp = 0;
	uint64 m_smokeSceneMapLoadSerial = 0;
	idStr m_pathTracePostLutName;
	nvrhi::TextureHandle m_pathTracePostLutTexture;
	int m_pathTracePostLutWidth = 0;
	int m_pathTracePostLutHeight = 0;
    bool m_pathTracePostLutInvalidLogged = false;
    RtSmokeGeometryUniverse m_smokeGeometryUniverse;
    RtSmokeGeometryUniverse m_staticBucketGeometryUniverse;
    RtPathTraceParticleCapture m_particleCapture;
    nvrhi::BindingLayoutHandle m_particleLightingBindingLayout;
    nvrhi::ShaderHandle m_particleLightingShader;
    nvrhi::ComputePipelineHandle m_particleLightingPipeline;
    nvrhi::BufferHandle m_particleLightingTaskBuffer;
    nvrhi::BufferHandle m_particleLightingOutputBuffer;
    nvrhi::BufferHandle m_particleLightingHistoryBuffer;
    std::vector<ParticleCompositeLightingTask> m_particleLightingPreviousTasks;
    uint64 m_particleLightingHistoryMapLoadSerial = 0;
    uint64 m_particleLightingHistorySettingsSignature = 0;
    int m_particleLightingHistoryFrame = -1;
    nvrhi::BindingLayoutHandle m_particleCompositeBindingLayout;
    nvrhi::ShaderHandle m_particleCompositeVertexShader;
    nvrhi::ShaderHandle m_particleCompositePixelShader;
    nvrhi::GraphicsPipelineHandle m_particleCompositePipelines[static_cast<int>(RtPathTraceParticleBlendClass::Count)];
    nvrhi::FramebufferHandle m_particleCompositeFramebuffer;
    nvrhi::TextureHandle m_particleCompositeFramebufferTexture;
    nvrhi::BufferHandle m_particleCompositeVertexBuffer;
    nvrhi::BufferHandle m_particleCompositeIndexBuffer;
    struct ParticleCompositeCachedBinding
    {
        nvrhi::BindingSetDesc desc;
        nvrhi::BindingSetHandle bindingSet;
    };
    std::vector<ParticleCompositeCachedBinding> m_particleCompositeCachedBindings;
    int m_particleDiagnosticFramesRemaining = 0;
    static constexpr int PARTICLE_COMPOSITE_GPU_TIMER_SLOTS = 16;
    struct ParticleCompositeGpuTimerSlot
    {
        nvrhi::TimerQueryHandle query;
        bool pending = false;
        int frame = 0;
        uint32_t invocation = 0;
        uint32_t batches = 0;
        uint32_t vertices = 0;
        uint32_t indexes = 0;
        uint32_t lightingTasks = 0;
        uint32_t draws = 0;
        int earliestPollFrame = 0;
    };
    ParticleCompositeGpuTimerSlot m_particleCompositeGpuTimers[PARTICLE_COMPOSITE_GPU_TIMER_SLOTS];
    uint32_t m_particleCompositeGpuTimerCursor = 0;
    uint32_t m_particleCompositeGpuTimerInvocation = 0;
    static constexpr int GEOMETRY_SKINNED_GPU_TIMER_SLOTS = 32;
    struct GeometrySkinnedGpuTimerSlot
    {
        nvrhi::TimerQueryHandle query;
        bool pending = false;
        bool skinnedBlas = false;
        bool submitted = false;
        int earliestPollFrame = 0;
        uint64 geometryFrame = 0;
        uint32 buildCount = 0;
        uint32 updateCount = 0;
        uint32 rebuildCount = 0;
        uint32 reuseCount = 0;
        uint32 skinnedSurfaceCount = 0;
        uint32 skinnedSourceIndexes = 0;
        uint32 cpuCapturedSkinnedIndexes = 0;
        uint32 dynamicBlasIndexes = 0;
        uint64 cpuSkinUs = 0;
        uint64 cpuBlasSubmitUs = 0;
        uint64 cpuTlasSubmitUs = 0;
        uint64 cpuAccelSubmitUs = 0;
    };
    GeometrySkinnedGpuTimerSlot
        m_geometrySkinnedGpuTimers[
            GEOMETRY_SKINNED_GPU_TIMER_SLOTS];
    uint32_t m_geometrySkinnedGpuTimerCursor = 0;
    std::vector<RtSmokeSkinnedSurfaceRecord> m_smokeSkinnedSurfaceRecords;
    PtSkinnedOutputAllocator m_smokeSkinnedOutputAllocator;
    PtSkinnedBlasStateTable m_smokeSkinnedBlasStateTable;
    struct SmokeSkinnedCaptureRouteSetState
    {
        uint64 signature = 0;
        uint64 lastUsedFrame = 0;
        PtSkinnedHitRouteBuild pendingBuild;
        uint64 pendingBuildSignature = 0;
        PtSkinnedHitRouteBuild acceptedBuild;
        uint64 acceptedBuildSignature = 0;
    };
    std::vector<SmokeSkinnedCaptureRouteSetState>
        m_smokeSkinnedCaptureRouteSets;
    std::vector<RtSmokeSkinnedComparisonBlasResource>
        m_smokeSkinnedComparisonBlases;
    std::deque<RtRetiredSmokeSkinnedComparisonBlasPackage>
        m_retiredSmokeSkinnedComparisonBlases;
    bool m_smokeSkinnedComparisonBlasBuildLogged = false;
    bool m_smokeSkinnedComparisonBlasUpdateLogged = false;
    bool m_smokeSkinnedComparisonBlasRebuildLogged = false;
    bool m_smokeSkinnedComparisonBlasReplacementLogged = false;
    bool m_smokeSkinnedHitRouteShadowLogged = false;
    uint32 m_smokeSkinnedCaptureSplitShadowMaxLogged = 0;
    uint32 m_smokeSkinnedCaptureSplitTlasMaxLogged = 0;
    uint32 m_smokeSkinnedEmissivePublishMaxLogged = 0;
    int m_smokeSkinnedCaptureLastShadowAccepted = -1;
    uint32 m_smokeSkinnedCaptureShadowTransitionsLogged = 0;
    uint64 m_smokeSkinnedCaptureRouteSetEvictions = 0;
    uint64 m_smokeSkinnedCapturePreCaptureInvalidations = 0;
    uint64 m_smokeSkinnedCaptureLateRevocations = 0;
    uint64 m_smokeNextSkinnedComparisonCompletionToken = 1;
    uint64 m_smokeLastCompletedSkinnedComparisonToken = 0;
    bool m_smokeSkinnedComparisonCompletionQueryFailureLogged = false;
    bool m_smokeSkinnedComparisonCompletionArmedLogged = false;
    bool m_smokeSkinnedComparisonCompletionReleasedLogged = false;
    RtSmokeSkinnedHistoryState m_smokeLegacySkinnedHistoryState;
    std::vector<RtSmokeSkinnedHistoryState> m_smokeSkinnedHistoryStates;
    uint64 m_smokeSkinnedHistoryUpdateSerial = 0;
    int m_smokeSkinnedHistoryOwnerGateLast = -1;
    RtPathTraceSceneUniverse m_sceneUniverse;
    RtPathTraceInstanceUniverse m_instanceUniverse;
    PathTraceRemixFramePrepare m_remixFramePrepare;
    PathTraceRemixLightManager m_remixLightManager;
    uint32_t m_smokeTextureProbeMaterialId;
    int m_smokeTextureProbeRequestedIndex;
    // World-space capture/order anchor. This is not subtracted from geometry.
    idVec3 m_smokeCaptureAnchor;
    nvrhi::BufferHandle m_smokeStaticVertexBuffer;
    nvrhi::BufferHandle m_smokeStaticIndexBuffer;
    nvrhi::BufferHandle m_smokeStaticTriangleClassBuffer;
    nvrhi::BufferHandle m_smokeStaticTriangleMaterialBuffer;
    nvrhi::BufferHandle m_smokeStaticTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle m_smokePreviousStaticVertexBuffer;
    nvrhi::BufferHandle m_smokePreviousStaticIndexBuffer;
    nvrhi::BufferHandle m_smokePreviousStaticTriangleClassBuffer;
    nvrhi::BufferHandle m_smokePreviousStaticTriangleMaterialBuffer;
    nvrhi::BufferHandle m_smokePreviousStaticTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle m_smokeDynamicVertexBuffer;
    nvrhi::BufferHandle m_smokeDynamicIndexBuffer;
    nvrhi::BufferHandle m_smokeDynamicTriangleClassBuffer;
    nvrhi::BufferHandle m_smokeDynamicTriangleMaterialBuffer;
    nvrhi::BufferHandle m_smokeDynamicTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle m_smokeMaterialTableBuffer;
    nvrhi::BufferHandle m_smokeMaterialFeatureBuffer;
    nvrhi::BufferHandle m_smokeMaterialFeatureParameterBuffer;
    nvrhi::BufferHandle m_smokeDynamicMaterialBuffer;
    nvrhi::BufferHandle m_smokeEmissiveTriangleBuffer;
    nvrhi::BufferHandle m_smokePreviousEmissiveTriangleBuffer;
    nvrhi::BufferHandle m_smokeEmissiveRemapBuffer;
    nvrhi::BufferHandle m_smokeEmissiveDistributionBuffer;
    nvrhi::BufferHandle m_smokeLightCandidateBuffer;
    nvrhi::BufferHandle m_smokeDoomAnalyticLightBuffer;
    nvrhi::BufferHandle m_smokeDoomAnalyticPreviousLightBuffer;
    nvrhi::BufferHandle m_smokeDoomAnalyticCurrentIdentityBuffer;
    nvrhi::BufferHandle m_smokeDoomAnalyticPreviousIdentityBuffer;
    nvrhi::BufferHandle m_smokeDoomAnalyticRemapBuffer;
    nvrhi::BufferHandle m_smokeUnifiedLightBuffer;
    nvrhi::BufferHandle m_smokeUnifiedPreviousLightBuffer;
    nvrhi::BufferHandle m_smokeUnifiedLightRemapBuffer;
    nvrhi::BufferHandle m_smokeRestirLightManagerCurrentToPreviousBuffer;
    nvrhi::BufferHandle m_smokeRestirLightManagerPreviousToCurrentBuffer;
    nvrhi::BufferHandle m_smokeRestirLightManagerCurrentPayloadBuffer;
    nvrhi::BufferHandle m_smokeRestirLightManagerPreviousPayloadBuffer;
    nvrhi::BufferHandle m_smokeUnifiedPtEmissiveLookupBuffer;
    nvrhi::BufferHandle m_smokeUnifiedPtEmissiveGeometryBuffer;
    nvrhi::BufferHandle m_smokeRigidRouteVertexBuffer;
    nvrhi::BufferHandle m_smokeRigidRouteIndexBuffer;
    nvrhi::BufferHandle m_smokeRigidRouteTriangleMaterialBuffer;
    nvrhi::BufferHandle m_smokeRigidRouteTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle m_smokeRigidRouteInstanceBuffer;
    nvrhi::BufferHandle m_smokeSkinnedHitRouteRecordBuffer;
    nvrhi::BufferHandle m_smokeSkinnedHitRouteTriangleBuffer;
    RtSmokeRigidRouteSideBufferSlot m_smokeRigidRouteSideBufferSlots[RT_SMOKE_RIGID_ROUTE_SIDE_BUFFER_SLOTS];
    int m_smokeRigidRouteSideBufferReadSlot = -1;
    int m_smokeRigidRouteSideBufferWriteSlot = 0;
    nvrhi::BufferHandle m_smokeSkinnedSourceVertexBuffer;
    nvrhi::BufferHandle m_smokeSkinnedCurrentOutputVertexBuffer;
    uint64 m_smokeSkinnedOutputBufferGeneration = 0;
    uint64 m_smokeNextSceneCompletionToken = 1;
    uint64 m_smokeLastCompletedSceneToken = 0;
    bool m_smokeSceneCompletionQueryFailureLogged = false;
    bool m_smokeSceneCompletionArmedLogged = false;
    bool m_smokeSceneCompletionReleasedLogged = false;
    uint64 m_smokeNextStaticBucketCompletionToken = 1;
    uint64 m_smokeLastCompletedStaticBucketToken = 0;
    bool m_smokeStaticBucketCompletionQueryFailureLogged = false;
    uint64 m_smokeNextRigidCompletionToken = 1;
    uint64 m_smokeLastCompletedRigidToken = 0;
    bool m_smokeRigidCompletionQueryFailureLogged = false;
    nvrhi::BufferHandle m_smokeSkinnedPreviousPositionBuffer;
    nvrhi::BufferHandle m_smokeSkinnedSurfaceDispatchBuffer;
    nvrhi::BufferHandle m_smokeSkinnedTriangleDispatchIndexBuffer;
    nvrhi::BufferHandle m_smokeSkinnedCurrentJointMatrixBuffer;
    nvrhi::BufferHandle m_smokeSkinnedPreviousJointMatrixBuffer;
    nvrhi::BufferHandle m_smokeConstantsBuffer;
    nvrhi::BufferHandle m_smokeBoundsOverlayLineBuffer;
    nvrhi::BufferHandle m_liquidPoolStatusBuffer;
    nvrhi::BufferHandle m_liquidPoolStatusReadbackBuffer;
    bool m_liquidPoolStatusReadbackQueued = false;
    int m_liquidPoolStatusReadbackDelayFrames = 0;
    nvrhi::BufferHandle m_staticContractShaderReadbackBuffer;
    bool m_staticContractShaderReadbackRequested = false;
    bool m_staticContractShaderReadbackQueued = false;
    int m_staticContractShaderReadbackDelayFrames = 0;
    uint64 m_staticContractShaderSampleFrame = 0;
    int m_staticContractShaderSampleX = 0;
    int m_staticContractShaderSampleY = 0;
    int m_staticContractShaderSampleWidth = 0;
    int m_staticContractShaderSampleHeight = 0;
    uint32_t m_staticContractExpectedInstance = 0;
    uint32_t m_staticContractExpectedPrimitiveFirst = UINT32_MAX;
    uint32_t m_staticContractExpectedPrimitiveCount = 0;
    uint32_t m_staticContractExpectedMaterialId = 0;
    uint32_t m_staticContractExpectedMaterialIndex = UINT32_MAX;
    bool m_canonicalRigidHitSample = false;
    bool m_canonicalRigidHitTraversalSelected = false;
    uint32_t m_canonicalRigidHitFirstInstance = 0;
    uint32_t m_canonicalRigidHitInstanceCount = 0;
    std::vector<CanonicalRigidHitRouteContext>
        m_canonicalRigidHitRouteContexts;
    std::vector<CanonicalRigidHitEmissiveContext>
        m_canonicalRigidHitEmissiveContexts;
    nvrhi::BufferHandle m_staticContractGeometryReadbackBuffer;
    bool m_staticContractGeometryReadbackQueued = false;
    int m_staticContractGeometryReadbackDelayFrames = 0;
    uint64 m_staticContractGeometrySampleFrame = 0;
    int m_staticContractGeometryVertexOffset = 0;
    int m_staticContractGeometryIndexOffset = 0;
    std::vector<PathTraceSmokeVertex> m_staticContractGeometryCpuVertices;
    std::vector<uint32_t> m_staticContractGeometryCpuIndexes;
    nvrhi::BufferHandle m_gpuSkinningParityReadbackBuffer;
    bool m_gpuSkinningParityReadbackQueued = false;
    int m_gpuSkinningParityReadbackDelayFrames = 0;
    int m_gpuSkinningParityMode = 0;
    uint64 m_gpuSkinningParityFrame = 0;
    std::vector<GpuSkinningParitySample> m_gpuSkinningParitySamples;
    nvrhi::BufferHandle m_skinnedEmissiveAuditReadbackBuffer;
    bool m_skinnedEmissiveAuditReadbackQueued = false;
    int m_skinnedEmissiveAuditReadbackDelayFrames = 0;
    uint64 m_skinnedEmissiveAuditFrame = 0;
    uint64 m_skinnedEmissiveAuditCurrentBytes = 0;
    uint64 m_skinnedEmissiveAuditPreviousBytes = 0;
    int m_skinnedEmissiveAuditForcedMaterialCount = 0;
    int m_skinnedEmissiveAuditProductionEligibleMaterialCount = 0;
    std::vector<PtSkinnedEmissiveAuditTriangle>
        m_skinnedEmissiveAuditTriangles;
    std::vector<uint32_t> m_skinnedEmissiveAuditMaterialIds;
    std::vector<PathTraceSmokeMaterial>
        m_skinnedEmissiveAuditMaterials;
    std::vector<PathTraceSmokeVertex>
        m_skinnedEmissiveAuditExpectedCurrentVertices;
    std::vector<PathTraceSkinnedPreviousPosition>
        m_skinnedEmissiveAuditExpectedPreviousPositions;
    PtSkinnedEmissiveAuditInventory
        m_skinnedEmissiveAuditExpected;
    nvrhi::BufferHandle
        m_skinnedEmissivePublishAuditReadbackBuffer;
    bool m_skinnedEmissivePublishAuditReadbackQueued = false;
    uint32 m_skinnedEmissivePublishAuditRecordCount = 0;
    nvrhi::BufferHandle m_skinnedHitRouteReadbackBuffer;
    bool m_skinnedHitRouteReadbackQueued = false;
    bool m_skinnedHitRouteReadbackCompleted = false;
    int m_skinnedHitRouteReadbackDelayFrames = 0;
    uint64 m_skinnedHitRouteReadbackFrame = 0;
    PtSkinnedHitRouteGpuUpload m_skinnedHitRouteReadbackExpected;
    nvrhi::BufferHandle m_skinnedHitAuditReadbackBuffer;
    bool m_skinnedHitAuditRequested = false;
    bool m_skinnedHitAuditReadbackQueued = false;
    int m_skinnedHitAuditReadbackDelayFrames = 0;
    uint64 m_skinnedHitAuditFrame = 0;
    int m_skinnedHitAuditWidth = 0;
    int m_skinnedHitAuditHeight = 0;
    PtSkinnedHitRouteBuild m_skinnedHitAuditLegacyShadow;
    uint32_t m_liquidPoolLastExceptionalMask[8] = {};
    uint32_t m_liquidPoolLastOverflowCount[8] = {};
    nvrhi::BufferHandle m_smokeCleanRtxdiDiCurrentReservoirBuffer;
    nvrhi::BufferHandle m_smokeCleanRtxdiDiTemporalReservoirBuffer;
    nvrhi::BufferHandle m_smokeCleanRtxdiDiPreviousReservoirBuffer;
    nvrhi::BufferHandle m_smokeCleanRtxdiDiSpatialReservoirBuffer;
    PathTraceBlueNoiseState m_smokeCleanRtxdiDiBlueNoise;
    PathTraceUnifiedPtState m_unifiedPtState;
    PathTraceCleanRestirGiState m_cleanRestirGiState;
    PathTraceNeeCacheState m_smokeNeeCacheState;
    PathTraceReGIRState m_smokeReGIRState;
    uint32_t m_smokeCleanRtxdiDiCurrentReservoirCount = 0;
    uint32_t m_smokeCleanRtxdiDiTemporalReservoirCount = 0;
    uint32_t m_smokeCleanRtxdiDiPreviousReservoirCount = 0;
    uint32_t m_smokeCleanRtxdiDiSpatialReservoirCount = 0;
    uint64_t m_smokeCleanRtxdiDiCurrentReservoirBytes = 0;
    uint64_t m_smokeCleanRtxdiDiTemporalReservoirBytes = 0;
    uint64_t m_smokeCleanRtxdiDiPreviousReservoirBytes = 0;
    uint64_t m_smokeCleanRtxdiDiSpatialReservoirBytes = 0;
    uint32_t m_smokeCleanRtxdiDiFrameIndex = 0;
    bool m_smokeCleanRtxdiDiPreviousReservoirValid = false;
    uint64 m_smokeCleanRtxdiDiHistorySignature = 0;
    uint32_t m_smokeCleanRtxdiDiHistoryResetCount = 0;
    uint32_t m_smokeCleanRtxdiDiPreviousReservoirResetReason = 0;
    std::vector<RtPathTraceBoundsOverlayLine> m_smokeBoundsOverlayLines;
    int m_smokeBoundsOverlayLineCount = 0;
    bool m_smokeBoundsOverlayViewValid = false;
    float m_smokeBoundsOverlayModelViewMatrix[16] = {};
    float m_smokeBoundsOverlayProjectionMatrix[16] = {};
    RtPathTraceFrameResources m_frameResources;
    RtPathTraceSceneInputs m_sceneInputs;

    struct RtCpuRewriteRetainedDedicatedMesh
    {
        uint64_t sourceAssetId = 0;
        uint64_t sourceAssetGeneration = 0;
        uint64_t topologySignature = 0;
        uint64_t worldGeneration = 0;
        uint64_t contentSignature = 0;
        uint32_t modelSurfaceIndex = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t packedVertexOffset = 0;
        uint32_t packedIndexOffset = 0;
        uint32_t packedTriangleOffset = 0;
        uint64_t bytes = 0;
        bool referenced = false;
        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;
        nvrhi::rt::AccelStructHandle blas;
    };
    std::vector<RtCpuRewriteRetainedDedicatedMesh> m_rewriteDedicatedMeshes;
    // GPU geometry whose last CPU reference was dropped while a committed TLAS
    // may still hold its BLAS device address. NVRHI does not pin BLASes for the
    // lifetime of a TLAS, so release waits for a newer TLAS commit plus frames.
    struct RtCpuRewriteRetiredGpuGeometry
    {
        uint64_t retireSerial = 0;
        uint64_t releaseFrame = 0;
        nvrhi::rt::AccelStructHandle blas;
        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;
        uint64_t bytes = 0;
        bool skinnedPair = false;
    };
    enum class RtCpuRewriteRetirementKind : uint32_t
    {
        Static = 0,
        Dedicated,
        Skinned
    };
    struct RtCpuRewriteRetirementCandidate
    {
        uint64_t retireSerial = 0;
        nvrhi::rt::AccelStructHandle blas;
        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;
        uint64_t logicalBytes = 0;
        RtCpuRewriteRetirementKind kind = RtCpuRewriteRetirementKind::Dedicated;
    };
    std::vector<RtCpuRewriteRetiredGpuGeometry> m_rewriteRetiredGeometry;
    bool TryRetireRewriteGpuGeometryBatch(
        const std::vector<RtCpuRewriteRetirementCandidate>& batch,
        bool forceDrain,
        RtCpuRewriteRetirementDecision* decision = nullptr);
    bool PreflightRewriteGpuGeometryBatch(
        const std::vector<RtCpuRewriteRetirementCandidate>& batch,
        RtCpuRewriteRetirementDecision* decision = nullptr);
    void EnqueuePreflightedRewriteGpuGeometryBatch(
        const std::vector<RtCpuRewriteRetirementCandidate>& batch);
    void PublishRewriteRetirementTelemetry();
    void PublishRewriteSkinnedTelemetry();
    void ReleaseExpiredRewriteGpuGeometry(uint64_t currentFrame, bool forceSchedule);
    nvrhi::BufferHandle m_rewriteEmptySkinnedHitRouteBuffer;
    nvrhi::BufferHandle m_rewriteEmptySkinnedHitRouteTriangleBuffer;
    nvrhi::BufferHandle m_rewriteLastBindingSkinnedRecordBuffer;
    nvrhi::BufferHandle m_rewriteLastBindingSkinnedIndexBuffer;
    nvrhi::BufferHandle m_rewriteLastBindingSkinnedOutputBuffer;
    nvrhi::BufferHandle m_rewritePackedRouteVertexBuffer;
    nvrhi::BufferHandle m_rewritePackedRouteIndexBuffer;
    nvrhi::BufferHandle m_rewritePackedRouteTriMatBuffer;
    nvrhi::BufferHandle m_rewritePackedRouteTriMatIndexBuffer;
    nvrhi::BufferHandle m_rewritePackedRouteInstanceBuffer;
    RtSmokeDynamicGeometryBuffers m_rewriteStaticDynamic;
    nvrhi::rt::AccelStructHandle m_rewriteStaticBlas;
    nvrhi::rt::AccelStructDesc m_rewriteStaticBlasDesc;
    uint64_t m_rewriteStaticLogicalBytes = 0;
    RtCpuRewriteRetirementLedger m_rewriteRetirementLedger;
    bool m_rewriteRetirementWarningLatched = false;
    bool m_rewriteRetirementRejectedThisAttempt = false;
    uint64_t m_rewriteGpuRigidRetainBytes = 0;
    bool m_rewriteHasRetainedPackage = false;
    struct RtCpuRewriteIsolatedTlasSlot
    {
        nvrhi::rt::AccelStructHandle tlas;
        RtPathTraceBindingReuseReceipt sceneBindingReceipt;
        uint32_t maxInstances = 0;
        uint64_t lastCommittedSerial = 0;
    };
    RtCpuRewriteIsolatedTlasSlot m_rewriteTlasSlots[3];
    uint64_t m_rewriteTlasCommitSerial = 0;
    uint64_t m_rewriteLastStaticSignature = 0;
    uint64_t m_rewriteStaticMaterialSignature = 0;
    std::vector<uint64_t> m_rewriteStaticSurfaceMaterialSignatures;
    uint64_t m_rewriteMaterialConfiguration = 0;
    std::vector<uint64_t> m_rewriteMaterialRowSignatures;
    struct RtCpuRewriteMaterialGpuSlot
    {
        nvrhi::BufferHandle buffers[4];
        std::vector<uint8_t> contents[4];
    };
    RtCpuRewriteMaterialGpuSlot m_rewriteMaterialGpuSlots[3];
    struct RtCpuRewriteLightGpuSlot
    {
        nvrhi::BufferHandle buffers[19];
        // Exact upload receipts, bounded to 8 MiB per idle slot.
        std::vector<uint8_t> contents[19];
    };
    struct RtCpuRewriteLightCandidate
    {
        RtCpuRewriteLightGpuSlot gpu;
        RtPathTraceSceneInputLights inputs;
        std::vector<PathTraceSmokeEmissiveTriangle> emissives;
        PathTraceRemixLightManagerPrepareResult manager;
        PathTraceRemixFramePrepare frame;
        uint64_t uploadBytes = 0;
        int portalAnalyticCount = 0;
    };
    RtCpuRewriteLightGpuSlot m_rewriteLightGpuSlots[3];
    std::vector<PathTraceSmokeEmissiveTriangle> m_rewritePreviousEmissives;
    bool m_rewriteLightHistoryValid = false;
    uint64_t m_rewriteLightWorld = 0, m_rewriteLightMap = 0;
    struct RtCpuRewriteLightWork;
    bool PrepareRewriteAnalyticLighting(const viewDef_t* viewDef,
        const RtCpuRewriteFrozenProductView& product, const RtCpuRewriteOverlayView* overlay,
        bool allowHistory, uint64_t frameIndex, std::shared_ptr<RtCpuRewriteLightWork>& work);
    bool BeginRewriteLighting(const viewDef_t* viewDef, const RtCpuRewriteFrozenProductView& product,
        const RtCpuRewriteOverlayView* overlay, const RtCpuRewriteJoinResult& join,
        const std::vector<uint32_t>& staticMaterialIds,
        const std::vector<RtCpuRewriteTextureMatrices>& staticMatrices,
        const std::vector<RtCpuRewriteMaterialBinding>& materialBindings,
        const RtSmokeMaterialTableBuild& materialTable, bool haveSkinned, int idleSlot,
        uint64_t frameIndex, RtCpuProducerRewriteService& service, std::shared_ptr<RtCpuRewriteLightWork>& work);
    bool FinishRewriteLighting(nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
        int idleSlot, uint64_t rootFrame, RtCpuProducerRewriteService& service,
        const std::shared_ptr<RtCpuRewriteLightWork>& work, RtCpuRewriteLightCandidate& candidate);
    uint32_t m_rewriteLastPackedVertexCount = 0;
    uint32_t m_rewriteLastPackedIndexCount = 0;
    uint32_t m_rewriteLastPackedTriangleCount = 0;
    using RtCpuRewritePackedLayoutRecord = RtCpuRewritePackedRangeWitness;
    std::vector<RtCpuRewriteMaterialBinding> m_rewriteMaterialBindings;
    nvrhi::BufferHandle m_rewriteMaterialTableOwner;
    uint64_t m_rewriteMaterialWorld = 0;
    uint64_t m_rewriteMaterialMap = 0;
    std::vector<RtCpuRewritePackedLayoutRecord> m_rewriteLastPackedLayout;
    nvrhi::BufferHandle m_rewriteLastCommittedPackedVertexBuffer;
    nvrhi::BufferHandle m_rewriteLastCommittedPackedIndexBuffer;
    nvrhi::BufferHandle m_rewriteLastCommittedPackedTriMatBuffer;
    nvrhi::BufferHandle m_rewriteLastCommittedPackedTriMatIndexBuffer;
    nvrhi::BufferHandle m_rewriteLastCommittedPackedInstanceBuffer;
    std::vector<uint64_t> m_rewriteLastCommittedBlasTokens;
    nvrhi::BufferHandle m_rewritePlaceholderStaticVertex;
    nvrhi::BindingSetHandle m_rewriteLastBindingSet;
    nvrhi::DescriptorTableHandle m_rewriteLastTextureDescriptorTable;
    nvrhi::rt::AccelStructHandle m_rewriteLastBindingTlas;
    bool m_rewriteLastTextureTableCreated = false;
    bool m_rewriteLastTextureTableWritten = false;
    RtSmokeMaterialTableBuild m_rewriteLastMaterialTable;
    RtSmokeMaterialTableBuild m_rewriteFrameMaterialTable;
    uint64_t m_rewriteFrameMaterialConfiguration = 0;
    struct RtCpuRewriteRetainedSkinnedJoints
    {
        PtCanonicalInstanceKey instanceKey;
        RtCpuRewriteSkinnedLayoutRow layout;
        float objectToWorld[16] = {};
        uint64_t rootFrame = 0;
        uint64_t epoch = 0;
        std::vector<PathTraceSkinnedJointMatrix> joints;
    };
    std::vector<RtCpuRewriteRetainedSkinnedJoints> m_rewritePreviousSkinnedJoints;
    nvrhi::BufferHandle m_rewriteSkinnedSourceVertexBuffer;
    nvrhi::BufferHandle m_rewriteSkinnedIndexBuffer;
    nvrhi::BufferHandle m_rewriteSkinnedOutputVertexBuffer;
    nvrhi::BufferHandle m_rewriteSkinnedDispatchBuffer;
    nvrhi::BufferHandle m_rewriteSkinnedJointBuffer;
    nvrhi::BufferHandle m_rewriteSkinnedPreviousPositionBuffer;
    nvrhi::BufferHandle m_rewriteSkinnedHitRouteRecordBuffer;
    nvrhi::BufferHandle m_rewriteSkinnedHitRouteTriangleBuffer;
    nvrhi::BindingSetHandle m_rewriteSkinnedGpuSkinningBindingSet;
    struct RtCpuRewriteSkinnedGpuSlot
    {
        nvrhi::rt::AccelStructHandle blas;
        nvrhi::BufferHandle indexBuffer;
        nvrhi::rt::AccelStructDesc desc;
        uint64_t bytes = 0;
        uint64_t allocationVertexCapacity = 0;
    };
    struct RtCpuRewriteSkinnedPackage
    {
        nvrhi::BufferHandle source, indexes, output, dispatch, joints, previous, records, triangles;
        nvrhi::BindingSetHandle computeBinding;
        std::vector<RtCpuRewriteSkinnedGpuSlot> meshes;
        RtCpuRewriteSkinnedLayout layout;
        uint64_t sourceGeneration = 0, outputGeneration = 0;
        uint64_t normalTextureSignature = 0;
        uint64_t logicalBytes = 0, cpuBytes = 0, lastCommittedSerial = 0;
        uint32_t usesSinceFullBuild = 0;
        uint32_t recordCount = 0, triangleCount = 0, sourceIndexCount = 0, outputCount = 0;
        bool contentsValid = true;
    };
    struct RtCpuRewriteSkinnedTransaction
    {
        RtCpuRewriteSkinnedPackage replacement;
        RtCpuRewriteSkinnedPackage* package = nullptr;
        std::vector<RtCpuRewriteRetainedSkinnedJoints> history;
        std::vector<RtCpuRewriteRetirementCandidate> retirement;
        int selectedSlot = -1;
        bool zero = false, replace = false, refresh = false, recorded = false, layoutMiss = false;
        RtCpuRewriteSkinnedLayoutComparison layoutComparison;
        uint64_t historyCpuBytes = 0, gpuPeak = 0, cpuPeak = 0;
        uint64_t stableUploadBytes = 0, exactUploadBytes = 0;
        uint64_t normalTextureSignature = 0;
        uint32_t bufferCreates = 0, bindingCreates = 0, blasCreates = 0;
        uint32_t blasReuses = 0;
        uint64_t allocationVertexCapacity = 0;
        uint32_t fullBuilds = 0, updates = 0, outcome = 0;
    };
    RtCpuRewriteSkinnedPackage m_rewriteSkinnedPackages[3];
    int m_rewriteCurrentSkinnedSlot = -1;
    uint64_t m_rewriteNextSkinnedGeneration = 1;
    uint64_t m_rewriteSkinnedHistoryCpuBytes = 0;
    int m_rewriteSkinnedHitRouteRecordCount = 0;
    int m_rewriteSkinnedHitRouteTriangleCount = 0;
    int m_rewriteSkinnedSourceIndexCount = 0;
    int m_rewriteSkinnedOutputVertexCount = 0;
    uint64_t m_rewriteSkinnedSourceIndexGeneration = 1;
    uint64_t m_rewriteSkinnedOutputStorageGeneration = 1;
    bool m_rewriteSkinnedGpuComputeDispatched = false;
    uint64_t m_rewriteSkinnedGpuBytes = 0;
    uint64_t m_rewriteSkinnedRetiredPairCount = 0;
    uint64_t m_rewriteSkinnedRetiredPairBytes = 0;
    uint64_t m_rewriteSkinnedRetiredPairHighWater = 0;
    uint64_t m_rewriteSkinnedRetiredPairBytesHighWater = 0;
    uint32_t m_rewriteSkinnedLastJoinCount = 0;
    uint32_t m_rewriteSkinnedKeepLastStreak = 0;
    uint32_t m_rewriteConsecutiveRejectedFrames = 0;
    RtCpuRewriteKeepLastFamily m_rewriteKeepLastFamily = RtCpuRewriteKeepLastFamily::None;
    bool m_rewriteKeepLastWarned = false;
    uint32_t m_rewriteSkinnedLastRejectReason = 0;
    uint64_t m_rewriteSkinnedDegradedCommits = 0;
    nvrhi::rt::AccelStructDesc m_smokeStaticBlasDesc;
    nvrhi::rt::AccelStructHandle m_smokeStaticBlas;
    nvrhi::rt::AccelStructHandle m_smokeDynamicBlas;
    nvrhi::rt::AccelStructHandle m_smokeTlas;
    uint32_t m_smokeTlasMaxInstances = 0;
    nvrhi::BindingLayoutHandle m_smokeBindingLayout;
    nvrhi::BindingLayoutHandle m_smokePdfNeeVerifierBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeCleanRtxdiDiSentinelBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeReGIRDebugBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeNeeCacheDebugBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeNeeCachePrimarySurfaceUpdateBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeSkinnedGpuSkinningBindingLayout;
    nvrhi::BindingLayoutHandle
        m_smokeSkinnedEmissivePublishBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeCleanRtxdiDiBoilingFilterBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeSkyCubeProbeBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeSkySurfaceResolveBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeTextureBindlessLayout;
    nvrhi::BindingSetHandle m_smokeBindingSet;
    nvrhi::BindingSetHandle m_smokeSkinnedGpuSkinningBindingSet;
    nvrhi::BindingSetHandle
        m_smokeSkinnedEmissivePublishBindingSet;
    nvrhi::BindingSetHandle m_smokeCleanRtxdiDiBoilingFilterBindingSet;
    nvrhi::BindingSetHandle m_smokeSkyCubeProbeBindingSet;
    nvrhi::BufferHandle m_smokeSkinnedGpuSkinningOutputBuffer;
    nvrhi::BufferHandle m_smokeSkinnedGpuSkinningPreviousPositionBuffer;
    nvrhi::BufferHandle
        m_smokeSkinnedEmissivePublishCurrentOutputBuffer;
    nvrhi::BufferHandle
        m_smokeSkinnedEmissivePublishPreviousPositionBuffer;
    nvrhi::BufferHandle
        m_smokeSkinnedEmissivePublishWorkBuffer;
    nvrhi::BufferHandle
        m_smokeSkinnedEmissiveWorkBuffer;
    nvrhi::BufferHandle m_smokeCleanRtxdiDiSentinelConstantsBuffer;
    nvrhi::BufferHandle m_smokeSkySurfaceResolveConstantsBuffer;
    nvrhi::BufferHandle m_smokeMaterialFeatureRuntimeConstantsBuffer;
    nvrhi::BufferHandle m_smokeCleanRtxdiDiBoilingFilterConstantsBuffer;
    nvrhi::TextureHandle m_smokeCleanRtxdiDiBoilingFilterInputTexture;
    nvrhi::TextureHandle m_smokeCleanRtxdiDiBoilingFilterOutputTexture;
    nvrhi::TextureHandle m_smokeSkyEnvironmentCube;
    nvrhi::TextureHandle m_smokeSkyCubeProbeOutputTexture;
    nvrhi::StagingTextureHandle m_smokeSkyCubeProbeReadbackTexture;
    bool m_smokeSkyCubeProbeReadbackQueued = false;
    int m_smokeSkyCubeProbeReadbackDelayFrames = 0;
    idStr m_smokeSkyEnvironmentSourceName;
    nvrhi::DescriptorTableHandle m_smokeTextureDescriptorTable;
    uint64 m_smokeTextureDescriptorGeneration = 0;
    std::vector<nvrhi::TextureHandle> m_smokeActiveTextureTable;
    std::deque<RtRetiredSmokeScenePackage> m_retiredSmokeScenePackages;
    std::deque<RtRetiredSmokeStaticBucketGpuPackage>
        m_retiredSmokeStaticBucketGpuPackages;
    std::deque<RtRetiredSmokeRigidGpuPackage>
        m_retiredSmokeRigidGpuPackages;
    std::vector<uint32_t> m_smokePreviousStaticTriangleMaterialIndexes;
    std::vector<PathTraceSmokeMaterial> m_smokeMaterialTableMaterials;
    std::vector<PathTraceDynamicMaterialRecord> m_smokeDynamicMaterialRecords;
    std::vector<PathTraceSmokeEmissiveTriangle> m_smokePreviousEmissiveTriangles;
    std::vector<uint32_t> m_smokeMaterialHydrationIds;
    bool m_smokeMaterialHydrationIdsValid = false;
    uint64 m_smokeMaterialHydrationStaticGeneration = 0;
    size_t m_smokeMaterialHydrationStaticTriangleMaterialCount = 0;
    uint64 m_smokeMaterialHydrationEmissiveSignature = 0;
    uint64 m_smokeMaterialHydrationRigidSignature = 0;
    uint64 m_smokeMaterialHydrationVisibleSignature = 0;
    uint64 m_smokeMaterialHydrationStaticBucketProbeSignature = 0;
    uint64 m_smokePreviousStaticSnapshotUploadSignature = 0;
    uint64 m_smokePreviousStaticMaterialIndexUploadSignature = 0;
    uint64 m_smokeStaticTriangleMaterialUploadSignature = 0;
    uint64 m_smokeStaticTriangleMaterialIndexUploadSignature = 0;
    uint64 m_smokeMaterialTableUploadSignature = 0;
    uint64 m_smokeDynamicMaterialUploadSignature = 0;
    bool m_smokeStaticTriangleMaterialUploadSignatureValid = false;
    bool m_smokeStaticTriangleMaterialIndexUploadSignatureValid = false;
    bool m_smokeMaterialTableUploadSignatureValid = false;
    bool m_smokeDynamicMaterialUploadSignatureValid = false;
    int m_smokeMaterialTableEntryCount = 0;
    int m_smokeEmissiveTriangleCount = 0;
    int m_smokeEmissiveStaticTriangleCount = 0;
    int m_smokeLightCandidateCount = 0;
    int m_smokeDoomAnalyticLightCount = 0;
    int m_smokeDoomAnalyticPortalRegionLightCount = 0;
    int m_smokeDoomAnalyticPreviousLightCount = 0;
    int m_smokeDoomAnalyticCurrentIdentityCount = 0;
    int m_smokeDoomAnalyticPreviousIdentityCount = 0;
    int m_smokeDoomAnalyticRemapCount = 0;
    int m_smokePreviousEmissiveTriangleCount = 0;
    int m_smokeUnifiedLightCount = 0;
    int m_smokeUnifiedPreviousLightCount = 0;
    int m_smokeUnifiedLightRemapCount = 0;
    int m_smokeRestirLightManagerCurrentPayloadCount = 0;
    int m_smokeRestirLightManagerPreviousPayloadCount = 0;
    int m_smokeUnifiedPtEmissiveLookupCount = 0;
    nvrhi::ShaderLibraryHandle m_smokeShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokePrimarySurfaceProducerShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeUptLeanPrimarySurfaceProducerShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirPdfNeeRluCurrentShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeCleanRtxdiDiSentinelShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeCleanRtxdiDiSkinnedHitsShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeCleanRtxdiDiInitialShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeCleanRtxdiDiTemporalShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeCleanRtxdiDiSpatialShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeCleanRtxdiDiInitialProductionShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeCleanRtxdiDiTemporalProductionShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeCleanRtxdiDiSpatialProductionShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeReGIRDebugShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeNeeCacheDebugShaderLibrary;
    RtPathTraceCleanRtxdiDiMaterialFeatureState m_smokeCleanRtxdiDiMaterialFeatures;
    nvrhi::ShaderHandle m_smokeSkinnedGpuSkinningShader;
    nvrhi::ShaderHandle
        m_smokeSkinnedEmissivePublishShader;
    nvrhi::ShaderHandle m_smokeCleanRtxdiDiBoilingFilterShader;
    nvrhi::ShaderHandle m_smokeSkyCubeProbeShader;
    nvrhi::ShaderHandle m_smokeSkySurfaceResolveShader;
    nvrhi::ShaderHandle m_smokeNeeCachePrimarySurfaceUpdateShader;
    nvrhi::ComputePipelineHandle m_smokeSkinnedGpuSkinningPipeline;
    nvrhi::ComputePipelineHandle
        m_smokeSkinnedEmissivePublishPipeline;
    nvrhi::ComputePipelineHandle m_smokeCleanRtxdiDiBoilingFilterPipeline;
    nvrhi::ComputePipelineHandle m_smokeSkyCubeProbePipeline;
    nvrhi::ComputePipelineHandle m_smokeSkySurfaceResolvePipeline;
    nvrhi::ComputePipelineHandle m_smokeNeeCachePrimarySurfaceUpdatePipeline;
    nvrhi::rt::PipelineHandle m_smokePipeline;
    nvrhi::rt::PipelineHandle m_smokePrimarySurfaceProducerPipeline;
    nvrhi::rt::PipelineHandle m_smokeUptLeanPrimarySurfaceProducerPipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirPdfNeeRluCurrentPipeline;
    nvrhi::rt::PipelineHandle m_smokeCleanRtxdiDiSentinelPipeline;
    nvrhi::rt::PipelineHandle m_smokeCleanRtxdiDiInitialPipeline;
    nvrhi::rt::PipelineHandle m_smokeCleanRtxdiDiTemporalPipeline;
    nvrhi::rt::PipelineHandle m_smokeCleanRtxdiDiSpatialPipeline;
    nvrhi::rt::PipelineHandle m_smokeCleanRtxdiDiInitialProductionPipeline;
    nvrhi::rt::PipelineHandle m_smokeCleanRtxdiDiTemporalProductionPipeline;
    nvrhi::rt::PipelineHandle m_smokeCleanRtxdiDiSpatialProductionPipeline;
    nvrhi::rt::PipelineHandle m_smokeReGIRDebugPipeline;
    nvrhi::rt::PipelineHandle m_smokeNeeCacheDebugPipeline;
    nvrhi::rt::ShaderTableHandle m_smokeShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokePrimarySurfaceProducerShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeUptLeanPrimarySurfaceProducerShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirPdfNeeRluCurrentShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeCleanRtxdiDiSentinelShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeCleanRtxdiDiInitialShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeCleanRtxdiDiTemporalShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeCleanRtxdiDiSpatialShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeCleanRtxdiDiInitialProductionShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeCleanRtxdiDiTemporalProductionShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeCleanRtxdiDiSpatialProductionShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeReGIRDebugShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeNeeCacheDebugShaderTable;
};

void RB_PathTraceGraphicsCommandListSubmitted(
    idRenderBackend* backend);
