#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCaptureDeriveRing.h"

#include "PathTraceCVars.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "../RenderCommon.h"

#include <limits>
#include <type_traits>

extern idCVar jobs_numThreads;

namespace {

constexpr int RT_PT_MATERIAL_CLASSIFY_MAX_SURFACES = 32768;
constexpr int RT_PT_MATERIAL_CLASSIFY_MAX_VIEWS = 32;
constexpr int RT_PT_MATERIAL_CLASSIFY_MAX_STAGES = 65536;
constexpr std::size_t RT_PT_MATERIAL_CLASSIFY_MAX_HASH_CAPACITY = 65536;
constexpr std::size_t RT_PT_MATERIAL_CLASSIFY_BATCH_OWNED_BYTES = 16u * 1024u * 1024u;

struct RtPathTraceMaterialClassifySurfaceInput
{
    std::uint32_t ordinal = 0;
    std::uint64_t materialIdentity = 0;
    RtSmokeTranslucentClassifierInput classifier;
    std::uint32_t stageOffset = 0;
    std::uint32_t stageCount = 0;
};

static_assert(std::is_trivially_copyable<
    RtPathTraceMaterialClassifySurfaceInput>::value,
    "material classifier worker input must remain owning scalar POD");

// Worker-visible state is pointer-free with respect to the renderer frontend:
// every pointer below addresses immutable frame-owned POD or worker output.
struct RtPathTraceMaterialClassifyJob
{
    const RtPathTraceMaterialClassifySurfaceInput* surfaces = nullptr;
    const RtSmokeTranslucentClassifierStageInput* stages = nullptr;
    std::uint32_t surfaceCount = 0;
    std::uint64_t* hashIdentities = nullptr;
    std::uint32_t* hashSlots = nullptr;
    std::size_t hashCapacity = 0;
    RtPathTraceMaterialClassifyProduct* product = nullptr;
    std::uint64_t cpuUs = 0;
};

struct RtPathTraceMaterialClassifyOwnerAssociation
{
    viewDef_t* viewDef = nullptr;
    RtPathTraceMaterialClassifyProduct* product = nullptr;
    RtPathTraceMaterialClassifyJob* job = nullptr;
};

struct RtPathTraceMaterialClassifyOwnerBatch
{
    RtPathTraceMaterialClassifyBatchContract contract;
    RtPathTraceMaterialClassifyOwnerAssociation views[
        RT_PT_MATERIAL_CLASSIFY_MAX_VIEWS];
    int frameNumber = -1;
    int jobsNumThreads = 1;
    int observedViews = 0;
    int observedSubviews = 0;
    int fallbackViews = 0;
    bool parityRequested = false;
};

struct RtPathTraceMaterialClassifyOwnerLane
{
    idParallelJobList* list = nullptr;
    RtPathTraceMaterialClassifyLaneContract contract;
};

RtPathTraceMaterialClassifyOwnerBatch g_materialClassifyBatch;
RtPathTraceMaterialClassifyOwnerLane g_materialClassifyLane;
int g_materialClassifySampledFrame = std::numeric_limits<int>::min();
bool g_materialClassifySampledEnabled = false;
bool g_materialClassifySampledParity = false;
int g_materialClassifySampledThreads = 1;

std::size_t PathTraceMaterialClassifyHashCapacity(int surfaceCount)
{
    std::size_t capacity = 1;
    const std::size_t required = surfaceCount > 0
        ? static_cast<std::size_t>(surfaceCount) * 2
        : 1;
    while (capacity < required)
    {
        capacity <<= 1;
    }
    return capacity;
}

bool AddPathTraceMaterialClassifyOwnedBytes(
    std::size_t count,
    std::size_t elementSize,
    std::size_t& total)
{
    if (elementSize != 0 && count >
        (RT_PT_MATERIAL_CLASSIFY_BATCH_OWNED_BYTES - total) / elementSize)
    {
        return false;
    }
    total += count * elementSize;
    return total <= RT_PT_MATERIAL_CLASSIFY_BATCH_OWNED_BYTES &&
        total <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}

void RunPathTraceMaterialClassifyJob(void* data)
{
    OPTICK_EVENT("PT Backend Job Material Classify");
    RtPathTraceMaterialClassifyJob* job =
        static_cast<RtPathTraceMaterialClassifyJob*>(data);
    if (!job || !job->product ||
        (job->surfaceCount > 0 && !job->surfaces) ||
        !job->hashIdentities || !job->hashSlots || job->hashCapacity == 0)
    {
        return;
    }

    const std::uint64_t startUs = Sys_Microseconds();
    RtPathTraceMaterialClassifyProduct& product = *job->product;
    product.materialIdentities[0] = 0;
    product.classifiers[0] = RtSmokeTranslucentClassifierInfo();
    std::uint32_t classifierCount = 1;
    for (std::uint32_t surfaceIndex = 0;
         surfaceIndex < job->surfaceCount;
         ++surfaceIndex)
    {
        const RtPathTraceMaterialClassifySurfaceInput& source =
            job->surfaces[surfaceIndex];
        RtPathTraceMaterialClassifySurface& destination =
            product.surfaces[surfaceIndex];
        destination.ordinal = source.ordinal;
        destination.materialIdentity = source.materialIdentity;
        if (!source.classifier.materialPresent)
        {
            destination.materialSlot = 0;
            continue;
        }

        std::uint32_t materialSlot = UINT32_MAX;
        bool inserted = false;
        if (!RtPathTraceMaterialClassifyFindOrInsert(
                source.materialIdentity,
                job->hashIdentities,
                job->hashSlots,
                job->hashCapacity,
                classifierCount,
                materialSlot,
                inserted))
        {
            return;
        }
        destination.materialSlot = materialSlot;
        if (!inserted)
        {
            continue;
        }

        const RtSmokeTranslucentClassifierStageInput* stages =
            source.stageCount > 0
                ? job->stages + source.stageOffset
                : nullptr;
        product.classifiers[materialSlot] =
            BuildSmokeTranslucentClassifierInfo(
                source.classifier,
                stages,
                static_cast<std::int32_t>(source.stageCount));
        product.materialIdentities[materialSlot] = source.materialIdentity;
        ++classifierCount;
    }
    product.classifierCount = classifierCount;
    product.complete = true;
    job->cpuUs = Sys_Microseconds() - startUs;
}

REGISTER_PARALLEL_JOB(
    RunPathTraceMaterialClassifyJob,
    "Path trace sealed-view material classifier derive");

void ResetPathTraceMaterialClassifyOwnerBatch()
{
    g_materialClassifyBatch = RtPathTraceMaterialClassifyOwnerBatch();
}

}

