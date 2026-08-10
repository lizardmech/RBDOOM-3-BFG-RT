import upt04_initial_work_contract;
import upt04_initial_random;
import upt04_initial_bindings;
import upt04_initial_control;
import upt04_initial_previous_best;
import upt04_hit_contract;
import upt04_light_contract;
import upt04_geometry_contract;
import upt04_emissive_replay;
import upt04_local_light_proposal;
import upt04_local_light_evaluation;
import upt04_material_contract;
import upt04_material_provider_contract;
import upt04_primary_surface_live_contract;
import upt04_random_contract;
import upt04_random_white_hash;
import upt04_transport_initial_bounded;
import upt04_initial_light_selection;

#ifndef UPT04_LIGHT_TILES
#define UPT04_LIGHT_TILES 0
#endif

#ifndef UPT_FROZEN_STATIC_SCENE
#define UPT_FROZEN_STATIC_SCENE 0
#endif

#ifndef UPT_FROZEN_FLAT_LIGHTS
#define UPT_FROZEN_FLAT_LIGHTS UPT_FROZEN_STATIC_SCENE
#endif

public Upt04LocalLightRanges Upt04InitialDirectLightRanges()
{
    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt04Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt04Control.emissiveRangeCount;
    ranges.analyticStart = gUpt04Control.analyticRangeStart;
    ranges.analyticCount = gUpt04Control.analyticRangeCount;
    return ranges;
}

public Upt04LocalLightSelection Upt04SelectInitialDirectLight(uint2 pixel)
{
    return Upt04SelectInitialDirectLight(pixel, 0u);
}

public Upt04LocalLightSelection Upt04SelectInitialDirectLight(
    uint2 pixel,
    uint candidateOrdinal)
{
    return Upt04SelectInitialDirectLight(
        Upt04MakeInitialLightSchedule(Upt04InitialDirectLightRanges()),
        pixel,
        candidateOrdinal);
}

public Upt04LocalLightSelection Upt04SelectInitialDirectLight(
    Upt04InitialLightSchedule schedule,
    uint2 pixel,
    uint candidateOrdinal)
{
    Upt04LocalLightSelection result = (Upt04LocalLightSelection)0;
    if (candidateOrdinal >= schedule.candidateCount)
        return result;
    const bool selectEmissive = candidateOrdinal < schedule.emissiveTrials;
    const uint selectedType = selectEmissive
        ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic;
    Upt04LocalLightRanges typedRanges = (Upt04LocalLightRanges)0;
    if (selectEmissive)
    {
        typedRanges.emissiveStart = schedule.ranges.emissiveStart;
        typedRanges.emissiveCount = schedule.ranges.emissiveCount;
    }
    else
    {
        typedRanges.analyticStart = schedule.ranges.analyticStart;
        typedRanges.analyticCount = schedule.ranges.analyticCount;
    }
    float identityRandom = Upt04WhiteHash(Upt04InitialRandomKeyAtVertex(
        pixel,
        candidateOrdinal,
        kUpt04StreamInitialLocalLightIdentity,
        kUpt04DimensionInitialLocalLightIdentity));
    if (schedule.proposalParity != 0u && !selectEmissive)
    {
        // Match the accepted clean-DI analytic schedule: cover disjoint
        // strata before repeating lights. Random sampling over the complete
        // Doom light universe left most real-map pixels without a positive D0
        // candidate even though their reprojected surfaces were valid.
        const uint analyticOrdinal = candidateOrdinal
            - schedule.emissiveTrials;
        const float stride = max(
            1.0f,
            float(schedule.ranges.analyticCount)
                / float(max(schedule.analyticTrials, 1u)));
        const float lightIndexInRange = min(
            (float(analyticOrdinal) + identityRandom) * stride,
            float(max(schedule.ranges.analyticCount, 1u) - 1u));
        identityRandom = (lightIndexInRange + 0.5f)
            / float(max(schedule.ranges.analyticCount, 1u));
    }
    const float2 surfaceRandom = float2(
            Upt04WhiteHash(Upt04InitialRandomKeyAtVertex(
                pixel,
                candidateOrdinal,
                kUpt04StreamInitialLocalLightSurfaceU,
                kUpt04DimensionInitialLocalLightSurfaceU)),
            Upt04WhiteHash(Upt04InitialRandomKeyAtVertex(
                pixel,
                candidateOrdinal,
                kUpt04StreamInitialLocalLightSurfaceV,
                kUpt04DimensionInitialLocalLightSurfaceV)));
#if UPT04_LIGHT_TILES
    return Upt04SelectInitialLightFromTile(
        schedule,
        pixel,
        0u,
        candidateOrdinal,
        identityRandom,
        surfaceRandom);
#else
    if (schedule.proposalParity != 0u && selectEmissive)
    {
        result = Upt04SelectInitialWeightedEmissive(
            schedule.ranges,
            identityRandom,
            surfaceRandom,
            schedule.emissiveTrials,
            schedule.candidateCount);
        if (result.valid != 0u)
            return result;
    }

    result = Upt04SelectUniformLocalLight(
        typedRanges, identityRandom, surfaceRandom);
    result.selectionPdf = Upt04InitialLightScheduleIdentityPdf(
        schedule, selectedType);
    return result;
#endif
}

