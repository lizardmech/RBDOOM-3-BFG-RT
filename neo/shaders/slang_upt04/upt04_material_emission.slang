import upt04_hit_contract;
import upt04_material_contract;
import upt04_material_live_contract;

public static const uint kUpt04MaterialEmissive = 0x00000008u;
public static const uint kUpt04MaterialEmissiveLightCandidate = 0x00020000u;
public static const uint kUpt04TriangleEmissiveStageOff = 0x00040000u;

// Complete the emitter-side material evaluation after the binding owner has
// loaded the material and, when required, sampled its emissive texture.  Keep
// this conversion shared by D0/T0/S0: replaying identical triangle geometry
// with different source radiance changes p-hat between passes and feeds
// brightness errors back through temporal and spatial reuse.
public Upt04MaterialEmissionEvaluation Upt04FinalizeEmissiveEndpoint(
    Upt04SmokeMaterialRecord material,
    uint triangleClassAndFlags,
    float3 emissiveTexel,
    uint emissiveTexelValid,
    float emissiveScale,
    float emissiveTexelBlackFloor)
{
    Upt04MaterialEmissionEvaluation result =
        (Upt04MaterialEmissionEvaluation)0;
    if ((material.flags & kUpt04MaterialEmissive) == 0u
        || (triangleClassAndFlags & kUpt04TriangleEmissiveStageOff) != 0u)
    {
        result.valid = 1u;
        return result;
    }

    float3 radiance = max(material.emissiveColor.xyz, float3(0.0f));
    if (material.emissiveTextureIndex != kUpt04InvalidIdentity)
    {
        if (emissiveTexelValid == 0u)
            return result;
        const float3 sampledEmission = saturate(emissiveTexel);
        const float largestComponent = max(
            sampledEmission.x, max(sampledEmission.y, sampledEmission.z));
        radiance *= largestComponent > max(emissiveTexelBlackFloor, 0.0f)
            ? sampledEmission : float3(0.0f);
    }
    radiance *= 1.75f * max(emissiveScale, 0.0f);
    if (!all(isfinite(radiance)))
        return result;
    result.radiance = radiance;
    result.valid = 1u;
    return result;
}