void BeginPathTraceMaterialClassifyFrame()
{
    // RenderScene is the owner boundary. A previous live batch would mean a
    // caller bypassed its mandatory post-recursion join; never reuse it.
    if (g_materialClassifyBatch.contract.active)
    {
        assert(false && "stale material-classifier batch Begin");
        RtPathTraceMaterialClassifyBatchBegin(
            g_materialClassifyLane.contract,
            g_materialClassifyBatch.contract,
            RT_PT_MATERIAL_CLASSIFY_MAX_VIEWS);
        return;
    }

    const int frameNumber = tr.frameCount;
    if (RtPathTraceMaterialClassifyConfigurationNeedsSample(
            g_materialClassifySampledFrame, frameNumber))
    {
        g_materialClassifySampledFrame = frameNumber;
        g_materialClassifySampledEnabled =
            r_pathTracing.GetInteger() != 0 &&
            r_pathTracingMaterialClassifyRing.GetInteger() != 0;
        g_materialClassifySampledParity =
            g_materialClassifySampledEnabled &&
            r_pathTracingMaterialClassifyRingParity.GetInteger() != 0;
        g_materialClassifySampledThreads = idMath::ClampInt(
            1, RT_PT_MATERIAL_CLASSIFY_MAX_VIEWS,
            jobs_numThreads.GetInteger());
    }

    if (!g_materialClassifySampledEnabled)
    {
        return;
    }

    if (!RtPathTraceMaterialClassifyBatchBegin(
            g_materialClassifyLane.contract,
            g_materialClassifyBatch.contract,
            0))
    {
        return;
    }
    g_materialClassifyBatch.frameNumber = frameNumber;
    g_materialClassifyBatch.jobsNumThreads =
        g_materialClassifySampledThreads;
    g_materialClassifyBatch.parityRequested =
        g_materialClassifySampledParity;
    if (!parallelJobManager || g_materialClassifyLane.contract.handedOff)
    {
        return;
    }

    if (!g_materialClassifyLane.list)
    {
        g_materialClassifyLane.list = parallelJobManager->AllocJobList(
            JOBLIST_RENDERER_FRONTEND,
            JOBLIST_PRIORITY_MEDIUM,
            RT_PT_MATERIAL_CLASSIFY_MAX_VIEWS,
            0,
            nullptr);
        if (g_materialClassifyLane.list)
        {
            g_materialClassifyLane.contract.listIdentity =
                reinterpret_cast<std::uintptr_t>(
                    g_materialClassifyLane.list);
            ++g_materialClassifyLane.contract.allocations;
        }
    }
    if (g_materialClassifyLane.list)
    {
        assert(!g_materialClassifyLane.list->IsSubmitted());
        g_materialClassifyBatch.contract.capacity =
            RT_PT_MATERIAL_CLASSIFY_MAX_VIEWS;
    }
}

