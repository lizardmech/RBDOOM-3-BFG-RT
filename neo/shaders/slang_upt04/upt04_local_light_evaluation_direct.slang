import upt04_light_contract;
import upt04_local_light_proposal;
import upt04_geometry_contract;
import upt04_emissive_replay;

public Upt04EvaluatedLocalLight Upt04EvaluateSelectedLocalLightDirectOnly(
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
    if (selection.requiresTriangleReplay != 0u)
    {
        const Upt04EmissiveGeometrySample replay =
            Upt04ReplayEmissiveTriangleGeometry(
                light,
                receiverPosition,
                selection.sampleCoordinates,
                (policyFlags & kUpt04EmissiveReplayTwoSided) != 0u);
        if (replay.status == kUpt04SampleInvalid)
            return result;
        const float3 replayRadiance =
            max(light.radianceAndLuminance.xyz, float3(0.0f))
            * max(emissiveScale, 0.0f);
        result.sample.status = replay.status == kUpt04SampleValidPositive
                && any(replayRadiance > 0.0f)
            ? kUpt04SampleValidPositive
            : kUpt04SampleValidZero;
        result.sample.position = replay.position;
        result.sample.normal = replay.normal;
        result.sample.distance = replay.distance;
        result.sample.direction = replay.direction;
        result.sample.solidAnglePdf = replay.solidAnglePdf;
        result.sample.radiance = replayRadiance;
        result.storedSampleCoordinates = replay.barycentrics;
    }
    else
    {
        if (!Upt04AnalyticLightCurrentPortalEligible(light, policyFlags))
            return result;
        result.sample = Upt04SampleAnalyticSphere(
            light, receiverPosition, selection.sampleCoordinates);
    }
    result.valid = result.sample.status != kUpt04SampleInvalid ? 1u : 0u;
    return result;
}
