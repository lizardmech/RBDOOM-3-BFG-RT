#include "PathTraceCacheGate.h"

RtSmokeCacheHandle SelectSmokeDiagnosticCacheHandle(
    RtSmokeCacheHandle drawSurfHandle,
    RtSmokeCacheHandle triHandle)
{
    return drawSurfHandle != 0 ? drawSurfHandle : triHandle;
}

RtSmokeDiagnosticCacheVerdict EvaluateSmokeDiagnosticCacheGate(
    RtSmokeCacheHandle drawSurfAmbientHandle,
    RtSmokeCacheHandle drawSurfIndexHandle,
    RtSmokeCacheHandle triAmbientHandle,
    RtSmokeCacheHandle triIndexHandle,
    int currentFrame,
    RtSmokeDiagnosticCacheGateSample* sample)
{
    if (sample)
    {
        *sample = {};
        sample->drawSurfAmbientHandle = drawSurfAmbientHandle;
        sample->drawSurfIndexHandle = drawSurfIndexHandle;
        sample->triAmbientHandle = triAmbientHandle;
        sample->triIndexHandle = triIndexHandle;
    }

    // These are intentionally retained values. Diagnostic evaluation mirrors
    // the copy helpers and never resamples either source slot.
    const RtSmokeCacheHandle selectedAmbient = SelectSmokeDiagnosticCacheHandle(
        drawSurfAmbientHandle,
        triAmbientHandle);
    const RtSmokeCacheHandle selectedIndex = SelectSmokeDiagnosticCacheHandle(
        drawSurfIndexHandle,
        triIndexHandle);
    if (sample)
    {
        sample->selectedAmbientHandle = selectedAmbient;
        sample->selectedIndexHandle = selectedIndex;
    }

    if (selectedAmbient != 0)
    {
        const bool current = VertCacheHandleIsCurrent(selectedAmbient, currentFrame);
        if (sample)
        {
            sample->ambientEvaluatedFrame = currentFrame;
            sample->ambientResult = current
                ? RtSmokeCacheEvaluationResult::Current
                : RtSmokeCacheEvaluationResult::NonCurrent;
        }
        if (!current)
        {
            return RtSmokeDiagnosticCacheVerdict(
                RtSmokeDiagnosticCacheDecision::Reject);
        }
    }

    if (selectedIndex != 0)
    {
        const bool current = VertCacheHandleIsCurrent(selectedIndex, currentFrame);
        if (sample)
        {
            sample->indexEvaluatedFrame = currentFrame;
            sample->indexResult = current
                ? RtSmokeCacheEvaluationResult::Current
                : RtSmokeCacheEvaluationResult::NonCurrent;
        }
        if (!current)
        {
            return RtSmokeDiagnosticCacheVerdict(
                RtSmokeDiagnosticCacheDecision::Reject);
        }
    }

    return RtSmokeDiagnosticCacheVerdict(
        RtSmokeDiagnosticCacheDecision::Accept);
}

RtSmokeCacheHandleState ClassifySmokeCacheHandle(
    RtSmokeCacheHandle handle,
    int currentFrame)
{
    if (handle == 0)
    {
        return RtSmokeCacheHandleState::Absent;
    }
    if (VertCacheHandleIsCurrent(handle, currentFrame))
    {
        return (handle & VERTCACHE_STATIC) != 0
            ? RtSmokeCacheHandleState::Static
            : RtSmokeCacheHandleState::Current;
    }

    const RtSmokeCacheHandle tag =
        (handle >> VERTCACHE_FRAME_SHIFT) & VERTCACHE_FRAME_MASK;
    const RtSmokeCacheHandle current =
        static_cast<RtSmokeCacheHandle>(currentFrame) & VERTCACHE_FRAME_MASK;
    const RtSmokeCacheHandle previous =
        (current + VERTCACHE_FRAME_MASK) & VERTCACHE_FRAME_MASK;
    return tag == previous
        ? RtSmokeCacheHandleState::Previous
        : RtSmokeCacheHandleState::Stale;
}
