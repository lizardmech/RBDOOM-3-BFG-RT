import upt04_material_provider_contract;
import upt04_geometry_contract;
import upt04_material_contract;
import upt04_material_primary_live;
import upt04_material_set_live_probe;
import upt04_path_event_contract;
import upt04_primary_surface_live_contract;

// Concrete opaque UPT-05 material composition. Entry points select this type;
// ReSTIR and transport modules see only IUpt04MaterialSet.
public struct Upt05OpenPbrMaterialSet : IUpt04MaterialSet
{
    static Upt04PreparedDirectMaterial PreparePrimaryDirect(
        Upt04PrimarySurfaceRecord surface)
    {
        return Upt04PreparePrimaryDirectProbe(surface);
    }

    static Upt04PreparedDirectMaterial PrepareSecondaryDirect(
        Upt04GeometryHitSurface surface,
        float3 viewDirection)
    {
        return Upt04PrepareSecondaryDirectProbe(surface, viewDirection);
    }

    static Upt04MaterialEvaluation EvaluatePreparedDirect(
        Upt04PreparedDirectMaterial material,
        float3 lightDirection,
        float3 incidentRadiance)
    {
        return Upt04EvaluatePreparedDirectProbe(
            material, lightDirection, incidentRadiance);
    }

    static Upt04MaterialEvaluation EvaluatePrimaryDirect(
        Upt04PrimarySurfaceRecord surface,
        float3 lightDirection,
        float3 incidentRadiance)
    {
        return Upt04EvaluatePrimaryDirectProbe(
            surface, lightDirection, incidentRadiance);
    }

    static bool PrimaryContinuationAvailable(
        Upt04PrimarySurfaceRecord surface)
    {
        return Upt04PrimaryContinuationAvailable(surface);
    }

    static Upt04PathEvent SamplePrimaryContinuation(
        Upt04PrimarySurfaceRecord surface,
        float lobeRandom,
        float2 directionRandom)
    {
        return Upt04SamplePrimaryContinuation(
            surface, lobeRandom, directionRandom);
    }

    static Upt04MaterialEmissionEvaluation EvaluateEmissiveEndpoint(
        uint materialIndex,
        uint triangleClassAndFlags,
        float2 uv,
        float2 uvGradient0,
        float2 uvGradient1)
    {
        return Upt04EvaluateEmissiveEndpoint(
            materialIndex,
            triangleClassAndFlags,
            uv,
            uvGradient0,
            uvGradient1);
    }

    static Upt04MaterialEvaluation EvaluateSecondaryDirect(
        Upt04GeometryHitSurface surface,
        float3 viewDirection,
        float3 lightDirection,
        float3 incidentRadiance)
    {
        return Upt04EvaluateSecondaryDirectProbe(
            surface, viewDirection, lightDirection, incidentRadiance);
    }
}