public struct Upt04DirectRisProposal
{
    public uint valid;
    public uint lobeMask;
    public uint candidateFlags;
    public uint replayKey;
    public uint replayIndex;
    public uint lightIndex;
    public uint lightType;
    public float sourceProposalPdf;
    public float2 sampleCoordinates;
    public float3 contribution;
    public float target;
    public float3 sampleDirection;
    public float sampleDistance;
};

public float Upt04DirectTechniqueMis(
    uint lightType,
    float physicalProposalPdf,
    float alternateDirectionalPdf)
{
    // A balance weight is valid only when both techniques are actually in the
    // active estimator.  The direct-only family never traces a BSDF endpoint,
    // so weighting emissive NEE against its hypothetical PDF suppresses real
    // emissive energy with no compensating sample.  Keep MIS for the unified
    // family, where the indirect/BSDF endpoint proposal is present.
    const bool bsdfEndpointCompetes =
        (Upt04EnabledFamilyMask(gUpt04Control.enabledFamilyMask)
            & kUpt04FamilyIndirect) != 0u;
    return lightType == kUpt04LightEmissiveTriangle && bsdfEndpointCompetes
        ? Upt04BalanceHeuristic(
            physicalProposalPdf, alternateDirectionalPdf)
        : 1.0f;
}

