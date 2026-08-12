#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCVars.h"
#include "PathTraceEmissiveCandidates.h"
#include "PathTraceMaterialClassifier.h"
#include "PathTraceUnifiedPt.h"
#include "PathTraceUnifiedLight.h"
#include "PathTraceUnifiedPtPrimaryReceiver.h"

#include <nvrhi/utils.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {

static constexpr uint32_t UPT04_RESERVOIR_STRIDE = 64u;
static constexpr uint32_t UPT04_COMPACT_VERTEX_STRIDE = 48u;
static constexpr uint32_t UPT04_COMPACT_GEOMETRY_PUSH_CONSTANT_BYTES = 16u;
static constexpr uint32_t UPT04_COMPACT_LIGHT_STRIDE = 64u;
static constexpr uint32_t UPT04_COMPACT_LIGHT_PUSH_CONSTANT_BYTES = 4u;
static constexpr uint32_t UPT04_LIGHT_TILE_COUNT = 128u;
static constexpr uint32_t UPT04_LIGHT_TILE_DOMAIN_SIZE = 1024u;
static constexpr uint32_t UPT04_LIGHT_TILE_DOMAIN_COUNT = 2u;
static constexpr uint32_t UPT04_LIGHT_TILE_ENTRY_STRIDE = 16u;
static constexpr uint32_t UPT04_LIGHT_TILE_ENTRY_COUNT =
    UPT04_LIGHT_TILE_COUNT * UPT04_LIGHT_TILE_DOMAIN_SIZE *
        UPT04_LIGHT_TILE_DOMAIN_COUNT;
static constexpr uint32_t UPT04_LIGHT_TILE_PUSH_CONSTANT_BYTES = 48u;
static constexpr uint32_t UPT04_COMPACT_MATERIAL_STRIDE = 48u;
static constexpr uint32_t UPT04_COMPACT_MATERIAL_PUSH_CONSTANT_BYTES = 4u;
static constexpr uint32_t UPT04_CONTINUATION_HIT_STRIDE = 32u;
static constexpr uint32_t UPT04_PUSH_CONSTANT_BYTES = 216u;
static constexpr uint32_t UPT04_FAMILY_LOCAL_LIGHT = 1u << 0u;
static constexpr uint32_t UPT04_FAMILY_INDIRECT = 1u << 1u;
static constexpr uint32_t UPT04_ROUTE_STATIC_BUCKETS = 1u << 1u;
static constexpr uint32_t UPT04_EMISSIVE_LOOKUP_EXACT = 1u << 3u;
static constexpr uint32_t UPT04_MATERIAL_USE_SPECULAR_MAPS = 1u << 4u;
static constexpr uint32_t UPT04_MATERIAL_LEGACY_SPECMAP_TO_PBR = 1u << 5u;
static constexpr uint32_t UPT04_TWO_SIDED_EMISSIVES = 1u << 6u;
static constexpr uint32_t UPT04_MATERIAL_DECODE_TEXTURES = 1u << 7u;
static constexpr uint32_t UPT04_EMISSIVE_TRIAL_COUNT_SHIFT = 8u;
static constexpr uint32_t UPT04_EMISSIVE_TRIAL_COUNT_MASK = 0x1fu << UPT04_EMISSIVE_TRIAL_COUNT_SHIFT;
static constexpr uint32_t UPT04_DIRECT_TARGET_PDF_PARITY = 1u << 29u;
static constexpr uint32_t UPT04_DIRECT_PROPOSAL_PARITY = 1u << 30u;
static constexpr uint32_t UPT04_D0_PREVIOUS_BEST = 1u << 31u;
static constexpr uint32_t UPT04_ANALYTIC_PORTAL_DOMAIN = 1u << 28u;
static constexpr uint32_t UPT04_CONTROL_METADATA_VALID_BIT = 1u << 31u;
static constexpr uint32_t UPT04_CONTROL_METADATA_COUNT_MASK = 0x7fffffffu;
static constexpr uint32_t UPT04_TRANSPORT_K_MAX = 2u;
static constexpr uint32_t UPT04_TRANSPORT_POLICY_ID = 1u;
static constexpr uint32_t UPT04_NEE_RIS_BASELINE_CANDIDATE_COUNT = 8u;
static constexpr uint32_t UPT04_NEE_RIS_PARITY_ANALYTIC_CANDIDATE_COUNT = 32u;
static constexpr uint32_t UPT04_NEE_RIS_MAX_EMISSIVE_CANDIDATE_COUNT = 16u;
static constexpr uint32_t UPT04_SECONDARY_NEE_BOUNCE_INDEX = 2u;
// Version 12 extends the packed word12 emissive replay-scale payload to primary
// NEE. T0/S0 can then consume the D0 region PDF without four repeated endpoint
// probes. Page generations invalidate the older primary-emissive encoding
// without clearing buffers.
static constexpr uint32_t UPT04_ABI_VERSION = 14u;
static constexpr uint32_t UPT04_DIAGNOSTIC_COUNTER_COUNT = 107u;
static constexpr uint32_t UPT04_DIAGNOSTIC_COUNTER_BYTES =
    UPT04_DIAGNOSTIC_COUNTER_COUNT * sizeof(uint32_t);
static constexpr uint32_t UPT04_DIAGNOSTIC_RESERVOIR_PROBE_WORD_COUNT = 16u;
static constexpr uint32_t UPT04_DIAGNOSTIC_D0_PROBE_WORD_COUNT = 16u;
static constexpr uint32_t UPT04_DIAGNOSTIC_C0_PROBE_WORD_COUNT = 20u;
static constexpr uint32_t UPT04_DIAGNOSTIC_PROBE_WORD_COUNT =
    UPT04_DIAGNOSTIC_RESERVOIR_PROBE_WORD_COUNT +
    UPT04_DIAGNOSTIC_D0_PROBE_WORD_COUNT +
    UPT04_DIAGNOSTIC_C0_PROBE_WORD_COUNT;
static constexpr uint32_t UPT04_DIAGNOSTIC_PROBE_BYTES =
    UPT04_DIAGNOSTIC_PROBE_WORD_COUNT * sizeof(uint32_t);
static constexpr uint32_t UPT04_DIAGNOSTIC_BYTES =
    UPT04_DIAGNOSTIC_COUNTER_BYTES + UPT04_DIAGNOSTIC_PROBE_BYTES;
static constexpr uint32_t UPT05_PUSH_CONSTANT_BYTES = 32u;
static constexpr uint32_t UPT07_PUSH_CONSTANT_BYTES = 168u;
static constexpr uint32_t UPT43_TEMPORAL_BOILING_PUSH_CONSTANT_BYTES = 16u;
static constexpr uint32_t UPT07_MAXIMUM_HISTORY_AGE = 63u;
static constexpr uint32_t UPT07_MAXIMUM_HISTORY_CONTRIBUTION_RATIO = 32u;
static constexpr uint32_t UPT07_GEOMETRY_FLAG_PREVIOUS_BEST_SEED = 1u << 31u;
static constexpr uint32_t UPT07_GEOMETRY_FLAG_PAIRWISE_MIS = 1u << 27u;
static constexpr uint32_t UPT07_GEOMETRY_FLAG_DUPLICATION_MAP = 1u << 26u;
static constexpr uint32_t UPT07_GEOMETRY_FLAG_INDIRECT_REPLAY = 1u << 25u;
static constexpr uint32_t UPT07_GEOMETRY_FLAG_EARLY_RECONNECT = 1u << 24u;
static constexpr uint32_t UPT07_GEOMETRY_FLAG_RECONNECT_DIAGNOSTICS = 1u << 23u;
static constexpr uint32_t UPT07_GEOMETRY_FLAG_ROUTE_DIAGNOSTICS = 1u << 22u;
static constexpr uint32_t UPT07_GEOMETRY_FLAG_TEMPORAL_PERMUTATION = 1u << 21u;
static constexpr uint32_t UPT07_TEMPORAL_DIAGNOSTIC_COUNT = 32u;
static constexpr uint32_t UPT07_TEMPORAL_DIAGNOSTIC_BYTES =
    UPT07_TEMPORAL_DIAGNOSTIC_COUNT * sizeof(uint32_t);
static constexpr uint32_t UPT07_WORK_BUDGET_COUNTER_COUNT = 8u;
static constexpr uint32_t UPT07_WORK_BUDGET_SITE_COUNT = 4u;
static constexpr uint32_t UPT07_WORK_BUDGET_ROUTE_COUNT = 4u;
static constexpr uint32_t UPT07_WORK_BUDGET_SITE_METRIC_COUNT = 4u;
static constexpr uint32_t UPT07_WORK_BUDGET_SITE_WORD_OFFSET =
    UPT07_WORK_BUDGET_COUNTER_COUNT;
static constexpr uint32_t UPT07_WORK_BUDGET_VIOLATION_WORD =
    UPT07_WORK_BUDGET_SITE_WORD_OFFSET + UPT07_WORK_BUDGET_SITE_COUNT
    + UPT07_WORK_BUDGET_ROUTE_COUNT;
static constexpr uint32_t UPT07_WORK_BUDGET_WORDS_PER_PIXEL =
    UPT07_WORK_BUDGET_VIOLATION_WORD + 1u;
static constexpr uint32_t UPT08_PUSH_CONSTANT_BYTES = 16u;
static constexpr uint32_t UPT09_PUSH_CONSTANT_BYTES = 96u;
static constexpr uint32_t UPT09_MAXIMUM_INPUT_M = 32u;
static constexpr uint32_t UPT09_REGULAR_NEIGHBOR_COUNT = 3u;
static constexpr uint32_t UPT09_RESCUE_NEIGHBOR_COUNT = 12u;
static constexpr float UPT09_NEIGHBOR_RADIUS = 30.0f;
static constexpr uint32_t UPT42_REUSE_TEXTURE_STRIDE = sizeof(uint32_t);
static constexpr float UPT42_REUSE_TEXTURE_SIGMA = 16.0f;
static constexpr std::array<uint32_t, 3> UPT42_REUSE_TEXTURE_SIZES = {
    254u, 230u, 210u
};
static constexpr uint32_t UPT42_REUSE_TEXTURE_DELTA_ENTRY_COUNT =
    254u * 254u + 230u * 230u + 210u * 210u;
static constexpr uint32_t UPT42_REUSE_TEXTURE_ENTRY_COUNT =
    UPT42_REUSE_TEXTURE_DELTA_ENTRY_COUNT;
static constexpr uint32_t UPT43_SPATIAL_SHIFT_STRIDE = 16u;
static constexpr uint32_t UPT438_DISOCCLUSION_HISTORY_M = 8u;
static constexpr uint32_t UPT438_DISOCCLUSION_MAX_SAMPLES = 8u;

struct Upt42ReuseTextureStats
{
    uint32_t size = 0u;
    uint32_t shuffleRounds = 0u;
    double componentSigma = 0.0;
    double meanRadius = 0.0;
};

