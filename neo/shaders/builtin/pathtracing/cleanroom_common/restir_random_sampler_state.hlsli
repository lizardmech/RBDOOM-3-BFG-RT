#ifndef RB_PATH_TRACING_RESTIR_RANDOM_SAMPLER_STATE_HLSLI
#define RB_PATH_TRACING_RESTIR_RANDOM_SAMPLER_STATE_HLSLI

// rbdoom-owned replacement for Rtxdi/Utils/RandomSamplerState.hlsli.
// Written against this repository's shader call-site contract. Random
// sequences are not expected to match any external implementation.

#ifndef RBPT_BLUE_NOISE_DIMS
#define RBPT_BLUE_NOISE_DIMS 16
#endif

#ifndef RBPT_BLUE_NOISE_SIZE
#define RBPT_BLUE_NOISE_SIZE 128
#endif

#ifndef RBPT_BLUE_NOISE_LAYERS
#define RBPT_BLUE_NOISE_LAYERS 64
#endif

#ifdef RBPT_ENABLE_BLUE_NOISE
#ifndef RBPT_BLUE_NOISE_TEXTURE_DECLARED
Texture2DArray<float> t_RbptBlueNoise : register(t127);
#define RBPT_BLUE_NOISE_TEXTURE_DECLARED 1
#endif
#endif

uint RBPT_RestirPcgHash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint RBPT_RestirHashCombine(uint a, uint b)
{
    const uint mixed =
        (a * 0x9e3779b9u) ^
        (b * 0x85ebca6bu) ^
        ((a >> 16u) | (a << 16u)) ^
        ((b >> 13u) | (b << 19u));
    return RBPT_RestirPcgHash(mixed);
}

uint RBPT_RestirPart1By1(uint v)
{
    v &= 0x0000ffffu;
    v = (v ^ (v << 8u)) & 0x00ff00ffu;
    v = (v ^ (v << 4u)) & 0x0f0f0f0fu;
    v = (v ^ (v << 2u)) & 0x33333333u;
    v = (v ^ (v << 1u)) & 0x55555555u;
    return v;
}

uint RBPT_RestirPixelMorton(uint2 pixel)
{
    return RBPT_RestirPart1By1(pixel.x) | (RBPT_RestirPart1By1(pixel.y) << 1u);
}

uint RBPT_RestirPixelSeed(uint2 pixel, uint frameIndex, uint passNamespace)
{
    const uint mixed =
        RBPT_RestirPixelMorton(pixel) ^
        (frameIndex * 0xc2b2ae35u) ^
        (passNamespace * 0x27d4eb2du);
    return RBPT_RestirPcgHash(mixed);
}

float RBPT_RestirUintToUnitFloat(uint v)
{
    return float(v >> 8u) * (1.0 / 16777216.0);
}

struct RTXDI_RandomSamplerState
{
    uint seed;
    uint index;
    uint2 pixel;
    uint frameIndex;
    uint dimensionBase;
    uint useBlueNoise;
};

RTXDI_RandomSamplerState RTXDI_CreateRandomSamplerFromDirectSeed(uint seed, uint index)
{
    RTXDI_RandomSamplerState rng;
    rng.seed = seed;
    rng.index = index;
    rng.pixel = uint2(0u, 0u);
    rng.frameIndex = 0u;
    rng.dimensionBase = 0u;
    rng.useBlueNoise = 0u;
    return rng;
}

// The third argument is the starting random dimension. Several call sites use
// large per-pass constants as dimension namespaces, so it must live in index.
RTXDI_RandomSamplerState RTXDI_InitRandomSampler(uint2 pixel, uint frameIndex, uint dimensionBase)
{
    RTXDI_RandomSamplerState rng;
    rng.seed = RBPT_RestirPixelSeed(pixel, frameIndex, dimensionBase);
    rng.index = dimensionBase;
    rng.pixel = pixel;
    rng.frameIndex = frameIndex;
    rng.dimensionBase = dimensionBase;
#ifdef RBPT_ENABLE_BLUE_NOISE
    rng.useBlueNoise = 1u;
#else
    rng.useBlueNoise = 0u;
#endif
    return rng;
}

// Pass-namespaced initializer for lanes that want low random dimensions to use
// blue-noise masks while still decorrelating independent sampling passes. The
// legacy RTXDI_InitRandomSampler contract keeps its third argument as the
// starting dimension for existing call sites.
RTXDI_RandomSamplerState RTXDI_InitRandomSamplerForPass(uint2 pixel, uint frameIndex, uint passNamespace, uint startDimension)
{
    RTXDI_RandomSamplerState rng;
    rng.seed = RBPT_RestirPixelSeed(pixel, frameIndex, passNamespace);
    rng.index = startDimension;
    rng.pixel = pixel;
    rng.frameIndex = frameIndex;
    rng.dimensionBase = passNamespace;
#ifdef RBPT_ENABLE_BLUE_NOISE
    rng.useBlueNoise = 1u;
#else
    rng.useBlueNoise = 0u;
#endif
    return rng;
}

float RBPT_RestirGetNextWhite(inout RTXDI_RandomSamplerState rng)
{
    const uint value = RBPT_RestirPcgHash(rng.seed ^ RBPT_RestirPcgHash(rng.index ^ 0xa511e9b3u));
    rng.index += 1u;
    return RBPT_RestirUintToUnitFloat(value);
}

#ifdef RBPT_ENABLE_BLUE_NOISE
float RBPT_RestirGetNextBlue(inout RTXDI_RandomSamplerState rng)
{
    const uint dimension = rng.index;
    rng.index += 1u;

    const uint layer = rng.frameIndex & (RBPT_BLUE_NOISE_LAYERS - 1u);
    const uint2 r2 = uint2(0xC13FA9A9u * dimension, 0x91E10DA5u * dimension);
    const uint2 shift = r2 >> (32u - firstbithigh(RBPT_BLUE_NOISE_SIZE));
    const uint2 texel = (rng.pixel + shift) & (RBPT_BLUE_NOISE_SIZE - 1u);
    const float mask = t_RbptBlueNoise.Load(int4(int2(texel), int(layer), 0)).x;

    const uint epoch = rng.frameIndex / RBPT_BLUE_NOISE_LAYERS;
    const uint rotBits = RBPT_RestirHashCombine(
        rng.dimensionBase,
        RBPT_RestirHashCombine(dimension, epoch));
    return frac(mask + RBPT_RestirUintToUnitFloat(rotBits));
}
#endif

float RTXDI_GetNextRandom(inout RTXDI_RandomSamplerState rng)
{
#ifdef RBPT_ENABLE_BLUE_NOISE
    if (rng.useBlueNoise != 0u && rng.index < RBPT_BLUE_NOISE_DIMS)
    {
        return RBPT_RestirGetNextBlue(rng);
    }
#endif

    return RBPT_RestirGetNextWhite(rng);
}

float RTXDI_SampleUniformRng(inout RTXDI_RandomSamplerState rng)
{
    return RTXDI_GetNextRandom(rng);
}

#endif
