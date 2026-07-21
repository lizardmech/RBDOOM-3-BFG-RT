#pragma once

// Output-sized frame resource owner for the RT/PT path.
//
// This is the first narrow ownership boundary in front of the legacy smoke
// dispatch path. It keeps resize/destructive replacement policy and frame
// reset bookkeeping together while existing scene build and raygen code still
// consume the same NVRHI handles.

#include "PathTraceReservoirs.h"
#include "PathTraceRestirPT.h"
#include "PathTraceRestirPTReservoirs.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

enum RtPathTraceFrameResetReason : uint32_t
{
    RT_FRAME_RESET_NONE = 0,
    RT_FRAME_RESET_OUTPUT_RESIZE = 1u << 0,
    RT_FRAME_RESET_BACKBUFFER_RESIZE = 1u << 1,
    RT_FRAME_RESET_SCENE_RESOURCES = 1u << 2,
    RT_FRAME_RESET_RESERVOIR_SCENE_SIGNATURE = 1u << 3,
    RT_FRAME_RESET_RESERVOIR_DISPATCH_SIGNATURE = 1u << 4,
    RT_FRAME_RESET_PRIMARY_HISTORY = 1u << 5,
    RT_FRAME_RESET_GPU_IDLE_WAIT = 1u << 6
};

struct RtPathTraceFrameCameraState
{
    bool valid = false;
    int width = 0;
    int height = 0;
    idVec3 origin = vec3_origin;
    idVec3 forward = idVec3(1.0f, 0.0f, 0.0f);
    idVec3 left = idVec3(0.0f, 1.0f, 0.0f);
    idVec3 up = idVec3(0.0f, 0.0f, 1.0f);
    float tanX = 1.0f;
    float tanY = 1.0f;

    void Reset();
};

struct RtPathTraceFrameSettings
{
    int width = 0;
    int height = 0;
    int outputWidth = 0;
    int outputHeight = 0;
    int debugMode = 0;
    RtRestirPTCheckerboardMode checkerboardMode = RtRestirPTCheckerboardMode::Off;
    uint32_t frameIndex = 0;
    RtPathTraceFrameCameraState currentCamera;
    RtPathTraceFrameCameraState previousCamera;
    uint32_t resetReasonFlags = RT_FRAME_RESET_NONE;
};

struct RtPathTraceFrameResourceDiagnostics
{
    int waitForIdleCalls = 0;
    const char* lastWaitForIdleReason = "";
    int outputTexturesCreated = 0;
    int diagnosticReadbackResourcesCreated = 0;
    int smokeReservoirBuffersReused = 0;
    int smokeReservoirBuffersRecreated = 0;
    int restirPTReservoirBuffersReused = 0;
    int restirPTReservoirBuffersRecreated = 0;
    int primarySurfaceHistoryBuffersReused = 0;
    int primarySurfaceHistoryBuffersRecreated = 0;
    int motionVectorTexturesCreated = 0;
    int motionVectorMaskTexturesCreated = 0;
    int rrGuideTexturesCreated = 0;
    int descriptorBindingSetRebuilds = 0;
    int blasTlasCommits = 0;
    int readbacksQueued = 0;
    int readbacksMapped = 0;
    int readbacksUnmapped = 0;
    uint64_t outputTextureBytes = 0;
    uint64_t smokeReservoirBytes = 0;
    uint64_t restirPTReservoirBytes = 0;
    uint64_t restirPTDiReservoirBytes = 0;
    uint64_t restirPTGiReservoirBytes = 0;
    uint64_t primarySurfaceHistoryBytes = 0;
    uint64_t motionVectorBytes = 0;
    uint64_t motionVectorMaskBytes = 0;
    uint64_t rrGuideBytes = 0;
    uint64_t sceneUploadBytes = 0;

    void ResetResizeStats();
};

