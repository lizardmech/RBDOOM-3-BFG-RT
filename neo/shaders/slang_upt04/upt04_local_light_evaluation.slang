import upt04_light_contract;
import upt04_local_light_proposal;
import upt04_geometry_contract;
import upt04_emissive_replay;
import upt04_material_contract;
import upt04_material_provider_contract;

#ifndef UPT04_PACKED_LIGHT_RECORD
#define UPT04_PACKED_LIGHT_RECORD 0
#endif
#ifndef UPT04_STATIC_ANALYTIC_INDIRECT_ONLY
#define UPT04_STATIC_ANALYTIC_INDIRECT_ONLY 0
#endif
#ifndef UPT04_EMISSIVE_COMPACT_CLASSIFY
#define UPT04_EMISSIVE_COMPACT_CLASSIFY 0
#endif
import upt04_random_white_hash;
import upt04_emissive_compact;

public float Upt04EmissiveImportance(float3 radiance)
{
    const float value = dot(
        max(radiance, float3(0.0f)),
        float3(0.2126f, 0.7152f, 0.0722f));
    return isfinite(value) ? max(value, 0.0f) : 0.0f;
}

public float4 Upt04EmissiveRegionWeights<
    TMaterialSet : IUpt04MaterialSet>(
    Upt04EmissiveGeometrySample replay)
{
    float4 weights = float4(0.0f);
    [unroll]
    for (uint region = 0u; region < 4u; ++region)
    {
        const float2 probeUv = Upt04EmissiveUvAtBarycentrics(
            replay, Upt04EmissiveRegionProbeBarycentrics(region));
        const Upt04MaterialEmissionEvaluation probe =
            TMaterialSet.EvaluateEmissiveEndpoint(
                replay.materialIndex,
                replay.triangleClassAndFlags,
                probeUv,
                replay.emissionUvGradient0 * 0.5f,
                replay.emissionUvGradient1 * 0.5f);
        weights[region] = probe.valid != 0u
            ? Upt04EmissiveImportance(probe.radiance) : 0.0f;
    }
    return weights;
}

public bool Upt04ResolveEmissiveRegionWeights<
    TMaterialSet : IUpt04MaterialSet>(
    Upt04UnifiedLightRecord light,
    Upt04EmissiveGeometrySample replay,
    out float4 weights)
{
#if UPT04_PACKED_LIGHT_RECORD
    weights = light.positionAndRadius;
    return light.type == kUpt04LightEmissiveTriangle
        && all(isfinite(weights)) && all(weights >= 0.0f);
#else
    weights = Upt04EmissiveRegionWeights<TMaterialSet>(replay);
    return all(isfinite(weights)) && all(weights >= 0.0f);
#endif
}

