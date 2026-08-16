import upt04_hit_contract;
import upt04_material_contract;
import upt04_material_live_contract;
import upt04_material_emission;
import upt04_geometry_contract;
import upt04_emissive_replay;

// T0 and S0 intentionally share this exact emitter-side binding contract.
// Receiver shading stays in each pass's replaceable MaterialSet; only an
// emissive-triangle replay reaches this table and bindless texture sample.
public [[vk::binding(27, 0)]] StructuredBuffer<Upt04SmokeMaterialRecord>
    gUptDirectEmitterMaterials;
public [[vk::binding(28, 0)]] SamplerState gUptDirectEmitterSampler;
public [[vk::binding(0, 1)]] Texture2D<float4> gUptDirectEmitterTextures[];

public Upt04MaterialEmissionEvaluation UptDirectEvaluateEmissiveEndpoint(
    uint materialIndex,
    uint triangleClassAndFlags,
    float2 uv,
    float2 uvGradient0,
    float2 uvGradient1,
    uint materialCount,
    uint logicalTextureCount,
    float emissiveScale,
    float emissiveTexelBlackFloor)
{
    Upt04MaterialEmissionEvaluation invalid =
        (Upt04MaterialEmissionEvaluation)0;
    if (materialIndex >= materialCount || !all(isfinite(uv))
        || !all(isfinite(uvGradient0))
        || !all(isfinite(uvGradient1)))
        return invalid;

    const Upt04SmokeMaterialRecord material =
        gUptDirectEmitterMaterials[materialIndex];
    float3 emissiveTexel = float3(1.0f);
    uint emissiveTexelValid = 1u;
    if (material.emissiveTextureIndex != kUpt04InvalidIdentity)
    {
        if (material.emissiveTextureIndex >= logicalTextureCount)
            emissiveTexelValid = 0u;
        else
            emissiveTexel =
                gUptDirectEmitterTextures[NonUniformResourceIndex(
                    material.emissiveTextureIndex)]
                    .SampleGrad(
                        gUptDirectEmitterSampler,
                        frac(uv),
                        uvGradient0,
                        uvGradient1).xyz;
    }
    return Upt04FinalizeEmissiveEndpoint(
        material,
        triangleClassAndFlags,
        emissiveTexel,
        emissiveTexelValid,
        emissiveScale,
        emissiveTexelBlackFloor);
}

public float4 UptDirectEmissiveRegionWeights(
    Upt04EmissiveGeometrySample replay,
    uint materialCount,
    uint logicalTextureCount,
    float emissiveScale,
    float emissiveTexelBlackFloor)
{
    float4 weights = float4(0.0f);
    [unroll]
    for (uint region = 0u; region < 4u; ++region)
    {
        const float2 probeUv = Upt04EmissiveUvAtBarycentrics(
            replay, Upt04EmissiveRegionProbeBarycentrics(region));
        const Upt04MaterialEmissionEvaluation probe =
            UptDirectEvaluateEmissiveEndpoint(
                replay.materialIndex,
                replay.triangleClassAndFlags,
                probeUv,
                replay.emissionUvGradient0 * 0.5f,
                replay.emissionUvGradient1 * 0.5f,
                materialCount,
                logicalTextureCount,
                emissiveScale,
                emissiveTexelBlackFloor);
        const float importance = dot(
            max(probe.radiance, float3(0.0f)),
            float3(0.2126f, 0.7152f, 0.0722f));
        weights[region] = probe.valid != 0u && isfinite(importance)
            ? max(importance, 0.0f) : 0.0f;
    }
    return weights;
}
