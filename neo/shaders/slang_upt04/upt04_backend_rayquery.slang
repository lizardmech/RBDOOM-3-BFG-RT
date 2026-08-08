import upt04_hit_contract;
import upt04_initial_bindings;

public Upt04HitFacts Upt04TraceRayQueryProof(
    Upt04TraceRequest request,
    bool readCommittedMetadata,
    bool visibilityTraversal,
    Upt04VisibilityIgnore ignore)
{
    Upt04HitFacts result = Upt04MissFacts(request.tMax);
    RayDesc ray;
    ray.Origin = request.origin;
    ray.TMin = request.tMin;
    ray.Direction = request.direction;
    ray.TMax = request.tMax;

    // Match the known-working Vulkan compute RayQuery source contract: keep the
    // type flags empty and provide the traversal flags at initialization.
    RayQuery<0> query = RayQuery<0>();
    const uint rayFlags = visibilityTraversal
        ? (RAY_FLAG_FORCE_NON_OPAQUE
            | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH)
        : (RAY_FLAG_FORCE_OPAQUE
            | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH);
    query.TraceRayInline(
        gUpt04Scene,
        rayFlags,
        0xffu,
        ray);
    while (query.Proceed())
    {
        if (!visibilityTraversal
            || query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        if (Upt04VisibilityIgnoresCandidate(
                ignore,
                query.CandidateInstanceID(),
                query.CandidatePrimitiveIndex()))
            continue;
        query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        result.status = kUpt04HitTriangle;
        if (readCommittedMetadata)
        {
            result.instanceId = query.CommittedInstanceID();
            result.geometryId = query.CommittedGeometryIndex();
            result.primitiveId = query.CommittedPrimitiveIndex();
            result.barycentrics = query.CommittedTriangleBarycentrics();
            result.hitT = query.CommittedRayT();
            result.frontFace = query.CommittedTriangleFrontFace() ? 1u : 0u;
        }
    }
    return result;
}

public Upt04HitFacts Upt04TraceRayQueryProof(
    Upt04TraceRequest request,
    bool readCommittedMetadata)
{
    return Upt04TraceRayQueryProof(
        request,
        readCommittedMetadata,
        false,
        Upt04NoVisibilityIgnore());
}

public Upt04HitFacts Upt04TraceRayQuery(Upt04TraceRequest request)
{
    return Upt04TraceRayQueryProof(request, true);
}

// Visibility traversal needs candidate control so that the selected emissive
// triangle cannot occlude its own endpoint. FORCE_NON_OPAQUE exposes triangle
// candidates to this loop; the first non-ignored triangle is committed and the
// ACCEPT_FIRST flag ends the search. Material-class filtering remains a later
// ABI decision and is intentionally not approximated here.
public Upt04HitFacts Upt04TraceVisibilityRayQuery(
    Upt04TraceRequest request,
    Upt04VisibilityIgnore ignore)
{
    return Upt04TraceRayQueryProof(request, true, true, ignore);
}