public Upt04EvaluatedLocalLight Upt04EvaluateSelectedLocalLight<
    TMaterialSet : IUpt04MaterialSet>(
    Upt04LocalLightSelection selection,
    Upt04UnifiedLightRecord light,
    float3 receiverPosition,
    uint policyFlags,
    float emissiveScale)
{
    Upt04EvaluatedLocalLight result = (Upt04EvaluatedLocalLight)0;
    result.storedSampleCoordinates = selection.sampleCoordinates;
    if (!Upt04SelectionMatchesRecord(selection, light))
        return result;

#if UPT04_STATIC_ANALYTIC_INDIRECT_ONLY
    // Static footprint proof. Unlike a zero emissive range at runtime, this
    // specialization must contain no triangle replay or material-texture
    // emission closure at all.
    if (selection.requiresTriangleReplay != 0u
        || selection.type != kUpt04LightDoomAnalytic)
        return result;
    if (!Upt04AnalyticLightCurrentPortalEligible(light, policyFlags))
        return result;
    result.sample = Upt04SampleAnalyticSphere(
        light,
        receiverPosition,
        selection.sampleCoordinates);
#elif UPT04_EMISSIVE_COMPACT_CLASSIFY
    if (selection.requiresTriangleReplay != 0u)
        return result;
    if (!Upt04AnalyticLightCurrentPortalEligible(light, policyFlags))
        return result;
    result.sample = Upt04SampleAnalyticSphere(
        light,
        receiverPosition,
        selection.sampleCoordinates);
#else
    if (selection.requiresTriangleReplay != 0u)
    {
#if UPT04_PACKED_LIGHT_RECORD
        const float4 regionWeights = light.positionAndRadius;
        if (!all(isfinite(regionWeights)) || any(regionWeights < 0.0f))
            return result;
#else
        // First resolve supplies the triangle UV basis used by the bounded
        // four-region proposal.  Region choice is hashed separately so the
        // original two random components remain a uniform point inside the
        // selected equal-area subtriangle.
        const Upt04EmissiveGeometrySample preliminaryReplay =
            Upt04ReplayEmissiveTriangleGeometry(
                light,
                receiverPosition,
                selection.sampleCoordinates,
                (policyFlags & kUpt04EmissiveReplayTwoSided) != 0u);
        if (preliminaryReplay.status == kUpt04SampleInvalid)
            return result;
        const float4 regionWeights =
            Upt04EmissiveRegionWeights<TMaterialSet>(preliminaryReplay);
#endif
        uint regionSeed = Upt04HashWord(
            asuint(selection.sampleCoordinates.x)
            ^ Upt04HashWord(asuint(selection.sampleCoordinates.y))
            ^ light.identityA ^ (light.identityB * 0x9e3779b9u));
        const float regionRandom = float(regionSeed >> 8u)
            * (1.0f / 16777216.0f);
        const uint region = Upt04SelectEmissiveRegion(
            regionWeights, regionRandom);
        const float2 selectedBarycentrics =
            Upt04SampleEmissiveRegionBarycentrics(
                region, selection.sampleCoordinates);
        Upt04EmissiveGeometrySample replay =
            Upt04ReplayEmissiveTriangleBarycentrics(
                light,
                receiverPosition,
                selectedBarycentrics,
                (policyFlags & kUpt04EmissiveReplayTwoSided) != 0u);
        if (replay.status == kUpt04SampleInvalid)
            return result;
        const Upt04MaterialEmissionEvaluation emission =
            TMaterialSet.EvaluateEmissiveEndpoint(
                replay.materialIndex,
                replay.triangleClassAndFlags,
                replay.emissionTexCoord,
                float2(0.0f),
                float2(0.0f));
        if (emission.valid == 0u)
            return result;
        // A valid zero is a sampled black texel in the additive emissive mask,
        // not a failed replay. Falling back to the triangle-level manager
        // estimate here turns the entire wall/object triangle into an emitter.
        const float3 replayRadiance = emission.radiance;
        result.sample.status = replay.status == kUpt04SampleValidPositive
                && any(replayRadiance > 0.0f)
            ? kUpt04SampleValidPositive
            : kUpt04SampleValidZero;
        result.sample.position = replay.position;
        result.sample.normal = replay.normal;
        result.sample.distance = replay.distance;
        result.sample.direction = replay.direction;
        result.sample.solidAnglePdf = replay.solidAnglePdf
            * (4.0f * Upt04EmissiveRegionSelectionPdf(
                regionWeights, region));
        result.sample.reserved = asuint(
            4.0f * Upt04EmissiveRegionSelectionPdf(regionWeights, region));
        result.sample.radiance = replayRadiance;
        result.storedSampleCoordinates = replay.barycentrics;
    }
    else
    {
        if (!Upt04AnalyticLightCurrentPortalEligible(light, policyFlags))
            return result;
        result.sample = Upt04SampleAnalyticSphere(
            light,
            receiverPosition,
            selection.sampleCoordinates);
    }
#endif
    result.valid = result.sample.status != kUpt04SampleInvalid ? 1u : 0u;
    return result;
}
