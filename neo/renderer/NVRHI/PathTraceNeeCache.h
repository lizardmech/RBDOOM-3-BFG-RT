#pragma once

// Remix-style NEE cache proposal-provider CPU/resource shell.
//
// NEECACHE-01 owns the provider ABI and fixed-budget buffers only. Candidate
// build, debug tracing, PDFNEE consume, temporal, and spatial integration are
// later tasks behind this ABI.

#include <nvrhi/nvrhi.h>

#include <stdint.h>

static constexpr uint32_t PATH_TRACE_NEE_CACHE_BINDING_PROVIDER_RESULT_UAV = 74u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_BINDING_CELL_UAV = 75u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_BINDING_TASK_UAV = 76u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_BINDING_CANDIDATE_UAV = 77u;

static constexpr uint32_t PATH_TRACE_NEE_CACHE_SOURCE_NONE = 0u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_SOURCE_CACHE_ANALYTIC = 1u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_SOURCE_CACHE_EMISSIVE = 2u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_FULL_RLU = 3u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_SOURCE_FALLBACK_TYPED_RLU = 4u;

static constexpr uint32_t PATH_TRACE_NEE_CACHE_FALLBACK_NONE = 0u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_FALLBACK_DISABLED = 1u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_FALLBACK_NO_RLU = 2u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_FALLBACK_EMPTY_CELL = 3u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_FALLBACK_INVALID_CANDIDATE = 4u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_FALLBACK_ZERO_SOURCE_PDF = 5u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_FALLBACK_RAB_REPLAY_FAILED = 6u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_FALLBACK_CACHE_ONLY_DIAGNOSTIC = 7u;

struct PathTraceNeeCacheProviderResult
{
    uint32_t selectedDenseRluIndex = 0xffffffffu;
    uint32_t sourceLabel = PATH_TRACE_NEE_CACHE_SOURCE_NONE;
    uint32_t fallbackReason = PATH_TRACE_NEE_CACHE_FALLBACK_DISABLED;
    uint32_t cellIndex = 0xffffffffu;
    uint32_t candidateSlot = 0xffffffffu;
    uint32_t flags = 0u;
    float sourcePdf = 0.0f;
    float invSourcePdf = 0.0f;
    float mixtureProbability = 0.0f;
    float reserved0 = 0.0f;
    uint32_t reserved1 = 0u;
    uint32_t reserved2 = 0u;
};
static_assert(sizeof(PathTraceNeeCacheProviderResult) == 48, "NEE cache provider result stride must match HLSL ABI");
static_assert((sizeof(PathTraceNeeCacheProviderResult) % 16) == 0, "NEE cache provider result must stay 16-byte aligned");

struct PathTraceNeeCacheCellRecord
{
    uint32_t flags = 0u;
    uint32_t hash = 0u;
    uint32_t taskOffset = 0u;
    uint32_t taskCount = 0u;
    uint32_t candidateOffset = 0u;
    uint32_t candidateCount = 0u;
    uint32_t reserved0 = 0u;
    uint32_t reserved1 = 0u;
};
static_assert(sizeof(PathTraceNeeCacheCellRecord) == 32, "NEE cache cell stride must match HLSL ABI");

struct PathTraceNeeCacheTaskRecord
{
    uint32_t denseRluIndex = 0xffffffffu;
    uint32_t taskClass = 0u;
    float accumulatedValue = 0.0f;
    float decayState = 0.0f;
    uint32_t cellIndex = 0xffffffffu;
    uint32_t flags = 0u;
    uint32_t reserved0 = 0u;
    uint32_t reserved1 = 0u;
};
static_assert(sizeof(PathTraceNeeCacheTaskRecord) == 32, "NEE cache task stride must match HLSL ABI");

struct PathTraceNeeCacheCandidateRecord
{
    uint32_t denseRluIndex = 0xffffffffu;
    uint32_t lightClass = 0u;
    float sourcePdf = 0.0f;
    float invSourcePdf = 0.0f;
    float candidateWeight = 0.0f;
    uint32_t cellIndex = 0xffffffffu;
    uint32_t candidateSlot = 0xffffffffu;
    uint32_t flags = 0u;
};
static_assert(sizeof(PathTraceNeeCacheCandidateRecord) == 32, "NEE cache candidate stride must match HLSL ABI");