static uint32_t Upt42HashWord(uint32_t value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

static uint32_t Upt42ShuffleRoundCount(float sigma)
{
    // ReSTIR PT Enhanced Equation 3. The fitted inverse-power terms matter
    // for small kernels; sigma=16 evaluates to exactly 128 rounds.
    const float inverseSigma = 1.0f / sigma;
    const float fitted = sigma * sigma * 0.5f
        + 1.46f * inverseSigma
        + 1.76f * inverseSigma * inverseSigma
        + 0.656f * inverseSigma * inverseSigma * inverseSigma
        + 0.5f;
    return static_cast<uint32_t>(std::floor(fitted));
}

static bool Upt42BuildReuseTexture(
    uint32_t size,
    uint32_t textureOrdinal,
    std::vector<uint32_t>& packedDeltas,
    Upt42ReuseTextureStats& stats)
{
    if (size == 0u || (size & 1u) != 0u)
        return false;

    const uint32_t pixelCount = size * size;
    const uint32_t linkCount = pixelCount / 2u;
    std::vector<uint32_t> links(pixelCount);
    for (uint32_t index = 0u; index < pixelCount; ++index)
        links[index] = index / 2u;

    const uint32_t shuffleRounds =
        Upt42ShuffleRoundCount(UPT42_REUSE_TEXTURE_SIGMA);
    for (uint32_t round = 0u; round < shuffleRounds; ++round)
    {
        const uint32_t offset = round & 1u;
        for (uint32_t tileY = 0u; tileY < size; tileY += 2u)
        {
            const uint32_t y0 = (tileY + offset) % size;
            const uint32_t y1 = (y0 + 1u) % size;
            for (uint32_t tileX = 0u; tileX < size; tileX += 2u)
            {
                const uint32_t x0 = (tileX + offset) % size;
                const uint32_t x1 = (x0 + 1u) % size;
                const uint32_t indices[4] = {
                    y0 * size + x0,
                    y0 * size + x1,
                    y1 * size + x0,
                    y1 * size + x1
                };
                uint32_t values[4] = {
                    links[indices[0]], links[indices[1]],
                    links[indices[2]], links[indices[3]]
                };
                uint32_t random = Upt42HashWord(
                    0x42a511e9u ^ (textureOrdinal * 0x9e3779b9u)
                    ^ (round * 0x85ebca6bu)
                    ^ (tileX * 0xc2b2ae35u)
                    ^ (tileY * 0x27d4eb2fu));
                for (uint32_t remaining = 3u; remaining > 0u; --remaining)
                {
                    random = Upt42HashWord(random + remaining);
                    const uint32_t selected = random % (remaining + 1u);
                    std::swap(values[remaining], values[selected]);
                }
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                    links[indices[lane]] = values[lane];
            }
        }
    }

    std::vector<uint32_t> first(linkCount, UINT32_MAX);
    std::vector<uint32_t> second(linkCount, UINT32_MAX);
    for (uint32_t index = 0u; index < pixelCount; ++index)
    {
        const uint32_t link = links[index];
        if (link >= linkCount)
            return false;
        if (first[link] == UINT32_MAX)
            first[link] = index;
        else if (second[link] == UINT32_MAX)
            second[link] = index;
        else
            return false;
    }

    const size_t base = packedDeltas.size();
    packedDeltas.resize(base + pixelCount, 0u);
    double squaredComponentSum = 0.0;
    double radiusSum = 0.0;
    const int32_t halfSize = static_cast<int32_t>(size / 2u);
    for (uint32_t link = 0u; link < linkCount; ++link)
    {
        if (first[link] == UINT32_MAX || second[link] == UINT32_MAX)
            return false;
        const uint32_t endpoints[2] = { first[link], second[link] };
        for (uint32_t direction = 0u; direction < 2u; ++direction)
        {
            const uint32_t from = endpoints[direction];
            const uint32_t to = endpoints[direction ^ 1u];
            int32_t dx = static_cast<int32_t>(to % size)
                - static_cast<int32_t>(from % size);
            int32_t dy = static_cast<int32_t>(to / size)
                - static_cast<int32_t>(from / size);
            if (dx > halfSize)
                dx -= static_cast<int32_t>(size);
            else if (dx < -halfSize)
                dx += static_cast<int32_t>(size);
            if (dy > halfSize)
                dy -= static_cast<int32_t>(size);
            else if (dy < -halfSize)
                dy += static_cast<int32_t>(size);
            if (dx == 0 && dy == 0
                || dx < INT16_MIN || dx > INT16_MAX
                || dy < INT16_MIN || dy > INT16_MAX)
            {
                return false;
            }
            packedDeltas[base + from] =
                static_cast<uint32_t>(static_cast<uint16_t>(dx))
                | (static_cast<uint32_t>(static_cast<uint16_t>(dy)) << 16u);
            squaredComponentSum += double(dx * dx + dy * dy);
            radiusSum += std::sqrt(double(dx * dx + dy * dy));
        }
    }

    // Validate the exact involution after the same toroidal wrapping used by
    // the shader. This is a construction-time proof, not a per-frame clear.
    for (uint32_t index = 0u; index < pixelCount; ++index)
    {
        const uint32_t packed = packedDeltas[base + index];
        const int32_t dx = static_cast<int16_t>(packed & 0xffffu);
        const int32_t dy = static_cast<int16_t>(packed >> 16u);
        const int32_t x = static_cast<int32_t>(index % size);
        const int32_t y = static_cast<int32_t>(index / size);
        const uint32_t partnerX = static_cast<uint32_t>(
            (x + dx + static_cast<int32_t>(size)) % static_cast<int32_t>(size));
        const uint32_t partnerY = static_cast<uint32_t>(
            (y + dy + static_cast<int32_t>(size)) % static_cast<int32_t>(size));
        const uint32_t partner = partnerY * size + partnerX;
        const uint32_t reversePacked = packedDeltas[base + partner];
        const int32_t reverseDx = static_cast<int16_t>(reversePacked & 0xffffu);
        const int32_t reverseDy = static_cast<int16_t>(reversePacked >> 16u);
        if ((static_cast<int32_t>(partnerX) + reverseDx
                    + static_cast<int32_t>(size))
                    % static_cast<int32_t>(size) != x
            || (static_cast<int32_t>(partnerY) + reverseDy
                    + static_cast<int32_t>(size))
                    % static_cast<int32_t>(size) != y)
        {
            return false;
        }
    }

    stats.size = size;
    stats.shuffleRounds = shuffleRounds;
    stats.componentSigma = std::sqrt(
        squaredComponentSum / (2.0 * double(pixelCount)));
    stats.meanRadius = radiusSum / double(pixelCount);
    return true;
}

static const char* Upt04BackendName(PathTraceUnifiedPtBackend backend)
{
    return backend == PathTraceUnifiedPtBackend::RayQuery
        ? "rayquery"
        : "raygen";
}

static const char* Upt04FamilyName(PathTraceUnifiedPtFamily family)
{
    switch (family)
    {
    case PathTraceUnifiedPtFamily::Unified:
        return "unified";
    case PathTraceUnifiedPtFamily::IndirectOnly:
        return "indirect-only";
    default:
        return "direct-only";
    }
}

static bool Upt30SharedSpatialEnabled(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    return r_pathTracingUnifiedPtSharedSpatial.GetBool()
        && inputs.temporal
        && r_pathTracingUnifiedPtTemporalIndirect.GetBool()
        && r_pathTracingUnifiedPtSharedReuseAdapter.GetBool()
        && r_pathTracingUnifiedPtCommonGrisMerge.GetBool()
        && r_pathTracingUnifiedPtTemporalPairwise.GetBool()
        && inputs.family == PathTraceUnifiedPtFamily::Unified
        && inputs.compactLights
        && inputs.duplication
        && !inputs.lambertDiagnostic
        && !inputs.frozenStaticDiagnostic
        && !inputs.frozenLightDiagnostic
        && inputs.temporalBottleneckProbe == 0u
        && !r_pathTracingUnifiedPtTemporalEarlyReconnect.GetBool()
        && !r_pathTracingUnifiedPtTemporalRouteDiagnostics.GetBool();
}

static bool Upt38ThreeVertexSpatialEnabled(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    return inputs.threeVertexInitial
        && inputs.spatial
        && Upt30SharedSpatialEnabled(inputs)
        && r_pathTracingUnifiedPtSpatialStoredSourceTarget.GetBool()
        && r_pathTracingUnifiedPtSpatialWorkgroupPairing.GetBool()
        && r_pathTracingUnifiedPtSpatialEmptyRescue.GetBool()
        && !r_pathTracingUnifiedPtSpatialMultiNeighbor.GetBool();
}

static bool Upt43TemporalReplayCompactionEnabled(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    return r_pathTracingUnifiedPtTemporalReplayCompaction.GetBool()
        && inputs.temporal
        && r_pathTracingUnifiedPtTemporalIndirect.GetBool()
        && r_pathTracingUnifiedPtSharedReuseAdapter.GetBool()
        && r_pathTracingUnifiedPtCommonGrisMerge.GetBool()
        && r_pathTracingUnifiedPtTemporalPairwise.GetBool()
        && inputs.family == PathTraceUnifiedPtFamily::Unified
        && inputs.compactLights
        && inputs.duplication
        && !inputs.lambertDiagnostic
        && !inputs.frozenStaticDiagnostic
        && !inputs.frozenLightDiagnostic
        && inputs.temporalBottleneckProbe == 0u
        && !r_pathTracingUnifiedPtTemporalEarlyReconnect.GetBool()
        && !r_pathTracingUnifiedPtTemporalRouteDiagnostics.GetBool();
}

static bool Upt436TemporalBoilingFilterEnabled(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    return r_pathTracingUnifiedPtTemporalBoilingFilter.GetBool()
        && Upt43TemporalReplayCompactionEnabled(inputs);
}

static bool Upt43SpatialShiftPrepassEnabled(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    return Upt30SharedSpatialEnabled(inputs)
        && r_pathTracingUnifiedPtSpatialStoredSourceTarget.GetBool()
        && r_pathTracingUnifiedPtSpatialWorkgroupPairing.GetBool()
        && r_pathTracingUnifiedPtSpatialEmptyRescue.GetBool()
        && r_pathTracingUnifiedPtSpatialReuseTexture.GetBool()
        && r_pathTracingUnifiedPtSpatialShiftPrepass.GetBool()
        && !r_pathTracingUnifiedPtSpatialMultiNeighbor.GetBool()
        && !r_pathTracingUnifiedPtThreeVertexInitial.GetBool()
        && !inputs.threeVertexInitial;
}

static bool Upt438SpatialDisocclusionBoostEnabled(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    return r_pathTracingUnifiedPtSpatialDisocclusionBoost.GetBool()
        && Upt43TemporalReplayCompactionEnabled(inputs)
        && Upt30SharedSpatialEnabled(inputs)
        && r_pathTracingUnifiedPtSpatialStoredSourceTarget.GetBool()
        && r_pathTracingUnifiedPtSpatialWorkgroupPairing.GetBool()
        && r_pathTracingUnifiedPtSpatialEmptyRescue.GetBool()
        && !r_pathTracingUnifiedPtSpatialMultiNeighbor.GetBool()
        && !r_pathTracingUnifiedPtSpatialReuseTexture.GetBool()
        && !r_pathTracingUnifiedPtSpatialShiftPrepass.GetBool()
        && !r_pathTracingUnifiedPtTemporalPermutationSampling.GetBool()
        && !inputs.threeVertexInitial;
}

static uint32_t Upt04FamilyMask(PathTraceUnifiedPtFamily family)
{
    switch (family)
    {
    case PathTraceUnifiedPtFamily::Unified:
        return UPT04_FAMILY_LOCAL_LIGHT | UPT04_FAMILY_INDIRECT;
    case PathTraceUnifiedPtFamily::IndirectOnly:
        return UPT04_FAMILY_INDIRECT;
    default:
        return UPT04_FAMILY_LOCAL_LIGHT;
    }
}

static uint32_t Upt04StableLightIdentityFingerprint(
    const PathTraceUnifiedLightRecord& light)
{
    uint32_t hash = 2166136261u;
    hash = (hash ^ light.type) * 16777619u;
    hash = (hash ^ light.identityA) * 16777619u;
    hash = (hash ^ light.identityB) * 16777619u;
    hash = (hash ^ light.materialOrLightId) * 16777619u;
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    hash ^= hash >> 15u;
    hash *= 0x846ca68bu;
    hash ^= hash >> 16u;
    return hash != 0u ? hash : 1u;
}

static const char* Upt04ReceiverName(uint32_t mode)
{
    return mode == 2u
        ? "compact32"
        : (mode == 1u ? "compact48" : "legacy176");
}

static const char* Upt04InitialShaderPath(
    PathTraceUnifiedPtBackend backend,
    PathTraceUnifiedPtFamily family,
    uint32_t primaryReceiverMode,
    bool compactGeometry,
    bool compactLights,
    bool compactMaterials,
    bool splitContinuation,
    bool directProposalParity)
{
    if (backend == PathTraceUnifiedPtBackend::RayQuery)
    {
        switch (family)
        {
        case PathTraceUnifiedPtFamily::Unified:
            if (directProposalParity && primaryReceiverMode == 2u &&
                !compactGeometry && !compactLights && !compactMaterials &&
                !splitContinuation)
            {
                return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_unified_parity1.bin";
            }
            return splitContinuation
                ? (compactMaterials
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48_light64_material48_continuation32.bin"
                    : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48_light64_continuation32.bin")
                : (compactMaterials
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48_light64_material48.bin"
                : (compactLights
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48_light64.bin"
                : (compactGeometry
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48.bin"
                : (primaryReceiverMode == 2u
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32.bin"
                : (primaryReceiverMode == 1u
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact.bin"
                    : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery.bin")))));
        case PathTraceUnifiedPtFamily::IndirectOnly:
            return compactMaterials
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_rayquery_compact32_geometry48_light64_material48.bin"
                : (compactLights
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_rayquery_compact32_geometry48_light64.bin"
                : (compactGeometry
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_rayquery_compact32_geometry48.bin"
                : (primaryReceiverMode == 2u
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_rayquery_compact32.bin"
                : (primaryReceiverMode == 1u
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_rayquery_compact.bin"
                    : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_rayquery.bin"))));
        default:
            return compactMaterials
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_rayquery_compact32_geometry48_light64_material48.bin"
                : (compactLights
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_rayquery_compact32_geometry48_light64.bin"
                : (compactGeometry
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_rayquery_compact32_geometry48.bin"
                : (primaryReceiverMode == 2u
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_rayquery_compact32.bin"
                : (primaryReceiverMode == 1u
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_rayquery_compact.bin"
                    : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_rayquery.bin"))));
        }
    }

    switch (family)
    {
    case PathTraceUnifiedPtFamily::Unified:
        return primaryReceiverMode == 2u
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_raygen_compact32.bin"
            : (primaryReceiverMode == 1u
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_raygen_compact.bin"
                : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_raygen.bin");
    case PathTraceUnifiedPtFamily::IndirectOnly:
        return primaryReceiverMode == 2u
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_raygen_compact32.bin"
            : (primaryReceiverMode == 1u
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_raygen_compact.bin"
                : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_raygen.bin");
    default:
        return primaryReceiverMode == 2u
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_raygen_compact32.bin"
            : (primaryReceiverMode == 1u
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_raygen_compact.bin"
                : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_raygen.bin");
    }
}

static const char* Upt04SplitDirectShaderPath(
    bool compactGeometry,
    bool compactLights,
    bool lightTiles)
{
    if (lightTiles)
    {
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_split_direct_rayquery_compact32_light_tiles.bin";
    }
    return compactGeometry
        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_split_direct_rayquery_compact32_geometry48_light64.bin"
        : (compactLights
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_split_direct_rayquery_compact32_light64.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_split_direct_rayquery_compact32.bin");
}

static const char* Upt04SplitIndirectShaderPath(
    bool compactGeometry,
    bool compactLights,
    bool compactMaterials,
    bool lightTiles,
    bool threeVertexInitial)
{
    if (threeVertexInitial)
    {
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_split_indirect_rayquery_compact32_light64_three_vertex.bin";
    }
    if (lightTiles)
    {
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_split_indirect_rayquery_compact32_light_tiles.bin";
    }
    if (!compactGeometry)
    {
        return compactLights
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_split_indirect_rayquery_compact32_light64.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_split_indirect_rayquery_compact32.bin";
    }
    return compactMaterials
        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_split_indirect_rayquery_compact32_geometry48_light64_material48.bin"
        : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_split_indirect_rayquery_compact32_geometry48_light64.bin";
}

static const char* Upt04ContinuationTraceShaderPath()
{
    return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_continuation_trace_rayquery_compact32.bin";
}

static const char* Upt04LambertInitialShaderPath(
    bool compactGeometry,
    bool compactLights,
    bool compactMaterials,
    bool splitContinuation)
{
    if (splitContinuation)
    {
        return compactMaterials
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48_light64_material48_continuation32_lambert.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48_light64_continuation32_lambert.bin";
    }
    if (compactMaterials)
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48_light64_material48_lambert.bin";
    if (compactLights)
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48_light64_lambert.bin";
    if (compactGeometry)
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48_lambert.bin";
    return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_lambert.bin";
}

static const char* Upt04DiagnosticShaderPath()
{
    return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_diagnostics.bin";
}

static const char* Upt04FrozenInitialShaderPath(
    bool lambert,
    bool staticGeometry,
    bool flatLights)
{
    if (staticGeometry && flatLights)
    {
        return lambert
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_frozen_static_lambert.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_frozen_static.bin";
    }
    if (flatLights)
    {
        return lambert
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_frozen_lights_lambert.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_frozen_lights.bin";
    }
    return lambert
        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_frozen_geometry_lambert.bin"
        : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_frozen_geometry.bin";
}

static const char* Upt07FrozenTemporalShaderPath(
    bool lambert,
    bool duplication,
    bool staticGeometry,
    bool flatLights)
{
    if (lambert)
    {
        if (duplication)
        {
            return staticGeometry && flatLights
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_frozen_static_duplication_lambert.bin"
                : (flatLights
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_frozen_lights_duplication_lambert.bin"
                    : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_frozen_geometry_duplication_lambert.bin");
        }
        return staticGeometry && flatLights
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_frozen_static_lambert.bin"
            : (flatLights
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_frozen_lights_lambert.bin"
                : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_frozen_geometry_lambert.bin");
    }
    if (duplication)
    {
        return staticGeometry && flatLights
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_frozen_static_duplication.bin"
            : (flatLights
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_frozen_lights_duplication.bin"
                : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_frozen_geometry_duplication.bin");
    }
    return staticGeometry && flatLights
        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_frozen_static.bin"
        : (flatLights
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_frozen_lights.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_frozen_geometry.bin");
}

static uint32_t Upt04PipelineVariant(const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (inputs.diagnostics)
    {
        return 0u;
    }
    return inputs.shaderProofMode >= 7u && inputs.shaderProofMode <= 14u
        ? inputs.shaderProofMode
        : 0u;
}

static bool Upt04UsesCompactProbeLayout(uint32_t pipelineVariant)
{
    return pipelineVariant == 7u || pipelineVariant == 8u;
}

static bool Upt04UsesMinimalProductionSlotLayout(uint32_t pipelineVariant)
{
    return pipelineVariant == 12u || pipelineVariant == 13u;
}

static bool Upt04UsesTraversalIsolationLayout(uint32_t pipelineVariant)
{
    return pipelineVariant == 14u;
}

static bool Upt04UsesPushConstants(uint32_t pipelineVariant)
{
    return pipelineVariant != 7u && pipelineVariant != 8u &&
        pipelineVariant != 12u;
}

static bool Upt04UsesBindlessSet(uint32_t pipelineVariant)
{
    return pipelineVariant != 7u && pipelineVariant != 8u &&
        pipelineVariant != 11u && pipelineVariant != 12u &&
        pipelineVariant != 13u && pipelineVariant != 14u;
}

static bool Upt04UsesDirectOnlyProductionLayout(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    return Upt04PipelineVariant(inputs) == 0u &&
        !inputs.diagnostics &&
        inputs.family == PathTraceUnifiedPtFamily::DirectOnly;
}

static const char* Upt04LiveTlasProbePath(uint32_t pipelineVariant)
{
    switch (pipelineVariant)
    {
    case 7u:
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_live_tlas_probe_slang.bin";
    case 8u:
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_live_tlas_probe_dxc.bin";
    case 9u:
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_live_tlas_full_layout_probe.bin";
    case 14u:
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_full_frame_traversal_probe.bin";
    default:
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_live_tlas_full_host_probe.bin";
    }
}

static const char* Upt04LiveTlasProbeDebugName(uint32_t pipelineVariant)
{
    switch (pipelineVariant)
    {
    case 7u: return "PathTraceUnifiedPtLiveTlasProbeSlang";
    case 8u: return "PathTraceUnifiedPtLiveTlasProbeDxc";
    case 9u: return "PathTraceUnifiedPtLiveTlasFullLayoutProbeSlang";
    case 10u: return "PathTraceUnifiedPtLiveTlasFullHostProbeSlang";
    case 11u: return "PathTraceUnifiedPtLiveTlasSet0OnlyProbeSlang";
    case 12u: return "PathTraceUnifiedPtLiveTlasB0B4ProbeSlang";
    case 14u: return "PathTraceUnifiedPtFullFrameTraversalProbeSlang";
    default: return "PathTraceUnifiedPtLiveTlasB0B4PushProbeSlang";
    }
}

static const char* Upt04LiveTlasProbeMarkerName(uint32_t pipelineVariant)
{
    switch (pipelineVariant)
    {
    case 7u: return "UPT.D0 LiveTLAS Probe Slang 1x1";
    case 8u: return "UPT.D0 LiveTLAS Probe DXC 1x1";
    case 9u: return "UPT.D0 LiveTLAS FullLayout Probe Slang 1x1";
    case 10u: return "UPT.D0 LiveTLAS FullHost Probe Slang 1x1";
    case 11u: return "UPT.D0 LiveTLAS Set0Only Probe Slang 1x1";
    case 12u: return "UPT.D0 LiveTLAS B0B4 Probe Slang 1x1";
    case 14u: return "UPT.D0 TraversalOnly RayQuery";
    default: return "UPT.D0 LiveTLAS B0B4Push Probe Slang 1x1";
    }
}

static uint64_t Upt04HashBytes(const void* data, size_t size)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t Upt04HashValue(uint64_t hash, uint64_t value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

static float Upt04ThreeVertexContinueProbability()
{
    return idMath::ClampFloat(0.0f, 1.0f,
        r_pathTracingUnifiedPtThreeVertexContinueProbability.GetFloat());
}

static float Upt04ThreeVertexMinimumPathThroughput()
{
    return idMath::ClampFloat(0.0f, 1.0f,
        r_pathTracingUnifiedPtThreeVertexMinimumPathThroughput.GetFloat());
}

static uint32_t Upt04FloatBitPattern(float value)
{
    uint32_t bits = 0u;
    static_assert(sizeof(bits) == sizeof(value),
        "UPT control float bit transport requires binary32");
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint64_t Upt06BuildContentGeneration(
    const PathTraceUnifiedPtDispatchInputs& dispatch,
    uint32_t enabledFamilyMask,
    uint32_t specializationIdentity)
{
    // Full-width temporal compatibility fingerprint. It describes the stored
    // proposal/estimator ABI, not live scene contents. Moving geometry and
    // changing light payloads are handled by reprojection plus per-sample
    // identity replay; hashing those live signatures invalidated alternating
    // pages almost every frame and made temporal admission nondeterministic.
    // Camera cuts and resource resets are owned by the separate historyEpoch.
    uint64_t hash = 1469598103934665603ull;
    hash = Upt04HashValue(hash, UPT04_ABI_VERSION);
    // These words define the proposal mixture and stored-PDF interpretation.
    // They must invalidate a future history page even when scene identity did
    // not change.
    hash = Upt04HashValue(hash, enabledFamilyMask);
    hash = Upt04HashValue(hash, specializationIdentity);
    hash = Upt04HashValue(hash, dispatch.lambertDiagnostic ? 1u : 0u);
    hash = Upt04HashValue(hash,
        dispatch.frozenStaticDiagnostic ? 1u : 0u);
    hash = Upt04HashValue(hash,
        dispatch.frozenLightDiagnostic ? 1u : 0u);
    hash = Upt04HashValue(hash, dispatch.temporalBottleneckProbe);
    hash = Upt04HashValue(hash, UPT04_TRANSPORT_K_MAX);
    hash = Upt04HashValue(hash, UPT04_TRANSPORT_POLICY_ID);
    const uint32_t emissiveTrialCount = static_cast<uint32_t>(
        idMath::ClampInt(1, UPT04_NEE_RIS_MAX_EMISSIVE_CANDIDATE_COUNT,
            r_pathTracingReservoirCandidateTrials.GetInteger()));
    hash = Upt04HashValue(hash, dispatch.directProposalParity
        ? UPT04_NEE_RIS_PARITY_ANALYTIC_CANDIDATE_COUNT + emissiveTrialCount
        : UPT04_NEE_RIS_BASELINE_CANDIDATE_COUNT);
    hash = Upt04HashValue(hash, emissiveTrialCount);
    hash = Upt04HashValue(hash, UPT04_SECONDARY_NEE_BOUNCE_INDEX);
    hash = Upt04HashValue(hash, dispatch.lightTiles ? 1u : 0u);
    hash = Upt04HashValue(hash, dispatch.threeVertexInitial ? 1u : 0u);
    hash = Upt04HashValue(hash, dispatch.threeVertexInitial
        ? Upt04FloatBitPattern(Upt04ThreeVertexContinueProbability()) : 0u);
    hash = Upt04HashValue(hash, dispatch.threeVertexInitial
        ? Upt04FloatBitPattern(Upt04ThreeVertexMinimumPathThroughput()) : 0u);
    hash = Upt04HashValue(hash,
        r_pathTracingUnifiedPtD0PreviousBest.GetBool() ? 1u : 0u);
    hash = Upt04HashValue(hash,
        r_pathTracingUnifiedPtDirectTargetPdfParity.GetBool() ? 1u : 0u);
    hash = Upt04HashValue(hash,
        r_pathTracingUnifiedPtAnalyticPortalDomain.GetBool() ? 1u : 0u);
    hash = Upt04HashValue(hash, dispatch.width);
    hash = Upt04HashValue(hash, dispatch.height);
    hash = Upt04HashValue(hash, dispatch.materialPolicyFlags);
    return hash;
}

struct Upt04InitialControl
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t surfaceCount;
    uint32_t frameSampleIndex;
    uint32_t enabledFamilyMask;
    float primaryCameraOriginX;
    uint32_t emissiveRangeStart;
    uint32_t emissiveRangeCount;
    uint32_t analyticRangeStart;
    uint32_t analyticRangeCount;
    uint32_t availabilityFlags;
    uint32_t logicalTextureCount;
    float primaryCameraOriginY;
    float primaryCameraOriginZ;
    uint32_t shaderProofMode;
    uint32_t materialCount;
    uint32_t currentLightCount;
    uint32_t staticVertexCount;
    uint32_t staticIndexCount;
    uint32_t staticTriangleCount;
    uint32_t dynamicVertexCount;
    uint32_t dynamicIndexCount;
    uint32_t dynamicTriangleCount;
    uint32_t rigidVertexCount;
    uint32_t rigidIndexCount;
    uint32_t rigidTriangleCount;
    uint32_t rigidInstanceCount;
    uint32_t skinnedRouteRecordCount;
    uint32_t skinnedRouteTriangleCount;
    uint32_t skinnedSourceIndexCount;
    uint32_t skinnedCurrentVertexCount;
    float emissiveScale;
    float previousCameraOrigin[3];
    uint32_t previousToCurrentLightCountAndHistory;
    float previousCameraForward[3];
    float previousCameraTanX;
    float previousCameraLeft[3];
    float previousCameraTanY;
    float previousCameraUp[3];
    uint32_t emissiveDistributionCountAndValid;
    uint32_t emissiveLookupCapacityAndValid;
    uint32_t indirectPolicyFlags;
    uint32_t reservedControl0;
    uint32_t reservedControl1;
    float previousCameraJitterPixels[2];
};
static_assert(sizeof(Upt04InitialControl) == UPT04_PUSH_CONSTANT_BYTES,
    "UPT-04 host push constants must match Slang reflection");

struct Upt05ResolveControl
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t surfaceCount;
    uint32_t view;
    float primaryCameraOrigin[3];
    uint32_t reserved0;
};
static_assert(sizeof(Upt05ResolveControl) == UPT05_PUSH_CONSTANT_BYTES,
    "UPT-05 host push constants must match Slang reflection");

struct Upt07TemporalDirectControl
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t surfaceCount;
    uint32_t historyAvailable;
    uint32_t emissiveRangeStart;
    uint32_t emissiveRangeCount;
    uint32_t analyticRangeStart;
    uint32_t analyticRangeCount;
    uint32_t currentLightCount;
    uint32_t frameSampleIndex;
    uint32_t maximumHistoryM;
    uint32_t maximumHistoryAge;
    float previousCameraOrigin[3];
    uint32_t previousCameraValid;
    float previousCameraForward[3];
    float previousCameraTanX;
    float previousCameraLeft[3];
    float previousCameraTanY;
    float previousCameraUp[3];
    uint32_t previousCameraHistorySearchMode;
    uint32_t geometryAvailabilityFlags;
    float emissiveScale;
    uint32_t previousToCurrentLightCount;
    uint32_t maximumHistoryContributionRatio;
    float primaryCameraOrigin[3];
    uint32_t compactPrimaryHistory;
    uint32_t materialCount;
    uint32_t logicalTextureCount;
    uint32_t emissiveDistributionCountAndValid;
    uint32_t emissiveLookupCapacityAndValid;
    float previousCameraJitterPixels[2];
};
static_assert(sizeof(Upt07TemporalDirectControl) == UPT07_PUSH_CONSTANT_BYTES,
    "UPT-07 host push constants must match Slang reflection");

struct Upt43TemporalBoilingControl
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t surfaceCount;
    float strength;
};
static_assert(sizeof(Upt43TemporalBoilingControl) ==
        UPT43_TEMPORAL_BOILING_PUSH_CONSTANT_BYTES,
    "UPT-43.6 boiling-filter push constants must match Slang reflection");

struct Upt08Control
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t surfaceCount;
    uint32_t packedRowPitch;
};
static_assert(sizeof(Upt08Control) == UPT08_PUSH_CONSTANT_BYTES,
    "UPT-08 host push constants must match Slang reflection");

struct Upt09SpatialDirectControl
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t surfaceCount;
    uint32_t frameSampleIndex;
    uint32_t emissiveRangeStart;
    uint32_t emissiveRangeCount;
    uint32_t analyticRangeStart;
    uint32_t analyticRangeCount;
    uint32_t currentLightCount;
    uint32_t maximumInputM;
    uint32_t regularNeighborCount;
    uint32_t rescueNeighborCount;
    float neighborRadius;
    uint32_t geometryAvailabilityFlags;
    float emissiveScale;
    uint32_t proofMode;
    float primaryCameraOrigin[3];
    uint32_t compactPrimaryHistory;
    uint32_t materialCount;
    uint32_t logicalTextureCount;
    uint32_t emissiveDistributionCountAndValid;
    uint32_t emissiveLookupCapacityAndValid;
};
static_assert(sizeof(Upt09SpatialDirectControl) == UPT09_PUSH_CONSTANT_BYTES,
    "UPT-09 host push constants must match Slang reflection");

struct Upt04CompactGeometryPackControl
{
    uint32_t staticVertexCount;
    uint32_t dynamicVertexCount;
    uint32_t rigidVertexCount;
    uint32_t skinnedVertexCount;
};
static_assert(
    sizeof(Upt04CompactGeometryPackControl) ==
        UPT04_COMPACT_GEOMETRY_PUSH_CONSTANT_BYTES,
    "UPT compact-geometry push constants must match Slang reflection");

struct Upt04CompactLightPackControl
{
    uint32_t lightCount;
};
static_assert(
    sizeof(Upt04CompactLightPackControl) == UPT04_COMPACT_LIGHT_PUSH_CONSTANT_BYTES,
    "UPT compact-light push constants must match Slang reflection");

struct Upt04LightTilePresampleControl
{
    uint32_t tileCount;
    uint32_t tileDomainSize;
    uint32_t frameSampleIndex;
    uint32_t emissiveDistributionCount;
    uint32_t emissiveRangeStart;
    uint32_t emissiveRangeCount;
    uint32_t analyticRangeStart;
    uint32_t analyticRangeCount;
    uint32_t emissiveTrials;
    uint32_t analyticTrials;
    uint32_t candidateCount;
    uint32_t reserved0;
};
static_assert(
    sizeof(Upt04LightTilePresampleControl) ==
        UPT04_LIGHT_TILE_PUSH_CONSTANT_BYTES,
    "UPT light-tile push constants must match Slang reflection");

struct Upt04CompactMaterialPackControl
{
    uint32_t materialCount;
};
static_assert(
    sizeof(Upt04CompactMaterialPackControl) ==
        UPT04_COMPACT_MATERIAL_PUSH_CONSTANT_BYTES,
    "UPT compact-material push constants must match Slang reflection");

class Upt04MarkerScope
{
public:
    Upt04MarkerScope(nvrhi::ICommandList* commandList, const char* name, bool enabled)
        : m_commandList(enabled ? commandList : nullptr)
    {
        if (m_commandList)
        {
            m_commandList->beginMarker(name);
        }
    }

    ~Upt04MarkerScope()
    {
        if (m_commandList)
        {
            m_commandList->endMarker();
        }
    }

private:
    nvrhi::ICommandList* m_commandList;
};

static bool Upt04ReadShader(
    const char* path,
    void*& data,
    int& size,
    ID_TIME_T& timestamp,
    uint64_t& hash)
{
    data = nullptr;
    size = fileSystem->ReadFile(path, &data, &timestamp);
    if (size <= 0 || !data)
    {
        common->Printf("PathTraceUnifiedPt: couldn't read Slang SPIR-V %s\n", path);
        return false;
    }
    hash = Upt04HashBytes(data, static_cast<size_t>(size));
    return true;
}

static nvrhi::BufferHandle Upt04SkinnedIndexBuffer(const RtPathTraceSceneInputs& inputs)
{
    return inputs.geometry.skinnedSourceIndexBuffer
        ? inputs.geometry.skinnedSourceIndexBuffer
        : inputs.geometry.dynamicIndexBuffer;
}

static nvrhi::BufferHandle Upt04SkinnedVertexBuffer(const RtPathTraceSceneInputs& inputs)
{
    return inputs.geometry.skinnedCurrentOutputVertexBuffer
        ? inputs.geometry.skinnedCurrentOutputVertexBuffer
        : inputs.geometry.dynamicVertexBuffer;
}

static bool Upt04InputsValid(const PathTraceUnifiedPtDispatchInputs& dispatch)
{
    if (!dispatch.device || !dispatch.commandList || !dispatch.sceneInputs ||
        !dispatch.sceneInputs->valid || dispatch.width == 0 || dispatch.height == 0 ||
        !dispatch.primarySurfaceBuffer || !dispatch.primarySurfacePreviousBuffer)
    {
        return false;
    }
    if (dispatch.lightTiles &&
        (!dispatch.splitInitial || !dispatch.directProposalParity ||
         dispatch.backend != PathTraceUnifiedPtBackend::RayQuery ||
         dispatch.family != PathTraceUnifiedPtFamily::Unified ||
         dispatch.primaryReceiverMode != 2u || dispatch.compactGeometry ||
         dispatch.compactLights || dispatch.compactMaterials ||
         dispatch.diagnostics || dispatch.shaderProofMode != 6u))
    {
        return false;
    }
    if (dispatch.threeVertexInitial &&
        (!dispatch.splitInitial || dispatch.compactGeometry ||
         !dispatch.compactLights || dispatch.compactMaterials ||
         dispatch.lightTiles ||
         (dispatch.spatial && !Upt38ThreeVertexSpatialEnabled(dispatch)) ||
         dispatch.backend != PathTraceUnifiedPtBackend::RayQuery ||
         dispatch.family != PathTraceUnifiedPtFamily::Unified ||
         dispatch.primaryReceiverMode != 2u || dispatch.diagnostics ||
         dispatch.lambertDiagnostic || dispatch.shaderProofMode != 6u))
    {
        return false;
    }
    const uint32_t expectedPrimaryStride = dispatch.primaryReceiverMode == 2u
        ? PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER32_STRIDE
        : (dispatch.primaryReceiverMode == 1u
            ? PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER_STRIDE
            : 176u);
    if (dispatch.primarySurfaceBuffer->getDesc().structStride !=
        expectedPrimaryStride)
    {
        return false;
    }

    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    const RtPathTraceSceneInputMaterials& materials = inputs.materials;
    const RtPathTraceSceneInputLights& lights = inputs.lights;
    const bool commonGeometryValid = geometry.tlas &&
        geometry.staticVertexBuffer && geometry.staticIndexBuffer &&
        geometry.staticTriangleMaterialIndexBuffer &&
        geometry.dynamicVertexBuffer && geometry.dynamicIndexBuffer &&
        geometry.dynamicTriangleMaterialIndexBuffer &&
        geometry.rigidRouteVertexBuffer && geometry.rigidRouteIndexBuffer &&
        geometry.rigidRouteInstanceBuffer && geometry.skinnedHitRouteRecordBuffer &&
        geometry.skinnedHitRouteTriangleBuffer && Upt04SkinnedIndexBuffer(inputs) &&
        Upt04SkinnedVertexBuffer(inputs) &&
        lights.restirLightManagerCurrentPayloadBuffer &&
        lights.emissiveTriangleBuffer && lights.emissiveDistributionBuffer &&
        lights.unifiedPtEmissiveGeometryBuffer;
    if (!commonGeometryValid)
    {
        return false;
    }
    if (Upt04UsesDirectOnlyProductionLayout(dispatch))
    {
        return materials.materialTableBuffer && materials.textureBindlessLayout &&
            materials.textureDescriptorTable && materials.textureSampler;
    }
    return geometry.staticTriangleClassBuffer &&
        geometry.dynamicTriangleClassBuffer && materials.materialTableBuffer &&
        materials.textureBindlessLayout && materials.textureDescriptorTable &&
        materials.textureSampler && lights.unifiedPtEmissiveLookupBuffer &&
        lights.unifiedPtEmissiveLookupExact;
}

static void Upt04AddBindingLayoutItems(
    nvrhi::BindingLayoutDesc& desc,
    bool diagnostics,
    bool splitContinuation,
    bool lightTiles)
{
    desc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4));
    desc.addItem(nvrhi::BindingLayoutItem::Sampler(5));
    for (uint32_t slot = 6; slot <= 21; ++slot)
    {
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
    }
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(22));
    if (diagnostics)
    {
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(23));
    }
    if (splitContinuation)
    {
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(24));
    }
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(25));
    for (uint32_t slot = 26u; slot <= 29u; ++slot)
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
    if (lightTiles)
    {
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(30));
    }
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(32));
    desc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, UPT04_PUSH_CONSTANT_BYTES));
}

static void Upt04AddDirectBindingLayoutItems(nvrhi::BindingLayoutDesc& desc)
{
    desc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4));
    desc.addItem(nvrhi::BindingLayoutItem::Sampler(5));
    for (const uint32_t slot : { 6u, 7u, 8u, 10u, 11u, 12u, 14u,
            15u, 16u, 17u, 18u, 19u, 20u, 21u })
    {
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
    }
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(25));
    for (uint32_t slot = 26u; slot <= 29u; ++slot)
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(32));
    desc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT04_PUSH_CONSTANT_BYTES));
}

// T0 and shared S0 import the same geometry replay module. Keep its complete
// set-0 descriptor contract in one host helper so a shader-visible route
// buffer cannot be omitted from one reuse pass while remaining present in the
// other. Bindings 9 and 13 are the hardware-opaque triangle-class streams.
static void Upt04AddReplayGeometryLayoutItems(nvrhi::BindingLayoutDesc& desc)
{
    for (uint32_t slot = 6u; slot <= 21u; ++slot)
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(32));
}

static bool Upt04ReplayGeometryBindingsValid(
    const RtPathTraceSceneInputs& inputs)
{
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    return inputs.lights.emissiveTriangleBuffer
        && inputs.lights.unifiedPtEmissiveGeometryBuffer
        && geometry.staticVertexBuffer && geometry.staticIndexBuffer
        && geometry.staticTriangleClassBuffer
        && geometry.staticTriangleMaterialIndexBuffer
        && geometry.dynamicVertexBuffer && geometry.dynamicIndexBuffer
        && geometry.dynamicTriangleClassBuffer
        && geometry.dynamicTriangleMaterialIndexBuffer
        && geometry.rigidRouteVertexBuffer && geometry.rigidRouteIndexBuffer
        && geometry.rigidRouteInstanceBuffer
        && Upt04SkinnedVertexBuffer(inputs) && Upt04SkinnedIndexBuffer(inputs)
        && geometry.skinnedHitRouteRecordBuffer
        && geometry.skinnedHitRouteTriangleBuffer;
}

static void Upt04AddReplayGeometryBindingItems(
    nvrhi::BindingSetDesc& desc,
    const RtPathTraceSceneInputs& inputs)
{
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, inputs.lights.emissiveTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, geometry.staticVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, geometry.staticIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(9, geometry.staticTriangleClassBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(10, geometry.staticTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(11, geometry.dynamicVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, geometry.dynamicIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(13, geometry.dynamicTriangleClassBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(14, geometry.dynamicTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(15, geometry.rigidRouteVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, geometry.rigidRouteIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(17, geometry.rigidRouteInstanceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(18, Upt04SkinnedVertexBuffer(inputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(19, Upt04SkinnedIndexBuffer(inputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(20, geometry.skinnedHitRouteRecordBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(21, geometry.skinnedHitRouteTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        32, inputs.lights.unifiedPtEmissiveGeometryBuffer));
}

static void Upt04SetReplayGeometrySrvStates(
    nvrhi::ICommandList* commandList,
    const RtPathTraceSceneInputs& inputs)
{
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    commandList->setBufferState(inputs.lights.emissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(Upt04SkinnedVertexBuffer(inputs), nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(Upt04SkinnedIndexBuffer(inputs), nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.skinnedHitRouteRecordBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.skinnedHitRouteTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.unifiedPtEmissiveGeometryBuffer, nvrhi::ResourceStates::ShaderResource);
}

static nvrhi::BindingSetDesc Upt04BuildBindingSetDesc(
    const PathTraceUnifiedPtDispatchInputs& dispatch,
    nvrhi::BufferHandle page0,
    nvrhi::BufferHandle historyPage,
    nvrhi::BufferHandle diagnosticCounters,
    nvrhi::BufferHandle compactStaticVertices,
    nvrhi::BufferHandle compactDynamicVertices,
    nvrhi::BufferHandle compactRigidVertices,
    nvrhi::BufferHandle compactSkinnedVertices,
    nvrhi::BufferHandle compactLights,
    nvrhi::BufferHandle compactMaterials,
    nvrhi::BufferHandle continuationHits,
    nvrhi::BufferHandle lightTiles)
{
    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    const RtPathTraceSceneInputMaterials& materials = inputs.materials;
    const RtPathTraceSceneInputLights& lights = inputs.lights;

    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, geometry.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, dispatch.primarySurfaceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        2, dispatch.compactLights ? compactLights : lights.restirLightManagerCurrentPayloadBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        3, dispatch.compactMaterials ? compactMaterials : materials.materialTableBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, page0));
    desc.addItem(nvrhi::BindingSetItem::Sampler(5, materials.textureSampler));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, lights.emissiveTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        7, dispatch.compactGeometry ? compactStaticVertices : geometry.staticVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, geometry.staticIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(9, geometry.staticTriangleClassBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(10, geometry.staticTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        11, dispatch.compactGeometry ? compactDynamicVertices : geometry.dynamicVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, geometry.dynamicIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(13, geometry.dynamicTriangleClassBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(14, geometry.dynamicTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        15, dispatch.compactGeometry ? compactRigidVertices : geometry.rigidRouteVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, geometry.rigidRouteIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(17, geometry.rigidRouteInstanceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        18, dispatch.compactGeometry ? compactSkinnedVertices : Upt04SkinnedVertexBuffer(inputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(19, Upt04SkinnedIndexBuffer(inputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(20, geometry.skinnedHitRouteRecordBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(21, geometry.skinnedHitRouteTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(22, lights.unifiedPtEmissiveLookupBuffer));
    if (dispatch.diagnostics)
    {
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            23, diagnosticCounters));
    }
    if (dispatch.splitContinuation)
    {
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            24, continuationHits));
    }
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        25, lights.emissiveDistributionBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        26, dispatch.primarySurfacePreviousBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        27, dispatch.primaryHistorySidecarPreviousBuffer
            ? dispatch.primaryHistorySidecarPreviousBuffer
            : dispatch.primarySurfacePreviousBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(28, historyPage));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        29, lights.restirLightManagerPreviousToCurrentBuffer
            ? lights.restirLightManagerPreviousToCurrentBuffer
            : lights.emissiveDistributionBuffer));
    if (dispatch.lightTiles)
    {
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            30, lightTiles));
    }
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        32, lights.unifiedPtEmissiveGeometryBuffer));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(0, UPT04_PUSH_CONSTANT_BYTES));
    return desc;
}

static nvrhi::BindingSetDesc Upt04BuildDirectBindingSetDesc(
    const PathTraceUnifiedPtDispatchInputs& dispatch,
    nvrhi::BufferHandle page0,
    nvrhi::BufferHandle historyPage,
    nvrhi::BufferHandle compactStaticVertices,
    nvrhi::BufferHandle compactDynamicVertices,
    nvrhi::BufferHandle compactRigidVertices,
    nvrhi::BufferHandle compactSkinnedVertices,
    nvrhi::BufferHandle compactLights,
    nvrhi::BufferHandle compactMaterials)
{
    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    const RtPathTraceSceneInputMaterials& materials = inputs.materials;
    const RtPathTraceSceneInputLights& lights = inputs.lights;
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, geometry.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, dispatch.primarySurfaceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        2, dispatch.compactLights ? compactLights : lights.restirLightManagerCurrentPayloadBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        3, dispatch.compactMaterials ? compactMaterials : materials.materialTableBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, page0));
    desc.addItem(nvrhi::BindingSetItem::Sampler(5, materials.textureSampler));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, lights.emissiveTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        7, dispatch.compactGeometry ? compactStaticVertices : geometry.staticVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, geometry.staticIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(10, geometry.staticTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        11, dispatch.compactGeometry ? compactDynamicVertices : geometry.dynamicVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, geometry.dynamicIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(14, geometry.dynamicTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        15, dispatch.compactGeometry ? compactRigidVertices : geometry.rigidRouteVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, geometry.rigidRouteIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(17, geometry.rigidRouteInstanceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        18, dispatch.compactGeometry ? compactSkinnedVertices : Upt04SkinnedVertexBuffer(inputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(19, Upt04SkinnedIndexBuffer(inputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(20, geometry.skinnedHitRouteRecordBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(21, geometry.skinnedHitRouteTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        25, lights.emissiveDistributionBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        26, dispatch.primarySurfacePreviousBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        27, dispatch.primaryHistorySidecarPreviousBuffer
            ? dispatch.primaryHistorySidecarPreviousBuffer
            : dispatch.primarySurfacePreviousBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(28, historyPage));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        29, lights.restirLightManagerPreviousToCurrentBuffer
            ? lights.restirLightManagerPreviousToCurrentBuffer
            : lights.emissiveDistributionBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        32, lights.unifiedPtEmissiveGeometryBuffer));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(0, UPT04_PUSH_CONSTANT_BYTES));
    return desc;
}

static void Upt04SetSrvStates(
    nvrhi::ICommandList* commandList,
    const PathTraceUnifiedPtDispatchInputs& dispatch)
{
    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    commandList->setAccelStructState(geometry.tlas, nvrhi::ResourceStates::AccelStructRead);
    commandList->setBufferState(dispatch.primarySurfaceBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.restirLightManagerCurrentPayloadBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.materials.materialTableBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.emissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.emissiveDistributionBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(dispatch.primarySurfacePreviousBuffer, nvrhi::ResourceStates::ShaderResource);
    if (dispatch.primaryHistorySidecarPreviousBuffer)
        commandList->setBufferState(dispatch.primaryHistorySidecarPreviousBuffer, nvrhi::ResourceStates::ShaderResource);
    if (inputs.lights.restirLightManagerPreviousToCurrentBuffer)
        commandList->setBufferState(inputs.lights.restirLightManagerPreviousToCurrentBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(Upt04SkinnedVertexBuffer(inputs), nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(Upt04SkinnedIndexBuffer(inputs), nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.skinnedHitRouteRecordBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.skinnedHitRouteTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.unifiedPtEmissiveLookupBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.unifiedPtEmissiveGeometryBuffer, nvrhi::ResourceStates::ShaderResource);
}

static void Upt04SetDirectSrvStates(
    nvrhi::ICommandList* commandList,
    const PathTraceUnifiedPtDispatchInputs& dispatch)
{
    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    commandList->setAccelStructState(geometry.tlas, nvrhi::ResourceStates::AccelStructRead);
    commandList->setBufferState(dispatch.primarySurfaceBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.restirLightManagerCurrentPayloadBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.materials.materialTableBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.emissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.unifiedPtEmissiveGeometryBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.emissiveDistributionBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(dispatch.primarySurfacePreviousBuffer, nvrhi::ResourceStates::ShaderResource);
    if (dispatch.primaryHistorySidecarPreviousBuffer)
        commandList->setBufferState(dispatch.primaryHistorySidecarPreviousBuffer, nvrhi::ResourceStates::ShaderResource);
    if (inputs.lights.restirLightManagerPreviousToCurrentBuffer)
        commandList->setBufferState(inputs.lights.restirLightManagerPreviousToCurrentBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(Upt04SkinnedVertexBuffer(inputs), nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(Upt04SkinnedIndexBuffer(inputs), nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.skinnedHitRouteRecordBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.skinnedHitRouteTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
}

static Upt04InitialControl Upt04BuildControl(
    const PathTraceUnifiedPtDispatchInputs& dispatch,
    bool previousBestHistoryAvailable = false)
{
    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    const RtPathTraceSceneInputMaterials& materials = inputs.materials;
    const RtPathTraceSceneInputLights& lights = inputs.lights;
    const uint32_t enabledFamilyMask = Upt04FamilyMask(dispatch.family);
    const uint32_t specializationIdentity =
        static_cast<uint32_t>(dispatch.family) |
        (static_cast<uint32_t>(dispatch.backend) << 8u);
    const uint32_t emissiveTrialCount = static_cast<uint32_t>(
        idMath::ClampInt(1, UPT04_NEE_RIS_MAX_EMISSIVE_CANDIDATE_COUNT,
            r_pathTracingReservoirCandidateTrials.GetInteger()));
    const uint32_t availabilityFlags =
        (!dispatch.frozenStaticDiagnostic &&
                geometry.staticBucketRoutePublicationValid
            ? UPT04_ROUTE_STATIC_BUCKETS
            : 0u) |
        (!dispatch.frozenLightDiagnostic &&
                lights.unifiedPtEmissiveLookupExact
            ? UPT04_EMISSIVE_LOOKUP_EXACT
            : 0u) |
        ((dispatch.materialPolicyFlags
                & PATH_TRACE_UPT_MATERIAL_USE_SPECULAR_MAPS) != 0u
            ? UPT04_MATERIAL_USE_SPECULAR_MAPS
            : 0u) |
        ((dispatch.materialPolicyFlags
                & PATH_TRACE_UPT_MATERIAL_LEGACY_SPECMAP_TO_PBR) != 0u
            ? UPT04_MATERIAL_LEGACY_SPECMAP_TO_PBR
            : 0u) |
        ((dispatch.materialPolicyFlags
                & PATH_TRACE_UPT_MATERIAL_DECODE_TEXTURES) != 0u
            ? UPT04_MATERIAL_DECODE_TEXTURES
            : 0u) |
        (r_pathTracingReservoirTwoSidedEmissives.GetBool()
            ? UPT04_TWO_SIDED_EMISSIVES
            : 0u) |
        (dispatch.directProposalParity
            ? UPT04_DIRECT_PROPOSAL_PARITY
            : 0u) |
        (!dispatch.frozenLightDiagnostic && previousBestHistoryAvailable &&
                r_pathTracingUnifiedPtD0PreviousBest.GetBool()
            ? UPT04_D0_PREVIOUS_BEST
            : 0u) |
        ((emissiveTrialCount << UPT04_EMISSIVE_TRIAL_COUNT_SHIFT)
            & UPT04_EMISSIVE_TRIAL_COUNT_MASK) |
        (r_pathTracingUnifiedPtDirectTargetPdfParity.GetBool()
            ? UPT04_DIRECT_TARGET_PDF_PARITY
            : 0u) |
        (r_pathTracingUnifiedPtAnalyticPortalDomain.GetBool()
            ? UPT04_ANALYTIC_PORTAL_DOMAIN
            : 0u);
    const uint64_t surfaceCount64 = uint64_t(dispatch.width) * uint64_t(dispatch.height);

    Upt04InitialControl control = {};
    control.renderWidth = dispatch.width;
    control.renderHeight = dispatch.height;
    control.surfaceCount = static_cast<uint32_t>(surfaceCount64);
    control.frameSampleIndex = dispatch.frameSampleIndex;
    control.enabledFamilyMask = enabledFamilyMask;
    control.primaryCameraOriginX = dispatch.primaryCameraOrigin[0];
    control.emissiveRangeStart = dispatch.frozenLightDiagnostic
        ? 0u : lights.restirLightManagerEmissiveRangeOffset;
    control.emissiveRangeCount = dispatch.frozenLightDiagnostic
        ? 0u : lights.restirLightManagerEmissiveRangeCount;
    control.analyticRangeStart = lights.restirLightManagerDoomAnalyticRangeOffset;
    control.analyticRangeCount = lights.restirLightManagerDoomAnalyticSampleableCount;
    control.availabilityFlags = availabilityFlags;
    control.logicalTextureCount = static_cast<uint32_t>(Max(0, materials.logicalTextureDescriptorCount));
    control.primaryCameraOriginY = dispatch.primaryCameraOrigin[1];
    control.primaryCameraOriginZ = dispatch.primaryCameraOrigin[2];
    control.shaderProofMode = dispatch.shaderProofMode;
    control.materialCount = static_cast<uint32_t>(Max(0, materials.materialTableEntryCount));
    control.currentLightCount = static_cast<uint32_t>(Max(0, lights.restirLightManagerCurrentPayloadCount));
    control.staticVertexCount = static_cast<uint32_t>(Max(0, geometry.staticVertexCount));
    control.staticIndexCount = static_cast<uint32_t>(Max(0, geometry.staticIndexCount));
    control.staticTriangleCount = static_cast<uint32_t>(Max(0, geometry.staticTriangleCount));
    control.dynamicVertexCount = dispatch.frozenStaticDiagnostic
        ? 0u : static_cast<uint32_t>(Max(0, geometry.dynamicVertexCount));
    control.dynamicIndexCount = dispatch.frozenStaticDiagnostic
        ? 0u : static_cast<uint32_t>(Max(0, geometry.dynamicIndexCount));
    control.dynamicTriangleCount = dispatch.frozenStaticDiagnostic
        ? 0u : static_cast<uint32_t>(Max(0, geometry.dynamicTriangleCount));
    control.rigidVertexCount = dispatch.frozenStaticDiagnostic
        ? 0u : static_cast<uint32_t>(Max(0, geometry.rigidRouteVertexCount));
    control.rigidIndexCount = dispatch.frozenStaticDiagnostic
        ? 0u : static_cast<uint32_t>(Max(0, geometry.rigidRouteIndexCount));
    control.rigidTriangleCount = dispatch.frozenStaticDiagnostic
        ? 0u : static_cast<uint32_t>(Max(0, geometry.rigidRouteTriangleCount));
    control.rigidInstanceCount = dispatch.frozenStaticDiagnostic
        ? 0u : static_cast<uint32_t>(Max(0, geometry.rigidRouteInstanceCount));
    control.skinnedRouteRecordCount = dispatch.frozenStaticDiagnostic
        ? 0u : static_cast<uint32_t>(Max(0, geometry.skinnedHitRouteRecordCount));
    control.skinnedRouteTriangleCount = dispatch.frozenStaticDiagnostic
        ? 0u : static_cast<uint32_t>(Max(0, geometry.skinnedHitRouteTriangleCount));
    control.skinnedSourceIndexCount = dispatch.frozenStaticDiagnostic
        ? 0u : static_cast<uint32_t>(Max(0, geometry.skinnedSourceIndexCount));
    control.skinnedCurrentVertexCount = dispatch.frozenStaticDiagnostic
        ? 0u : static_cast<uint32_t>(Max(0, geometry.skinnedGpuComputeVertexCount));
    control.emissiveScale = Max(0.0f, dispatch.emissiveScale);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
        control.previousCameraOrigin[axis] = dispatch.previousCameraOrigin[axis];
        control.previousCameraForward[axis] = dispatch.previousCameraForward[axis];
        control.previousCameraLeft[axis] = dispatch.previousCameraLeft[axis];
        control.previousCameraUp[axis] = dispatch.previousCameraUp[axis];
    }
    const uint32_t previousToCurrentCount = dispatch.frozenLightDiagnostic
        ? control.currentLightCount
        : static_cast<uint32_t>(Max(
            0, lights.restirLightManagerPreviousToCurrentCount));
    control.previousToCurrentLightCountAndHistory =
        (previousToCurrentCount & UPT04_CONTROL_METADATA_COUNT_MASK)
        | (previousBestHistoryAvailable
            ? UPT04_CONTROL_METADATA_VALID_BIT : 0u);
    control.previousCameraTanX = dispatch.previousCameraTanX;
    control.previousCameraTanY = dispatch.previousCameraTanY;
    const uint32_t distributionCount = dispatch.frozenLightDiagnostic
        ? 0u : static_cast<uint32_t>(Max(
            0, lights.emissiveDistributionCount));
    control.emissiveDistributionCountAndValid =
        (distributionCount & UPT04_CONTROL_METADATA_COUNT_MASK)
        | (lights.emissiveDistributionValid && distributionCount != 0u
            ? UPT04_CONTROL_METADATA_VALID_BIT : 0u);
    const uint32_t emissiveLookupCapacity = dispatch.frozenLightDiagnostic
        ? 0u : static_cast<uint32_t>(Max(
            0, lights.unifiedPtEmissiveLookupCount));
    control.emissiveLookupCapacityAndValid =
        (emissiveLookupCapacity & UPT04_CONTROL_METADATA_COUNT_MASK)
        | (lights.unifiedPtEmissiveLookupExact && emissiveLookupCapacity >= 2u
            ? UPT04_CONTROL_METADATA_VALID_BIT : 0u);
    // Preserve the accepted 208-byte prefix: UPT-37 transports two binary32
    // values through the previously reserved uint words. The RR reprojection
    // jitter is appended after that prefix.
    control.reservedControl0 = Upt04FloatBitPattern(
        Upt04ThreeVertexContinueProbability());
    control.reservedControl1 = Upt04FloatBitPattern(
        Upt04ThreeVertexMinimumPathThroughput());
    control.previousCameraJitterPixels[0] =
        dispatch.previousCameraJitterPixels[0];
    control.previousCameraJitterPixels[1] =
        dispatch.previousCameraJitterPixels[1];
    return control;
}

} // namespace

void PathTraceUnifiedPtState::ReleasePipeline()
{
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescs[page] = nvrhi::BindingSetDesc();
        m_bindingSetDescValid[page] = false;
    }
    m_shaderTable = nullptr;
    m_rayPipeline = nullptr;
    m_rayGenerationLibrary = nullptr;
    m_missLibrary = nullptr;
    m_closestHitLibrary = nullptr;
    m_splitIndirectComputePipeline = nullptr;
    m_splitIndirectComputeShader = nullptr;
    m_computePipeline = nullptr;
    m_computeShader = nullptr;
    m_bindingLayout = nullptr;
    m_pipelineAttempted = false;
}

void PathTraceUnifiedPtState::ReleaseCompactGeometry()
{
    m_compactGeometryBindingSet = nullptr;
    m_compactGeometryBindingSetDesc = nvrhi::BindingSetDesc();
    m_compactGeometryBindingSetDescValid = false;
    m_compactGeometryPipeline = nullptr;
    m_compactGeometryShader = nullptr;
    m_compactGeometryBindingLayout = nullptr;
    m_compactGeometryPipelineAttempted = false;
    m_compactStaticVertices = nullptr;
    m_compactDynamicVertices = nullptr;
    m_compactRigidVertices = nullptr;
    m_compactSkinnedVertices = nullptr;
    m_compactStaticVertexCapacity = 0;
    m_compactDynamicVertexCapacity = 0;
    m_compactRigidVertexCapacity = 0;
    m_compactSkinnedVertexCapacity = 0;
}

void PathTraceUnifiedPtState::ReleaseCompactLights()
{
    m_compactLightBindingSet = nullptr;
    m_compactLightBindingSetDesc = nvrhi::BindingSetDesc();
    m_compactLightBindingSetDescValid = false;
    m_compactLightPipeline = nullptr;
    m_compactLightShader = nullptr;
    m_compactLightBindingLayout = nullptr;
    m_compactLightPipelineAttempted = false;
    m_compactLightsBuffer = nullptr;
    m_compactLightCapacity = 0;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_temporalBindingSets[page] = nullptr;
        m_temporalBindingSetDescValid[page] = false;
        m_temporalReplayBindingSets[page] = nullptr;
        m_temporalReplayBindingSetDescValid[page] = false;
        m_spatialBindingSets[page] = nullptr;
        m_spatialBindingSetDescValid[page] = false;
    }
}

void PathTraceUnifiedPtState::ReleaseLightTiles()
{
    m_lightTileBindingSet = nullptr;
    m_lightTileBindingSetDesc = nvrhi::BindingSetDesc();
    m_lightTileBindingSetDescValid = false;
    m_lightTilePipeline = nullptr;
    m_lightTileShader = nullptr;
    m_lightTileBindingLayout = nullptr;
    m_lightTilePipelineAttempted = false;
    m_lightTileBuffer = nullptr;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescValid[page] = false;
    }
}

void PathTraceUnifiedPtState::ReleaseCompactMaterials()
{
    m_compactMaterialBindingSet = nullptr;
    m_compactMaterialBindingSetDesc = nvrhi::BindingSetDesc();
    m_compactMaterialBindingSetDescValid = false;
    m_compactMaterialPipeline = nullptr;
    m_compactMaterialShader = nullptr;
    m_compactMaterialBindingLayout = nullptr;
    m_compactMaterialPipelineAttempted = false;
    m_compactMaterialsBuffer = nullptr;
    m_compactMaterialCapacity = 0;
}

void PathTraceUnifiedPtState::ReleaseContinuation()
{
    m_continuationBindingSet = nullptr;
    m_continuationBindingSetDesc = nvrhi::BindingSetDesc();
    m_continuationBindingSetDescValid = false;
    m_continuationPipeline = nullptr;
    m_continuationShader = nullptr;
    m_continuationBindingLayout = nullptr;
    m_continuationPipelineAttempted = false;
    m_continuationHits = nullptr;
    m_continuationCapacity = 0;
    // The final D0 binding set owns the same sidecar as an SRV at slot 24.
    // Never retain a descriptor set that points at a released/replaced buffer.
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescValid[page] = false;
    }
}

void PathTraceUnifiedPtState::ReleaseTemporal()
{
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_temporalBindingSets[page] = nullptr;
        m_temporalBindingSetDescs[page] = nvrhi::BindingSetDesc();
        m_temporalBindingSetDescValid[page] = false;
        m_temporalReplayBindingSets[page] = nullptr;
        m_temporalReplayBindingSetDescs[page] = nvrhi::BindingSetDesc();
        m_temporalReplayBindingSetDescValid[page] = false;
        m_temporalBoilingFilterBindingSets[page] = nullptr;
    }
    m_temporalPipeline = nullptr;
    m_temporalShader = nullptr;
    m_temporalReplayPipeline = nullptr;
    m_temporalReplayShader = nullptr;
    m_temporalBoilingFilterPipeline = nullptr;
    m_temporalBoilingFilterShader = nullptr;
    m_temporalBindingLayout = nullptr;
    m_temporalReplayBindingLayout = nullptr;
    m_temporalBoilingFilterBindingLayout = nullptr;
    m_temporalReplayQueue = nullptr;
    m_temporalReplayMeta = nullptr;
    m_temporalReplayDispatchArgs = nullptr;
    m_temporalReplayReadback = nullptr;
    m_temporalReplayCapacity = 0u;
    m_temporalReplayReadbackPending = false;
    m_temporalReplayDiagnosticCaptured = false;
    m_temporalReplayReadbackDelayFrames = 0;
    m_temporalBottleneckBuffer = nullptr;
    m_temporalBottleneckCapacity = 0u;
    m_temporalPipelineAttempted = false;
    m_reportedTemporalHistoryAvailable = -1;
    m_reportedTemporalSkipReason = -1;
}

void PathTraceUnifiedPtState::ReleaseDuplication()
{
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_duplicationFillBindingSets[page] = nullptr;
        m_duplicationComputeBindingSets[page] = nullptr;
        m_duplicationScores[page] = nullptr;
        m_duplicationMetadata[page].Invalidate();
        m_temporalBindingSets[page] = nullptr;
        m_temporalBindingSetDescValid[page] = false;
        m_temporalReplayBindingSets[page] = nullptr;
        m_temporalReplayBindingSetDescValid[page] = false;
    }
    m_duplicationSampleIds = nullptr;
    m_duplicationFillPipeline = nullptr;
    m_duplicationComputePipeline = nullptr;
    m_duplicationFillShader = nullptr;
    m_duplicationComputeShader = nullptr;
    m_duplicationBindingLayout = nullptr;
    m_duplicationPipelineAttempted = false;
    m_duplicationWidth = 0u;
    m_duplicationHeight = 0u;
    m_duplicationPackedRowPitch = 0u;
}

void PathTraceUnifiedPtState::ReleaseSpatial()
{
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_spatialBindingSets[page] = nullptr;
        m_spatialBindingSetDescs[page] = nvrhi::BindingSetDesc();
        m_spatialBindingSetDescValid[page] = false;
        m_spatialBoostBindingSets[page] = nullptr;
        m_spatialBoostBindingSetDescs[page] = nvrhi::BindingSetDesc();
        m_spatialBoostBindingSetDescValid[page] = false;
    }
    m_spatialPipeline = nullptr;
    m_spatialShader = nullptr;
    m_spatialShiftPipeline = nullptr;
    m_spatialShiftShader = nullptr;
    m_spatialBindingLayout = nullptr;
    m_spatialBoostPipeline = nullptr;
    m_spatialBoostShader = nullptr;
    m_spatialBoostBindingLayout = nullptr;
    m_spatialPipelineAttempted = false;
    m_spatialExecutedThisFrame = false;
}

void PathTraceUnifiedPtState::Release()
{
    ReleasePipeline();
    ReleaseCompactGeometry();
    ReleaseCompactLights();
    ReleaseLightTiles();
    ReleaseCompactMaterials();
    ReleaseContinuation();
    ReleaseTemporal();
    ReleaseDuplication();
    ReleaseSpatial();
    m_spatialReuseTextureBuffer = nullptr;
    m_spatialShiftBuffer = nullptr;
    m_spatialShiftCapacity = 0u;
    m_spatialBoostReadback = nullptr;
    m_spatialBoostReadbackPending = false;
    m_spatialBoostDiagnosticCaptured = false;
    m_spatialBoostReadbackDelayFrames = 0;
    ReleaseResolve();
    for (TemporalGpuTimerSlot& slot : m_temporalGpuTimers)
    {
        slot.query = nullptr;
        slot.pending = false;
    }
    m_temporalGpuTimerCursor = 0u;
    m_temporalGpuTimingMode = UINT32_MAX;
    m_temporalGpuTimingWarmupRemaining = 0u;
    m_temporalGpuTimingSubmitted = 0u;
    m_temporalGpuTimingCompleted = 0u;
    m_temporalGpuTimingBatchComplete = false;
    m_temporalGpuTimingQueryFailureLogged = false;
    for (SpatialGpuTimerSlot& slot : m_spatialGpuTimers)
    {
        slot.query = nullptr;
        slot.pending = false;
    }
    m_spatialGpuTimerCursor = 0u;
    m_spatialGpuTimingMode = UINT32_MAX;
    m_spatialGpuTimingWarmupRemaining = 0u;
    m_spatialGpuTimingSubmitted = 0u;
    m_spatialGpuTimingCompleted = 0u;
    m_spatialGpuTimingBatchComplete = false;
    m_spatialGpuTimingQueryFailureLogged = false;
    m_page0 = nullptr;
    m_page1 = nullptr;
    m_page0Metadata.Invalidate();
    m_page1Metadata.Invalidate();
    m_currentPageIndex = 0u;
    m_historyPageIndex = 1u;
    m_observedHistoryEpoch = 0;
    m_lastPublishedFrameSerial = 0;
    m_pageWidth = 0;
    m_pageHeight = 0;
    m_pageBytes = 0;
    m_page0NeedsAllocationClear = false;
    m_diagnosticCounters = nullptr;
    m_diagnosticReadback = nullptr;
    m_diagnosticReadbackPending = false;
    m_diagnosticReadbackDelayFrames = 0;
    m_diagnosticReadbackSampleIndex = 0;
    m_diagnosticReadbackWidth = 0;
    m_diagnosticReadbackHeight = 0;
    m_diagnosticReadbackMaterialPolicyFlags = 0;
    m_diagnosticProbeFromHistory = false;
    m_diagnosticProbeFrameSerial = 0;
    m_diagnosticReadbackFamily = PathTraceUnifiedPtFamily::DirectOnly;
    m_temporalDiagnosticCounters = nullptr;
    m_temporalDiagnosticReadback = nullptr;
    m_temporalDiagnosticReadbackPending = false;
    m_temporalDiagnosticReadbackIsRoute = false;
    m_temporalDiagnosticReadbackDelayFrames = 0;
    m_pipelineVariant = 0;
    m_selectionValid = false;
    m_diagnostics = false;
    m_primaryReceiverMode = 0;
    m_compactGeometry = false;
    m_compactLights = false;
    m_compactMaterials = false;
    m_splitInitial = false;
    m_threeVertexInitial = false;
    m_splitContinuation = false;
    m_directProposalParity = false;
    m_lightTiles = false;
    m_temporalModeActive = false;
    m_initialPublishedThisFrame = false;
    m_spatialModeActive = false;
    m_temporalCompactLights = false;
    m_temporalDuplication = false;
    m_temporalIndirect = false;
    m_temporalReplayCompaction = false;
    m_temporalBoilingFilter = false;
    m_spatialCompactLights = false;
    m_resourceFailureLogged = false;
    m_reportedProofStage = UINT32_MAX;
}

void PathTraceUnifiedPtState::ReleaseResolve()
{
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_resolveBindingSets[page] = nullptr;
        m_resolveBindingSetDescs[page] = nvrhi::BindingSetDesc();
        m_resolveBindingSetDescValid[page] = false;
    }
    m_resolvePipeline = nullptr;
    m_resolveShader = nullptr;
    m_resolveBindingLayout = nullptr;
    m_resolveOutput = nullptr;
    m_presentationOutput = nullptr;
    m_resolveWidth = 0;
    m_resolveHeight = 0;
    m_resolvePrimaryReceiverMode = UINT32_MAX;
    m_resolvePipelineAttempted = false;
    m_resolveFailureLogged = false;
    m_resolveReady = false;
}

nvrhi::BufferHandle PathTraceUnifiedPtState::CurrentPage() const
{
    return m_currentPageIndex == 0u ? m_page0 : m_page1;
}

nvrhi::BufferHandle PathTraceUnifiedPtState::HistoryPage() const
{
    return m_historyPageIndex == 0u ? m_page0 : m_page1;
}

PathTraceUnifiedPtPageMetadata& PathTraceUnifiedPtState::CurrentPageMetadata()
{
    return m_currentPageIndex == 0u ? m_page0Metadata : m_page1Metadata;
}

const PathTraceUnifiedPtPageMetadata& PathTraceUnifiedPtState::CurrentPageMetadata() const
{
    return m_currentPageIndex == 0u ? m_page0Metadata : m_page1Metadata;
}

PathTraceUnifiedPtPageMetadata& PathTraceUnifiedPtState::HistoryPageMetadata()
{
    return m_historyPageIndex == 0u ? m_page0Metadata : m_page1Metadata;
}

const PathTraceUnifiedPtPageMetadata& PathTraceUnifiedPtState::HistoryPageMetadata() const
{
    return m_historyPageIndex == 0u ? m_page0Metadata : m_page1Metadata;
}

void PathTraceUnifiedPtState::ReportProofStage(
    uint32_t stage,
    const char* label,
    PathTraceUnifiedPtBackend backend,
    PathTraceUnifiedPtFamily family)
{
    if (m_reportedProofStage == stage)
    {
        return;
    }
    common->Printf(
        "PathTraceUnifiedPt: proofStage=%u reached=%s backend=%s family=%s\n",
        stage,
        label,
        Upt04BackendName(backend),
        Upt04FamilyName(family));
    m_reportedProofStage = stage;
}

bool PathTraceUnifiedPtState::EnsurePages(const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const uint64_t count = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (count == 0 || count > std::numeric_limits<uint32_t>::max() ||
        count > std::numeric_limits<uint64_t>::max() / UPT04_RESERVOIR_STRIDE)
    {
        return false;
    }
    const uint64_t bytes = count * UPT04_RESERVOIR_STRIDE;
    if (m_page0 && m_page1 && m_pageWidth == inputs.width && m_pageHeight == inputs.height &&
        m_page0->getDesc().structStride == UPT04_RESERVOIR_STRIDE &&
        m_page0->getDesc().byteSize >= bytes &&
        m_page1->getDesc().structStride == UPT04_RESERVOIR_STRIDE &&
        m_page1->getDesc().byteSize >= bytes)
    {
        return true;
    }

    // UPT-08 resources are extent-coupled and their per-page metadata follows
    // the physical reservoir roles. Reallocate them lazily only if the proof
    // cvar remains active; disabled means no retained correlation resources.
    ReleaseDuplication();

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtReservoirPage0";
    desc.byteSize = bytes;
    desc.structStride = UPT04_RESERVOIR_STRIDE;
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    nvrhi::BufferHandle page0 = inputs.device->createBuffer(desc);
    desc.debugName = "PathTraceUnifiedPtReservoirPage1";
    nvrhi::BufferHandle page1 = inputs.device->createBuffer(desc);
    if (!page0 || !page1)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate two-page history set (%ux%u, %llu bytes/page)\n",
            inputs.width,
            inputs.height,
            static_cast<unsigned long long>(bytes));
        return false;
    }

    m_page0 = page0;
    m_page1 = page1;
    m_page0Metadata.Invalidate();
    m_page1Metadata.Invalidate();
    m_currentPageIndex = 0u;
    m_historyPageIndex = 1u;
    m_pageWidth = inputs.width;
    m_pageHeight = inputs.height;
    m_pageBytes = bytes;
    // Allocation-only deterministic initialization protects partial proof
    // stages. Production D0 fully overwrites the page, including canonical
    // empty records. Never set this for camera cuts or history invalidation;
    // future reuse invalidates small per-page metadata instead.
    m_page0NeedsAllocationClear = true;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescValid[page] = false;
        m_temporalBindingSets[page] = nullptr;
        m_temporalBindingSetDescValid[page] = false;
        m_temporalReplayBindingSets[page] = nullptr;
        m_temporalReplayBindingSetDescValid[page] = false;
        m_temporalBoilingFilterBindingSets[page] = nullptr;
        m_spatialBindingSets[page] = nullptr;
        m_spatialBindingSetDescValid[page] = false;
        m_resolveBindingSets[page] = nullptr;
        m_resolveBindingSetDescValid[page] = false;
    }
    common->Printf(
        "PathTraceUnifiedPt: allocated pages 0+1 %ux%u records/page=%llu bytes/page=%llu totalBytes=%llu stride=%u page1Clear=never\n",
        inputs.width,
        inputs.height,
        static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(bytes),
        static_cast<unsigned long long>(bytes * 2ull),
        UPT04_RESERVOIR_STRIDE);
    return true;
}

bool PathTraceUnifiedPtState::EnsurePipeline(const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const uint32_t pipelineVariant = Upt04PipelineVariant(inputs);
    const bool liveTlasProbe = pipelineVariant != 0u;
    const bool compactLiveTlasProbe = Upt04UsesCompactProbeLayout(pipelineVariant);
    const bool minimalProductionSlotLayout =
        Upt04UsesMinimalProductionSlotLayout(pipelineVariant);
    const bool traversalIsolationLayout =
        Upt04UsesTraversalIsolationLayout(pipelineVariant);
    const bool usesPushConstants = Upt04UsesPushConstants(pipelineVariant);
    const bool directOnlyProduction =
        Upt04UsesDirectOnlyProductionLayout(inputs);
    const bool usesBindlessSet = Upt04UsesBindlessSet(pipelineVariant);
    if (!m_selectionValid || m_backend != inputs.backend || m_family != inputs.family ||
        m_pipelineVariant != pipelineVariant || m_diagnostics != inputs.diagnostics ||
        m_primaryReceiverMode != inputs.primaryReceiverMode ||
        m_compactGeometry != inputs.compactGeometry ||
        m_compactLights != inputs.compactLights ||
        m_compactMaterials != inputs.compactMaterials ||
        m_splitInitial != inputs.splitInitial ||
        m_threeVertexInitial != inputs.threeVertexInitial ||
        m_splitContinuation != inputs.splitContinuation ||
        m_lambertDiagnostic != inputs.lambertDiagnostic ||
        m_frozenStaticDiagnostic != inputs.frozenStaticDiagnostic ||
        m_frozenLightDiagnostic != inputs.frozenLightDiagnostic ||
        m_directProposalParity != inputs.directProposalParity ||
        m_lightTiles != inputs.lightTiles)
    {
        ReleasePipeline();
        m_backend = inputs.backend;
        m_family = inputs.family;
        m_pipelineVariant = pipelineVariant;
        m_diagnostics = inputs.diagnostics;
        m_primaryReceiverMode = inputs.primaryReceiverMode;
        m_compactGeometry = inputs.compactGeometry;
        m_compactLights = inputs.compactLights;
        m_compactMaterials = inputs.compactMaterials;
        m_splitInitial = inputs.splitInitial;
        m_threeVertexInitial = inputs.threeVertexInitial;
        m_splitContinuation = inputs.splitContinuation;
        m_lambertDiagnostic = inputs.lambertDiagnostic;
        m_frozenStaticDiagnostic = inputs.frozenStaticDiagnostic;
        m_frozenLightDiagnostic = inputs.frozenLightDiagnostic;
        m_directProposalParity = inputs.directProposalParity;
        m_lightTiles = inputs.lightTiles;
        m_selectionValid = true;
        m_resourceFailureLogged = false;
    }

    if (((liveTlasProbe || m_backend == PathTraceUnifiedPtBackend::RayQuery) &&
            m_computePipeline &&
            (!inputs.splitInitial || m_splitIndirectComputePipeline)) ||
        (m_backend == PathTraceUnifiedPtBackend::RayGeneration && m_shaderTable))
    {
        return true;
    }
    if (m_pipelineAttempted)
    {
        return false;
    }
    m_pipelineAttempted = true;

    if (inputs.diagnostics && inputs.backend != PathTraceUnifiedPtBackend::RayQuery)
    {
        common->Printf(
            "PathTraceUnifiedPt: diagnostics require the RayQuery backend; dispatch skipped\n");
        return false;
    }

    if ((liveTlasProbe || m_backend == PathTraceUnifiedPtBackend::RayQuery) &&
        !inputs.device->queryFeatureSupport(nvrhi::Feature::RayQuery))
    {
        common->Printf("PathTraceUnifiedPt: RayQuery backend requested but unsupported\n");
        return false;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = liveTlasProbe || m_backend == PathTraceUnifiedPtBackend::RayQuery
        ? nvrhi::ShaderType::Compute
        : nvrhi::ShaderType::AllRayTracing;
    // UPT Slang shaders declare explicit Vulkan descriptor sets. Keep NVRHI's
    // host-side set mapping explicit as well; this is the same contract used by
    // the standalone UPT-00 RayQuery harness.
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    // Keep NVRHI's default constant-buffer offset (256). Its old Vulkan
    // backend represents PushConstants as a zero-count descriptor-layout item
    // for binding-index bookkeeping. Flattening b0 to Vulkan binding 0 would
    // collide with the TLAS at t0 and make traversal consume an undefined AS
    // descriptor even though push constants are not shader descriptors.
    if (compactLiveTlasProbe)
    {
        layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
    }
    else if (traversalIsolationLayout)
    {
        layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
            0, UPT04_PUSH_CONSTANT_BYTES));
    }
    else if (minimalProductionSlotLayout)
    {
        layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4));
        if (usesPushConstants)
        {
            layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
                0, UPT04_PUSH_CONSTANT_BYTES));
        }
    }
    else if (directOnlyProduction)
    {
        Upt04AddDirectBindingLayoutItems(layoutDesc);
    }
    else
    {
        Upt04AddBindingLayoutItems(
            layoutDesc,
            inputs.diagnostics,
            inputs.splitContinuation,
            inputs.lightTiles);
    }
    m_bindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_bindingLayout)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create %s binding layout\n",
            compactLiveTlasProbe
                ? "compact live-TLAS probe"
                : (traversalIsolationLayout
                    ? "full-frame traversal-isolation"
                    : (minimalProductionSlotLayout
                    ? "minimal production-slot probe"
                    : (directOnlyProduction
                        ? "direct-only production"
                        : "24-descriptor"))));
        return false;
    }

    const char* initialPath = inputs.diagnostics
        ? Upt04DiagnosticShaderPath()
        : (liveTlasProbe
        ? Upt04LiveTlasProbePath(pipelineVariant)
        : (inputs.frozenStaticDiagnostic || inputs.frozenLightDiagnostic
        ? Upt04FrozenInitialShaderPath(
            inputs.lambertDiagnostic,
            inputs.frozenStaticDiagnostic,
            inputs.frozenLightDiagnostic)
        : (inputs.lambertDiagnostic
        ? Upt04LambertInitialShaderPath(
            inputs.compactGeometry, inputs.compactLights,
            inputs.compactMaterials, inputs.splitContinuation)
        : (inputs.splitInitial
        ? Upt04SplitDirectShaderPath(
            inputs.compactGeometry, inputs.compactLights, inputs.lightTiles)
        : Upt04InitialShaderPath(
            m_backend,
            m_family,
            inputs.primaryReceiverMode,
            inputs.compactGeometry,
            inputs.compactLights,
            inputs.compactMaterials,
            inputs.splitContinuation,
            inputs.directProposalParity)))));
    void* initialData = nullptr;
    int initialSize = 0;
    ID_TIME_T initialTimestamp = 0;
    uint64_t initialHash = 0;
    if (!Upt04ReadShader(initialPath, initialData, initialSize, initialTimestamp, initialHash))
    {
        return false;
    }

    if (liveTlasProbe || m_backend == PathTraceUnifiedPtBackend::RayQuery)
    {
        nvrhi::ShaderDesc shaderDesc;
        shaderDesc.shaderType = nvrhi::ShaderType::Compute;
        shaderDesc.entryName = "main";
        shaderDesc.debugName = liveTlasProbe
            ? Upt04LiveTlasProbeDebugName(pipelineVariant)
            : (inputs.diagnostics
                ? "PathTraceUnifiedPtInitialDiagnostics"
                : "PathTraceUnifiedPtInitialRayQuery");
        m_computeShader = inputs.device->createShader(shaderDesc, initialData, initialSize);
        Mem_Free(initialData);
        if (!m_computeShader)
        {
            common->Printf("PathTraceUnifiedPt: failed to create RayQuery shader\n");
            return false;
        }

        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.CS = m_computeShader;
        pipelineDesc.bindingLayouts = { m_bindingLayout };
        if (usesBindlessSet)
        {
            pipelineDesc.bindingLayouts.push_back(
                inputs.sceneInputs->materials.textureBindlessLayout);
        }
        const uint64_t pipelineStartUs = Sys_Microseconds();
        m_computePipeline = inputs.device->createComputePipeline(pipelineDesc);
        const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
        if (!m_computePipeline)
        {
            common->Printf("PathTraceUnifiedPt: failed to create RayQuery pipeline\n");
            return false;
        }
        int splitIndirectSize = 0;
        ID_TIME_T splitIndirectTimestamp = 0;
        uint64_t splitIndirectHash = 0;
        uint64_t splitIndirectPipelineUs = 0;
        if (inputs.splitInitial)
        {
            void* splitIndirectData = nullptr;
            if (!Upt04ReadShader(
                    Upt04SplitIndirectShaderPath(
                        inputs.compactGeometry,
                        inputs.compactLights,
                        inputs.compactMaterials,
                        inputs.lightTiles,
                        inputs.threeVertexInitial),
                    splitIndirectData,
                    splitIndirectSize,
                    splitIndirectTimestamp,
                    splitIndirectHash))
            {
                return false;
            }
            nvrhi::ShaderDesc splitShaderDesc;
            splitShaderDesc.shaderType = nvrhi::ShaderType::Compute;
            splitShaderDesc.entryName = "main";
            splitShaderDesc.debugName = "PathTraceUnifiedPtSplitIndirectRayQuery";
            m_splitIndirectComputeShader = inputs.device->createShader(
                splitShaderDesc,
                splitIndirectData,
                splitIndirectSize);
            Mem_Free(splitIndirectData);
            if (!m_splitIndirectComputeShader)
            {
                common->Printf(
                    "PathTraceUnifiedPt: failed to create split indirect RayQuery shader\n");
                return false;
            }
            pipelineDesc.CS = m_splitIndirectComputeShader;
            const uint64_t splitPipelineStartUs = Sys_Microseconds();
            m_splitIndirectComputePipeline =
                inputs.device->createComputePipeline(pipelineDesc);
            splitIndirectPipelineUs = Sys_Microseconds() - splitPipelineStartUs;
            if (!m_splitIndirectComputePipeline)
            {
                common->Printf(
                    "PathTraceUnifiedPt: failed to create split indirect RayQuery pipeline\n");
                return false;
            }
        }
        common->Printf(
            "PathTraceUnifiedPt: pipeline backend=%s family=%s variant=%u compiler=%s blobBytes=%d hash=%016llx timestamp=%lld groups=%s bindlessSet=%d receiver=%s geometry=%s lights=%s materials=%s shading=%s split=%s continuation=%s lightTiles=%u payload=0 createUs=%llu deferredHost=0 driverCache=opaque\n",
            Upt04BackendName(m_backend),
            Upt04FamilyName(m_family),
            pipelineVariant,
            pipelineVariant == 8u ? "dxc" : "slang",
            initialSize,
            static_cast<unsigned long long>(initialHash),
            static_cast<long long>(initialTimestamp),
            traversalIsolationLayout || !liveTlasProbe ? "8x8" : "1x1",
            usesBindlessSet ? 1 : 0,
            Upt04ReceiverName(inputs.primaryReceiverMode),
            inputs.compactGeometry ? "compact48" : "legacy112",
            inputs.compactLights ? "compact64" : "legacy112",
            inputs.compactMaterials ? "compact48" : "legacy112",
            (inputs.frozenStaticDiagnostic || inputs.frozenLightDiagnostic)
                ? (inputs.lambertDiagnostic
                    ? "frozen-factor-lambert"
                    : "frozen-factor-openpbr")
                : (inputs.lambertDiagnostic
                    ? "lambert-diagnostic" : "openpbr"),
            inputs.splitInitial ? "direct+indirect" : "monolithic",
            inputs.splitContinuation ? "split-hit32" : "inline",
            inputs.lightTiles ? 1u : 0u,
            static_cast<unsigned long long>(pipelineUs));
        if (inputs.splitInitial)
        {
            common->Printf(
                "PathTraceUnifiedPt: split indirect compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=8x8 intermediate=page0x64 exactM=1 threeVertex=%d continuationRaysMax=%u rouletteQ=%.2f createUs=%llu\n",
                splitIndirectSize,
                static_cast<unsigned long long>(splitIndirectHash),
                static_cast<long long>(splitIndirectTimestamp),
                inputs.threeVertexInitial ? 1 : 0,
                inputs.threeVertexInitial ? 2u : 1u,
                inputs.threeVertexInitial
                    ? Upt04ThreeVertexContinueProbability() : 1.0f,
                static_cast<unsigned long long>(splitIndirectPipelineUs));
        }
        return true;
    }

    m_rayGenerationLibrary = inputs.device->createShaderLibrary(initialData, initialSize);
    Mem_Free(initialData);
    if (!m_rayGenerationLibrary)
    {
        common->Printf("PathTraceUnifiedPt: failed to create raygen shader library\n");
        return false;
    }

    const char* missPath =
        "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_miss.bin";
    const char* closestHitPath =
        "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_closest_hit.bin";
    void* missData = nullptr;
    int missSize = 0;
    ID_TIME_T missTimestamp = 0;
    uint64_t missHash = 0;
    if (!Upt04ReadShader(missPath, missData, missSize, missTimestamp, missHash))
    {
        return false;
    }
    m_missLibrary = inputs.device->createShaderLibrary(missData, missSize);
    Mem_Free(missData);
    if (!m_missLibrary)
    {
        common->Printf("PathTraceUnifiedPt: failed to create miss shader library\n");
        return false;
    }

    void* closestHitData = nullptr;
    int closestHitSize = 0;
    ID_TIME_T closestHitTimestamp = 0;
    uint64_t closestHitHash = 0;
    if (!Upt04ReadShader(
            closestHitPath,
            closestHitData,
            closestHitSize,
            closestHitTimestamp,
            closestHitHash))
    {
        return false;
    }
    m_closestHitLibrary = inputs.device->createShaderLibrary(closestHitData, closestHitSize);
    Mem_Free(closestHitData);
    if (!m_closestHitLibrary)
    {
        common->Printf("PathTraceUnifiedPt: failed to create closest-hit shader library\n");
        return false;
    }

    nvrhi::ShaderHandle rayGeneration =
        m_rayGenerationLibrary->getShader("main", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle miss =
        m_missLibrary->getShader("main", nvrhi::ShaderType::Miss);
    nvrhi::ShaderHandle closestHit =
        m_closestHitLibrary->getShader("main", nvrhi::ShaderType::ClosestHit);
    if (!rayGeneration || !miss || !closestHit)
    {
        common->Printf("PathTraceUnifiedPt: Slang raygen libraries are missing main entry points\n");
        return false;
    }

    nvrhi::rt::PipelineDesc pipelineDesc;
    pipelineDesc.globalBindingLayouts = { m_bindingLayout };
    if (usesBindlessSet)
    {
        pipelineDesc.globalBindingLayouts.push_back(
            inputs.sceneInputs->materials.textureBindlessLayout);
    }
    pipelineDesc.shaders = {
        { "Upt04RayGen", rayGeneration, nullptr },
        { "Upt04Miss", miss, nullptr }
    };
    // The live TLAS contributes SBT record 0 for legacy/static/rigid geometry
    // and record 2 for skinned geometry. Record 1 is retained only so record 2
    // is addressable; all three use the same trace-only closest-hit decoder.
    pipelineDesc.hitGroups = {
        { "Upt04HitGroupLegacy", closestHit, nullptr, nullptr, nullptr, false },
        { "Upt04HitGroupUnusedShadow", closestHit, nullptr, nullptr, nullptr, false },
        { "Upt04HitGroupSkinned", closestHit, nullptr, nullptr, nullptr, false }
    };
    pipelineDesc.maxPayloadSize = 32;
    pipelineDesc.maxAttributeSize = 8;
    pipelineDesc.maxRecursionDepth = 1;
    pipelineDesc.useDeferredHostOperations = false;
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_rayPipeline = inputs.device->createRayTracingPipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_rayPipeline)
    {
        common->Printf("PathTraceUnifiedPt: failed to create raygen pipeline\n");
        return false;
    }
    m_shaderTable = m_rayPipeline->createShaderTable();
    if (!m_shaderTable)
    {
        common->Printf("PathTraceUnifiedPt: failed to create raygen shader table\n");
        m_rayPipeline = nullptr;
        return false;
    }
    m_shaderTable->setRayGenerationShader("Upt04RayGen");
    m_shaderTable->addMissShader("Upt04Miss");
    m_shaderTable->addHitGroup("Upt04HitGroupLegacy");
    m_shaderTable->addHitGroup("Upt04HitGroupUnusedShadow");
    m_shaderTable->addHitGroup("Upt04HitGroupSkinned");

    common->Printf(
        "PathTraceUnifiedPt: pipeline backend=%s family=%s receiver=%s raygenBytes=%d raygenHash=%016llx missBytes=%d missHash=%016llx hitBytes=%d hitHash=%016llx sbtHitGroups=3 payload=32 attribute=8 recursion=1 createUs=%llu deferredHost=0 driverCache=opaque\n",
        Upt04BackendName(m_backend),
        Upt04FamilyName(m_family),
        Upt04ReceiverName(inputs.primaryReceiverMode),
        initialSize,
        static_cast<unsigned long long>(initialHash),
        missSize,
        static_cast<unsigned long long>(missHash),
        closestHitSize,
        static_cast<unsigned long long>(closestHitHash),
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const uint32_t pageIndex = m_currentPageIndex;
    const nvrhi::BufferHandle currentPage = CurrentPage();
    const bool compactLiveTlasProbe =
        Upt04UsesCompactProbeLayout(Upt04PipelineVariant(inputs));
    const bool minimalProductionSlotLayout =
        Upt04UsesMinimalProductionSlotLayout(Upt04PipelineVariant(inputs));
    const bool traversalIsolationLayout =
        Upt04UsesTraversalIsolationLayout(Upt04PipelineVariant(inputs));
    const bool usesPushConstants =
        Upt04UsesPushConstants(Upt04PipelineVariant(inputs));
    const bool directOnlyProduction =
        Upt04UsesDirectOnlyProductionLayout(inputs);
    nvrhi::BindingSetDesc desc;
    if (compactLiveTlasProbe)
    {
        desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
            0, inputs.sceneInputs->geometry.tlas));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, currentPage));
    }
    else if (traversalIsolationLayout)
    {
        desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
            0, inputs.sceneInputs->geometry.tlas));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            1, inputs.primarySurfaceBuffer));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, currentPage));
        desc.addItem(nvrhi::BindingSetItem::PushConstants(
            0, UPT04_PUSH_CONSTANT_BYTES));
    }
    else if (minimalProductionSlotLayout)
    {
        desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
            0, inputs.sceneInputs->geometry.tlas));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, currentPage));
        if (usesPushConstants)
        {
            desc.addItem(nvrhi::BindingSetItem::PushConstants(
                0, UPT04_PUSH_CONSTANT_BYTES));
        }
    }
    else if (directOnlyProduction)
    {
        desc = Upt04BuildDirectBindingSetDesc(
            inputs,
            currentPage,
            HistoryPage(),
            m_compactStaticVertices,
            m_compactDynamicVertices,
            m_compactRigidVertices,
            m_compactSkinnedVertices,
            m_compactLightsBuffer,
            m_compactMaterialsBuffer);
    }
    else
    {
        desc = Upt04BuildBindingSetDesc(
            inputs,
            currentPage,
            HistoryPage(),
            m_diagnosticCounters,
            m_compactStaticVertices,
            m_compactDynamicVertices,
            m_compactRigidVertices,
            m_compactSkinnedVertices,
            m_compactLightsBuffer,
            m_compactMaterialsBuffer,
            m_continuationHits,
            m_lightTileBuffer);
    }
    if (m_bindingSets[pageIndex] && m_bindingSetDescValid[pageIndex] &&
        m_bindingSetDescs[pageIndex] == desc)
    {
        return true;
    }
    m_bindingSets[pageIndex] = inputs.device->createBindingSet(desc, m_bindingLayout);
    if (!m_bindingSets[pageIndex])
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create %s set-0 binding set\n",
            compactLiveTlasProbe
                ? "compact live-TLAS probe"
                : (traversalIsolationLayout
                    ? "full-frame traversal-isolation"
                    : (minimalProductionSlotLayout
                    ? "minimal production-slot probe"
                    : (directOnlyProduction
                        ? "direct-only production"
                        : "UPT-04"))));
        return false;
    }
    m_bindingSetDescs[pageIndex] = desc;
    m_bindingSetDescValid[pageIndex] = true;
    return true;
}

