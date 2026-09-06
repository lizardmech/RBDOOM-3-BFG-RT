#pragma once

#include <cstddef>
#include <cstdint>

#if !defined(RT_PT_PRODUCER_LANES_HARNESS)
#include "PathTraceCaptureProduct.h"
#endif

struct PathTraceSmokeVertex;
class idDrawVert;
class idJointMat;
struct RtPathTraceCaptureRawSurface;
struct RtSmokeTranslucentClassifierStageInput;
struct RtPathTraceRuntimeMaterialStagePod;
struct RtPathTraceCaptureModelTokenTablePod;
struct RtPathTraceMaterialTextureVariantBasePod;
struct RtPathTraceCaptureRegistryMaterialPod;
struct drawSurf_t;
struct srfTriangles_t;
struct viewDef_t;

struct RtPathTraceCommittedVertexInput
{
    const idDrawVert* vertices = nullptr;
    const idJointMat* joints = nullptr;
    float modelMatrix[16] = {};
    float bumpMatrix[6] = {};
};

PathTraceSmokeVertex BuildPathTraceCommittedVertexFromOwned(
    const RtPathTraceCommittedVertexInput& input,
    std::uint32_t vertexIndex);

enum class RtPathTraceCommittedGeometrySource : std::uint8_t
{
    CpuTriArrays = 0
};

struct RtPathTraceCommittedGeometryCounters
{
    std::int32_t invalidNormalVerts = 0;
    std::int32_t invalidUvVerts = 0;
    std::int32_t invalidNormalTriangles = 0;
    std::int32_t invalidUvTriangles = 0;
    std::int32_t forcedGeometricNormalTriangles = 0;
    std::int32_t invalidIndexCount = 0;
    std::int32_t zeroAreaOnly = 0;
};

struct RtPathTraceCommittedGeometrySurface
{
    std::uint32_t ordinal = 0;
    RtPathTraceCommittedGeometrySource source =
        RtPathTraceCommittedGeometrySource::CpuTriArrays;
    std::uint64_t ambientHandle = 0;
    std::uint64_t indexHandle = 0;
    std::uint64_t jointHandle = 0;
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
    RtPathTraceCommittedGeometryCounters counters;
    bool complete = false;
};

struct RtPathTraceCommittedCaptureTelemetry
{
    std::uint64_t captureToken = 0;
    std::int32_t frameNumber = -1;
    std::uint32_t observedViews = 0;
    std::uint32_t queuedViews = 0;
    std::uint32_t completeViews = 0;
    std::uint32_t fallbackViews = 0;
    std::uint32_t fallbackInvalidView = 0;
    std::uint32_t fallbackListUnavailable = 0;
    std::uint32_t fallbackViewCapacity = 0;
    std::uint32_t fallbackPreflight = 0;
    std::uint32_t fallbackBudget = 0;
    std::uint32_t fallbackCopy = 0;
    std::uint32_t fallbackNotAccepting = 0;
    std::uint32_t fallbackIncomplete = 0;
    std::uint32_t listAllocations = 0;
    std::uint32_t listReuses = 0;
    std::uint32_t configuredParallelism = 1;
    std::uint64_t ownedBytes = 0;
    std::uint64_t ownedBytesHighWater = 0;
    std::uint64_t jobCpuSumUs = 0;
    std::uint64_t jobCpuMaxUs = 0;
    std::uint64_t submitUs = 0;
    std::uint64_t joinUs = 0;
    std::uint64_t finalizeUs = 0;
    std::uint64_t semanticMarshalUs = 0;
    std::uint64_t semanticOwnedBytes = 0;
    std::uint32_t compareCalls = 0;
    std::uint32_t compareSourceExcluded = 0;
    std::uint32_t compareExact = 0;
    std::uint32_t compareMismatch = 0;
    std::uint32_t compareShapeMismatch = 0;
    std::uint32_t compareVertexMismatch = 0;
    std::uint32_t compareIndexMismatch = 0;
    std::uint32_t compareCounterMismatch = 0;
    bool viewReconciled = false;
    bool compareReconciled = false;
    bool finalized = false;
};

