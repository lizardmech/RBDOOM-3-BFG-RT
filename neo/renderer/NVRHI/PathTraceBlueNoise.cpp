#include "precompiled.h"
#pragma hdrstop

#include "PathTraceBlueNoise.h"

namespace
{

constexpr const char* PATH_TRACE_BLUE_NOISE_PATH = "textures/bluenoise/stbn_scalar_128x128x64.raw";

size_t PathTraceBlueNoiseExpectedBytes()
{
    return size_t(PATH_TRACE_BLUE_NOISE_SIZE) * PATH_TRACE_BLUE_NOISE_SIZE * PATH_TRACE_BLUE_NOISE_LAYERS;
}

} // namespace

void PathTraceBlueNoiseState::Release()
{
    texture = nullptr;
    stagedBytes.clear();
    initAttempted = false;
    valid = false;
    uploaded = false;
}

bool PathTraceEnsureBlueNoise(
    PathTraceBlueNoiseState& state,
    nvrhi::IDevice* device,
    const char* ownerName,
    const char* textureDebugName)
{
    if (state.initAttempted)
    {
        return state.texture != nullptr;
    }
    state.initAttempted = true;

    nvrhi::TextureDesc desc;
    desc.width = PATH_TRACE_BLUE_NOISE_SIZE;
    desc.height = PATH_TRACE_BLUE_NOISE_SIZE;
    desc.arraySize = PATH_TRACE_BLUE_NOISE_LAYERS;
    desc.mipLevels = 1;
    desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    desc.format = nvrhi::Format::R8_UNORM;
    desc.debugName = textureDebugName;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    state.texture = device->createTexture(desc);
    if (!state.texture)
    {
        common->Printf("%s: failed to create blue-noise texture; blue noise disabled\n", ownerName);
        return false;
    }

    void* maskData = nullptr;
    ID_TIME_T maskTimestamp = 0;
    const size_t expectedBytes = PathTraceBlueNoiseExpectedBytes();
    const int maskSize = fileSystem->ReadFile(PATH_TRACE_BLUE_NOISE_PATH, &maskData, &maskTimestamp);
    if (maskSize != static_cast<int>(expectedBytes) || !maskData)
    {
        if (maskData)
        {
            Mem_Free(maskData);
        }
        common->Printf("%s: blue-noise mask missing or wrong size (got %d, expected %zu); blue noise disabled\n",
            ownerName, maskSize, expectedBytes);
        return true;
    }

    state.stagedBytes.assign(static_cast<const uint8_t*>(maskData), static_cast<const uint8_t*>(maskData) + expectedBytes);
    Mem_Free(maskData);
    state.valid = true;
    return true;
}

void PathTraceUploadBlueNoise(PathTraceBlueNoiseState& state, nvrhi::ICommandList* commandList)
{
    if (!state.valid || state.uploaded || state.stagedBytes.empty() || !state.texture)
    {
        return;
    }

    const size_t layerBytes = size_t(PATH_TRACE_BLUE_NOISE_SIZE) * PATH_TRACE_BLUE_NOISE_SIZE;
    for (uint32_t layer = 0; layer < PATH_TRACE_BLUE_NOISE_LAYERS; ++layer)
    {
        commandList->writeTexture(
            state.texture,
            layer,
            0,
            state.stagedBytes.data() + size_t(layer) * layerBytes,
            PATH_TRACE_BLUE_NOISE_SIZE);
    }

    state.uploaded = true;
    state.stagedBytes.clear();
    state.stagedBytes.shrink_to_fit();
}