bool PathTraceUnifiedPtState::EnsureDiagnosticBuffers(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.diagnostics)
    {
        return true;
    }
    if (!m_diagnosticCounters)
    {
        nvrhi::BufferDesc desc;
        desc.debugName = "PathTraceUnifiedPtDiagnosticCounters";
        desc.byteSize = UPT04_DIAGNOSTIC_BYTES;
        desc.structStride = sizeof(uint32_t);
        desc.canHaveUAVs = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;
        m_diagnosticCounters = inputs.device->createBuffer(desc);
    }
    if (!m_diagnosticReadback)
    {
        nvrhi::BufferDesc desc;
        desc.debugName = "PathTraceUnifiedPtDiagnosticReadback";
        desc.byteSize = UPT04_DIAGNOSTIC_BYTES;
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        m_diagnosticReadback = inputs.device->createBuffer(desc);
    }
    if (!m_diagnosticCounters || !m_diagnosticReadback)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate diagnostic counter/readback buffers\n");
        return false;
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactGeometryResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactGeometry)
    {
        return true;
    }

    const RtPathTraceSceneInputGeometry& geometry = inputs.sceneInputs->geometry;
    const uint32_t requestedCounts[4] = {
        static_cast<uint32_t>(Max(0, geometry.staticVertexCount)),
        static_cast<uint32_t>(Max(0, geometry.dynamicVertexCount)),
        static_cast<uint32_t>(Max(0, geometry.rigidRouteVertexCount)),
        static_cast<uint32_t>(Max(0, geometry.skinnedGpuComputeVertexCount))
    };
    const char* debugNames[4] = {
        "PathTraceUnifiedPtCompactStaticVertices",
        "PathTraceUnifiedPtCompactDynamicVertices",
        "PathTraceUnifiedPtCompactRigidVertices",
        "PathTraceUnifiedPtCompactSkinnedVertices"
    };

    bool replaced = false;
    const auto ensureRouteBuffer = [&inputs, &replaced](
        nvrhi::BufferHandle& buffer,
        uint32_t& storedCapacity,
        uint32_t requestedCount,
        uint32_t route,
        const char* debugName)
    {
        const uint32_t capacity = Max(1u, requestedCount);
        const uint64_t bytes = uint64_t(capacity) * UPT04_COMPACT_VERTEX_STRIDE;
        if (buffer &&
            buffer->getDesc().structStride == UPT04_COMPACT_VERTEX_STRIDE &&
            buffer->getDesc().byteSize >= bytes)
        {
            return true;
        }
        nvrhi::BufferDesc desc;
        desc.debugName = debugName;
        desc.byteSize = bytes;
        desc.structStride = UPT04_COMPACT_VERTEX_STRIDE;
        desc.canHaveUAVs = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;
        buffer = inputs.device->createBuffer(desc);
        if (!buffer)
        {
            common->Printf(
                "PathTraceUnifiedPt: failed to allocate compact geometry route %u count=%u bytes=%llu\n",
                route,
                requestedCount,
                static_cast<unsigned long long>(bytes));
            return false;
        }
        storedCapacity = capacity;
        replaced = true;
        return true;
    };
    if (!ensureRouteBuffer(
            m_compactStaticVertices,
            m_compactStaticVertexCapacity,
            requestedCounts[0],
            0u,
            debugNames[0]) ||
        !ensureRouteBuffer(
            m_compactDynamicVertices,
            m_compactDynamicVertexCapacity,
            requestedCounts[1],
            1u,
            debugNames[1]) ||
        !ensureRouteBuffer(
            m_compactRigidVertices,
            m_compactRigidVertexCapacity,
            requestedCounts[2],
            2u,
            debugNames[2]) ||
        !ensureRouteBuffer(
            m_compactSkinnedVertices,
            m_compactSkinnedVertexCapacity,
            requestedCounts[3],
            3u,
            debugNames[3]))
    {
        return false;
    }
    if (replaced)
    {
        m_compactGeometryBindingSet = nullptr;
        m_compactGeometryBindingSetDescValid = false;
        for (uint32_t page = 0; page < 2u; ++page)
        {
            m_bindingSets[page] = nullptr;
            m_bindingSetDescValid[page] = false;
        }
        common->Printf(
            "PathTraceUnifiedPt: compact geometry sidecars stride=%u capacities=%u/%u/%u/%u bytes=%llu\n",
            UPT04_COMPACT_VERTEX_STRIDE,
            m_compactStaticVertexCapacity,
            m_compactDynamicVertexCapacity,
            m_compactRigidVertexCapacity,
            m_compactSkinnedVertexCapacity,
            static_cast<unsigned long long>(
                uint64_t(m_compactStaticVertexCapacity +
                    m_compactDynamicVertexCapacity +
                    m_compactRigidVertexCapacity +
                    m_compactSkinnedVertexCapacity) *
                UPT04_COMPACT_VERTEX_STRIDE));
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactGeometryPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactGeometry || m_compactGeometryPipeline)
    {
        return true;
    }
    if (m_compactGeometryPipelineAttempted)
    {
        return false;
    }
    m_compactGeometryPipelineAttempted = true;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    for (uint32_t slot = 0; slot < 4u; ++slot)
    {
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
    }
    for (uint32_t slot = 4; slot < 8u; ++slot)
    {
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(slot));
    }
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT04_COMPACT_GEOMETRY_PUSH_CONSTANT_BYTES));
    m_compactGeometryBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_compactGeometryBindingLayout)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact geometry binding layout\n");
        return false;
    }

    void* shaderData = nullptr;
    int shaderSize = 0;
    ID_TIME_T shaderTimestamp = 0;
    uint64_t shaderHash = 0;
    if (!Upt04ReadShader(
            "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_compact_geometry_pack.bin",
            shaderData,
            shaderSize,
            shaderTimestamp,
            shaderHash))
    {
        return false;
    }
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtCompactGeometryPack";
    m_compactGeometryShader = inputs.device->createShader(
        shaderDesc, shaderData, shaderSize);
    Mem_Free(shaderData);
    if (!m_compactGeometryShader)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact geometry shader\n");
        return false;
    }
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_compactGeometryShader;
    pipelineDesc.bindingLayouts = { m_compactGeometryBindingLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_compactGeometryPipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_compactGeometryPipeline)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact geometry pipeline\n");
        return false;
    }
    common->Printf(
        "PathTraceUnifiedPt: compact geometry pack compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=128x1 stride=48 createUs=%llu\n",
        shaderSize,
        static_cast<unsigned long long>(shaderHash),
        static_cast<long long>(shaderTimestamp),
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactGeometryBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactGeometry)
    {
        return true;
    }
    const RtPathTraceSceneInputs& scene = *inputs.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = scene.geometry;
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, geometry.staticVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, geometry.dynamicVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, geometry.rigidRouteVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, Upt04SkinnedVertexBuffer(scene)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, m_compactStaticVertices));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(5, m_compactDynamicVertices));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(6, m_compactRigidVertices));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(7, m_compactSkinnedVertices));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT04_COMPACT_GEOMETRY_PUSH_CONSTANT_BYTES));
    if (m_compactGeometryBindingSet &&
        m_compactGeometryBindingSetDescValid &&
        m_compactGeometryBindingSetDesc == desc)
    {
        return true;
    }
    m_compactGeometryBindingSet = inputs.device->createBindingSet(
        desc, m_compactGeometryBindingLayout);
    if (!m_compactGeometryBindingSet)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact geometry binding set\n");
        return false;
    }
    m_compactGeometryBindingSetDesc = desc;
    m_compactGeometryBindingSetDescValid = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteCompactGeometryPack(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactGeometry)
    {
        return true;
    }
    const RtPathTraceSceneInputs& scene = *inputs.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = scene.geometry;
    const Upt04CompactGeometryPackControl control = {
        static_cast<uint32_t>(Max(0, geometry.staticVertexCount)),
        static_cast<uint32_t>(Max(0, geometry.dynamicVertexCount)),
        static_cast<uint32_t>(Max(0, geometry.rigidRouteVertexCount)),
        static_cast<uint32_t>(Max(0, geometry.skinnedGpuComputeVertexCount))
    };
    const uint32_t maxVertexCount = Max(
        Max(control.staticVertexCount, control.dynamicVertexCount),
        Max(control.rigidVertexCount, control.skinnedVertexCount));

    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.G0 CompactGeometry48 Bind+Barriers",
            inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            geometry.staticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.dynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.rigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            Upt04SkinnedVertexBuffer(scene), nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_compactStaticVertices, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->setBufferState(
            m_compactDynamicVertices, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->setBufferState(
            m_compactRigidVertices, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->setBufferState(
            m_compactSkinnedVertices, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();

        nvrhi::ComputeState state;
        state.pipeline = m_compactGeometryPipeline;
        state.bindings = { m_compactGeometryBindingSet };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
    }
    if (maxVertexCount > 0u)
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.G0 CompactGeometry48 Dispatch",
            inputs.nsightMarkers);
        inputs.commandList->dispatch((maxVertexCount + 127u) / 128u, 1u, 1u);
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.G0 CompactGeometry48 OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_compactStaticVertices);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_compactDynamicVertices);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_compactRigidVertices);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_compactSkinnedVertices);
        inputs.commandList->setBufferState(
            m_compactStaticVertices, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_compactDynamicVertices, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_compactRigidVertices, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_compactSkinnedVertices, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->commitBarriers();
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactLightResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactLights)
    {
        return true;
    }
    const uint32_t requestedCount = static_cast<uint32_t>(Max(
        0, inputs.sceneInputs->lights.restirLightManagerCurrentPayloadCount));
    const uint32_t capacity = Max(1u, requestedCount);
    const uint64_t bytes = uint64_t(capacity) * UPT04_COMPACT_LIGHT_STRIDE;
    if (m_compactLightsBuffer &&
        m_compactLightsBuffer->getDesc().structStride == UPT04_COMPACT_LIGHT_STRIDE &&
        m_compactLightsBuffer->getDesc().byteSize >= bytes)
    {
        return true;
    }

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtCompactCurrentLights";
    desc.byteSize = bytes;
    desc.structStride = UPT04_COMPACT_LIGHT_STRIDE;
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    m_compactLightsBuffer = inputs.device->createBuffer(desc);
    if (!m_compactLightsBuffer)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate compact lights count=%u bytes=%llu\n",
            requestedCount,
            static_cast<unsigned long long>(bytes));
        return false;
    }
    m_compactLightCapacity = capacity;
    m_compactLightBindingSet = nullptr;
    m_compactLightBindingSetDescValid = false;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescValid[page] = false;
        m_temporalBindingSets[page] = nullptr;
        m_temporalBindingSetDescValid[page] = false;
        m_spatialBindingSets[page] = nullptr;
        m_spatialBindingSetDescValid[page] = false;
    }
    common->Printf(
        "PathTraceUnifiedPt: compact light sidecar stride=%u capacity=%u bytes=%llu\n",
        UPT04_COMPACT_LIGHT_STRIDE,
        capacity,
        static_cast<unsigned long long>(bytes));
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactLightPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactLights || m_compactLightPipeline)
    {
        return true;
    }
    if (m_compactLightPipelineAttempted)
    {
        return false;
    }
    m_compactLightPipelineAttempted = true;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT04_COMPACT_LIGHT_PUSH_CONSTANT_BYTES));
    m_compactLightBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_compactLightBindingLayout)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact light binding layout\n");
        return false;
    }

    void* shaderData = nullptr;
    int shaderSize = 0;
    ID_TIME_T shaderTimestamp = 0;
    uint64_t shaderHash = 0;
    if (!Upt04ReadShader(
            "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_compact_light_pack.bin",
            shaderData,
            shaderSize,
            shaderTimestamp,
            shaderHash))
    {
        return false;
    }
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtCompactLightPack";
    m_compactLightShader = inputs.device->createShader(
        shaderDesc, shaderData, shaderSize);
    Mem_Free(shaderData);
    if (!m_compactLightShader)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact light shader\n");
        return false;
    }
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_compactLightShader;
    pipelineDesc.bindingLayouts = { m_compactLightBindingLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_compactLightPipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_compactLightPipeline)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact light pipeline\n");
        return false;
    }
    common->Printf(
        "PathTraceUnifiedPt: compact light pack compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=128x1 stride=64 createUs=%llu\n",
        shaderSize,
        static_cast<unsigned long long>(shaderHash),
        static_cast<long long>(shaderTimestamp),
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactLightBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactLights)
    {
        return true;
    }
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        0, inputs.sceneInputs->lights.restirLightManagerCurrentPayloadBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
        1, m_compactLightsBuffer));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT04_COMPACT_LIGHT_PUSH_CONSTANT_BYTES));
    if (m_compactLightBindingSet && m_compactLightBindingSetDescValid &&
        m_compactLightBindingSetDesc == desc)
    {
        return true;
    }
    m_compactLightBindingSet = inputs.device->createBindingSet(
        desc, m_compactLightBindingLayout);
    if (!m_compactLightBindingSet)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact light binding set\n");
        return false;
    }
    m_compactLightBindingSetDesc = desc;
    m_compactLightBindingSetDescValid = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteCompactLightPack(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactLights)
    {
        return true;
    }
    const uint32_t lightCount = static_cast<uint32_t>(Max(
        0, inputs.sceneInputs->lights.restirLightManagerCurrentPayloadCount));
    const Upt04CompactLightPackControl control = { lightCount };
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.L0 CompactLight64 Bind+Barriers",
            inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            inputs.sceneInputs->lights.restirLightManagerCurrentPayloadBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_compactLightsBuffer, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_compactLightPipeline;
        state.bindings = { m_compactLightBindingSet };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
    }
    if (lightCount > 0u)
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.L0 CompactLight64 Dispatch",
            inputs.nsightMarkers);
        inputs.commandList->dispatch((lightCount + 127u) / 128u, 1u, 1u);
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.L0 CompactLight64 OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_compactLightsBuffer);
        inputs.commandList->setBufferState(
            m_compactLightsBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->commitBarriers();
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureLightTileResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.lightTiles)
    {
        return true;
    }
    const uint64_t bytes = uint64_t(UPT04_LIGHT_TILE_ENTRY_COUNT) *
        UPT04_LIGHT_TILE_ENTRY_STRIDE;
    if (m_lightTileBuffer &&
        m_lightTileBuffer->getDesc().structStride ==
            UPT04_LIGHT_TILE_ENTRY_STRIDE &&
        m_lightTileBuffer->getDesc().byteSize == bytes)
    {
        return true;
    }

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtLightTiles";
    desc.byteSize = bytes;
    desc.structStride = UPT04_LIGHT_TILE_ENTRY_STRIDE;
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    m_lightTileBuffer = inputs.device->createBuffer(desc);
    if (!m_lightTileBuffer)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate light tiles entries=%u bytes=%llu\n",
            UPT04_LIGHT_TILE_ENTRY_COUNT,
            static_cast<unsigned long long>(bytes));
        return false;
    }
    m_lightTileBindingSet = nullptr;
    m_lightTileBindingSetDescValid = false;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescValid[page] = false;
    }
    common->Printf(
        "PathTraceUnifiedPt: light tiles tiles=%u domains=%u entriesPerDomain=%u stride=%u entries=%u bytes=%llu clear=never\n",
        UPT04_LIGHT_TILE_COUNT,
        UPT04_LIGHT_TILE_DOMAIN_COUNT,
        UPT04_LIGHT_TILE_DOMAIN_SIZE,
        UPT04_LIGHT_TILE_ENTRY_STRIDE,
        UPT04_LIGHT_TILE_ENTRY_COUNT,
        static_cast<unsigned long long>(bytes));
    return true;
}

