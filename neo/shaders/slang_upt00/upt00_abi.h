#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace upt00
{
struct Float2 { float x, y; };
struct Float3 { float x, y, z; };
struct Float4 { float x, y, z, w; };
struct Uint4 { uint32_t x, y, z, w; };

struct StructuredMathInput { Uint4 words; Float4 values; };
using StructuredMathOutput = StructuredMathInput;

struct RayInput { Float3 origin; float tMin; Float3 direction; float tMax; };
struct RayOutput
{
    uint32_t hit;
    uint32_t instanceId;
    uint32_t primitiveId;
    uint32_t geometryId;
    float hitT;
    Float2 barycentrics;
    uint32_t frontFace;
};

struct Candidate
{
    float weight;
    float target;
    uint32_t sampleId;
    uint32_t status;
    Float3 radiance;
    uint32_t padding;
};

struct Reservoir
{
    float weightSum;
    float selectedTarget;
    uint32_t selectedId;
    uint32_t effectiveM;
    Float3 selectedRadiance;
    uint32_t flags;
};

static_assert(std::is_standard_layout_v<StructuredMathInput>);
static_assert(sizeof(StructuredMathInput) == 32);
static_assert(offsetof(StructuredMathInput, values) == 16);
static_assert(sizeof(RayInput) == 32);
static_assert(offsetof(RayInput, tMin) == 12);
static_assert(offsetof(RayInput, direction) == 16);
static_assert(offsetof(RayInput, tMax) == 28);
static_assert(sizeof(RayOutput) == 32);
static_assert(offsetof(RayOutput, hitT) == 16);
static_assert(offsetof(RayOutput, barycentrics) == 20);
static_assert(offsetof(RayOutput, frontFace) == 28);
static_assert(sizeof(Candidate) == 32);
static_assert(offsetof(Candidate, radiance) == 16);
static_assert(offsetof(Candidate, padding) == 28);
static_assert(sizeof(Reservoir) == 32);
static_assert(offsetof(Reservoir, selectedRadiance) == 16);
static_assert(offsetof(Reservoir, flags) == 28);
}
