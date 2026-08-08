import upt03_reservoir_contract;
import upt04_initial_bindings;
import upt04_initial_control;
import upt04_initial_work_contract;
import upt04_primary_surface_live_contract;

public struct Upt04D0PreviousProjection
{
    public int2 pixel;
    public float linearDepth;
    public uint valid;
};

public struct Upt04D0PreviousBestSource
{
    public uint valid;
    public uint diagnostics;
    public uint historyIndex;
    public Upt03Candidate candidate;
};

public float Upt04D0UnpackSnorm16(uint bits)
{
    int value = int(bits & 0xffffu);
    if (value >= 32768)
        value -= 65536;
    return clamp(float(value) / 32767.0f, -1.0f, 1.0f);
}

public uint Upt04D0DecodePreviousSelected(
    Upt03PackedReservoir packed,
    out Upt03Candidate candidate)
{
    candidate = (Upt03Candidate)0;
    const uint header = packed.word0;
    if ((header & 0x0fu) != kUpt03PackedSelectedTag
        || (header & 0x08u) != 0u
        || packed.word1 == 0u
        || !Upt03FinitePositive(asfloat(packed.word11)))
        return 0u;

    candidate.status = 2u;
    candidate.eventKind = (header >> 4u) & 0x7u;
    candidate.lobeMask = (header >> 7u) & 0x1fu;
    candidate.flags = (header >> 12u) & 0x3fu;
    candidate.age = (header >> 18u) & 0x3fu;
    candidate.pathLength = (header >> 24u) & 0x0fu;
    candidate.reconnectionVertexLength = (header >> 28u) & 0x0fu;
    candidate.identity0 = packed.word2;
    candidate.identity1 = packed.word3;
    candidate.replayKey = packed.word4;
    candidate.replayIndex = packed.word5;
    candidate.generationFingerprint = kUpt03ExpectedGeneration;
    if (candidate.eventKind == 3u)
    {
        candidate.sampleCoord0 = Upt04D0UnpackSnorm16(packed.word6);
        candidate.sampleCoord1 = Upt04D0UnpackSnorm16(packed.word6 >> 16u);
    }
    else
    {
        candidate.sampleCoord0 = float(packed.word6 & 0xffffu) / 65535.0f;
        candidate.sampleCoord1 = float(packed.word6 >> 16u) / 65535.0f;
    }
    candidate.contributionR = asfloat(packed.word7);
    candidate.contributionG = asfloat(packed.word8);
    candidate.contributionB = asfloat(packed.word9);
    candidate.target = asfloat(packed.word10);
    candidate.proposalPdf = asfloat(packed.word12);
    candidate.pathPdf = asfloat(packed.word13);
    candidate.partialJacobian = asfloat(packed.word14);
    candidate.accumulatedRouletteProbability = asfloat(packed.word15);
    return Upt03StructurallyValid(candidate) ? 1u : 0u;
}

public Upt04D0PreviousProjection Upt04D0ProjectPrevious(
    Upt04PrimarySurfaceRecord current,
    uint2 dimensions)
{
    Upt04D0PreviousProjection result = (Upt04D0PreviousProjection)0;
    result.pixel = int2(-1, -1);
    if (dimensions.x == 0u || dimensions.y == 0u)
        return result;

    const float3 previousPosition = current.previousPositionOrMotion.w >= 0.5f
        ? current.previousPositionOrMotion.xyz
        : current.worldPositionAndViewDepth.xyz;
    const float3 cameraOrigin = float3(
        gUpt04Control.previousCameraOriginX,
        gUpt04Control.previousCameraOriginY,
        gUpt04Control.previousCameraOriginZ);
    const float3 cameraForward = float3(
        gUpt04Control.previousCameraForwardX,
        gUpt04Control.previousCameraForwardY,
        gUpt04Control.previousCameraForwardZ);
    const float3 cameraLeft = float3(
        gUpt04Control.previousCameraLeftX,
        gUpt04Control.previousCameraLeftY,
        gUpt04Control.previousCameraLeftZ);
    const float3 cameraUp = float3(
        gUpt04Control.previousCameraUpX,
        gUpt04Control.previousCameraUpY,
        gUpt04Control.previousCameraUpZ);
    const float3 delta = previousPosition - cameraOrigin;
    const float forwardDistance = dot(delta, cameraForward);
    result.linearDepth = length(delta);
    if (!isfinite(forwardDistance) || forwardDistance <= 0.05f
        || !isfinite(result.linearDepth)
        || !isfinite(gUpt04Control.previousCameraTanX)
        || !isfinite(gUpt04Control.previousCameraTanY)
        || gUpt04Control.previousCameraTanX <= 0.0f
        || gUpt04Control.previousCameraTanY <= 0.0f)
        return result;

    const float2 ndc = float2(
        -dot(delta, cameraLeft)
            / (forwardDistance * gUpt04Control.previousCameraTanX),
        -dot(delta, cameraUp)
            / (forwardDistance * gUpt04Control.previousCameraTanY));
    const float2 pixelFloat = (ndc * 0.5f + 0.5f) * float2(dimensions);
    if (!all(isfinite(ndc)) || !all(isfinite(pixelFloat)))
        return result;
    result.pixel = int2(floor(pixelFloat));
    if (result.pixel.x < 0 || result.pixel.y < 0
        || uint(result.pixel.x) >= dimensions.x
        || uint(result.pixel.y) >= dimensions.y)
        return (Upt04D0PreviousProjection)0;
    result.valid = 1u;
    return result;
}

