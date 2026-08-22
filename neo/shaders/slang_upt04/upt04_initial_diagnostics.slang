import upt04_initial_shared;
import upt04_initial_work_contract;
import upt04_backend_rayquery;
import upt04_initial_bindings;
import upt04_hit_contract;
import upt03_reservoir_contract;
import upt04_transport_initial_bounded;
import upt04_initial_indirect;
import upt04_initial_multibounce;
import upt04_initial_reservoir_stream;
import upt04_reservoir_image;
import upt04_primary_image;
import upt04_primary_surface_live_contract;
import upt04_light_contract;
import upt04_initial_control;
import upt05_material_set_openpbr;
import upt04_random_white_hash;
import upt04_initial_direct;
import upt04_local_light_evaluation;
import upt04_local_light_evaluation_direct;
import upt04_local_light_proposal;
import upt04_emissive_replay;
import upt04_geometry_contract;
import upt04_geometry_bindings;
import upt04_geometry_decode;
import upt04_material_contract;
import upt04_material_live_contract;
import upt04_material_set_live_probe;

// This entry point is intentionally separate from every production entry.
// The first 16 words are the bring-up counters.  Words 16..19 are a
// group-reduced, order-independent signature of every packed reservoir.
[[vk::binding(23, 0)]] RWStructuredBuffer<uint> gUpt04Diagnostics;

public static const uint kUpt04DiagReceiverValid = 0u;
public static const uint kUpt04DiagReceiverInvalid = 1u;
public static const uint kUpt04DiagDirectInvalid = 2u;
public static const uint kUpt04DiagDirectValidZero = 3u;
public static const uint kUpt04DiagDirectValidPositive = 4u;
public static const uint kUpt04DiagIndirectInvalid = 5u;
public static const uint kUpt04DiagIndirectValidZero = 6u;
public static const uint kUpt04DiagIndirectValidPositive = 7u;
public static const uint kUpt04DiagSelectedPrimaryNee = 8u;
public static const uint kUpt04DiagSelectedBsdfEndpoint = 9u;
public static const uint kUpt04DiagSelectedSecondaryNee = 10u;
public static const uint kUpt04DiagContinuationRays = 11u;
public static const uint kUpt04DiagVisibilityRays = 12u;
public static const uint kUpt04DiagCanonicalEmptyWrites = 13u;
public static const uint kUpt04DiagCandidateInputs = 14u;
public static const uint kUpt04DiagRayCeilingViolations = 15u;
public static const uint kUpt04DiagReservoirHash0 = 16u;
public static const uint kUpt04DiagReservoirHash1 = 17u;
public static const uint kUpt04DiagReservoirHash2 = 18u;
public static const uint kUpt04DiagReservoirHash3 = 19u;
public static const uint kUpt04DiagDirectRejectSelection = 20u;
public static const uint kUpt04DiagDirectRejectRecord = 21u;
public static const uint kUpt04DiagDirectRejectEmissiveReplay = 22u;
public static const uint kUpt04DiagDirectRejectAnalyticSample = 23u;
public static const uint kUpt04DiagDirectRejectMaterial = 24u;
public static const uint kUpt04DiagDirectRejectPdf = 25u;
public static const uint kUpt04DiagDirectRejectOther = 26u;
public static const uint kUpt04DiagEmissiveAttempts = 27u;
public static const uint kUpt04DiagEmissiveSelectionReject = 28u;
public static const uint kUpt04DiagEmissiveIdentityReject = 29u;
public static const uint kUpt04DiagEmissiveResolveReject = 30u;
public static const uint kUpt04DiagEmissiveMaterialReject = 31u;
public static const uint kUpt04DiagEmissiveOtherReject = 32u;
public static const uint kUpt04DiagEmissiveGeometryPositive = 33u;
public static const uint kUpt04DiagEmissiveGeometryZero = 34u;
public static const uint kUpt04DiagEmissiveEndpointPositive = 35u;
public static const uint kUpt04DiagEmissiveEndpointZero = 36u;
public static const uint kUpt04DiagEmissiveTargetPositive = 37u;
public static const uint kUpt04DiagEmissiveProposalPositive = 38u;
public static const uint kUpt04DiagSelectedEmissive = 39u;
public static const uint kUpt04DiagSelectedAnalytic = 40u;
public static const uint kUpt04DiagSelectedEmissiveVisible = 41u;
public static const uint kUpt04DiagSelectedEmissiveOccluded = 42u;
public static const uint kUpt04DiagFinalEmissive = 43u;
public static const uint kUpt04DiagFinalAnalytic = 44u;
public static const uint kUpt04DiagFinalEmissiveBelow1e4 = 45u;
public static const uint kUpt04DiagFinalEmissiveBelow1e2 = 46u;
public static const uint kUpt04DiagFinalEmissiveBelow1 = 47u;
public static const uint kUpt04DiagFinalEmissiveAtLeast1 = 48u;
public static const uint kUpt04DiagFinalAnalyticBelow1e4 = 49u;
public static const uint kUpt04DiagFinalAnalyticBelow1e2 = 50u;
public static const uint kUpt04DiagFinalAnalyticBelow1 = 51u;
public static const uint kUpt04DiagFinalAnalyticAtLeast1 = 52u;
public static const uint kUpt04DiagEmissiveOccluderSameInstanceSameMaterial = 53u;
public static const uint kUpt04DiagEmissiveOccluderSameInstanceOtherMaterial = 54u;
public static const uint kUpt04DiagEmissiveOccluderOtherInstanceSameMaterial = 55u;
public static const uint kUpt04DiagEmissiveOccluderOtherInstanceOtherMaterial = 56u;
public static const uint kUpt04DiagEmissiveOccluderDecodeFailed = 57u;
public static const uint kUpt04DiagPreviousLookupAttempted = 58u;
public static const uint kUpt04DiagPreviousLookupSucceeded = 59u;
public static const uint kUpt04DiagPreviousRemapFailure = 60u;
public static const uint kUpt04DiagPreviousReplayFailure = 61u;
public static const uint kUpt04DiagPreviousTargetFailure = 62u;
public static const uint kUpt04DiagPreviousVisibilityFailure = 63u;
public static const uint kUpt04DiagPreviousDuplicateRejected = 64u;
public static const uint kUpt04DiagPreviousAdmitted = 65u;
public static const uint kUpt04DiagFreshAnalyticSelected = 66u;
public static const uint kUpt04DiagFreshEmissiveSelected = 67u;
public static const uint kUpt04DiagWeightedCdfUnavailable = 68u;
public static const uint kUpt04DiagContinuationMiss = 69u;
public static const uint kUpt04DiagContinuationHit = 70u;
public static const uint kUpt04DiagIndirectGeometryFailure = 71u;
public static const uint kUpt04DiagEndpointEmissionFailure = 72u;
public static const uint kUpt04DiagEndpointEmissionZero = 73u;
public static const uint kUpt04DiagEndpointEmissionPositive = 74u;
public static const uint kUpt04DiagReverseLookupFailure = 75u;
public static const uint kUpt04DiagReversePdfZero = 76u;
public static const uint kUpt04DiagReversePdfPositive = 77u;
public static const uint kUpt04DiagSecondarySelectionFailure = 78u;
public static const uint kUpt04DiagSecondaryMaterialFailure = 79u;
public static const uint kUpt04DiagSecondaryPdfRayFailure = 80u;
public static const uint kUpt04DiagEndpointCandidatePositive = 81u;
public static const uint kUpt04DiagSecondaryCandidatePositive = 82u;
public static const uint kUpt04DiagSecondaryVisibilityPassed = 83u;
public static const uint kUpt04DiagSecondaryVisibilityOccluded = 84u;
public static const uint kUpt04DiagSecondaryVisibilitySelfHit = 85u;
public static const uint kUpt04DiagFinalIndirectSelected = 86u;
public static const uint kUpt04DiagSecondaryMaterialIndexValid = 87u;
public static const uint kUpt04DiagSecondaryMaterialIndexInvalid = 88u;
public static const uint kUpt04DiagSecondaryMaterialForceDebug = 89u;
public static const uint kUpt04DiagSecondaryMaterialMissingFallback = 90u;
public static const uint kUpt04DiagSecondaryMaterialTexturedRgb = 91u;
public static const uint kUpt04DiagSecondaryMaterialTexturedYCoCg = 92u;
public static const uint kUpt04DiagSecondaryMaterialDescriptorOob = 93u;
public static const uint kUpt04DiagContinuationHitStatic = 94u;
public static const uint kUpt04DiagContinuationHitStaticBucket = 95u;
public static const uint kUpt04DiagContinuationHitDynamic = 96u;
public static const uint kUpt04DiagContinuationHitRigid = 97u;
public static const uint kUpt04DiagContinuationHitSkinned = 98u;
public static const uint kUpt04DiagContinuationHitUnknown = 99u;
public static const uint kUpt04DiagContinuationDecodeSuccess = 100u;
public static const uint kUpt04DiagContinuationDecodeFailureStatic = 101u;
public static const uint kUpt04DiagContinuationDecodeFailureStaticBucket = 102u;
public static const uint kUpt04DiagContinuationDecodeFailureDynamic = 103u;
public static const uint kUpt04DiagContinuationDecodeFailureRigid = 104u;
public static const uint kUpt04DiagContinuationDecodeFailureSkinned = 105u;
public static const uint kUpt04DiagContinuationDecodeFailureUnknown = 106u;
public static const uint kUpt04DiagX3ContinuationPrepared = 107u;
public static const uint kUpt04DiagX3ContinuationHit = 108u;
public static const uint kUpt04DiagX3EndpointPositive = 109u;
public static const uint kUpt04DiagX3NeePreparedPositive = 110u;
public static const uint kUpt04DiagX3NeeVisibilityPassed = 111u;
public static const uint kUpt04DiagX3EndpointSelected = 112u;
public static const uint kUpt04DiagX3NeeSelected = 113u;
public static const uint kUpt04DiagX3AnySelected = 114u;
public static const uint kUpt04DiagD0ProbeBase = 131u;
public static const uint kUpt04DiagD0ProbeMagic = 0x44305052u;
public static const uint kUpt04DiagC0ProbeBase = 147u;
public static const uint kUpt04DiagC0ProbeMagic = 0x43305052u;

