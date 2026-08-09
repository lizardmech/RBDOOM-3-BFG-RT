import upt04_hit_contract;
import upt04_geometry_contract;

[shader("closesthit")]
void main(
    inout Upt04HitFacts payload,
    BuiltInTriangleIntersectionAttributes attributes)
{
    payload.status = kUpt04HitTriangle;
    payload.instanceId = InstanceID();
    payload.geometryId = 0u;
    if (!Upt04CanonicalizeHardwarePrimitive(
            payload.instanceId,
            GeometryIndex(),
            PrimitiveIndex(),
            payload.primitiveId))
    {
        payload = Upt04MissFacts(RayTCurrent());
        return;
    }
    payload.barycentrics = attributes.barycentrics;
    payload.hitT = RayTCurrent();
    payload.frontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE ? 1u : 0u;
}