struct RtPathTracePrimarySemanticDto
{
    std::uint64_t sealedPrimaryViewToken = 0;
    std::uint64_t materialRegistryGeneration = 0;
    std::uint64_t residentMaterialFactsGeneration = 0;
    std::uint32_t sourceDrawSurfCount = 0;
    RtPathTraceCaptureRawSurface* surfaces = nullptr;
    std::uint32_t surfaceCount = 0;
    std::uint32_t factsDerivedSurfaceCount = 0;
    RtSmokeTranslucentClassifierStageInput* classifierStages = nullptr;
    std::uint32_t classifierStageCount = 0;
    RtPathTraceRuntimeMaterialStagePod* runtimeStages = nullptr;
    std::uint32_t runtimeStageCount = 0;
    float* registers = nullptr;
    std::uint32_t registerCount = 0;
    idDrawVert* vertices = nullptr;
    std::uint32_t vertexCount = 0;
    void* indexes = nullptr;
    std::uint32_t indexCount = 0;
    idJointMat* joints = nullptr;
    std::uint32_t jointCount = 0;
    RtPathTraceCaptureModelTokenTablePod* modelTables = nullptr;
    std::uint32_t modelTableCount = 0;
    std::uint64_t* modelSurfaceTokens = nullptr;
    std::uint32_t modelSurfaceTokenCount = 0;
    RtPathTraceMaterialTextureVariantBasePod* variantBases = nullptr;
    std::uint32_t variantBaseCount = 0;
    RtPathTraceCaptureRegistryMaterialPod* registryMaterials = nullptr;
    std::uint32_t registryMaterialCount = 0;
    bool complete = false;
};

inline bool RtPathTracePrimarySemanticDtoCarrierComplete(
    const RtPathTracePrimarySemanticDto& dto,
    std::uint64_t expectedSealedPrimaryViewToken)
{
    return dto.complete && expectedSealedPrimaryViewToken != 0 &&
        dto.sealedPrimaryViewToken == expectedSealedPrimaryViewToken &&
        dto.sourceDrawSurfCount == dto.surfaceCount &&
        dto.factsDerivedSurfaceCount == 0 &&
        (dto.surfaceCount == 0 || dto.surfaces != nullptr) &&
        (dto.classifierStageCount == 0 || dto.classifierStages != nullptr) &&
        (dto.runtimeStageCount == 0 || dto.runtimeStages != nullptr) &&
        (dto.registerCount == 0 || dto.registers != nullptr) &&
        (dto.vertexCount == 0 || dto.vertices != nullptr) &&
        (dto.indexCount == 0 || dto.indexes != nullptr) &&
        (dto.jointCount == 0 || dto.joints != nullptr) &&
        (dto.modelTableCount == 0 || dto.modelTables != nullptr) &&
        (dto.modelSurfaceTokenCount == 0 || dto.modelSurfaceTokens != nullptr) &&
        (dto.variantBaseCount == 0 || dto.variantBases != nullptr) &&
        (dto.registryMaterialCount == 0 || dto.registryMaterials != nullptr);
}

#if !defined(RT_PT_PRODUCER_LANES_HARNESS)
inline bool RtPathTracePrimarySemanticDtoShapeComplete(
    const RtPathTracePrimarySemanticDto& dto,
    std::uint64_t expectedSealedPrimaryViewToken)
{
    if (!RtPathTracePrimarySemanticDtoCarrierComplete(
            dto, expectedSealedPrimaryViewToken))
    {
        return false;
    }
    for (std::uint32_t index = 0; index < dto.surfaceCount; ++index)
    {
        const RtPathTraceCaptureRawSurface& raw = dto.surfaces[index];
        if (raw.ordinal != index ||
            !RtPathTraceCaptureSourceSurfaceComplete(raw,
                dto.classifierStageCount, dto.runtimeStageCount,
                dto.registerCount, dto.vertexCount, dto.indexCount,
                dto.jointCount))
        {
            return false;
        }
        if (raw.modelTableIndex != RT_PT_CAPTURE_INVALID_MODEL_TABLE)
        {
            if (raw.modelTableIndex >= dto.modelTableCount)
            {
                return false;
            }
            const RtPathTraceCaptureModelTokenTablePod& table =
                dto.modelTables[raw.modelTableIndex];
            if (table.modelBits != raw.modelBits ||
                table.modelEpoch != raw.modelEpoch ||
                table.tokenOffset > dto.modelSurfaceTokenCount ||
                table.tokenCount >
                    dto.modelSurfaceTokenCount - table.tokenOffset)
            {
                return false;
            }
        }
        else if (raw.modelBits != 0)
        {
            return false;
        }
    }
    for (std::uint32_t index = 1;
        index < dto.registryMaterialCount; ++index)
    {
        // Registry lookup is unique by materialId; its fill sorts the unique
        // rows, so equality is as invalid as descending order.
        if (dto.registryMaterials[index - 1].materialId >=
            dto.registryMaterials[index].materialId)
        {
            return false;
        }
    }
    for (std::uint32_t index = 1; index < dto.variantBaseCount; ++index)
    {
        // The source is an ID-keyed map, hence variantId is unique and the
        // enumeration contract is strictly ascending.
        if (dto.variantBases[index - 1].variantId >=
            dto.variantBases[index].variantId)
        {
            return false;
        }
    }
    return true;
}
#endif

