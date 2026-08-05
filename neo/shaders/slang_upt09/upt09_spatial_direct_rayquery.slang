import upt03_reservoir_contract;
import upt04_hit_contract;
import upt04_initial_work_contract;
import upt04_geometry_contract;
import upt04_light_contract;
import upt04_local_light_proposal;
import upt04_emissive_replay;
import upt04_material_contract;
import upt04_primary_surface_live_contract;
import upt04_random_white_hash;
import upt07_material_openpbr;
import upt07_material_provider_contract;
import upt07_temporal_reservoir;
import upt09_spatial_direct_bindings;

public static const uint kUpt09MaximumNeighborAttempts = 12u;

public static const float2 kUpt09NeighborOffsets[32] =
{
    float2( 0.096f,  0.071f), float2(-0.154f,  0.141f),
    float2( 0.032f, -0.287f), float2( 0.247f,  0.246f),
    float2(-0.365f, -0.065f), float2( 0.362f, -0.248f),
    float2(-0.135f,  0.492f), float2(-0.282f, -0.451f),
    float2( 0.553f,  0.087f), float2(-0.502f,  0.321f),
    float2( 0.160f, -0.615f), float2( 0.364f,  0.563f),
    float2(-0.681f, -0.157f), float2( 0.648f, -0.331f),
    float2(-0.245f,  0.718f), float2(-0.390f, -0.686f),
    float2( 0.793f,  0.201f), float2(-0.762f,  0.356f),
    float2( 0.313f, -0.803f), float2( 0.407f,  0.779f),
    float2(-0.869f, -0.272f), float2( 0.857f, -0.347f),
    float2(-0.392f,  0.849f), float2(-0.393f, -0.864f),
    float2( 0.908f,  0.358f), float2(-0.929f,  0.306f),
    float2( 0.488f, -0.858f), float2( 0.341f,  0.930f),
    float2(-0.909f, -0.442f), float2( 0.981f, -0.226f),
    float2(-0.589f,  0.829f), float2(-0.242f, -0.971f)
};

public float Upt09Random(uint2 pixel, uint ordinal, uint stream)
{
    uint value = Upt04HashWord(pixel.x ^ (pixel.y * 0x9e3779b9u));
    value = Upt04HashWord(value ^ gUpt09Control.frameSampleIndex);
    value = Upt04HashWord(value ^ (ordinal * 0x85ebca6bu));
    value = Upt04HashWord(value ^ stream);
    return float(value >> 8u) * (1.0f / 16777216.0f);
}