bool PathTraceUnifiedPtState::EnsureLightTilePipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.lightTiles || m_lightTilePipeline)
    {
        return true;
    }
    if (m_lightTilePipelineAttempted)
    {
        return false;
    }
    m_lightTilePipelineAttempted = true;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT04_LIGHT_TILE_PUSH_CONSTANT_BYTES));
    m_lightTileBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_lightTileBindingLayout)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create light-tile binding layout\n");
        return false;
    }

    void* shaderData = nullptr;
    int shaderSize = 0;
    ID_TIME_T shaderTimestamp = 0;
    uint64_t shaderHash = 0;
    if (!Upt04ReadShader(
            "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_light_tile_presample.bin",
            shaderData,
            shaderSize,
            shaderTimestamp,
            shaderHash))
    {
        return false;
    }
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtLightTilePresample";
    m_lightTileShader = inputs.device->createShader(
        shaderDesc, shaderData, shaderSize);
    Mem_Free(shaderData);
    if (!m_lightTileShader)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create light-tile shader\n");
        return false;
    }
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_lightTileShader;
    pipelineDesc.bindingLayouts = { m_lightTileBindingLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_lightTilePipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_lightTilePipeline)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create light-tile pipeline\n");
        return false;
    }
    common->Printf(
        "PathTraceUnifiedPt: light-tile presample compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=128x1 tiles=%u domains=%u entriesPerDomain=%u createUs=%llu\n",
        shaderSize,
        static_cast<unsigned long long>(shaderHash),
        static_cast<long long>(shaderTimestamp),
        UPT04_LIGHT_TILE_COUNT,
        UPT04_LIGHT_TILE_DOMAIN_COUNT,
        UPT04_LIGHT_TILE_DOMAIN_SIZE,
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureLightTileBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.lightTiles)
    {
        return true;
    }
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        0, inputs.sceneInputs->lights.emissiveDistributionBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
        1, m_lightTileBuffer));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT04_LIGHT_TILE_PUSH_CONSTANT_BYTES));
    if (m_lightTileBindingSet && m_lightTileBindingSetDescValid &&
        m_lightTileBindingSetDesc == desc)
    {
        return true;
    }
    m_lightTileBindingSet = inputs.device->createBindingSet(
        desc, m_lightTileBindingLayout);
    if (!m_lightTileBindingSet)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create light-tile binding set\n");
        return false;
    }
    m_lightTileBindingSetDesc = desc;
    m_lightTileBindingSetDescValid = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteLightTilePresample(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.lightTiles)
    {
        return true;
    }
    const RtPathTraceSceneInputLights& lights = inputs.sceneInputs->lights;
    const uint32_t distributionCount = lights.emissiveDistributionValid
        ? static_cast<uint32_t>(Max(0, lights.emissiveDistributionCount))
        : 0u;
    const uint32_t emissiveRangeCount =
        lights.restirLightManagerEmissiveRangeCount;
    const uint32_t analyticRangeCount =
        lights.restirLightManagerDoomAnalyticSampleableCount;
    const uint32_t configuredEmissiveTrials = static_cast<uint32_t>(
        idMath::ClampInt(1, UPT04_NEE_RIS_MAX_EMISSIVE_CANDIDATE_COUNT,
            r_pathTracingReservoirCandidateTrials.GetInteger()));
    const uint32_t emissiveTrials =
        distributionCount != 0u && emissiveRangeCount != 0u
            ? configuredEmissiveTrials : 0u;
    const uint32_t analyticTrials = Min(
        analyticRangeCount,
        UPT04_NEE_RIS_PARITY_ANALYTIC_CANDIDATE_COUNT);
    Upt04LightTilePresampleControl control = {};
    control.tileCount = UPT04_LIGHT_TILE_COUNT;
    control.tileDomainSize = UPT04_LIGHT_TILE_DOMAIN_SIZE;
    control.frameSampleIndex = inputs.frameSampleIndex;
    control.emissiveDistributionCount = distributionCount;
    control.emissiveRangeStart =
        lights.restirLightManagerEmissiveRangeOffset;
    control.emissiveRangeCount = emissiveRangeCount;
    control.analyticRangeStart =
        lights.restirLightManagerDoomAnalyticRangeOffset;
    control.analyticRangeCount = analyticRangeCount;
    control.emissiveTrials = emissiveTrials;
    control.analyticTrials = analyticTrials;
    control.candidateCount = emissiveTrials + analyticTrials;

    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.LT0 LightTiles Presample",
            inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            lights.emissiveDistributionBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_lightTileBuffer,
            nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_lightTilePipeline;
        state.bindings = { m_lightTileBindingSet };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
        const uint32_t entriesPerTile = UPT04_LIGHT_TILE_DOMAIN_SIZE *
            UPT04_LIGHT_TILE_DOMAIN_COUNT;
        inputs.commandList->dispatch(
            (entriesPerTile + 127u) / 128u,
            UPT04_LIGHT_TILE_COUNT,
            1u);
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.LT0 LightTiles OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_lightTileBuffer);
        inputs.commandList->setBufferState(
            m_lightTileBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->commitBarriers();
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactMaterialResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactMaterials)
    {
        return true;
    }
    const uint32_t requestedCount = static_cast<uint32_t>(Max(
        0, inputs.sceneInputs->materials.materialTableEntryCount));
    const uint32_t capacity = Max(1u, requestedCount);
    const uint64_t bytes = uint64_t(capacity) * UPT04_COMPACT_MATERIAL_STRIDE;
    if (m_compactMaterialsBuffer &&
        m_compactMaterialsBuffer->getDesc().structStride == UPT04_COMPACT_MATERIAL_STRIDE &&
        m_compactMaterialsBuffer->getDesc().byteSize >= bytes)
    {
        return true;
    }

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtCompactMaterials";
    desc.byteSize = bytes;
    desc.structStride = UPT04_COMPACT_MATERIAL_STRIDE;
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    m_compactMaterialsBuffer = inputs.device->createBuffer(desc);
    if (!m_compactMaterialsBuffer)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate compact materials count=%u bytes=%llu\n",
            requestedCount,
            static_cast<unsigned long long>(bytes));
        return false;
    }
    m_compactMaterialCapacity = capacity;
    m_compactMaterialBindingSet = nullptr;
    m_compactMaterialBindingSetDescValid = false;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescValid[page] = false;
    }
    common->Printf(
        "PathTraceUnifiedPt: compact material sidecar stride=%u capacity=%u bytes=%llu\n",
        UPT04_COMPACT_MATERIAL_STRIDE,
        capacity,
        static_cast<unsigned long long>(bytes));
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactMaterialPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactMaterials || m_compactMaterialPipeline)
    {
        return true;
    }
    if (m_compactMaterialPipelineAttempted)
    {
        return false;
    }
    m_compactMaterialPipelineAttempted = true;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT04_COMPACT_MATERIAL_PUSH_CONSTANT_BYTES));
    m_compactMaterialBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_compactMaterialBindingLayout)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact material binding layout\n");
        return false;
    }

    void* shaderData = nullptr;
    int shaderSize = 0;
    ID_TIME_T shaderTimestamp = 0;
    uint64_t shaderHash = 0;
    if (!Upt04ReadShader(
            "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_compact_material_pack.bin",
            shaderData,
            shaderSize,
            shaderTimestamp,
            shaderHash))
    {
        return false;
    }
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtCompactMaterialPack";
    m_compactMaterialShader = inputs.device->createShader(
        shaderDesc, shaderData, shaderSize);
    Mem_Free(shaderData);
    if (!m_compactMaterialShader)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact material shader\n");
        return false;
    }
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_compactMaterialShader;
    pipelineDesc.bindingLayouts = { m_compactMaterialBindingLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_compactMaterialPipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_compactMaterialPipeline)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact material pipeline\n");
        return false;
    }
    common->Printf(
        "PathTraceUnifiedPt: compact material pack compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=128x1 stride=48 createUs=%llu\n",
        shaderSize,
        static_cast<unsigned long long>(shaderHash),
        static_cast<long long>(shaderTimestamp),
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactMaterialBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactMaterials)
    {
        return true;
    }
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        0, inputs.sceneInputs->materials.materialTableBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
        1, m_compactMaterialsBuffer));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT04_COMPACT_MATERIAL_PUSH_CONSTANT_BYTES));
    if (m_compactMaterialBindingSet && m_compactMaterialBindingSetDescValid &&
        m_compactMaterialBindingSetDesc == desc)
    {
        return true;
    }
    m_compactMaterialBindingSet = inputs.device->createBindingSet(
        desc, m_compactMaterialBindingLayout);
    if (!m_compactMaterialBindingSet)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact material binding set\n");
        return false;
    }
    m_compactMaterialBindingSetDesc = desc;
    m_compactMaterialBindingSetDescValid = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteCompactMaterialPack(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactMaterials)
    {
        return true;
    }
    const uint32_t materialCount = static_cast<uint32_t>(Max(
        0, inputs.sceneInputs->materials.materialTableEntryCount));
    const Upt04CompactMaterialPackControl control = { materialCount };
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.M0 CompactMaterial48 Bind+Barriers",
            inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            inputs.sceneInputs->materials.materialTableBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_compactMaterialsBuffer, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_compactMaterialPipeline;
        state.bindings = { m_compactMaterialBindingSet };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
    }
    if (materialCount > 0u)
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.M0 CompactMaterial48 Dispatch",
            inputs.nsightMarkers);
        inputs.commandList->dispatch((materialCount + 127u) / 128u, 1u, 1u);
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.M0 CompactMaterial48 OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_compactMaterialsBuffer);
        inputs.commandList->setBufferState(
            m_compactMaterialsBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->commitBarriers();
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureContinuationResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.splitContinuation)
    {
        return true;
    }
    const uint64_t requestedCount64 = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (requestedCount64 == 0u || requestedCount64 > UINT32_MAX)
    {
        common->Printf(
            "PathTraceUnifiedPt: invalid continuation-hit capacity=%llu\n",
            static_cast<unsigned long long>(requestedCount64));
        return false;
    }
    const uint32_t capacity = static_cast<uint32_t>(requestedCount64);
    const uint64_t bytes = requestedCount64 * UPT04_CONTINUATION_HIT_STRIDE;
    if (m_continuationHits &&
        m_continuationHits->getDesc().structStride == UPT04_CONTINUATION_HIT_STRIDE &&
        m_continuationHits->getDesc().byteSize >= bytes)
    {
        return true;
    }

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtContinuationHits";
    desc.byteSize = bytes;
    desc.structStride = UPT04_CONTINUATION_HIT_STRIDE;
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    m_continuationHits = inputs.device->createBuffer(desc);
    if (!m_continuationHits)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate continuation hits count=%u bytes=%llu\n",
            capacity,
            static_cast<unsigned long long>(bytes));
        return false;
    }
    m_continuationCapacity = capacity;
    m_continuationBindingSet = nullptr;
    m_continuationBindingSetDescValid = false;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescValid[page] = false;
    }
    common->Printf(
        "PathTraceUnifiedPt: continuation-hit sidecar stride=%u capacity=%u bytes=%llu\n",
        UPT04_CONTINUATION_HIT_STRIDE,
        capacity,
        static_cast<unsigned long long>(bytes));
    return true;
}

bool PathTraceUnifiedPtState::EnsureContinuationPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.splitContinuation || m_continuationPipeline)
    {
        return true;
    }
    if (m_continuationPipelineAttempted)
    {
        return false;
    }
    m_continuationPipelineAttempted = true;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(24));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT04_PUSH_CONSTANT_BYTES));
    m_continuationBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_continuationBindingLayout)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create continuation-trace binding layout\n");
        return false;
    }

    void* shaderData = nullptr;
    int shaderSize = 0;
    ID_TIME_T shaderTimestamp = 0;
    uint64_t shaderHash = 0;
    if (!Upt04ReadShader(
            inputs.lambertDiagnostic
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_continuation_trace_rayquery_compact32_lambert.bin"
                : Upt04ContinuationTraceShaderPath(),
            shaderData,
            shaderSize,
            shaderTimestamp,
            shaderHash))
    {
        return false;
    }
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtContinuationTraceRayQuery";
    m_continuationShader = inputs.device->createShader(
        shaderDesc, shaderData, shaderSize);
    Mem_Free(shaderData);
    if (!m_continuationShader)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create continuation-trace shader\n");
        return false;
    }
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_continuationShader;
    pipelineDesc.bindingLayouts = { m_continuationBindingLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_continuationPipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_continuationPipeline)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create continuation-trace pipeline\n");
        return false;
    }
    common->Printf(
        "PathTraceUnifiedPt: continuation trace compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=8x8 hitStride=32 queries=1 createUs=%llu\n",
        shaderSize,
        static_cast<unsigned long long>(shaderHash),
        static_cast<long long>(shaderTimestamp),
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureContinuationBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.splitContinuation)
    {
        return true;
    }
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
        0, inputs.sceneInputs->geometry.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        1, inputs.primarySurfaceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
        24, m_continuationHits));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT04_PUSH_CONSTANT_BYTES));
    if (m_continuationBindingSet && m_continuationBindingSetDescValid &&
        m_continuationBindingSetDesc == desc)
    {
        return true;
    }
    m_continuationBindingSet = inputs.device->createBindingSet(
        desc, m_continuationBindingLayout);
    if (!m_continuationBindingSet)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create continuation-trace binding set\n");
        return false;
    }
    m_continuationBindingSetDesc = desc;
    m_continuationBindingSetDescValid = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteContinuationTrace(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.splitContinuation)
    {
        return true;
    }
    const Upt04InitialControl control = Upt04BuildControl(inputs);
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.C0 ContinuationTrace32 Bind+Barriers",
            inputs.nsightMarkers);
        inputs.commandList->setAccelStructState(
            inputs.sceneInputs->geometry.tlas,
            nvrhi::ResourceStates::AccelStructRead);
        inputs.commandList->setBufferState(
            inputs.primarySurfaceBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_continuationHits,
            nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_continuationPipeline;
        state.bindings = { m_continuationBindingSet };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
    }
    {
        const bool oneGroup = inputs.proofStage == 7u;
        const bool oneGroupRow = inputs.proofStage == 8u;
        const char* markerName = oneGroup
            ? "UPT.C0 ContinuationTrace32 RayQuery 8x8"
            : (oneGroupRow
                ? "UPT.C0 ContinuationTrace32 RayQuery OneGroupRow"
                : "UPT.C0 ContinuationTrace32 RayQuery FullFrame");
        Upt04MarkerScope marker(
            inputs.commandList, markerName, inputs.nsightMarkers);
        inputs.commandList->dispatch(
            oneGroup ? 1u : (inputs.width + 7u) / 8u,
            oneGroup || oneGroupRow ? 1u : (inputs.height + 7u) / 8u,
            1u);
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.C0 ContinuationTrace32 OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_continuationHits);
        inputs.commandList->setBufferState(
            m_continuationHits,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->commitBarriers();
    }
    return true;
}

void PathTraceUnifiedPtState::DrainDiagnosticReadback(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!m_diagnosticReadbackPending || !m_diagnosticReadback || !inputs.device)
    {
        return;
    }
    if (m_diagnosticReadbackDelayFrames > 0)
    {
        --m_diagnosticReadbackDelayFrames;
        return;
    }
    const uint32_t* counters = static_cast<const uint32_t*>(
        inputs.device->mapBuffer(
            m_diagnosticReadback, nvrhi::CpuAccessMode::Read));
    if (!counters)
    {
        common->Printf("PathTraceUnifiedPt: diagnostic readback map failed\n");
        m_diagnosticReadbackPending = false;
        return;
    }
    common->Printf(
        "PathTraceUnifiedPt: diagnostic receivers(valid/invalid)=%u/%u candidates(direct invalid/zero/positive)=%u/%u/%u candidates(indirect invalid/zero/positive)=%u/%u/%u selected(primaryNee/bsdfEndpoint/secondaryNee)=%u/%u/%u rays(continuation/visibility)=%u/%u canonicalEmpty=%u candidateInputs=%u rayCeilingViolations=%u reservoirSignature=%08x:%08x:%08x:%08x directReject(selection/record/emissiveReplay/analyticSample/material/pdf/other)=%u/%u/%u/%u/%u/%u/%u emissiveTrials(attempts/selection/identity/resolve/material/otherReject)=%u/%u/%u/%u/%u/%u emissiveEval(geometryPositive/geometryZero/endpointPositive/endpointZero/targetPositive/proposalPositive)=%u/%u/%u/%u/%u/%u emissiveSelected(selected/analytic/visible/occluded)=%u/%u/%u/%u finalDirect(emissive/analytic)=%u/%u finalEmissiveLuma(<1e-4/<1e-2/<1/>=1)=%u/%u/%u/%u finalAnalyticLuma(<1e-4/<1e-2/<1/>=1)=%u/%u/%u/%u emissiveOccluder(sameInstanceSameMaterial/sameInstanceOtherMaterial/otherInstanceSameMaterial/otherInstanceOtherMaterial/decodeFailed)=%u/%u/%u/%u/%u previousBest(lookupAttempted/lookupSucceeded/remapFailure/replayFailure/targetFailure/visibilityFailure/duplicateRejected/admitted)=%u/%u/%u/%u/%u/%u/%u/%u freshSelected(analytic/emissive)=%u/%u weightedCdfUnavailable=%u cdf(current/capacity/valid/zeroPdfSkipped)=%d/%llu/%d/%d sampleIndex=%u family=%s size=%ux%u\n",
        counters[0], counters[1], counters[2], counters[3], counters[4],
        counters[5], counters[6], counters[7], counters[8], counters[9],
        counters[10], counters[11], counters[12], counters[13], counters[14],
        counters[15], counters[16], counters[17], counters[18], counters[19],
        counters[20], counters[21], counters[22], counters[23], counters[24],
        counters[25], counters[26], counters[27], counters[28], counters[29],
        counters[30], counters[31], counters[32], counters[33], counters[34],
        counters[35], counters[36], counters[37], counters[38], counters[39],
        counters[40], counters[41], counters[42], counters[43], counters[44],
        counters[45], counters[46], counters[47], counters[48], counters[49],
        counters[50], counters[51], counters[52], counters[53], counters[54],
        counters[55], counters[56], counters[57], counters[58], counters[59],
        counters[60], counters[61], counters[62], counters[63], counters[64],
        counters[65], counters[66], counters[67], counters[68],
        inputs.sceneInputs->lights.emissiveDistributionCount,
        static_cast<unsigned long long>(
            inputs.sceneInputs->lights.emissiveDistributionBuffer
                ? inputs.sceneInputs->lights.emissiveDistributionBuffer->getDesc().byteSize /
                    sizeof(PathTraceEmissiveDistributionEntry)
                : 0ull),
        inputs.sceneInputs->lights.emissiveDistributionValid ? 1 : 0,
        inputs.sceneInputs->lights.emissiveDistributionZeroPdfSkipped,
        m_diagnosticReadbackSampleIndex,
        Upt04FamilyName(m_diagnosticReadbackFamily),
        m_diagnosticReadbackWidth,
        m_diagnosticReadbackHeight);
    common->Printf(
        "PathTraceUnifiedPt: diagnostic indirect continuation(miss/hit)=%u/%u geometryFailure=%u endpointEmission(failure/zero/positive)=%u/%u/%u reverseNee(lookupFailure/zeroPdf/positivePdf)=%u/%u/%u secondary(rejectSelection/rejectMaterial/rejectPdfRay/candidatePositive)=%u/%u/%u/%u visibility(passed/occluded/selfHit)=%u/%u/%u endpointCandidatePositive=%u finalIndirectSelected=%u lookup(logical/physical/exact)=%d/%llu/%d\n",
        counters[69], counters[70], counters[71], counters[72], counters[73],
        counters[74], counters[75], counters[76], counters[77], counters[78],
        counters[79], counters[80], counters[82], counters[83], counters[84],
        counters[85], counters[81], counters[86],
        inputs.sceneInputs->lights.unifiedPtEmissiveLookupCount,
        static_cast<unsigned long long>(
            inputs.sceneInputs->lights.unifiedPtEmissiveLookupBuffer
                ? inputs.sceneInputs->lights.unifiedPtEmissiveLookupBuffer->getDesc().byteSize /
                    sizeof(PathTraceUnifiedEmissiveLookupEntry)
                : 0ull),
        inputs.sceneInputs->lights.unifiedPtEmissiveLookupExact ? 1 : 0);
    common->Printf(
        "PathTraceUnifiedPt: diagnostic secondaryMaterial(indexValid/indexInvalid)=%u/%u diffuse(forceDebug/missingFallback/texturedRgb/texturedYCoCg/descriptorOob)=%u/%u/%u/%u/%u textureDecode=%d\n",
        counters[87], counters[88], counters[89], counters[90], counters[91],
        counters[92], counters[93],
        (m_diagnosticReadbackMaterialPolicyFlags &
            PATH_TRACE_UPT_MATERIAL_DECODE_TEXTURES) != 0u ? 1 : 0);
    common->Printf(
        "PathTraceUnifiedPt: diagnostic continuationRoute hit(static/staticBucket/dynamic/rigid/skinned/unknown)=%u/%u/%u/%u/%u/%u decode(success/failStatic/failStaticBucket/failDynamic/failRigid/failSkinned/failUnknown)=%u/%u/%u/%u/%u/%u/%u\n",
        counters[94], counters[95], counters[96], counters[97], counters[98],
        counters[99], counters[100], counters[101], counters[102],
        counters[103], counters[104], counters[105], counters[106]);

    const uint32_t* probe = counters + UPT04_DIAGNOSTIC_COUNTER_COUNT;
    const uint32_t eventKind = (probe[0] >> 4u) & 0x7u;
    const uint32_t denseIdentity = probe[2];
    const uint32_t stableFingerprint = probe[3];
    const uint32_t denseIndex = denseIdentity != 0u
        ? denseIdentity - 1u : UINT32_MAX;
    const PathTraceUnifiedLightRecord* denseRecord =
        inputs.currentLightRecords && denseIndex < inputs.currentLightRecordCount
            ? &inputs.currentLightRecords[denseIndex]
            : nullptr;
    const uint32_t denseFingerprint = denseRecord
        ? Upt04StableLightIdentityFingerprint(*denseRecord) : 0u;
    const PathTraceUnifiedLightRecord* fingerprintRecord = nullptr;
    uint32_t fingerprintIndex = UINT32_MAX;
    if (inputs.currentLightRecords && stableFingerprint != 0u)
    {
        for (uint32_t index = 0u; index < inputs.currentLightRecordCount; ++index)
        {
            if (Upt04StableLightIdentityFingerprint(
                    inputs.currentLightRecords[index]) == stableFingerprint)
            {
                fingerprintRecord = &inputs.currentLightRecords[index];
                fingerprintIndex = index;
                break;
            }
        }
    }
    const PathTraceUnifiedLightRecord* identifiedRecord = denseRecord
        && denseFingerprint == stableFingerprint
            ? denseRecord : fingerprintRecord;
    float accumulatedRouletteProbability = 0.0f;
    std::memcpy(
        &accumulatedRouletteProbability,
        &probe[15],
        sizeof(accumulatedRouletteProbability));
    const bool denseMatches = denseRecord &&
        denseFingerprint == stableFingerprint;
    common->Printf(
        "PathTraceUnifiedPt: diagnostic crosshair reservoir page=%s serial=%llu event=%u header=%08x dense=%u fingerprint=%08x currentIndex=%u match=%d replay=%u roulette=%.6f\n",
        m_diagnosticProbeFromHistory ? "previous-production" : "diagnostic-current",
        static_cast<unsigned long long>(m_diagnosticProbeFrameSerial),
        eventKind,
        probe[0],
        denseIdentity,
        stableFingerprint,
        fingerprintIndex,
        denseMatches ? 1 : 0,
        probe[5],
        accumulatedRouletteProbability);

    const uint32_t* d0Probe =
        probe + UPT04_DIAGNOSTIC_RESERVOIR_PROBE_WORD_COUNT;
    if (d0Probe[0] == 0x44305052u)
    {
        common->Printf(
            "PathTraceUnifiedPt: diagnostic crosshair D0 receiver(valid/material)=%u/%u selected(valid/type/dense/source/instance/primitive/material)=%u/%u/%u/%u/%u/%u/%u visibility(hit/instance/geometry/primitive/material)=%u/%u/%u/%u/%u result(ready/status/flags)=%u/%u/%08x ranges(emissive/analytic)=%u/%u\n",
            d0Probe[1] & 1u,
            d0Probe[2],
            (d0Probe[1] >> 1u) & 1u,
            d0Probe[3],
            d0Probe[4],
            d0Probe[5],
            d0Probe[6],
            d0Probe[7],
            d0Probe[8],
            d0Probe[9],
            d0Probe[10],
            d0Probe[11],
            d0Probe[12],
            d0Probe[13],
            d0Probe[14] & 0xffu,
            (d0Probe[14] >> 8u) & 0xffu,
            d0Probe[15],
            d0Probe[1] >> 16u,
            d0Probe[14] >> 16u);
    }
    const uint32_t* c0Probe =
        d0Probe + UPT04_DIAGNOSTIC_D0_PROBE_WORD_COUNT;
    if (c0Probe[0] == 0x43305052u)
    {
        float hitT = 0.0f;
        float rayOrigin[3] = {};
        float rayDirection[3] = {};
        float rayTMin = 0.0f;
        float rayTMax = 0.0f;
        std::memcpy(&hitT, &c0Probe[9], sizeof(hitT));
        std::memcpy(rayOrigin, &c0Probe[12], sizeof(rayOrigin));
        std::memcpy(rayDirection, &c0Probe[15], sizeof(rayDirection));
        std::memcpy(&rayTMin, &c0Probe[18], sizeof(rayTMin));
        std::memcpy(&rayTMax, &c0Probe[19], sizeof(rayTMax));
        common->Printf(
            "PathTraceUnifiedPt: diagnostic crosshair C0 receiver(valid/material/surface)=%u/%u/%08x continuation(valid/traceRequired)=%u/%u route=%u decoded=%u hit(status/instance/geometry/primitive/frontFace/t/material/flags)=%u/%u/%u/%u/%u/%.6f/%u/%08x ray(origin=%.3f,%.3f,%.3f direction=%.6f,%.6f,%.6f t=%.6f..%.3e)\n",
            c0Probe[1] & 1u,
            c0Probe[2],
            c0Probe[3],
            (c0Probe[1] >> 1u) & 1u,
            (c0Probe[1] >> 2u) & 1u,
            (c0Probe[1] >> 8u) & 0xffu,
            (c0Probe[1] >> 16u) & 1u,
            c0Probe[4],
            c0Probe[5],
            c0Probe[6],
            c0Probe[7],
            c0Probe[8],
            hitT,
            c0Probe[10],
            c0Probe[11],
            rayOrigin[0], rayOrigin[1], rayOrigin[2],
            rayDirection[0], rayDirection[1], rayDirection[2],
            rayTMin, rayTMax);
    }
    if (identifiedRecord)
    {
        const PathTraceSmokeEmissiveTriangle* sourceTriangle =
            identifiedRecord->type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE &&
            inputs.currentEmissiveTriangles &&
            identifiedRecord->sourceIndex < inputs.currentEmissiveTriangleCount
                ? &inputs.currentEmissiveTriangles[identifiedRecord->sourceIndex]
                : nullptr;
        const RtMaterialRecord* materialRecord = sourceTriangle
            ? FindPathTraceMaterialRecord(sourceTriangle->materialId)
            : nullptr;
        common->Printf(
            "PathTraceUnifiedPt: diagnostic crosshair source type=%u source=%u material(index/id/name)=%u/%u/'%s' primitive=%u instance=%u center=(%.3f %.3f %.3f) area=%.3f radiance=(%.3f %.3f %.3f luma=%.3f) texture(index/name)=%u/'%s' pdf=%.9f weight=%.3f flags=%08x\n",
            identifiedRecord->type,
            identifiedRecord->sourceIndex,
            identifiedRecord->materialOrLightId,
            sourceTriangle ? sourceTriangle->materialId : 0u,
            materialRecord ? materialRecord->materialName.c_str() : "unknown",
            identifiedRecord->primitiveIndex,
            identifiedRecord->instanceId,
            identifiedRecord->positionAndRadius[0],
            identifiedRecord->positionAndRadius[1],
            identifiedRecord->positionAndRadius[2],
            identifiedRecord->normalAndArea[3],
            identifiedRecord->radianceAndLuminance[0],
            identifiedRecord->radianceAndLuminance[1],
            identifiedRecord->radianceAndLuminance[2],
            identifiedRecord->radianceAndLuminance[3],
            sourceTriangle ? sourceTriangle->emissiveTextureIndex : UINT32_MAX,
            materialRecord ? materialRecord->emissiveImageName.c_str() : "unknown",
            identifiedRecord->sourcePdf,
            identifiedRecord->sourceWeight,
            identifiedRecord->flags);
    }
    inputs.device->unmapBuffer(m_diagnosticReadback);
    m_diagnosticReadbackPending = false;
}

