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

// In-register visibility metadata. This is deliberately not part of the
// persistent reservoir or trace-work ABI: a selected emissive triangle can be
// reconstructed from the selected light immediately before traversal.
public struct Upt04VisibilityIgnore
{
    public uint instanceId;
    public uint primitiveId;
};

public Upt04VisibilityIgnore Upt04NoVisibilityIgnore()
{
    Upt04VisibilityIgnore ignore;
    ignore.instanceId = kUpt04InvalidIdentity;
    ignore.primitiveId = kUpt04InvalidIdentity;
    return ignore;
}

public Upt04VisibilityIgnore Upt04MakeVisibilityIgnore(
    uint instanceId,
    uint primitiveId)
{
    Upt04VisibilityIgnore ignore;
    ignore.instanceId = instanceId;
    ignore.primitiveId = primitiveId;
    return ignore;
}

public bool Upt04VisibilityIgnoresCandidate(
    Upt04VisibilityIgnore ignore,
    uint candidateInstanceId,
    uint candidatePrimitiveId)
{
    return ignore.instanceId != kUpt04InvalidIdentity
        && candidateInstanceId == ignore.instanceId
        && candidatePrimitiveId == ignore.primitiveId;
}

public float3 Upt04VisibilitySafeNormalize(float3 value, float3 fallback)
{
    const float valueLengthSquared = dot(value, value);
    if (all(isfinite(value)) && isfinite(valueLengthSquared)
        && valueLengthSquared > 1.0e-12f)
        return value * rsqrt(valueLengthSquared);
    const float fallbackLengthSquared = dot(fallback, fallback);
    return all(isfinite(fallback)) && isfinite(fallbackLengthSquared)
            && fallbackLengthSquared > 1.0e-12f
        ? fallback * rsqrt(fallbackLengthSquared)
        : float3(0.0f, 0.0f, 1.0f);
}

// Shared primary-direct shadow-ray contract. Doom world coordinates require
// substantially more separation than the old 0.02-unit endpoint epsilon.
// Both normals participate in the horizon test, matching the proven legacy DI
// behavior and preventing a shading-normal-only sample from tracing through
// the geometric back face.
public bool Upt04BuildPrimaryVisibilityRequest(
    float3 surfacePosition,
    float3 geometricNormalInput,
    float3 shadingNormalInput,
    float3 sampleDirectionInput,
    float sampleDistance,
    out Upt04TraceRequest request)
{
    request = (Upt04TraceRequest)0;
    const float3 shadingNormal = Upt04VisibilitySafeNormalize(
        shadingNormalInput, geometricNormalInput);
    const float3 geometricNormal = Upt04VisibilitySafeNormalize(
        geometricNormalInput, shadingNormal);
    const float3 direction = Upt04VisibilitySafeNormalize(
        sampleDirectionInput, geometricNormal);
    if (!all(isfinite(surfacePosition)) || !isfinite(sampleDistance)
        || sampleDistance <= 0.0f
        || dot(shadingNormal, direction) <= 0.0f
        || dot(geometricNormal, direction) <= 0.0f)
        return false;
    const float normalSign = dot(geometricNormal, shadingNormal) >= 0.0f
        ? 1.0f : -1.0f;
    request.origin = surfacePosition
        + geometricNormal * (normalSign * 0.75f)
        + direction * 0.25f;
    request.tMin = 0.01f;
    request.direction = direction;
    request.tMax = max(sampleDistance - 0.5f, request.tMin + 1.0e-4f);
    return Upt04TraceRequestValid(request);
}

// Use the accepted Doom-unit origin separation for the unbounded BSDF
// continuation as well. Unlike a shadow request there is no finite endpoint,
// but both geometric and shading horizons must still admit the sampled ray.
public bool Upt04BuildContinuationRequest(
    float3 surfacePosition,
    float3 geometricNormalInput,
    float3 shadingNormalInput,
    float3 continuationDirectionInput,
    out Upt04TraceRequest request)
{
    request = (Upt04TraceRequest)0;
    const float3 shadingNormal = Upt04VisibilitySafeNormalize(
        shadingNormalInput, geometricNormalInput);
    const float3 geometricNormal = Upt04VisibilitySafeNormalize(
        geometricNormalInput, shadingNormal);
    const float3 direction = Upt04VisibilitySafeNormalize(
        continuationDirectionInput, geometricNormal);
    if (!all(isfinite(surfacePosition))
        || dot(shadingNormal, direction) <= 0.0f
        || dot(geometricNormal, direction) <= 0.0f)
        return false;
    const float normalSign = dot(geometricNormal, shadingNormal) >= 0.0f
        ? 1.0f : -1.0f;
    request.origin = surfacePosition
        + geometricNormal * (normalSign * 0.75f)
        + direction * 0.25f;
    request.tMin = 0.01f;
    request.direction = direction;
    request.tMax = 1.0e30f;
    return Upt04TraceRequestValid(request);
}

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
