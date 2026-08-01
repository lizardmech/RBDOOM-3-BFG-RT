public static const uint kUpt04HitMiss = 0u;
public static const uint kUpt04HitTriangle = 1u;
public static const uint kUpt04InvalidIdentity = 0xffffffffu;

// Compact trace-adapter result and ray payload.  It deliberately contains no
// material or shading state.  Scalar layout gives this contract a 32-byte ABI.
public struct Upt04HitFacts
{
    public uint status;
    public uint instanceId;
    public uint geometryId;
    public uint primitiveId;
    public float2 barycentrics;
    public float hitT;
    public uint frontFace;
};

public struct Upt04TraceRequest
{
    public float3 origin;
    public float tMin;
    public float3 direction;
    public float tMax;
};

public Upt04HitFacts Upt04MissFacts(float tMax)
{
    Upt04HitFacts result = (Upt04HitFacts)0;
    result.status = kUpt04HitMiss;
    result.instanceId = kUpt04InvalidIdentity;
    result.geometryId = kUpt04InvalidIdentity;
    result.primitiveId = kUpt04InvalidIdentity;
    result.hitT = tMax;
    return result;
}

public bool Upt04TraceRequestValid(Upt04TraceRequest request)
{
    const float directionLengthSquared = dot(request.direction, request.direction);
    return all(isfinite(request.origin))
        && all(isfinite(request.direction))
        && isfinite(request.tMin)
        && isfinite(request.tMax)
        && directionLengthSquared > 0.999f
        && directionLengthSquared < 1.001f
        && request.tMin >= 0.0f
        && request.tMax > request.tMin;
}
