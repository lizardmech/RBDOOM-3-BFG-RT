#pragma once

#include <cstddef>
#include <cstdint>

// Pointer-free authority for resolving the ordered model-surface ordinal used
// by both runtime-material variants and rigid instance identity.  Token values
// are opaque same-generation parity values; this kernel never dereferences
// them.
template<typename TokenAt>
inline std::int32_t ResolvePathTraceModelSurfaceIndexFromOrderedTokens(
    std::int32_t requestedIndex,
    std::uint64_t currentTriToken,
    std::size_t tokenCount,
    const TokenAt& tokenAt)
{
    if (requestedIndex >= 0)
    {
        return requestedIndex;
    }
    for (std::size_t index = 0; index < tokenCount; ++index)
    {
        if (tokenAt(index) == currentTriToken)
        {
            return index <= static_cast<std::size_t>(INT32_MAX)
                ? static_cast<std::int32_t>(index) : -1;
        }
    }
    return -1;
}

inline std::int32_t ResolvePathTraceModelSurfaceIndexFromPod(
    std::int32_t requestedIndex,
    std::uint64_t currentTriToken,
    const std::uint64_t* orderedGeometryTokens,
    std::size_t tokenCount)
{
    if (requestedIndex < 0 && orderedGeometryTokens == nullptr)
    {
        return -1;
    }
    return ResolvePathTraceModelSurfaceIndexFromOrderedTokens(
        requestedIndex, currentTriToken, tokenCount,
        [orderedGeometryTokens](std::size_t index)
        {
            return orderedGeometryTokens[index];
        });
}

enum class RtPathTraceSerialRigidSnapshotState : std::uint8_t
{
    Inactive,
    ActiveInputMismatch,
    ActiveInvalidTokenSpan,
    ActiveResolved
};

struct RtPathTraceSerialRigidSnapshotResult
{
    RtPathTraceSerialRigidSnapshotState state =
        RtPathTraceSerialRigidSnapshotState::Inactive;
    std::int32_t resolvedModelSurfaceIndex = -1;
    bool resolvedSurfaceMatchesCurrent = false;
    bool comparedCall = false;
    bool useSnapshot = false;
    bool allowLiveFallback = true;
};

// Shared control-flow authority for the serial rigid-identity bridge.  Once an
// oracle row is active, input or token-span failure is a compared failure and
// can never reopen the legacy live-model fallback for that call.
inline RtPathTraceSerialRigidSnapshotResult
PlanPathTraceSerialRigidSnapshotUseFromPod(
    bool oracleRowActive,
    std::uint64_t snapshotModelBits,
    std::uint64_t snapshotModelEpoch,
    std::int32_t snapshotRequestedIndex,
    std::uint64_t snapshotCurrentTriToken,
    std::uint64_t currentModelBits,
    std::uint64_t currentModelEpoch,
    std::int32_t currentRequestedIndex,
    std::uint64_t currentTriToken,
    bool tokenSpanValid,
    const std::uint64_t* orderedGeometryTokens,
    std::size_t tokenCount)
{
    RtPathTraceSerialRigidSnapshotResult result;
    if (!oracleRowActive)
    {
        return result;
    }

    result.comparedCall = true;
    result.allowLiveFallback = false;
    if (snapshotModelBits != currentModelBits ||
        snapshotModelEpoch != currentModelEpoch ||
        snapshotRequestedIndex != currentRequestedIndex ||
        snapshotCurrentTriToken != currentTriToken)
    {
        result.state = RtPathTraceSerialRigidSnapshotState::ActiveInputMismatch;
        return result;
    }
    if (!tokenSpanValid)
    {
        result.state = RtPathTraceSerialRigidSnapshotState::ActiveInvalidTokenSpan;
        return result;
    }

    result.state = RtPathTraceSerialRigidSnapshotState::ActiveResolved;
    result.useSnapshot = true;
    result.resolvedModelSurfaceIndex = ResolvePathTraceModelSurfaceIndexFromPod(
        currentRequestedIndex, currentTriToken,
        orderedGeometryTokens, tokenCount);
    result.resolvedSurfaceMatchesCurrent =
        result.resolvedModelSurfaceIndex >= 0 &&
        static_cast<std::size_t>(result.resolvedModelSurfaceIndex) < tokenCount &&
        orderedGeometryTokens[result.resolvedModelSurfaceIndex] == currentTriToken;
    return result;
}
