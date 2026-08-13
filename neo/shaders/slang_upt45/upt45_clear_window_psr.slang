import upt45_glass_contract;
import upt45_narrow_geometry;
import upt04_binary16;
import upt04_packed_primary_surface;
import upt04_packed_primary_surface32;
import upt04_primary_history_sidecar;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure gUpt45Scene;
[[vk::binding(3, 0)]] RWStructuredBuffer<Upt04PackedPrimarySurfaceRecord32> gUpt45PrimaryOutput;
[[vk::binding(4, 0)]] RWStructuredBuffer<Upt04PrimaryHistorySidecar> gUpt45SidecarOutput;
[[vk::binding(5, 0)]]
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> gUpt45CompositionToken;
[[vk::binding(16, 0)]] StructuredBuffer<uint4> gUpt45MaterialWords;
[[vk::binding(25, 0)]] SamplerState gUpt45MaterialSampler;
[[vk::binding(26, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> gUpt45MotionVectors;
[[vk::binding(27, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> gUpt45RrMotionVectors;
[[vk::binding(28, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> gUpt45MotionMask;
[[vk::binding(29, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> gUpt45RrAlbedo;
[[vk::binding(30, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> gUpt45RrSpecularAlbedo;
[[vk::binding(31, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> gUpt45RrNormalRoughness;
[[vk::binding(32, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> gUpt45RrDepth;
[[vk::binding(33, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> gUpt45RrResetMask;
[[vk::binding(34, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> gUpt45RrPosition;
[[vk::binding(0, 1)]] Texture2D<float4> gUpt45MaterialTextures[];
[[vk::push_constant]] ConstantBuffer<Upt45Control> gUpt45Control;

public float Upt45SrgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f :
        pow((value + 0.055f) / 1.055f, 2.4f);
}

public float3 Upt45YCoCgToRgb(float4 ycocg)
{
    ycocg.z = ycocg.z * 31.875f + 1.0f;
    ycocg.z = 1.0f / ycocg.z;
    ycocg.xy *= ycocg.z;
    return saturate(float3(
        dot(ycocg, float4(1.0f, -1.0f, 0.0f, 1.0f)),
        dot(ycocg, float4(0.0f, 1.0f, -0.50196078f, 1.0f)),
        dot(ycocg, float4(-1.0f, -1.0f, 1.00392156f, 1.0f))));
}

public Upt45NarrowHit Upt45TraceBehind(float3 origin, float3 direction)
{
    Upt45NarrowHit hit = (Upt45NarrowHit)0;
    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = 0.01f;
    ray.Direction = direction;
    ray.TMax = 1.0e30f;
    RayQuery<0> query = RayQuery<0>();
    query.TraceRayInline(gUpt45Scene, RAY_FLAG_NONE, 0xffu, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        uint primitiveId;
        if (!Upt45CanonicalPrimitive(
                query.CandidateInstanceID(),
                query.CandidateGeometryIndex(),
                query.CandidatePrimitiveIndex(),
                primitiveId))
            continue;
        uint materialIndex;
        uint triangleClassAndFlags;
        if (!Upt45ResolveCandidateMaterialClass(
                query.CandidateInstanceID(),
                primitiveId,
                gUpt45Control,
                materialIndex,
                triangleClassAndFlags))
            continue;
        uint materialFlags = 0u;
        if (materialIndex < gUpt45Control.materialCount)
            materialFlags = gUpt45MaterialWords[materialIndex * 7u + 3u].z;
        const uint surfaceClass = triangleClassAndFlags & kUpt45SurfaceClassMask;
        const uint translucentSubtype =
            (triangleClassAndFlags & 0x0f000000u) >> 24u;
        const bool clearGlass =
            (materialFlags &
                (kUpt45MaterialPortalWindow | kUpt45MaterialObjectGlass)) != 0u ||
            (surfaceClass == kUpt45SurfaceClassTranslucent &&
                (translucentSubtype == 1u || translucentSubtype == 4u));
        if (clearGlass)
            continue;
        query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return hit;
    uint primitiveId;
    if (!Upt45CanonicalPrimitive(
            query.CommittedInstanceID(),
            query.CommittedGeometryIndex(),
            query.CommittedPrimitiveIndex(),
            primitiveId))
        return hit;
    hit.valid = 1u;
    hit.instanceId = query.CommittedInstanceID();
    hit.primitiveId = primitiveId;
    hit.frontFace = query.CommittedTriangleFrontFace() ? 1u : 0u;
    hit.barycentrics = query.CommittedTriangleBarycentrics();
    hit.hitT = query.CommittedRayT();
    return hit;
}

public void Upt45EvaluateMaterial(
    Upt45NarrowSurface surface,
    out float3 baseColor,
    out float3 specularF0,
    out float roughness,
    out float3 emission,
    out float alphaCutoff,
    out uint materialFlags)
{
    baseColor = float3(0.5f);
    specularF0 = float3(0.04f);
    roughness = 1.0f;
    emission = float3(0.0f);
    alphaCutoff = 0.0f;
    materialFlags = 0u;
    if (surface.materialIndex >= gUpt45Control.materialCount)
        return;
    const uint wordBase = surface.materialIndex * 7u;
    const float4 debugAlbedo = asfloat(gUpt45MaterialWords[wordBase + 0u]);
    const float4 emissiveColor = asfloat(gUpt45MaterialWords[wordBase + 1u]);
    const uint4 textureWords = gUpt45MaterialWords[wordBase + 2u];
    const uint4 sizeWords = gUpt45MaterialWords[wordBase + 3u];
    const uint diffuseTexture = textureWords.x;
    const uint specularTexture = textureWords.w;
    const uint emissiveTexture = sizeWords.x;
    alphaCutoff = asfloat(sizeWords.y);
    materialFlags = sizeWords.z;
    float4 diffuse = saturate(debugAlbedo);
    if ((materialFlags & kUpt45MaterialForceDebugAlbedo) == 0u &&
        diffuseTexture != kUpt45Invalid &&
        diffuseTexture < gUpt45Control.logicalTextureCount)
    {
        diffuse = saturate(gUpt45MaterialTextures[
            NonUniformResourceIndex(diffuseTexture)].SampleLevel(
                gUpt45MaterialSampler, frac(surface.texCoord), 0.0f));
        if ((materialFlags & kUpt45MaterialDiffuseYCoCg) != 0u)
            diffuse.xyz = Upt45YCoCgToRgb(diffuse);
    }
    baseColor = diffuse.xyz;
    if (specularTexture != kUpt45Invalid &&
        specularTexture < gUpt45Control.logicalTextureCount)
    {
        const float3 specular = saturate(gUpt45MaterialTextures[
            NonUniformResourceIndex(specularTexture)].SampleLevel(
                gUpt45MaterialSampler, frac(surface.texCoord), 0.0f).xyz);
        const float luminance = dot(specular, float3(0.2125f, 0.7154f, 0.0721f));
        specularF0 = max(specular, float3(0.04f));
        roughness = clamp(1.0f - sqrt(saturate(luminance)), 0.02f, 1.0f);
    }
    if ((materialFlags & kUpt45MaterialEmissive) != 0u &&
        (surface.triangleClassAndFlags & kUpt45TriangleEmissiveStageOff) == 0u)
    {
        float3 texel = float3(1.0f);
        if (emissiveTexture != kUpt45Invalid &&
            emissiveTexture < gUpt45Control.logicalTextureCount)
            texel = saturate(gUpt45MaterialTextures[
                NonUniformResourceIndex(emissiveTexture)].SampleLevel(
                    gUpt45MaterialSampler, frac(surface.texCoord), 0.0f).xyz);
        emission = max(emissiveColor.xyz, float3(0.0f)) * texel *
            1.75f * max(gUpt45Control.emissiveScale, 0.0f);
    }
}

public float Upt45RrDepthFromViewZ(float viewZ)
{
    const float zNear = max(gUpt45Control.rrNear, 1.0e-4f);
    const float zFar = 100000.0f;
    return saturate((zFar / max(zFar - zNear, 1.0e-4f)) *
        (1.0f - zNear / max(viewZ, zNear)));
}

public uint Upt45MotionSource(Upt45NarrowSurface surface)
{
    const uint surfaceClass = surface.triangleClassAndFlags & kUpt45SurfaceClassMask;
    if (surfaceClass == 2u)
        return 2u;
    if (surfaceClass == 1u)
        return 3u;
    return 1u;
}

public void Upt45WriteReplacementMotion(uint2 pixel, Upt45NarrowSurface surface)
{
    const uint sourceBits = (Upt45MotionSource(surface) & 0x0fu) <<
        kUpt45MotionSourceShift;
    if (gUpt45Control.cameraUpAndPreviousValid.w < 0.5f)
    {
        gUpt45MotionVectors[pixel] = float4(0.0f);
        gUpt45RrMotionVectors[pixel] = float2(0.0f);
        gUpt45MotionMask[pixel] = sourceBits |
            (kUpt45MotionMissingPreviousCamera << kUpt45MotionInvalidReasonShift);
        return;
    }
    if (surface.hasPreviousPosition == 0u)
    {
        gUpt45MotionVectors[pixel] = float4(0.0f);
        gUpt45RrMotionVectors[pixel] = float2(0.0f);
        gUpt45MotionMask[pixel] = sourceBits |
            (kUpt45MotionRejectedPrevious << kUpt45MotionInvalidReasonShift);
        return;
    }
    const float3 previousDelta = surface.previousPosition -
        gUpt45Control.previousCameraOrigin.xyz;
    const float3 currentDelta = surface.position - gUpt45Control.cameraOrigin;
    const float previousViewZ = dot(previousDelta,
        gUpt45Control.previousCameraForwardAndTanX.xyz);
    const float currentViewZ = dot(currentDelta,
        gUpt45Control.cameraForwardAndTanX.xyz);
    if (previousViewZ <= 0.05f || currentViewZ <= 0.05f)
    {
        gUpt45MotionVectors[pixel] = float4(0.0f);
        gUpt45RrMotionVectors[pixel] = float2(0.0f);
        gUpt45MotionMask[pixel] = sourceBits |
            (kUpt45MotionPreviousBehindCamera << kUpt45MotionInvalidReasonShift);
        return;
    }
    const float2 previousNdc = float2(
        -dot(previousDelta, gUpt45Control.previousCameraLeftAndTanY.xyz) /
            max(previousViewZ * gUpt45Control.previousCameraForwardAndTanX.w, 1.0e-5f),
        -dot(previousDelta, gUpt45Control.previousCameraUp.xyz) /
            max(previousViewZ * gUpt45Control.previousCameraLeftAndTanY.w, 1.0e-5f));
    const float2 currentNdc = float2(
        -dot(currentDelta, gUpt45Control.cameraLeftAndTanY.xyz) /
            max(currentViewZ * gUpt45Control.cameraForwardAndTanX.w, 1.0e-5f),
        -dot(currentDelta, gUpt45Control.cameraUpAndPreviousValid.xyz) /
            max(currentViewZ * gUpt45Control.cameraLeftAndTanY.w, 1.0e-5f));
    const float2 dimensions = float2(gUpt45Control.width, gUpt45Control.height);
    const float2 motionPixels = (previousNdc - currentNdc) * 0.5f * dimensions;
    if (!all(isfinite(motionPixels)))
    {
        gUpt45MotionVectors[pixel] = float4(0.0f);
        gUpt45RrMotionVectors[pixel] = float2(0.0f);
        gUpt45MotionMask[pixel] = sourceBits |
            (kUpt45MotionRejectedPrevious << kUpt45MotionInvalidReasonShift);
        return;
    }
    const float previousDepth = Upt45RrDepthFromViewZ(previousViewZ);
    const float currentDepth = Upt45RrDepthFromViewZ(currentViewZ);
    gUpt45MotionVectors[pixel] = float4(
        motionPixels, previousDepth - currentDepth, 0.0f);
    gUpt45RrMotionVectors[pixel] = motionPixels;
    gUpt45MotionMask[pixel] = kUpt45MotionValid | sourceBits;
}

public void Upt45WriteReplacementRrGuides(
    uint2 pixel,
    Upt45NarrowSurface surface,
    float3 baseColor,
    float3 specularF0,
    float roughness,
    float3 emission)
{
    if ((gUpt45Control.flags & kUpt45ControlWriteRrGuides) == 0u)
        return;
    const float peak = max(emission.x, max(emission.y, emission.z));
    const float3 rrSpecular = peak > 1.0e-4f
        ? max(saturate(specularF0), saturate(emission / peak))
        : saturate(specularF0);
    const float viewZ = dot(surface.position - gUpt45Control.cameraOrigin,
        gUpt45Control.cameraForwardAndTanX.xyz);
    gUpt45RrAlbedo[pixel] = float4(saturate(baseColor), 1.0f);
    gUpt45RrSpecularAlbedo[pixel] = float4(rrSpecular, 1.0f);
    gUpt45RrNormalRoughness[pixel] = float4(
        Upt45SafeNormalize(surface.shadingNormal, surface.geometricNormal),
        saturate(roughness));
    gUpt45RrDepth[pixel] = Upt45RrDepthFromViewZ(viewZ);
    gUpt45RrPosition[pixel] = float4(surface.position, 1.0f);
    Upt45WriteReplacementMotion(pixel, surface);
    gUpt45RrResetMask[pixel] =
        gUpt45Control.cameraUpAndPreviousValid.w < 0.5f ||
        surface.hasPreviousPosition == 0u
            ? kUpt45RrResetStochasticTranslucent
            : 0u;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt45Control.width || pixel.y >= gUpt45Control.height)
        return;
    const uint index = pixel.y * gUpt45Control.width + pixel.x;
    if (index >= gUpt45Control.surfaceCount)
        return;
    gUpt45CompositionToken[pixel] = float4(0.0f);

    const Upt04PackedPrimarySurfaceRecord32 front = gUpt45PrimaryOutput[index];
    const Upt04PrimaryHistorySidecar frontSidecar = gUpt45SidecarOutput[index];
    if (!Upt45FrontIsEligible(front, frontSidecar))
        return;

    const float hitDistance = asfloat(front.geometry.x);
    const float3 toCamera = Upt04DecodeOctahedral(front.geometry.w);
    if (!isfinite(hitDistance) || hitDistance <= 0.0f)
        return;
    const float3 frontPosition = gUpt45Control.cameraOrigin - toCamera * hitDistance;
    const float3 direction = -toCamera;
    const Upt45NarrowHit hit = Upt45TraceBehind(
        frontPosition + direction * max(gUpt45Control.forwardOffset, 0.01f),
        direction);
    if (hit.valid == 0u)
        return;
    const Upt45NarrowSurface surface = Upt45ResolveNarrowHit(hit, gUpt45Control);
    if (surface.valid == 0u)
        return;

    float3 baseColor, specularF0, emission;
    float roughness, alphaCutoff;
    uint materialFlags;
    Upt45EvaluateMaterial(
        surface, baseColor, specularF0, roughness, emission,
        alphaCutoff, materialFlags);
    const float3 replacementToCamera = Upt45SafeNormalize(
        gUpt45Control.cameraOrigin - surface.position, toCamera);
    const float replacementDistance = length(
        gUpt45Control.cameraOrigin - surface.position);
    Upt04PackedPrimarySurfaceRecord32 replacement = (Upt04PackedPrimarySurfaceRecord32)0;
    replacement.geometry = uint4(
        asuint(replacementDistance),
        Upt45PackOctahedral(surface.geometricNormal),
        Upt45PackOctahedral(surface.shadingNormal),
        Upt45PackOctahedral(replacementToCamera));
    replacement.material = uint4(
        Upt45PackRgb10(baseColor),
        Upt45PackRgb10(specularF0),
        Upt45PackSharedExponent(emission),
        (Upt04PackBinary16Pair(float2(saturate(roughness), 0.0f)) & 0xffffu) |
            ((kUpt04PackedPrimary32Version | kUpt04PackedPrimary32Valid) << 16u));
    gUpt45PrimaryOutput[index] = replacement;

    const uint surfaceClass = surface.triangleClassAndFlags & kUpt45SurfaceClassMask;
    uint historyFlags = kUpt45PrimaryValid;
    if (surface.hasPreviousPosition != 0u)
        historyFlags |= kUpt45PrimaryPreviousPosition;
    if (surfaceClass == 1u || surfaceClass == 2u)
        historyFlags |= kUpt45PrimaryObjectMotion;
    Upt04PrimaryHistorySidecar sidecar = (Upt04PrimaryHistorySidecar)0;
    sidecar.metadata = uint4(
        kUpt04PrimaryHistorySidecarVersion | (surfaceClass << 16u),
        historyFlags,
        surface.materialId,
        materialFlags);
    sidecar.previousPositionAndAlphaCutoff = float4(
        surface.previousPosition, alphaCutoff);
    gUpt45SidecarOutput[index] = sidecar;

    // Clear glass is absent from RR's geometric contract.  Publish the actual
    // see-through receiver for normal/depth/position and reconstruct motion
    // from that receiver, matching the established glass PSR path.
    Upt45WriteReplacementRrGuides(
        pixel, surface, baseColor, specularF0, roughness, emission);

    // Accepted legacy baseline: clear glass contributes no authored diffuse
    // tint or partial opacity.  The pane is retained only for eligibility and
    // future normal-map distortion; the resolved receiver owns appearance.
    gUpt45CompositionToken[pixel] = float4(1.0f, 1.0f, 1.0f, 1.0f);
}
