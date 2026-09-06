#pragma once

#include "PathTraceCommittedSemanticFacts.h"

#include <cstddef>
#include <cstdint>

enum class RtPathTraceCommittedSemanticFinalizeReject : std::uint8_t
{
    None,
    BaselineInvalid,
    SourceFactsAlreadyDerived,
    SourceCarrierInvalid,
    SourceShapeInvalid,
    TokenZero,
    TokenMismatch,
    LineageInvalid,
    FrameStale,
    FrameTooNew,
    MapNameMismatch,
    MapTimeMismatch,
    MapLoadSerialMismatch,
    WorldLifecycleMismatch,
    BarrierMismatch,
    BaselineSplitGate,
    BaselineAdmissionRoutes,
    CaptureAllocationSeam,
    ProducerAllocationSeam,
    TransportedFingerprintZero,
    TransportedFingerprintMismatch,
    CurrentConfigIncomplete,
    CurrentConfigMismatch,
    CurrentSplitGate,
    CurrentAdmissionRoutes,
    BridgeFailure,
    BridgeBadAlloc,
    BridgeLengthError,
    FingerprintMismatch,
    FinalizerFailure,
    CanonicalQueryViolation,
    DerivedFactsIncomplete
};

struct RtPathTraceCommittedSemanticFinalizeInput
{
    const RtPathTraceCommittedGeometryProduct& product;
    const RtPathTracePrimaryViewDtoLineage& lineage;
    const RtPathTraceCommittedPlanningBaseline& baseline;
    const RtPathTraceCurrentSemanticConfig& currentConfig;
    int currentProducerMode;
    int currentProducerLaneMask;
    bool currentSkinnedCaptureSplitGate;
    bool currentAdmissionRoutesPresent;
    bool currentApplyGateEnabled;
    bool currentAcceptedSkinnedBuildLive;
};

struct RtPathTraceCommittedSemanticFinalizeResult
{
    RtPathTraceCommittedSemanticFinalizeReject rejection =
        RtPathTraceCommittedSemanticFinalizeReject::None;
    std::size_t prechecksPassed = 0;
    std::size_t finalizerCalls = 0;
    std::size_t rigidReadyQueries = 0;
    std::size_t residentReadyQueries = 0;
    std::size_t canonicalQueryViolations = 0;
    std::size_t derivedSurfaceCount = 0;
    std::uint64_t recomputedConfigFingerprint = 0;
    bool accepted = false;
};

struct RtPathTraceCommittedSemanticFinalizeTestSeam
{
    RtPathTracePrimarySemanticBridgeTestSeam* bridge = nullptr;
    bool invalidateFirstSurfaceBeforeFinalize = false;
    bool injectCanonicalQueryViolation = false;
};

RtPathTraceCommittedSemanticFinalizeResult
FinalizePathTraceCommittedPrimarySemanticP2b(
    const RtPathTraceCommittedSemanticFinalizeInput& input,
    RtPathTracePrimarySemanticScratch& output,
    const RtPathTraceCommittedSemanticFinalizeTestSeam* testSeam = nullptr)
    noexcept;