public Upt04DirectRisProposal Upt04EvaluateDirectRisProposal<
    TMaterialSet : IUpt04MaterialSet>(
    Upt04PrimarySurfaceRecord surface,
    Upt04PreparedDirectMaterial preparedMaterial,
    Upt04InitialLightSchedule schedule,
    uint2 pixel,
    uint candidateOrdinal)
{
    Upt04DirectRisProposal proposal = (Upt04DirectRisProposal)0;
    const Upt04LocalLightSelection selection =
        Upt04SelectInitialDirectLight(schedule, pixel, candidateOrdinal);
    if (selection.valid == 0u
        || selection.denseIndex >= gUpt04Control.currentLightCount)
        return proposal;

    const Upt04UnifiedLightRecord light =
        Upt04LoadCurrentLightEvaluationRecord(
            selection.denseIndex, selection.type);
    if (selection.type == kUpt04LightEmissiveTriangle
        && selection.reserved != 0u
        && light.sourceIndex != selection.reserved - 1u)
        return proposal;
    const Upt04EvaluatedLocalLight evaluated =
        Upt04EvaluateSelectedLocalLight<TMaterialSet>(
            selection,
            light,
            surface.worldPositionAndViewDepth.xyz,
            gUpt04Control.availabilityFlags,
            gUpt04Control.emissiveScale);
    if (evaluated.valid == 0u)
        return proposal;

    const Upt04MaterialEvaluation material = TMaterialSet.EvaluatePreparedDirect(
        preparedMaterial,
        evaluated.sample.direction,
        evaluated.sample.radiance);
    if (material.valid == 0u)
        return proposal;

    const float physicalProposalPdf = selection.selectionPdf
        * evaluated.sample.solidAnglePdf;
    if (!Upt04FinitePositive(physicalProposalPdf)
        || evaluated.sample.status != kUpt04SampleValidPositive
        || !Upt04FinitePositive(material.target))
        return proposal;

    const float multiSampleNeePdf = float(schedule.candidateCount)
        * physicalProposalPdf;
    const float techniqueMis = Upt04DirectTechniqueMis(
        selection.type, multiSampleNeePdf, material.directionalPdf);
    proposal.contribution = Upt04DirectReservoirContribution(
        material.weightedContribution,
        evaluated.sample.solidAnglePdf,
        techniqueMis,
        gUpt04Control.availabilityFlags);
    proposal.target = Upt04DirectReservoirTarget(
        proposal.contribution,
        gUpt04Control.availabilityFlags);
    const float reservoirSourcePdf = Upt04DirectReservoirSourcePdf(
        selection.selectionPdf,
        evaluated.sample.solidAnglePdf,
        techniqueMis,
        gUpt04Control.availabilityFlags);
    if (!Upt04FinitePositive(proposal.target)
        || !Upt04FinitePositive(reservoirSourcePdf))
        return (Upt04DirectRisProposal)0;

    if (!Upt04PrimaryVisibilityEndpointValid(
            surface.worldPositionAndViewDepth.xyz,
            surface.geometricNormalAndRoughness.xyz,
            surface.shadingNormalAndOpacity.xyz,
            evaluated.sample.direction,
            evaluated.sample.distance))
        return (Upt04DirectRisProposal)0;

    proposal.lobeMask = material.lobeMask;
    proposal.candidateFlags = Upt04DirectProposalParityValue(
            gUpt04Control.availabilityFlags) != 0u
        ? kUpt04CandidateFlagDirectProposalParity : 0u;
    proposal.lightIndex = selection.denseIndex;
    proposal.lightType = selection.type;
    proposal.replayKey = Upt04InitialReplayKey(pixel);
    proposal.sourceProposalPdf = reservoirSourcePdf;
    proposal.sampleCoordinates = evaluated.storedSampleCoordinates;
    proposal.sampleDirection = evaluated.sample.direction;
    proposal.sampleDistance = evaluated.sample.distance;
    proposal.valid = 1u;
    return proposal;
}