public static const uint kUpt04DiagRouteStatic = 0u;
public static const uint kUpt04DiagRouteStaticBucket = 1u;
public static const uint kUpt04DiagRouteDynamic = 2u;
public static const uint kUpt04DiagRouteRigid = 3u;
public static const uint kUpt04DiagRouteSkinned = 4u;
public static const uint kUpt04DiagRouteUnknown = 5u;

groupshared uint gUpt04GroupReservoirHash0;
groupshared uint gUpt04GroupReservoirHash1;
groupshared uint gUpt04GroupReservoirHash2;
groupshared uint gUpt04GroupReservoirHash3;

public void Upt04DiagIncrement(uint counter)
{
    uint ignored;
    InterlockedAdd(gUpt04Diagnostics[counter], 1u, ignored);
}

public void Upt04DiagD0AcquisitionStages(Upt04InitialTraceWork work)
{
    const uint bits = work.directDiagnostics;
    if ((bits & kUpt04D0DiagPreviousLookupAttempted) != 0u)
        Upt04DiagIncrement(kUpt04DiagPreviousLookupAttempted);
    if ((bits & kUpt04D0DiagPreviousLookupSucceeded) != 0u)
        Upt04DiagIncrement(kUpt04DiagPreviousLookupSucceeded);
    if ((bits & kUpt04D0DiagPreviousRemapFailure) != 0u)
        Upt04DiagIncrement(kUpt04DiagPreviousRemapFailure);
    if ((bits & kUpt04D0DiagPreviousReplayFailure) != 0u)
        Upt04DiagIncrement(kUpt04DiagPreviousReplayFailure);
    if ((bits & kUpt04D0DiagPreviousTargetFailure) != 0u)
        Upt04DiagIncrement(kUpt04DiagPreviousTargetFailure);
    if ((bits & kUpt04D0DiagPreviousVisibilityFailure) != 0u)
        Upt04DiagIncrement(kUpt04DiagPreviousVisibilityFailure);
    if ((bits & kUpt04D0DiagPreviousDuplicateRejected) != 0u)
        Upt04DiagIncrement(kUpt04DiagPreviousDuplicateRejected);
    if ((bits & kUpt04D0DiagPreviousAdmitted) != 0u)
        Upt04DiagIncrement(kUpt04DiagPreviousAdmitted);
    if ((bits & kUpt04D0DiagFreshAnalyticSelected) != 0u)
        Upt04DiagIncrement(kUpt04DiagFreshAnalyticSelected);
    if ((bits & kUpt04D0DiagFreshEmissiveSelected) != 0u)
        Upt04DiagIncrement(kUpt04DiagFreshEmissiveSelected);
    if ((bits & kUpt04D0DiagWeightedCdfUnavailable) != 0u)
        Upt04DiagIncrement(kUpt04DiagWeightedCdfUnavailable);
}

