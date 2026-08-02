import upt04_geometry_contract;
import upt04_material_contract;
import upt04_path_event_contract;
import upt04_primary_surface_live_contract;

// Static material-provider contract specialized at the shader composition
// root. ReSTIR/transport modules are generic over this interface and therefore
// cannot import Lambert, OpenPBR, texture decode, or Doom fallback formulas.
public interface IUpt04MaterialSet
{
    static Upt04MaterialEvaluation EvaluatePrimaryDirect(
        Upt04PrimarySurfaceRecord surface,
        float3 lightDirection,
        float3 incidentRadiance);

    static bool PrimaryContinuationAvailable(
        Upt04PrimarySurfaceRecord surface);

    static Upt04PathEvent SamplePrimaryContinuation(
        Upt04PrimarySurfaceRecord surface,
        float lobeRandom,
        float2 directionRandom);

    static Upt04MaterialEmissionEvaluation EvaluateEmissiveEndpoint(
        uint materialIndex,
        uint triangleClassAndFlags,
        float2 uv);

    static Upt04MaterialEvaluation EvaluateSecondaryDirect(
        Upt04GeometryHitSurface surface,
        float3 viewDirection,
        float3 lightDirection,
        float3 incidentRadiance);
}