public Upt04DirectRisProposal Upt04EvaluatePreviousBestProposal<
    TMaterialSet : IUpt04MaterialSet>(
    Upt04D0PreviousBestSource source,
    Upt04PrimarySurfaceRecord receiver,
    Upt04PreparedDirectMaterial preparedMaterial,
    inout uint diagnostics)
{
    Upt04DirectRisProposal proposal = (Upt04DirectRisProposal)0;
    if (source.valid == 0u || source.candidate.identity0 == 0u)
        return proposal;

    const uint previousDenseIndex = source.candidate.identity0 - 1u;
    const uint remapCount = Upt04D0PreviousToCurrentLightCount(
        gUpt04Control.previousToCurrentLightCountAndHistory);
    if (previousDenseIndex >= remapCount)
    {
        diagnostics |= kUpt04D0DiagPreviousRemapFailure;
        return proposal;
    }
    const uint currentDenseIndex =
        gUpt04PreviousToCurrentLights[previousDenseIndex];
    const Upt04LocalLightRanges ranges = Upt04InitialDirectLightRanges();
    const bool emissive = source.candidate.eventKind == 2u;
    const bool inExpectedRange = emissive
        ? currentDenseIndex >= ranges.emissiveStart
            && currentDenseIndex - ranges.emissiveStart < ranges.emissiveCount
        : currentDenseIndex >= ranges.analyticStart
            && currentDenseIndex - ranges.analyticStart < ranges.analyticCount;
    if (currentDenseIndex == kUpt04InvalidLightIndex
        || currentDenseIndex >= gUpt04Control.currentLightCount
        || !inExpectedRange)
    {
        diagnostics |= kUpt04D0DiagPreviousRemapFailure;
        return proposal;
    }

    const Upt04UnifiedLightRecord light =
        Upt04LoadCurrentLightRecord(currentDenseIndex);
    const uint expectedType = emissive
        ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic;
    if (light.type != expectedType
        || source.candidate.identity1 != Upt04StableLightIdentityFingerprint(light))
    {
        diagnostics |= kUpt04D0DiagPreviousRemapFailure;
        return proposal;
    }
    if (emissive && (light.flags & kUpt04EmissiveHistoryDynamic) != 0u)
    {
        diagnostics |= kUpt04D0DiagPreviousReplayFailure;
        return proposal;
    }

    Upt04LocalLightSample sample = (Upt04LocalLightSample)0;
    if (emissive)
    {
        const Upt04EmissiveGeometrySample replay =
            Upt04ReplayEmissiveTriangleBarycentrics(
                light,
                receiver.worldPositionAndViewDepth.xyz,
                float2(
                    source.candidate.sampleCoord0,
                    source.candidate.sampleCoord1),
                (gUpt04Control.availabilityFlags
                    & kUpt04EmissiveReplayTwoSided) != 0u);
        if (replay.status == kUpt04SampleInvalid)
        {
            diagnostics |= kUpt04D0DiagPreviousReplayFailure;
            return proposal;
        }
        const float4 regionWeights =
            Upt04EmissiveRegionWeights<TMaterialSet>(replay);
        const uint region = Upt04EmissiveRegionFromBarycentrics(
            replay.barycentrics);
        const Upt04MaterialEmissionEvaluation emission =
            TMaterialSet.EvaluateEmissiveEndpoint(
                replay.materialIndex,
                replay.triangleClassAndFlags,
                replay.emissionTexCoord,
                float2(0.0f),
                float2(0.0f));
        if (emission.valid == 0u)
        {
            diagnostics |= kUpt04D0DiagPreviousReplayFailure;
            return proposal;
        }
        sample.status = replay.status == kUpt04SampleValidPositive
                && any(emission.radiance > 0.0f)
            ? kUpt04SampleValidPositive : kUpt04SampleValidZero;
        sample.position = replay.position;
        sample.normal = replay.normal;
        sample.distance = replay.distance;
        sample.direction = replay.direction;
        sample.solidAnglePdf = replay.solidAnglePdf
            * (4.0f * Upt04EmissiveRegionSelectionPdf(
                regionWeights, region));
        sample.radiance = emission.radiance;
    }
    else
    {
        if (!Upt04AnalyticLightCurrentPortalEligible(
                light, gUpt04Control.availabilityFlags))
        {
            diagnostics |= kUpt04D0DiagPreviousReplayFailure;
            return proposal;
        }
        sample = Upt04SampleAnalyticSphere(
            light,
            receiver.worldPositionAndViewDepth.xyz,
            float2(
                source.candidate.sampleCoord0,
                source.candidate.sampleCoord1));
    }
    if (sample.status != kUpt04SampleValidPositive
        || !Upt04FinitePositive(sample.solidAnglePdf))
    {
        diagnostics |= sample.status == kUpt04SampleInvalid
            ? kUpt04D0DiagPreviousReplayFailure
            : kUpt04D0DiagPreviousTargetFailure;
        return proposal;
    }

    const Upt04MaterialEvaluation material = TMaterialSet.EvaluatePreparedDirect(
        preparedMaterial, sample.direction, sample.radiance);
    if (material.valid == 0u || !Upt04FinitePositive(material.target))
    {
        diagnostics |= kUpt04D0DiagPreviousTargetFailure;
        return proposal;
    }
    const float3 geometricNormal = normalize(
        receiver.geometricNormalAndRoughness.xyz);
    const float3 shadingNormal = normalize(
        receiver.shadingNormalAndOpacity.xyz);
    if (!all(isfinite(geometricNormal)) || !all(isfinite(shadingNormal))
        || dot(geometricNormal, sample.direction) <= 0.0f
        || dot(shadingNormal, sample.direction) <= 0.0f)
    {
        diagnostics |= kUpt04D0DiagPreviousTargetFailure;
        return proposal;
    }

    const uint sourceProposalParity =
        (source.candidate.flags & kUpt04CandidateFlagDirectProposalParity) != 0u
            ? 1u : 0u;
    const float identityPdf = Upt04DirectRisIdentityPdf(
        ranges,
        expectedType,
        sourceProposalParity,
        Upt04ConfiguredEmissiveTrialCount(
            gUpt04Control.availabilityFlags));
    const float physicalProposalPdf = identityPdf * sample.solidAnglePdf;
    const uint sourceCandidateCount = Upt04DirectRisCandidateCount(
        ranges,
        sourceProposalParity,
        Upt04ConfiguredEmissiveTrialCount(
            gUpt04Control.availabilityFlags));
    const float techniqueMis = Upt04DirectTechniqueMis(
        expectedType,
        float(sourceCandidateCount) * physicalProposalPdf,
        material.directionalPdf);
    proposal.contribution = Upt04DirectReservoirContribution(
        material.weightedContribution,
        sample.solidAnglePdf,
        techniqueMis,
        gUpt04Control.availabilityFlags);
    proposal.target = Upt04DirectReservoirTarget(
        proposal.contribution,
        gUpt04Control.availabilityFlags);
    proposal.sourceProposalPdf = Upt04DirectReservoirSourcePdf(
        identityPdf,
        sample.solidAnglePdf,
        techniqueMis,
        gUpt04Control.availabilityFlags);
    if (!Upt04FinitePositive(proposal.target)
        || !Upt04FinitePositive(proposal.sourceProposalPdf)
        || !Upt04PrimaryVisibilityEndpointValid(
            receiver.worldPositionAndViewDepth.xyz,
            receiver.geometricNormalAndRoughness.xyz,
            receiver.shadingNormalAndOpacity.xyz,
            sample.direction,
            sample.distance))
    {
        diagnostics |= kUpt04D0DiagPreviousTargetFailure;
        return (Upt04DirectRisProposal)0;
    }

    proposal.valid = 1u;
    proposal.lobeMask = material.lobeMask;
    proposal.candidateFlags = source.candidate.flags
        & kUpt04CandidateFlagDirectProposalParity;
    proposal.replayKey = source.candidate.replayKey;
    proposal.replayIndex = source.candidate.replayIndex;
    proposal.lightIndex = currentDenseIndex;
    proposal.lightType = expectedType;
    proposal.sampleCoordinates = float2(
        source.candidate.sampleCoord0,
        source.candidate.sampleCoord1);
    proposal.sampleDirection = sample.direction;
    proposal.sampleDistance = sample.distance;
    return proposal;
}

