import upt04_hit_contract;
import upt04_initial_bindings;

public Upt04HitFacts Upt04TraceRayQueryProof(
    Upt04TraceRequest request,
    bool readCommittedMetadata)
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
    query.TraceRayInline(
        gUpt04Scene,
        RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xffu,
        ray);
    while (query.Proceed()) {}
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

public Upt04HitFacts Upt04TraceRayQuery(Upt04TraceRequest request)
{
    return Upt04TraceRayQueryProof(request, true);
}