void CapturePathTraceMaterialClassifyFrontendInput(viewDef_t* viewDef)
{
    if (!viewDef)
    {
        return;
    }
    viewDef->pathTraceMaterialClassifyProduct = nullptr;
    if (!g_materialClassifyBatch.contract.active ||
        !g_materialClassifyBatch.contract.accepting)
    {
        return;
    }

    OPTICK_EVENT("PT Capture Material Classify Input");
    ++g_materialClassifyBatch.observedViews;
    g_materialClassifyBatch.observedSubviews += viewDef->isSubview ? 1 : 0;

    const int surfaceCountInt = viewDef->numDrawSurfs;
    if (surfaceCountInt < 0 ||
        surfaceCountInt > RT_PT_MATERIAL_CLASSIFY_MAX_SURFACES ||
        (surfaceCountInt > 0 && !viewDef->drawSurfs) ||
        g_materialClassifyBatch.contract.queuedViews >=
            g_materialClassifyBatch.contract.capacity)
    {
        ++g_materialClassifyBatch.fallbackViews;
        return;
    }

    const std::size_t surfaceCount =
        static_cast<std::size_t>(surfaceCountInt);
    const std::size_t hashCapacity =
        PathTraceMaterialClassifyHashCapacity(surfaceCountInt);
    if (hashCapacity > RT_PT_MATERIAL_CLASSIFY_MAX_HASH_CAPACITY)
    {
        ++g_materialClassifyBatch.fallbackViews;
        return;
    }

    std::uint32_t totalStages = 0;
    for (int surfaceIndex = 0;
         surfaceIndex < surfaceCountInt;
         ++surfaceIndex)
    {
        const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
        const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
        const int stageCount = material ? material->GetNumStages() : 0;
        const char* name = material ? material->GetName() : nullptr;
        const std::size_t nameLength = name ? strlen(name) : 0;
        if (!RtSmokeTranslucentClassifierCaptureFits(
                nameLength, stageCount, stageCount) ||
            stageCount > RT_PT_MATERIAL_CLASSIFY_MAX_STAGES -
                static_cast<int>(totalStages))
        {
            ++g_materialClassifyBatch.fallbackViews;
            return;
        }
        totalStages += static_cast<std::uint32_t>(stageCount);
    }

    std::size_t ownedBytes = 0;
    const std::size_t outputMaterialCapacity = surfaceCount + 1;
    if (!AddPathTraceMaterialClassifyOwnedBytes(
            1, sizeof(RtPathTraceMaterialClassifyProduct), ownedBytes) ||
        !AddPathTraceMaterialClassifyOwnedBytes(
            surfaceCount, sizeof(RtPathTraceMaterialClassifySurface), ownedBytes) ||
        !AddPathTraceMaterialClassifyOwnedBytes(
            outputMaterialCapacity, sizeof(std::uint64_t), ownedBytes) ||
        !AddPathTraceMaterialClassifyOwnedBytes(
            outputMaterialCapacity,
            sizeof(RtSmokeTranslucentClassifierInfo), ownedBytes) ||
        !AddPathTraceMaterialClassifyOwnedBytes(
            surfaceCount,
            sizeof(RtPathTraceMaterialClassifySurfaceInput), ownedBytes) ||
        !AddPathTraceMaterialClassifyOwnedBytes(
            totalStages,
            sizeof(RtSmokeTranslucentClassifierStageInput), ownedBytes) ||
        !AddPathTraceMaterialClassifyOwnedBytes(
            hashCapacity, sizeof(std::uint64_t), ownedBytes) ||
        !AddPathTraceMaterialClassifyOwnedBytes(
            hashCapacity, sizeof(std::uint32_t), ownedBytes) ||
        !AddPathTraceMaterialClassifyOwnedBytes(
            1, sizeof(RtPathTraceMaterialClassifyJob), ownedBytes) ||
        !RtPathTraceMaterialClassifyBatchTryReserveOwnedBytes(
            g_materialClassifyBatch.contract,
            ownedBytes,
            RT_PT_MATERIAL_CLASSIFY_BATCH_OWNED_BYTES))
    {
        ++g_materialClassifyBatch.fallbackViews;
        return;
    }

    RtPathTraceMaterialClassifyProduct* product =
        new (R_ClearedFrameAlloc(
            sizeof(RtPathTraceMaterialClassifyProduct),
            FRAME_ALLOC_DRAW_SURFACE)) RtPathTraceMaterialClassifyProduct{};
    product->viewIdentity = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(viewDef));
    product->surfaceCount = surfaceCountInt;
    product->parityRequested = g_materialClassifyBatch.parityRequested;

    product->surfaces = surfaceCount > 0
        ? static_cast<RtPathTraceMaterialClassifySurface*>(
            R_ClearedFrameAlloc(
                static_cast<int>(surfaceCount *
                    sizeof(RtPathTraceMaterialClassifySurface)),
                FRAME_ALLOC_DRAW_SURFACE))
        : nullptr;
    product->materialIdentities = static_cast<std::uint64_t*>(
        R_ClearedFrameAlloc(
            static_cast<int>(outputMaterialCapacity * sizeof(std::uint64_t)),
            FRAME_ALLOC_DRAW_SURFACE));
    product->classifiers = static_cast<RtSmokeTranslucentClassifierInfo*>(
        R_ClearedFrameAlloc(
            static_cast<int>(outputMaterialCapacity *
                sizeof(RtSmokeTranslucentClassifierInfo)),
            FRAME_ALLOC_DRAW_SURFACE));
    RtPathTraceMaterialClassifySurfaceInput* surfaceInputs = surfaceCount > 0
        ? static_cast<RtPathTraceMaterialClassifySurfaceInput*>(
            R_ClearedFrameAlloc(
                static_cast<int>(surfaceCount *
                    sizeof(RtPathTraceMaterialClassifySurfaceInput)),
                FRAME_ALLOC_DRAW_SURFACE))
        : nullptr;
    RtSmokeTranslucentClassifierStageInput* stageInputs = totalStages > 0
        ? static_cast<RtSmokeTranslucentClassifierStageInput*>(R_FrameAlloc(
            static_cast<int>(totalStages *
                sizeof(RtSmokeTranslucentClassifierStageInput)),
            FRAME_ALLOC_DRAW_SURFACE))
        : nullptr;

    std::uint32_t stageOffset = 0;
    for (int surfaceIndex = 0;
         surfaceIndex < surfaceCountInt;
         ++surfaceIndex)
    {
        const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
        const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
        const int stageCount = material ? material->GetNumStages() : 0;
        RtPathTraceMaterialClassifySurfaceInput& input =
            surfaceInputs[surfaceIndex];
        input.ordinal = static_cast<std::uint32_t>(surfaceIndex);
        input.materialIdentity = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(material));
        input.stageOffset = stageOffset;
        input.stageCount = static_cast<std::uint32_t>(stageCount);
        if (!CaptureSmokeTranslucentClassifierInput(
                material,
                input.classifier,
                stageCount > 0 ? stageInputs + stageOffset : nullptr,
                stageCount))
        {
            ++g_materialClassifyBatch.fallbackViews;
            return;
        }
        stageOffset += static_cast<std::uint32_t>(stageCount);
    }

    RtPathTraceMaterialClassifyJob* job =
        new (R_ClearedFrameAlloc(
            sizeof(RtPathTraceMaterialClassifyJob),
            FRAME_ALLOC_DRAW_SURFACE)) RtPathTraceMaterialClassifyJob{};
    job->surfaces = surfaceInputs;
    job->stages = stageInputs;
    job->surfaceCount = static_cast<std::uint32_t>(surfaceCount);
    job->hashCapacity = hashCapacity;
    job->hashIdentities = static_cast<std::uint64_t*>(
        R_ClearedFrameAlloc(
            static_cast<int>(hashCapacity * sizeof(std::uint64_t)),
            FRAME_ALLOC_DRAW_SURFACE));
    job->hashSlots = static_cast<std::uint32_t*>(R_FrameAlloc(
        static_cast<int>(hashCapacity * sizeof(std::uint32_t)),
        FRAME_ALLOC_DRAW_SURFACE));
    job->product = product;

    const std::uint32_t queueIndex =
        g_materialClassifyBatch.contract.queuedViews;
    if (!RtPathTraceMaterialClassifyBatchTryQueue(
            g_materialClassifyBatch.contract))
    {
        ++g_materialClassifyBatch.fallbackViews;
        return;
    }
    RtPathTraceMaterialClassifyOwnerAssociation& association =
        g_materialClassifyBatch.views[queueIndex];
    association.viewDef = viewDef;
    association.product = product;
    association.job = job;
    g_materialClassifyLane.list->AddJob(
        RunPathTraceMaterialClassifyJob, job);
}