bool PathTraceUnifiedPtState::ExecuteInitial(const PathTraceUnifiedPtDispatchInputs& inputs)
{
    // Presentation is admitted per frame only after the matching UPT-05
    // resolve completes; never expose a previous frame after an early return.
    m_resolveReady = false;
    m_presentationOutput = nullptr;
    m_initialPublishedThisFrame = false;
    m_spatialExecutedThisFrame = false;
    DrainDiagnosticReadback(inputs);
    if (!inputs.duplication && m_duplicationSampleIds)
    {
        ReleaseDuplication();
    }
    if (!inputs.lightTiles && m_lightTileBuffer)
    {
        ReleaseLightTiles();
    }
    if (!Upt04InputsValid(inputs))
    {
        if (!m_resourceFailureLogged)
        {
            common->Printf(
                "PathTraceUnifiedPt: UPT-04 live input closure is incomplete; initial dispatch skipped\n");
            m_resourceFailureLogged = true;
        }
        return false;
    }
    if (inputs.proofStage <= 1u)
    {
        ReportProofStage(1u, "input-closure", inputs.backend, inputs.family);
        return true;
    }
    if (inputs.historyEpoch == 0)
    {
        if (!m_resourceFailureLogged)
        {
            common->Printf(
                "PathTraceUnifiedPt: missing full-width history epoch; dispatch skipped\n");
            m_resourceFailureLogged = true;
        }
        return false;
    }
    if (m_observedHistoryEpoch != inputs.historyEpoch)
    {
        m_page0Metadata.Invalidate();
        m_page1Metadata.Invalidate();
        m_duplicationMetadata[0].Invalidate();
        m_duplicationMetadata[1].Invalidate();
        m_reportedTemporalHistoryAvailable = -1;
        m_currentPageIndex = 0u;
        m_historyPageIndex = 1u;
        m_observedHistoryEpoch = inputs.historyEpoch;
        common->Printf(
            "PathTraceUnifiedPt: history metadata invalidated epoch=%llu reasons=0x%08x pageClear=none\n",
            static_cast<unsigned long long>(inputs.historyEpoch),
            inputs.historyResetReasonFlags);
    }
    if (m_temporalModeActive != inputs.temporal ||
        m_spatialModeActive != inputs.spatial)
    {
        m_temporalModeActive = inputs.temporal;
        m_spatialModeActive = inputs.spatial;
        m_currentPageIndex = 0u;
        m_historyPageIndex = 1u;
        m_page0Metadata.Invalidate();
        m_page1Metadata.Invalidate();
        m_duplicationMetadata[0].Invalidate();
        m_duplicationMetadata[1].Invalidate();
        m_reportedTemporalHistoryAvailable = -1;
        common->Printf(
            "PathTraceUnifiedPt: reuse mode temporal=%d spatial=%d pageRoles=current0/history1 historyInvalidated=1 pageClear=none\n",
            inputs.temporal ? 1 : 0,
            inputs.spatial ? 1 : 0);
    }
    if (!EnsurePages(inputs))
    {
        return false;
    }
    if (!EnsureDiagnosticBuffers(inputs))
    {
        return false;
    }
    if (!inputs.splitContinuation && m_continuationHits)
    {
        ReleaseContinuation();
    }
    if (inputs.proofStage == 2u)
    {
        ReportProofStage(2u, "page-allocation", inputs.backend, inputs.family);
        return true;
    }
    if (!EnsurePipeline(inputs))
    {
        return false;
    }
    if (inputs.proofStage == 3u)
    {
        ReportProofStage(3u, "pipeline-creation", inputs.backend, inputs.family);
        return true;
    }
    if (inputs.compactGeometry &&
        (!EnsureCompactGeometryResources(inputs) ||
         !EnsureCompactGeometryPipeline(inputs) ||
         !EnsureCompactGeometryBindingSet(inputs)))
    {
        return false;
    }
    if (inputs.compactLights &&
        (!EnsureCompactLightResources(inputs) ||
         !EnsureCompactLightPipeline(inputs) ||
         !EnsureCompactLightBindingSet(inputs)))
    {
        return false;
    }
    if (inputs.lightTiles &&
        (!EnsureLightTileResources(inputs) ||
         !EnsureLightTilePipeline(inputs) ||
         !EnsureLightTileBindingSet(inputs)))
    {
        return false;
    }
    if (inputs.compactMaterials &&
        (!EnsureCompactMaterialResources(inputs) ||
         !EnsureCompactMaterialPipeline(inputs) ||
         !EnsureCompactMaterialBindingSet(inputs)))
    {
        return false;
    }
    if (inputs.splitContinuation &&
        (!EnsureContinuationResources(inputs) ||
         !EnsureContinuationPipeline(inputs) ||
         !EnsureContinuationBindingSet(inputs)))
    {
        return false;
    }
    if (!EnsureBindingSet(inputs))
    {
        return false;
    }
    if (inputs.proofStage == 4u)
    {
        ReportProofStage(4u, "descriptor-creation", inputs.backend, inputs.family);
        return true;
    }
    if (!ExecuteCompactGeometryPack(inputs))
    {
        return false;
    }
    if (!ExecuteCompactLightPack(inputs))
    {
        return false;
    }
    if (!ExecuteLightTilePresample(inputs))
    {
        return false;
    }
    if (!ExecuteCompactMaterialPack(inputs))
    {
        return false;
    }
    if (!ExecuteContinuationTrace(inputs))
    {
        return false;
    }
    m_resourceFailureLogged = false;

    const bool liveTlasProbe = Upt04PipelineVariant(inputs) != 0u;
    const bool compactLiveTlasProbe =
        Upt04UsesCompactProbeLayout(Upt04PipelineVariant(inputs));
    const bool minimalProductionSlotLayout =
        Upt04UsesMinimalProductionSlotLayout(Upt04PipelineVariant(inputs));
    const bool traversalIsolationLayout =
        Upt04UsesTraversalIsolationLayout(Upt04PipelineVariant(inputs));
    const bool usesPushConstants =
        Upt04UsesPushConstants(Upt04PipelineVariant(inputs));
    const bool usesBindlessSet =
        Upt04UsesBindlessSet(Upt04PipelineVariant(inputs));
    const bool directOnlyProduction =
        Upt04UsesDirectOnlyProductionLayout(inputs);
    const bool productionFullFrame = inputs.proofStage >= 9u &&
        Upt04PipelineVariant(inputs) == 0u;
    const uint32_t enabledFamilyMask = Upt04FamilyMask(inputs.family);
    const uint32_t specializationIdentity =
        static_cast<uint32_t>(inputs.family) |
        (static_cast<uint32_t>(inputs.backend) << 8u);
    const uint64_t contentGeneration = Upt06BuildContentGeneration(
        inputs, enabledFamilyMask, specializationIdentity);
    const PathTraceUnifiedPtPageMetadata& d0HistoryMetadata =
        HistoryPageMetadata();
    const bool previousReceiverLayoutAvailable =
        inputs.primaryReceiverMode == 0u ||
        (inputs.primaryReceiverMode == 2u && inputs.compactPrimaryHistory &&
            inputs.primaryHistorySidecarPreviousBuffer);
    const bool d0PreviousBestHistoryAvailable = productionFullFrame &&
        r_pathTracingUnifiedPtD0PreviousBest.GetBool() &&
        inputs.primarySurfaceHistoryValid && previousReceiverLayoutAvailable &&
        inputs.sceneInputs->lights.restirLightManagerPreviousToCurrentBuffer &&
        inputs.sceneInputs->lights.restirLightManagerPreviousToCurrentCount > 0 &&
        d0HistoryMetadata.fullyWritten &&
        d0HistoryMetadata.width == inputs.width &&
        d0HistoryMetadata.height == inputs.height &&
        d0HistoryMetadata.contentGeneration == contentGeneration &&
        d0HistoryMetadata.historyEpoch == inputs.historyEpoch &&
        d0HistoryMetadata.frameSerial != 0u &&
        d0HistoryMetadata.frameSerial == m_lastPublishedFrameSerial;
    const Upt04InitialControl control = !usesPushConstants
        ? Upt04InitialControl{}
        : Upt04BuildControl(inputs, d0PreviousBestHistoryAvailable);
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.D0 Initial Bind+Barriers",
            inputs.nsightMarkers);
        if (compactLiveTlasProbe || minimalProductionSlotLayout)
        {
            inputs.commandList->setAccelStructState(
                inputs.sceneInputs->geometry.tlas,
                nvrhi::ResourceStates::AccelStructRead);
        }
        else if (traversalIsolationLayout)
        {
            inputs.commandList->setAccelStructState(
                inputs.sceneInputs->geometry.tlas,
                nvrhi::ResourceStates::AccelStructRead);
            inputs.commandList->setBufferState(
                inputs.primarySurfaceBuffer,
                nvrhi::ResourceStates::ShaderResource);
        }
        else if (directOnlyProduction)
        {
            Upt04SetDirectSrvStates(inputs.commandList, inputs);
        }
        else
        {
            Upt04SetSrvStates(inputs.commandList, inputs);
        }
        if (!compactLiveTlasProbe && !minimalProductionSlotLayout &&
            !traversalIsolationLayout)
        {
            inputs.commandList->setBufferState(
                HistoryPage(), nvrhi::ResourceStates::ShaderResource);
        }
        if (inputs.splitContinuation)
        {
            inputs.commandList->setBufferState(
                m_continuationHits,
                nvrhi::ResourceStates::ShaderResource);
        }
        inputs.commandList->setBufferState(
            CurrentPage(), nvrhi::ResourceStates::UnorderedAccess);
        if (inputs.diagnostics)
        {
            inputs.commandList->setBufferState(
                m_diagnosticCounters, nvrhi::ResourceStates::UnorderedAccess);
        }
        inputs.commandList->commitBarriers();
        if (m_page0NeedsAllocationClear)
        {
            // One-shot allocation clear, not a per-frame reservoir operation.
            inputs.commandList->clearBufferUInt(CurrentPage(), 0u);
            nvrhi::utils::BufferUavBarrier(inputs.commandList, CurrentPage());
            m_page0NeedsAllocationClear = false;
        }
        if (inputs.diagnostics)
        {
            inputs.commandList->clearBufferUInt(m_diagnosticCounters, 0u);
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, m_diagnosticCounters);
        }

        if (inputs.proofStage == 5u)
        {
            ReportProofStage(5u, "barriers-and-clear", inputs.backend, inputs.family);
            return true;
        }

        if (liveTlasProbe || m_backend == PathTraceUnifiedPtBackend::RayQuery)
        {
            nvrhi::ComputeState state;
            state.pipeline = m_computePipeline;
            state.bindings = { m_bindingSets[m_currentPageIndex] };
            if (usesBindlessSet)
            {
                state.bindings.push_back(
                    inputs.sceneInputs->materials.textureDescriptorTable);
            }
            inputs.commandList->setComputeState(state);
        }
        else
        {
            nvrhi::rt::State state;
            state.shaderTable = m_shaderTable;
            state.bindings = { m_bindingSets[m_currentPageIndex] };
            if (usesBindlessSet)
            {
                state.bindings.push_back(
                    inputs.sceneInputs->materials.textureDescriptorTable);
            }
            inputs.commandList->setRayTracingState(state);
        }
        if (usesPushConstants)
        {
            inputs.commandList->setPushConstants(&control, sizeof(control));
        }

        if (inputs.proofStage == 6u)
        {
            ReportProofStage(6u, "state-and-push-binding", inputs.backend, inputs.family);
            return true;
        }
    }

    {
        const bool oneGroup = inputs.proofStage == 7u;
        const bool oneGroupRow = inputs.proofStage == 8u;
        const char* oneGroupRayQueryMarker = inputs.shaderProofMode == 1u
            ? "UPT.D0 Initial RayQuery 8x8 UavWrite"
            : (inputs.shaderProofMode == 2u
                ? "UPT.D0 Initial RayQuery 8x8 PrimaryRead"
                : (inputs.shaderProofMode == 3u
                    ? "UPT.D0 Initial RayQuery 8x8 ProposalNoTrace"
                    : (inputs.shaderProofMode == 4u
                        ? "UPT.D0 Initial RayQuery 8x8 FixedRayStatus"
                        : (inputs.shaderProofMode == 5u
                            ? "UPT.D0 Initial RayQuery 8x8 ProposedRayStatus"
                            : "UPT.D0 Initial RayQuery 8x8 FullHitMetadata"))));
        const char* markerName = liveTlasProbe
            ? Upt04LiveTlasProbeMarkerName(m_pipelineVariant)
            : (m_backend == PathTraceUnifiedPtBackend::RayQuery
            ? (inputs.splitInitial
                ? (oneGroup
                    ? "UPT.D0a Split Direct RayQuery 8x8"
                    : (oneGroupRow
                        ? "UPT.D0a Split Direct RayQuery OneGroupRow"
                        : "UPT.D0a Split Direct RayQuery FullFrame"))
                : (inputs.splitContinuation
                    ? (oneGroup
                        ? "UPT.D0 ContinuationShade RayQuery 8x8"
                        : (oneGroupRow
                            ? "UPT.D0 ContinuationShade RayQuery OneGroupRow"
                            : "UPT.D0 ContinuationShade RayQuery FullFrame"))
                    : (oneGroup
                        ? oneGroupRayQueryMarker
                        : (oneGroupRow
                            ? "UPT.D0 Initial RayQuery Dispatch OneGroupRow"
                            : "UPT.D0 Initial RayQuery Dispatch FullFrame"))))
            : (oneGroup
                ? "UPT.D0 Initial RayGen DispatchRays 8x8"
                : (oneGroupRow
                    ? "UPT.D0 Initial RayGen DispatchRays OneGroupRow"
                    : "UPT.D0 Initial RayGen DispatchRays FullFrame")));
        Upt04MarkerScope marker(inputs.commandList, markerName, inputs.nsightMarkers);
        if (liveTlasProbe && !traversalIsolationLayout)
        {
            inputs.commandList->dispatch(1u, 1u, 1u);
        }
        else if (traversalIsolationLayout ||
            m_backend == PathTraceUnifiedPtBackend::RayQuery)
        {
            const uint32_t groupCountX = oneGroup
                ? 1u
                : (inputs.width + 7u) / 8u;
            const uint32_t groupCountY = oneGroup || oneGroupRow
                ? 1u
                : (inputs.height + 7u) / 8u;
            inputs.commandList->dispatch(
                groupCountX,
                groupCountY,
                1u);
        }
        else
        {
            nvrhi::rt::DispatchRaysArguments args;
            args.width = oneGroup ? Min(inputs.width, 8u) : inputs.width;
            args.height = oneGroup || oneGroupRow ? Min(inputs.height, 8u) : inputs.height;
            args.depth = 1u;
            inputs.commandList->dispatchRays(args);
        }
    }

    if (inputs.splitInitial)
    {
        const bool oneGroup = inputs.proofStage == 7u;
        const bool oneGroupRow = inputs.proofStage == 8u;
        {
            Upt04MarkerScope marker(
                inputs.commandList,
                "UPT.D0 Split Intermediate Reservoir Barrier",
                inputs.nsightMarkers);
            nvrhi::utils::BufferUavBarrier(inputs.commandList, CurrentPage());
            inputs.commandList->commitBarriers();
            nvrhi::ComputeState state;
            state.pipeline = m_splitIndirectComputePipeline;
            state.bindings = { m_bindingSets[m_currentPageIndex] };
            state.bindings.push_back(
                inputs.sceneInputs->materials.textureDescriptorTable);
            inputs.commandList->setComputeState(state);
            inputs.commandList->setPushConstants(&control, sizeof(control));
        }
        {
            const char* markerName = oneGroup
                ? "UPT.D0b Split Indirect RayQuery 8x8"
                : (oneGroupRow
                    ? "UPT.D0b Split Indirect RayQuery OneGroupRow"
                    : "UPT.D0b Split Indirect RayQuery FullFrame");
            Upt04MarkerScope marker(
                inputs.commandList,
                markerName,
                inputs.nsightMarkers);
            inputs.commandList->dispatch(
                oneGroup ? 1u : (inputs.width + 7u) / 8u,
                oneGroup || oneGroupRow ? 1u : (inputs.height + 7u) / 8u,
                1u);
        }
    }

    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.D0 Initial OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, CurrentPage());
        if (inputs.diagnostics)
        {
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, m_diagnosticCounters);
            inputs.commandList->setBufferState(
                m_diagnosticCounters, nvrhi::ResourceStates::CopySource);
            inputs.commandList->setBufferState(
                m_diagnosticReadback, nvrhi::ResourceStates::CopyDest);
            inputs.commandList->commitBarriers();
            inputs.commandList->copyBuffer(
                m_diagnosticReadback,
                0,
                m_diagnosticCounters,
                0,
                UPT04_DIAGNOSTIC_BYTES);
            m_diagnosticReadbackPending = true;
            m_diagnosticReadbackDelayFrames = 2;
            m_diagnosticReadbackSampleIndex = inputs.frameSampleIndex;
            m_diagnosticReadbackWidth = inputs.width;
            m_diagnosticReadbackHeight = inputs.height;
            m_diagnosticReadbackMaterialPolicyFlags =
                inputs.materialPolicyFlags;
            m_diagnosticReadbackFamily = inputs.family;
        }
    }
    if (productionFullFrame)
    {
        PathTraceUnifiedPtPageMetadata& currentMetadata = CurrentPageMetadata();
        const bool firstPublicationForEpoch = !currentMetadata.fullyWritten ||
            currentMetadata.historyEpoch != inputs.historyEpoch;
        currentMetadata.fullyWritten = true;
        currentMetadata.width = inputs.width;
        currentMetadata.height = inputs.height;
        currentMetadata.contentGeneration = contentGeneration;
        currentMetadata.historyEpoch = inputs.historyEpoch;
        currentMetadata.frameSerial = ++m_lastPublishedFrameSerial;
        // D0 is a complete current-frame publication even when a later reuse
        // pass cannot run. CompleteFrame must still promote it alongside the
        // primary-surface history or the next T0 dispatch would pair an N-2
        // reservoir with the N-1 surface published by SmokeDispatch.
        m_initialPublishedThisFrame = true;
        if (firstPublicationForEpoch)
        {
            common->Printf(
                "PathTraceUnifiedPt: page metadata current=%u(full/generation/epoch/serial)=%d/%016llx/%llu/%llu history=%u(full/serial)=%d/%llu clears(allocation/invalidation)=1/0 pages=2\n",
                m_currentPageIndex,
                currentMetadata.fullyWritten ? 1 : 0,
                static_cast<unsigned long long>(contentGeneration),
                static_cast<unsigned long long>(inputs.historyEpoch),
                static_cast<unsigned long long>(currentMetadata.frameSerial),
                m_historyPageIndex,
                HistoryPageMetadata().fullyWritten ? 1 : 0,
                static_cast<unsigned long long>(
                    HistoryPageMetadata().frameSerial));
        }
    }
    if (inputs.proofStage == 7u)
    {
        ReportProofStage(
            7u,
            liveTlasProbe && !traversalIsolationLayout
                ? "one-invocation-live-tlas-probe"
                : "one-8x8-group",
            inputs.backend,
            inputs.family);
    }
    else if (inputs.proofStage == 8u)
    {
        ReportProofStage(8u, "one-full-width-group-row", inputs.backend, inputs.family);
    }
    else
    {
        ReportProofStage(9u, "full-frame-dispatch", inputs.backend, inputs.family);
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureDuplicationResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.duplication)
        return false;
    const uint64_t surfaceCount = uint64_t(inputs.width) * uint64_t(inputs.height);
    const uint32_t packedPitch = (inputs.width + 3u) / 4u;
    const uint64_t packedCount = uint64_t(packedPitch) * uint64_t(inputs.height);
    if (surfaceCount == 0u || surfaceCount > std::numeric_limits<uint32_t>::max()
        || packedCount > std::numeric_limits<uint32_t>::max())
        return false;
    if (m_duplicationSampleIds && m_duplicationScores[0]
        && m_duplicationScores[1] && m_duplicationWidth == inputs.width
        && m_duplicationHeight == inputs.height
        && m_duplicationPackedRowPitch == packedPitch)
        return true;

    ReleaseDuplication();
    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtSampleIds";
    desc.byteSize = surfaceCount * sizeof(uint32_t);
    desc.structStride = sizeof(uint32_t);
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    m_duplicationSampleIds = inputs.device->createBuffer(desc);
    desc.byteSize = packedCount * sizeof(uint32_t);
    desc.debugName = "PathTraceUnifiedPtDuplicationPage0";
    m_duplicationScores[0] = inputs.device->createBuffer(desc);
    desc.debugName = "PathTraceUnifiedPtDuplicationPage1";
    m_duplicationScores[1] = inputs.device->createBuffer(desc);
    if (!m_duplicationSampleIds || !m_duplicationScores[0]
        || !m_duplicationScores[1])
    {
        common->Printf("PathTraceUnifiedPt: failed to allocate UPT-08 correlation resources\n");
        ReleaseDuplication();
        return false;
    }
    m_duplicationWidth = inputs.width;
    m_duplicationHeight = inputs.height;
    m_duplicationPackedRowPitch = packedPitch;
    common->Printf(
        "PathTraceUnifiedPt: allocated UPT-08 sampleIds=%llu duplicationPages=%llu totalBytes=%llu format=R32+packedUNORM8 clear=never\n",
        static_cast<unsigned long long>(surfaceCount * 4ull),
        static_cast<unsigned long long>(packedCount * 8ull),
        static_cast<unsigned long long>(surfaceCount * 4ull + packedCount * 8ull));
    return true;
}

bool PathTraceUnifiedPtState::EnsureDuplicationPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (m_duplicationFillPipeline && m_duplicationComputePipeline)
        return true;
    if (m_duplicationPipelineAttempted)
        return false;
    m_duplicationPipelineAttempted = true;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT08_PUSH_CONSTANT_BYTES));
    m_duplicationBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_duplicationBindingLayout)
        return false;

    const char* paths[2] = {
        "renderprogs2/spirv/builtin/pathtracing/slang_upt08/upt08_fill_sample_id.bin",
        "renderprogs2/spirv/builtin/pathtracing/slang_upt08/upt08_compute_duplication.bin"
    };
    for (uint32_t pass = 0; pass < 2u; ++pass)
    {
        void* data = nullptr;
        int size = 0;
        ID_TIME_T timestamp = 0;
        uint64_t hash = 0;
        if (!Upt04ReadShader(paths[pass], data, size, timestamp, hash))
            return false;
        nvrhi::ShaderDesc shaderDesc;
        shaderDesc.shaderType = nvrhi::ShaderType::Compute;
        shaderDesc.entryName = "main";
        shaderDesc.debugName = pass == 0u
            ? "PathTraceUnifiedPtFillSampleId"
            : "PathTraceUnifiedPtComputeDuplication";
        nvrhi::ShaderHandle shader = inputs.device->createShader(
            shaderDesc, data, size);
        Mem_Free(data);
        if (!shader)
            return false;
        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.CS = shader;
        pipelineDesc.bindingLayouts = { m_duplicationBindingLayout };
        nvrhi::ComputePipelineHandle pipeline =
            inputs.device->createComputePipeline(pipelineDesc);
        if (!pipeline)
            return false;
        if (pass == 0u)
        {
            m_duplicationFillShader = shader;
            m_duplicationFillPipeline = pipeline;
        }
        else
        {
            m_duplicationComputeShader = shader;
            m_duplicationComputePipeline = pipeline;
        }
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureDuplicationBindingSets(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!m_duplicationSampleIds || !m_duplicationScores[0]
        || !m_duplicationScores[1] || !m_page0 || !m_page1)
        return false;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        if (!m_duplicationFillBindingSets[page])
        {
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                0, page == 0u ? m_page0 : m_page1));
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
                1, m_duplicationSampleIds));
            desc.addItem(nvrhi::BindingSetItem::PushConstants(
                0, UPT08_PUSH_CONSTANT_BYTES));
            m_duplicationFillBindingSets[page] = inputs.device->createBindingSet(
                desc, m_duplicationBindingLayout);
        }
        if (!m_duplicationComputeBindingSets[page])
        {
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                0, m_duplicationSampleIds));
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
                1, m_duplicationScores[page]));
            desc.addItem(nvrhi::BindingSetItem::PushConstants(
                0, UPT08_PUSH_CONSTANT_BYTES));
            m_duplicationComputeBindingSets[page] = inputs.device->createBindingSet(
                desc, m_duplicationBindingLayout);
        }
        if (!m_duplicationFillBindingSets[page]
            || !m_duplicationComputeBindingSets[page])
            return false;
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureTemporalPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const bool indirect = r_pathTracingUnifiedPtTemporalIndirect.GetBool();
    const bool earlyReconnect = indirect
        && r_pathTracingUnifiedPtTemporalEarlyReconnect.GetBool();
    const bool routeDiagnostics = indirect && !earlyReconnect
        && r_pathTracingUnifiedPtTemporalRouteDiagnostics.GetBool();
    const bool sharedReuseAdapterRequested =
        r_pathTracingUnifiedPtSharedReuseAdapter.GetBool();
    const bool sharedReuseAdapter = indirect
        && sharedReuseAdapterRequested
        && inputs.compactLights
        && inputs.duplication
        && inputs.family == PathTraceUnifiedPtFamily::Unified
        && !earlyReconnect
        && !routeDiagnostics
        && !inputs.lambertDiagnostic
        && !inputs.frozenStaticDiagnostic
        && !inputs.frozenLightDiagnostic
        && inputs.temporalBottleneckProbe == 0u;
    const bool commonGrisMergeRequested =
        r_pathTracingUnifiedPtCommonGrisMerge.GetBool();
    const bool commonGrisMerge = sharedReuseAdapter
        && commonGrisMergeRequested
        && r_pathTracingUnifiedPtTemporalPairwise.GetBool();
    const bool threeVertexReplay = inputs.threeVertexInitial
        && commonGrisMerge;
    const bool replayCompaction = commonGrisMerge
        && Upt43TemporalReplayCompactionEnabled(inputs);
    const bool boilingFilter = replayCompaction
        && Upt436TemporalBoilingFilterEnabled(inputs);
    if (m_temporalPipeline
        && (!replayCompaction || (m_temporalReplayPipeline
            && m_temporalReplayBindingLayout))
        && (!boilingFilter || (m_temporalBoilingFilterPipeline
            && m_temporalBoilingFilterBindingLayout))
        && m_temporalCompactLights == inputs.compactLights
        && m_temporalDuplication == inputs.duplication
        && m_temporalIndirect == indirect
        && m_temporalSharedReuseAdapter == sharedReuseAdapter
        && m_temporalCommonGrisMerge == commonGrisMerge
        && m_temporalThreeVertexReplay == threeVertexReplay
        && m_temporalReplayCompaction == replayCompaction
        && m_temporalBoilingFilter == boilingFilter
        && m_temporalEarlyReconnect == earlyReconnect
        && m_temporalRouteDiagnostics == routeDiagnostics
        && m_temporalLambertDiagnostic == inputs.lambertDiagnostic
        && m_temporalFrozenStaticDiagnostic ==
            inputs.frozenStaticDiagnostic
        && m_temporalFrozenLightDiagnostic ==
            inputs.frozenLightDiagnostic
        && m_temporalBottleneckProbe == inputs.temporalBottleneckProbe)
    {
        return true;
    }
    if (m_temporalCompactLights != inputs.compactLights
        || m_temporalDuplication != inputs.duplication
        || m_temporalIndirect != indirect
        || m_temporalSharedReuseAdapter != sharedReuseAdapter
        || m_temporalCommonGrisMerge != commonGrisMerge
        || m_temporalThreeVertexReplay != threeVertexReplay
        || m_temporalReplayCompaction != replayCompaction
        || m_temporalBoilingFilter != boilingFilter
        || m_temporalEarlyReconnect != earlyReconnect
        || m_temporalRouteDiagnostics != routeDiagnostics
        || m_temporalLambertDiagnostic != inputs.lambertDiagnostic
        || m_temporalFrozenStaticDiagnostic !=
            inputs.frozenStaticDiagnostic
        || m_temporalFrozenLightDiagnostic !=
            inputs.frozenLightDiagnostic
        || m_temporalBottleneckProbe != inputs.temporalBottleneckProbe)
    {
        if (m_temporalCommonGrisMerge != commonGrisMerge)
        {
            // The accepted pairwise path and the common GRIS path persist
            // different mapped-weight conventions. Never let either consume a
            // history page written by the other; metadata invalidation makes
            // this frame use fresh D0 without a full-buffer clear.
            HistoryPageMetadata().Invalidate();
            m_duplicationMetadata[m_historyPageIndex].Invalidate();
            m_reportedTemporalHistoryAvailable = -1;
            common->Printf(
                "PathTraceUnifiedPt: common GRIS merge transition effective=%u historyInvalidated=1 pageClear=none\n",
                commonGrisMerge ? 1u : 0u);
        }
        ReleaseTemporal();
        m_temporalCompactLights = inputs.compactLights;
        m_temporalDuplication = inputs.duplication;
        m_temporalIndirect = indirect;
        m_temporalSharedReuseAdapter = sharedReuseAdapter;
        m_temporalCommonGrisMerge = commonGrisMerge;
        m_temporalThreeVertexReplay = threeVertexReplay;
        m_temporalReplayCompaction = replayCompaction;
        m_temporalBoilingFilter = boilingFilter;
        m_temporalEarlyReconnect = earlyReconnect;
        m_temporalRouteDiagnostics = routeDiagnostics;
        m_temporalLambertDiagnostic = inputs.lambertDiagnostic;
        m_temporalFrozenStaticDiagnostic =
            inputs.frozenStaticDiagnostic;
        m_temporalFrozenLightDiagnostic =
            inputs.frozenLightDiagnostic;
        m_temporalBottleneckProbe = inputs.temporalBottleneckProbe;
    }
    if (m_temporalPipelineAttempted)
    {
        return false;
    }
    m_temporalPipelineAttempted = true;

    if (!inputs.device->queryFeatureSupport(nvrhi::Feature::RayQuery))
    {
        common->Printf(
            "PathTraceUnifiedPt: UPT-07 temporal requires RayQuery support; skipped\n");
        return false;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
    Upt04AddReplayGeometryLayoutItems(layoutDesc);
    if (indirect)
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(22));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(23));
    if (inputs.duplication)
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(24));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(25));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(26));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(27));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(28));
    if (earlyReconnect || routeDiagnostics)
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(30));
    if (inputs.temporalBottleneckProbe != 0u)
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(31));
    if (replayCompaction)
    {
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(33));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(34));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(35));
    }
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT07_PUSH_CONSTANT_BYTES));
    m_temporalBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_temporalBindingLayout)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-07 temporal binding layout\n");
        return false;
    }

    if (replayCompaction)
    {
        nvrhi::BindingLayoutDesc replayLayoutDesc;
        replayLayoutDesc.visibility = nvrhi::ShaderType::Compute;
        replayLayoutDesc.registerSpace = 0;
        replayLayoutDesc.registerSpaceIsDescriptorSet = true;
        replayLayoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
            .setShaderResourceOffset(0)
            .setSamplerOffset(0)
            .setUnorderedAccessViewOffset(0);
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
        Upt04AddReplayGeometryLayoutItems(replayLayoutDesc);
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(22));
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(23));
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(24));
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(25));
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(26));
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(27));
        replayLayoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(28));
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(33));
        replayLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(34));
        replayLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
            0, UPT07_PUSH_CONSTANT_BYTES));
        m_temporalReplayBindingLayout =
            inputs.device->createBindingLayout(replayLayoutDesc);
        if (!m_temporalReplayBindingLayout)
        {
            common->Printf(
                "PathTraceUnifiedPt: failed to create UPT-43.4 compact replay binding layout\n");
            return false;
        }
    }

    if (boilingFilter)
    {
        nvrhi::BindingLayoutDesc boilingLayoutDesc;
        boilingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
        boilingLayoutDesc.registerSpace = 0;
        boilingLayoutDesc.registerSpaceIsDescriptorSet = true;
        boilingLayoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
            .setShaderResourceOffset(0)
            .setSamplerOffset(0)
            .setUnorderedAccessViewOffset(0);
        boilingLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
        boilingLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
            0, UPT43_TEMPORAL_BOILING_PUSH_CONSTANT_BYTES));
        m_temporalBoilingFilterBindingLayout =
            inputs.device->createBindingLayout(boilingLayoutDesc);
        if (!m_temporalBoilingFilterBindingLayout)
        {
            common->Printf(
                "PathTraceUnifiedPt: failed to create UPT-43.6 boiling-filter binding layout\n");
            return false;
        }
    }

    static const char* bottleneckPaths[12] = {
        "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_probe1.bin",
        "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_probe2.bin",
        "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_probe3.bin",
        "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_probe4.bin",
        "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_probe5.bin",
        "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_probe6.bin",
        "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_probe7.bin",
        "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_probe8.bin",
        "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_probe9.bin",
        "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_probe10.bin",
        "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_probe11.bin",
        "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_probe12.bin"
    };
    const char* path = replayCompaction
        ? (threeVertexReplay
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex_replay_classify.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_common_gris_replay_classify.bin")
        : threeVertexReplay
        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex.bin"
        : commonGrisMerge
        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_common_gris.bin"
        : (sharedReuseAdapter
        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_shared_adapter.bin"
        : nullptr);
    if (!path)
        path = inputs.temporalBottleneckProbe != 0u
        ? bottleneckPaths[inputs.temporalBottleneckProbe - 1u]
        : (inputs.frozenStaticDiagnostic || inputs.frozenLightDiagnostic
        ? Upt07FrozenTemporalShaderPath(
            inputs.lambertDiagnostic,
            inputs.duplication,
            inputs.frozenStaticDiagnostic,
            inputs.frozenLightDiagnostic)
        : (inputs.lambertDiagnostic
        ? (inputs.duplication
            ? (inputs.compactLights
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_lambert.bin"
                : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_duplication_lambert.bin")
            : (inputs.compactLights
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_lambert.bin"
                : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_lambert.bin"))
        : (indirect
        ? (inputs.duplication
            ? (inputs.compactLights
                ? (earlyReconnect
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_reconnect.bin"
                    : (routeDiagnostics
                        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_route_diag.bin"
                        : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication.bin"))
                : (earlyReconnect
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_duplication_reconnect.bin"
                    : (routeDiagnostics
                        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_duplication_route_diag.bin"
                        : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_duplication.bin")))
            : (inputs.compactLights
                ? (earlyReconnect
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_reconnect.bin"
                    : (routeDiagnostics
                        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_route_diag.bin"
                        : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64.bin"))
                : (earlyReconnect
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_reconnect.bin"
                    : (routeDiagnostics
                        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_route_diag.bin"
                        : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery.bin"))))
        : (inputs.duplication
            ? (inputs.compactLights
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_direct_rayquery_light64_duplication.bin"
                : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_direct_rayquery_duplication.bin")
            : (inputs.compactLights
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_direct_rayquery_light64.bin"
                : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_direct_rayquery.bin")))));
    void* data = nullptr;
    int size = 0;
    ID_TIME_T timestamp = 0;
    uint64_t hash = 0;
    if (!Upt04ReadShader(path, data, size, timestamp, hash))
    {
        return false;
    }
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtTemporalUnified";
    m_temporalShader = inputs.device->createShader(shaderDesc, data, size);
    Mem_Free(data);
    if (!m_temporalShader)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-07 temporal shader\n");
        return false;
    }

    int replaySize = 0;
    ID_TIME_T replayTimestamp = 0;
    uint64_t replayHash = 0;
    if (replayCompaction)
    {
        const char* replayPath = threeVertexReplay
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_common_gris_three_vertex_replay_compact.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_unified_rayquery_light64_duplication_common_gris_replay_compact.bin";
        void* replayData = nullptr;
        if (!Upt04ReadShader(
                replayPath, replayData, replaySize,
                replayTimestamp, replayHash))
            return false;
        nvrhi::ShaderDesc replayShaderDesc = shaderDesc;
        replayShaderDesc.debugName =
            "PathTraceUnifiedPtTemporalCompactReplay";
        m_temporalReplayShader = inputs.device->createShader(
            replayShaderDesc, replayData, replaySize);
        Mem_Free(replayData);
        if (!m_temporalReplayShader)
        {
            common->Printf(
                "PathTraceUnifiedPt: failed to create UPT-43.4 compact replay shader\n");
            return false;
        }
    }

    int boilingSize = 0;
    ID_TIME_T boilingTimestamp = 0;
    uint64_t boilingHash = 0;
    if (boilingFilter)
    {
        void* boilingData = nullptr;
        if (!Upt04ReadShader(
                "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_boiling_filter.bin",
                boilingData, boilingSize, boilingTimestamp, boilingHash))
            return false;
        nvrhi::ShaderDesc boilingShaderDesc = shaderDesc;
        boilingShaderDesc.debugName =
            "PathTraceUnifiedPtTemporalBoilingFilter";
        m_temporalBoilingFilterShader = inputs.device->createShader(
            boilingShaderDesc, boilingData, boilingSize);
        Mem_Free(boilingData);
        if (!m_temporalBoilingFilterShader)
        {
            common->Printf(
                "PathTraceUnifiedPt: failed to create UPT-43.6 boiling-filter shader\n");
            return false;
        }
    }

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_temporalShader;
    pipelineDesc.bindingLayouts = {
        m_temporalBindingLayout,
        inputs.sceneInputs->materials.textureBindlessLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_temporalPipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_temporalPipeline)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-07 temporal pipeline\n");
        return false;
    }
    if (replayCompaction)
    {
        nvrhi::ComputePipelineDesc replayPipelineDesc;
        replayPipelineDesc.CS = m_temporalReplayShader;
        replayPipelineDesc.bindingLayouts = {
            m_temporalReplayBindingLayout,
            inputs.sceneInputs->materials.textureBindlessLayout };
        m_temporalReplayPipeline =
            inputs.device->createComputePipeline(replayPipelineDesc);
        if (!m_temporalReplayPipeline)
        {
            common->Printf(
                "PathTraceUnifiedPt: failed to create UPT-43.4 compact replay pipeline\n");
            return false;
        }
    }
    if (boilingFilter)
    {
        nvrhi::ComputePipelineDesc boilingPipelineDesc;
        boilingPipelineDesc.CS = m_temporalBoilingFilterShader;
        boilingPipelineDesc.bindingLayouts = {
            m_temporalBoilingFilterBindingLayout };
        m_temporalBoilingFilterPipeline =
            inputs.device->createComputePipeline(boilingPipelineDesc);
        if (!m_temporalBoilingFilterPipeline)
        {
            common->Printf(
                "PathTraceUnifiedPt: failed to create UPT-43.6 boiling-filter pipeline\n");
            return false;
        }
    }
    common->Printf(
        "PathTraceUnifiedPt: temporal compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=8x8 lightStride=%u historyTaps=9 duplication=%u directVisibilityRaysMax=1 indirectReplay=%u indirectReplayRaysMax=%u threeVertexReplay=%u family=%s sharedReuseAdapter(requested/effective)=%u/%u commonGrisMerge(requested/effective)=%u/%u replayCompaction=%u replayBlobBytes=%d replayHash=%016llx boilingFilter=%u boilingBlobBytes=%d boilingHash=%016llx shading=%s bottleneckProbe=%u createUs=%llu\n",
        size,
        static_cast<unsigned long long>(hash),
        static_cast<long long>(timestamp),
        inputs.compactLights ? UPT04_COMPACT_LIGHT_STRIDE : 112u,
        inputs.duplication ? 1u : 0u,
        indirect ? 1u : 0u,
        indirect ? (threeVertexReplay ? 6u : 4u) : 0u,
        threeVertexReplay ? 1u : 0u,
        indirect ? (earlyReconnect ? "unified-reconnect"
            : (routeDiagnostics ? "unified-route-diagnostics" : "unified"))
            : "direct-basic",
        sharedReuseAdapterRequested ? 1u : 0u,
        sharedReuseAdapter ? 1u : 0u,
        commonGrisMergeRequested ? 1u : 0u,
        commonGrisMerge ? 1u : 0u,
        replayCompaction ? 1u : 0u,
        replaySize,
        static_cast<unsigned long long>(replayHash),
        boilingFilter ? 1u : 0u,
        boilingSize,
        static_cast<unsigned long long>(boilingHash),
        (inputs.frozenStaticDiagnostic || inputs.frozenLightDiagnostic)
            ? (inputs.lambertDiagnostic
                ? "frozen-factor-lambert"
                : "frozen-factor-openpbr")
            : (inputs.lambertDiagnostic
                ? "lambert-diagnostic" : "openpbr"),
        inputs.temporalBottleneckProbe,
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureTemporalBottleneckBuffer(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (inputs.temporalBottleneckProbe == 0u)
        return true;
    const uint64_t count64 = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (count64 == 0u || count64 > UINT32_MAX)
        return false;
    const uint32_t count = static_cast<uint32_t>(count64);
    if (m_temporalBottleneckBuffer && m_temporalBottleneckCapacity == count
        && (inputs.temporalBottleneckProbe != 12u
            || m_temporalWorkBudgetReadback))
        return true;

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtTemporalBottleneckProbe";
    const uint64_t wordsPerPixel = inputs.temporalBottleneckProbe == 12u
        ? UPT07_WORK_BUDGET_WORDS_PER_PIXEL : 1u;
    desc.byteSize = count64 * wordsPerPixel * sizeof(uint32_t);
    desc.structStride = sizeof(uint32_t);
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    m_temporalBottleneckBuffer = inputs.device->createBuffer(desc);
    m_temporalBottleneckCapacity = m_temporalBottleneckBuffer ? count : 0u;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_temporalBindingSets[page] = nullptr;
        m_temporalBindingSetDescValid[page] = false;
    }
    if (inputs.temporalBottleneckProbe == 12u)
    {
        nvrhi::BufferDesc readbackDesc;
        readbackDesc.debugName =
            "PathTraceUnifiedPtTemporalWorkBudgetReadback";
        readbackDesc.byteSize = desc.byteSize;
        readbackDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        readbackDesc.initialState = nvrhi::ResourceStates::CopyDest;
        readbackDesc.keepInitialState = true;
        m_temporalWorkBudgetReadback =
            inputs.device->createBuffer(readbackDesc);
        m_temporalWorkBudgetCaptureArmed =
            m_temporalWorkBudgetReadback != nullptr;
        m_temporalWorkBudgetWarmupFrames =
            m_temporalWorkBudgetCaptureArmed
                ? TEMPORAL_GPU_TIMING_WARMUP_FRAMES
                : 0u;
    }
    if (m_temporalBottleneckBuffer)
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal bottleneck probe buffer pixels=%u bytes=%llu clear=never\n",
            count,
            static_cast<unsigned long long>(desc.byteSize));
    }
    if (inputs.temporalBottleneckProbe == 12u
        && m_temporalWorkBudgetCaptureArmed)
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal work budget armed warmup=%u history=production-feedback\n",
            m_temporalWorkBudgetWarmupFrames);
    }
    return m_temporalBottleneckBuffer != nullptr
        && (inputs.temporalBottleneckProbe != 12u
            || m_temporalWorkBudgetReadback != nullptr);
}

bool PathTraceUnifiedPtState::EnsureTemporalReplayCompactionBuffers(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!m_temporalReplayCompaction)
        return true;
    const uint64_t count64 = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (count64 == 0u || count64 > UINT32_MAX)
        return false;
    const uint32_t count = static_cast<uint32_t>(count64);
    if (m_temporalReplayQueue && m_temporalReplayMeta
        && m_temporalReplayDispatchArgs
        && m_temporalReplayCapacity == count
        && (!r_pathTracingUnifiedPtTemporalReplayCompactionDiagnostics.GetBool()
            || m_temporalReplayReadback))
        return true;

    nvrhi::BufferDesc queueDesc;
    queueDesc.debugName = "PathTraceUnifiedPtTemporalReplayQueue";
    queueDesc.byteSize = count64 * sizeof(uint32_t);
    queueDesc.structStride = sizeof(uint32_t);
    queueDesc.canHaveUAVs = true;
    queueDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    queueDesc.keepInitialState = true;
    m_temporalReplayQueue = inputs.device->createBuffer(queueDesc);

    nvrhi::BufferDesc metaDesc;
    metaDesc.debugName = "PathTraceUnifiedPtTemporalReplayMeta";
    metaDesc.byteSize = 2u * sizeof(uint32_t);
    metaDesc.structStride = sizeof(uint32_t);
    metaDesc.canHaveUAVs = true;
    metaDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    metaDesc.keepInitialState = true;
    m_temporalReplayMeta = inputs.device->createBuffer(metaDesc);

    nvrhi::BufferDesc argsDesc;
    argsDesc.debugName = "PathTraceUnifiedPtTemporalReplayDispatchArgs";
    argsDesc.byteSize = 3u * sizeof(uint32_t);
    argsDesc.structStride = sizeof(uint32_t);
    argsDesc.canHaveUAVs = true;
    argsDesc.isDrawIndirectArgs = true;
    argsDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    argsDesc.keepInitialState = true;
    m_temporalReplayDispatchArgs = inputs.device->createBuffer(argsDesc);

    m_temporalReplayReadback = nullptr;
    if (r_pathTracingUnifiedPtTemporalReplayCompactionDiagnostics.GetBool())
    {
        nvrhi::BufferDesc readbackDesc;
        readbackDesc.debugName =
            "PathTraceUnifiedPtTemporalReplayReadback";
        readbackDesc.byteSize = 5u * sizeof(uint32_t);
        readbackDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        readbackDesc.initialState = nvrhi::ResourceStates::CopyDest;
        readbackDesc.keepInitialState = true;
        m_temporalReplayReadback = inputs.device->createBuffer(readbackDesc);
    }
    m_temporalReplayCapacity = m_temporalReplayQueue
            && m_temporalReplayMeta && m_temporalReplayDispatchArgs
        ? count : 0u;
    for (uint32_t page = 0u; page < 2u; ++page)
    {
        m_temporalBindingSets[page] = nullptr;
        m_temporalBindingSetDescValid[page] = false;
        m_temporalReplayBindingSets[page] = nullptr;
        m_temporalReplayBindingSetDescValid[page] = false;
    }
    if (m_temporalReplayCapacity != 0u)
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal replay compaction queue pixels=%u bytes=%llu recordBytes=4 clear=never resetBytes=20 groups=64x1\n",
            count,
            static_cast<unsigned long long>(queueDesc.byteSize));
    }
    return m_temporalReplayCapacity != 0u
        && (!r_pathTracingUnifiedPtTemporalReplayCompactionDiagnostics.GetBool()
            || m_temporalReplayReadback != nullptr);
}

void PathTraceUnifiedPtState::DrainTemporalReplayCompactionReadback(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!m_temporalReplayReadbackPending
        || !m_temporalReplayReadback || !inputs.device)
        return;
    if (m_temporalReplayReadbackDelayFrames > 0)
    {
        --m_temporalReplayReadbackDelayFrames;
        return;
    }
    const uint32_t* words = static_cast<const uint32_t*>(
        inputs.device->mapBuffer(
            m_temporalReplayReadback, nvrhi::CpuAccessMode::Read));
    if (!words)
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal replay compaction readback map failed\n");
        m_temporalReplayReadbackPending = false;
        return;
    }
    const uint32_t groups = words[0];
    const uint32_t groupY = words[1];
    const uint32_t groupZ = words[2];
    const uint32_t published = words[3];
    const uint32_t capacity = words[4];
    const uint32_t bounded = Min(published, capacity);
    const uint32_t expectedGroups = (bounded + 63u) / 64u;
    common->Printf(
        "PathTraceUnifiedPt: temporal replay compaction published/bounded/capacity=%u/%u/%u groups(actual/expected,y,z)=%u/%u/%u/%u overflow=%u status=%s\n",
        published, bounded, capacity,
        groups, expectedGroups, groupY, groupZ,
        published > capacity ? published - capacity : 0u,
        groups == expectedGroups && groupY == 1u && groupZ == 1u
            ? "PASS" : "FAIL");
    inputs.device->unmapBuffer(m_temporalReplayReadback);
    m_temporalReplayReadbackPending = false;
}

bool PathTraceUnifiedPtState::EnsureTemporalDiagnosticBuffers(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!m_temporalDiagnosticCounters)
    {
        nvrhi::BufferDesc desc;
        desc.debugName = "PathTraceUnifiedPtTemporalDiagnosticCounters";
        desc.byteSize = UPT07_TEMPORAL_DIAGNOSTIC_BYTES;
        desc.structStride = sizeof(uint32_t);
        desc.canHaveUAVs = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;
        m_temporalDiagnosticCounters = inputs.device->createBuffer(desc);
    }
    if ((r_pathTracingUnifiedPtTemporalReconnectDiagnostics.GetBool()
            || r_pathTracingUnifiedPtTemporalRouteDiagnostics.GetBool())
        && !m_temporalDiagnosticReadback)
    {
        nvrhi::BufferDesc desc;
        desc.debugName = "PathTraceUnifiedPtTemporalDiagnosticReadback";
        desc.byteSize = UPT07_TEMPORAL_DIAGNOSTIC_BYTES;
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        m_temporalDiagnosticReadback = inputs.device->createBuffer(desc);
    }
    return m_temporalDiagnosticCounters
        && (!(r_pathTracingUnifiedPtTemporalReconnectDiagnostics.GetBool()
                || r_pathTracingUnifiedPtTemporalRouteDiagnostics.GetBool())
            || m_temporalDiagnosticReadback);
}

void PathTraceUnifiedPtState::DrainTemporalDiagnosticReadback(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!m_temporalDiagnosticReadbackPending
        || !m_temporalDiagnosticReadback || !inputs.device)
        return;
    if (m_temporalDiagnosticReadbackDelayFrames > 0)
    {
        --m_temporalDiagnosticReadbackDelayFrames;
        return;
    }
    const uint32_t* counters = static_cast<const uint32_t*>(
        inputs.device->mapBuffer(
            m_temporalDiagnosticReadback, nvrhi::CpuAccessMode::Read));
    if (!counters)
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal reconnect diagnostic readback map failed\n");
        m_temporalDiagnosticReadbackPending = false;
        return;
    }
    if (m_temporalDiagnosticReadbackIsRoute)
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal route receivers=%u current(direct/endpoint/secondary/empty)=%u/%u/%u/%u history(selected/direct/endpoint/secondary/unsupported/missing)=%u/%u/%u/%u/%u/%u replayHistory(attempt/pass/fail)=%u/%u/%u replayReciprocal(attempt/pass/fail)=%u/%u/%u basic(history/current)=%u/%u merge(history/current/empty)=%u/%u/%u mergeHistory(direct/endpoint/secondary)=%u/%u/%u historyAge(0/1-3/4-15/16-31/32-63)=%u/%u/%u/%u/%u visibilityFallback=%u\n",
            counters[0], counters[1], counters[2], counters[3], counters[29],
            counters[4], counters[5], counters[6], counters[7], counters[8], counters[28],
            counters[9], counters[10], counters[11],
            counters[12], counters[13], counters[14],
            counters[15], counters[16], counters[17], counters[18], counters[19],
            counters[27], counters[25], counters[26],
            counters[20], counters[21], counters[22], counters[23], counters[24],
            counters[31]);
    }
    else
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal reconnect attempted=%u locator(decoded/invalid/noStatic)=%u/%u/%u footprint(accepted/rejected)=%u/%u visibility(executed/passed/failed)=%u/%u/%u shift(positive/zero/jacobianFailure)=%u/%u/%u replayFallback=%u evaluationFailure(source/target)=%u/%u historySelected=%u\n",
            counters[0], counters[1], counters[11], counters[10],
            counters[2], counters[3], counters[4], counters[5], counters[6],
            counters[7], counters[8], counters[14], counters[9],
            counters[12], counters[13], counters[15]);
    }
    inputs.device->unmapBuffer(m_temporalDiagnosticReadback);
    m_temporalDiagnosticReadbackPending = false;
}

void PathTraceUnifiedPtState::DrainTemporalWorkBudgetReadback(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!m_temporalWorkBudgetReadbackPending
        || !m_temporalWorkBudgetReadback || !inputs.device)
        return;
    if (m_temporalWorkBudgetReadbackDelayFrames > 0)
    {
        --m_temporalWorkBudgetReadbackDelayFrames;
        return;
    }
    const uint32_t* words = static_cast<const uint32_t*>(
        inputs.device->mapBuffer(
            m_temporalWorkBudgetReadback, nvrhi::CpuAccessMode::Read));
    if (!words)
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal work budget readback map failed\n");
        m_temporalWorkBudgetReadbackPending = false;
        return;
    }

    static const char* names[UPT07_WORK_BUDGET_COUNTER_COUNT] = {
        "rays", "proceed", "remapLoads", "emissiveHashLoads",
        "lightLoads", "geometryResolves", "materialLoads", "textureSamples"
    };
    static const char* siteNames[UPT07_WORK_BUDGET_SITE_COUNT] = {
        "historyReplay", "reciprocalReplay",
        "finalIndirectVisibility", "finalDirectVisibility"
    };
    static const char* routeNames[UPT07_WORK_BUDGET_ROUTE_COUNT] = {
        "cachedX2Attempted", "cachedX2Admitted",
        "cachedX2Fallback", "cachedX2Winner"
    };
    std::array<uint64_t, UPT07_WORK_BUDGET_COUNTER_COUNT> totals = {};
    std::array<uint32_t, UPT07_WORK_BUDGET_COUNTER_COUNT> maxima = {};
    std::array<uint32_t, UPT07_WORK_BUDGET_COUNTER_COUNT> nonzero = {};
    std::array<std::array<uint32_t, 257>,
        UPT07_WORK_BUDGET_COUNTER_COUNT> histograms = {};
    std::array<uint32_t, 4> violationCounts = {};
    std::array<uint64_t, UPT07_WORK_BUDGET_ROUTE_COUNT> routeTotals = {};
    std::array<uint32_t, UPT07_WORK_BUDGET_ROUTE_COUNT> routeActive = {};
    std::array<std::array<uint64_t, UPT07_WORK_BUDGET_SITE_METRIC_COUNT>,
        UPT07_WORK_BUDGET_SITE_COUNT> siteTotals = {};
    std::array<std::array<uint32_t, UPT07_WORK_BUDGET_SITE_METRIC_COUNT>,
        UPT07_WORK_BUDGET_SITE_COUNT> siteActivePixels = {};
    std::array<std::array<uint32_t, UPT07_WORK_BUDGET_SITE_METRIC_COUNT>,
        UPT07_WORK_BUDGET_SITE_COUNT> siteSaturatedPixels = {};
    uint32_t anyViolationCount = 0u;
    uint64_t logicalDependentLoads = 0u;
    for (uint32_t pixel = 0u; pixel < m_temporalBottleneckCapacity; ++pixel)
    {
        const uint32_t base = pixel * UPT07_WORK_BUDGET_WORDS_PER_PIXEL;
        for (uint32_t counter = 0u;
            counter < UPT07_WORK_BUDGET_COUNTER_COUNT; ++counter)
        {
            const uint32_t value = words[base + counter];
            totals[counter] += value;
            maxima[counter] = Max(maxima[counter], value);
            nonzero[counter] += value != 0u ? 1u : 0u;
            ++histograms[counter][Min(value, 256u)];
            if (counter >= 2u)
                logicalDependentLoads += value;
        }
        for (uint32_t site = 0u; site < UPT07_WORK_BUDGET_SITE_COUNT; ++site)
        {
            const uint32_t packed = words[
                base + UPT07_WORK_BUDGET_SITE_WORD_OFFSET + site];
            for (uint32_t metric = 0u;
                metric < UPT07_WORK_BUDGET_SITE_METRIC_COUNT; ++metric)
            {
                const uint32_t value = (packed >> (metric * 8u)) & 0xffu;
                siteTotals[site][metric] += value;
                siteActivePixels[site][metric] += value != 0u ? 1u : 0u;
                siteSaturatedPixels[site][metric] += value == 0xffu ? 1u : 0u;
            }
        }
        const uint32_t routeBase = base + UPT07_WORK_BUDGET_SITE_WORD_OFFSET
            + UPT07_WORK_BUDGET_SITE_COUNT;
        for (uint32_t route = 0u;
            route < UPT07_WORK_BUDGET_ROUTE_COUNT; ++route)
        {
            const uint32_t value = words[routeBase + route];
            routeTotals[route] += value;
            routeActive[route] += value != 0u ? 1u : 0u;
        }
        const uint32_t violationMask =
            words[base + UPT07_WORK_BUDGET_VIOLATION_WORD];
        anyViolationCount += violationMask != 0u ? 1u : 0u;
        for (uint32_t bit = 0u; bit < violationCounts.size(); ++bit)
            violationCounts[bit] += (violationMask & (1u << bit)) != 0u
                ? 1u : 0u;
    }
    const auto percentile = [&](uint32_t counter, uint32_t numerator)
    {
        const uint64_t threshold =
            (uint64_t(m_temporalBottleneckCapacity) * numerator + 99u) / 100u;
        uint64_t cumulative = 0u;
        for (uint32_t value = 0u; value <= 256u; ++value)
        {
            cumulative += histograms[counter][value];
            if (cumulative >= threshold)
                return value;
        }
        return 256u;
    };
    common->Printf(
        "PathTraceUnifiedPt: temporal work budget status=%s pixels=%u violations(any/rays/remap/hash/light)=%u/%u/%u/%u/%u logicalDependentLoadsPerPixel=%.3f ceilings(rays/remap/hash/light)=3/2/32/8\n",
        anyViolationCount == 0u ? "PASS" : "FAIL",
        m_temporalBottleneckCapacity,
        anyViolationCount,
        violationCounts[0], violationCounts[1], violationCounts[2],
        violationCounts[3],
        m_temporalBottleneckCapacity != 0u
            ? double(logicalDependentLoads)
                / double(m_temporalBottleneckCapacity)
            : 0.0);
    for (uint32_t counter = 0u;
        counter < UPT07_WORK_BUDGET_COUNTER_COUNT; ++counter)
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal work %-19s mean=%8.3f p50=%u p90=%u p99=%u max=%u activePixels=%u\n",
            names[counter],
            m_temporalBottleneckCapacity != 0u
                ? double(totals[counter])
                    / double(m_temporalBottleneckCapacity)
                : 0.0,
            percentile(counter, 50u), percentile(counter, 90u),
            percentile(counter, 99u), maxima[counter], nonzero[counter]);
    }
    for (uint32_t route = 0u;
        route < UPT07_WORK_BUDGET_ROUTE_COUNT; ++route)
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal route %-19s mean=%8.3f total=%llu activePixels=%u\n",
            routeNames[route],
            m_temporalBottleneckCapacity != 0u
                ? double(routeTotals[route])
                    / double(m_temporalBottleneckCapacity)
                : 0.0,
            static_cast<unsigned long long>(routeTotals[route]),
            routeActive[route]);
    }

    static const uint32_t proceedLaneThresholds[] = { 1u, 2u, 4u, 8u, 16u, 32u };
    std::array<uint32_t, UPT07_WORK_BUDGET_SITE_COUNT> rayActiveGroups = {};
    std::array<uint32_t, UPT07_WORK_BUDGET_SITE_COUNT> proceedActiveGroups = {};
    std::array<uint32_t, UPT07_WORK_BUDGET_SITE_COUNT> maxProceedLanes = {};
    std::array<uint32_t, UPT07_WORK_BUDGET_SITE_COUNT> maxProceedIterations = {};
    std::array<uint64_t, UPT07_WORK_BUDGET_SITE_COUNT> totalProceedLanes = {};
    std::array<std::array<uint32_t, 6>,
        UPT07_WORK_BUDGET_SITE_COUNT> proceedLaneGroups = {};
    const uint32_t groupCountX = (inputs.width + 7u) / 8u;
    const uint32_t groupCountY = (inputs.height + 7u) / 8u;
    const uint32_t totalGroupCount = groupCountX * groupCountY;
    for (uint32_t groupY = 0u; groupY < groupCountY; ++groupY)
    {
        for (uint32_t groupX = 0u; groupX < groupCountX; ++groupX)
        {
            std::array<uint32_t, UPT07_WORK_BUDGET_SITE_COUNT> rayLanes = {};
            std::array<uint32_t, UPT07_WORK_BUDGET_SITE_COUNT> proceedLanes = {};
            std::array<uint32_t,
                UPT07_WORK_BUDGET_SITE_COUNT> proceedIterations = {};
            for (uint32_t localY = 0u; localY < 8u; ++localY)
            {
                const uint32_t y = groupY * 8u + localY;
                if (y >= inputs.height)
                    continue;
                for (uint32_t localX = 0u; localX < 8u; ++localX)
                {
                    const uint32_t x = groupX * 8u + localX;
                    if (x >= inputs.width)
                        continue;
                    const uint32_t pixel = y * inputs.width + x;
                    if (pixel >= m_temporalBottleneckCapacity)
                        continue;
                    const uint32_t base =
                        pixel * UPT07_WORK_BUDGET_WORDS_PER_PIXEL;
                    for (uint32_t site = 0u;
                        site < UPT07_WORK_BUDGET_SITE_COUNT; ++site)
                    {
                        const uint32_t packed = words[
                            base + UPT07_WORK_BUDGET_SITE_WORD_OFFSET + site];
                        const uint32_t rays = packed & 0xffu;
                        const uint32_t proceed = (packed >> 8u) & 0xffu;
                        rayLanes[site] += rays != 0u ? 1u : 0u;
                        proceedLanes[site] += proceed != 0u ? 1u : 0u;
                        proceedIterations[site] += proceed;
                    }
                }
            }
            for (uint32_t site = 0u;
                site < UPT07_WORK_BUDGET_SITE_COUNT; ++site)
            {
                rayActiveGroups[site] += rayLanes[site] != 0u ? 1u : 0u;
                proceedActiveGroups[site] += proceedLanes[site] != 0u ? 1u : 0u;
                maxProceedLanes[site] = Max(
                    maxProceedLanes[site], proceedLanes[site]);
                maxProceedIterations[site] = Max(
                    maxProceedIterations[site], proceedIterations[site]);
                totalProceedLanes[site] += proceedLanes[site];
                for (uint32_t threshold = 0u;
                    threshold < proceedLaneGroups[site].size(); ++threshold)
                {
                    proceedLaneGroups[site][threshold] +=
                        proceedLanes[site] >= proceedLaneThresholds[threshold]
                            ? 1u : 0u;
                }
            }
        }
    }
    for (uint32_t site = 0u; site < UPT07_WORK_BUDGET_SITE_COUNT; ++site)
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal site %-24s mean(rays/proceed/geometry/material)=%.3f/%.3f/%.3f/%.3f activePixels=%u/%u/%u/%u groups(ray/proceed/total)=%u/%u/%u proceedLaneGroups(1+/2+/4+/8+/16+/32+)=%u/%u/%u/%u/%u/%u meanLanesPerProceedGroup=%.2f max(lanes/iterations)=%u/%u saturatedPixels=%u/%u/%u/%u\n",
            siteNames[site],
            m_temporalBottleneckCapacity != 0u
                ? double(siteTotals[site][0]) / double(m_temporalBottleneckCapacity)
                : 0.0,
            m_temporalBottleneckCapacity != 0u
                ? double(siteTotals[site][1]) / double(m_temporalBottleneckCapacity)
                : 0.0,
            m_temporalBottleneckCapacity != 0u
                ? double(siteTotals[site][2]) / double(m_temporalBottleneckCapacity)
                : 0.0,
            m_temporalBottleneckCapacity != 0u
                ? double(siteTotals[site][3]) / double(m_temporalBottleneckCapacity)
                : 0.0,
            siteActivePixels[site][0], siteActivePixels[site][1],
            siteActivePixels[site][2], siteActivePixels[site][3],
            rayActiveGroups[site], proceedActiveGroups[site], totalGroupCount,
            proceedLaneGroups[site][0], proceedLaneGroups[site][1],
            proceedLaneGroups[site][2], proceedLaneGroups[site][3],
            proceedLaneGroups[site][4], proceedLaneGroups[site][5],
            proceedActiveGroups[site] != 0u
                ? double(totalProceedLanes[site])
                    / double(proceedActiveGroups[site])
                : 0.0,
            maxProceedLanes[site], maxProceedIterations[site],
            siteSaturatedPixels[site][0], siteSaturatedPixels[site][1],
            siteSaturatedPixels[site][2], siteSaturatedPixels[site][3]);
    }
    inputs.device->unmapBuffer(m_temporalWorkBudgetReadback);
    m_temporalWorkBudgetReadbackPending = false;
}

