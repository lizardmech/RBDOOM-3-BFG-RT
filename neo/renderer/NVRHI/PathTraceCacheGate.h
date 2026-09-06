#pragma once

#include "../VertexCacheHandle.h"

#include <cstdint>
#include <type_traits>

using RtSmokeCacheHandle = vertCacheHandle_t;

// Defined inline beside the authoritative vertex-cache layout in VertexCacheHandle.h.

enum class RtSmokeLiveCacheDecision : uint8_t
{
    Accept,
    Reject
};

enum class RtSmokeDiagnosticCacheDecision : uint8_t
{
    Accept,
    Reject
};

class RtSmokeLiveCacheVerdict
{
public:
    explicit constexpr RtSmokeLiveCacheVerdict(
        RtSmokeLiveCacheDecision decision = RtSmokeLiveCacheDecision::Accept)
        : decision_(decision)
    {
    }

    constexpr RtSmokeLiveCacheDecision Decision() const
    {
        return decision_;
    }

private:
    RtSmokeLiveCacheDecision decision_;
};

inline bool SmokeLiveCacheGateRejects(const RtSmokeLiveCacheVerdict& verdict)
{
    return verdict.Decision() == RtSmokeLiveCacheDecision::Reject;
}

class RtSmokeDiagnosticCacheVerdict
{
public:
    explicit constexpr RtSmokeDiagnosticCacheVerdict(
        RtSmokeDiagnosticCacheDecision decision = RtSmokeDiagnosticCacheDecision::Accept)
        : decision_(decision)
    {
    }

    constexpr RtSmokeDiagnosticCacheDecision Decision() const
    {
        return decision_;
    }

private:
    RtSmokeDiagnosticCacheDecision decision_;
};

static_assert(!std::is_convertible<RtSmokeDiagnosticCacheVerdict, RtSmokeLiveCacheVerdict>::value,
    "diagnostic cache verdicts must not convert to live verdicts");
static_assert(!std::is_constructible<RtSmokeLiveCacheVerdict, RtSmokeDiagnosticCacheVerdict>::value,
    "the live cache consumer must reject diagnostic verdicts at compile time");

enum class RtSmokeCacheEvaluationResult : uint8_t
{
    NotEvaluated,
    Current,
    NonCurrent
};

enum class RtSmokeCacheHandleState : uint8_t
{
    Absent,
    Static,
    Current,
    Previous,
    Stale,
    NotEvaluated,
    Count
};

struct RtSmokeLiveCacheSlotSample
{
    RtSmokeCacheHandle presenceHandle = 0;
    RtSmokeCacheHandle evaluatedHandle = 0;
    int evaluatedFrame = 0;
    RtSmokeCacheEvaluationResult result = RtSmokeCacheEvaluationResult::NotEvaluated;
};

struct RtSmokeLiveCacheGateSample
{
    RtSmokeLiveCacheSlotSample ambient;
    RtSmokeLiveCacheSlotSample index;
};

struct RtSmokeDiagnosticCacheGateSample
{
    RtSmokeCacheHandle drawSurfAmbientHandle = 0;
    RtSmokeCacheHandle drawSurfIndexHandle = 0;
    RtSmokeCacheHandle triAmbientHandle = 0;
    RtSmokeCacheHandle triIndexHandle = 0;
    RtSmokeCacheHandle selectedAmbientHandle = 0;
    RtSmokeCacheHandle selectedIndexHandle = 0;
    int ambientEvaluatedFrame = 0;
    int indexEvaluatedFrame = 0;
    RtSmokeCacheEvaluationResult ambientResult = RtSmokeCacheEvaluationResult::NotEvaluated;
    RtSmokeCacheEvaluationResult indexResult = RtSmokeCacheEvaluationResult::NotEvaluated;
};

struct RtSmokeDirectCacheGateReadPolicy
{
    constexpr RtSmokeCacheHandle ReadPresence(const RtSmokeCacheHandle& slot) const
    {
        return slot;
    }

