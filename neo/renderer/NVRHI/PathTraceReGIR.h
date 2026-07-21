#pragma once

// Standalone clean-room ReGIR CPU/resource shell.
//
// This module owns only parameter shaping and candidate-cache capacity for the
// isolated ReGIR lane. Keep it independent from other path-tracing reuse and
// presentation paths unless a later task explicitly extends the contract.

#include <nvrhi/nvrhi.h>

#include <stdint.h>

struct PathTraceReGIRSettings
{
    bool enabled = false;
    int debugView = 0;
    int mode = 1;
    float cellSize = 256.0f;
    uint32_t gridX = 16;
    uint32_t gridY = 16;
    uint32_t gridZ = 8;
    uint32_t lightsPerCell = 64;
    uint32_t buildSamples = 8;
    int lightDomain = 0;
    int centerMode = 0;
    float manualCenter[3] = {};
};

struct PathTraceReGIRLightCounts
{
    uint32_t analyticCount = 0;
    uint32_t emissiveCount = 0;
    uint32_t unifiedCount = 0;
};

struct PathTraceReGIRResourceDesc
{
    bool requested = false;
    bool structuralValid = false;
    uint32_t cellCount = 0;
    uint32_t slotCount = 0;
    uint32_t candidateStride = 0;
    uint64_t candidateBytes = 0;
    const char* firstMissingContract = "disabled";
};

struct PathTraceReGIRState
{
    nvrhi::BufferHandle candidateCacheBuffer;
    nvrhi::BufferHandle placeholderSrvBuffer;
    PathTraceReGIRSettings settings;
    PathTraceReGIRResourceDesc resourceDesc;
    uint64_t allocationSerial = 0;

    void Clear();
    bool EnsureResources(nvrhi::IDevice* device, const PathTraceReGIRSettings& nextSettings, const PathTraceReGIRResourceDesc& nextDesc);
};

PathTraceReGIRSettings BuildPathTraceReGIRSettingsFromCVars();
PathTraceReGIRResourceDesc BuildPathTraceReGIRResourceDesc(const PathTraceReGIRSettings& settings, const PathTraceReGIRLightCounts& lightCounts);
