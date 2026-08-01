import upt04_surface_contract;
import upt04_path_event_contract;

// Transport-neutral material boundary. ReSTIR modules may depend on these
// values, but never on a concrete material implementation or its resources.
public struct Upt04MaterialEvaluation
{
    public float3 weightedContribution;
    public float target;
    public uint lobeMask;
    public uint valid;
};