void PathTraceUnifiedPtState::UpdateTemporalGpuTiming(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const uint32_t requestedMode =
        r_pathTracingUnifiedPtTemporalGpuTiming.GetBool() && inputs.temporal
            ? inputs.temporalBottleneckProbe
            : UINT32_MAX;
    if (requestedMode == m_temporalGpuTimingMode)
        return;

    m_temporalGpuTimingMode = requestedMode;
    m_temporalGpuTimingWarmupRemaining = requestedMode == UINT32_MAX
        ? 0u
        : TEMPORAL_GPU_TIMING_WARMUP_FRAMES;
    m_temporalGpuTimingSubmitted = 0u;
    m_temporalGpuTimingCompleted = 0u;
    m_temporalGpuTimingBatchComplete = false;
    m_temporalGpuTimingSamples.fill(0.0);
    if (requestedMode != UINT32_MAX)
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal GPU timing armed probe=%u warmup=%u samples=%u scope=dispatch-only\n",
            requestedMode,
            TEMPORAL_GPU_TIMING_WARMUP_FRAMES,
            TEMPORAL_GPU_TIMING_SAMPLE_COUNT);
    }
}

void PathTraceUnifiedPtState::PollTemporalGpuTiming(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.device)
        return;

    const int currentFrame = idLib::frameNumber;
    for (TemporalGpuTimerSlot& slot : m_temporalGpuTimers)
    {
        if (!slot.pending || !slot.query
            || currentFrame < slot.earliestPollFrame
            || !inputs.device->pollTimerQuery(slot.query))
        {
            continue;
        }

        if (slot.collect
            && slot.probeMode == m_temporalGpuTimingMode
            && slot.sampleIndex < TEMPORAL_GPU_TIMING_SAMPLE_COUNT
            && !m_temporalGpuTimingBatchComplete)
        {
            m_temporalGpuTimingSamples[slot.sampleIndex] =
                static_cast<double>(inputs.device->getTimerQueryTime(slot.query))
                * 1000.0;
            ++m_temporalGpuTimingCompleted;
        }
        slot.pending = false;
    }

    if (m_temporalGpuTimingBatchComplete
        || m_temporalGpuTimingCompleted != TEMPORAL_GPU_TIMING_SAMPLE_COUNT)
    {
        return;
    }

    std::array<double, TEMPORAL_GPU_TIMING_SAMPLE_COUNT> sorted =
        m_temporalGpuTimingSamples;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (const double sample : sorted)
        sum += sample;
    const double median = 0.5 * (sorted[31] + sorted[32]);
    const double mean = sum / double(TEMPORAL_GPU_TIMING_SAMPLE_COUNT);
    const double p90 = sorted[57];
    common->Printf(
        "PathTraceUnifiedPt: temporal GPU timing probe=%u samples=%u resolution=%ux%u medianMs=%.3f meanMs=%.3f minMs=%.3f p90Ms=%.3f maxMs=%.3f scope=dispatch-only\n",
        m_temporalGpuTimingMode,
        TEMPORAL_GPU_TIMING_SAMPLE_COUNT,
        inputs.width,
        inputs.height,
        median,
        mean,
        sorted.front(),
        p90,
        sorted.back());
    m_temporalGpuTimingBatchComplete = true;
}

nvrhi::TimerQueryHandle PathTraceUnifiedPtState::BeginTemporalGpuTiming(
    const PathTraceUnifiedPtDispatchInputs& inputs,
    bool historyAvailable)
{
    if (m_temporalGpuTimingMode == UINT32_MAX
        || m_temporalGpuTimingBatchComplete
        || !historyAvailable || !inputs.device || !inputs.commandList)
    {
        return nullptr;
    }

    if (m_temporalGpuTimingWarmupRemaining == 0u
        && m_temporalGpuTimingSubmitted >= TEMPORAL_GPU_TIMING_SAMPLE_COUNT)
    {
        return nullptr;
    }

    for (uint32_t slotOffset = 0u;
        slotOffset < TEMPORAL_GPU_TIMER_SLOT_COUNT;
        ++slotOffset)
    {
        const uint32_t slotIndex =
            (m_temporalGpuTimerCursor + slotOffset)
            % TEMPORAL_GPU_TIMER_SLOT_COUNT;
        TemporalGpuTimerSlot& slot = m_temporalGpuTimers[slotIndex];
        if (slot.pending)
            continue;
        if (!slot.query)
            slot.query = inputs.device->createTimerQuery();
        if (!slot.query)
        {
            if (!m_temporalGpuTimingQueryFailureLogged)
            {
                common->Printf(
                    "PathTraceUnifiedPt: temporal GPU timing query creation failed\n");
                m_temporalGpuTimingQueryFailureLogged = true;
            }
            return nullptr;
        }

        slot.pending = true;
        slot.probeMode = m_temporalGpuTimingMode;
        slot.collect = m_temporalGpuTimingWarmupRemaining == 0u;
        if (slot.collect)
            slot.sampleIndex = m_temporalGpuTimingSubmitted++;
        else
        {
            slot.sampleIndex = UINT32_MAX;
            --m_temporalGpuTimingWarmupRemaining;
        }
        slot.earliestPollFrame = idLib::frameNumber + int(NUM_FRAME_DATA);
        m_temporalGpuTimerCursor =
            (slotIndex + 1u) % TEMPORAL_GPU_TIMER_SLOT_COUNT;
        inputs.commandList->beginTimerQuery(slot.query);
        return slot.query;
    }
    return nullptr;
}

bool PathTraceUnifiedPtState::EnsureTemporalBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const bool indirect = r_pathTracingUnifiedPtTemporalIndirect.GetBool();
    if (!inputs.compactPrimaryHistory ||
        !inputs.primarySurfaceCurrentBuffer ||
        !inputs.primarySurfacePreviousBuffer ||
        !inputs.primaryHistorySidecarCurrentBuffer ||
        !inputs.primaryHistorySidecarPreviousBuffer ||
        inputs.primarySurfaceCurrentBuffer->getDesc().structStride !=
            PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER32_STRIDE ||
        inputs.primarySurfacePreviousBuffer->getDesc().structStride !=
            PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER32_STRIDE ||
        inputs.primaryHistorySidecarCurrentBuffer->getDesc().structStride !=
            PATH_TRACE_UNIFIED_PT_PRIMARY_HISTORY_SIDECAR_STRIDE ||
        inputs.primaryHistorySidecarPreviousBuffer->getDesc().structStride !=
            PATH_TRACE_UNIFIED_PT_PRIMARY_HISTORY_SIDECAR_STRIDE)
    {
        common->Printf(
            "PathTraceUnifiedPt: UPT-07 temporal requires compact current/previous 32-byte receivers plus 32-byte history sidecars\n");
        return false;
    }
    const nvrhi::BufferHandle lightBuffer = inputs.compactLights
        ? m_compactLightsBuffer
        : inputs.sceneInputs->lights.restirLightManagerCurrentPayloadBuffer;
    const RtPathTraceSceneInputGeometry& geometry = inputs.sceneInputs->geometry;
    const RtPathTraceSceneInputMaterials& materials = inputs.sceneInputs->materials;
    const RtPathTraceSceneInputLights& lights = inputs.sceneInputs->lights;
    if (!lightBuffer || !CurrentPage() || !HistoryPage() || !geometry.tlas
        || (inputs.duplication && !m_duplicationScores[m_historyPageIndex])
        || !lights.restirLightManagerPreviousToCurrentBuffer
        || !Upt04ReplayGeometryBindingsValid(*inputs.sceneInputs)
        || (indirect && !lights.unifiedPtEmissiveLookupBuffer)
        || !materials.materialTableBuffer || !materials.textureSampler
        || !materials.textureBindlessLayout || !materials.textureDescriptorTable)
    {
        return false;
    }
    if (m_temporalReplayCompaction
        && (!m_temporalReplayQueue || !m_temporalReplayMeta
            || !m_temporalReplayDispatchArgs
            || !m_temporalReplayPipeline
            || !m_temporalReplayBindingLayout))
        return false;

    const uint32_t pageIndex = m_currentPageIndex;
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
        0, geometry.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        1, inputs.primarySurfaceCurrentBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        2, inputs.primarySurfacePreviousBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, CurrentPage()));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, HistoryPage()));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, lightBuffer));
    Upt04AddReplayGeometryBindingItems(desc, *inputs.sceneInputs);
    if (indirect)
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            22, lights.unifiedPtEmissiveLookupBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        23, lights.restirLightManagerPreviousToCurrentBuffer));
    if (inputs.duplication)
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            24, m_duplicationScores[m_historyPageIndex]));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        25, inputs.primaryHistorySidecarCurrentBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        26, inputs.primaryHistorySidecarPreviousBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        27, materials.materialTableBuffer));
    desc.addItem(nvrhi::BindingSetItem::Sampler(28, materials.textureSampler));
    if (m_temporalEarlyReconnect || m_temporalRouteDiagnostics)
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            30, m_temporalDiagnosticCounters));
    if (inputs.temporalBottleneckProbe != 0u)
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            31, m_temporalBottleneckBuffer));
    if (m_temporalReplayCompaction)
    {
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            33, m_temporalReplayQueue));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            34, m_temporalReplayMeta));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            35, m_temporalReplayDispatchArgs));
    }
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT07_PUSH_CONSTANT_BYTES));
    if (m_temporalBindingSets[pageIndex] &&
        m_temporalBindingSetDescValid[pageIndex] &&
        m_temporalBindingSetDescs[pageIndex] == desc
        && (!m_temporalReplayCompaction
            || (m_temporalReplayBindingSets[pageIndex]
                && m_temporalReplayBindingSetDescValid[pageIndex])))
    {
        return true;
    }
    m_temporalBindingSets[pageIndex] = inputs.device->createBindingSet(
        desc, m_temporalBindingLayout);
    if (!m_temporalBindingSets[pageIndex])
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-07 temporal binding set\n");
        return false;
    }
    m_temporalBindingSetDescs[pageIndex] = desc;
    m_temporalBindingSetDescValid[pageIndex] = true;

    if (m_temporalReplayCompaction)
    {
        nvrhi::BindingSetDesc replayDesc;
        replayDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
            0, geometry.tlas));
        replayDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            1, inputs.primarySurfaceCurrentBuffer));
        replayDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            2, inputs.primarySurfacePreviousBuffer));
        replayDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            3, CurrentPage()));
        replayDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            4, HistoryPage()));
        replayDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            5, lightBuffer));
        Upt04AddReplayGeometryBindingItems(
            replayDesc, *inputs.sceneInputs);
        replayDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            22, lights.unifiedPtEmissiveLookupBuffer));
        replayDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            23, lights.restirLightManagerPreviousToCurrentBuffer));
        replayDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            24, m_duplicationScores[m_historyPageIndex]));
        replayDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            25, inputs.primaryHistorySidecarCurrentBuffer));
        replayDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            26, inputs.primaryHistorySidecarPreviousBuffer));
        replayDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            27, materials.materialTableBuffer));
        replayDesc.addItem(nvrhi::BindingSetItem::Sampler(
            28, materials.textureSampler));
        replayDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            33, m_temporalReplayQueue));
        replayDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            34, m_temporalReplayMeta));
        replayDesc.addItem(nvrhi::BindingSetItem::PushConstants(
            0, UPT07_PUSH_CONSTANT_BYTES));
        m_temporalReplayBindingSets[pageIndex] =
            inputs.device->createBindingSet(
                replayDesc, m_temporalReplayBindingLayout);
        if (!m_temporalReplayBindingSets[pageIndex])
        {
            common->Printf(
                "PathTraceUnifiedPt: failed to create UPT-43.4 compact replay binding set\n");
            return false;
        }
        m_temporalReplayBindingSetDescs[pageIndex] = replayDesc;
        m_temporalReplayBindingSetDescValid[pageIndex] = true;
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureTemporalBoilingFilterBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!m_temporalBoilingFilter)
        return true;
    if (!CurrentPage() || !m_temporalBoilingFilterPipeline
        || !m_temporalBoilingFilterBindingLayout)
        return false;

    const uint32_t pageIndex = m_currentPageIndex;
    if (m_temporalBoilingFilterBindingSets[pageIndex])
        return true;

    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
        0, CurrentPage()));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT43_TEMPORAL_BOILING_PUSH_CONSTANT_BYTES));
    m_temporalBoilingFilterBindingSets[pageIndex] =
        inputs.device->createBindingSet(
            desc, m_temporalBoilingFilterBindingLayout);
    if (!m_temporalBoilingFilterBindingSets[pageIndex])
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-43.6 boiling-filter binding set\n");
        return false;
    }
    return true;
}