public Upt04InitialTraceWork Upt04PrepareDirectTrial<
    TMaterialSet : IUpt04MaterialSet>(
    Upt04InitialTraceWork work,
    Upt04PrimarySurfaceRecord surface,
    uint2 pixel)
{
    const Upt04PreparedDirectMaterial preparedMaterial =
        TMaterialSet.PreparePrimaryDirect(surface);
#if UPT_FROZEN_FLAT_LIGHTS
    const Upt04D0PreviousBestSource previousSource =
        (Upt04D0PreviousBestSource)0;
    uint directDiagnostics = 0u;
    const Upt04DirectRisProposal previousBest =
        (Upt04DirectRisProposal)0;
#else
    const Upt04D0PreviousBestSource previousSource =
        Upt04D0LoadPreviousBestSource(surface);
    uint directDiagnostics = previousSource.diagnostics;
    const Upt04DirectRisProposal previousBest =
        Upt04EvaluatePreviousBestProposal<TMaterialSet>(
            previousSource, surface, preparedMaterial, directDiagnostics);
#endif

    Upt04DirectRisProposal freshSelected = (Upt04DirectRisProposal)0;
    float weightSum = 0.0f;
    const Upt04InitialLightSchedule schedule =
        Upt04MakeInitialLightSchedule(Upt04InitialDirectLightRanges());
    const uint candidateCount = schedule.candidateCount;
    if (schedule.ranges.emissiveCount != 0u
        && !Upt04EmissiveDistributionValid(
            gUpt04Control.emissiveDistributionCountAndValid))
        directDiagnostics |= kUpt04D0DiagWeightedCdfUnavailable;
    [loop]
    for (uint candidateOrdinal = 0u;
         candidateOrdinal < candidateCount;
         ++candidateOrdinal)
    {
        const Upt04DirectRisProposal proposal =
            Upt04EvaluateDirectRisProposal<TMaterialSet>(
                surface,
                preparedMaterial,
                schedule,
                pixel,
                candidateOrdinal);
        if (proposal.valid == 0u)
            continue;
        if (previousBest.valid != 0u
            && proposal.lightIndex == previousBest.lightIndex)
        {
            // The fixed fresh schedule still contains this defined trial, but
            // it contributes zero rather than admitting the same stable light
            // twice. The candidateCount denominator therefore remains exact.
            directDiagnostics |=
                kUpt04D0DiagPreviousDuplicateRejected;
            continue;
        }
        const float candidateWeight = proposal.target
            / proposal.sourceProposalPdf;
        const float newWeightSum = weightSum + candidateWeight;
        if (!Upt04FinitePositive(candidateWeight)
            || !Upt04FinitePositive(newWeightSum))
            continue;
        const float selectionRandom = Upt04WhiteHash(
            Upt04InitialRandomKeyAtVertex(
                pixel,
                candidateOrdinal,
                kUpt04StreamInitialReservoirSelection,
                kUpt04DimensionInitialReservoirSelection));
        if (freshSelected.valid == 0u
            || selectionRandom * newWeightSum < candidateWeight)
            freshSelected = proposal;
        weightSum = newWeightSum;
    }
    if (freshSelected.valid != 0u)
        directDiagnostics |= freshSelected.lightType
                == kUpt04LightEmissiveTriangle
            ? kUpt04D0DiagFreshEmissiveSelected
            : kUpt04D0DiagFreshAnalyticSelected;

    // The RIS schedule drew candidateCount proposals, including candidates
    // whose target evaluated to zero at this receiver. Those are defined zero
    // trials and must remain in M. Dividing by positiveCount conditions the
    // estimator on finding illumination: sparse real-map pixels become too
    // bright, and changing the proposal schedule changes energy. Malformed
    // candidates are still never admitted as selectable reservoir samples;
    // they contribute exactly zero to this fixed trial schedule.
    const float freshMass = freshSelected.valid != 0u && candidateCount != 0u
        ? weightSum / float(candidateCount) : 0.0f;
    const float previousBestMass = previousBest.valid != 0u
        ? previousBest.target : 0.0f;
    if (previousBest.valid != 0u)
        directDiagnostics |= kUpt04D0DiagPreviousAdmitted;
    const uint sourceCount = (freshSelected.valid != 0u ? 1u : 0u)
        + (previousBest.valid != 0u ? 1u : 0u);
    const float combinedMass = freshMass + previousBestMass;
    if (sourceCount == 0u || !Upt04FinitePositive(combinedMass))
    {
        work.directDiagnostics = directDiagnostics;
        return work;
    }

    Upt04DirectRisProposal selected = freshSelected;
    if (previousBest.valid != 0u)
    {
        const float selectionRandom = Upt04WhiteHash(
            Upt04InitialRandomKeyAtVertex(
                pixel,
                candidateCount,
                kUpt04StreamInitialReservoirSelection,
                kUpt04DimensionInitialReservoirSelection));
        if (freshSelected.valid == 0u
            || selectionRandom * combinedMass < previousBestMass)
        {
            selected = previousBest;
            directDiagnostics |= kUpt04D0DiagPreviousSelected;
        }
    }
    const float finalizedMass = combinedMass / float(sourceCount);
    const float effectiveProposalPdf = selected.target / finalizedMass;
    if (!Upt04FinitePositive(effectiveProposalPdf))
    {
        work.directDiagnostics = directDiagnostics;
        return work;
    }

    work.familyKind = kUpt04InitialFamilyDirect;
    work.eventKind = selected.lightType == kUpt04LightEmissiveTriangle
        ? 2u : 1u;
    work.lobeMask = selected.lobeMask;
    work.candidateFlags = selected.candidateFlags;
    work.identity0 = selected.lightIndex + 1u;
    if (work.identity0 == 0u
        || selected.lightIndex >= gUpt04Control.currentLightCount)
        return Upt04InvalidateInitialWork(work);
    const Upt04UnifiedLightRecord selectedLight =
        Upt04LoadCurrentLightIdentityRecord(selected.lightIndex);
    work.identity1 = Upt04StableLightIdentityFingerprint(selectedLight);
    if (work.identity1 == 0u)
        return Upt04InvalidateInitialWork(work);
    work.replayKey = selected.replayKey;
    work.replayIndex = selected.replayIndex;
    work.lightIndex = selected.lightIndex;
    work.lightType = selected.lightType;
    work.proposalPdf = effectiveProposalPdf;
    work.pathPdf = selected.sourceProposalPdf;
    work.partialJacobian = 1.0f;
    work.accumulatedRouletteProbability = 1.0f;
    work.sampleCoordinates = selected.sampleCoordinates;
    work.pathLength = 1u;
    work.reconnectionVertexLength = 0u;
    work.contribution = selected.contribution;
    work.target = selected.target;
    work.directDiagnostics = directDiagnostics;
    work.candidateStatus = kUpt04SampleValidPositive;
    work.transport = Upt04BeginBoundedTransport();
    if (!Upt04AdmitDirectVisibility(work.transport)
        || !Upt04RayBudgetWithinD0Ceiling(work.transport))
        return Upt04InvalidateInitialWork(work);
    if (!Upt04BuildPrimaryVisibilityRequest(
            surface.worldPositionAndViewDepth.xyz,
            surface.geometricNormalAndRoughness.xyz,
            surface.shadingNormalAndOpacity.xyz,
            selected.sampleDirection,
            selected.sampleDistance,
            work.ray))
        return Upt04InvalidateInitialWork(work);
    work.traceRequired = 1u;
    work.valid = 1u;
    return work;
}

