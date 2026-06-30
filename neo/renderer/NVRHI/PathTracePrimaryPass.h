#pragma once

// Thin frame-level shell for the experimental RT smoke/path tracing path.
//
// PathTracePrimaryPass owns the persistent smoke test state and exposes the
// frame entry/present hooks used by the renderer. Scene build, resource
// lifetime, dispatch, readback, and diagnostics live in PathTrace* modules.

#include "PathTraceGeometryUniverse.h"
#include "PathTraceAccelerationPlan.h"
#include "PathTraceDrawSurfCapture.h"
#include "PathTraceCpuWork.h"
#include "PathTraceEmissiveCandidates.h"
#include "PathTraceFrameResources.h"
#include "PathTraceInstanceUniverse.h"
#include "PathTraceLightUniverse.h"
#include "PathTraceCleanRestirGi.h"
#include "PathTraceMaterialFeatureRuntime.h"
#include "PathTraceNeeCache.h"
#include "PathTraceReGIR.h"
#include "PathTraceRemixFramePrepare.h"
#include "PathTraceRemixLightManager.h"
#include "PathTraceRemixRtxdiResources.h"
#include "PathTraceRestirLightManager.h"
#include "PathTraceRestirPasses.h"
#include "PathTraceSceneInputs.h"
#include "PathTraceSceneUniverse.h"
#include "PathTraceSmokeResources.h"

#include <nvrhi/nvrhi.h>
#include <array>
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
class TonemapPass;
struct viewDef_t;

struct RtRetiredSmokeScenePackage
{
    uint64 retireFrame = 0;
    uint64 sceneSignature = 0;
    int currentArea = -1;
    int selectedAreaCount = 0;

    RtPathTraceSceneInputs sceneInputs;
    RtSmokeSceneBufferHandles buffers;

    nvrhi::rt::AccelStructHandle staticBlas;
    nvrhi::rt::AccelStructHandle dynamicBlas;
    nvrhi::rt::AccelStructHandle tlas;
    nvrhi::BindingSetHandle bindingSet;
    nvrhi::DescriptorTableHandle textureDescriptorTable;
    std::vector<nvrhi::TextureHandle> activeTextureTable;
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

class PathTracePrimaryPass {
public:
    explicit PathTracePrimaryPass(idRenderBackend* backend);
    ~PathTracePrimaryPass();

    // Called every frame when r_pathTracing >= 1
    void Execute(const viewDef_t* viewDef);
    void InvalidateForBackBufferResize();
    void PresentDebugOutput();
    void BlitDebugOutput(nvrhi::IFramebuffer* targetFramebuffer, const nvrhi::Viewport& targetViewport);
    void TonemapDebugOutput(TonemapPass* tonemapPass, const viewDef_t* viewDef, nvrhi::IFramebuffer* targetFramebuffer);
    void DrawBoundsOverlayRaster(nvrhi::IFramebuffer* targetFramebuffer, const nvrhi::Viewport& targetViewport);

private:
    void InitRayTracingSmokeTest();
    bool InitRayTracingSmokeRestirPipeline(int restirLibraryKind);
    bool InitPathTraceMaterialFeaturePipeline(const RtPathTraceMaterialFeaturePassDesc& passDesc);
    bool EnsurePathTraceMaterialFeatureRuntimePassPipeline(const RtPathTraceMaterialFeatureRuntimePass& pass);
    bool ResizeRayTracingSmokeOutput(int width, int height, int outputWidth, int outputHeight);
    void ResetRayTracingSmokeAsyncCpuWork();
    void ResetRayTracingSmokeSceneResources();
    void CommitRayTracingSmokeSceneResources(const RtSmokeSceneResourceCommitDesc& desc);
    bool HasRetainableRayTracingSmokeScenePackage() const;
    RtRetiredSmokeScenePackage CaptureRetiredRayTracingSmokeScenePackage() const;
    void PushRetiredRayTracingSmokeScenePackage(RtRetiredSmokeScenePackage& package, uint64 currentFrame, int retireFrames, const RtPathTraceSceneInputs& nextSceneInputs, bool sceneTransitionChanged, bool portalTransitionChanged, bool waitedForIdle);
    int ReleaseExpiredRetiredRayTracingSmokeScenePackages(uint64 currentFrame, const RtPathTraceSceneInputs& previousSceneInputs, const RtPathTraceSceneInputs& nextSceneInputs, bool sceneTransitionChanged, bool portalTransitionChanged, bool waitedForIdle);
    void BuildRayTracingSmokeTestScene(const viewDef_t* viewDef);
    void ExecuteRayTracingSmokeTest(const viewDef_t* viewDef);
    void ReadBackRayTracingSmokeTest();
    void ReadBackDLSSRRInputColorDump();
    void QueueDLSSRRInputColorDump(nvrhi::ICommandList* commandList, nvrhi::ITexture* inputColor, int source, uint32_t frameIndex);

