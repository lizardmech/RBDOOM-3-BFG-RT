import upt04_hit_contract;
import upt04_initial_bindings;
import upt04_material_alpha_clip;

public Upt04HitFacts Upt04TraceRayQueryProof(
    Upt04TraceRequest request,
    bool readCommittedMetadata,
    bool visibilityTraversal,
    Upt04VisibilityIgnore ignore,
    bool alphaClipTraversal)
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
        : RAY_FLAG_FORCE_NON_OPAQUE;
    query.TraceRayInline(
        gUpt04Scene,
        rayFlags,
        0xffu,
        ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        if (visibilityTraversal && Upt04VisibilityIgnoresCandidate(
                ignore,
                query.CandidateInstanceID(),
                query.CandidatePrimitiveIndex()))
            continue;
        if (alphaClipTraversal)
        {
            Upt04HitFacts candidate = (Upt04HitFacts)0;
            candidate.status = kUpt04HitTriangle;
            candidate.instanceId = query.CandidateInstanceID();
            candidate.geometryId = query.CandidateGeometryIndex();
            candidate.primitiveId = query.CandidatePrimitiveIndex();
            candidate.barycentrics = query.CandidateTriangleBarycentrics();
            candidate.hitT = query.CandidateTriangleRayT();
            candidate.frontFace = 1u;
            if (Upt04MaterialRejectsAlphaClipCandidate(candidate))
                continue;
        }
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
        Upt04NoVisibilityIgnore(),
        false);
}

public Upt04HitFacts Upt04TraceRayQuery(Upt04TraceRequest request)
{
    return Upt04TraceRayQueryProof(
        request, true, false, Upt04NoVisibilityIgnore(), true);
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
    return Upt04TraceRayQueryProof(request, true, true, ignore, false);
}

public Upt04HitFacts Upt04TraceIndirectVisibilityRayQuery(
    Upt04TraceRequest request,
    Upt04VisibilityIgnore ignore)
{
    return Upt04TraceRayQueryProof(request, true, true, ignore, true);
}