public Upt04VisibilityIgnore Upt04DirectVisibilityIgnore(
    Upt04InitialTraceWork work)
{
    if (work.valid == 0u || work.familyKind != kUpt04InitialFamilyDirect
        || work.lightType != kUpt04LightEmissiveTriangle
        || work.lightIndex >= gUpt04Control.currentLightCount)
        return Upt04NoVisibilityIgnore();
    const Upt04UnifiedLightRecord light =
        Upt04LoadCurrentLightVisibilityRecord(work.lightIndex);
    return light.type == kUpt04LightEmissiveTriangle
        ? Upt04MakeVisibilityIgnore(light.instanceId, light.primitiveIndex)
        : Upt04NoVisibilityIgnore();
}

public Upt04InitialTraceWork Upt04CompleteDirectVisibility(
    Upt04InitialTraceWork work,
    Upt04HitFacts hit)
{
    if (work.valid == 0u || work.familyKind != kUpt04InitialFamilyDirect
        || work.transport.stage != kUpt04TransportDirectVisibility
        || work.traceRequired == 0u)
        return Upt04InvalidateInitialWork(work);
    work.traceRequired = 0u;
    work.candidateReady = 1u;
    work.candidateFlags =
        (work.candidateFlags & kUpt04CandidateFlagDirectProposalParity)
        | kUpt04CandidateFlagVisibilityKnown;
    if (hit.status == kUpt04HitMiss)
        work.candidateFlags |= kUpt04CandidateFlagVisibilityPassed;
    else
    {
        work.candidateStatus = kUpt04SampleValidZero;
        if ((work.directDiagnostics
                & kUpt04D0DiagPreviousSelected) != 0u)
            work.directDiagnostics |=
                kUpt04D0DiagPreviousVisibilityFailure;
    }
    return work;
}