struct RtPathTraceCommittedGeometryProduct
{
    std::uint64_t viewIdentity = 0;
    std::uint64_t captureToken = 0;
    std::uint64_t sealedPrimaryViewToken = 0;
    std::uint64_t configFingerprint = 0;
    std::int32_t surfaceCount = 0;
    std::uint32_t vertexCapacity = 0;
    std::uint32_t indexCapacity = 0;
    RtPathTraceCommittedGeometrySurface* surfaces = nullptr;
    PathTraceSmokeVertex* vertices = nullptr;
    std::uint32_t* indexes = nullptr;
    RtPathTracePrimarySemanticDto semanticDto;
    bool complete = false;
};

bool RtPathTracePrimarySemanticDtoShapeValid(
    const RtPathTracePrimarySemanticDto& dto,
    std::uint64_t expectedSealedPrimaryViewToken);

inline bool RtPathTraceCommittedPrimaryDtoMayPublish(
    bool primaryView,
    bool queued,
    bool joined,
    bool productComplete,
    std::uint64_t productSealedViewToken,
    std::uint64_t expectedSealedViewToken)
{
    return primaryView && queued && joined && productComplete &&
        expectedSealedViewToken != 0 &&
        productSealedViewToken == expectedSealedViewToken;
}

// Dependency-light lifecycle kernel shared by production and the producer
// harness. A submitted batch may only be released after its owner joins it.
struct RtPathTraceCommittedCaptureBatchContract
{
    std::uint64_t captureToken = 0;
    std::uint32_t capacity = 0;
    std::uint32_t queuedViews = 0;
    std::size_t ownedBytes = 0;
    bool active = false;
    bool accepting = false;
    bool submitted = false;
    bool joined = false;
};

inline bool RtPathTraceCommittedCaptureBegin(
    RtPathTraceCommittedCaptureBatchContract& batch,
    std::uint32_t capacity,
    std::uint64_t captureToken = 1)
{
    if (batch.active)
    {
        batch.accepting = false;
        return false;
    }
    if (captureToken == 0)
    {
        return false;
    }
    batch = RtPathTraceCommittedCaptureBatchContract();
    batch.capacity = capacity;
    batch.captureToken = captureToken;
    batch.active = true;
    batch.accepting = true;
    return true;
}

inline std::uint64_t RtPathTraceCommittedCaptureNextToken(
    std::uint64_t& lastToken)
{
    ++lastToken;
    if (lastToken == 0)
    {
        ++lastToken;
    }
    return lastToken;
}

inline bool RtPathTraceCommittedCaptureKeyMatches(
    std::uint64_t productViewIdentity,
    std::int32_t productSurfaceCount,
    std::uint64_t productCaptureToken,
    std::uint64_t expectedViewIdentity,
    std::int32_t expectedSurfaceCount,
    std::uint64_t expectedCaptureToken)
{
    return productViewIdentity == expectedViewIdentity &&
        productSurfaceCount == expectedSurfaceCount &&
        productCaptureToken != 0 &&
        productCaptureToken == expectedCaptureToken;
}

inline bool RtPathTraceCommittedCaptureTelemetryDue(
    std::uint64_t captureToken,
    std::uint32_t interval = 120)
{
    return captureToken != 0 && interval != 0 &&
        (captureToken == 1 || (captureToken % interval) == 0);
}

inline bool RtPathTraceCommittedCaptureReserve(
    RtPathTraceCommittedCaptureBatchContract& batch,
    std::size_t bytes,
    std::size_t cap)
{
    if (!batch.active || !batch.accepting || batch.submitted || batch.joined ||
        bytes > cap ||
        batch.ownedBytes > cap - bytes)
    {
        return false;
    }
    batch.ownedBytes += bytes;
    return true;
}

inline bool RtPathTraceCommittedCaptureQueue(
    RtPathTraceCommittedCaptureBatchContract& batch)
{
    if (!batch.active || !batch.accepting || batch.submitted || batch.joined ||
        batch.queuedViews >= batch.capacity)
    {
        return false;
    }
    ++batch.queuedViews;
    return true;
}