public void Upt04DiagIndirectStages(Upt04IndirectHitTrialResult result)
{
    const uint bits = result.diagnostics;
    if ((bits & kUpt04IndirectDiagContinuationMiss) != 0u)
        Upt04DiagIncrement(kUpt04DiagContinuationMiss);
    if ((bits & kUpt04IndirectDiagContinuationHit) != 0u)
        Upt04DiagIncrement(kUpt04DiagContinuationHit);
    if ((bits & kUpt04IndirectDiagGeometryFailure) != 0u)
        Upt04DiagIncrement(kUpt04DiagIndirectGeometryFailure);
    if ((bits & kUpt04IndirectDiagEndpointEmissionFailure) != 0u)
        Upt04DiagIncrement(kUpt04DiagEndpointEmissionFailure);
    if ((bits & kUpt04IndirectDiagEndpointEmissionZero) != 0u)
        Upt04DiagIncrement(kUpt04DiagEndpointEmissionZero);
    if ((bits & kUpt04IndirectDiagEndpointEmissionPositive) != 0u)
        Upt04DiagIncrement(kUpt04DiagEndpointEmissionPositive);
    if ((bits & kUpt04IndirectDiagReverseLookupFailure) != 0u)
        Upt04DiagIncrement(kUpt04DiagReverseLookupFailure);
    if ((bits & kUpt04IndirectDiagReversePdfZero) != 0u)
        Upt04DiagIncrement(kUpt04DiagReversePdfZero);
    if ((bits & kUpt04IndirectDiagReversePdfPositive) != 0u)
        Upt04DiagIncrement(kUpt04DiagReversePdfPositive);
    if ((bits & kUpt04IndirectDiagSecondarySelectionFailure) != 0u)
        Upt04DiagIncrement(kUpt04DiagSecondarySelectionFailure);
    if ((bits & kUpt04IndirectDiagSecondaryMaterialFailure) != 0u)
        Upt04DiagIncrement(kUpt04DiagSecondaryMaterialFailure);
    if ((bits & kUpt04IndirectDiagSecondaryPdfRayFailure) != 0u)
        Upt04DiagIncrement(kUpt04DiagSecondaryPdfRayFailure);
    if (result.endpointCandidate.valid != 0u
        && result.endpointCandidate.candidateReady != 0u
        && result.endpointCandidate.candidateStatus == kUpt04SampleValidPositive)
        Upt04DiagIncrement(kUpt04DiagEndpointCandidatePositive);
}

public uint Upt04DiagContinuationRoute(Upt04HitFacts hit)
{
    if (hit.instanceId == 0u)
        return kUpt04DiagRouteStatic;
    if (Upt04StaticBucketRouteAvailable()
        && (hit.instanceId & ~kUpt04ShaderInstanceMask) == 0u
        && (hit.instanceId & kUpt04StaticBucketInstanceBit) != 0u)
        return kUpt04DiagRouteStaticBucket;
    if (hit.instanceId == 1u)
        return kUpt04DiagRouteDynamic;
    uint skinnedRouteIndex;
    if (Upt04SkinnedInstanceInRange(hit.instanceId, skinnedRouteIndex))
        return kUpt04DiagRouteSkinned;
    if (hit.instanceId >= 2u
        && hit.instanceId - 2u < Upt04RigidInstanceCount())
        return kUpt04DiagRouteRigid;
    return kUpt04DiagRouteUnknown;
}

public void Upt04DiagSecondaryMaterialRoute(Upt04HitFacts hit)
{
    if (hit.status != kUpt04HitTriangle)
        return;
    const uint route = Upt04DiagContinuationRoute(hit);
    Upt04DiagIncrement(kUpt04DiagContinuationHitStatic + route);
    const Upt04GeometryHitSurface surface = Upt04DecodeGeometryHit(hit);
    if (surface.valid == 0u)
    {
        Upt04DiagIncrement(kUpt04DiagContinuationDecodeFailureStatic + route);
        Upt04DiagIncrement(kUpt04DiagSecondaryMaterialIndexInvalid);
        return;
    }
    Upt04DiagIncrement(kUpt04DiagContinuationDecodeSuccess);
    if (surface.materialIndex >= gUpt04Control.materialCount)
    {
        Upt04DiagIncrement(kUpt04DiagSecondaryMaterialIndexInvalid);
        return;
    }

    Upt04DiagIncrement(kUpt04DiagSecondaryMaterialIndexValid);
    const Upt04SmokeMaterialRecord material =
        Upt04LoadMaterialRecord(surface.materialIndex);
    if ((material.flags & kUpt04MaterialForceDebugAlbedo) != 0u)
    {
        Upt04DiagIncrement(kUpt04DiagSecondaryMaterialForceDebug);
        return;
    }
    if (material.diffuseTextureIndex == kUpt04InvalidIdentity)
    {
        Upt04DiagIncrement(kUpt04DiagSecondaryMaterialMissingFallback);
        return;
    }
    if (material.diffuseTextureIndex >= gUpt04Control.logicalTextureCount)
    {
        Upt04DiagIncrement(kUpt04DiagSecondaryMaterialDescriptorOob);
        return;
    }
    Upt04DiagIncrement(
        (material.flags & kUpt04MaterialDiffuseYCoCg) != 0u
            ? kUpt04DiagSecondaryMaterialTexturedYCoCg
            : kUpt04DiagSecondaryMaterialTexturedRgb);
}

// Diagnostic-only classification of the first committed blocker for an
// emissive shadow ray. The production visibility contract is deliberately not
// changed here: this distinguishes an endpoint self-hit, a duplicated TLAS
// route, and a genuine unrelated blocker before choosing a fix.
public void Upt04DiagEmissiveOccluder(
    Upt04InitialTraceWork work,
    Upt04HitFacts hit)
{
    if (work.valid == 0u
        || work.lightType != kUpt04LightEmissiveTriangle
        || hit.status != kUpt04HitTriangle
        || work.lightIndex >= gUpt04Control.currentLightCount)
        return;

    const Upt04UnifiedLightRecord light =
        Upt04LoadCurrentLightRecord(work.lightIndex);
    Upt04ResolvedTriangle blocker;
    if (!Upt04ResolveTriangle(hit, blocker) || blocker.valid == 0u)
    {
        Upt04DiagIncrement(kUpt04DiagEmissiveOccluderDecodeFailed);
        return;
    }

    const bool sameInstance = hit.instanceId == light.instanceId;
    const bool sameMaterial = blocker.materialIndex == light.materialOrLightId;
    const uint counter = sameInstance
        ? (sameMaterial
            ? kUpt04DiagEmissiveOccluderSameInstanceSameMaterial
            : kUpt04DiagEmissiveOccluderSameInstanceOtherMaterial)
        : (sameMaterial
            ? kUpt04DiagEmissiveOccluderOtherInstanceSameMaterial
            : kUpt04DiagEmissiveOccluderOtherInstanceOtherMaterial);
    Upt04DiagIncrement(counter);
}