    idRenderBackend* m_backend;
    bool m_reportedMode;
    bool m_rayTracingSupported;
    bool m_smokeTestInitialized;
    bool m_smokeSceneBuilt;
    bool m_smokeSceneRebuildLogged;
    bool m_smokeTestDispatched;
    bool m_smokeWaitingForDoomSurfaceLogged;
    int m_smokeSceneLogCooldownFrames;
    bool m_smokeStaticBlasCacheValid;
    uint64 m_smokeStaticBlasSignature;
    int m_smokeStaticBlasCacheHitCount;
    int m_smokeStaticBlasCacheMissCount;
    uint64 m_smokeGeometryFrameIndex;
    int m_smokeSceneSourceLast;
    int m_smokeSceneSource2RigidEntitiesLast;
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
    std::vector<RtSmokeSkinnedSurfaceRecord> m_smokeSkinnedSurfaceRecords;
    std::vector<RtSmokeSkinnedSurfaceRecord> m_smokePreviousSkinnedSurfaceRecords;
    std::vector<PathTraceSmokeVertex> m_smokePreviousSkinnedVertexData;
    std::vector<PathTraceSkinnedJointMatrix> m_smokePreviousSkinnedJointMatrices;
    RtSmokeSkinnedPreviousFrameStats m_smokeSkinnedPreviousStats;
    RtPathTraceSceneUniverse m_sceneUniverse;
    RtPathTraceInstanceUniverse m_instanceUniverse;
    RtSmokeLightUniverse m_smokeLightUniverse;
    PathTraceRemixFramePrepare m_remixFramePrepare;
    PathTraceRemixLightManager m_remixLightManager;
    PathTraceRemixRtxdiResources m_remixRtxdiResources;
    PathTraceRestirLightManager m_restirLightManager;
    const void* m_smokeLightUniverseRenderWorld = nullptr;
    uint32_t m_smokeTextureProbeMaterialId;
    int m_smokeTextureProbeRequestedIndex;
    idVec3 m_smokeSceneOrigin;
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
    nvrhi::BufferHandle m_smokeRestirLightManagerCurrentBuffer;
    nvrhi::BufferHandle m_smokeRestirLightManagerPreviousBuffer;
    nvrhi::BufferHandle m_smokeRestirLightManagerCurrentToPreviousBuffer;
    nvrhi::BufferHandle m_smokeRestirLightManagerPreviousToCurrentBuffer;
    nvrhi::BufferHandle m_smokeRestirLightManagerCurrentPayloadBuffer;
    nvrhi::BufferHandle m_smokeRestirLightManagerPreviousPayloadBuffer;
    nvrhi::BufferHandle m_smokeRigidRouteVertexBuffer;
    nvrhi::BufferHandle m_smokeRigidRouteIndexBuffer;
    nvrhi::BufferHandle m_smokeRigidRouteTriangleMaterialBuffer;
    nvrhi::BufferHandle m_smokeRigidRouteTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle m_smokeRigidRouteInstanceBuffer;
    RtSmokeRigidRouteSideBufferSlot m_smokeRigidRouteSideBufferSlots[RT_SMOKE_RIGID_ROUTE_SIDE_BUFFER_SLOTS];
    int m_smokeRigidRouteSideBufferReadSlot = -1;
    int m_smokeRigidRouteSideBufferWriteSlot = 0;
    nvrhi::BufferHandle m_smokeSkinnedSourceVertexBuffer;
    nvrhi::BufferHandle m_smokeSkinnedCurrentOutputVertexBuffer;
    nvrhi::BufferHandle m_smokeSkinnedPreviousPositionBuffer;
    nvrhi::BufferHandle m_smokeSkinnedSurfaceDispatchBuffer;
    nvrhi::BufferHandle m_smokeSkinnedTriangleDispatchIndexBuffer;
    nvrhi::BufferHandle m_smokeSkinnedCurrentJointMatrixBuffer;
    nvrhi::BufferHandle m_smokeSkinnedPreviousJointMatrixBuffer;
    nvrhi::BufferHandle m_smokeConstantsBuffer;
    nvrhi::BufferHandle m_restirPTConstantsBuffer;
    nvrhi::BufferHandle m_smokeBoundsOverlayLineBuffer;
    nvrhi::BufferHandle m_smokeCleanRtxdiDiCurrentReservoirBuffer;
    nvrhi::BufferHandle m_smokeCleanRtxdiDiTemporalReservoirBuffer;
    nvrhi::BufferHandle m_smokeCleanRtxdiDiPreviousReservoirBuffer;
    nvrhi::BufferHandle m_smokeCleanRtxdiDiSpatialReservoirBuffer;
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
    idVec3 m_smokeBoundsOverlayCameraOrigin = vec3_origin;
    idVec3 m_smokeBoundsOverlayCameraForward = idVec3(1.0f, 0.0f, 0.0f);
    idVec3 m_smokeBoundsOverlayCameraLeft = idVec3(0.0f, 1.0f, 0.0f);
    idVec3 m_smokeBoundsOverlayCameraUp = idVec3(0.0f, 0.0f, 1.0f);
    float m_smokeBoundsOverlayTanX = 1.0f;
    float m_smokeBoundsOverlayTanY = 1.0f;
    float m_smokeBoundsOverlayModelViewMatrix[16] = {};
    float m_smokeBoundsOverlayProjectionMatrix[16] = {};
    RtPathTraceFrameResources m_frameResources;
    RtPathTraceSceneInputs m_sceneInputs;
    nvrhi::rt::AccelStructDesc m_smokeStaticBlasDesc;
    nvrhi::rt::AccelStructDesc m_smokeDynamicBlasDesc;
    nvrhi::rt::AccelStructHandle m_smokeStaticBlas;
    nvrhi::rt::AccelStructHandle m_smokeDynamicBlas;
    nvrhi::rt::AccelStructHandle m_smokeTlas;
    nvrhi::BindingLayoutHandle m_smokeBindingLayout;
    nvrhi::BindingLayoutHandle m_smokePdfNeeVerifierBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeCleanRtxdiDiSentinelBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeReGIRDebugBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeNeeCacheDebugBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeNeeCachePrimarySurfaceUpdateBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeSkinnedGpuSkinningBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeCleanRtxdiDiBoilingFilterBindingLayout;
    nvrhi::BindingLayoutHandle m_smokeTextureBindlessLayout;
    nvrhi::BindingSetHandle m_smokeBindingSet;
    nvrhi::BindingSetHandle m_smokeSkinnedGpuSkinningBindingSet;
    nvrhi::BindingSetHandle m_smokeCleanRtxdiDiBoilingFilterBindingSet;
    nvrhi::BufferHandle m_smokeSkinnedGpuSkinningOutputBuffer;
    nvrhi::BufferHandle m_smokeSkinnedGpuSkinningPreviousPositionBuffer;
    nvrhi::BufferHandle m_smokeCleanRtxdiDiSentinelConstantsBuffer;
    nvrhi::BufferHandle m_smokeCleanRtxdiDiBoilingFilterConstantsBuffer;
    nvrhi::TextureHandle m_smokeCleanRtxdiDiBoilingFilterInputTexture;
    nvrhi::TextureHandle m_smokeCleanRtxdiDiBoilingFilterOutputTexture;
    nvrhi::DescriptorTableHandle m_smokeTextureDescriptorTable;
    std::vector<nvrhi::TextureHandle> m_smokeActiveTextureTable;
    std::deque<RtRetiredSmokeScenePackage> m_retiredSmokeScenePackages;
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
    int m_smokeEmissiveDynamicTriangleCount = 0;
    int m_smokeLightCandidateCount = 0;
    int m_smokeTexturedLightCandidateCount = 0;
    int m_smokeLightCandidateBytes = 0;
    int m_smokeDoomAnalyticLightCount = 0;
    int m_smokeDoomAnalyticPortalRegionLightCount = 0;
    int m_smokeDoomAnalyticLightBytes = 0;
    int m_smokeDoomAnalyticPreviousLightCount = 0;
    int m_smokeDoomAnalyticCurrentIdentityCount = 0;
    int m_smokeDoomAnalyticPreviousIdentityCount = 0;
    int m_smokeDoomAnalyticRemapCount = 0;
    int m_smokeDoomAnalyticInvalidRemapCount = 0;
    int m_smokePreviousEmissiveTriangleCount = 0;
    int m_smokeUnifiedLightCount = 0;
    int m_smokeUnifiedPreviousLightCount = 0;
    int m_smokeUnifiedLightRemapCount = 0;
    int m_smokeRestirLightManagerCurrentPayloadCount = 0;
    int m_smokeRestirLightManagerPreviousPayloadCount = 0;
    nvrhi::ShaderLibraryHandle m_smokeShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirInitialShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirTemporalShadingShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirAttributionShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirSpatialReservoirShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirSpatialShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirSpatialAttributionShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeMode18RestirHybridShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirCombinedShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokePrimarySurfaceProducerShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirIndirectInitialProducerShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirDirectTemporalProducerShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirDirectSpatialReservoirProducerShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirReflectionProducerShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirCombinedResolveShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeRestirPdfNeeRluCurrentShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokePdfNeeVerifierShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeCleanRtxdiDiSentinelShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeCleanRtxdiDiInitialShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeCleanRtxdiDiTemporalShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeCleanRtxdiDiSpatialShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeReGIRDebugShaderLibrary;
    nvrhi::ShaderLibraryHandle m_smokeNeeCacheDebugShaderLibrary;
    std::array<RtPathTraceMaterialFeatureShaderState, RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_TABLE_COUNT> m_smokeMaterialFeatureShaders;
    nvrhi::ShaderHandle m_smokeSkinnedGpuSkinningShader;
    nvrhi::ShaderHandle m_smokeCleanRtxdiDiBoilingFilterShader;
    nvrhi::ShaderHandle m_smokeNeeCachePrimarySurfaceUpdateShader;
    nvrhi::ComputePipelineHandle m_smokeSkinnedGpuSkinningPipeline;
    nvrhi::ComputePipelineHandle m_smokeCleanRtxdiDiBoilingFilterPipeline;
    nvrhi::ComputePipelineHandle m_smokeNeeCachePrimarySurfaceUpdatePipeline;
    nvrhi::rt::PipelineHandle m_smokePipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirPipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirInitialPipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirTemporalShadingPipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirAttributionPipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirSpatialReservoirPipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirSpatialPipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirSpatialAttributionPipeline;
    nvrhi::rt::PipelineHandle m_smokeMode18RestirHybridPipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirCombinedPipeline;
    nvrhi::rt::PipelineHandle m_smokePrimarySurfaceProducerPipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirIndirectInitialProducerPipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirDirectTemporalProducerPipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirDirectSpatialReservoirProducerPipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirReflectionProducerPipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirCombinedResolvePipeline;
    nvrhi::rt::PipelineHandle m_smokeRestirPdfNeeRluCurrentPipeline;
    nvrhi::rt::PipelineHandle m_smokePdfNeeVerifierPipeline;
    nvrhi::rt::PipelineHandle m_smokeCleanRtxdiDiSentinelPipeline;
    nvrhi::rt::PipelineHandle m_smokeCleanRtxdiDiInitialPipeline;
    nvrhi::rt::PipelineHandle m_smokeCleanRtxdiDiTemporalPipeline;
    nvrhi::rt::PipelineHandle m_smokeCleanRtxdiDiSpatialPipeline;
    nvrhi::rt::PipelineHandle m_smokeReGIRDebugPipeline;
    nvrhi::rt::PipelineHandle m_smokeNeeCacheDebugPipeline;
    nvrhi::rt::ShaderTableHandle m_smokeShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirInitialShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirTemporalShadingShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirAttributionShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirSpatialReservoirShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirSpatialShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirSpatialAttributionShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeMode18RestirHybridShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirCombinedShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokePrimarySurfaceProducerShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirIndirectInitialProducerShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirDirectTemporalProducerShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirDirectSpatialReservoirProducerShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirReflectionProducerShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirCombinedResolveShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeRestirPdfNeeRluCurrentShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokePdfNeeVerifierShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeCleanRtxdiDiSentinelShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeCleanRtxdiDiInitialShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeCleanRtxdiDiTemporalShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeCleanRtxdiDiSpatialShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeReGIRDebugShaderTable;
    nvrhi::rt::ShaderTableHandle m_smokeNeeCacheDebugShaderTable;
};