void JoinPathTraceMaterialClassifyFrame()
{
    if (!g_materialClassifyBatch.contract.active)
    {
        return;
    }

    const std::uint64_t joinStartUs = Sys_Microseconds();
    std::uint64_t jobCpuSumUs = 0;
    std::uint64_t jobCpuMaxUs = 0;
    int completeViews = 0;
    const std::uint32_t queuedViews =
        g_materialClassifyBatch.contract.queuedViews;
    if (queuedViews > 0)
    {
        {
            OPTICK_EVENT("PT Capture Material Classify Submit");
            g_materialClassifyLane.list->Submit(
                nullptr, g_materialClassifyBatch.jobsNumThreads);
            RtPathTraceMaterialClassifyBatchNoteSubmitted(
                g_materialClassifyBatch.contract);
        }
        {
            OPTICK_EVENT("PT Capture Material Classify Join");
            // This is the single post-recursion owner join. No frame-owned input
            // can rotate until every launched worker has completed.
            g_materialClassifyLane.list->Wait();
            RtPathTraceMaterialClassifyBatchNoteJoined(
                g_materialClassifyBatch.contract);
        }

        for (std::uint32_t viewIndex = 0;
             viewIndex < queuedViews;
             ++viewIndex)
        {
            RtPathTraceMaterialClassifyOwnerAssociation& association =
                g_materialClassifyBatch.views[viewIndex];
            jobCpuSumUs += association.job->cpuUs;
            if (association.job->cpuUs > jobCpuMaxUs)
            {
                jobCpuMaxUs = association.job->cpuUs;
            }
            if (RtPathTraceMaterialClassifyProductShapeValid(
                    association.product,
                    association.viewDef,
                    association.viewDef->numDrawSurfs))
            {
                association.viewDef->pathTraceMaterialClassifyProduct =
                    association.product;
                ++completeViews;
            }
            else
            {
                ++g_materialClassifyBatch.fallbackViews;
            }
        }
    }

    assert(RtPathTraceMaterialClassifyBatchMayRelease(
        g_materialClassifyBatch.contract));
    assert(!g_materialClassifyLane.list ||
        !g_materialClassifyLane.list->IsSubmitted());
    RtPathTraceMaterialClassifyLaneNoteBatchCompleted(
        g_materialClassifyLane.contract,
        g_materialClassifyBatch.contract);

    const std::uint64_t joinFinalizeUs = Sys_Microseconds() - joinStartUs;
    const int viewDenominator = g_materialClassifyBatch.observedViews;
    const int completeRatePermille = viewDenominator > 0
        ? completeViews * 1000 / viewDenominator
        : 0;
    const int fallbackRatePermille = viewDenominator > 0
        ? g_materialClassifyBatch.fallbackViews * 1000 / viewDenominator
        : 0;
    if (g_materialClassifyBatch.parityRequested ||
        (g_materialClassifyBatch.frameNumber % 120) == 0)
    {
        common->Printf(
            "PathTracePrimaryPass: materialClassifyForkJoin frame=%d views=%d subviews=%d queued=%u complete=%d fallback=%d completePermille=%d fallbackPermille=%d jobsThreads=%d jobCpuSumUs=%llu jobCpuMaxUs=%llu joinFinalizeUs=%llu listAllocations=%u listReuses=%u batchOwnedBytes=%llu batchOwnedHighWater=%llu\n",
            g_materialClassifyBatch.frameNumber,
            g_materialClassifyBatch.observedViews,
            g_materialClassifyBatch.observedSubviews,
            queuedViews,
            completeViews,
            g_materialClassifyBatch.fallbackViews,
            completeRatePermille,
            fallbackRatePermille,
            g_materialClassifyBatch.jobsNumThreads,
            static_cast<unsigned long long>(jobCpuSumUs),
            static_cast<unsigned long long>(jobCpuMaxUs),
            static_cast<unsigned long long>(joinFinalizeUs),
            g_materialClassifyLane.contract.allocations,
            g_materialClassifyLane.contract.reuses,
            static_cast<unsigned long long>(
                g_materialClassifyBatch.contract.ownedBytes),
            static_cast<unsigned long long>(
                g_materialClassifyLane.contract.ownedBytesHighWater));
    }
    ResetPathTraceMaterialClassifyOwnerBatch();
}

