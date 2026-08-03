import upt04_material_contract;
import upt04_primary_surface_live_contract;

// T0 sees only this transport-neutral boundary. Concrete material composition
// remains replaceable without embedding OpenPBR or a fallback closure in the
// temporal reservoir/reprojection modules.
public interface IUpt07PrimaryMaterialSet
{
    static Upt04MaterialEvaluation EvaluatePrimaryDirect(
        Upt04PrimarySurfaceRecord surface,
        float3 lightDirection,
        float3 incidentRadiance);
}
