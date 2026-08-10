import upt04_material_provider_contract;
import upt04_geometry_contract;
import upt04_material_contract;
import upt04_path_event_contract;
import upt04_primary_surface_live_contract;
import upt05_material_lambert_diagnostic;

public struct Upt05LambertDiagnosticMaterialSet : IUpt04MaterialSet
{
    static Upt04PreparedDirectMaterial PreparePrimaryDirect(
        Upt04PrimarySurfaceRecord surface)
    { return Upt05LambertPreparePrimary(surface); }

    static Upt04PreparedDirectMaterial PrepareSecondaryDirect(
        Upt04GeometryHitSurface surface, float3 viewDirection)
    { return Upt05LambertPrepareSecondary(surface, viewDirection); }

    static Upt04MaterialEvaluation EvaluatePreparedDirect(
        Upt04PreparedDirectMaterial material, float3 lightDirection,
        float3 incidentRadiance)
    { return Upt05LambertEvaluatePrepared(material, lightDirection, incidentRadiance); }

    static Upt04MaterialEvaluation EvaluatePrimaryDirect(
        Upt04PrimarySurfaceRecord surface, float3 lightDirection,
        float3 incidentRadiance)
    {
        return Upt05LambertEvaluatePrepared(
            Upt05LambertPreparePrimary(surface), lightDirection, incidentRadiance);
    }

    static bool PrimaryContinuationAvailable(Upt04PrimarySurfaceRecord surface)
    { return Upt05LambertPreparePrimary(surface).valid != 0u; }

    static Upt04PathEvent SamplePrimaryContinuation(
        Upt04PrimarySurfaceRecord surface, float lobeRandom,
        float2 directionRandom)
    { return Upt05LambertSamplePrimary(surface, lobeRandom, directionRandom); }

    static Upt04MaterialEmissionEvaluation EvaluateEmissiveEndpoint(
        uint materialIndex, uint triangleClassAndFlags, float2 uv,
        float2 uvGradient0, float2 uvGradient1)
    {
        return Upt05LambertEvaluateEmissiveEndpoint(materialIndex,
            triangleClassAndFlags, uv, uvGradient0, uvGradient1);
    }

    static Upt04MaterialEvaluation EvaluateSecondaryDirect(
        Upt04GeometryHitSurface surface, float3 viewDirection,
        float3 lightDirection, float3 incidentRadiance)
    {
        return Upt05LambertEvaluatePrepared(
            Upt05LambertPrepareSecondary(surface, viewDirection),
            lightDirection, incidentRadiance);
    }
}