public bool Upt04DiagIsCrosshair(uint2 pixel)
{
    return pixel.x == gUpt04Control.renderWidth / 2u
        && pixel.y == gUpt04Control.renderHeight / 2u;
}

// Preserve one receiver-local D0 decision next to the full-frame counters.
// The copied reservoir and this record are placed after the complete counter
// range so neither can overwrite the other as diagnostics grow.
public void Upt04DiagWriteD0Probe(
    uint2 pixel,
    bool receiverValid,
    Upt04PrimarySurfaceRecord receiver,
    Upt04InitialTraceWork selected,
    Upt04HitFacts hit,
    Upt04InitialTraceWork completed)
{
    if (!Upt04DiagIsCrosshair(pixel))
        return;

    Upt04UnifiedLightRecord light = (Upt04UnifiedLightRecord)0;
    const bool selectedValid = selected.valid != 0u
        && selected.lightIndex < gUpt04Control.currentLightCount;
    if (selectedValid)
        light = Upt04LoadCurrentLightRecord(selected.lightIndex);

    uint blockerMaterial = kUpt04InvalidIdentity;
    Upt04ResolvedTriangle blocker;
    if (hit.status == kUpt04HitTriangle
        && Upt04ResolveTriangle(hit, blocker)
        && blocker.valid != 0u)
        blockerMaterial = blocker.materialIndex;

    const Upt04LocalLightRanges ranges = Upt04InitialDirectLightRanges();
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 0u] = kUpt04DiagD0ProbeMagic;
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 1u] =
        (receiverValid ? 1u : 0u)
        | (selectedValid ? 2u : 0u)
        | (ranges.emissiveCount << 16u);
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 2u] =
        receiverValid ? receiver.materialAndSurface.x : kUpt04InvalidIdentity;
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 3u] =
        selectedValid ? selected.lightType : 0u;
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 4u] =
        selectedValid ? selected.lightIndex : kUpt04InvalidIdentity;
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 5u] =
        selectedValid ? light.sourceIndex : kUpt04InvalidIdentity;
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 6u] =
        selectedValid ? light.instanceId : kUpt04InvalidIdentity;
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 7u] =
        selectedValid ? light.primitiveIndex : kUpt04InvalidIdentity;
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 8u] =
        selectedValid ? light.materialOrLightId : kUpt04InvalidIdentity;
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 9u] = hit.status;
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 10u] = hit.instanceId;
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 11u] = hit.geometryId;
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 12u] = hit.primitiveId;
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 13u] = blockerMaterial;
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 14u] =
        (completed.candidateReady & 0xffu)
        | ((completed.candidateStatus & 0xffu) << 8u)
        | (ranges.analyticCount << 16u);
    gUpt04Diagnostics[kUpt04DiagD0ProbeBase + 15u] = completed.candidateFlags;
}

// Crosshair continuation proof. A triangle miss means traversal never saw the
// reflected geometry; a triangle hit plus decode failure localizes the loss to
// the static/dynamic/rigid/skinned route rather than TLAS residency.
public void Upt04DiagWriteC0Probe(
    uint2 pixel,
    Upt04PrimarySurfaceRecord receiver,
    Upt04InitialTraceWork continuation,
    Upt04HitFacts hit)
{
    if (!Upt04DiagIsCrosshair(pixel))
        return;

    const uint route = hit.status == kUpt04HitTriangle
        ? Upt04DiagContinuationRoute(hit)
        : kUpt04DiagRouteUnknown;
    const Upt04GeometryHitSurface decoded = Upt04DecodeGeometryHit(hit);
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 0u] = kUpt04DiagC0ProbeMagic;
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 1u] =
        (Upt04PrimarySurfaceRecordValid(receiver) ? 1u : 0u)
        | ((continuation.valid != 0u ? 1u : 0u) << 1u)
        | ((continuation.traceRequired != 0u ? 1u : 0u) << 2u)
        | ((route & 0xffu) << 8u)
        | ((decoded.valid != 0u ? 1u : 0u) << 16u);
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 2u] =
        receiver.materialAndSurface.x;
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 3u] =
        receiver.materialAndSurface.y;
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 4u] = hit.status;
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 5u] = hit.instanceId;
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 6u] = hit.geometryId;
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 7u] = hit.primitiveId;
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 8u] = hit.frontFace;
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 9u] = asuint(hit.hitT);
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 10u] = decoded.materialIndex;
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 11u] =
        decoded.triangleClassAndFlags;
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 12u] =
        asuint(continuation.ray.origin.x);
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 13u] =
        asuint(continuation.ray.origin.y);
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 14u] =
        asuint(continuation.ray.origin.z);
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 15u] =
        asuint(continuation.ray.direction.x);
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 16u] =
        asuint(continuation.ray.direction.y);
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 17u] =
        asuint(continuation.ray.direction.z);
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 18u] =
        asuint(continuation.ray.tMin);
    gUpt04Diagnostics[kUpt04DiagC0ProbeBase + 19u] =
        asuint(continuation.ray.tMax);
}

public void Upt04DiagCandidate(Upt04InitialTraceWork work, bool direct)
{
    const uint base = direct ? kUpt04DiagDirectInvalid : kUpt04DiagIndirectInvalid;
    uint statusOffset = 0u;
    if (work.valid != 0u && work.candidateReady != 0u)
    {
        statusOffset = work.candidateStatus == 2u ? 2u
            : (work.candidateStatus == 1u ? 1u : 0u);
    }
    Upt04DiagIncrement(base + statusOffset);
    Upt04DiagIncrement(kUpt04DiagCandidateInputs);
}

