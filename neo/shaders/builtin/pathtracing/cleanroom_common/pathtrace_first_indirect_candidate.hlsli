// Shared first-indirect path candidate records.
//
// These records are intentionally estimator-neutral. Clean ReSTIR GI consumes
// them today, but the layout is meant to survive a later ReSTIR PT / unified
// DI-GI experiment without inheriting GI-only names.

#ifndef PATH_TRACE_FIRST_INDIRECT_CANDIDATE_HLSLI
#define PATH_TRACE_FIRST_INDIRECT_CANDIDATE_HLSLI

static const uint PATH_TRACE_FIRST_INDIRECT_CANDIDATE_SURFACE_STRIDE_BYTES = 144u;

static const uint PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_VALID = 1u << 0u;
static const uint PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_DIFFUSE_LOBE = 1u << 1u;
static const uint PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_SPECULAR_LOBE = 1u << 2u;

// The current diffuse cosine and GGX half-vector samplers both consume two
// random values. Keep it explicit so split trace/shade kernels do not hide RNG
// replay assumptions in GI-only code.
static const uint PATH_TRACE_FIRST_INDIRECT_CANDIDATE_RANDOMS_TWO_D = 2u;

struct PathTraceFirstIndirectCandidateRaySample
{
    float3 direction;
    float sourcePdf;
    uint flags;
    uint randomsConsumed;
    float lobeProbability;
    float lobePdf;
};

// Trace -> shade surface record for a first-indirect hit. Mirrors the subset of
// RAB_Surface + RAB_Material needed to shade the candidate without reloading hit
// geometry. Keep this in lockstep with the host buffer stride.
struct PathTraceFirstIndirectCandidateSurface
{
    float3 worldPos;
    uint valid;
    float3 geometryNormal;
    float linearDepth;
    float3 shadingNormal;
    uint materialId;
    float3 viewDir;
    uint materialIndex;
    float3 diffuseAlbedo;
    float roughness;
    float3 specularF0;
    float opacity;
    float3 emissiveRadiance;
    float alphaCutoff;
    uint instanceId;
    uint primitiveIndex;
    uint surfaceClass;
    uint surfaceFlags;
    uint materialFlags;
    uint emissiveTextureIndex;
    uint primarySampledSpecular;
    float sourcePdf;
};

// Shaded first-indirect candidate. Radiance is incoming radiance at the
// receiver and excludes the receiver BSDF/albedo.
struct PathTraceFirstIndirectCandidateResult
{
    uint valid;
    float3 radiance;
    float pathLength;
    float3 hitPosition;
    float3 hitNormal;
    float3 materialAlbedo;
    float materialOpacity;
    uint materialFlags;
    uint diffuseTextureIndex;
    float sourcePdf;
};

bool PathTraceFirstIndirectCandidateRaySampleIsValid(PathTraceFirstIndirectCandidateRaySample sample)
{
    return (sample.flags & PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_VALID) != 0u &&
        sample.sourcePdf > 0.0;
}

bool PathTraceFirstIndirectCandidateRaySampleIsSpecular(PathTraceFirstIndirectCandidateRaySample sample)
{
    return (sample.flags & PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_SPECULAR_LOBE) != 0u;
}

uint PathTraceFirstIndirectCandidateSurfaceSampleFlags(PathTraceFirstIndirectCandidateSurface surface)
{
    if (surface.valid == 0u)
    {
        return 0u;
    }
    return PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_VALID |
        (surface.primarySampledSpecular != 0u
            ? PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_SPECULAR_LOBE
            : PATH_TRACE_FIRST_INDIRECT_CANDIDATE_FLAG_DIFFUSE_LOBE);
}

float3 PathTraceFirstIndirectCandidateSurfaceSampleDirection(PathTraceFirstIndirectCandidateSurface surface)
{
    return -surface.viewDir;
}

#endif // PATH_TRACE_FIRST_INDIRECT_CANDIDATE_HLSLI