inline bool RtPathTraceCommittedCaptureSurfaceShapeValid(
    const RtPathTraceCommittedGeometrySurface& surface,
    std::uint32_t expectedOrdinal,
    std::uint32_t vertexCapacity,
    std::uint32_t indexCapacity)
{
    return surface.complete && surface.ordinal == expectedOrdinal &&
        surface.source == RtPathTraceCommittedGeometrySource::CpuTriArrays &&
        surface.vertexOffset <= vertexCapacity &&
        surface.vertexCount <= vertexCapacity - surface.vertexOffset &&
        surface.indexOffset <= indexCapacity &&
        surface.indexCount <= indexCapacity - surface.indexOffset;
}

inline std::uint64_t RtPathTraceCommittedCaptureFallbackReasonCount(
    const RtPathTraceCommittedCaptureTelemetry& telemetry)
{
    return static_cast<std::uint64_t>(telemetry.fallbackInvalidView) +
        telemetry.fallbackListUnavailable +
        telemetry.fallbackViewCapacity + telemetry.fallbackPreflight +
        telemetry.fallbackBudget + telemetry.fallbackCopy +
        telemetry.fallbackNotAccepting + telemetry.fallbackIncomplete;
}

inline bool RtPathTraceCommittedCaptureViewTelemetryReconciles(
    const RtPathTraceCommittedCaptureTelemetry& telemetry)
{
    return static_cast<std::uint64_t>(telemetry.observedViews) ==
            static_cast<std::uint64_t>(telemetry.completeViews) +
                telemetry.fallbackViews &&
        static_cast<std::uint64_t>(telemetry.queuedViews) ==
            static_cast<std::uint64_t>(telemetry.completeViews) +
                telemetry.fallbackIncomplete &&
        static_cast<std::uint64_t>(telemetry.fallbackViews) ==
            RtPathTraceCommittedCaptureFallbackReasonCount(telemetry);
}

inline bool RtPathTraceCommittedCaptureCompareTelemetryReconciles(
    const RtPathTraceCommittedCaptureTelemetry& telemetry)
{
    return static_cast<std::uint64_t>(telemetry.compareCalls) ==
        static_cast<std::uint64_t>(telemetry.compareSourceExcluded) +
            telemetry.compareExact + telemetry.compareMismatch;
}

enum class RtPathTraceCommittedCaptureCompareTerminal : std::uint8_t
{
    SourceExcluded,
    Exact,
    Mismatch
};

inline void RtPathTraceCommittedCaptureRecordCompare(
    RtPathTraceCommittedCaptureTelemetry& telemetry,
    RtPathTraceCommittedCaptureCompareTerminal terminal,
    bool shapeMismatch = false,
    bool vertexMismatch = false,
    bool indexMismatch = false,
    bool counterMismatch = false)
{
    ++telemetry.compareCalls;
    switch (terminal)
    {
    case RtPathTraceCommittedCaptureCompareTerminal::SourceExcluded:
        ++telemetry.compareSourceExcluded;
        break;
    case RtPathTraceCommittedCaptureCompareTerminal::Exact:
        ++telemetry.compareExact;
        break;
    case RtPathTraceCommittedCaptureCompareTerminal::Mismatch:
        ++telemetry.compareMismatch;
        telemetry.compareShapeMismatch += shapeMismatch ? 1u : 0u;
        telemetry.compareVertexMismatch += vertexMismatch ? 1u : 0u;
        telemetry.compareIndexMismatch += indexMismatch ? 1u : 0u;
        telemetry.compareCounterMismatch += counterMismatch ? 1u : 0u;
        break;
    }
}

inline bool RtPathTraceCommittedCaptureMayRelease(
    const RtPathTraceCommittedCaptureBatchContract& batch)
{
    return !batch.submitted || batch.joined;
}

bool RtPathTraceCommittedGeometryProductShapeValid(
    const RtPathTraceCommittedGeometryProduct* product,
    const viewDef_t* viewDef,
    std::int32_t expectedSurfaceCount,
    std::uint64_t expectedCaptureToken);

void BeginPathTraceCommittedCaptureFrame();
void CapturePathTraceCommittedGeometryFrontendInput(viewDef_t* viewDef);
void JoinPathTraceCommittedCaptureFrame();
void ShutdownPathTraceCommittedCaptureLane();

void ComparePathTraceCommittedDynamicGeometry(
    const viewDef_t* viewDef,
    std::int32_t surfaceIndex,
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    const PathTraceSmokeVertex* serialVertices,
    std::uint32_t serialVertexCount,
    const std::uint32_t* serialIndexes,
    std::uint32_t serialIndexCount,
    std::uint32_t serialVertexBase,
    const RtPathTraceCommittedGeometryCounters& serialCounters);
void FinalizePathTraceCommittedCaptureTelemetry(const viewDef_t* viewDef);