public uint Upt04DiagClassifyDirectRejection(
    uint2 pixel,
    Upt04PrimarySurfaceRecord surface)
{
    const Upt04LocalLightSelection selection =
        Upt04SelectInitialDirectLight(pixel);
    if (selection.valid == 0u
        || selection.denseIndex >= gUpt04Control.currentLightCount)
        return kUpt04DiagDirectRejectSelection;

    const Upt04UnifiedLightRecord light =
        Upt04LoadCurrentLightRecord(selection.denseIndex);
    if (!Upt04SelectionMatchesRecord(selection, light))
        return kUpt04DiagDirectRejectRecord;

    if (selection.requiresTriangleReplay != 0u)
    {
        const Upt04EmissiveGeometrySample replay =
            Upt04ReplayEmissiveTriangleGeometry(
                light,
                surface.worldPositionAndViewDepth.xyz,
                selection.sampleCoordinates,
                (gUpt04Control.availabilityFlags & kUpt04TwoSidedEmissives) != 0u);
        if (replay.status == kUpt04SampleInvalid)
            return kUpt04DiagDirectRejectEmissiveReplay;
    }
    else
    {
        if (!Upt04AnalyticLightCurrentPortalEligible(
                light, gUpt04Control.availabilityFlags))
            return kUpt04DiagDirectRejectAnalyticSample;
        const Upt04LocalLightSample analytic = Upt04SampleAnalyticSphere(
            light,
            surface.worldPositionAndViewDepth.xyz,
            selection.sampleCoordinates);
        if (analytic.status == kUpt04SampleInvalid)
            return kUpt04DiagDirectRejectAnalyticSample;
    }

    const Upt04EvaluatedLocalLight evaluated =
        Upt04EvaluateSelectedLocalLightDirectOnly(
            selection,
            light,
            surface.worldPositionAndViewDepth.xyz,
            gUpt04Control.availabilityFlags,
            gUpt04Control.emissiveScale);
    if (evaluated.valid == 0u)
        return kUpt04DiagDirectRejectOther;

    const Upt04MaterialEvaluation material =
        Upt05OpenPbrMaterialSet.EvaluatePrimaryDirect(
            surface,
            evaluated.sample.direction,
            evaluated.sample.radiance);
    if (material.valid == 0u)
        return kUpt04DiagDirectRejectMaterial;

    const float proposalPdf = selection.selectionPdf
        * evaluated.sample.solidAnglePdf;
    if (!Upt04FinitePositive(proposalPdf))
        return kUpt04DiagDirectRejectPdf;
    return kUpt04DiagDirectRejectOther;
}

// The aggregate direct-rejection classifier above only runs when every direct
// trial failed.  Proposal parity normally supplies 32 analytic trials, so an
// analytic success used to hide a failed emissive replay completely.  Audit
// every emissive trial independently in the diagnostic specialization.
public void Upt04DiagEmissiveTrials(
    uint2 pixel,
    Upt04PrimarySurfaceRecord surface)
{
    const Upt04LocalLightRanges ranges = Upt04InitialDirectLightRanges();
    const uint proposalParity =
        (gUpt04Control.availabilityFlags & kUpt04DirectProposalParity) != 0u
            ? 1u : 0u;
    const uint configuredEmissiveTrials = Upt04ConfiguredEmissiveTrialCount(
        gUpt04Control.availabilityFlags);
    const uint emissiveTrials = Upt04DirectRisDomainTrialCount(
        ranges, kUpt04LightEmissiveTriangle, proposalParity,
        configuredEmissiveTrials);
    [loop]
    for (uint candidateOrdinal = 0u;
         candidateOrdinal < emissiveTrials;
         ++candidateOrdinal)
    {
        Upt04DiagIncrement(kUpt04DiagEmissiveAttempts);
        const Upt04LocalLightSelection selection =
            Upt04SelectInitialDirectLight(pixel, candidateOrdinal);
        if (selection.valid == 0u
            || selection.type != kUpt04LightEmissiveTriangle
            || selection.denseIndex >= gUpt04Control.currentLightCount)
        {
            Upt04DiagIncrement(kUpt04DiagEmissiveSelectionReject);
            continue;
        }

        const Upt04UnifiedLightRecord light =
            Upt04LoadCurrentLightRecord(selection.denseIndex);
        if (!Upt04SelectionMatchesRecord(selection, light))
        {
            Upt04DiagIncrement(kUpt04DiagEmissiveIdentityReject);
            continue;
        }

        if (light.sourceIndex >= Upt04EmissiveReplayCount())
        {
            Upt04DiagIncrement(kUpt04DiagEmissiveIdentityReject);
            continue;
        }
        const Upt04EmissiveReplayRecord replay =
            gUpt04EmissiveReplay[light.sourceIndex];
        if (replay.instanceId != light.instanceId
            || replay.primitiveIndex != light.primitiveIndex
            || replay.materialIndex != light.materialOrLightId
            || replay.identityHashLo != light.identityA
            || replay.identityHashHi != light.identityB)
        {
            Upt04DiagIncrement(kUpt04DiagEmissiveIdentityReject);
            continue;
        }

        const float squareRootU = sqrt(selection.sampleCoordinates.x);
        const float barycentric1 =
            selection.sampleCoordinates.y * squareRootU;
        const float barycentric2 = squareRootU - barycentric1;
        Upt04HitFacts replayHit = Upt04MissFacts(0.0f);
        replayHit.status = kUpt04HitTriangle;
        replayHit.instanceId = replay.instanceId;
        replayHit.geometryId = 0u;
        replayHit.primitiveId = replay.primitiveIndex;
        replayHit.barycentrics = float2(barycentric1, barycentric2);
        replayHit.frontFace = 1u;
        Upt04ResolvedTriangle triangle;
        if (!Upt04ResolveTriangle(replayHit, triangle))
        {
            Upt04DiagIncrement(kUpt04DiagEmissiveResolveReject);
            continue;
        }
        if (triangle.materialIndex != replay.materialIndex)
        {
            Upt04DiagIncrement(kUpt04DiagEmissiveMaterialReject);
            continue;
        }

        const Upt04EmissiveGeometrySample sample =
            Upt04ReplayEmissiveTriangleBarycentrics(
                light,
                surface.worldPositionAndViewDepth.xyz,
                replayHit.barycentrics,
                (gUpt04Control.availabilityFlags
                    & kUpt04TwoSidedEmissives) != 0u);
        if (sample.status == kUpt04SampleInvalid)
        {
            Upt04DiagIncrement(kUpt04DiagEmissiveOtherReject);
            continue;
        }
        Upt04DiagIncrement(sample.status == kUpt04SampleValidPositive
            ? kUpt04DiagEmissiveGeometryPositive
            : kUpt04DiagEmissiveGeometryZero);

        const Upt04MaterialEmissionEvaluation emission =
            Upt05OpenPbrMaterialSet.EvaluateEmissiveEndpoint(
                sample.materialIndex,
                sample.triangleClassAndFlags,
                sample.emissionTexCoord,
                float2(0.0f),
                float2(0.0f));
        if (emission.valid != 0u && any(emission.radiance > 0.0f))
            Upt04DiagIncrement(kUpt04DiagEmissiveEndpointPositive);
        else
            Upt04DiagIncrement(kUpt04DiagEmissiveEndpointZero);

        const Upt04EvaluatedLocalLight evaluated =
            Upt04EvaluateSelectedLocalLight<Upt05OpenPbrMaterialSet>(
                selection,
                light,
                surface.worldPositionAndViewDepth.xyz,
                gUpt04Control.availabilityFlags,
                gUpt04Control.emissiveScale);
        if (evaluated.valid == 0u
            || evaluated.sample.status != kUpt04SampleValidPositive)
            continue;
        const Upt04MaterialEvaluation material =
            Upt05OpenPbrMaterialSet.EvaluatePrimaryDirect(
                surface,
                evaluated.sample.direction,
                evaluated.sample.radiance);
        if (material.valid == 0u || !Upt04FinitePositive(material.target))
            continue;
        Upt04DiagIncrement(kUpt04DiagEmissiveTargetPositive);

        const float physicalProposalPdf = selection.selectionPdf
            * evaluated.sample.solidAnglePdf;
        const uint candidateCount = Upt04DirectRisCandidateCount(
            ranges, proposalParity, configuredEmissiveTrials);
        const float techniqueMis = Upt04DirectTechniqueMis(
            selection.type,
            float(candidateCount) * physicalProposalPdf,
            material.directionalPdf);
        const float3 contribution = Upt04DirectReservoirContribution(
            material.weightedContribution,
            evaluated.sample.solidAnglePdf,
            techniqueMis,
            gUpt04Control.availabilityFlags);
        const float target = Upt04DirectReservoirTarget(
            contribution, gUpt04Control.availabilityFlags);
        const float reservoirSourcePdf = Upt04DirectReservoirSourcePdf(
            selection.selectionPdf,
            evaluated.sample.solidAnglePdf,
            techniqueMis,
            gUpt04Control.availabilityFlags);
        if (Upt04FinitePositive(target)
            && Upt04FinitePositive(reservoirSourcePdf))
            Upt04DiagIncrement(kUpt04DiagEmissiveProposalPositive);
    }
}

