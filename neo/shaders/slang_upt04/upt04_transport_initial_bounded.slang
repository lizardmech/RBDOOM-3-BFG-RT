import upt04_hit_contract;

public static const uint kUpt04TransportInitial = 0u;
public static const uint kUpt04TransportDirectVisibility = 1u;
public static const uint kUpt04TransportIndirectContinuation = 2u;
public static const uint kUpt04TransportIndirectNeeVisibility = 3u;
public static const uint kUpt04TransportComplete = 4u;

public struct Upt04RayBudget
{
    public uint continuationRays;
    public uint visibilityRays;
};

public struct Upt04BoundedTransportState
{
    public uint stage;
    public uint valid;
    public Upt04RayBudget budget;
};

public Upt04BoundedTransportState Upt04BeginBoundedTransport()
{
    Upt04BoundedTransportState result = (Upt04BoundedTransportState)0;
    result.stage = kUpt04TransportInitial;
    result.valid = 1u;
    return result;
}

public bool Upt04AdmitDirectVisibility(inout Upt04BoundedTransportState state)
{
    if (state.valid == 0u || state.stage != kUpt04TransportInitial
        || state.budget.continuationRays != 0u || state.budget.visibilityRays != 0u)
        return false;
    state.budget.visibilityRays = 1u;
    state.stage = kUpt04TransportDirectVisibility;
    return true;
}

public bool Upt04AdmitIndirectContinuation(inout Upt04BoundedTransportState state)
{
    if (state.valid == 0u || state.stage != kUpt04TransportInitial
        || state.budget.continuationRays != 0u || state.budget.visibilityRays != 0u)
        return false;
    state.budget.continuationRays = 1u;
    state.stage = kUpt04TransportIndirectContinuation;
    return true;
}

public bool Upt04AdmitIndirectNeeVisibility(inout Upt04BoundedTransportState state)
{
    if (state.valid == 0u || state.stage != kUpt04TransportIndirectContinuation
        || state.budget.continuationRays != 1u || state.budget.visibilityRays != 0u)
        return false;
    state.budget.visibilityRays = 1u;
    state.stage = kUpt04TransportIndirectNeeVisibility;
    return true;
}

public bool Upt04RayBudgetWithinD0Ceiling(Upt04BoundedTransportState state)
{
    return state.valid != 0u
        && state.budget.continuationRays <= 1u
        && state.budget.visibilityRays <= 1u
        && state.budget.continuationRays + state.budget.visibilityRays <= 2u;
}
