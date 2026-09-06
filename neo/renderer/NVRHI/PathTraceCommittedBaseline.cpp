#include "PathTraceCommittedBaseline.h"

#include <atomic>
#include <mutex>

namespace
{
std::atomic<std::uint64_t> g_nextSealedPrimaryViewToken{1};
std::atomic<std::uint64_t> g_lastSealedPrimaryViewToken{0};
std::atomic<std::uint64_t> g_nextRenderWorldLifecycleGeneration{1};
std::mutex g_committedBaselineMutex;
RtPathTraceCommittedBaselineRendezvous g_committedBaselineRendezvous;
RtPathTraceCommittedPlanningBaseline g_committedBaseline;
bool g_committedBaselineReady = false;

std::uint64_t NextNonZero(std::atomic<std::uint64_t>& value) noexcept
{
    std::uint64_t result = value.fetch_add(1, std::memory_order_relaxed);
    if (result == 0)
    {
        result = value.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}
}

std::uint64_t NextPathTraceSealedPrimaryViewToken(
    std::uint64_t* predecessorToken) noexcept
{
    const std::uint64_t token = NextNonZero(g_nextSealedPrimaryViewToken);
    const std::uint64_t predecessor = g_lastSealedPrimaryViewToken.exchange(
        token, std::memory_order_acq_rel);
    if (predecessorToken != nullptr)
    {
        *predecessorToken = predecessor;
    }
    return token;
}

std::uint64_t NextPathTraceRenderWorldLifecycleGeneration() noexcept
{
    return NextNonZero(g_nextRenderWorldLifecycleGeneration);
}

std::uint64_t PathTraceCommittedBaselineBarrierGeneration() noexcept
{
    std::lock_guard<std::mutex> lock(g_committedBaselineMutex);
    return g_committedBaselineRendezvous.BarrierGeneration();
}

void PublishPathTracePrimaryViewDtoLineagePhase1(
    const RtPathTracePrimaryViewDtoLineage& dto) noexcept
{
    std::lock_guard<std::mutex> lock(g_committedBaselineMutex);
    g_committedBaselineRendezvous.PublishPrimaryDto(dto);
}

bool PublishPathTraceCommittedPlanningBaselinePhase1(
    RtPathTraceCommittedPlanningBaseline&& baseline) noexcept
{
    try
    {
        if (!baseline.Valid())
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(g_committedBaselineMutex);
        const std::uint64_t before =
            g_committedBaselineRendezvous.BaselinePublicationSerial();
        g_committedBaselineRendezvous.PublishBaseline(baseline.epoch);
        if (g_committedBaselineRendezvous.BaselinePublicationSerial() == before)
        {
            return false;
        }
        g_committedBaseline = std::move(baseline);
        g_committedBaselineReady = true;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void DrainPathTraceCommittedBaselinePhase1(
    RtPathTraceCommittedBaselineDrainReason reason) noexcept
{
    std::lock_guard<std::mutex> lock(g_committedBaselineMutex);
    g_committedBaseline = RtPathTraceCommittedPlanningBaseline{};
    g_committedBaselineReady = false;
    g_committedBaselineRendezvous.Drain(reason);
}

bool CopyPathTraceCommittedSemanticConfigForSourcePhase1(
    const RtPathTracePrimaryViewDtoLineage& sourceLineage,
    RtPathTraceCommittedSemanticConfig& output) noexcept
{
    std::lock_guard<std::mutex> lock(g_committedBaselineMutex);
    if (!g_committedBaselineReady || !g_committedBaseline.Valid() ||
        !sourceLineage.primaryView || sourceLineage.sealedViewToken == 0 ||
        sourceLineage.sealedViewToken - 1 !=
            g_committedBaseline.epoch.sealedPrimaryViewToken ||
        sourceLineage.predecessorCommittedViewToken !=
            g_committedBaseline.epoch.committedViewToken ||
        sourceLineage.frameIndex == 0 ||
        sourceLineage.frameIndex - 1 !=
            g_committedBaseline.epoch.baselineFrameIndex ||
        sourceLineage.barrierGeneration !=
            g_committedBaseline.epoch.barrierGeneration ||
        !RtPathTraceCommittedMapIdentityMatches(
            g_committedBaseline.epoch, sourceLineage))
    {
        return false;
    }
    output = g_committedBaseline.semanticConfig;
    return output.configComplete && output.configFingerprint != 0;
}