bool PathTraceUnifiedPtState::ExecuteTemporal(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    UpdateTemporalGpuTiming(inputs);
    PollTemporalGpuTiming(inputs);
    DrainTemporalDiagnosticReadback(inputs);
    DrainTemporalWorkBudgetReadback(inputs);
    DrainTemporalReplayCompactionReadback(inputs);
    if (!r_pathTracingUnifiedPtTemporalReplayCompactionDiagnostics.GetBool())
        m_temporalReplayDiagnosticCaptured = false;
    if (!inputs.temporal)
    {
        m_reportedTemporalSkipReason = -1;
        return false;
    }
    const auto reportSkip = [&](int32_t reason, const char* label) -> bool
    {
        if (m_reportedTemporalSkipReason != reason)
        {
            const PathTraceUnifiedPtPageMetadata& current =
                CurrentPageMetadata();
            const PathTraceUnifiedPtPageMetadata& history =
                HistoryPageMetadata();
            common->Printf(
                "PathTraceUnifiedPt: temporal skipped reason=%s sampleIndex=%u currentPage=%u serial=%llu historyPage=%u serial=%llu\n",
                label,
                inputs.frameSampleIndex,
                m_currentPageIndex,
                static_cast<unsigned long long>(current.frameSerial),
                m_historyPageIndex,
                static_cast<unsigned long long>(history.frameSerial));
            m_reportedTemporalSkipReason = reason;
        }
        return false;
    };
    const bool productionFullFrame = inputs.proofStage >= 9u &&
        Upt04PipelineVariant(inputs) == 0u;
    const PathTraceUnifiedPtPageMetadata& currentMetadata = CurrentPageMetadata();
    if (!productionFullFrame)
        return reportSkip(1, "not-production-full-frame");
    if (!currentMetadata.fullyWritten)
        return reportSkip(2, "current-page-not-published");
    if (currentMetadata.width != inputs.width ||
        currentMetadata.height != inputs.height)
        return reportSkip(3, "current-page-extent-mismatch");
    if (currentMetadata.historyEpoch != inputs.historyEpoch)
        return reportSkip(4, "current-page-epoch-mismatch");
    if (inputs.duplication && !EnsureDuplicationResources(inputs))
        return reportSkip(8, "duplication-resources-unavailable");
    if (r_pathTracingUnifiedPtTemporalIndirect.GetBool()
        && (r_pathTracingUnifiedPtTemporalEarlyReconnect.GetBool()
            || r_pathTracingUnifiedPtTemporalRouteDiagnostics.GetBool())
        && !EnsureTemporalDiagnosticBuffers(inputs))
        return reportSkip(9, "temporal-diagnostics-unavailable");
    if (!EnsureTemporalPipeline(inputs))
        return reportSkip(5, "pipeline-unavailable");
    if (!EnsureTemporalBottleneckBuffer(inputs))
        return reportSkip(10, "bottleneck-probe-buffer-unavailable");
    if (!EnsureTemporalReplayCompactionBuffers(inputs))
        return reportSkip(11, "replay-compaction-buffer-unavailable");
    if (!EnsureTemporalBindingSet(inputs))
        return reportSkip(6, "binding-set-unavailable");
    if (!EnsureTemporalBoilingFilterBindingSet(inputs))
        return reportSkip(12, "boiling-filter-binding-set-unavailable");

    const PathTraceUnifiedPtPageMetadata& historyMetadata = HistoryPageMetadata();
    const bool historyAvailable = inputs.primarySurfaceHistoryValid &&
        historyMetadata.fullyWritten &&
        historyMetadata.width == currentMetadata.width &&
        historyMetadata.height == currentMetadata.height &&
        historyMetadata.contentGeneration == currentMetadata.contentGeneration &&
        historyMetadata.historyEpoch == currentMetadata.historyEpoch &&
        currentMetadata.frameSerial > 1u &&
        historyMetadata.frameSerial == currentMetadata.frameSerial - 1u;
    const PathTraceUnifiedPtPageMetadata& duplicationMetadata =
        m_duplicationMetadata[m_historyPageIndex];
    const bool duplicationAvailable = inputs.duplication && historyAvailable
        && duplicationMetadata.fullyWritten
        && duplicationMetadata.width == historyMetadata.width
        && duplicationMetadata.height == historyMetadata.height
        && duplicationMetadata.contentGeneration == historyMetadata.contentGeneration
        && duplicationMetadata.historyEpoch == historyMetadata.historyEpoch
        && duplicationMetadata.frameSerial == historyMetadata.frameSerial;
    if (m_reportedTemporalHistoryAvailable != (historyAvailable ? 1 : 0))
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal historyAvailable=%d currentPage=%u historyPage=%u pageGeneration=%016llx epoch=%llu serial(current/history)=%llu/%llu\n",
            historyAvailable ? 1 : 0,
            m_currentPageIndex,
            m_historyPageIndex,
            static_cast<unsigned long long>(currentMetadata.contentGeneration),
            static_cast<unsigned long long>(currentMetadata.historyEpoch),
            static_cast<unsigned long long>(currentMetadata.frameSerial),
            static_cast<unsigned long long>(historyMetadata.frameSerial));
        m_reportedTemporalHistoryAvailable = historyAvailable ? 1 : 0;
    }
    const RtPathTraceSceneInputLights& lights = inputs.sceneInputs->lights;
    const RtPathTraceSceneInputGeometry& geometry = inputs.sceneInputs->geometry;
    const uint64_t surfaceCount64 = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (surfaceCount64 > std::numeric_limits<uint32_t>::max())
    {
        return reportSkip(7, "surface-count-overflow");
    }

    Upt07TemporalDirectControl control = {};
    control.renderWidth = inputs.width;
    control.renderHeight = inputs.height;
    control.surfaceCount = static_cast<uint32_t>(surfaceCount64);
    control.historyAvailable = historyAvailable ? 1u : 0u;
    control.emissiveRangeStart = inputs.frozenLightDiagnostic
        ? 0u : lights.restirLightManagerEmissiveRangeOffset;
    control.emissiveRangeCount = inputs.frozenLightDiagnostic
        ? 0u : lights.restirLightManagerEmissiveRangeCount;
    control.analyticRangeStart = lights.restirLightManagerDoomAnalyticRangeOffset;
    control.analyticRangeCount = lights.restirLightManagerDoomAnalyticSampleableCount;
    control.currentLightCount = static_cast<uint32_t>(Max(
        0, lights.restirLightManagerCurrentPayloadCount));
    control.frameSampleIndex = inputs.frameSampleIndex;
    control.maximumHistoryM = static_cast<uint32_t>(idMath::ClampInt(
        1,
        1024,
        r_pathTracingUnifiedPtTemporalMaxHistoryM.GetInteger()));
    control.maximumHistoryAge = static_cast<uint32_t>(idMath::ClampInt(
        1,
        static_cast<int>(UPT07_MAXIMUM_HISTORY_AGE),
        r_pathTracingUnifiedPtTemporalMaxAge.GetInteger()));
    for (uint32_t axis = 0; axis < 3u; ++axis)
    {
        control.previousCameraOrigin[axis] = inputs.previousCameraOrigin[axis];
        control.previousCameraForward[axis] = inputs.previousCameraForward[axis];
        control.previousCameraLeft[axis] = inputs.previousCameraLeft[axis];
        control.previousCameraUp[axis] = inputs.previousCameraUp[axis];
    }
    control.previousCameraValid = historyAvailable ? 1u : 0u;
    control.previousCameraTanX = inputs.previousCameraTanX;
    control.previousCameraTanY = inputs.previousCameraTanY;
    control.previousCameraHistorySearchMode = static_cast<uint32_t>(
        idMath::ClampInt(
            0, 2, r_pathTracingUnifiedPtTemporalSearch.GetInteger()));
    control.previousCameraJitterPixels[0] =
        inputs.previousCameraJitterPixels[0];
    control.previousCameraJitterPixels[1] =
        inputs.previousCameraJitterPixels[1];
    control.geometryAvailabilityFlags =
        (!inputs.frozenStaticDiagnostic &&
                geometry.staticBucketRoutePublicationValid
            ? UPT04_ROUTE_STATIC_BUCKETS : 0u)
        | ((static_cast<uint32_t>(idMath::ClampInt(
                1,
                UPT04_NEE_RIS_MAX_EMISSIVE_CANDIDATE_COUNT,
                r_pathTracingReservoirCandidateTrials.GetInteger()))
                << UPT04_EMISSIVE_TRIAL_COUNT_SHIFT)
            & UPT04_EMISSIVE_TRIAL_COUNT_MASK)
        | (r_pathTracingUnifiedPtTemporalPreviousBest.GetBool()
            ? UPT07_GEOMETRY_FLAG_PREVIOUS_BEST_SEED : 0u)
        | (r_pathTracingUnifiedPtTemporalPairwise.GetBool()
            ? UPT07_GEOMETRY_FLAG_PAIRWISE_MIS : 0u)
        | (duplicationAvailable
            ? UPT07_GEOMETRY_FLAG_DUPLICATION_MAP : 0u)
        | (r_pathTracingUnifiedPtTemporalIndirect.GetBool()
            ? UPT07_GEOMETRY_FLAG_INDIRECT_REPLAY : 0u)
        | (r_pathTracingUnifiedPtTemporalEarlyReconnect.GetBool()
            ? UPT07_GEOMETRY_FLAG_EARLY_RECONNECT : 0u)
        | (r_pathTracingUnifiedPtTemporalReconnectDiagnostics.GetBool()
            ? UPT07_GEOMETRY_FLAG_RECONNECT_DIAGNOSTICS : 0u)
        | (r_pathTracingUnifiedPtTemporalRouteDiagnostics.GetBool()
            ? UPT07_GEOMETRY_FLAG_ROUTE_DIAGNOSTICS : 0u)
        | (r_pathTracingUnifiedPtTemporalPermutationSampling.GetBool()
            ? UPT07_GEOMETRY_FLAG_TEMPORAL_PERMUTATION : 0u)
        | (r_pathTracingUnifiedPtDirectTargetPdfParity.GetBool()
            ? UPT04_DIRECT_TARGET_PDF_PARITY : 0u)
        | (r_pathTracingUnifiedPtAnalyticPortalDomain.GetBool()
            ? UPT04_ANALYTIC_PORTAL_DOMAIN : 0u)
        | (r_pathTracingReservoirTwoSidedEmissives.GetBool()
            ? UPT04_TWO_SIDED_EMISSIVES : 0u)
        | (lights.unifiedPtEmissiveLookupExact
            ? UPT04_EMISSIVE_LOOKUP_EXACT : 0u)
        | ((inputs.materialPolicyFlags
                & PATH_TRACE_UPT_MATERIAL_USE_SPECULAR_MAPS) != 0u
            ? UPT04_MATERIAL_USE_SPECULAR_MAPS : 0u)
        | ((inputs.materialPolicyFlags
                & PATH_TRACE_UPT_MATERIAL_LEGACY_SPECMAP_TO_PBR) != 0u
            ? UPT04_MATERIAL_LEGACY_SPECMAP_TO_PBR : 0u)
        | ((inputs.materialPolicyFlags
                & PATH_TRACE_UPT_MATERIAL_DECODE_TEXTURES) != 0u
            ? UPT04_MATERIAL_DECODE_TEXTURES : 0u);
    control.emissiveScale = Max(0.0f, inputs.emissiveScale);
    control.previousToCurrentLightCount = inputs.frozenLightDiagnostic
        ? control.currentLightCount
        : static_cast<uint32_t>(Max(
            0, lights.restirLightManagerPreviousToCurrentCount));
    control.maximumHistoryContributionRatio =
        UPT07_MAXIMUM_HISTORY_CONTRIBUTION_RATIO;
    for (uint32_t axis = 0; axis < 3u; ++axis)
        control.primaryCameraOrigin[axis] = inputs.primaryCameraOrigin[axis];
    control.compactPrimaryHistory = inputs.compactPrimaryHistory ? 1u : 0u;
    control.materialCount = static_cast<uint32_t>(Max(
        0, inputs.sceneInputs->materials.materialTableEntryCount));
    control.logicalTextureCount = static_cast<uint32_t>(Max(
        0, inputs.sceneInputs->materials.logicalTextureDescriptorCount));
    const uint32_t distributionCount = inputs.frozenLightDiagnostic
        ? 0u : static_cast<uint32_t>(Max(
            0, lights.emissiveDistributionCount));
    control.emissiveDistributionCountAndValid =
        (distributionCount & UPT04_CONTROL_METADATA_COUNT_MASK)
        | (lights.emissiveDistributionValid && distributionCount != 0u
            ? UPT04_CONTROL_METADATA_VALID_BIT : 0u);
    const uint32_t lookupCapacity = inputs.frozenLightDiagnostic
        ? 0u : static_cast<uint32_t>(Max(
            0, lights.unifiedPtEmissiveLookupCount));
    control.emissiveLookupCapacityAndValid =
        (lookupCapacity & UPT04_CONTROL_METADATA_COUNT_MASK)
        | (lights.unifiedPtEmissiveLookupExact && lookupCapacity >= 2u
            ? UPT04_CONTROL_METADATA_VALID_BIT : 0u);

    const nvrhi::BufferHandle lightBuffer = inputs.compactLights
        ? m_compactLightsBuffer
        : lights.restirLightManagerCurrentPayloadBuffer;
    const bool captureReconnectDiagnostics = m_temporalEarlyReconnect
        && r_pathTracingUnifiedPtTemporalReconnectDiagnostics.GetBool()
        && !m_temporalDiagnosticReadbackPending;
    const bool captureRouteDiagnostics = m_temporalRouteDiagnostics
        && r_pathTracingUnifiedPtTemporalRouteDiagnostics.GetBool()
        && !m_temporalDiagnosticReadbackPending;
    const bool captureTemporalDiagnostics = captureReconnectDiagnostics
        || captureRouteDiagnostics;
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            historyAvailable
                ? (duplicationAvailable
                    ? (m_temporalIndirect
                        ? "UPT.T0 Temporal Unified Shift AdaptiveDupCap"
                        : "UPT.T0 Temporal Direct Shift AdaptiveDupCap")
                    : (m_temporalIndirect
                        ? "UPT.T0 Temporal Unified Shift History"
                        : "UPT.T0 Temporal Direct Shift History"))
                : (m_temporalIndirect
                    ? "UPT.T0 Temporal Unified Shift NoHistory"
                    : "UPT.T0 Temporal Direct Shift NoHistory"),
            inputs.nsightMarkers);
        inputs.commandList->setAccelStructState(
            inputs.sceneInputs->geometry.tlas,
            nvrhi::ResourceStates::AccelStructRead);
        inputs.commandList->setBufferState(
            inputs.primarySurfaceCurrentBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            inputs.primarySurfacePreviousBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            inputs.primaryHistorySidecarCurrentBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            inputs.primaryHistorySidecarPreviousBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            CurrentPage(), nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->setBufferState(
            HistoryPage(), nvrhi::ResourceStates::ShaderResource);
        if (inputs.duplication)
            inputs.commandList->setBufferState(
                m_duplicationScores[m_historyPageIndex],
                nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            lightBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            lights.restirLightManagerPreviousToCurrentBuffer,
            nvrhi::ResourceStates::ShaderResource);
        Upt04SetReplayGeometrySrvStates(
            inputs.commandList, *inputs.sceneInputs);
        inputs.commandList->setBufferState(
            lights.unifiedPtEmissiveLookupBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            inputs.sceneInputs->materials.materialTableBuffer,
            nvrhi::ResourceStates::ShaderResource);
        if (m_temporalEarlyReconnect || m_temporalRouteDiagnostics)
            inputs.commandList->setBufferState(
                m_temporalDiagnosticCounters,
                nvrhi::ResourceStates::UnorderedAccess);
        if (inputs.temporalBottleneckProbe != 0u)
            inputs.commandList->setBufferState(
                m_temporalBottleneckBuffer,
                nvrhi::ResourceStates::UnorderedAccess);
        if (m_temporalReplayCompaction)
        {
            const uint32_t replayArgs[3] = { 0u, 1u, 1u };
            const uint32_t replayMeta[2] = {
                0u, m_temporalReplayCapacity };
            // Reset only the 20 bytes that own count/capacity/indirect args.
            // Queue contents are bounded by the freshly published count and
            // are deliberately never cleared.
            inputs.commandList->writeBuffer(
                m_temporalReplayDispatchArgs,
                replayArgs, sizeof(replayArgs));
            inputs.commandList->writeBuffer(
                m_temporalReplayMeta,
                replayMeta, sizeof(replayMeta));
            inputs.commandList->setBufferState(
                m_temporalReplayQueue,
                nvrhi::ResourceStates::UnorderedAccess);
            inputs.commandList->setBufferState(
                m_temporalReplayMeta,
                nvrhi::ResourceStates::UnorderedAccess);
            inputs.commandList->setBufferState(
                m_temporalReplayDispatchArgs,
                nvrhi::ResourceStates::UnorderedAccess);
        }
        inputs.commandList->commitBarriers();
        if (captureTemporalDiagnostics)
        {
            inputs.commandList->clearBufferUInt(
                m_temporalDiagnosticCounters, 0u);
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, m_temporalDiagnosticCounters);
        }

        nvrhi::ComputeState state;
        state.pipeline = m_temporalPipeline;
        state.bindings = {
            m_temporalBindingSets[m_currentPageIndex],
            inputs.sceneInputs->materials.textureDescriptorTable };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
        const nvrhi::TimerQueryHandle temporalGpuTimer =
            BeginTemporalGpuTiming(inputs, historyAvailable);
        inputs.commandList->dispatch(
            (inputs.width + 7u) / 8u,
            (inputs.height + 7u) / 8u,
            1u);
        if (m_temporalReplayCompaction)
        {
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, CurrentPage());
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, m_temporalReplayQueue);
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, m_temporalReplayMeta);
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, m_temporalReplayDispatchArgs);
            inputs.commandList->setBufferState(
                m_temporalReplayQueue,
                nvrhi::ResourceStates::ShaderResource);
            inputs.commandList->setBufferState(
                m_temporalReplayMeta,
                nvrhi::ResourceStates::ShaderResource);
            inputs.commandList->setBufferState(
                m_temporalReplayDispatchArgs,
                nvrhi::ResourceStates::IndirectArgument);
            inputs.commandList->commitBarriers();

            Upt04MarkerScope replayMarker(
                inputs.commandList,
                "UPT.T0b Temporal Compact Exact Replay",
                inputs.nsightMarkers);
            nvrhi::ComputeState replayState;
            replayState.pipeline = m_temporalReplayPipeline;
            replayState.bindings = {
                m_temporalReplayBindingSets[m_currentPageIndex],
                inputs.sceneInputs->materials.textureDescriptorTable };
            replayState.indirectParams = m_temporalReplayDispatchArgs;
            inputs.commandList->setComputeState(replayState);
            inputs.commandList->setPushConstants(&control, sizeof(control));
            inputs.commandList->dispatchIndirect(0u);
        }
        if (m_temporalBoilingFilter)
        {
            // T0b may have replaced queued D0 records. The boiling decision
            // must observe the completed page, so it cannot live in T0a.
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, CurrentPage());
            inputs.commandList->commitBarriers();

            Upt04MarkerScope boilingMarker(
                inputs.commandList,
                "UPT.T0c Temporal Boiling Filter",
                inputs.nsightMarkers);
            nvrhi::ComputeState boilingState;
            boilingState.pipeline = m_temporalBoilingFilterPipeline;
            boilingState.bindings = {
                m_temporalBoilingFilterBindingSets[m_currentPageIndex] };
            inputs.commandList->setComputeState(boilingState);
            Upt43TemporalBoilingControl boilingControl = {};
            boilingControl.renderWidth = inputs.width;
            boilingControl.renderHeight = inputs.height;
            boilingControl.surfaceCount =
                static_cast<uint32_t>(surfaceCount64);
            boilingControl.strength = idMath::ClampFloat(
                1.0e-6f, 1.0f,
                r_pathTracingUnifiedPtTemporalBoilingFilterStrength.GetFloat());
            inputs.commandList->setPushConstants(
                &boilingControl, sizeof(boilingControl));
            inputs.commandList->dispatch(
                (inputs.width + 7u) / 8u,
                (inputs.height + 7u) / 8u,
                1u);
        }
        if (temporalGpuTimer)
            inputs.commandList->endTimerQuery(temporalGpuTimer);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, CurrentPage());
        if (m_temporalReplayCompaction
            && r_pathTracingUnifiedPtTemporalReplayCompactionDiagnostics.GetBool()
            && !m_temporalReplayReadbackPending
            && !m_temporalReplayDiagnosticCaptured)
        {
            inputs.commandList->setBufferState(
                m_temporalReplayDispatchArgs,
                nvrhi::ResourceStates::CopySource);
            inputs.commandList->setBufferState(
                m_temporalReplayMeta,
                nvrhi::ResourceStates::CopySource);
            inputs.commandList->setBufferState(
                m_temporalReplayReadback,
                nvrhi::ResourceStates::CopyDest);
            inputs.commandList->commitBarriers();
            inputs.commandList->copyBuffer(
                m_temporalReplayReadback, 0u,
                m_temporalReplayDispatchArgs, 0u,
                3u * sizeof(uint32_t));
            inputs.commandList->copyBuffer(
                m_temporalReplayReadback, 3u * sizeof(uint32_t),
                m_temporalReplayMeta, 0u,
                2u * sizeof(uint32_t));
            m_temporalReplayReadbackPending = true;
            m_temporalReplayDiagnosticCaptured = true;
            m_temporalReplayReadbackDelayFrames = 2;
        }
        if (captureTemporalDiagnostics)
        {
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, m_temporalDiagnosticCounters);
            inputs.commandList->setBufferState(
                m_temporalDiagnosticCounters,
                nvrhi::ResourceStates::CopySource);
            inputs.commandList->setBufferState(
                m_temporalDiagnosticReadback,
                nvrhi::ResourceStates::CopyDest);
            inputs.commandList->commitBarriers();
            inputs.commandList->copyBuffer(
                m_temporalDiagnosticReadback,
                0,
                m_temporalDiagnosticCounters,
                0,
                UPT07_TEMPORAL_DIAGNOSTIC_BYTES);
            m_temporalDiagnosticReadbackPending = true;
            m_temporalDiagnosticReadbackIsRoute = captureRouteDiagnostics;
            m_temporalDiagnosticReadbackDelayFrames = 2;
        }
        if (inputs.temporalBottleneckProbe == 12u
            && historyAvailable
            && m_temporalWorkBudgetCaptureArmed
            && !m_temporalWorkBudgetReadbackPending)
        {
            if (m_temporalWorkBudgetWarmupFrames > 0u)
            {
                --m_temporalWorkBudgetWarmupFrames;
            }
            else
            {
                const uint64_t workBudgetBytes =
                    uint64_t(m_temporalBottleneckCapacity)
                    * UPT07_WORK_BUDGET_WORDS_PER_PIXEL * sizeof(uint32_t);
                nvrhi::utils::BufferUavBarrier(
                    inputs.commandList, m_temporalBottleneckBuffer);
                inputs.commandList->setBufferState(
                    m_temporalBottleneckBuffer,
                    nvrhi::ResourceStates::CopySource);
                inputs.commandList->setBufferState(
                    m_temporalWorkBudgetReadback,
                    nvrhi::ResourceStates::CopyDest);
                inputs.commandList->commitBarriers();
                inputs.commandList->copyBuffer(
                    m_temporalWorkBudgetReadback, 0,
                    m_temporalBottleneckBuffer, 0, workBudgetBytes);
                m_temporalWorkBudgetReadbackPending = true;
                m_temporalWorkBudgetReadbackDelayFrames = 2;
                m_temporalWorkBudgetCaptureArmed = false;
            }
        }
    }
    m_reportedTemporalSkipReason = -1;
    return true;
}

void PathTraceUnifiedPtState::UpdateSpatialGpuTiming(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    uint32_t requestedMode = UINT32_MAX;
    if (r_pathTracingUnifiedPtSpatialGpuTiming.GetBool() && inputs.spatial)
    {
        const bool sharedSpatial = Upt30SharedSpatialEnabled(inputs);
        const bool storedSourceTarget = sharedSpatial
            && r_pathTracingUnifiedPtSpatialStoredSourceTarget.GetBool();
        const bool workgroupPairing = storedSourceTarget
            && r_pathTracingUnifiedPtSpatialWorkgroupPairing.GetBool();
        const bool emptyRescue = workgroupPairing
            && r_pathTracingUnifiedPtSpatialEmptyRescue.GetBool();
        const bool multiNeighbor = emptyRescue
            && r_pathTracingUnifiedPtSpatialMultiNeighbor.GetBool();
        const bool disocclusionBoost = Upt438SpatialDisocclusionBoostEnabled(
            inputs);
        const uint32_t proofMode = static_cast<uint32_t>(idMath::ClampInt(
            0, 6, r_pathTracingUnifiedPtSpatialProofMode.GetInteger()));
        const bool fixedPhase =
            r_pathTracingUnifiedPtFixedSampleIndex.GetInteger() >= 0;
        const uint32_t phaseState = fixedPhase
            ? (inputs.frameSampleIndex & 3u) : 4u;
        requestedMode = (sharedSpatial ? 1u : 0u)
            | (storedSourceTarget ? 2u : 0u)
            | (workgroupPairing ? 4u : 0u)
            | (emptyRescue ? 8u : 0u)
            | (proofMode << 4u)
            | (phaseState << 8u)
            | (multiNeighbor ? 0x800u : 0u)
            | (disocclusionBoost ? 0x1000u : 0u);
    }
    if (requestedMode == m_spatialGpuTimingMode)
        return;

    m_spatialGpuTimingMode = requestedMode;
    m_spatialGpuTimingWarmupRemaining = requestedMode == UINT32_MAX
        ? 0u : SPATIAL_GPU_TIMING_WARMUP_FRAMES;
    m_spatialGpuTimingSubmitted = 0u;
    m_spatialGpuTimingCompleted = 0u;
    m_spatialGpuTimingBatchComplete = false;
    m_spatialGpuTimingSamples.fill(0.0);
    if (requestedMode != UINT32_MAX)
    {
        const uint32_t phaseState = (requestedMode >> 8u) & 7u;
        common->Printf(
            "PathTraceUnifiedPt: spatial GPU timing armed shared=%u storedSourceTarget=%u workgroupPairing=%u emptyRescue=%u multiNeighbor=%u disocclusionBoost=%u proof=%u phase=%s warmup=%u samples=%u scope=dispatch-only\n",
            requestedMode & 1u,
            (requestedMode >> 1u) & 1u,
            (requestedMode >> 2u) & 1u,
            (requestedMode >> 3u) & 1u,
            (requestedMode >> 11u) & 1u,
            (requestedMode >> 12u) & 1u,
            (requestedMode >> 4u) & 0xfu,
            phaseState < 4u ? va("%u", phaseState) : "mixed",
            SPATIAL_GPU_TIMING_WARMUP_FRAMES,
            SPATIAL_GPU_TIMING_SAMPLE_COUNT);
    }
}

void PathTraceUnifiedPtState::PollSpatialGpuTiming(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.device)
        return;

    const int currentFrame = idLib::frameNumber;
    for (SpatialGpuTimerSlot& slot : m_spatialGpuTimers)
    {
        if (!slot.pending || !slot.query
            || currentFrame < slot.earliestPollFrame
            || !inputs.device->pollTimerQuery(slot.query))
        {
            continue;
        }
        if (slot.collect && slot.mode == m_spatialGpuTimingMode
            && slot.sampleIndex < SPATIAL_GPU_TIMING_SAMPLE_COUNT
            && !m_spatialGpuTimingBatchComplete)
        {
            m_spatialGpuTimingSamples[slot.sampleIndex] =
                static_cast<double>(inputs.device->getTimerQueryTime(slot.query))
                * 1000.0;
            ++m_spatialGpuTimingCompleted;
        }
        slot.pending = false;
    }

    if (m_spatialGpuTimingBatchComplete
        || m_spatialGpuTimingCompleted != SPATIAL_GPU_TIMING_SAMPLE_COUNT)
    {
        return;
    }

    std::array<double, SPATIAL_GPU_TIMING_SAMPLE_COUNT> sorted =
        m_spatialGpuTimingSamples;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (const double sample : sorted)
        sum += sample;
    const double median = 0.5 * (sorted[31] + sorted[32]);
    const double mean = sum / double(SPATIAL_GPU_TIMING_SAMPLE_COUNT);
    const double p90 = sorted[57];
    const uint32_t phaseState = (m_spatialGpuTimingMode >> 8u) & 7u;
    common->Printf(
        "PathTraceUnifiedPt: spatial GPU timing shared=%u storedSourceTarget=%u workgroupPairing=%u emptyRescue=%u multiNeighbor=%u disocclusionBoost=%u proof=%u phase=%s samples=%u resolution=%ux%u medianMs=%.3f meanMs=%.3f minMs=%.3f p90Ms=%.3f maxMs=%.3f scope=dispatch-only\n",
        m_spatialGpuTimingMode & 1u,
        (m_spatialGpuTimingMode >> 1u) & 1u,
        (m_spatialGpuTimingMode >> 2u) & 1u,
        (m_spatialGpuTimingMode >> 3u) & 1u,
        (m_spatialGpuTimingMode >> 11u) & 1u,
        (m_spatialGpuTimingMode >> 12u) & 1u,
        (m_spatialGpuTimingMode >> 4u) & 0xfu,
        phaseState < 4u ? va("%u", phaseState) : "mixed",
        SPATIAL_GPU_TIMING_SAMPLE_COUNT,
        inputs.width,
        inputs.height,
        median,
        mean,
        sorted.front(),
        p90,
        sorted.back());
    m_spatialGpuTimingBatchComplete = true;
}

nvrhi::TimerQueryHandle PathTraceUnifiedPtState::BeginSpatialGpuTiming(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (m_spatialGpuTimingMode == UINT32_MAX
        || m_spatialGpuTimingBatchComplete
        || !inputs.device || !inputs.commandList)
    {
        return nullptr;
    }
    if (m_spatialGpuTimingWarmupRemaining == 0u
        && m_spatialGpuTimingSubmitted >= SPATIAL_GPU_TIMING_SAMPLE_COUNT)
    {
        return nullptr;
    }

    for (uint32_t slotOffset = 0u;
        slotOffset < SPATIAL_GPU_TIMER_SLOT_COUNT;
        ++slotOffset)
    {
        const uint32_t slotIndex = (m_spatialGpuTimerCursor + slotOffset)
            % SPATIAL_GPU_TIMER_SLOT_COUNT;
        SpatialGpuTimerSlot& slot = m_spatialGpuTimers[slotIndex];
        if (slot.pending)
            continue;
        if (!slot.query)
            slot.query = inputs.device->createTimerQuery();
        if (!slot.query)
        {
            if (!m_spatialGpuTimingQueryFailureLogged)
            {
                common->Printf(
                    "PathTraceUnifiedPt: spatial GPU timing query creation failed\n");
                m_spatialGpuTimingQueryFailureLogged = true;
            }
            return nullptr;
        }

        slot.pending = true;
        slot.mode = m_spatialGpuTimingMode;
        slot.collect = m_spatialGpuTimingWarmupRemaining == 0u;
        if (slot.collect)
            slot.sampleIndex = m_spatialGpuTimingSubmitted++;
        else
        {
            slot.sampleIndex = UINT32_MAX;
            --m_spatialGpuTimingWarmupRemaining;
        }
        slot.earliestPollFrame = idLib::frameNumber + int(NUM_FRAME_DATA);
        m_spatialGpuTimerCursor =
            (slotIndex + 1u) % SPATIAL_GPU_TIMER_SLOT_COUNT;
        inputs.commandList->beginTimerQuery(slot.query);
        return slot.query;
    }
    return nullptr;
}

bool PathTraceUnifiedPtState::EnsureSpatialReuseTextureResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const uint64_t bytes = uint64_t(UPT42_REUSE_TEXTURE_ENTRY_COUNT)
        * UPT42_REUSE_TEXTURE_STRIDE;
    if (m_spatialReuseTextureBuffer
        && m_spatialReuseTextureBuffer->getDesc().structStride
            == UPT42_REUSE_TEXTURE_STRIDE
        && m_spatialReuseTextureBuffer->getDesc().byteSize == bytes)
    {
        return true;
    }

    std::vector<uint32_t> packedDeltas;
    packedDeltas.reserve(UPT42_REUSE_TEXTURE_DELTA_ENTRY_COUNT);
    std::array<Upt42ReuseTextureStats, 3> stats = {};
    for (uint32_t ordinal = 0u;
         ordinal < UPT42_REUSE_TEXTURE_SIZES.size(); ++ordinal)
    {
        if (!Upt42BuildReuseTexture(
                UPT42_REUSE_TEXTURE_SIZES[ordinal],
                ordinal,
                packedDeltas,
                stats[ordinal]))
        {
            common->Printf(
                "PathTraceUnifiedPt: UPT-42 reuse texture construction failed ordinal=%u size=%u\n",
                ordinal,
                UPT42_REUSE_TEXTURE_SIZES[ordinal]);
            return false;
        }
    }
    if (packedDeltas.size() != UPT42_REUSE_TEXTURE_DELTA_ENTRY_COUNT)
        return false;

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtSpatialReuseTextures";
    desc.byteSize = bytes;
    desc.structStride = UPT42_REUSE_TEXTURE_STRIDE;
    desc.initialState = nvrhi::ResourceStates::Common;
    desc.keepInitialState = true;
    nvrhi::BufferHandle buffer = inputs.device->createBuffer(desc);
    if (!buffer)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate UPT-42 reuse textures entries=%u bytes=%llu\n",
            UPT42_REUSE_TEXTURE_ENTRY_COUNT,
            static_cast<unsigned long long>(bytes));
        return false;
    }
    inputs.commandList->writeBuffer(
        buffer,
        packedDeltas.data(),
        static_cast<size_t>(bytes));
    inputs.commandList->setBufferState(
        buffer, nvrhi::ResourceStates::ShaderResource);
    inputs.commandList->commitBarriers();
    m_spatialReuseTextureBuffer = buffer;
    for (uint32_t page = 0u; page < 2u; ++page)
    {
        m_spatialBindingSets[page] = nullptr;
        m_spatialBindingSetDescValid[page] = false;
    }
    common->Printf(
        "PathTraceUnifiedPt: UPT-42 reuse textures sizes=%u/%u/%u sigma=%.1f rounds=%u/%u/%u measuredComponentSigma=%.3f/%.3f/%.3f meanRadius=%.3f/%.3f/%.3f deltaEntries=%u bytes=%llu involution=verified upload=once clear=never\n",
        stats[0].size, stats[1].size, stats[2].size,
        UPT42_REUSE_TEXTURE_SIGMA,
        stats[0].shuffleRounds,
        stats[1].shuffleRounds,
        stats[2].shuffleRounds,
        stats[0].componentSigma,
        stats[1].componentSigma,
        stats[2].componentSigma,
        stats[0].meanRadius,
        stats[1].meanRadius,
        stats[2].meanRadius,
        UPT42_REUSE_TEXTURE_DELTA_ENTRY_COUNT,
        static_cast<unsigned long long>(bytes));
    return true;
}

bool PathTraceUnifiedPtState::EnsureSpatialPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const bool sharedSpatial = Upt30SharedSpatialEnabled(inputs);
    const bool storedSourceTarget = sharedSpatial
        && r_pathTracingUnifiedPtSpatialStoredSourceTarget.GetBool();
    const bool workgroupPairing = storedSourceTarget
        && r_pathTracingUnifiedPtSpatialWorkgroupPairing.GetBool();
    const bool emptyRescue = workgroupPairing
        && r_pathTracingUnifiedPtSpatialEmptyRescue.GetBool();
    const bool multiNeighbor = emptyRescue
        && r_pathTracingUnifiedPtSpatialMultiNeighbor.GetBool();
    const bool disocclusionBoost = emptyRescue && !multiNeighbor
        && Upt438SpatialDisocclusionBoostEnabled(inputs);
    const bool reuseTexturePairing = emptyRescue && !multiNeighbor
        && r_pathTracingUnifiedPtSpatialReuseTexture.GetBool();
    const bool shiftPrepass = reuseTexturePairing
        && Upt43SpatialShiftPrepassEnabled(inputs);
    const bool threeVertexReplay = Upt38ThreeVertexSpatialEnabled(inputs)
        && emptyRescue && !multiNeighbor;
    static int reportedMultiNeighborRequested = -1;
    static int reportedMultiNeighborEffective = -1;
    const int multiNeighborRequested =
        r_pathTracingUnifiedPtSpatialMultiNeighbor.GetBool() ? 1 : 0;
    const int multiNeighborEffective = multiNeighbor ? 1 : 0;
    if (reportedMultiNeighborRequested != multiNeighborRequested
        || reportedMultiNeighborEffective != multiNeighborEffective)
    {
        common->Printf(
            "PathTraceUnifiedPt: spatial multi-neighbor requested/effective=%d/%d gate(workgroupPairing/emptyRescue)=%u/%u\n",
            multiNeighborRequested,
            multiNeighborEffective,
            workgroupPairing ? 1u : 0u,
            emptyRescue ? 1u : 0u);
        reportedMultiNeighborRequested = multiNeighborRequested;
        reportedMultiNeighborEffective = multiNeighborEffective;
    }
    static int reportedBoostRequested = -1;
    static int reportedBoostEffective = -1;
    const int boostRequested =
        r_pathTracingUnifiedPtSpatialDisocclusionBoost.GetBool() ? 1 : 0;
    const int boostEffective = disocclusionBoost ? 1 : 0;
    if (reportedBoostRequested != boostRequested
        || reportedBoostEffective != boostEffective)
    {
        common->Printf(
            "PathTraceUnifiedPt: spatial disocclusion boost requested/effective=%d/%d gate(replayCompaction/shared/storedTarget/workgroup/rescue/noMulti/noReuseTexture/noShiftPrepass/noTemporalPermutation/noThreeVertex)=%u/%u/%u/%u/%u/%u/%u/%u/%u/%u samples=%d historyM=%u\n",
            boostRequested,
            boostEffective,
            Upt43TemporalReplayCompactionEnabled(inputs) ? 1u : 0u,
            sharedSpatial ? 1u : 0u,
            storedSourceTarget ? 1u : 0u,
            workgroupPairing ? 1u : 0u,
            emptyRescue ? 1u : 0u,
            multiNeighbor ? 0u : 1u,
            reuseTexturePairing ? 0u : 1u,
            r_pathTracingUnifiedPtSpatialShiftPrepass.GetBool() ? 0u : 1u,
            r_pathTracingUnifiedPtTemporalPermutationSampling.GetBool()
                ? 0u : 1u,
            (inputs.threeVertexInitial
                || r_pathTracingUnifiedPtThreeVertexInitial.GetBool())
                ? 0u : 1u,
            idMath::ClampInt(
                1, UPT438_DISOCCLUSION_MAX_SAMPLES,
                r_pathTracingUnifiedPtSpatialDisocclusionBoostSamples.GetInteger()),
            UPT438_DISOCCLUSION_HISTORY_M);
        reportedBoostRequested = boostRequested;
        reportedBoostEffective = boostEffective;
    }
    if (m_spatialPipeline && m_spatialCompactLights == inputs.compactLights
        && m_spatialSharedReuse == sharedSpatial
        && m_spatialStoredSourceTarget == storedSourceTarget
        && m_spatialWorkgroupPairing == workgroupPairing
        && m_spatialEmptyRescue == emptyRescue
        && m_spatialMultiNeighbor == multiNeighbor
        && m_spatialDisocclusionBoost == disocclusionBoost
        && m_spatialReuseTexturePairing == reuseTexturePairing
        && m_spatialShiftPrepass == shiftPrepass
        && m_spatialThreeVertexReplay == threeVertexReplay
        && m_spatialLambertDiagnostic == inputs.lambertDiagnostic)
    {
        return true;
    }
    if (m_spatialCompactLights != inputs.compactLights
        || m_spatialSharedReuse != sharedSpatial
        || m_spatialStoredSourceTarget != storedSourceTarget
        || m_spatialWorkgroupPairing != workgroupPairing
        || m_spatialEmptyRescue != emptyRescue
        || m_spatialMultiNeighbor != multiNeighbor
        || m_spatialDisocclusionBoost != disocclusionBoost
        || m_spatialReuseTexturePairing != reuseTexturePairing
        || m_spatialShiftPrepass != shiftPrepass
        || m_spatialThreeVertexReplay != threeVertexReplay
        || m_spatialLambertDiagnostic != inputs.lambertDiagnostic)
    {
        ReleaseSpatial();
        m_spatialCompactLights = inputs.compactLights;
        m_spatialSharedReuse = sharedSpatial;
        m_spatialStoredSourceTarget = storedSourceTarget;
        m_spatialWorkgroupPairing = workgroupPairing;
        m_spatialEmptyRescue = emptyRescue;
        m_spatialMultiNeighbor = multiNeighbor;
        m_spatialDisocclusionBoost = disocclusionBoost;
        m_spatialReuseTexturePairing = reuseTexturePairing;
        m_spatialShiftPrepass = shiftPrepass;
        m_spatialThreeVertexReplay = threeVertexReplay;
        m_spatialLambertDiagnostic = inputs.lambertDiagnostic;
    }
    if (m_spatialPipelineAttempted)
    {
        return false;
    }
    m_spatialPipelineAttempted = true;

    if (!inputs.device->queryFeatureSupport(nvrhi::Feature::RayQuery))
    {
        common->Printf(
            "PathTraceUnifiedPt: UPT-09 spatial requires RayQuery support; skipped\n");
        return false;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
    Upt04AddReplayGeometryLayoutItems(layoutDesc);
    if (sharedSpatial)
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(22));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(27));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(28));
    if (reuseTexturePairing)
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(29));
    if (shiftPrepass)
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(30));
    if (disocclusionBoost)
    {
        layoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(33));
        layoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(34));
        layoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(35));
    }
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT09_PUSH_CONSTANT_BYTES));
    m_spatialBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_spatialBindingLayout)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-09 spatial binding layout\n");
        return false;
    }

    const char* path = disocclusionBoost
        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_boost_classify.bin"
        : shiftPrepass
        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_shared_reuse_texture_resample_light64.bin"
        : threeVertexReplay
        ? (reuseTexturePairing
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_shared_reuse_texture_pair_rescue_stored_target_light64_three_vertex.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_shared_workgroup_pair_rescue_stored_target_light64_three_vertex.bin")
        : sharedSpatial
        ? (workgroupPairing
            ? (emptyRescue
                ? (multiNeighbor
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_shared_workgroup_multi3_rescue_stored_target_light64.bin"
                    : "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_shared_reuse_texture_pair_rescue_stored_target_light64.bin")
                : "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_shared_workgroup_pair_stored_target_light64.bin")
            : storedSourceTarget
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_shared_selected_pair_stored_target_light64.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_shared_selected_pair_light64.bin")
        : (inputs.lambertDiagnostic
        ? (inputs.compactLights
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_direct_rayquery_light64_lambert.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_direct_rayquery_lambert.bin")
        : (inputs.compactLights
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_direct_rayquery_light64.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_direct_rayquery.bin"));
    void* data = nullptr;
    int size = 0;
    ID_TIME_T timestamp = 0;
    uint64_t hash = 0;
    if (!Upt04ReadShader(path, data, size, timestamp, hash))
    {
        return false;
    }
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtSpatialDirect";
    m_spatialShader = inputs.device->createShader(shaderDesc, data, size);
    Mem_Free(data);
    if (!m_spatialShader)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-09 spatial shader\n");
        return false;
    }

    int shiftSize = 0;
    uint64_t shiftHash = 0u;
    if (shiftPrepass)
    {
        void* shiftData = nullptr;
        ID_TIME_T shiftTimestamp = 0;
        const char* shiftPath =
            "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_shared_reuse_texture_shift_prepass_light64.bin";
        if (!Upt04ReadShader(
                shiftPath, shiftData, shiftSize, shiftTimestamp, shiftHash))
            return false;
        nvrhi::ShaderDesc shiftShaderDesc;
        shiftShaderDesc.shaderType = nvrhi::ShaderType::Compute;
        shiftShaderDesc.entryName = "main";
        shiftShaderDesc.debugName = "PathTraceUnifiedPtSpatialShiftPrepass";
        m_spatialShiftShader = inputs.device->createShader(
            shiftShaderDesc, shiftData, shiftSize);
        Mem_Free(shiftData);
        if (!m_spatialShiftShader)
            return false;
    }

    int boostSize = 0;
    uint64_t boostHash = 0u;
    if (disocclusionBoost)
    {
        nvrhi::BindingLayoutDesc boostLayoutDesc;
        boostLayoutDesc.visibility = nvrhi::ShaderType::Compute;
        boostLayoutDesc.registerSpace = 0;
        boostLayoutDesc.registerSpaceIsDescriptorSet = true;
        boostLayoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
            .setShaderResourceOffset(0)
            .setSamplerOffset(0)
            .setUnorderedAccessViewOffset(0);
        boostLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        boostLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
        boostLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
        boostLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));
        boostLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
        boostLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
        Upt04AddReplayGeometryLayoutItems(boostLayoutDesc);
        boostLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(22));
        boostLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(27));
        boostLayoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(28));
        boostLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(33));
        boostLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(34));
        boostLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
            0, UPT09_PUSH_CONSTANT_BYTES));
        m_spatialBoostBindingLayout =
            inputs.device->createBindingLayout(boostLayoutDesc);
        if (!m_spatialBoostBindingLayout)
            return false;

        void* boostData = nullptr;
        ID_TIME_T boostTimestamp = 0;
        const char* boostPath =
            "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_shared_disocclusion_boost_compact_light64.bin";
        if (!Upt04ReadShader(
                boostPath, boostData, boostSize, boostTimestamp, boostHash))
            return false;
        nvrhi::ShaderDesc boostShaderDesc;
        boostShaderDesc.shaderType = nvrhi::ShaderType::Compute;
        boostShaderDesc.entryName = "main";
        boostShaderDesc.debugName =
            "PathTraceUnifiedPtSpatialDisocclusionBoost";
        m_spatialBoostShader = inputs.device->createShader(
            boostShaderDesc, boostData, boostSize);
        Mem_Free(boostData);
        if (!m_spatialBoostShader)
            return false;
    }

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_spatialShader;
    pipelineDesc.bindingLayouts = {
        m_spatialBindingLayout,
        inputs.sceneInputs->materials.textureBindlessLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_spatialPipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_spatialPipeline)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-09 spatial pipeline\n");
        return false;
    }
    if (shiftPrepass)
    {
        nvrhi::ComputePipelineDesc shiftPipelineDesc;
        shiftPipelineDesc.CS = m_spatialShiftShader;
        shiftPipelineDesc.bindingLayouts = {
            m_spatialBindingLayout,
            inputs.sceneInputs->materials.textureBindlessLayout };
        m_spatialShiftPipeline = inputs.device->createComputePipeline(
            shiftPipelineDesc);
        if (!m_spatialShiftPipeline)
            return false;
    }
    if (disocclusionBoost)
    {
        nvrhi::ComputePipelineDesc boostPipelineDesc;
        boostPipelineDesc.CS = m_spatialBoostShader;
        boostPipelineDesc.bindingLayouts = {
            m_spatialBoostBindingLayout,
            inputs.sceneInputs->materials.textureBindlessLayout };
        m_spatialBoostPipeline = inputs.device->createComputePipeline(
            boostPipelineDesc);
        if (!m_spatialBoostPipeline)
            return false;
    }
    common->Printf(
        "PathTraceUnifiedPt: spatial compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=%s lightStride=%u attempts=%u/%u radius=%.1f visibilityRaysMax=%u sharedSpatial=%u storedSourceTarget=%u workgroupPairing=%u reuseTexturePairing=%u reuseTextureSize=%u shiftPrepass=%u shiftBlobBytes=%d shiftHash=%016llx shiftRecordBytes=%u emptyRescue=%u multiNeighbor=%u disocclusionBoost=%u boostBlobBytes=%d boostHash=%016llx threeVertexReplay=%u neighbors=%u pairedPixels=%u continuationRaysMaxPerMapping=%u mappingRaysMaxPerPair=%u shading=%s createUs=%llu\n",
        size,
        static_cast<unsigned long long>(hash),
        static_cast<long long>(timestamp),
        shiftPrepass ? "8x8-shift+resample"
            : (reuseTexturePairing ? "8x8-screen-leaders" : "8x8-pixels"),
        inputs.compactLights ? UPT04_COMPACT_LIGHT_STRIDE : 112u,
        sharedSpatial
            ? (multiNeighbor ? UPT09_REGULAR_NEIGHBOR_COUNT : 1u)
            : UPT09_REGULAR_NEIGHBOR_COUNT,
        sharedSpatial ? 0u : UPT09_RESCUE_NEIGHBOR_COUNT,
        reuseTexturePairing ? UPT09_NEIGHBOR_RADIUS
            : (sharedSpatial ? 1.0f : UPT09_NEIGHBOR_RADIUS),
        1u,
        sharedSpatial ? 1u : 0u,
        storedSourceTarget ? 1u : 0u,
        workgroupPairing ? 1u : 0u,
        reuseTexturePairing ? 1u : 0u,
        reuseTexturePairing ? UPT42_REUSE_TEXTURE_SIZES[0] : 0u,
        shiftPrepass ? 1u : 0u,
        shiftSize,
        static_cast<unsigned long long>(shiftHash),
        shiftPrepass ? UPT43_SPATIAL_SHIFT_STRIDE : 0u,
        emptyRescue ? 1u : 0u,
        multiNeighbor ? 1u : 0u,
        disocclusionBoost ? 1u : 0u,
        boostSize,
        static_cast<unsigned long long>(boostHash),
        threeVertexReplay ? 1u : 0u,
        multiNeighbor ? UPT09_REGULAR_NEIGHBOR_COUNT
            : (sharedSpatial ? 1u : UPT09_REGULAR_NEIGHBOR_COUNT),
        sharedSpatial ? 2u : 1u,
        threeVertexReplay ? 2u : (sharedSpatial ? 1u : 0u),
        sharedSpatial ? (threeVertexReplay ? 4u : 2u) : 0u,
        inputs.lambertDiagnostic ? "lambert-diagnostic" : "openpbr",
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureSpatialBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const bool sharedSpatial = m_spatialSharedReuse;
    const bool reuseTexturePairing = m_spatialReuseTexturePairing;
    const bool shiftPrepass = m_spatialShiftPrepass;
    if (!inputs.compactPrimaryHistory ||
        !inputs.primarySurfaceCurrentBuffer ||
        !inputs.primaryHistorySidecarCurrentBuffer ||
        inputs.primarySurfaceCurrentBuffer->getDesc().structStride !=
            PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER32_STRIDE ||
        inputs.primaryHistorySidecarCurrentBuffer->getDesc().structStride !=
            PATH_TRACE_UNIFIED_PT_PRIMARY_HISTORY_SIDECAR_STRIDE)
    {
        common->Printf(
            "PathTraceUnifiedPt: UPT-09 spatial requires a compact 32-byte current receiver plus 32-byte history sidecar\n");
        return false;
    }
    const nvrhi::BufferHandle lightBuffer = inputs.compactLights
        ? m_compactLightsBuffer
        : inputs.sceneInputs->lights.restirLightManagerCurrentPayloadBuffer;
    const RtPathTraceSceneInputGeometry& geometry = inputs.sceneInputs->geometry;
    const RtPathTraceSceneInputMaterials& materials = inputs.sceneInputs->materials;
    const RtPathTraceSceneInputLights& lights = inputs.sceneInputs->lights;
    if (!lightBuffer || !CurrentPage() || !HistoryPage()
        || !Upt04ReplayGeometryBindingsValid(*inputs.sceneInputs)
        || (sharedSpatial && !lights.unifiedPtEmissiveLookupBuffer)
        || (reuseTexturePairing && !m_spatialReuseTextureBuffer)
        || (shiftPrepass && !m_spatialShiftBuffer)
        || !materials.materialTableBuffer || !materials.textureSampler
        || !materials.textureBindlessLayout || !materials.textureDescriptorTable)
    {
        return false;
    }

    const uint32_t pageIndex = m_currentPageIndex;
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
        0, inputs.sceneInputs->geometry.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        1, inputs.primarySurfaceCurrentBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, CurrentPage()));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, HistoryPage()));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, lightBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        5, inputs.primaryHistorySidecarCurrentBuffer));
    Upt04AddReplayGeometryBindingItems(desc, *inputs.sceneInputs);
    if (sharedSpatial)
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            22, lights.unifiedPtEmissiveLookupBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        27, materials.materialTableBuffer));
    desc.addItem(nvrhi::BindingSetItem::Sampler(28, materials.textureSampler));
    if (reuseTexturePairing)
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            29, m_spatialReuseTextureBuffer));
    if (shiftPrepass)
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            30, m_spatialShiftBuffer));
    if (m_spatialDisocclusionBoost)
    {
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            33, m_temporalReplayQueue));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            34, m_temporalReplayMeta));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            35, m_temporalReplayDispatchArgs));
    }
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT09_PUSH_CONSTANT_BYTES));
    if (m_spatialBindingSets[pageIndex] &&
        m_spatialBindingSetDescValid[pageIndex] &&
        m_spatialBindingSetDescs[pageIndex] == desc)
    {
        return true;
    }
    m_spatialBindingSets[pageIndex] = inputs.device->createBindingSet(
        desc, m_spatialBindingLayout);
    if (!m_spatialBindingSets[pageIndex])
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-09 spatial binding set\n");
        return false;
    }
    m_spatialBindingSetDescs[pageIndex] = desc;
    m_spatialBindingSetDescValid[pageIndex] = true;
    return true;
}