public bool Upt04D0PreviousReceiverCompatible(
    Upt04PrimarySurfaceRecord current,
    Upt04PrimarySurfaceRecord previous,
    float projectedPreviousLinearDepth)
{
    if (!Upt04PrimarySurfaceRecordValid(previous)
        || !Upt04PrimarySurfaceAlphaReuseCompatible(current, previous))
        return false;
    const float currentLengthSquared = dot(
        current.geometricNormalAndRoughness.xyz,
        current.geometricNormalAndRoughness.xyz);
    const float previousLengthSquared = dot(
        previous.geometricNormalAndRoughness.xyz,
        previous.geometricNormalAndRoughness.xyz);
    if (!isfinite(currentLengthSquared) || !isfinite(previousLengthSquared)
        || currentLengthSquared <= 1.0e-12f
        || previousLengthSquared <= 1.0e-12f)
        return false;
    const float3 currentNormal = current.geometricNormalAndRoughness.xyz
        * rsqrt(currentLengthSquared);
    const float3 previousNormal = previous.geometricNormalAndRoughness.xyz
        * rsqrt(previousLengthSquared);
    if (dot(currentNormal, previousNormal) < 0.35f)
        return false;
    const float depthError = abs(previous.worldPositionAndViewDepth.w
        - projectedPreviousLinearDepth);
    const float depthTolerance = 0.10f * max(
        previous.worldPositionAndViewDepth.w,
        projectedPreviousLinearDepth);
    return isfinite(previous.worldPositionAndViewDepth.w)
        && isfinite(depthError) && isfinite(depthTolerance)
        && depthTolerance >= 0.0f && depthError <= depthTolerance;
}

public Upt04D0PreviousBestSource Upt04D0LoadPreviousBestSource(
    Upt04PrimarySurfaceRecord current)
{
    Upt04D0PreviousBestSource result = (Upt04D0PreviousBestSource)0;
    if (!Upt04D0PreviousBestHistoryAvailable(
            gUpt04Control.availabilityFlags,
            gUpt04Control.previousToCurrentLightCountAndHistory))
        return result;
    result.diagnostics |= kUpt04D0DiagPreviousLookupAttempted;

    const uint2 dimensions = uint2(
        gUpt04Control.renderWidth, gUpt04Control.renderHeight);
    const Upt04D0PreviousProjection projection =
        Upt04D0ProjectPrevious(current, dimensions);
    if (projection.valid == 0u)
        return result;
    result.historyIndex = uint(projection.pixel.y) * dimensions.x
        + uint(projection.pixel.x);
    const Upt04PrimarySurfaceRecord previous =
        Upt04LoadPreviousPrimarySurface(result.historyIndex);
    if (!Upt04D0PreviousReceiverCompatible(
            current, previous, projection.linearDepth))
        return result;

    const Upt03PackedReservoir packed =
        gUpt04PreviousReservoirs[result.historyIndex];
    if (Upt04D0DecodePreviousSelected(packed, result.candidate) == 0u
        || result.candidate.eventKind != 2u
        || (result.candidate.flags & kUpt04CandidateFlagGlobal) != 0u
        || Upt03IsDirectSpatialRescue(result.candidate)
        || result.candidate.pathLength != 1u
        || result.candidate.reconnectionVertexLength != 0u)
        return result;

    result.diagnostics |= kUpt04D0DiagPreviousLookupSucceeded;
    result.valid = 1u;
    return result;
}
