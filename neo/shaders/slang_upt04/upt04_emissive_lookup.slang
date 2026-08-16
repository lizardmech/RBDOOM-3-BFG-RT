import upt04_initial_bindings;
import upt04_initial_control;
import upt04_geometry_bindings;
import upt04_light_contract;
import upt04_local_light_proposal;
import upt04_local_light_evaluation;
import upt04_material_provider_contract;
import upt04_geometry_contract;
import upt04_emissive_replay;

#ifndef UPT_FROZEN_STATIC_SCENE
#define UPT_FROZEN_STATIC_SCENE 0
#endif

#ifndef UPT_FROZEN_FLAT_LIGHTS
#define UPT_FROZEN_FLAT_LIGHTS UPT_FROZEN_STATIC_SCENE
#endif

public static const uint kUpt04ReverseNeeLookupFailure = 1u;
public static const uint kUpt04ReverseNeeZeroPdf = 2u;
public static const uint kUpt04ReverseNeePositivePdf = 3u;

public uint Upt04EmissiveLookupHash(uint instanceId, uint primitiveIndex)
{
    uint value = instanceId ^ (primitiveIndex + 0x9e3779b9u
        + (instanceId << 6u) + (instanceId >> 2u));
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

public bool Upt04FindEmissiveDenseLight(
    uint instanceId,
    uint primitiveIndex,
    out uint denseLightIndex,
    out float conditionalIdentityPdf)
{
#if UPT_FROZEN_FLAT_LIGHTS
    denseLightIndex = kUpt04InvalidLightIndex;
    conditionalIdentityPdf = 0.0f;
    return false;
#else
    denseLightIndex = kUpt04InvalidLightIndex;
    conditionalIdentityPdf = 0.0f;
    uint physicalEntryCount = 0u;
    uint entryStride = 0u;
    gUpt04EmissiveLookup.GetDimensions(physicalEntryCount, entryStride);
    if (!Upt04EmissiveLookupValid(
            gUpt04Control.availabilityFlags,
            gUpt04Control.emissiveLookupCapacityAndValid))
        return false;
    const uint logicalCapacity = Upt04EmissiveLookupCapacity(
        gUpt04Control.emissiveLookupCapacityAndValid);
    if (logicalCapacity < 2u
        || (logicalCapacity & (logicalCapacity - 1u)) != 0u
        || logicalCapacity > physicalEntryCount
        || entryStride != 20u)
        return false;

    const uint mask = logicalCapacity - 1u;
    const uint firstSlot = Upt04EmissiveLookupHash(instanceId, primitiveIndex) & mask;
    [unroll]
    for (uint probe = 0u; probe < kUpt04EmissiveLookupMaxProbes; ++probe)
    {
        const Upt04EmissiveLookupEntry entry =
            gUpt04EmissiveLookup[(firstSlot + probe) & mask];
        if (entry.occupied == 0u)
            return false;
        if (entry.instanceId == instanceId
            && entry.primitiveIndex == primitiveIndex)
        {
            if (entry.denseLightIndex >= gUpt04Control.currentLightCount)
                return false;
            const Upt04UnifiedLightRecord light =
                Upt04LoadCurrentLightRecord(entry.denseLightIndex);
            if (light.type != kUpt04LightEmissiveTriangle
                || light.instanceId != instanceId
                || light.primitiveIndex != primitiveIndex)
                return false;
            denseLightIndex = entry.denseLightIndex;
            conditionalIdentityPdf =
                max(entry.conditionalIdentityPdf, 0.0f);
            return true;
        }
    }
    return false;
#endif
}

public float Upt04ExactNeePdfForEmissiveHit<
    TMaterialSet : IUpt04MaterialSet>(
    uint instanceId,
    uint primitiveIndex,
    float3 receiverPosition,
    float2 hitBarycentrics,
    uint neeBounceIndex,
    out uint reverseStatus)
{
    reverseStatus = kUpt04ReverseNeeLookupFailure;
    uint denseLightIndex = kUpt04InvalidLightIndex;
    float conditionalIdentityPdf = 0.0f;
    if (!Upt04FindEmissiveDenseLight(
            instanceId,
            primitiveIndex,
            denseLightIndex,
            conditionalIdentityPdf))
        return 0.0f;
    reverseStatus = kUpt04ReverseNeeZeroPdf;

    Upt04LocalLightRanges ranges;
    ranges.emissiveStart = gUpt04Control.emissiveRangeStart;
    ranges.emissiveCount = gUpt04Control.emissiveRangeCount;
    ranges.analyticStart = gUpt04Control.analyticRangeStart;
    ranges.analyticCount = gUpt04Control.analyticRangeCount;
    if (!Upt04LocalLightRangesStructurallyValid(
            ranges, gUpt04Control.currentLightCount))
        return 0.0f;

    const uint proposalParity = Upt04DirectProposalParityValue(
        gUpt04Control.availabilityFlags);
    const uint configuredEmissiveTrials = Upt04ConfiguredEmissiveTrialCount(
        gUpt04Control.availabilityFlags);
    float selectionPdf = 0.0f;
    if (proposalParity != 0u && Upt04EmissiveDistributionValid(
            gUpt04Control.emissiveDistributionCountAndValid))
    {
        const uint emissiveTrials = Upt04InverseSquareNeeTrialCount(
            Upt04DirectRisDomainTrialCount(
                ranges,
                kUpt04LightEmissiveTriangle,
                proposalParity,
                configuredEmissiveTrials),
            neeBounceIndex);
        const uint analyticTrials = Upt04InverseSquareNeeTrialCount(
            Upt04DirectRisDomainTrialCount(
                ranges,
                kUpt04LightDoomAnalytic,
                proposalParity,
                configuredEmissiveTrials),
            neeBounceIndex);
        const uint candidateCount = emissiveTrials + analyticTrials;
        selectionPdf = candidateCount != 0u
            ? conditionalIdentityPdf * float(emissiveTrials)
                / float(candidateCount)
            : 0.0f;
    }
    else
    {
        const uint emissiveTrials = Upt04InverseSquareNeeTrialCount(
            Upt04DirectRisDomainTrialCount(
                ranges,
                kUpt04LightEmissiveTriangle,
                proposalParity,
                configuredEmissiveTrials),
            neeBounceIndex);
        const uint analyticTrials = Upt04InverseSquareNeeTrialCount(
            Upt04DirectRisDomainTrialCount(
                ranges,
                kUpt04LightDoomAnalytic,
                proposalParity,
                configuredEmissiveTrials),
            neeBounceIndex);
        const uint candidateCount = emissiveTrials + analyticTrials;
        selectionPdf = ranges.emissiveCount != 0u && candidateCount != 0u
            ? float(emissiveTrials)
                / float(candidateCount * ranges.emissiveCount)
            : 0.0f;
    }
    if (!Upt04FinitePositive(selectionPdf))
        return 0.0f;

    const Upt04UnifiedLightRecord light =
        Upt04LoadCurrentLightRecord(denseLightIndex);
    const Upt04EmissiveGeometrySample replay =
        Upt04ReplayEmissiveTriangleBarycentrics(
            light,
            receiverPosition,
            hitBarycentrics,
            (gUpt04Control.availabilityFlags
                & kUpt04EmissiveReplayTwoSided) != 0u);
    if (replay.status == kUpt04SampleInvalid
        || !Upt04FinitePositive(replay.solidAnglePdf))
        return 0.0f;
    float4 regionWeights;
    if (!Upt04ResolveEmissiveRegionWeights<TMaterialSet>(
            light, replay, regionWeights))
        return 0.0f;
    const uint region = Upt04EmissiveRegionFromBarycentrics(
        replay.barycentrics);
    const float solidAnglePdf = replay.solidAnglePdf
        * (4.0f * Upt04EmissiveRegionSelectionPdf(
            regionWeights, region));
    // Multi-sample balance heuristic density: selectionPdf is the normalized
    // per-draw typed mixture, so multiplying by the complete RIS trial count
    // yields sum_j n_j p_j for the heterogeneous NEE schedule.
    const uint candidateCount =
        Upt04InverseSquareNeeTrialCount(
            Upt04DirectRisDomainTrialCount(
                ranges,
                kUpt04LightEmissiveTriangle,
                proposalParity,
                configuredEmissiveTrials),
            neeBounceIndex)
        + Upt04InverseSquareNeeTrialCount(
            Upt04DirectRisDomainTrialCount(
                ranges,
                kUpt04LightDoomAnalytic,
                proposalParity,
                configuredEmissiveTrials),
            neeBounceIndex);
    const float neePdf = float(candidateCount)
        * selectionPdf * solidAnglePdf;
    if (!Upt04FinitePositive(neePdf))
        return 0.0f;
    reverseStatus = kUpt04ReverseNeePositivePdf;
    return neePdf;
}
