import upt04_material_contract;
import upt04_surface_contract;
import upt04_path_event_contract;

public Upt04MaterialEvaluation Upt04EvaluateDeterministicProbe(Upt04Surface surface)
{
    Upt04MaterialEvaluation result = (Upt04MaterialEvaluation)0;
    if (surface.valid == 0u)
        return result;
    result.weightedContribution = float3(0.5f, 0.25f, 0.125f);
    result.target = 0.5f;
    result.lobeMask = kUpt04LobeDiffuse;
    result.valid = 1u;
    return result;
}
