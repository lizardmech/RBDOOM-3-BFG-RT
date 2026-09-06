#pragma once

#include <cstddef>
#include <cstdint>

struct RtSmokeTranslucentClassifierInfo;
struct viewDef_t;

struct RtPathTraceMaterialClassifySurface
{
    std::uint32_t ordinal = 0;
    std::uint32_t materialSlot = UINT32_MAX;
    std::uint64_t materialIdentity = 0;
};

// Exact-frame, immutable after the frontend join, and owned by the same frame
// arena as its viewDef. The backend never searches for or releases this product.
struct RtPathTraceMaterialClassifyProduct
{
    std::uint64_t viewIdentity = 0;
    std::int32_t surfaceCount = 0;
    std::uint32_t classifierCount = 0;
    RtPathTraceMaterialClassifySurface* surfaces = nullptr;
    std::uint64_t* materialIdentities = nullptr;
    RtSmokeTranslucentClassifierInfo* classifiers = nullptr;
    bool complete = false;
    bool parityRequested = false;
};

// One owner-thread batch spans a top-level view and every recursively produced
// subview.  It is deliberately dependency-light so the producer harness tests
// the same queue/release decisions used by production.
struct RtPathTraceMaterialClassifyBatchContract
{
    std::uint32_t capacity = 0;
    std::uint32_t queuedViews = 0;
    std::size_t ownedBytes = 0;
    bool active = false;
    bool accepting = false;
    bool submitted = false;
    bool joined = false;
};

struct RtPathTraceMaterialClassifyLaneContract
{
    std::uintptr_t listIdentity = 0;
    std::uint32_t allocations = 0;
    std::uint32_t reuses = 0;
    std::uint32_t completedBatches = 0;
    std::size_t ownedBytesHighWater = 0;
    bool handedOff = false;
};

inline bool RtPathTraceMaterialClassifyBatchBegin(
    RtPathTraceMaterialClassifyLaneContract& lane,
    RtPathTraceMaterialClassifyBatchContract& batch,
    std::uint32_t capacity)
{
    if (lane.handedOff)
    {
        return false;
    }
    if (batch.active)
    {
        // A stale Begin must make the old batch non-appendable. Its eventual
        // owner Join still drains any already queued frame-arena readers.
        batch.accepting = false;
        return false;
    }
    batch = RtPathTraceMaterialClassifyBatchContract();
    batch.capacity = capacity;
    batch.active = true;
    batch.accepting = true;
    if (lane.listIdentity != 0)
    {
        ++lane.reuses;
    }
    return true;
}

inline bool RtPathTraceMaterialClassifyBatchTryReserveOwnedBytes(
    RtPathTraceMaterialClassifyBatchContract& batch,
    std::size_t bytes,
    std::size_t cap)
{
    if (!batch.active || !batch.accepting ||
        bytes > cap || batch.ownedBytes > cap - bytes)
    {
        return false;
    }
    batch.ownedBytes += bytes;
    return true;
}

inline bool RtPathTraceMaterialClassifyConfigurationNeedsSample(
    std::int32_t sampledFrame,
    std::int32_t currentFrame)
{
    return sampledFrame != currentFrame;
}

inline bool RtPathTraceMaterialClassifyBatchTryQueue(
    RtPathTraceMaterialClassifyBatchContract& batch)
{
    if (!batch.active || !batch.accepting || batch.submitted || batch.joined ||
        batch.queuedViews >= batch.capacity)
    {
        return false;
    }
    ++batch.queuedViews;
    return true;
}

inline void RtPathTraceMaterialClassifyBatchNoteSubmitted(
    RtPathTraceMaterialClassifyBatchContract& batch)
{
    batch.submitted = batch.active && batch.queuedViews > 0;
}

inline void RtPathTraceMaterialClassifyBatchNoteJoined(
    RtPathTraceMaterialClassifyBatchContract& batch)
{
    batch.joined = batch.submitted;
}

inline bool RtPathTraceMaterialClassifyBatchMayRelease(
    const RtPathTraceMaterialClassifyBatchContract& batch)
{
    return !batch.submitted || batch.joined;
}