bool PathTraceUnifiedPtState::EnsureSpatialBoostBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!m_spatialDisocclusionBoost)
        return true;
    if (!m_spatialBoostBindingLayout || !m_temporalReplayQueue
        || !m_temporalReplayMeta || !m_spatialBoostPipeline)
        return false;
    const nvrhi::BufferHandle lightBuffer = inputs.compactLights
        ? m_compactLightsBuffer
        : inputs.sceneInputs->lights.restirLightManagerCurrentPayloadBuffer;
    if (!lightBuffer || !CurrentPage() || !HistoryPage())
        return false;

    const uint32_t pageIndex = m_currentPageIndex;
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
        0, inputs.sceneInputs->geometry.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        1, inputs.primarySurfaceCurrentBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        2, CurrentPage()));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
        3, HistoryPage()));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, lightBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        5, inputs.primaryHistorySidecarCurrentBuffer));
    Upt04AddReplayGeometryBindingItems(desc, *inputs.sceneInputs);
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        22, inputs.sceneInputs->lights.unifiedPtEmissiveLookupBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        27, inputs.sceneInputs->materials.materialTableBuffer));
    desc.addItem(nvrhi::BindingSetItem::Sampler(
        28, inputs.sceneInputs->materials.textureSampler));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        33, m_temporalReplayQueue));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        34, m_temporalReplayMeta));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT09_PUSH_CONSTANT_BYTES));
    if (m_spatialBoostBindingSets[pageIndex]
        && m_spatialBoostBindingSetDescValid[pageIndex]
        && m_spatialBoostBindingSetDescs[pageIndex] == desc)
        return true;
    m_spatialBoostBindingSets[pageIndex] =
        inputs.device->createBindingSet(desc, m_spatialBoostBindingLayout);
    if (!m_spatialBoostBindingSets[pageIndex])
        return false;
    m_spatialBoostBindingSetDescs[pageIndex] = desc;
    m_spatialBoostBindingSetDescValid[pageIndex] = true;
    return true;
}

void PathTraceUnifiedPtState::DrainSpatialBoostReadback(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!m_spatialBoostReadbackPending || !m_spatialBoostReadback
        || !inputs.device)
        return;
    if (m_spatialBoostReadbackDelayFrames > 0)
    {
        --m_spatialBoostReadbackDelayFrames;
        return;
    }
    const uint32_t* words = static_cast<const uint32_t*>(
        inputs.device->mapBuffer(
            m_spatialBoostReadback, nvrhi::CpuAccessMode::Read));
    if (!words)
    {
        common->Printf(
            "PathTraceUnifiedPt: spatial disocclusion boost readback map failed\n");
        m_spatialBoostReadbackPending = false;
        return;
    }
    const uint32_t bounded = Min(words[3], words[4]);
    const uint32_t expectedGroups = (bounded + 63u) / 64u;
    const uint64_t screenPixels = uint64_t(inputs.width) * inputs.height;
    common->Printf(
        "PathTraceUnifiedPt: spatial disocclusion boost published/bounded/capacity=%u/%u/%u fraction=%.4f groups(actual/expected,y,z)=%u/%u/%u/%u overflow=%u status=%s\n",
        words[3], bounded, words[4],
        screenPixels != 0u ? double(bounded) / double(screenPixels) : 0.0,
        words[0], expectedGroups, words[1], words[2],
        words[3] > words[4] ? words[3] - words[4] : 0u,
        words[0] == expectedGroups && words[1] == 1u && words[2] == 1u
            ? "PASS" : "FAIL");
    inputs.device->unmapBuffer(m_spatialBoostReadback);
    m_spatialBoostReadbackPending = false;
}

bool PathTraceUnifiedPtState::ExecuteSpatial(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    UpdateSpatialGpuTiming(inputs);
    PollSpatialGpuTiming(inputs);
    DrainSpatialBoostReadback(inputs);
    m_spatialExecutedThisFrame = false;
    if (!inputs.spatial)
    {
        return false;
    }
    const bool sharedSpatial = Upt30SharedSpatialEnabled(inputs);
    if (inputs.family != PathTraceUnifiedPtFamily::DirectOnly && !sharedSpatial)
    {
        common->Printf(
            "PathTraceUnifiedPt: UPT-09 spatial baseline is direct-only; dispatch skipped for family=%s\n",
            Upt04FamilyName(inputs.family));
        return false;
    }
    const bool productionFullFrame = inputs.proofStage >= 9u &&
        Upt04PipelineVariant(inputs) == 0u;
    const bool reuseTexturePairing = sharedSpatial
        && r_pathTracingUnifiedPtSpatialStoredSourceTarget.GetBool()
        && r_pathTracingUnifiedPtSpatialWorkgroupPairing.GetBool()
        && r_pathTracingUnifiedPtSpatialEmptyRescue.GetBool()
        && r_pathTracingUnifiedPtSpatialReuseTexture.GetBool()
        && !r_pathTracingUnifiedPtSpatialMultiNeighbor.GetBool();
    const bool shiftPrepass = reuseTexturePairing
        && Upt43SpatialShiftPrepassEnabled(inputs);
    const bool disocclusionBoost =
        Upt438SpatialDisocclusionBoostEnabled(inputs);
    if (!r_pathTracingUnifiedPtSpatialDisocclusionBoostDiagnostics.GetBool())
        m_spatialBoostDiagnosticCaptured = false;
    const PathTraceUnifiedPtPageMetadata& currentMetadata = CurrentPageMetadata();
    if (!productionFullFrame || !currentMetadata.fullyWritten ||
        currentMetadata.width != inputs.width ||
        currentMetadata.height != inputs.height ||
        currentMetadata.historyEpoch != inputs.historyEpoch ||
        (reuseTexturePairing
            && !EnsureSpatialReuseTextureResources(inputs)) ||
        !EnsureSpatialPipeline(inputs))
    {
        return false;
    }

    const RtPathTraceSceneInputLights& lights = inputs.sceneInputs->lights;
    const RtPathTraceSceneInputGeometry& geometry = inputs.sceneInputs->geometry;
    const uint64_t surfaceCount64 = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (surfaceCount64 > std::numeric_limits<uint32_t>::max())
    {
        return false;
    }
    if (shiftPrepass)
    {
        const uint32_t surfaceCount = static_cast<uint32_t>(surfaceCount64);
        if (!m_spatialShiftBuffer || m_spatialShiftCapacity != surfaceCount)
        {
            nvrhi::BufferDesc shiftDesc;
            shiftDesc.debugName = "PathTraceUnifiedPtSpatialShiftRecords";
            shiftDesc.byteSize = surfaceCount64 * UPT43_SPATIAL_SHIFT_STRIDE;
            shiftDesc.structStride = UPT43_SPATIAL_SHIFT_STRIDE;
            shiftDesc.canHaveUAVs = true;
            shiftDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            shiftDesc.keepInitialState = true;
            m_spatialShiftBuffer = inputs.device->createBuffer(shiftDesc);
            m_spatialShiftCapacity = m_spatialShiftBuffer
                ? surfaceCount : 0u;
            for (uint32_t page = 0u; page < 2u; ++page)
            {
                m_spatialBindingSets[page] = nullptr;
                m_spatialBindingSetDescValid[page] = false;
            }
            if (!m_spatialShiftBuffer)
                return false;
            common->Printf(
                "PathTraceUnifiedPt: spatial shift buffer pixels=%u bytes=%llu recordBytes=%u clear=never overwrite=full-frame\n",
                surfaceCount,
                static_cast<unsigned long long>(shiftDesc.byteSize),
                UPT43_SPATIAL_SHIFT_STRIDE);
        }
    }
    if (disocclusionBoost
        && (!m_temporalReplayQueue || !m_temporalReplayMeta
            || !m_temporalReplayDispatchArgs))
        return false;
    if (disocclusionBoost
        && r_pathTracingUnifiedPtSpatialDisocclusionBoostDiagnostics.GetBool()
        && !m_spatialBoostReadback)
    {
        nvrhi::BufferDesc readbackDesc;
        readbackDesc.debugName =
            "PathTraceUnifiedPtSpatialBoostReadback";
        readbackDesc.byteSize = 5u * sizeof(uint32_t);
        readbackDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        readbackDesc.initialState = nvrhi::ResourceStates::CopyDest;
        readbackDesc.keepInitialState = true;
        m_spatialBoostReadback = inputs.device->createBuffer(readbackDesc);
        if (!m_spatialBoostReadback)
            return false;
    }
    if (!EnsureSpatialBindingSet(inputs)
        || !EnsureSpatialBoostBindingSet(inputs))
        return false;
    Upt09SpatialDirectControl control = {};
    control.renderWidth = inputs.width;
    control.renderHeight = inputs.height;
    control.surfaceCount = static_cast<uint32_t>(surfaceCount64);
    control.frameSampleIndex = inputs.frameSampleIndex;
    control.emissiveRangeStart = lights.restirLightManagerEmissiveRangeOffset;
    control.emissiveRangeCount = lights.restirLightManagerEmissiveRangeCount;
    control.analyticRangeStart = lights.restirLightManagerDoomAnalyticRangeOffset;
    control.analyticRangeCount = lights.restirLightManagerDoomAnalyticSampleableCount;
    control.currentLightCount = static_cast<uint32_t>(Max(
        0, lights.restirLightManagerCurrentPayloadCount));
    control.maximumInputM = UPT09_MAXIMUM_INPUT_M;
    control.regularNeighborCount = disocclusionBoost
        ? static_cast<uint32_t>(idMath::ClampInt(
            1, UPT438_DISOCCLUSION_MAX_SAMPLES,
            r_pathTracingUnifiedPtSpatialDisocclusionBoostSamples.GetInteger()))
        : UPT09_REGULAR_NEIGHBOR_COUNT;
    control.rescueNeighborCount = disocclusionBoost
        ? UPT438_DISOCCLUSION_HISTORY_M
        : UPT09_RESCUE_NEIGHBOR_COUNT;
    control.neighborRadius = UPT09_NEIGHBOR_RADIUS;
    control.geometryAvailabilityFlags =
        (geometry.staticBucketRoutePublicationValid
            ? UPT04_ROUTE_STATIC_BUCKETS : 0u)
        | ((static_cast<uint32_t>(idMath::ClampInt(
                1,
                UPT04_NEE_RIS_MAX_EMISSIVE_CANDIDATE_COUNT,
                r_pathTracingReservoirCandidateTrials.GetInteger()))
                << UPT04_EMISSIVE_TRIAL_COUNT_SHIFT)
            & UPT04_EMISSIVE_TRIAL_COUNT_MASK)
        | (r_pathTracingUnifiedPtDirectTargetPdfParity.GetBool()
            ? UPT04_DIRECT_TARGET_PDF_PARITY : 0u)
        | (r_pathTracingUnifiedPtAnalyticPortalDomain.GetBool()
            ? UPT04_ANALYTIC_PORTAL_DOMAIN : 0u)
        | (r_pathTracingReservoirTwoSidedEmissives.GetBool()
            ? UPT04_TWO_SIDED_EMISSIVES : 0u)
        | (sharedSpatial ? UPT07_GEOMETRY_FLAG_INDIRECT_REPLAY : 0u);
    control.emissiveScale = Max(0.0f, inputs.emissiveScale);
    control.proofMode = static_cast<uint32_t>(idMath::ClampInt(
        0, 6, r_pathTracingUnifiedPtSpatialProofMode.GetInteger()));
    for (uint32_t axis = 0; axis < 3u; ++axis)
        control.primaryCameraOrigin[axis] = inputs.primaryCameraOrigin[axis];
    control.compactPrimaryHistory = inputs.compactPrimaryHistory ? 1u : 0u;
    control.materialCount = static_cast<uint32_t>(Max(
        0, inputs.sceneInputs->materials.materialTableEntryCount));
    control.logicalTextureCount = static_cast<uint32_t>(Max(
        0, inputs.sceneInputs->materials.logicalTextureDescriptorCount));
    const uint32_t distributionCount = static_cast<uint32_t>(Max(
        0, lights.emissiveDistributionCount));
    control.emissiveDistributionCountAndValid =
        (distributionCount & UPT04_CONTROL_METADATA_COUNT_MASK)
        | (lights.emissiveDistributionValid && distributionCount != 0u
            ? UPT04_CONTROL_METADATA_VALID_BIT : 0u);
    const uint32_t lookupCapacity = static_cast<uint32_t>(Max(
        0, lights.unifiedPtEmissiveLookupCount));
    control.emissiveLookupCapacityAndValid =
        (lookupCapacity & UPT04_CONTROL_METADATA_COUNT_MASK)
        | (lights.unifiedPtEmissiveLookupExact && lookupCapacity >= 2u
            ? UPT04_CONTROL_METADATA_VALID_BIT : 0u);

    const nvrhi::BufferHandle lightBuffer = inputs.compactLights
        ? m_compactLightsBuffer
        : lights.restirLightManagerCurrentPayloadBuffer;
    {
        const char* spatialMarker = shiftPrepass
            ? "UPT.S0b Spatial Gaussian Reciprocal Resample"
            : sharedSpatial
            ? (m_spatialWorkgroupPairing
                ? (reuseTexturePairing
                    ? "UPT.S0 Enhanced Reuse Texture Reciprocal Pair GRIS"
                    : "UPT.S0 Shared Workgroup Reciprocal Pair GRIS")
                : "UPT.S0 Shared Disjoint Reciprocal Pair GRIS")
            : "UPT.S0 Spatial Unique BasicCorrection";
        if (control.proofMode == 1u)
            spatialMarker = "UPT.S0 Proof PassThrough";
        else if (control.proofMode == 2u)
            spatialMarker = "UPT.S0 Proof WithReplacement";
        else if (control.proofMode == 3u)
            spatialMarker = "UPT.S0 Proof Unique Standard1OverM";
        else if (control.proofMode == 4u)
            spatialMarker = "UPT.S0 Proof Legacy SelectedOnly";
        else if (control.proofMode == 5u)
            spatialMarker = "UPT.S0 Proof Fresh SourceTarget";
        else if (control.proofMode == 6u)
            spatialMarker = "UPT.S0 Proof PairwiseMIS";
        Upt04MarkerScope marker(
            inputs.commandList,
            spatialMarker,
            inputs.nsightMarkers);
        inputs.commandList->setAccelStructState(
            inputs.sceneInputs->geometry.tlas,
            nvrhi::ResourceStates::AccelStructRead);
        inputs.commandList->setBufferState(
            inputs.primarySurfaceCurrentBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            inputs.primaryHistorySidecarCurrentBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            CurrentPage(), nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            HistoryPage(), nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->setBufferState(
            lightBuffer, nvrhi::ResourceStates::ShaderResource);
        Upt04SetReplayGeometrySrvStates(
            inputs.commandList, *inputs.sceneInputs);
        if (sharedSpatial)
            inputs.commandList->setBufferState(
                lights.unifiedPtEmissiveLookupBuffer,
                nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            inputs.sceneInputs->materials.materialTableBuffer,
            nvrhi::ResourceStates::ShaderResource);
        if (reuseTexturePairing)
            inputs.commandList->setBufferState(
                m_spatialReuseTextureBuffer,
                nvrhi::ResourceStates::ShaderResource);
        if (shiftPrepass)
            inputs.commandList->setBufferState(
                m_spatialShiftBuffer,
                nvrhi::ResourceStates::UnorderedAccess);
        if (disocclusionBoost)
        {
            const uint32_t boostArgs[3] = { 0u, 1u, 1u };
            const uint32_t boostMeta[2] = {
                0u, m_temporalReplayCapacity };
            inputs.commandList->writeBuffer(
                m_temporalReplayDispatchArgs,
                boostArgs, sizeof(boostArgs));
            inputs.commandList->writeBuffer(
                m_temporalReplayMeta,
                boostMeta, sizeof(boostMeta));
            inputs.commandList->setBufferState(
                m_temporalReplayQueue,
                nvrhi::ResourceStates::UnorderedAccess);
            inputs.commandList->setBufferState(
                m_temporalReplayMeta,
                nvrhi::ResourceStates::UnorderedAccess);
            inputs.commandList->setBufferState(
                m_temporalReplayDispatchArgs,
                nvrhi::ResourceStates::UnorderedAccess);
        }
        inputs.commandList->commitBarriers();

        const nvrhi::TimerQueryHandle spatialGpuTimer =
            BeginSpatialGpuTiming(inputs);
        if (shiftPrepass)
        {
            {
                Upt04MarkerScope shiftMarker(
                    inputs.commandList,
                    "UPT.S0a Spatial Gaussian Shift Prepass",
                    inputs.nsightMarkers);
                nvrhi::ComputeState shiftState;
                shiftState.pipeline = m_spatialShiftPipeline;
                shiftState.bindings = {
                    m_spatialBindingSets[m_currentPageIndex],
                    inputs.sceneInputs->materials.textureDescriptorTable };
                inputs.commandList->setComputeState(shiftState);
                inputs.commandList->setPushConstants(&control, sizeof(control));
                inputs.commandList->dispatch(
                    (inputs.width + 7u) / 8u,
                    (inputs.height + 7u) / 8u,
                    1u);
            }
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, m_spatialShiftBuffer);
        }
        nvrhi::ComputeState state;
        state.pipeline = m_spatialPipeline;
        state.bindings = {
            m_spatialBindingSets[m_currentPageIndex],
            inputs.sceneInputs->materials.textureDescriptorTable };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
        inputs.commandList->dispatch(
            (inputs.width + 7u) / 8u,
            (inputs.height + 7u) / 8u,
            1u);
        if (disocclusionBoost)
        {
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, HistoryPage());
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, m_temporalReplayQueue);
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, m_temporalReplayMeta);
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, m_temporalReplayDispatchArgs);
            inputs.commandList->setBufferState(
                m_temporalReplayQueue,
                nvrhi::ResourceStates::ShaderResource);
            inputs.commandList->setBufferState(
                m_temporalReplayMeta,
                nvrhi::ResourceStates::ShaderResource);
            inputs.commandList->setBufferState(
                m_temporalReplayDispatchArgs,
                nvrhi::ResourceStates::IndirectArgument);
            inputs.commandList->commitBarriers();

            Upt04MarkerScope boostMarker(
                inputs.commandList,
                "UPT.S0b Spatial Compact Disocclusion Boost",
                inputs.nsightMarkers);
            nvrhi::ComputeState boostState;
            boostState.pipeline = m_spatialBoostPipeline;
            boostState.bindings = {
                m_spatialBoostBindingSets[m_currentPageIndex],
                inputs.sceneInputs->materials.textureDescriptorTable };
            boostState.indirectParams = m_temporalReplayDispatchArgs;
            inputs.commandList->setComputeState(boostState);
            inputs.commandList->setPushConstants(&control, sizeof(control));
            inputs.commandList->dispatchIndirect(0u);
        }
        if (spatialGpuTimer)
            inputs.commandList->endTimerQuery(spatialGpuTimer);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, HistoryPage());
        if (disocclusionBoost
            && r_pathTracingUnifiedPtSpatialDisocclusionBoostDiagnostics.GetBool()
            && !m_spatialBoostReadbackPending
            && !m_spatialBoostDiagnosticCaptured)
        {
            inputs.commandList->setBufferState(
                m_temporalReplayDispatchArgs,
                nvrhi::ResourceStates::CopySource);
            inputs.commandList->setBufferState(
                m_temporalReplayMeta,
                nvrhi::ResourceStates::CopySource);
            inputs.commandList->setBufferState(
                m_spatialBoostReadback,
                nvrhi::ResourceStates::CopyDest);
            inputs.commandList->commitBarriers();
            inputs.commandList->copyBuffer(
                m_spatialBoostReadback, 0u,
                m_temporalReplayDispatchArgs, 0u,
                3u * sizeof(uint32_t));
            inputs.commandList->copyBuffer(
                m_spatialBoostReadback, 3u * sizeof(uint32_t),
                m_temporalReplayMeta, 0u,
                2u * sizeof(uint32_t));
            m_spatialBoostReadbackPending = true;
            m_spatialBoostDiagnosticCaptured = true;
            m_spatialBoostReadbackDelayFrames = 2;
        }
    }
    HistoryPageMetadata() = currentMetadata;
    HistoryPageMetadata().fullyWritten = true;
    m_spatialExecutedThisFrame = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteDuplication(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.duplication || inputs.proofStage < 9u
        || Upt04PipelineVariant(inputs) != 0u
        || !EnsureDuplicationResources(inputs)
        || !EnsureDuplicationPipeline(inputs)
        || !EnsureDuplicationBindingSets(inputs))
        return false;

    // S0 publishes into the physical history page. Without S0, D0/T0 leaves
    // the final current-frame reservoir in the physical current page. Publish
    // the correlation map against that exact physical page so CompleteFrame's
    // role transition keeps reservoir and duplication histories aligned.
    const uint32_t finalPageIndex = m_spatialExecutedThisFrame
        ? m_historyPageIndex : m_currentPageIndex;
    const nvrhi::BufferHandle finalPage = finalPageIndex == 0u
        ? m_page0 : m_page1;
    const PathTraceUnifiedPtPageMetadata& finalMetadata = finalPageIndex == 0u
        ? m_page0Metadata : m_page1Metadata;
    if (!finalMetadata.fullyWritten || finalMetadata.width != inputs.width
        || finalMetadata.height != inputs.height
        || finalMetadata.historyEpoch != inputs.historyEpoch)
        return false;

    const uint64_t surfaceCount64 = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (surfaceCount64 > std::numeric_limits<uint32_t>::max())
        return false;
    Upt08Control control = {};
    control.renderWidth = inputs.width;
    control.renderHeight = inputs.height;
    control.surfaceCount = static_cast<uint32_t>(surfaceCount64);
    control.packedRowPitch = m_duplicationPackedRowPitch;

    {
        Upt04MarkerScope marker(
            inputs.commandList, "UPT.U0 Fill SampleID", inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            finalPage, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_duplicationSampleIds, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_duplicationFillPipeline;
        state.bindings = { m_duplicationFillBindingSets[finalPageIndex] };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
        inputs.commandList->dispatch(
            (inputs.width + 7u) / 8u, (inputs.height + 7u) / 8u, 1u);
        nvrhi::utils::BufferUavBarrier(
            inputs.commandList, m_duplicationSampleIds);
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList, "UPT.U1 Compute Duplication17x17", inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            m_duplicationSampleIds, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_duplicationScores[finalPageIndex],
            nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_duplicationComputePipeline;
        state.bindings = { m_duplicationComputeBindingSets[finalPageIndex] };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
        inputs.commandList->dispatch(
            (inputs.width + 7u) / 8u, (inputs.height + 7u) / 8u, 1u);
        nvrhi::utils::BufferUavBarrier(
            inputs.commandList, m_duplicationScores[finalPageIndex]);
    }
    m_duplicationMetadata[finalPageIndex] = finalMetadata;
    m_duplicationMetadata[finalPageIndex].fullyWritten = true;
    return true;
}

void PathTraceUnifiedPtState::CompleteFrame()
{
    // Spatial writes the final current-frame result directly into the physical
    // history page, so its roles already describe the next frame. Otherwise
    // promote every complete D0/T0 publication, even when T0 is disabled: D0
    // now owns a legacy-shaped previous-best lookup and must advance beside
    // the canonical primary-surface history without allocating a third page.
    if (m_initialPublishedThisFrame && !m_spatialExecutedThisFrame)
    {
        const uint32_t oldCurrent = m_currentPageIndex;
        m_currentPageIndex = m_historyPageIndex;
        m_historyPageIndex = oldCurrent;
    }
    m_initialPublishedThisFrame = false;
    m_spatialExecutedThisFrame = false;
}

bool PathTraceUnifiedPtState::EnsureResolveResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (m_resolveOutput && m_resolveWidth == inputs.width &&
        m_resolveHeight == inputs.height &&
        m_resolveOutput->getDesc().format == nvrhi::Format::RGBA16_FLOAT)
    {
        return true;
    }

    nvrhi::TextureDesc desc;
    desc.width = inputs.width;
    desc.height = inputs.height;
    desc.mipLevels = 1;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.isUAV = true;
    desc.debugName = "PathTraceUnifiedPtResolveOutput";
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    nvrhi::TextureHandle output = inputs.device->createTexture(desc);
    if (!output)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate UPT-05 RGBA16F output %ux%u\n",
            inputs.width,
            inputs.height);
        return false;
    }

    m_resolveOutput = output;
    m_resolveWidth = inputs.width;
    m_resolveHeight = inputs.height;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_resolveBindingSets[page] = nullptr;
        m_resolveBindingSetDescValid[page] = false;
    }
    common->Printf(
        "PathTraceUnifiedPt: allocated UPT-05 output %ux%u format=RGBA16_FLOAT bytes=%llu\n",
        inputs.width,
        inputs.height,
        static_cast<unsigned long long>(
            uint64_t(inputs.width) * uint64_t(inputs.height) * 8ull));
    return true;
}

bool PathTraceUnifiedPtState::EnsureResolvePipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (m_resolvePipeline)
    {
        return true;
    }
    if (m_resolvePipelineAttempted)
    {
        return false;
    }
    m_resolvePipelineAttempted = true;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT05_PUSH_CONSTANT_BYTES));
    m_resolveBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_resolveBindingLayout)
    {
        common->Printf("PathTraceUnifiedPt: failed to create UPT-05 resolve binding layout\n");
        return false;
    }

    const char* path = inputs.primaryReceiverMode == 2u
        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt05/upt05_resolve_compact32.bin"
        : (inputs.primaryReceiverMode == 1u
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt05/upt05_resolve_compact.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt05/upt05_resolve_legacy.bin");
    void* data = nullptr;
    int size = 0;
    ID_TIME_T timestamp = 0;
    uint64_t hash = 0;
    if (!Upt04ReadShader(path, data, size, timestamp, hash))
    {
        return false;
    }
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtResolve";
    m_resolveShader = inputs.device->createShader(shaderDesc, data, size);
    Mem_Free(data);
    if (!m_resolveShader)
    {
        common->Printf("PathTraceUnifiedPt: failed to create UPT-05 resolve shader\n");
        return false;
    }

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_resolveShader;
    pipelineDesc.bindingLayouts = { m_resolveBindingLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_resolvePipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_resolvePipeline)
    {
        common->Printf("PathTraceUnifiedPt: failed to create UPT-05 resolve pipeline\n");
        return false;
    }
    m_resolvePrimaryReceiverMode = inputs.primaryReceiverMode;
    common->Printf(
        "PathTraceUnifiedPt: resolve compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=8x8 receiver=%s output=RGBA16_FLOAT rays=0 samples=0 createUs=%llu\n",
        size,
        static_cast<unsigned long long>(hash),
        static_cast<long long>(timestamp),
        Upt04ReceiverName(inputs.primaryReceiverMode),
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureResolveBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const uint32_t pageIndex = m_spatialExecutedThisFrame
        ? m_historyPageIndex : m_currentPageIndex;
    const nvrhi::BufferHandle resolvePage = m_spatialExecutedThisFrame
        ? HistoryPage() : CurrentPage();
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, resolvePage));
    desc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, m_resolveOutput));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        2, inputs.primarySurfaceBuffer));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT05_PUSH_CONSTANT_BYTES));
    if (m_resolveBindingSets[pageIndex] &&
        m_resolveBindingSetDescValid[pageIndex] &&
        m_resolveBindingSetDescs[pageIndex] == desc)
    {
        return true;
    }
    m_resolveBindingSets[pageIndex] = inputs.device->createBindingSet(
        desc, m_resolveBindingLayout);
    if (!m_resolveBindingSets[pageIndex])
    {
        common->Printf("PathTraceUnifiedPt: failed to create UPT-05 resolve binding set\n");
        return false;
    }
    m_resolveBindingSetDescs[pageIndex] = desc;
    m_resolveBindingSetDescValid[pageIndex] = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteResolve(
    const PathTraceUnifiedPtDispatchInputs& inputs,
    uint32_t view)
{
    m_resolveReady = false;
    m_presentationOutput = nullptr;
    if ((m_resolvePipeline || m_resolvePipelineAttempted) &&
        m_resolvePrimaryReceiverMode != inputs.primaryReceiverMode)
    {
        ReleaseResolve();
    }
    const bool productionFullFrame = inputs.proofStage >= 9u &&
        Upt04PipelineVariant(inputs) == 0u;
    const nvrhi::BufferHandle resolvePage = m_spatialExecutedThisFrame
        ? HistoryPage() : CurrentPage();
    const PathTraceUnifiedPtPageMetadata& resolveMetadata = m_spatialExecutedThisFrame
        ? HistoryPageMetadata() : CurrentPageMetadata();
    const uint32_t resolvePageIndex = m_spatialExecutedThisFrame
        ? m_historyPageIndex : m_currentPageIndex;
    if (!productionFullFrame || !resolvePage || m_pageWidth != inputs.width ||
        m_pageHeight != inputs.height || !resolveMetadata.fullyWritten ||
        resolveMetadata.width != inputs.width ||
        resolveMetadata.height != inputs.height ||
        resolveMetadata.historyEpoch != inputs.historyEpoch)
    {
        if (!m_resolveFailureLogged)
        {
            common->Printf(
                "PathTraceUnifiedPt: UPT-05 resolve requires production shaderProof 1..6 and proofStage 9; skipped\n");
            m_resolveFailureLogged = true;
        }
        return false;
    }
    if (!EnsureResolveResources(inputs) || !EnsureResolvePipeline(inputs) ||
        !EnsureResolveBindingSet(inputs))
    {
        return false;
    }
    m_resolveFailureLogged = false;

    const uint64_t surfaceCount64 = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (surfaceCount64 > std::numeric_limits<uint32_t>::max())
    {
        return false;
    }
    Upt05ResolveControl control = {
        inputs.width,
        inputs.height,
        static_cast<uint32_t>(surfaceCount64),
        view,
        {},
        0u
    };
    for (uint32_t axis = 0u; axis < 3u; ++axis)
        control.primaryCameraOrigin[axis] = inputs.primaryCameraOrigin[axis];
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.R0 Resolve Bind+Barriers",
            inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            resolvePage, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            inputs.primarySurfaceBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setTextureState(
            m_resolveOutput,
            nvrhi::AllSubresources,
            nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();

        nvrhi::ComputeState state;
        state.pipeline = m_resolvePipeline;
        state.bindings = { m_resolveBindingSets[resolvePageIndex] };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.R0 Resolve Dispatch",
            inputs.nsightMarkers);
        inputs.commandList->dispatch(
            (inputs.width + 7u) / 8u,
            (inputs.height + 7u) / 8u,
            1u);
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.R0 Resolve OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::TextureUavBarrier(inputs.commandList, m_resolveOutput);
        inputs.commandList->setTextureState(
            m_resolveOutput,
            nvrhi::AllSubresources,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->commitBarriers();
    }
    if (inputs.diagnostics && m_diagnosticReadback)
    {
        // Diagnostics intentionally selects the wide receiver/bindless D0
        // specialization, so compact T0/S0 cannot execute on this one-shot
        // frame. Probing resolvePage here would therefore inspect a fresh,
        // sparse D0 publication instead of the temporally/spatially reused
        // image the user was aiming at. Both reuse schedules retain the prior
        // production publication in HistoryPage until this frame completes;
        // prefer it when its serial is exactly N-1. This also preserves the
        // stable identity needed to distinguish a stale/remapped emitter from
        // a valid but over-weighted sample without making diagnostics alter
        // the evidence it is meant to inspect.
        const PathTraceUnifiedPtPageMetadata& currentMetadata =
            CurrentPageMetadata();
        const PathTraceUnifiedPtPageMetadata& historyMetadata =
            HistoryPageMetadata();
        const bool previousProductionAvailable =
            (inputs.temporal || inputs.spatial) &&
            historyMetadata.fullyWritten &&
            historyMetadata.width == inputs.width &&
            historyMetadata.height == inputs.height &&
            historyMetadata.historyEpoch == inputs.historyEpoch &&
            currentMetadata.frameSerial > 0u &&
            historyMetadata.frameSerial + 1u == currentMetadata.frameSerial;
        const nvrhi::BufferHandle probePage = previousProductionAvailable
            ? HistoryPage() : resolvePage;
        const PathTraceUnifiedPtPageMetadata& probeMetadata =
            previousProductionAvailable ? historyMetadata : resolveMetadata;
        const uint32_t probeX = inputs.width / 2u;
        const uint32_t probeY = inputs.height / 2u;
        const uint64_t probeIndex = uint64_t(probeY) * inputs.width + probeX;
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.R0 Crosshair Reservoir Probe",
            inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            probePage, nvrhi::ResourceStates::CopySource);
        inputs.commandList->setBufferState(
            m_diagnosticReadback, nvrhi::ResourceStates::CopyDest);
        inputs.commandList->commitBarriers();
        inputs.commandList->copyBuffer(
            m_diagnosticReadback,
            UPT04_DIAGNOSTIC_COUNTER_BYTES,
            probePage,
            probeIndex * UPT04_RESERVOIR_STRIDE,
            UPT04_DIAGNOSTIC_RESERVOIR_PROBE_WORD_COUNT * sizeof(uint32_t));
        m_diagnosticProbeFromHistory = previousProductionAvailable;
        m_diagnosticProbeFrameSerial = probeMetadata.frameSerial;
    }
    m_resolveReady = true;
    return true;
}
