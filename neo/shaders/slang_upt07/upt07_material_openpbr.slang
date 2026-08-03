import upt04_material_contract;
import upt04_material_primary_live;
import upt04_primary_surface_live_contract;
import upt07_material_provider_contract;

public struct Upt07OpenPbrMaterialSet : IUpt07PrimaryMaterialSet
{
    static Upt04MaterialEvaluation EvaluatePrimaryDirect(
        Upt04PrimarySurfaceRecord surface,
        float3 lightDirection,
        float3 incidentRadiance)
    {
        return Upt04EvaluatePrimaryDirectProbe(
            surface, lightDirection, incidentRadiance);
    }
}
