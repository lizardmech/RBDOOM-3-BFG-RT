#pragma once

#include <cstddef>
#include <cstdint>

struct RtPathTraceCommittedSemanticConfig
{
    bool recordAllInstanceClasses = false;
    bool removeRoutedRigidDynamic = false;
    bool rigidRouteEmissiveCards = false;
    std::uint32_t admissionMaxSurfaces = 0;
    std::uint64_t admissionMaxBytes = 0;
    std::uint64_t configFingerprint = 0;
    bool configComplete = false;
};

struct RtPathTraceCurrentSemanticConfig
{
    bool recordAllInstanceClasses;
    bool removeRoutedRigidDynamic;
    bool rigidRouteEmissiveCards;
    std::uint32_t admissionMaxSurfaces;
    std::uint64_t admissionMaxBytes;
    bool configComplete;
};

inline std::uint64_t RtPathTraceSemanticConfigFingerprintBegin() noexcept
{
    return 14695981039346656037ull;
}

inline void RtPathTraceSemanticConfigFingerprintAppend(
    std::uint64_t& fingerprint, const void* bytes, std::size_t count) noexcept
{
    const std::uint8_t* source = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t index = 0; index < count; ++index)
    {
        fingerprint ^= source[index];
        fingerprint *= 1099511628211ull;
    }
}

inline bool RtPathTraceCommittedSemanticConfigMatches(
    const RtPathTraceCommittedSemanticConfig& committed,
    bool recordAllInstanceClasses, bool removeRoutedRigidDynamic,
    bool rigidRouteEmissiveCards,
    std::uint32_t admissionMaxSurfaces,
    std::uint64_t admissionMaxBytes,
    std::uint64_t configFingerprint) noexcept
{
    return committed.configComplete && committed.configFingerprint != 0 &&
        configFingerprint == committed.configFingerprint &&
        recordAllInstanceClasses == committed.recordAllInstanceClasses &&
        removeRoutedRigidDynamic == committed.removeRoutedRigidDynamic &&
        rigidRouteEmissiveCards == committed.rigidRouteEmissiveCards &&
        admissionMaxSurfaces == committed.admissionMaxSurfaces &&
        admissionMaxBytes == committed.admissionMaxBytes;
}

inline bool RtPathTraceCommittedSemanticConfigMatches(
    const RtPathTraceCurrentSemanticConfig& current,
    const RtPathTraceCommittedSemanticConfig& committed) noexcept
{
    return current.configComplete && committed.configComplete &&
        committed.configFingerprint != 0 &&
        current.recordAllInstanceClasses ==
            committed.recordAllInstanceClasses &&
        current.removeRoutedRigidDynamic ==
            committed.removeRoutedRigidDynamic &&
        current.rigidRouteEmissiveCards ==
            committed.rigidRouteEmissiveCards &&
        current.admissionMaxSurfaces == committed.admissionMaxSurfaces &&
        current.admissionMaxBytes == committed.admissionMaxBytes;
}
