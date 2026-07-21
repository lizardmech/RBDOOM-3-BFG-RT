#pragma once

// CPU-only frame preparation scaffold for the RTX Remix-shaped ReSTIR rebuild.
//
// This mirrors the frame-ordering responsibility of Remix SceneManager without
// owning resources, RAB bindings, RTXDI dispatch, or reservoir reset policy.

#include <cstdint>

struct PathTraceRemixFramePrepareDesc
{
    uint64_t frameIndex = 0;
    uint32_t resetReasonFlags = 0;
    bool previousSceneInputsValid = false;
    int sceneSource = 0;
    int debugMode = 0;
    int outputWidth = 0;
    int outputHeight = 0;
};

struct PathTraceRemixFramePrepareLightInputs
{
    uint32_t emissiveObservationCount = 0;
    uint32_t previousEmissiveObservationCount = 0;
    uint32_t doomAnalyticObservationCount = 0;
    uint32_t previousDoomAnalyticObservationCount = 0;
    uint32_t doomAnalyticIdentityCount = 0;
    uint32_t previousDoomAnalyticIdentityCount = 0;
    uint32_t restirLightObservationCount = 0;
};

struct PathTraceRemixFramePrepareObservationPackage
{
    uint64_t frameIndex = 0;
    uint32_t resetReasonFlags = 0;
    bool previousSceneInputsValid = false;
    int sceneSource = 0;
    int debugMode = 0;
    int outputWidth = 0;
    int outputHeight = 0;
    PathTraceRemixFramePrepareLightInputs lights;
};

struct PathTraceRemixFramePrepareStats
{
    uint64_t preparedFrameIndex = 0;
    uint32_t beginFrameCount = 0;
    uint32_t endFrameCount = 0;
    uint32_t lightInputUpdateCount = 0;
    uint32_t structuralResetReasonFlags = 0;
    uint32_t payloadObservationCount = 0;
    uint32_t mappingObservationCount = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t resourceAllocationCount = 0;
    uint32_t shaderRouteCount = 0;
};

class PathTraceRemixFramePrepare
{
public:
    void Clear();
    void BeginFrame(const PathTraceRemixFramePrepareDesc& desc);
    void SetLightInputs(const PathTraceRemixFramePrepareLightInputs& inputs);
    void EndFrame();

    const PathTraceRemixFramePrepareObservationPackage& GetObservationPackage() const;
    const PathTraceRemixFramePrepareStats& GetStats() const;

private:
    PathTraceRemixFramePrepareObservationPackage m_observationPackage;
    PathTraceRemixFramePrepareStats m_stats;
    bool m_frameOpen = false;
};