public bool Upt09VisibilityPassed(
    Upt04TraceRequest request,
    Upt04VisibilityIgnore ignore)
{
    RayDesc ray;
    ray.Origin = request.origin;
    ray.TMin = request.tMin;
    ray.Direction = request.direction;
    ray.TMax = request.tMax;
    RayQuery<0> query = RayQuery<0>();
    query.TraceRayInline(
        gUpt09Scene,
        RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xffu,
        ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        if (Upt04VisibilityIgnoresCandidate(
                ignore,
                query.CandidateInstanceID(),
                query.CandidatePrimitiveIndex()))
            continue;
        query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

public bool Upt09DirectReservoirEligible(Upt07DecodedReservoir decoded)
{
    if (decoded.valid == 0u || decoded.logical.hasSelectedSample == 0u)
        return false;
    const Upt03Candidate candidate = decoded.logical.selected;
    return (candidate.eventKind == 1u || candidate.eventKind == 2u)
        && (candidate.flags & kUpt04CandidateFlagGlobal) == 0u
        && !Upt03IsDirectSpatialRescue(candidate)
        && candidate.pathLength == 1u
        && candidate.reconnectionVertexLength == 0u;
}

public uint Upt09EvaluateDirectSample(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord receiver,
    out Upt04LocalLightSample sample,
    out uint emissive,
    out uint denseIndex)
{
    sample = (Upt04LocalLightSample)0;
    emissive = source.eventKind == 2u ? 1u : 0u;
    denseIndex = 0xffffffffu;
    if ((source.eventKind != 1u && source.eventKind != 2u)
        || source.identity0 == 0u)
        return 0u;
    denseIndex = source.identity0 - 1u;
    const bool inEmissiveRange = denseIndex >= gUpt09Control.emissiveRangeStart
        && denseIndex - gUpt09Control.emissiveRangeStart
            < gUpt09Control.emissiveRangeCount;
    const bool inAnalyticRange = denseIndex >= gUpt09Control.analyticRangeStart
        && denseIndex - gUpt09Control.analyticRangeStart
            < gUpt09Control.analyticRangeCount;
    if (denseIndex >= gUpt09Control.currentLightCount
        || (emissive != 0u ? !inEmissiveRange : !inAnalyticRange))
        return 0u;
    const Upt04UnifiedLightRecord light = Upt09LoadLight(denseIndex);
    const uint expectedType = emissive != 0u
        ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic;
    if (light.type != expectedType
        || source.identity1 != Upt04StableLightIdentityFingerprint(light))
        return 0u;

    if (emissive != 0u)
    {
        const Upt04EmissiveGeometrySample replay =
            Upt04ReplayEmissiveTriangleBarycentrics(
                light,
                receiver.worldPositionAndViewDepth.xyz,
                float2(source.sampleCoord0, source.sampleCoord1));
        if (replay.status == kUpt04SampleInvalid)
            return 0u;
        sample.status = replay.status == kUpt04SampleValidPositive
                && any(light.radianceAndLuminance.xyz > 0.0f)
            ? kUpt04SampleValidPositive : kUpt04SampleValidZero;
        sample.position = replay.position;
        sample.normal = replay.normal;
        sample.distance = replay.distance;
        sample.direction = replay.direction;
        sample.solidAnglePdf = replay.solidAnglePdf;
        sample.radiance = max(light.radianceAndLuminance.xyz, float3(0.0f));
    }
    else
    {
        if (!Upt04AnalyticLightCurrentPortalEligible(
                light, gUpt09Control.geometryAvailabilityFlags))
            return 0u;
        sample = Upt04SampleAnalyticSphere(
            light,
            receiver.worldPositionAndViewDepth.xyz,
            float2(source.sampleCoord0, source.sampleCoord1));
        if (sample.status == kUpt04SampleInvalid)
            return 0u;
    }
    return 1u;
}

public uint Upt09ShiftPrimaryDirectUnshadowed<
    TMaterialSet : IUpt07PrimaryMaterialSet>(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord receiver,
    out Upt03Candidate shifted,
    out Upt04TraceRequest visibility,
    out Upt04VisibilityIgnore visibilityIgnore)
{
    shifted = (Upt03Candidate)0;
    visibility = (Upt04TraceRequest)0;
    visibilityIgnore = Upt04NoVisibilityIgnore();
    Upt04LocalLightSample sample;
    uint emissive;
    uint denseIndex;
    if (Upt09EvaluateDirectSample(
            source, receiver, sample, emissive, denseIndex) == 0u)
        return 0u;
    const Upt04MaterialEvaluation material =
        TMaterialSet.EvaluatePrimaryDirect(
            receiver, sample.direction, sample.radiance);
    if (material.valid == 0u)
        return 0u;
    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt09Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt09Control.emissiveRangeCount;
    ranges.analyticStart = gUpt09Control.analyticRangeStart;
    ranges.analyticCount = gUpt09Control.analyticRangeCount;
    const float proposalPdf = Upt04DirectRisIdentityPdf(
        ranges,
        emissive != 0u
            ? kUpt04LightEmissiveTriangle : kUpt04LightDoomAnalytic,
        (source.flags & kUpt04CandidateFlagDirectProposalParity) != 0u
            ? 1u : 0u)
        * sample.solidAnglePdf;
    if (!Upt04FinitePositive(proposalPdf))
        return 0u;

    shifted.status = sample.status == kUpt04SampleValidPositive
            && material.target > 0.0f
        ? kUpt04SampleValidPositive : kUpt04SampleValidZero;
    const float techniqueMis = emissive != 0u
        ? Upt04BalanceHeuristic(proposalPdf, material.directionalPdf) : 1.0f;
    const float3 contribution = Upt04DirectReservoirContribution(
        material.weightedContribution,
        sample.solidAnglePdf,
        techniqueMis,
        gUpt09Control.geometryAvailabilityFlags);
    shifted.eventKind = emissive != 0u ? 2u : 1u;
    shifted.lobeMask = material.lobeMask;
    shifted.flags = source.flags
        & kUpt04CandidateFlagDirectProposalParity;
    shifted.age = source.age;
    shifted.pathLength = 1u;
    shifted.reconnectionVertexLength = 0u;
    shifted.identity0 = denseIndex + 1u;
    shifted.identity1 = Upt04StableLightIdentityFingerprint(
        Upt09LoadLight(denseIndex));
    shifted.replayKey = source.replayKey;
    shifted.replayIndex = source.replayIndex;
    shifted.generationFingerprint = kUpt03ExpectedGeneration;
    shifted.sampleCoord0 = source.sampleCoord0;
    shifted.sampleCoord1 = source.sampleCoord1;
    shifted.proposalPdf = proposalPdf;
    shifted.pathPdf = proposalPdf;
    shifted.partialJacobian = 1.0f;
    shifted.accumulatedRouletteProbability = 1.0f;
    if (shifted.status == kUpt04SampleValidZero)
        return 1u;
    shifted.contributionR = contribution.x;
    shifted.contributionG = contribution.y;
    shifted.contributionB = contribution.z;
    shifted.target = Upt04DirectReservoirTarget(
        contribution,
        gUpt09Control.geometryAvailabilityFlags);
    if (!Upt04FinitePositive(shifted.target))
        return 0u;

    if (!Upt04BuildPrimaryVisibilityRequest(
            receiver.worldPositionAndViewDepth.xyz,
            receiver.geometricNormalAndRoughness.xyz,
            receiver.shadingNormalAndOpacity.xyz,
            sample.direction,
            sample.distance,
            visibility))
        return 0u;
    if (emissive != 0u)
    {
        const Upt04UnifiedLightRecord visibilityLight =
            Upt09LoadLight(denseIndex);
        visibilityIgnore = Upt04MakeVisibilityIgnore(
            visibilityLight.instanceId, visibilityLight.primitiveIndex);
    }
    return 1u;
}

public float Upt09PrimaryDirectTargetAtReceiver<
    TMaterialSet : IUpt07PrimaryMaterialSet>(
    Upt03Candidate source,
    Upt04PrimarySurfaceRecord receiver)
{
    Upt03Candidate shifted;
    Upt04TraceRequest visibility;
    Upt04VisibilityIgnore visibilityIgnore;
    return Upt09ShiftPrimaryDirectUnshadowed<TMaterialSet>(
            source, receiver, shifted, visibility, visibilityIgnore) != 0u
        ? max(shifted.target, 0.0f) : 0.0f;
}

public bool Upt09SpatialSurfacesCompatible(
    Upt04PrimarySurfaceRecord receiver,
    Upt04PrimarySurfaceRecord neighbor)
{
    if (!Upt04PrimarySurfaceRecordValid(neighbor)
        || receiver.materialAndSurface.w != neighbor.materialAndSurface.w
        || !Upt04PrimarySurfaceAlphaReuseCompatible(receiver, neighbor)
        || !Upt04PrimarySurfaceMaterialsSimilar(receiver, neighbor))
        return false;
    const float3 receiverNormal = normalize(
        receiver.geometricNormalAndRoughness.xyz);
    const float3 neighborNormal = normalize(
        neighbor.geometricNormalAndRoughness.xyz);
    if (!all(isfinite(receiverNormal)) || !all(isfinite(neighborNormal))
        || dot(receiverNormal, neighborNormal) < 0.85f)
        return false;
    const float receiverDepth = receiver.worldPositionAndViewDepth.w;
    const float neighborDepth = neighbor.worldPositionAndViewDepth.w;
    const float depthTolerance = max(0.25f, abs(receiverDepth) * 0.10f);
    const float planeDistance = abs(dot(
        neighbor.worldPositionAndViewDepth.xyz
            - receiver.worldPositionAndViewDepth.xyz,
        receiverNormal));
    return isfinite(receiverDepth) && isfinite(neighborDepth)
        && abs(receiverDepth - neighborDepth) <= depthTolerance
        && isfinite(planeDistance) && planeDistance <= depthTolerance;
}

public int2 Upt09NeighborPixel(
    uint2 pixel,
    uint ordinal,
    uint2 dimensions)
{
    // Production walks a randomized contiguous portion of the 32-entry table,
    // so an input reservoir cannot be admitted repeatedly merely because two
    // independent draws selected the same offset.  The old with-replacement
    // schedule is retained only by proof mode 2 for a controlled A/B.
    const uint startIndex = min(
        uint(Upt09Random(pixel, 0u, 0x09110001u) * 32.0f), 31u);
    const uint offsetIndex = gUpt09Control.proofMode == 2u
        ? min(uint(Upt09Random(pixel, ordinal, 0x09110001u) * 32.0f), 31u)
        : ((startIndex + ordinal) & 31u);
    const int2 offset = int2(round(
        kUpt09NeighborOffsets[offsetIndex]
            * max(gUpt09Control.neighborRadius, 1.0f)));
    return clamp(
        int2(pixel) + offset,
        int2(0, 0),
        int2(dimensions) - int2(1, 1));
}

public Upt03PackedReservoir Upt09PackSpatialResult(
    Upt03LogicalReservoir logical,
    uint needsRescue)
{
    Upt03PackedReservoir packed = Upt03PackReservoir(logical);
    if (logical.hasSelectedSample == 0u && needsRescue != 0u)
        packed.word0 |= kUpt03PackedNeedsRescueBit;
    return packed;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt09Control.renderWidth
        || pixel.y >= gUpt09Control.renderHeight)
        return;
    const uint index = pixel.y * gUpt09Control.renderWidth + pixel.x;
    if (index >= gUpt09Control.surfaceCount)
        return;

    // This is an exact ownership control: S0 still dispatches, publishes the
    // alternate page, and participates in the normal page flip, but performs
    // no decode, reuse, visibility, or normalization.  Any image difference
    // from spatial-off in this mode is therefore outside the S0 estimator.
    if (gUpt09Control.proofMode == 1u)
    {
        gUpt09OutputReservoirs[index] = gUpt09InputReservoirs[index];
        return;
    }

    const Upt04PrimarySurfaceRecord receiver = Upt09LoadSurface(index);
    if (!Upt04PrimarySurfaceRecordValid(receiver))
    {
        gUpt09OutputReservoirs[index] = (Upt03PackedReservoir)0;
        return;
    }

    const Upt07DecodedReservoir center =
        Upt07DecodeReservoir(gUpt09InputReservoirs[index]);
    const bool centerSelected = Upt09DirectReservoirEligible(center);
    const bool pairwiseProof = gUpt09Control.proofMode == 6u;
    // Mode 4 mirrors the accepted legacy DI admission boundary exactly: an
    // empty center is not spatially rescued.  It is a proof control for the
    // distinction between ordinary selected-sample reuse and UPT09's separate
    // disocclusion/empty-center rescue policy.
    if ((gUpt09Control.proofMode == 4u || pairwiseProof) && !centerSelected)
    {
        gUpt09OutputReservoirs[index] = gUpt09InputReservoirs[index];
        return;
    }
    const uint centerM = center.valid != 0u
        ? min(center.logical.effectiveM, gUpt09Control.maximumInputM) : 0u;
    const float centerReceiverTarget = centerSelected
        && (gUpt09Control.proofMode == 5u || pairwiseProof)
        ? Upt09PrimaryDirectTargetAtReceiver<Upt07OpenPbrMaterialSet>(
            center.logical.selected, receiver)
        : (centerSelected ? center.logical.selected.target : 0.0f);
    uint outputM = centerM;
    float totalMass = centerSelected && !pairwiseProof
        ? center.logical.weightSum * float(centerM) : 0.0f;
    Upt03Candidate selected = centerSelected && !pairwiseProof
        ? center.logical.selected : (Upt03Candidate)0;
    float selectedSourceTarget = centerSelected
        ? centerReceiverTarget
        : 0.0f;
    float selectedReceiverTarget = centerSelected && !pairwiseProof
        ? centerReceiverTarget : 0.0f;
    uint selectedFromNeighbor = 0u;
    Upt04TraceRequest selectedVisibility = (Upt04TraceRequest)0;
    Upt04VisibilityIgnore selectedVisibilityIgnore =
        Upt04NoVisibilityIgnore();

    int2 acceptedPixels[kUpt09MaximumNeighborAttempts];
    uint acceptedM[kUpt09MaximumNeighborAttempts];
    uint acceptedCount = 0u;
    int2 sampledPixels[kUpt09MaximumNeighborAttempts];
    uint sampledCount = 0u;
    const uint requestedAttempts = min(
        centerSelected
            ? gUpt09Control.regularNeighborCount
            : gUpt09Control.rescueNeighborCount,
        kUpt09MaximumNeighborAttempts);
    const uint2 dimensions = uint2(
        gUpt09Control.renderWidth, gUpt09Control.renderHeight);
    const uint pairwiseK = max(requestedAttempts, 1u);
    float pairwiseCanonicalMisSum = 0.0f;
    uint pairwisePairsSeen = 0u;
    [loop]
    for (uint ordinal = 0u; ordinal < requestedAttempts; ++ordinal)
    {
        const int2 neighborPixel = Upt09NeighborPixel(
            pixel, ordinal, dimensions);
        if (all(neighborPixel == int2(pixel)))
            continue;
        // Distinct table entries can still collapse to the same edge pixel
        // after clamping.  Never stream one physical reservoir twice.  Proof
        // mode 2 deliberately retains the old behavior for comparison.
        bool duplicatePixel = false;
        if (gUpt09Control.proofMode != 2u)
        {
            [loop]
            for (uint sampledIndex = 0u;
                 sampledIndex < sampledCount;
                 ++sampledIndex)
            {
                duplicatePixel = duplicatePixel
                    || all(sampledPixels[sampledIndex] == neighborPixel);
            }
            if (duplicatePixel)
                continue;
            sampledPixels[sampledCount] = neighborPixel;
            ++sampledCount;
        }
        const uint neighborIndex = uint(neighborPixel.y) * dimensions.x
            + uint(neighborPixel.x);
        const Upt04PrimarySurfaceRecord neighborSurface =
            Upt09LoadSurface(neighborIndex);
        if (!Upt09SpatialSurfacesCompatible(receiver, neighborSurface))
            continue;
        const Upt07DecodedReservoir neighbor = Upt07DecodeReservoir(
            gUpt09InputReservoirs[neighborIndex]);
        const uint neighborM = neighbor.valid != 0u
            ? min(neighbor.logical.effectiveM,
                gUpt09Control.maximumInputM)
            : 0u;

        // Pairwise MIS compares the canonical technique with every physical,
        // surface-compatible neighbor technique before looking at the
        // neighbor's selected sample.  This is deliberately not the failing
        // post-hoc basic-correction denominator: each pair's two balance
        // weights sum locally, so a selected light having almost no support
        // on other neighbors cannot collapse a global divisor and recursively
        // amplify its published weight.
        if (pairwiseProof)
        {
            ++pairwisePairsSeen;
            const float centerTargetAtNeighbor = neighborM > 0u
                ? Upt09PrimaryDirectTargetAtReceiver<Upt07OpenPbrMaterialSet>(
                    center.logical.selected, neighborSurface)
                : 0.0f;
            const float canonicalTerm = float(centerM)
                * centerReceiverTarget;
            const float pairDenominator = float(pairwiseK)
                    * float(neighborM) * centerTargetAtNeighbor
                + canonicalTerm;
            pairwiseCanonicalMisSum += Upt04FinitePositive(pairDenominator)
                ? canonicalTerm / pairDenominator
                : 1.0f;
        }

        if (neighbor.valid == 0u)
            continue;

        if (neighborM == 0u || outputM > 0xffffffffu - neighborM)
            continue;

        // A count-only record is a defined zero trial, not an invalid
        // reservoir. D0 deliberately publishes M=1 without a selected sample
        // when its complete proposal schedule finds no positive candidate.
        // Dropping those records here conditioned the spatial estimator on
        // neighbors that happened to find illumination: adding neighbors
        // brightened sparse surfaces, while reducing the schedule made the
        // same surfaces dark. Preserve their confidence and include their
        // source domain in the later bias-correction denominator, but never
        // allow them to become the selected sample or issue a visibility ray.
        if (neighbor.logical.hasSelectedSample == 0u)
        {
            if (gUpt09Control.proofMode == 4u)
                continue;
            outputM += neighborM;
            acceptedPixels[acceptedCount] = neighborPixel;
            acceptedM[acceptedCount] = neighborM;
            ++acceptedCount;
            continue;
        }
        if (!Upt09DirectReservoirEligible(neighbor))
            continue;

        Upt03Candidate shifted;
        Upt04TraceRequest visibility;
        Upt04VisibilityIgnore visibilityIgnore;
        if (Upt09ShiftPrimaryDirectUnshadowed<Upt07OpenPbrMaterialSet>(
                neighbor.logical.selected,
                receiver,
                shifted,
                visibility,
                visibilityIgnore) == 0u)
            continue;
        outputM += neighborM;
        acceptedPixels[acceptedCount] = neighborPixel;
        acceptedM[acceptedCount] = neighborM;
        ++acceptedCount;

        // Basic correction needs the target of this sample in its source
        // domain twice: once to shift the reservoir mass and, if it wins, once
        // as the normalization numerator.  Treating the packed target as an
        // immutable source value is not safe after prior temporal/spatial
        // reuse.  Mode 5 isolates that contract by replaying the light at the
        // actual neighbor surface; modes 0..4 retain their prior behavior for
        // a strict runtime A/B.
        const float neighborSourceTarget =
            (gUpt09Control.proofMode == 5u || pairwiseProof)
            ? Upt09PrimaryDirectTargetAtReceiver<Upt07OpenPbrMaterialSet>(
                neighbor.logical.selected, neighborSurface)
            : neighbor.logical.selected.target;
        float neighborMass = 0.0f;
        if (shifted.status == kUpt04SampleValidPositive
            && Upt04FinitePositive(neighborSourceTarget))
        {
            if (pairwiseProof)
            {
                // m_i = M_i p_i / (k M_i p_i + M_c p_c).  Direct-light
                // replay has no reconnection Jacobian: p_i is the freshly
                // evaluated target at the donor and p_c is the shifted target
                // at this receiver.  MIS already carries confidence, so do
                // not multiply by M a second time.
                const float pairDenominator = float(pairwiseK)
                        * float(neighborM) * neighborSourceTarget
                    + float(centerM) * shifted.target;
                const float neighborMis = Upt04FinitePositive(pairDenominator)
                    ? float(neighborM) * neighborSourceTarget
                        / pairDenominator
                    : 0.0f;
                const float neighborUcw = Upt03ScaledWeightToUcw(
                    neighbor.logical.weightSum, neighborSourceTarget);
                neighborMass = neighborMis * shifted.target * neighborUcw;
            }
            else
            {
                neighborMass = neighbor.logical.weightSum * float(neighborM)
                    * (shifted.target / neighborSourceTarget);
            }
        }
        if (!isfinite(neighborMass) || neighborMass <= 0.0f)
            continue;
        const float newMass = totalMass + neighborMass;
        if (!Upt04FinitePositive(newMass))
            continue;
        if (selected.status != kUpt04SampleValidPositive
            || Upt09Random(pixel, ordinal, 0x09110002u) * newMass
                < neighborMass)
        {
            selected = shifted;
            selectedSourceTarget = neighborSourceTarget;
            selectedReceiverTarget = shifted.target;
            selectedFromNeighbor = 1u;
            selectedVisibility = visibility;
            selectedVisibilityIgnore = visibilityIgnore;
        }
        totalMass = newMass;
    }

    if (pairwiseProof)
    {
        // A missing physical pair gives the canonical technique the complete
        // pair weight.  Average over the configured k pairs, then stream the
        // canonical candidate exactly once.  This mirrors the normalization
        // proof in the repo-owned clean-room GI pairwise implementation while
        // using UPT's direct-light target/replay contract.
        const uint missingPairs = pairwiseK > pairwisePairsSeen
            ? pairwiseK - pairwisePairsSeen : 0u;
        const float canonicalMis =
            (pairwiseCanonicalMisSum + float(missingPairs))
            / float(pairwiseK);
        const float canonicalUcw = Upt03ScaledWeightToUcw(
            center.logical.weightSum, centerReceiverTarget);
        const float canonicalMass = canonicalMis * centerReceiverTarget
            * canonicalUcw;
        const float newMass = totalMass + canonicalMass;
        if (Upt04FinitePositive(canonicalMass)
            && Upt04FinitePositive(newMass)
            && (selected.status != kUpt04SampleValidPositive
                || Upt09Random(pixel, pairwiseK, 0x09110003u) * newMass
                    < canonicalMass))
        {
            selected = center.logical.selected;
            selected.target = centerReceiverTarget;
            selectedSourceTarget = centerReceiverTarget;
            selectedReceiverTarget = centerReceiverTarget;
            selectedFromNeighbor = 0u;
            selectedVisibility = (Upt04TraceRequest)0;
            selectedVisibilityIgnore = Upt04NoVisibilityIgnore();
        }
        if (Upt04FinitePositive(newMass))
            totalMass = newMass;
    }

    Upt03LogicalReservoir result = (Upt03LogicalReservoir)0;
    result.effectiveM = outputM;
    if (selected.status != kUpt04SampleValidPositive
        || !Upt04FinitePositive(totalMass)
        || !Upt04FinitePositive(selectedSourceTarget))
    {
        gUpt09OutputReservoirs[index] = Upt09PackSpatialResult(result, 1u);
        return;
    }

    float finalizedMeanWeight = 0.0f;
    if (pairwiseProof)
    {
        // Pair weights already form a partition of unity. totalMass is the
        // current-domain target-scaled UCW and is therefore exactly UPT's
        // persisted weight convention. Do not divide by the selected target:
        // that would persist UCW while resolve still expects pHat * UCW.
        finalizedMeanWeight = totalMass;
    }
    else
    {
        float normalizationDenominator = selected.target * float(centerM);
        [loop]
        for (uint acceptedIndex = 0u;
             acceptedIndex < acceptedCount;
             ++acceptedIndex)
        {
            const int2 neighborPixel = acceptedPixels[acceptedIndex];
            const uint neighborIndex = uint(neighborPixel.y) * dimensions.x
                + uint(neighborPixel.x);
            normalizationDenominator +=
                Upt09PrimaryDirectTargetAtReceiver<Upt07OpenPbrMaterialSet>(
                    selected,
                    Upt09LoadSurface(neighborIndex))
                * float(acceptedM[acceptedIndex]);
        }
        // Mode 3 removes only the pairwise-MIS/basic-correction denominator.
        // It keeps the same surfaces, shifted candidates, selection, and
        // visibility, and instead finalizes the streamed scalar weight with
        // the ordinary RIS 1/M estimator.
        // RTXDI BASIC correction uses the selected sample's target in its
        // supplying domain as pi. UPT stores pHat_receiver(selected) * W;
        // pHat_receiver cancels the final RTXDI denominator, leaving the same
        // source-domain numerator here.
        finalizedMeanWeight = gUpt09Control.proofMode == 3u
            ? totalMass / float(outputM)
            : totalMass * selectedSourceTarget / normalizationDenominator;
    }
    if (!Upt04FinitePositive(finalizedMeanWeight))
    {
        gUpt09OutputReservoirs[index] = Upt09PackSpatialResult(result, 1u);
        return;
    }

    if (selectedFromNeighbor != 0u)
    {
        selected.flags =
            (selected.flags & ~(kUpt04CandidateFlagVisibilityKnown
                | kUpt04CandidateFlagVisibilityPassed))
            | kUpt04CandidateFlagVisibilityKnown;
        if (!Upt09VisibilityPassed(
                selectedVisibility, selectedVisibilityIgnore))
        {
            gUpt09OutputReservoirs[index] = Upt09PackSpatialResult(result, 1u);
            return;
        }
        selected.flags |= kUpt04CandidateFlagVisibilityPassed;
    }
    // Empty-center rescue is useful for current-frame coverage, but it is not
    // a statistically independent source reservoir.  Publishing it without a
    // stamp allowed the same borrowed sample to donate again through temporal
    // and spatial reuse, so basic correction preserved and expanded a bright
    // island while 1/M merely turned the same feedback loop into progressive
    // darkness.  Direct candidates have no replay payload, so word4 provides
    // a zero-cost one-frame stamp.  Mode 2 intentionally omits it to reproduce
    // the former recursive behavior.
    if (!centerSelected && selectedFromNeighbor != 0u
        && gUpt09Control.proofMode != 2u)
    {
        selected.replayKey |= kUpt03ReplayKeyRescueBit;
    }
    result.hasSelectedSample = 1u;
    result.weightSum = finalizedMeanWeight;
    result.selected = selected;
    gUpt09OutputReservoirs[index] = Upt09PackSpatialResult(result, 0u);
}
