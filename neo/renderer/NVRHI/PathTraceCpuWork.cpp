#include "PathTraceCpuWork.h"

bool RtPathTraceCpuWorkGenerationEquals(
    const RtPathTraceCpuWorkGeneration& a,
    const RtPathTraceCpuWorkGeneration& b)
{
    return
        a.frameIndex == b.frameIndex &&
        a.sceneGeneration == b.sceneGeneration &&
        a.geometryGeneration == b.geometryGeneration &&
        a.materialGeneration == b.materialGeneration &&
        a.lightGeneration == b.lightGeneration;
}

void RtPathTraceCpuWorkPublishSnapshot(
    RtPathTraceCpuWorkState& state,
    const RtPathTraceCpuWorkGeneration& generation)
{
    state.currentGeneration = generation;
    state.pendingGeneration = generation;
    state.hasPending = true;
}

void RtPathTraceCpuWorkPublishCompletedResult(
    RtPathTraceCpuWorkState& state,
    const RtPathTraceCpuWorkResultEnvelope& result)
{
    if (!result.completed)
    {
        return;
    }

    state.pendingResult.result = result;
    state.pendingResult.valid = true;
    state.hasPending = true;
}

RtPathTraceCpuWorkFrameDecision RtPathTraceCpuWorkAcceptLatest(
    RtPathTraceCpuWorkState& state,
    const RtPathTraceCpuWorkGeneration& expectedGeneration,
    const RtPathTraceCpuWorkResultEnvelope* latestResult,
    bool synchronousFallbackAllowed)
{
    RtPathTraceCpuWorkFrameDecision decision;
    state.currentGeneration = expectedGeneration;
    const bool latestResultIsPending =
        latestResult == nullptr &&
        state.pendingResult.valid;
    if (!latestResult && state.pendingResult.valid)
    {
        latestResult = &state.pendingResult.result;
    }
    const bool latestResultMatchesPending =
        latestResult &&
        state.pendingResult.valid &&
        state.pendingResult.result.completed == latestResult->completed &&
        RtPathTraceCpuWorkGenerationEquals(state.pendingResult.result.generation, latestResult->generation);

    if (!latestResult || !latestResult->completed)
    {
        if (state.currentResult.valid &&
            state.currentResult.result.completed &&
            RtPathTraceCpuWorkGenerationEquals(state.currentResult.result.generation, expectedGeneration))
        {
            decision.accepted = true;
            decision.reusedCurrent = true;
            state.acceptedGeneration = state.currentResult.result.generation;
            state.lastAcceptedTiming = state.currentResult.result.timing;
            state.hasAccepted = true;
            state.hasPending = false;
            return decision;
        }

        decision.lateFallback = true;
        decision.syncFallback = synchronousFallbackAllowed;
        ++state.lateResultCount;
        if (synchronousFallbackAllowed)
        {
            ++state.syncFallbackCount;
        }
        return decision;
    }

    if (!RtPathTraceCpuWorkGenerationEquals(latestResult->generation, expectedGeneration))
    {
        decision.staleRejected = true;
        decision.syncFallback = synchronousFallbackAllowed;
        ++state.rejectedStaleResultCount;
        if (latestResultIsPending || latestResultMatchesPending)
        {
            state.pendingResult = RtPathTraceCpuWorkResultSlot();
            state.hasPending = false;
        }
        if (synchronousFallbackAllowed)
        {
            ++state.syncFallbackCount;
        }
        return decision;
    }

    decision.accepted = true;
    state.acceptedGeneration = latestResult->generation;
    state.previousResult = state.currentResult;
    state.currentResult.result = *latestResult;
    state.currentResult.valid = true;
    if (state.pendingResult.valid &&
        RtPathTraceCpuWorkGenerationEquals(state.pendingResult.result.generation, latestResult->generation))
    {
        state.pendingResult = RtPathTraceCpuWorkResultSlot();
    }
    state.lastAcceptedTiming = latestResult->timing;
    state.hasAccepted = true;
    state.hasPending = false;
    ++state.acceptedResultCount;
    return decision;
}

bool RtPathTraceCpuWorkRecordRenderSubmit(
    RtPathTraceCpuWorkState& state,
    const RtPathTraceCpuWorkGeneration& generation,
    double renderSubmitMs)
{
    if (!state.currentResult.valid ||
        !RtPathTraceCpuWorkGenerationEquals(state.currentResult.result.generation, generation))
    {
        return false;
    }

    state.currentResult.result.timing.renderSubmitMs = renderSubmitMs;
    state.lastAcceptedTiming.renderSubmitMs = renderSubmitMs;
    return true;
}

void RtPathTraceCpuWorkDiscardStoppedWorkerGeneration(
    bool& workerGenerationValid)
{
    workerGenerationValid = false;
}

bool RtPathTraceCpuWorkShouldStartWorker(
    bool workerValid,
    bool acceptedCachedResultValid,
    bool generationAlreadyQueued)
{
    return !workerValid &&
        !acceptedCachedResultValid &&
        !generationAlreadyQueued;
}

bool RtPathTraceBackendJobListCanPopulate(
    RtPathTraceBackendJobListPhaseState state)
{
    return state == RtPathTraceBackendJobListPhaseState::Idle;
}

bool RtPathTraceBackendJobListMarkSubmitted(
    RtPathTraceBackendJobListPhaseState& state)
{
    if (!RtPathTraceBackendJobListCanPopulate(state))
    {
        return false;
    }
    state = RtPathTraceBackendJobListPhaseState::Submitted;
    return true;
}

bool RtPathTraceBackendJobListMarkWaited(
    RtPathTraceBackendJobListPhaseState& state)
{
    if (state != RtPathTraceBackendJobListPhaseState::Submitted)
    {
        return false;
    }
    state = RtPathTraceBackendJobListPhaseState::Idle;
    return true;
}
