import upt04_hit_contract;
import upt04_initial_bindings;

public Upt04HitFacts Upt04TraceRayQuery(Upt04TraceRequest request)
{
    Upt04HitFacts result = Upt04MissFacts(request.tMax);
    RayDesc ray;
    ray.Origin = request.origin;
    ray.TMin = request.tMin;
    ray.Direction = request.direction;
    ray.TMax = request.tMax;

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(gUpt04Scene, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed()) {}
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        result.status = kUpt04HitTriangle;
        result.instanceId = query.CommittedInstanceID();
        result.geometryId = query.CommittedGeometryIndex();
        result.primitiveId = query.CommittedPrimitiveIndex();
        result.barycentrics = query.CommittedTriangleBarycentrics();
        result.hitT = query.CommittedRayT();
        result.frontFace = query.CommittedTriangleFrontFace() ? 1u : 0u;
    }
    return result;
}
