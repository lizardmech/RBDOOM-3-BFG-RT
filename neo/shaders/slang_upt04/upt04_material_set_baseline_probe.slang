import upt04_material_contract;
import upt04_surface_contract;
import upt04_path_event_contract;

// Compile-probe implementation only. Production OpenPBR remains a separate
// material module and is not approximated inside the ReSTIR closure.
public Upt04MaterialEvaluation Upt04EvaluateBaselineProbe(Upt04Surface surface)
{
    Upt04MaterialEvaluation result = (Upt04MaterialEvaluation)0;
    if (surface.valid == 0u)
        return result;
    const float normalTerm = saturate(surface.shadingNormal.z * 0.5f + 0.5f);
    result.weightedContribution = float3(0.25f, 0.5f, 0.75f) * normalTerm;
    result.target = max(result.weightedContribution.x,
        max(result.weightedContribution.y, result.weightedContribution.z));
    result.lobeMask = kUpt04LobeDiffuse;
    result.valid = 1u;
    return result;
}
