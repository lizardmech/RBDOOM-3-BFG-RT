import upt04_light_contract;
import upt04_local_light_proposal;
import upt04_geometry_contract;
import upt04_emissive_replay;
import upt04_material_contract;
import upt04_material_set_live_probe;

public struct Upt04EvaluatedLocalLight
{
    public uint valid;
    public Upt04LocalLightSample sample;
    public float2 storedSampleCoordinates;
};

public Upt04EvaluatedLocalLight Upt04EvaluateSelectedLocalLight(
    Upt04LocalLightSelection selection,
    Upt04UnifiedLightRecord light,
    float3 receiverPosition)
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
                selection.sampleCoordinates);
        if (replay.status == kUpt04SampleInvalid)
            return result;
        const Upt04MaterialEmissionEvaluation emission =
            Upt04EvaluateEmissiveEndpoint(
                replay.materialIndex,
                replay.triangleClassAndFlags,
                replay.texCoord);
        if (emission.valid == 0u)
            return result;
        result.sample.status = replay.status == kUpt04SampleValidPositive
                && any(emission.radiance > 0.0f)
            ? kUpt04SampleValidPositive
            : kUpt04SampleValidZero;
        result.sample.position = replay.position;
        result.sample.normal = replay.normal;
        result.sample.distance = replay.distance;
        result.sample.direction = replay.direction;
        result.sample.solidAnglePdf = replay.solidAnglePdf;
        result.sample.radiance = emission.radiance;
        result.storedSampleCoordinates = replay.barycentrics;
    }
    else
    {
        result.sample = Upt04SampleAnalyticSphere(
            light,
            receiverPosition,
            selection.sampleCoordinates);
    }
    result.valid = result.sample.status != kUpt04SampleInvalid ? 1u : 0u;
    return result;
}