enum PathTraceNeeCacheInvalidationFlags : uint32_t
{
    PATH_TRACE_NEE_CACHE_INVALIDATE_NONE = 0u,
    PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_STRUCTURAL = 1u << 0,
    PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_MAPPING = 1u << 1,
    PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_PAYLOAD = 1u << 2,
    PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_PAYLOAD_ONLY = 1u << 3,
    PATH_TRACE_NEE_CACHE_INVALIDATE_RESOURCE_ALLOCATION = 1u << 4,
    PATH_TRACE_NEE_CACHE_INVALIDATE_DIAGNOSTIC_OWNERSHIP = 1u << 5
};

static constexpr uint32_t PATH_TRACE_NEE_CACHE_CLEAN_PROVIDER_STARTUP_DELAY_FRAMES = 20u;
static constexpr uint32_t PATH_TRACE_NEE_CACHE_CLEAN_PROVIDER_STARTUP_REFRESH_FRAMES = 8u;

struct PathTraceNeeCacheSettings
{
    bool enabled = false;
    int mode = 1;
    int debugView = 0;
    int cellResolution = 8;
    float minRange = 256.0f;
    uint32_t cellCount = 65536u;
    uint32_t candidateSlots = 8u;
    uint32_t taskSlots = 8u;
    float fallbackProbability = 0.25f;
    int sourceDomain = 0;
};

struct PathTraceNeeCacheRluInputs
{
    uint32_t currentLightCount = 0u;
    uint32_t emissiveRangeOffset = 0u;
    uint32_t emissiveRangeCount = 0u;
    uint32_t doomAnalyticRangeOffset = 0u;
    uint32_t doomAnalyticRangeCount = 0u;
    uint32_t nonEmptyRangeCount = 0u;
    bool remixDenseDomain = false;
};

struct PathTraceNeeCacheResourceDesc
{
    bool requested = false;
    bool structuralValid = false;
    uint32_t providerResultStride = 0u;
    uint32_t cellStride = 0u;
    uint32_t taskStride = 0u;
    uint32_t candidateStride = 0u;
    uint32_t providerResultCount = 0u;
    uint32_t cellCount = 0u;
    uint32_t taskCount = 0u;
    uint32_t candidateCount = 0u;
    uint64_t providerResultBytes = 0u;
    uint64_t cellBytes = 0u;
    uint64_t taskBytes = 0u;
    uint64_t candidateBytes = 0u;
    uint64_t totalBytes = 0u;
};

struct PathTraceNeeCacheState
{
    nvrhi::BufferHandle providerResultBuffer;
    nvrhi::BufferHandle cellBuffer;
    nvrhi::BufferHandle taskBuffer;
    nvrhi::BufferHandle candidateBuffer;
    nvrhi::BufferHandle placeholderSrvBuffer;
    PathTraceNeeCacheSettings settings;
    PathTraceNeeCacheResourceDesc resourceDesc;
    uint64_t allocationSerial = 0u;
    uint64_t invalidationSerial = 0u;
    uint64_t observedRluStructuralSignature = 0u;
    uint64_t observedRluMappingSignature = 0u;
    uint64_t observedRluPayloadSignature = 0u;
    uint32_t pendingInvalidationFlags = PATH_TRACE_NEE_CACHE_INVALIDATE_NONE;
    uint32_t lastInvalidationFlags = PATH_TRACE_NEE_CACHE_INVALIDATE_NONE;
    uint32_t cleanProviderStartupDelayFrames = 0u;
    uint32_t cleanProviderStartupRefreshFrames = 0u;
    uint32_t cleanProviderStableViewFrames = 0u;
    float cleanProviderLastViewOrigin[3] = {};
    float cleanProviderLastViewForward[3] = {};
    bool observedRluSignaturesValid = false;
    bool taskClearPending = false;
    bool cleanProviderSnapshotHoldActive = false;
    bool cleanProviderRequestedLastFrame = false;
    bool cleanProviderLastViewValid = false;
    bool secondaryVisualSnapshotHoldActive = false;
    bool secondaryVisualBandActiveLastFrame = false;

    void Clear();
    bool EnsureResources(nvrhi::IDevice* device, const PathTraceNeeCacheSettings& nextSettings, const PathTraceNeeCacheResourceDesc& nextDesc);
    void ObserveRluSignatures(uint64_t structuralSignature, uint64_t mappingSignature, uint64_t payloadSignature, uint32_t changeFlags);
};

PathTraceNeeCacheSettings BuildPathTraceNeeCacheSettingsFromCVars();
PathTraceNeeCacheResourceDesc BuildPathTraceNeeCacheResourceDesc(const PathTraceNeeCacheSettings& settings);
const char* PathTraceNeeCacheSourceDomainName(int sourceDomain);