struct RtPathTraceFrameResources
{
    nvrhi::TextureHandle outputTexture;
    nvrhi::TextureHandle accumulationTexture;
    nvrhi::TextureHandle restirPTReflectionTexture;
    nvrhi::TextureHandle transmissionTexture;
    nvrhi::TextureHandle reflectionSidecarTexture;
    nvrhi::TextureHandle glassDistortionSidecarTexture;
    nvrhi::TextureHandle rrInputColorTexture;
    nvrhi::TextureHandle cleanRtxdiDiBoilingFilterTexture;
    nvrhi::TextureHandle motionVectorTexture;
    nvrhi::TextureHandle rrMotionVectorTexture;
    nvrhi::TextureHandle motionVectorMaskTexture;
    nvrhi::TextureHandle rrGuideAlbedoTexture;
    nvrhi::TextureHandle rrGuideSpecularAlbedoTexture;
    nvrhi::TextureHandle rrGuideNormalRoughnessTexture;
    nvrhi::TextureHandle rrGuideDepthTexture;
    nvrhi::TextureHandle rrGuideHitDistanceTexture;
    nvrhi::TextureHandle rrGuideResetMaskTexture;
    nvrhi::TextureHandle rrGuidePositionTexture;
    nvrhi::StagingTextureHandle readbackTexture;
    nvrhi::StagingTextureHandle rrInputColorDumpReadbackTexture;
    int width = 0;
    int height = 0;
    int outputWidth = 0;
    int outputHeight = 0;

    RtSmokeReservoirBufferHandles smokeReservoirBuffers;
    RtRestirPTReservoirBufferHandles restirPTReservoirBuffers;
    RtRestirPTReservoirBufferHandles restirPTDiReservoirBuffers;
    RtRestirPTReservoirBufferHandles restirPTGiReservoirBuffers;
    RtRestirPTPrimarySurfaceHistoryBufferHandles primarySurfaceHistoryBuffers;
    RtRestirPTContextState restirPTContextState;
    uint32_t restirPTFrameIndex = 0;

    uint64 smokeReservoirSceneSignature = 0;
    uint64 smokeReservoirDispatchSignature = 0;
    bool smokeReservoirNeedsClear = false;
    bool restirPTReservoirNeedsClear = true;
    bool restirPTDiReservoirNeedsClear = true;
    bool restirPTGiReservoirNeedsClear = true;
    bool primarySurfaceHistoryNeedsClear = true;
    RtPathTracePrimarySurfaceHistoryState primarySurfaceHistoryState;
    RtPathTraceFrameCameraState primarySurfaceHistoryView;
    int smokeReservoirResetCount = 0;
    int smokeReservoirClearCount = 0;
    int restirPTReservoirClearCount = 0;
    int restirPTDiReservoirClearCount = 0;
    int restirPTGiReservoirClearCount = 0;
    uint64 smokeAccumulationSignature = 0;
    int smokeAccumulationFrameCount = 0;

    bool readbackQueued = false;
    bool readbackLogged = false;
    int readbackDelayFrames = 0;
    int readbackCooldownFrames = 0;
    bool rrInputColorDumpQueued = false;
    int rrInputColorDumpDelayFrames = 0;
    int rrInputColorDumpSource = 0;
    uint32_t rrInputColorDumpFrameIndex = 0;

    RtPathTraceFrameSettings settings;
    RtPathTraceFrameResourceDiagnostics diagnostics;

    bool IsValidFor(int requestedWidth, int requestedHeight, int requestedOutputWidth, int requestedOutputHeight, RtRestirPTCheckerboardMode checkerboardMode) const;
    bool HasAnyOutputSizedResource() const;
    bool ResizeOutputSizedResources(nvrhi::IDevice* device, int requestedWidth, int requestedHeight, int requestedOutputWidth, int requestedOutputHeight, RtRestirPTCheckerboardMode checkerboardMode);
    void ResetOutputSizedResources(uint32_t reasonFlags);
    void ResetSceneDependentState();
    void ResetReadbackQueue();
    void MarkResetReason(uint32_t reasonFlags);
    void ClearResetReasons();
    void SetPrimarySurfaceHistoryView(const RtPathTraceFrameCameraState& view, bool objectMotionAvailable);
    void InvalidatePrimarySurfaceHistory(uint32_t reasonFlags);
    void RecordSceneResourceCommit(uint64_t uploadBytes, bool rebuiltBindingSet, bool committedAccelerationStructures);
    void RecordReadbackQueued();
    void RecordReadbackMapped();
    void RecordReadbackUnmapped();
    void DescribeResetReasons(idStr& out) const;
    void PrintDiagnostics(const char* prefix) const;
};
