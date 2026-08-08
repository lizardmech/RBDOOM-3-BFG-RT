import upt04_surface_contract;
import upt04_path_event_contract;

// Transport-neutral material boundary. ReSTIR modules may depend on these
// values, but never on a concrete material implementation or its resources.
public struct Upt04MaterialEvaluation
{
    public float3 weightedContribution;
    public float target;
    public float directionalPdf;
    public uint lobeMask;
    public uint valid;
};

// Material-owned endpoint emission. Geometry/ReSTIR code sees only the
// evaluated radiance and validity; texture indices and Doom material policy
// remain inside the selected MaterialSet.
public struct Upt04MaterialEmissionEvaluation
{
    public float3 radiance;
    public uint valid;
};

// Reference RTXDI DI does not store an arbitrary RGB norm of the shaded
// sample as its target density.  Its direct-light target is luminance of the
// reflected contribution divided by the light sample's solid-angle PDF, and
// technique-MIS remains in the scalar streaming weight.  Keeping this measure
// identical in D0, T0, and S0 is essential: basic correction evaluates the
// selected sample at several receiver surfaces and is only valid when every
// term uses the same measure.
public static const uint kUpt04DirectTargetPdfParity = 0x20000000u;

public float Upt04DirectLuminance(float3 value)
{
    return dot(max(value, float3(0.0f)),
        float3(0.2126f, 0.7152f, 0.0722f));
}

public bool Upt04DirectTargetPdfParityEnabled(uint policyFlags)
{
    return (policyFlags & kUpt04DirectTargetPdfParity) != 0u;
}

public float3 Upt04DirectReservoirContribution(
    float3 weightedContribution,
    float solidAnglePdf,
    float techniqueMis,
    uint policyFlags)
{
    if (Upt04DirectTargetPdfParityEnabled(policyFlags))
    {
        return solidAnglePdf > 0.0f
            ? weightedContribution / solidAnglePdf : float3(0.0f);
    }
    return weightedContribution * techniqueMis;
}

public float Upt04DirectReservoirTarget(
    float3 reservoirContribution,
    uint policyFlags)
{
    return Upt04DirectTargetPdfParityEnabled(policyFlags)
        ? Upt04DirectLuminance(reservoirContribution)
        : max(reservoirContribution.x,
            max(reservoirContribution.y, reservoirContribution.z));
}

public float Upt04DirectReservoirSourcePdf(
    float lightSelectionPdf,
    float solidAnglePdf,
    float techniqueMis,
    uint policyFlags)
{
    if (Upt04DirectTargetPdfParityEnabled(policyFlags))
    {
        // target already contains 1 / solidAnglePdf.  Dividing by this
        // effective source PDF applies technique MIS once and the discrete
        // light-selection PDF once, matching RTXDI_StreamSample(target,
        // invLightSelectionPdf * techniqueMis).
        return techniqueMis > 0.0f
            ? lightSelectionPdf / techniqueMis : 0.0f;
    }
    return lightSelectionPdf * solidAnglePdf;
}