    constexpr RtSmokeCacheHandle ReadEvaluated(const RtSmokeCacheHandle& slot) const
    {
        return slot;
    }

    constexpr int ReadCurrentFrame(const int& currentFrame) const
    {
        return currentFrame;
    }
};

template<typename ReadPolicy>
RtSmokeLiveCacheVerdict EvaluateSmokeLiveCacheGateKernel(
    const RtSmokeCacheHandle& ambientSlot,
    const RtSmokeCacheHandle& indexSlot,
    const int& currentFrame,
    RtSmokeLiveCacheGateSample* sample,
    ReadPolicy& reads)
{
    if (sample)
    {
        *sample = {};
    }

    const RtSmokeCacheHandle ambientPresence = reads.ReadPresence(ambientSlot);
    const bool hasAmbient = ambientPresence != 0;
    const RtSmokeCacheHandle indexPresence = reads.ReadPresence(indexSlot);
    const bool hasIndex = indexPresence != 0;
    if (sample)
    {
        sample->ambient.presenceHandle = ambientPresence;
        sample->index.presenceHandle = indexPresence;
    }

    if (hasAmbient)
    {
        const RtSmokeCacheHandle evaluatedHandle = reads.ReadEvaluated(ambientSlot);
        const int evaluatedFrame = reads.ReadCurrentFrame(currentFrame);
        const bool current = VertCacheHandleIsCurrent(evaluatedHandle, evaluatedFrame);
        if (sample)
        {
            sample->ambient.evaluatedHandle = evaluatedHandle;
            sample->ambient.evaluatedFrame = evaluatedFrame;
            sample->ambient.result = current
                ? RtSmokeCacheEvaluationResult::Current
                : RtSmokeCacheEvaluationResult::NonCurrent;
        }
        if (!current)
        {
            return RtSmokeLiveCacheVerdict(RtSmokeLiveCacheDecision::Reject);
        }
    }

    if (hasIndex)
    {
        const RtSmokeCacheHandle evaluatedHandle = reads.ReadEvaluated(indexSlot);
        const int evaluatedFrame = reads.ReadCurrentFrame(currentFrame);
        const bool current = VertCacheHandleIsCurrent(evaluatedHandle, evaluatedFrame);
        if (sample)
        {
            sample->index.evaluatedHandle = evaluatedHandle;
            sample->index.evaluatedFrame = evaluatedFrame;
            sample->index.result = current
                ? RtSmokeCacheEvaluationResult::Current
                : RtSmokeCacheEvaluationResult::NonCurrent;
        }
        if (!current)
        {
            return RtSmokeLiveCacheVerdict(RtSmokeLiveCacheDecision::Reject);
        }
    }

    return RtSmokeLiveCacheVerdict(RtSmokeLiveCacheDecision::Accept);
}

inline RtSmokeLiveCacheVerdict EvaluateSmokeLiveCacheGate(
    const RtSmokeCacheHandle& ambientSlot,
    const RtSmokeCacheHandle& indexSlot,
    const int& currentFrame,
    RtSmokeLiveCacheGateSample* sample)
{
    RtSmokeDirectCacheGateReadPolicy reads;
    return EvaluateSmokeLiveCacheGateKernel(
        ambientSlot,
        indexSlot,
        currentFrame,
        sample,
        reads);
}

RtSmokeCacheHandle SelectSmokeDiagnosticCacheHandle(
    RtSmokeCacheHandle drawSurfHandle,
    RtSmokeCacheHandle triHandle);

RtSmokeDiagnosticCacheVerdict EvaluateSmokeDiagnosticCacheGate(
    RtSmokeCacheHandle drawSurfAmbientHandle,
    RtSmokeCacheHandle drawSurfIndexHandle,
    RtSmokeCacheHandle triAmbientHandle,
    RtSmokeCacheHandle triIndexHandle,
    int currentFrame,
    RtSmokeDiagnosticCacheGateSample* sample);

RtSmokeCacheHandleState ClassifySmokeCacheHandle(
    RtSmokeCacheHandle handle,
    int currentFrame);
