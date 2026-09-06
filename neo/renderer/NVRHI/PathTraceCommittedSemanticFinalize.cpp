#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCommittedSemanticFinalize.h"
#include "PathTraceOwnerSemanticKernel.h"

#include <cstring>
#include <new>
#include <stdexcept>
#include <utility>

RtPathTraceCommittedSemanticFinalizeResult
FinalizePathTraceCommittedPrimarySemanticP2b(
    const RtPathTraceCommittedSemanticFinalizeInput& input,
    RtPathTracePrimarySemanticScratch& output,
    const RtPathTraceCommittedSemanticFinalizeTestSeam* testSeam) noexcept
{
    RtPathTraceCommittedSemanticFinalizeResult result;
    const auto reject = [&result](
        RtPathTraceCommittedSemanticFinalizeReject reason)
    {
        result.rejection = reason;
        return result;
    };
    const RtPathTraceCommittedPlanningBaseline& baseline = input.baseline;
    const RtPathTraceCommittedGeometryProduct& product = input.product;
    const RtPathTracePrimarySemanticDto& dto = product.semanticDto;
    const RtPathTracePrimaryViewDtoLineage& lineage = input.lineage;

    if (!baseline.Valid())
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::BaselineInvalid);
    }
    ++result.prechecksPassed;
    if (dto.factsDerivedSurfaceCount != 0)
    {
        return reject(
            RtPathTraceCommittedSemanticFinalizeReject::SourceFactsAlreadyDerived);
    }
    ++result.prechecksPassed;
    if (product.sealedPrimaryViewToken == 0 || dto.sealedPrimaryViewToken == 0 ||
        lineage.sealedViewToken == 0 ||
        baseline.epoch.sealedPrimaryViewToken == 0 ||
        baseline.epoch.committedViewToken == 0)
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::TokenZero);
    }
    if (dto.sealedPrimaryViewToken != product.sealedPrimaryViewToken ||
        lineage.sealedViewToken != product.sealedPrimaryViewToken ||
        product.sealedPrimaryViewToken - 1 !=
            baseline.epoch.sealedPrimaryViewToken ||
        lineage.predecessorCommittedViewToken !=
            baseline.epoch.committedViewToken)
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::TokenMismatch);
    }
    ++result.prechecksPassed;
    if (!RtPathTracePrimarySemanticDtoCarrierComplete(
            dto, product.sealedPrimaryViewToken))
    {
        return reject(
            RtPathTraceCommittedSemanticFinalizeReject::SourceCarrierInvalid);
    }
    ++result.prechecksPassed;
    if (!RtPathTracePrimarySemanticDtoShapeComplete(
            dto, product.sealedPrimaryViewToken))
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::SourceShapeInvalid);
    }
    ++result.prechecksPassed;
    if (!product.complete || !lineage.primaryView || !lineage.complete)
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::LineageInvalid);
    }
    ++result.prechecksPassed;
    if (lineage.frameIndex <= baseline.epoch.baselineFrameIndex)
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::FrameStale);
    }
    if (lineage.frameIndex - 1 != baseline.epoch.baselineFrameIndex)
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::FrameTooNew);
    }
    ++result.prechecksPassed;
    if (std::memcmp(lineage.mapName, baseline.epoch.mapName,
            sizeof(lineage.mapName)) != 0)
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::MapNameMismatch);
    }
    if (lineage.mapTimeStamp != baseline.epoch.mapTimeStamp)
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::MapTimeMismatch);
    }
    if (lineage.mapLoadSerial != baseline.epoch.mapLoadSerial)
    {
        return reject(
            RtPathTraceCommittedSemanticFinalizeReject::MapLoadSerialMismatch);
    }
    if (lineage.worldLifecycleGeneration !=
        baseline.epoch.worldLifecycleGeneration)
    {
        return reject(
            RtPathTraceCommittedSemanticFinalizeReject::WorldLifecycleMismatch);
    }
    if (!RtPathTraceCommittedMapIdentityMatches(baseline.epoch, lineage))
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::MapNameMismatch);
    }
    ++result.prechecksPassed;
    if (lineage.barrierGeneration != baseline.epoch.barrierGeneration)
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::BarrierMismatch);
    }
    ++result.prechecksPassed;
    if (baseline.dispatchGuard.baselineSkinnedSplitGate)
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::BaselineSplitGate);
    }
    if (baseline.dispatchGuard.baselineAdmissionRoutesPresent)
    {
        return reject(
            RtPathTraceCommittedSemanticFinalizeReject::BaselineAdmissionRoutes);
    }
    if (baseline.dispatchGuard.captureAllocationFailureArmed)
    {
        return reject(
            RtPathTraceCommittedSemanticFinalizeReject::CaptureAllocationSeam);
    }
    if (baseline.dispatchGuard.producerAllocationFailureArmed)
    {
        return reject(
            RtPathTraceCommittedSemanticFinalizeReject::ProducerAllocationSeam);
    }
    if (!RtPathTraceCommittedBaselineMayDispatch(baseline.dispatchGuard))
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::BaselineSplitGate);
    }
    ++result.prechecksPassed;
    if (product.configFingerprint == 0 || lineage.configFingerprint == 0)
    {
        return reject(
            RtPathTraceCommittedSemanticFinalizeReject::TransportedFingerprintZero);
    }
    if (product.configFingerprint != lineage.configFingerprint)
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::
            TransportedFingerprintMismatch);
    }
    ++result.prechecksPassed;
    if (!input.currentConfig.configComplete ||
        input.currentProducerMode < 0 || input.currentProducerMode > 2 ||
        input.currentProducerLaneMask < 0 || input.currentProducerLaneMask > 7)
    {
        return reject(
            RtPathTraceCommittedSemanticFinalizeReject::CurrentConfigIncomplete);
    }
    if (!RtPathTraceCommittedSemanticConfigMatches(
            input.currentConfig, baseline.semanticConfig))
    {
        return reject(
            RtPathTraceCommittedSemanticFinalizeReject::CurrentConfigMismatch);
    }
    if (input.currentSkinnedCaptureSplitGate)
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::CurrentSplitGate);
    }
    if (input.currentAdmissionRoutesPresent)
    {
        return reject(
            RtPathTraceCommittedSemanticFinalizeReject::CurrentAdmissionRoutes);
    }
    ++result.prechecksPassed;

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        RtPathTracePrimarySemanticScratch candidate;
        RtPathTracePrimarySemanticBridgeTestSeam* bridgeSeam =
            testSeam != nullptr ? testSeam->bridge : nullptr;
        if (!BuildPathTracePrimarySemanticScratch(dto,
                product.sealedPrimaryViewToken, baseline.semanticConfig,
                candidate, bridgeSeam))
        {
            if (bridgeSeam != nullptr && bridgeSeam->allocationCalls != 0 &&
                bridgeSeam->failure ==
                    RtPathTracePrimarySemanticBridgeFailure::BadAlloc)
            {
                return reject(
                    RtPathTraceCommittedSemanticFinalizeReject::BridgeBadAlloc);
            }
            if (bridgeSeam != nullptr && bridgeSeam->allocationCalls != 0 &&
                bridgeSeam->failure ==
                    RtPathTracePrimarySemanticBridgeFailure::LengthError)
            {
                return reject(
                    RtPathTraceCommittedSemanticFinalizeReject::BridgeLengthError);
            }
            return reject(RtPathTraceCommittedSemanticFinalizeReject::BridgeFailure);
        }
        candidate.snapshot.applyGate.enabled = input.currentApplyGateEnabled;
        candidate.snapshot.applyGate.acceptedSkinnedBuildLive =
            input.currentAcceptedSkinnedBuildLive;
        result.recomputedConfigFingerprint =
            RtPathTraceOwnerSemanticConfigFingerprint(candidate.snapshot,
                input.currentProducerMode, input.currentProducerLaneMask,
                input.currentSkinnedCaptureSplitGate);
        if (result.recomputedConfigFingerprint != product.configFingerprint)
        {
            return reject(
                RtPathTraceCommittedSemanticFinalizeReject::FingerprintMismatch);
        }
        if (testSeam != nullptr &&
            testSeam->invalidateFirstSurfaceBeforeFinalize &&
            !candidate.snapshot.surfaces.empty())
        {
            candidate.snapshot.surfaces[0].semanticFactsPresent = true;
        }
        RtPathTraceCommittedSemanticFactsProvider facts(baseline);
        ++result.finalizerCalls;
        const bool finalized = FinalizePathTraceOwnerSemanticSnapshot(
            candidate.snapshot, facts, nullptr, 0, false);
        if (testSeam != nullptr && testSeam->injectCanonicalQueryViolation)
        {
            const PtCanonicalInstanceKey key{};
            facts.FindCanonicalIdentityBinding(key);
        }
        result.rigidReadyQueries = facts.RigidReadyQueryCount();
        result.residentReadyQueries = facts.ResidentReadyQueryCount();
        result.canonicalQueryViolations =
            facts.CanonicalQueryViolationCount();
        if (!finalized)
        {
            return reject(
                RtPathTraceCommittedSemanticFinalizeReject::FinalizerFailure);
        }
        if (result.canonicalQueryViolations != 0)
        {
            return reject(RtPathTraceCommittedSemanticFinalizeReject::
                CanonicalQueryViolation);
        }
        for (const RtPathTraceCaptureRawSurface& raw :
            candidate.snapshot.surfaces)
        {
            if (!raw.semanticFactsDerived || !raw.semanticFactsPresent)
            {
                return reject(RtPathTraceCommittedSemanticFinalizeReject::
                    DerivedFactsIncomplete);
            }
            ++result.derivedSurfaceCount;
        }
        if (!candidate.complete || !candidate.snapshot.complete ||
            result.derivedSurfaceCount != candidate.snapshot.surfaces.size())
        {
            return reject(RtPathTraceCommittedSemanticFinalizeReject::
                DerivedFactsIncomplete);
        }
        output = std::move(candidate);
        result.rejection = RtPathTraceCommittedSemanticFinalizeReject::None;
        result.accepted = true;
        return result;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (const std::bad_alloc&)
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::BridgeBadAlloc);
    }
    catch (const std::length_error&)
    {
        return reject(RtPathTraceCommittedSemanticFinalizeReject::BridgeLengthError);
    }
#endif
}