void ShutdownPathTraceMaterialClassifyLane()
{
    const bool idle = RtPathTraceMaterialClassifyLaneCanHandoff(
        g_materialClassifyLane.contract,
        g_materialClassifyBatch.contract);
    assert(idle);
    if (!idle)
    {
        return;
    }
    assert(!g_materialClassifyLane.list ||
        !g_materialClassifyLane.list->IsSubmitted());

    idParallelJobList* list = g_materialClassifyLane.list;
    if (!RtPathTraceMaterialClassifyLaneNoteHandoff(
            g_materialClassifyLane.contract,
            g_materialClassifyBatch.contract))
    {
        return;
    }
    g_materialClassifyLane.list = nullptr;
    if (list && parallelJobManager)
    {
        // The caller has stopped Game/Draw and established sole MainThread
        // ownership. This is the only cross-owner operation on the list.
        parallelJobManager->FreeJobList(list);
    }
}

const RtSmokeTranslucentClassifierInfo* PathTraceMaterialClassifierForSurface(
    const RtPathTraceMaterialClassifyProduct* product,
    int surfaceIndex)
{
    if (!product || !product->complete || surfaceIndex < 0 ||
        surfaceIndex >= product->surfaceCount)
    {
        return nullptr;
    }
    const RtPathTraceMaterialClassifySurface& surface =
        product->surfaces[surfaceIndex];
    if (surface.ordinal != static_cast<std::uint32_t>(surfaceIndex) ||
        surface.materialSlot >= product->classifierCount)
    {
        return nullptr;
    }
    return &product->classifiers[surface.materialSlot];
}