inline void RtPathTraceMaterialClassifyLaneNoteBatchCompleted(
    RtPathTraceMaterialClassifyLaneContract& lane,
    const RtPathTraceMaterialClassifyBatchContract& batch)
{
    ++lane.completedBatches;
    if (batch.ownedBytes > lane.ownedBytesHighWater)
    {
        lane.ownedBytesHighWater = batch.ownedBytes;
    }
}

inline bool RtPathTraceMaterialClassifyLaneCanHandoff(
    const RtPathTraceMaterialClassifyLaneContract& lane,
    const RtPathTraceMaterialClassifyBatchContract& batch)
{
    return !lane.handedOff && !batch.active && !batch.accepting &&
        batch.queuedViews == 0 && !batch.submitted && !batch.joined;
}

inline bool RtPathTraceMaterialClassifyLaneNoteHandoff(
    RtPathTraceMaterialClassifyLaneContract& lane,
    const RtPathTraceMaterialClassifyBatchContract& batch)
{
    if (!RtPathTraceMaterialClassifyLaneCanHandoff(lane, batch))
    {
        return false;
    }
    lane.handedOff = true;
    lane.listIdentity = 0;
    return true;
}

inline bool RtPathTraceMaterialClassifyProductShapeValid(
    const RtPathTraceMaterialClassifyProduct* product,
    const viewDef_t* viewDef,
    std::int32_t expectedSurfaceCount)
{
    if (!product || !viewDef || !product->complete ||
        product->viewIdentity != static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(viewDef)) ||
        product->surfaceCount != expectedSurfaceCount ||
        expectedSurfaceCount < 0 ||
        product->classifierCount == 0 ||
        product->classifierCount >
            static_cast<std::uint32_t>(expectedSurfaceCount) + 1u ||
        (expectedSurfaceCount > 0 && !product->surfaces) ||
        !product->materialIdentities || !product->classifiers ||
        product->materialIdentities[0] != 0)
    {
        return false;
    }
    for (std::int32_t surfaceIndex = 0;
         surfaceIndex < expectedSurfaceCount;
         ++surfaceIndex)
    {
        const RtPathTraceMaterialClassifySurface& surface =
            product->surfaces[surfaceIndex];
        if (surface.ordinal != static_cast<std::uint32_t>(surfaceIndex) ||
            surface.materialSlot >= product->classifierCount ||
            product->materialIdentities[surface.materialSlot] !=
                surface.materialIdentity ||
            ((surface.materialIdentity == 0) !=
                (surface.materialSlot == 0)))
        {
            return false;
        }
    }
    return true;
}

// Dependency-light open-addressing kernel used by the worker and producer
// harness. Zero is reserved for an empty material identity.
inline bool RtPathTraceMaterialClassifyFindOrInsert(
    std::uint64_t materialIdentity,
    std::uint64_t* tableIdentities,
    std::uint32_t* tableSlots,
    std::size_t tableCapacity,
    std::uint32_t newSlot,
    std::uint32_t& materialSlot,
    bool& inserted)
{
    inserted = false;
    if (materialIdentity == 0 || !tableIdentities || !tableSlots ||
        tableCapacity == 0 ||
        (tableCapacity & (tableCapacity - 1)) != 0)
    {
        return false;
    }

    std::size_t tableIndex = static_cast<std::size_t>(
        (materialIdentity >> 4) * 11400714819323198485ull) &
        (tableCapacity - 1);
    for (std::size_t probe = 0; probe < tableCapacity; ++probe)
    {
        if (tableIdentities[tableIndex] == materialIdentity)
        {
            materialSlot = tableSlots[tableIndex];
            return true;
        }
        if (tableIdentities[tableIndex] == 0)
        {
            tableIdentities[tableIndex] = materialIdentity;
            tableSlots[tableIndex] = newSlot;
            materialSlot = newSlot;
            inserted = true;
            return true;
        }
        tableIndex = (tableIndex + 1) & (tableCapacity - 1);
    }
    return false;
}

void BeginPathTraceMaterialClassifyFrame();
void CapturePathTraceMaterialClassifyFrontendInput(viewDef_t* viewDef);
void JoinPathTraceMaterialClassifyFrame();
void ShutdownPathTraceMaterialClassifyLane();
const RtSmokeTranslucentClassifierInfo* PathTraceMaterialClassifierForSurface(
    const RtPathTraceMaterialClassifyProduct* product,
    int surfaceIndex);