public void Upt04DiagRay(Upt04InitialTraceWork work, bool continuation)
{
    if (work.valid == 0u || work.traceRequired == 0u)
        return;
    Upt04DiagIncrement(continuation
        ? kUpt04DiagContinuationRays
        : kUpt04DiagVisibilityRays);
    const bool threeVertex = asfloat(gUpt04Control.reservedControl0) > 0.0f;
    if (!(threeVertex
            ? Upt04RayBudgetWithinThreeVertexCeiling(work.transport)
            : Upt04RayBudgetWithinD0Ceiling(work.transport)))
        Upt04DiagIncrement(kUpt04DiagRayCeilingViolations);
}

public void Upt04DiagSelection(Upt03LogicalReservoir reservoir)
{
    if (reservoir.hasSelectedSample == 0u)
    {
        Upt04DiagIncrement(kUpt04DiagCanonicalEmptyWrites);
        return;
    }
    const bool indirect =
        (reservoir.selected.flags & kUpt04CandidateFlagGlobal) != 0u;
    if (!indirect)
    {
        Upt04DiagIncrement(kUpt04DiagSelectedPrimaryNee);
        const bool emissive = reservoir.selected.eventKind == 2u;
        Upt04DiagIncrement(emissive
            ? kUpt04DiagFinalEmissive
            : kUpt04DiagFinalAnalytic);
        const float meanWeight = reservoir.effectiveM != 0u
            ? reservoir.weightSum : 0.0f;
        const float normalization = reservoir.selected.target > 0.0f
            ? meanWeight / reservoir.selected.target
            : 0.0f;
        const float3 estimate = float3(
            reservoir.selected.contributionR,
            reservoir.selected.contributionG,
            reservoir.selected.contributionB) * normalization;
        const float estimateLuminance = Upt04DirectLuminance(estimate);
        const uint binOffset = estimateLuminance < 1.0e-4f ? 0u
            : (estimateLuminance < 1.0e-2f ? 1u
                : (estimateLuminance < 1.0f ? 2u : 3u));
        Upt04DiagIncrement((emissive
            ? kUpt04DiagFinalEmissiveBelow1e4
            : kUpt04DiagFinalAnalyticBelow1e4) + binOffset);
    }
    else if (reservoir.selected.eventKind == 4u)
    {
        Upt04DiagIncrement(kUpt04DiagSelectedBsdfEndpoint);
        Upt04DiagIncrement(kUpt04DiagFinalIndirectSelected);
        if (reservoir.selected.pathLength == 2u)
        {
            Upt04DiagIncrement(kUpt04DiagX3EndpointSelected);
            Upt04DiagIncrement(kUpt04DiagX3AnySelected);
        }
    }
    else
    {
        Upt04DiagIncrement(kUpt04DiagSelectedSecondaryNee);
        Upt04DiagIncrement(kUpt04DiagFinalIndirectSelected);
        if (reservoir.selected.pathLength == 3u)
        {
            Upt04DiagIncrement(kUpt04DiagX3NeeSelected);
            Upt04DiagIncrement(kUpt04DiagX3AnySelected);
        }
    }
}

public uint Upt04DiagMixReservoirWord(uint hash, uint word, uint salt)
{
    return Upt04HashWord(hash ^ Upt04HashWord(word + salt));
}

public uint4 Upt04DiagReservoirHash(
    uint surfaceIndex,
    Upt03PackedReservoir packed)
{
    uint4 hash = uint4(
        Upt04HashWord(surfaceIndex ^ 0x243f6a88u),
        Upt04HashWord(surfaceIndex ^ 0x85a308d3u),
        Upt04HashWord(surfaceIndex ^ 0x13198a2eu),
        Upt04HashWord(surfaceIndex ^ 0x03707344u));
#define UPT04_DIAG_MIX_WORD(field, ordinal) \
    hash.x = Upt04DiagMixReservoirWord(hash.x, packed.field, 0x9e3779b9u * (ordinal)); \
    hash.y = Upt04DiagMixReservoirWord(hash.y, packed.field, 0x7f4a7c15u * (17u - (ordinal))); \
    hash.z = Upt04DiagMixReservoirWord(hash.z, packed.field, 0x94d049bbu ^ (ordinal)); \
    hash.w = Upt04DiagMixReservoirWord(hash.w, packed.field, 0x5bd1e995u + (ordinal));
    UPT04_DIAG_MIX_WORD(word0, 1u)
    UPT04_DIAG_MIX_WORD(word1, 2u)
    UPT04_DIAG_MIX_WORD(word2, 3u)
    UPT04_DIAG_MIX_WORD(word3, 4u)
    UPT04_DIAG_MIX_WORD(word4, 5u)
    UPT04_DIAG_MIX_WORD(word5, 6u)
    UPT04_DIAG_MIX_WORD(word6, 7u)
    UPT04_DIAG_MIX_WORD(word7, 8u)
    UPT04_DIAG_MIX_WORD(word8, 9u)
    UPT04_DIAG_MIX_WORD(word9, 10u)
    UPT04_DIAG_MIX_WORD(word10, 11u)
    UPT04_DIAG_MIX_WORD(word11, 12u)
    UPT04_DIAG_MIX_WORD(word12, 13u)
    UPT04_DIAG_MIX_WORD(word13, 14u)
    UPT04_DIAG_MIX_WORD(word14, 15u)
    UPT04_DIAG_MIX_WORD(word15, 16u)
#undef UPT04_DIAG_MIX_WORD
    return hash;
}

