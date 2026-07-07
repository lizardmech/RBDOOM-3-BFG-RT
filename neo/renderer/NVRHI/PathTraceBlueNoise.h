#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

struct PathTraceBlueNoiseState
{
    nvrhi::TextureHandle texture;        // STBN mask array (t127), or unused dummy
    std::vector<uint8_t> stagedBytes;    // staged mask bytes, uploaded once then cleared
    bool initAttempted = false;
    bool valid = false;                  // true only when the mask loaded successfully
    bool uploaded = false;

    void Release();
};

static constexpr uint32_t PATH_TRACE_BLUE_NOISE_SIZE = 128u;
static constexpr uint32_t PATH_TRACE_BLUE_NOISE_LAYERS = 64u;
static constexpr uint32_t PATH_TRACE_BLUE_NOISE_BINDING = 127u;

bool PathTraceEnsureBlueNoise(
    PathTraceBlueNoiseState& state,
    nvrhi::IDevice* device,
    const char* ownerName,
    const char* textureDebugName);

void PathTraceUploadBlueNoise(PathTraceBlueNoiseState& state, nvrhi::ICommandList* commandList);