public Upt03PackedReservoir Upt04BuildDiagnosticReservoir(
    uint2 pixel,
    uint surfaceIndex)
{
    Upt03PackedReservoir packedEmpty = (Upt03PackedReservoir)0;
    Upt04InitialTraceWork receiverWork;
    Upt04PrimarySurfaceRecord receiverSurface;
    if (!Upt04LoadInitialSurface(pixel, receiverWork, receiverSurface))
    {
        Upt04DiagWriteD0Probe(
            pixel,
            false,
            receiverSurface,
            (Upt04InitialTraceWork)0,
            Upt04MissFacts(0.0f),
            (Upt04InitialTraceWork)0);
        Upt04DiagWriteC0Probe(
            pixel,
            receiverSurface,
            (Upt04InitialTraceWork)0,
            Upt04MissFacts(0.0f));
        Upt04DiagIncrement(kUpt04DiagReceiverInvalid);
        Upt04DiagIncrement(kUpt04DiagCanonicalEmptyWrites);
        return packedEmpty;
    }
    Upt04DiagIncrement(kUpt04DiagReceiverValid);

    Upt03LogicalReservoir reservoir = (Upt03LogicalReservoir)0;
    Upt04BoundedTransportState transport = Upt04BeginBoundedTransport();

    const bool directEnabled =
        (gUpt04Control.enabledFamilyMask & kUpt04FamilyLocalLight) != 0u;
    if (directEnabled)
        Upt04DiagEmissiveTrials(pixel, receiverSurface);
    Upt04InitialTraceWork direct =
        Upt04PrepareDirectPathTreeWork<Upt05OpenPbrMaterialSet>(pixel);
    if (direct.valid != 0u)
    {
        if (direct.lightType == kUpt04LightEmissiveTriangle)
            Upt04DiagIncrement(kUpt04DiagSelectedEmissive);
        else if (direct.lightType == kUpt04LightDoomAnalytic)
            Upt04DiagIncrement(kUpt04DiagSelectedAnalytic);
    }
    Upt04HitFacts hit = Upt04MissFacts(direct.ray.tMax);
    Upt04DiagRay(direct, false);
    if (direct.valid != 0u && direct.traceRequired != 0u)
        hit = Upt04TraceVisibilityRayQuery(
            direct.ray, Upt04DirectVisibilityIgnore(direct));
    Upt04DiagEmissiveOccluder(direct, hit);
    const Upt04InitialTraceWork selectedDirect = direct;
    direct = Upt04AdvanceInitialTraceWork(direct, hit);
    Upt04DiagD0AcquisitionStages(direct);
    Upt04DiagWriteD0Probe(
        pixel, true, receiverSurface, selectedDirect, hit, direct);
    if (direct.lightType == kUpt04LightEmissiveTriangle
        && direct.candidateReady != 0u)
    {
        Upt04DiagIncrement(
            (direct.candidateFlags & kUpt04CandidateFlagVisibilityPassed) != 0u
                ? kUpt04DiagSelectedEmissiveVisible
                : kUpt04DiagSelectedEmissiveOccluded);
    }
    if (directEnabled)
    {
        Upt04DiagCandidate(direct, true);
        if (direct.valid == 0u || direct.candidateReady == 0u)
            Upt04DiagIncrement(
                Upt04DiagClassifyDirectRejection(pixel, receiverSurface));
    }
    Upt04StreamInitialTraceWork(reservoir, direct, pixel, 0u);
    if (direct.valid != 0u)
        transport = direct.transport;
    if (!Upt04CompletePrimaryNeeStage(transport))
    {
        Upt04DiagSelection(reservoir);
        return Upt04FinalizeInitialReservoir(reservoir);
    }

    const bool indirectEnabled =
        (gUpt04Control.enabledFamilyMask & kUpt04FamilyIndirect) != 0u;
    Upt04InitialTraceWork continuation =
        Upt04PrepareIndirectPathTreeWork<Upt05OpenPbrMaterialSet>(pixel, transport);
    hit = Upt04MissFacts(continuation.ray.tMax);
    Upt04DiagRay(continuation, true);
    if (continuation.valid != 0u && continuation.traceRequired != 0u)
        hit = Upt04TraceRayQuery(continuation.ray);
    Upt04DiagWriteC0Probe(pixel, receiverSurface, continuation, hit);
    if (indirectEnabled)
        Upt04DiagSecondaryMaterialRoute(hit);
    const Upt04IndirectHitTrialResult indirect =
        Upt04PrepareIndirectHitTrials<Upt05OpenPbrMaterialSet>(
            continuation, hit, pixel, Upt04EmptyLocalLightSelection());
    if (indirectEnabled)
        Upt04DiagIndirectStages(indirect);
    if (indirectEnabled)
        Upt04DiagCandidate(indirect.endpointCandidate, false);
    Upt04StreamInitialTraceWork(
        reservoir, indirect.endpointCandidate, pixel, 1u);

    Upt04InitialTraceWork secondaryNee = indirect.neeWork;
    hit = Upt04MissFacts(secondaryNee.ray.tMax);
    Upt04DiagRay(secondaryNee, false);
    const Upt04VisibilityIgnore secondaryIgnore =
        Upt04IndirectNeeVisibilityIgnore(secondaryNee);
    if (secondaryNee.valid != 0u && secondaryNee.traceRequired != 0u)
        hit = Upt04TraceIndirectVisibilityRayQuery(
            secondaryNee.ray,
            secondaryIgnore);
    if (indirectEnabled && secondaryNee.valid != 0u
        && secondaryNee.traceRequired != 0u)
    {
        if (hit.status == kUpt04HitMiss)
            Upt04DiagIncrement(kUpt04DiagSecondaryVisibilityPassed);
        else
        {
            Upt04DiagIncrement(kUpt04DiagSecondaryVisibilityOccluded);
            if (Upt04VisibilityIgnoresCandidate(
                    secondaryIgnore, hit.instanceId, hit.primitiveId))
                Upt04DiagIncrement(kUpt04DiagSecondaryVisibilitySelfHit);
        }
    }
    secondaryNee = Upt04AdvanceInitialTraceWork(secondaryNee, hit);
    if (indirectEnabled)
    {
        Upt04DiagCandidate(secondaryNee, false);
        if (secondaryNee.valid != 0u
            && secondaryNee.candidateReady != 0u
            && secondaryNee.candidateStatus == kUpt04SampleValidPositive)
            Upt04DiagIncrement(kUpt04DiagSecondaryCandidatePositive);
    }
    Upt04StreamInitialTraceWork(reservoir, secondaryNee, pixel, 2u);

    // The diagnostic specialization contains the production x3 closure, but
    // the host publishes q=0 when three-vertex transport is disabled.  Count
    // candidate existence separately from final unified-reservoir selection;
    // this distinguishes path-production loss from energy-weighted competition
    // with primary and x2 terms.
    if (asfloat(gUpt04Control.reservedControl0) > 0.0f)
    {
        Upt04InitialTraceWork secondSource = continuation;
        if (indirect.neeWork.valid != 0u)
        {
            secondSource.transport.budget.visibilityRays = max(
                secondSource.transport.budget.visibilityRays,
                indirect.neeWork.transport.budget.visibilityRays);
        }
        const Upt04SecondContinuationResult secondContinuation =
            Upt04PrepareSecondContinuation<Upt05OpenPbrMaterialSet>(
                secondSource, indirect.secondarySurface);
        if (secondContinuation.work.valid != 0u)
            Upt04DiagIncrement(kUpt04DiagX3ContinuationPrepared);
        hit = Upt04MissFacts(secondContinuation.work.ray.tMax);
        Upt04DiagRay(secondContinuation.work, true);
        if (secondContinuation.work.valid != 0u
            && secondContinuation.work.traceRequired != 0u)
            hit = Upt04TraceRayQuery(secondContinuation.work.ray);
        if (hit.status == kUpt04HitTriangle)
            Upt04DiagIncrement(kUpt04DiagX3ContinuationHit);
        const Upt04InitialTraceWork secondEndpoint =
            Upt04EvaluateSecondEndpoint<Upt05OpenPbrMaterialSet>(
                secondContinuation, indirect.secondarySurface, hit);
        if (secondEndpoint.candidateReady != 0u
            && secondEndpoint.candidateStatus == kUpt04SampleValidPositive)
            Upt04DiagIncrement(kUpt04DiagX3EndpointPositive);
        Upt04StreamInitialTraceWork(reservoir, secondEndpoint, pixel, 3u);

        Upt04InitialTraceWork thirdNee = (Upt04InitialTraceWork)0;
        if (hit.status == kUpt04HitTriangle)
        {
            const Upt04GeometryHitSurface thirdSurface =
                Upt04DecodeGeometryHit(hit);
            thirdNee = Upt04PrepareThirdVertexNee<Upt05OpenPbrMaterialSet>(
                secondContinuation, thirdSurface, pixel);
        }
        if (thirdNee.valid != 0u && thirdNee.traceRequired != 0u)
            Upt04DiagIncrement(kUpt04DiagX3NeePreparedPositive);
        hit = Upt04MissFacts(thirdNee.ray.tMax);
        Upt04DiagRay(thirdNee, false);
        if (thirdNee.valid != 0u && thirdNee.traceRequired != 0u)
            hit = Upt04TraceIndirectVisibilityRayQuery(
                thirdNee.ray, Upt04IndirectNeeVisibilityIgnore(thirdNee));
        thirdNee = Upt04CompleteIndirectNeeVisibility(thirdNee, hit);
        if (thirdNee.candidateReady != 0u
            && thirdNee.candidateStatus == kUpt04SampleValidPositive)
            Upt04DiagIncrement(kUpt04DiagX3NeeVisibilityPassed);
        Upt04StreamInitialTraceWork(reservoir, thirdNee, pixel, 4u);
    }
    Upt04DiagSelection(reservoir);
    return Upt04FinalizeInitialReservoir(reservoir);
}

[numthreads(8, 8, 1)]
void main(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint groupIndex : SV_GroupIndex)
{
    if (groupIndex == 0u)
    {
        gUpt04GroupReservoirHash0 = 0u;
        gUpt04GroupReservoirHash1 = 0u;
        gUpt04GroupReservoirHash2 = 0u;
        gUpt04GroupReservoirHash3 = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint2 pixel = dispatchThreadId.xy;
    const uint surfaceIndex = pixel.y * gUpt04Control.renderWidth + pixel.x;
    if (pixel.x < gUpt04Control.renderWidth
        && pixel.y < gUpt04Control.renderHeight
        && surfaceIndex < gUpt04Control.surfaceCount)
    {
        const Upt03PackedReservoir packed =
            Upt04BuildDiagnosticReservoir(pixel, surfaceIndex);
        Upt04StorePackedReservoir(surfaceIndex, packed);
        const uint4 hash = Upt04DiagReservoirHash(surfaceIndex, packed);
        uint ignored;
        InterlockedXor(gUpt04GroupReservoirHash0, hash.x, ignored);
        InterlockedXor(gUpt04GroupReservoirHash1, hash.y, ignored);
        InterlockedXor(gUpt04GroupReservoirHash2, hash.z, ignored);
        InterlockedXor(gUpt04GroupReservoirHash3, hash.w, ignored);
    }
    GroupMemoryBarrierWithGroupSync();
    if (groupIndex == 0u)
    {
        uint ignored;
        InterlockedXor(
            gUpt04Diagnostics[kUpt04DiagReservoirHash0],
            gUpt04GroupReservoirHash0,
            ignored);
        InterlockedXor(
            gUpt04Diagnostics[kUpt04DiagReservoirHash1],
            gUpt04GroupReservoirHash1,
            ignored);
        InterlockedXor(
            gUpt04Diagnostics[kUpt04DiagReservoirHash2],
            gUpt04GroupReservoirHash2,
            ignored);
        InterlockedXor(
            gUpt04Diagnostics[kUpt04DiagReservoirHash3],
            gUpt04GroupReservoirHash3,
            ignored);
    }
}
