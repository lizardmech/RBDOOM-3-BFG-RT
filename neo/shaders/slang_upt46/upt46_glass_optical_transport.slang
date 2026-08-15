import upt04_backend_rayquery;
import upt04_geometry_contract;
import upt04_geometry_decode;
import upt04_hit_contract;
import upt04_initial_bindings;
import upt04_material_contract;
import upt04_material_alpha_clip;
import upt04_primary_surface_live_contract;
import upt04_packed_primary_surface;
import upt04_packed_primary_surface32;
import upt04_primary_history_sidecar;
import upt04_random_white_hash;
import upt05_material_set_openpbr;

public static const uint kUpt46MaterialKindGlass = 5u;
public static const uint kUpt46MaterialCapPathTransmission = 1u << 4u;
public static const uint kUpt46MaterialLobeSpecularTransmission = 1u << 3u;
public static const uint kUpt46MaterialFeatureAbi = 4u;
public static const uint kUpt46MaterialAdditiveDecal = 0x00000004u;
public static const uint kUpt46MaterialFilterDecal = 0x00000010u;
public static const uint kUpt46MaterialObjectGlassFallback = 0x00000400u;
public static const uint kUpt46MaterialPortalWindowFallback = 0x00000200u;
public static const uint kUpt46MaterialAdditiveDecalWhiteKey = 0x00000800u;
public static const uint kUpt46MaterialEmissive = 0x00000008u;
public static const uint kUpt46MaterialSkyEnvironment = 0x00040000u;

public struct Upt46MaterialFeatureRecord
{
    public uint materialKind;
    public uint materialCaps;
    public uint lobeCaps;
    public uint passSupport;
    public uint modifierKind;
    public uint parameterRecordIndex;
    public uint recordAbiVersion;
    public uint reserved1;
};

public struct Upt46MaterialFeatureParameterRecord
{
    public float4 params0;
    public float4 params1;
    public uint4 orderedStages0;
    public uint4 orderedStages1;
    public uint4 orderedStageTextures0;
    public uint4 orderedStageTextures1;
};

public struct Upt46OpticalControl
{
    public uint renderWidth;
    public uint renderHeight;
    public uint surfaceCount;
    public uint flags;
    public float3 cameraOrigin;
    public float reflectionComposeBoost;
    public float skyBrightness;
    public float rayTMax;
    public uint materialFeatureCount;
    public uint materialFeatureParameterCount;
};

[[vk::binding(33, 0)]] StructuredBuffer<Upt46MaterialFeatureRecord>
    gUpt46MaterialFeatures;
[[vk::binding(34, 0)]] StructuredBuffer<Upt46MaterialFeatureParameterRecord>
    gUpt46MaterialFeatureParameters;
[[vk::binding(35, 0)]] TextureCube<float4> gUpt46Sky;
[[vk::binding(37, 0)]]
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> gUpt46TransmissionOutput;
[[vk::binding(38, 0)]]
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> gUpt46ReflectionOutput;
[[vk::binding(39, 0)]] StructuredBuffer<Upt46OpticalControl> gUpt46OpticalRecords;
[[vk::binding(40, 0)]]
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> gUpt46RrSpecularAlbedo;
[[vk::binding(41, 0)]]
[[vk::image_format("r32f")]]
RWTexture2D<float> gUpt46RrHitDistance;
[[vk::binding(42, 0)]] StructuredBuffer<Upt04PackedPrimarySurfaceRecord32>
    gUpt46CanonicalPrimaryReceivers;
[[vk::binding(43, 0)]] RWStructuredBuffer<uint> gUpt46GlassDiagnostic;
[[vk::binding(44, 0)]]
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> gUpt46RrAlbedo;
[[vk::binding(45, 0)]] RWStructuredBuffer<Upt04PackedPrimarySurfaceRecord>
    gUpt46ReflectionReceivers;
[[vk::binding(46, 0)]] RWStructuredBuffer<Upt04PrimaryHistorySidecar>
    gUpt46ReflectionSidecars;

public Upt46OpticalControl Upt46Control()
{
    return gUpt46OpticalRecords[0];
}

public float3 Upt46SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    if (all(isfinite(value)) && isfinite(lengthSquared) && lengthSquared > 1.0e-12f)
        return value * rsqrt(lengthSquared);
    return Upt04VisibilitySafeNormalize(fallback, float3(0.0f, 0.0f, 1.0f));
}

public bool Upt46FeatureGlass(uint materialIndex, out float ior)
{
    ior = 1.45f;
    const Upt46OpticalControl optical = Upt46Control();
    if (materialIndex >= optical.materialFeatureCount)
        return false;
    const Upt46MaterialFeatureRecord feature = gUpt46MaterialFeatures[materialIndex];
    const bool glass = feature.recordAbiVersion == kUpt46MaterialFeatureAbi
        && feature.parameterRecordIndex == materialIndex
        && feature.materialKind == kUpt46MaterialKindGlass
        && (feature.materialCaps & kUpt46MaterialCapPathTransmission) != 0u
        && (feature.lobeCaps & kUpt46MaterialLobeSpecularTransmission) != 0u;
    if (glass && materialIndex < optical.materialFeatureParameterCount)
        ior = max(gUpt46MaterialFeatureParameters[materialIndex].params1.x, 1.0001f);
    return glass;
}

public bool Upt46LoadGlassParameters(
    uint materialIndex,
    out float3 transmittance,
    out float thickness,
    out float ior,
    out float strength,
    out float reflectionBoost,
    out float transmissionFloor)
{
    transmittance = float3(1.0f);
    thickness = 0.0f;
    ior = 1.45f;
    strength = 1.0f;
    reflectionBoost = 1.0f;
    transmissionFloor = 0.0f;
    if (!Upt46FeatureGlass(materialIndex, ior))
        return false;
    const Upt46OpticalControl optical = Upt46Control();
    if (materialIndex >= optical.materialFeatureParameterCount)
        return true;
    const Upt46MaterialFeatureParameterRecord parameters =
        gUpt46MaterialFeatureParameters[materialIndex];
    transmittance = saturate(parameters.params0.xyz);
    thickness = max(parameters.params0.w, 0.0f);
    ior = max(parameters.params1.x, 1.0001f);
    strength = saturate(parameters.params1.y);
    reflectionBoost = max(parameters.params1.z, 0.0f);
    transmissionFloor = max(parameters.params1.w, 0.0f);
    return true;
}

// Match the accepted legacy transmission traversal rather than recognizing
// only the base glass material. Doom 3 glass declarations can contribute a
// filter/additive grime card as a separate traced material record. That card
// is part of the pane optics and must not become the terminal reflected or
// transmitted hit. Keep this candidate test on UPT's narrow material/class
// resolvers: pulling the full vertex/attribute decode into this loop bloats the
// ray-query family and needlessly raises its live working set.
public bool Upt46HitDoesNotOccludeOptical(Upt04HitFacts hit)
{
    uint materialIndex;
    if (!Upt04ResolveTriangleMaterialIndex(hit, materialIndex)
        || materialIndex >= gUpt04Control.materialCount)
        return false;
    float unusedIor;
    if (Upt46FeatureGlass(materialIndex, unusedIor))
        return true;
    const uint flags = Upt04LoadMaterialFlags(materialIndex);
    const uint alwaysTransparentFlags =
        kUpt46MaterialAdditiveDecal
        | kUpt46MaterialFilterDecal
        | kUpt46MaterialPortalWindowFallback
        | kUpt46MaterialObjectGlassFallback
        | kUpt46MaterialAdditiveDecalWhiteKey;
    if ((flags & alwaysTransparentFlags) != 0u)
        return true;
    uint triangleClassAndFlags;
    return Upt04ResolveTriangleClassAndFlags(hit, triangleClassAndFlags)
        && Upt04TriangleClassIsTransmittingGlass(triangleClassAndFlags);
}

public float3 Upt46TransparentHitEmission(Upt04HitFacts hit)
{
    const Upt04GeometryHitSurface surface = Upt04DecodeGeometryHit(hit);
    if (surface.valid == 0u)
        return float3(0.0f);
    const Upt04MaterialEmissionEvaluation emission =
        Upt05OpenPbrMaterialSet.EvaluateEmissiveEndpoint(
            surface.materialIndex,
            surface.triangleClassAndFlags,
            surface.texCoord,
            float2(0.0f),
            float2(0.0f));
    return emission.valid != 0u && all(isfinite(emission.radiance))
        ? max(emission.radiance, float3(0.0f))
        : float3(0.0f);
}

// Optical continuation traversal shares UPT's canonical primitive mapping,
// alpha test and geometry decoder. Supported glass candidates are transparent
// to the bounded R/T continuation, so a second pane cannot become an opaque
// placeholder merely because it is encountered by an optical ray.
public Upt04HitFacts Upt46TraceOptical(
    Upt04TraceRequest request,
    bool collectPassthroughEmission,
    out float3 passthroughEmission)
{
    passthroughEmission = float3(0.0f);
    float nearestEmissionT = request.tMax;
    float3 nearestEmission = float3(0.0f);
    Upt04HitFacts result = Upt04MissFacts(request.tMax);
    RayDesc ray;
    ray.Origin = request.origin;
    ray.TMin = request.tMin;
    ray.Direction = request.direction;
    ray.TMax = request.tMax;
    RayQuery<0> query = RayQuery<0>();
    // Match the accepted reflection/transmission service. Glass identity is a
    // shader material property, so fixed-function opaque commitment must not
    // bypass the candidate classifier when an old BLAS opacity decision is
    // stale or a routed surface lacks its fallback bit.
    query.TraceRayInline(
        gUpt04Scene, RAY_FLAG_FORCE_NON_OPAQUE, 0xffu, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        uint primitiveId;
        if (!Upt04CanonicalizeHardwarePrimitive(
                query.CandidateInstanceID(),
                query.CandidateGeometryIndex(),
                query.CandidatePrimitiveIndex(),
                primitiveId))
            continue;
        Upt04HitFacts candidate = (Upt04HitFacts)0;
        candidate.status = kUpt04HitTriangle;
        candidate.instanceId = query.CandidateInstanceID();
        candidate.geometryId = 0u;
        candidate.primitiveId = primitiveId;
        candidate.barycentrics = query.CandidateTriangleBarycentrics();
        candidate.hitT = query.CandidateTriangleRayT();
        candidate.frontFace = 1u;
        if (Upt46HitDoesNotOccludeOptical(candidate))
        {
            if (collectPassthroughEmission)
            {
                const float3 emission = Upt46TransparentHitEmission(candidate);
                if (any(emission > 0.0f) && candidate.hitT < nearestEmissionT)
                {
                    nearestEmissionT = candidate.hitT;
                    nearestEmission = emission;
                }
            }
            continue;
        }
        if (Upt04MaterialRejectsAlphaClipCandidate(candidate))
            continue;
        query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        passthroughEmission = nearestEmission;
        return result;
    }
    result.status = kUpt04HitTriangle;
    result.instanceId = query.CommittedInstanceID();
    result.geometryId = 0u;
    if (!Upt04CanonicalizeHardwarePrimitive(
            result.instanceId,
            query.CommittedGeometryIndex(),
            query.CommittedPrimitiveIndex(),
            result.primitiveId))
        return Upt04MissFacts(request.tMax);
    result.barycentrics = query.CommittedTriangleBarycentrics();
    result.hitT = query.CommittedRayT();
    result.frontFace = query.CommittedTriangleFrontFace() ? 1u : 0u;
    if (nearestEmissionT < result.hitT)
        passthroughEmission = nearestEmission;
    return result;
}

public float Upt46Pow5(float value)
{
    const float value2 = value * value;
    return value2 * value2 * value;
}

public float3 Upt46Sky(float3 direction)
{
    const Upt46OpticalControl optical = Upt46Control();
    if ((optical.flags & 1u) == 0u)
        return float3(0.0f);
    const float3 encoded = max(
        gUpt46Sky.SampleLevel(gUpt04MaterialSampler, direction, 0.0f).rgb,
        float3(0.0f));
    const float3 linear = float3(
        encoded.x <= 0.04045f ? encoded.x / 12.92f : pow((encoded.x + 0.055f) / 1.055f, 2.4f),
        encoded.y <= 0.04045f ? encoded.y / 12.92f : pow((encoded.y + 0.055f) / 1.055f, 2.4f),
        encoded.z <= 0.04045f ? encoded.z / 12.92f : pow((encoded.z + 0.055f) / 1.055f, 2.4f));
    return linear * max(optical.skyBrightness, 0.0f);
}

// Clear-glass reflections deliberately use one deterministic material lookup,
// not a private DI/GI estimator.  This matches the production glass shape used
// by RTXDI FullSample and Remix-style glass: trace one sharp reflected ray,
// ignore other glass along that ray, and return a stable flat material signal.
// max(diffuse, F0) keeps metal hits visible without adding two independent
// albedo lobes or introducing any random light/shadow work.
public float3 Upt46ShadeFlatReflectionHit(
    Upt04HitFacts hit,
    float3 incomingDirection,
    out float3 diffuseAlbedo,
    out float3 specularF0,
    out float3 terminalEmission)
{
    diffuseAlbedo = float3(0.0f);
    specularF0 = float3(0.0f);
    terminalEmission = float3(0.0f);
    if (hit.status == kUpt04HitMiss)
        return Upt46Sky(incomingDirection);
    const Upt04GeometryHitSurface surface = Upt04DecodeGeometryHit(hit);
    if (surface.valid == 0u)
        return float3(0.0f);
    const Upt04PreparedDirectMaterial material =
        Upt05OpenPbrMaterialSet.PrepareSecondaryDirect(
            surface, -incomingDirection);
    if (material.valid == 0u)
        return float3(0.0f);
    diffuseAlbedo = saturate(material.baseColor);
    specularF0 = saturate(material.specularF0);
    const Upt04MaterialEmissionEvaluation emission =
        Upt05OpenPbrMaterialSet.EvaluateEmissiveEndpoint(
            surface.materialIndex,
            surface.triangleClassAndFlags,
            surface.texCoord,
            float2(0.0f),
            float2(0.0f));
    terminalEmission = emission.valid != 0u
        && all(isfinite(emission.radiance))
        ? max(emission.radiance, float3(0.0f))
        : float3(0.0f);
    // Raw texture albedo is a material parameter, not scene radiance. RTXDI
    // FullSample's deterministic glass reflection uses the same deliberately
    // small fixed ambient response so dark receivers do not glow in mirrors.
    const float3 flatAmbient = float3(0.05f, 0.045f, 0.03f);
    const float3 flatMaterial = max(diffuseAlbedo, specularF0) * flatAmbient;
    const float3 reflected = terminalEmission + flatMaterial;
    return all(isfinite(reflected)) ? reflected : terminalEmission;
}

public uint Upt46PackSharedExponentEmission(float3 value)
{
    const float3 finiteValue = all(isfinite(value))
        ? min(max(value, float3(0.0f)), float3(65408.0f))
        : float3(0.0f);
    const float maximum = max(finiteValue.x, max(finiteValue.y, finiteValue.z));
    if (maximum <= 0.0f)
        return 0u;
    uint exponent = uint(clamp(int(floor(log2(maximum))) + 16, 0, 31));
    float scale = exp2(24.0f - float(exponent));
    uint3 mantissa = uint3(round(finiteValue * scale));
    if (max(mantissa.x, max(mantissa.y, mantissa.z)) > 511u && exponent < 31u)
    {
        ++exponent;
        scale *= 0.5f;
        mantissa = uint3(round(finiteValue * scale));
    }
    mantissa = min(mantissa, uint3(511u));
    return mantissa.x | (mantissa.y << 9u) | (mantissa.z << 18u)
        | (exponent << 27u);
}

public uint Upt46ReflectionInstanceFingerprint(uint instanceId)
{
    // Reflection Compact48 leaves the upper binary16 lane beside roughness
    // unused. Preserve a small, nonzero endpoint identity there so private
    // T0/S0 cannot propagate a donor across a moving reflected-object edge.
    // The hash matters for static-bucket instance IDs whose distinguishing
    // bits live above bit 15; zero remains the invalid/unpublished sentinel.
    const uint fingerprint = Upt04HashWord(instanceId ^ 0x46c0ffeeu) & 0xffffu;
    return fingerprint != 0u ? fingerprint : 1u;
}

public void Upt46PublishReflectionReceiver(
    uint reflectionIndex,
    Upt04PrimarySurfaceRecord pane,
    Upt04HitFacts hit,
    float3 reflectedDirection,
    float3 terminalEmission)
{
    Upt04PackedPrimarySurfaceRecord packed = (Upt04PackedPrimarySurfaceRecord)0;
    packed.positionAndHeader.w = kUpt04PackedPrimaryVersion;
    Upt04PrimaryHistorySidecar sidecar = (Upt04PrimaryHistorySidecar)0;
    sidecar.metadata.x = kUpt04PrimaryHistorySidecarVersion;

    if (hit.status == kUpt04HitTriangle)
    {
        const Upt04GeometryHitSurface surface = Upt04DecodeGeometryHit(hit);
        const Upt04PreparedDirectMaterial material =
            Upt05OpenPbrMaterialSet.PrepareSecondaryDirect(
                surface, -reflectedDirection);
        if (surface.valid != 0u && material.valid != 0u)
        {
            packed.positionAndHeader = uint4(
                asuint(surface.position),
                kUpt04PackedPrimaryVersion | kUpt04PackedPrimaryValid);
            packed.directionsAndRoughness = uint4(
                Upt04EncodeVertexOctahedral(surface.geometricNormal),
                Upt04EncodeVertexOctahedral(material.normal),
                Upt04EncodeVertexOctahedral(material.viewDirection),
                Upt04PackHalf2(float2(material.roughness, 0.0f))
                    | (Upt46ReflectionInstanceFingerprint(hit.instanceId)
                        << 16u));
            packed.material = uint4(
                Upt04PackHalf2(material.baseColor.xy),
                Upt04PackHalf2(float2(material.baseColor.z, material.specularF0.x)),
                Upt04PackHalf2(material.specularF0.yz),
                Upt46PackSharedExponentEmission(terminalEmission));

            // The reflection receiver is not camera-visible, so its own world
            // position cannot drive ordinary screen-space reprojection. The
            // sidecar always carries the front-pane anchor, including for a
            // static pane. Previously HasPreviousPosition was copied only
            // when the pane itself advertised object motion; static glass then
            // silently projected the reflected hit and T0 rejected almost all
            // history, leaving the child resolve looking like raw D0.
            const uint paneFlags =
                (pane.header.y & kUpt04PrimarySurfaceHasObjectMotion) |
                kUpt04PrimarySurfaceHasPreviousPosition;
            const float3 panePrevious =
                (paneFlags & kUpt04PrimarySurfaceHasPreviousPosition) != 0u
                    && pane.previousPositionOrMotion.w >= 0.5f
                ? pane.previousPositionOrMotion.xyz
                : pane.worldPositionAndViewDepth.xyz;
            sidecar.metadata = uint4(
                kUpt04PrimaryHistorySidecarVersion |
                    ((pane.materialAndSurface.w & 0xffffu) << 16u),
                kUpt04PrimarySurfaceValid | paneFlags,
                surface.materialIndex + 1u,
                surface.materialIndex < gUpt04Control.materialCount
                    ? Upt04LoadMaterialFlags(surface.materialIndex) : 0u);
            // Reflection T0 projects the front-pane anchor but evaluates the
            // candidate at the exact reflected hit packed above.  The fourth
            // lane is pane depth for the ordinary temporal depth gate.
            sidecar.previousPositionAndAlphaCutoff = float4(
                panePrevious,
                max(pane.worldPositionAndViewDepth.w, 0.0f));
        }
    }
    gUpt46ReflectionReceivers[reflectionIndex] = packed;
    gUpt46ReflectionSidecars[reflectionIndex] = sidecar;
}

// Canonical transmission only needs exact terminal environment/emission for
// endpoints Compact32 cannot represent.  Keep that narrow fallback separate
// from the deliberately unlit glass-reflection contract above.
public float3 Upt46ShadeTransmissionTerminal(
    Upt04HitFacts hit,
    float3 incomingDirection)
{
    if (hit.status == kUpt04HitMiss)
        return Upt46Sky(incomingDirection);
    const Upt04GeometryHitSurface surface = Upt04DecodeGeometryHit(hit);
    if (surface.valid == 0u)
        return float3(0.0f);
    const Upt04MaterialEmissionEvaluation emission =
        Upt05OpenPbrMaterialSet.EvaluateEmissiveEndpoint(
            surface.materialIndex,
            surface.triangleClassAndFlags,
            surface.texCoord,
            float2(0.0f),
            float2(0.0f));
    return emission.valid != 0u && all(isfinite(emission.radiance))
        ? max(emission.radiance, float3(0.0f)) : float3(0.0f);
}

public void Upt46PublishReflectionRrGuides(
    uint2 pixel,
    Upt04HitFacts hit,
    float3 reflectedDiffuseAlbedo,
    float3 reflectedSpecularF0,
    float3 reflectedColorContribution)
{
    if (hit.status == kUpt04HitTriangle)
    {
        if (isfinite(hit.hitT) && hit.hitT > 0.0f)
            gUpt46RrHitDistance[pixel] = hit.hitT;
    }

    // DLSS RR's practical input contract in this renderer is deliberately not
    // a physical F0-only material description. Mirror/glass radiance must be
    // duplicated into the specular channel at the same HDR brightness as the
    // contribution added to beauty, otherwise RR erases a bright reflection
    // over a dark transmitted background and retains only a Fresnel hint.
    // This must be byte-for-byte the radiance term G2 adds to beauty at this
    // glass/mirror pixel. Do not retain primary F0 or an unrelated reservoir
    // guide here: either changes the brightness and recreates the sparse
    // Fresnel-only signal seen by RR diagnostics.
    const bool reflectionReuse = (Upt46Control().flags & 4u) != 0u;
    const float materialBlend = 0.35f;
    const float3 behindAlbedo = max(
        gUpt46RrAlbedo[pixel].rgb, float3(0.0f));
    const float3 behindSpecular = max(
        gUpt46RrSpecularAlbedo[pixel].rgb, float3(0.0f));
    if (!reflectionReuse)
    {
        gUpt46RrAlbedo[pixel] = float4(
            saturate(behindAlbedo +
                saturate(reflectedDiffuseAlbedo) * materialBlend),
            1.0f);
        gUpt46RrSpecularAlbedo[pixel] = float4(
            max(
                max(behindSpecular,
                    saturate(reflectedSpecularF0) * materialBlend),
                max(reflectedColorContribution, float3(0.0f))),
            1.0f);
    }
    else
    {
        // The isolated reflection reservoir is a specular path even when its
        // terminal material is diffuse. Leave the canonical behind-glass
        // diffuse guide untouched and classify the exact reflected radiance
        // as specular for RR reconstruction.
        gUpt46RrSpecularAlbedo[pixel] = float4(
            max(reflectedColorContribution, float3(0.0f)), 1.0f);
    }
}

public void Upt46BuildThinGlassWeights(
    uint materialIndex,
    float cosine,
    out float3 transmissionWeight,
    out float3 reflectionWeight)
{
    float3 transmittance;
    float thickness;
    float ior;
    float strength;
    float reflectionBoost;
    float transmissionFloor;
    const bool valid = Upt46LoadGlassParameters(
        materialIndex,
        transmittance,
        thickness,
        ior,
        strength,
        reflectionBoost,
        transmissionFloor);
    if (!valid)
    {
        transmittance = float3(1.0f);
        thickness = 0.0f;
        ior = 1.45f;
        strength = 1.0f;
        reflectionBoost = 1.0f;
        transmissionFloor = 0.0f;
    }

    const float eta = rcp(max(ior, 1.0001f));
    const float refractedCosine = sqrt(max(
        1.0f - eta * eta * (1.0f - cosine * cosine), 0.0f));
    const float f0Ratio = (ior - 1.0f) / (ior + 1.0f);
    const float f0 = saturate(f0Ratio * f0Ratio);
    const float outsideFresnel = saturate(
        f0 + (1.0f - f0) * Upt46Pow5(1.0f - cosine));
    const float insideFresnel = saturate(
        f0 + (1.0f - f0) * Upt46Pow5(1.0f - refractedCosine));
    const float attenuationDistance = thickness /
        max(refractedCosine, 1.0e-3f);
    const float3 attenuation = pow(
        max(transmittance, float3(1.0e-4f)),
        float3(attenuationDistance));
    const float3 geometricSeries = rcp(max(
        1.0f - attenuation * insideFresnel,
        float3(1.0e-4f)));
    const float3 thinTransmission = saturate(
        attenuation * (1.0f - outsideFresnel)
            * (1.0f - insideFresnel) * geometricSeries);
    const float3 thinReflection = saturate(
        outsideFresnel
            + insideFresnel * attenuation * attenuation
                * (1.0f - outsideFresnel)
                * (1.0f - insideFresnel) * geometricSeries);
    transmissionWeight = max(
        lerp(float3(1.0f), thinTransmission, strength),
        float3(transmissionFloor));
    reflectionWeight = saturate(
        thinReflection * strength * reflectionBoost);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    const Upt46OpticalControl optical = Upt46Control();
    if (pixel.x >= optical.renderWidth
        || pixel.y >= optical.renderHeight)
        return;
    const uint index = pixel.y * optical.renderWidth + pixel.x;
    if (index >= optical.surfaceCount)
        return;
    const bool reflectionReuse = (optical.flags & 4u) != 0u;
    const bool reflectionFullResolution = (optical.flags & 8u) != 0u;
    const bool reflectionDomainPixel = reflectionReuse
        && (reflectionFullResolution
            || ((pixel.x & 1u) == 0u && (pixel.y & 1u) == 0u));
    const uint reflectionWidth = reflectionFullResolution
        ? optical.renderWidth : ((optical.renderWidth + 1u) >> 1u);
    const uint2 reflectionPixel = reflectionFullResolution
        ? pixel : (pixel >> 1u);
    const uint reflectionIndex = reflectionPixel.y * reflectionWidth
        + reflectionPixel.x;
    if (reflectionDomainPixel)
    {
        gUpt46ReflectionReceivers[reflectionIndex] =
            (Upt04PackedPrimarySurfaceRecord)0;
        gUpt46ReflectionSidecars[reflectionIndex] =
            (Upt04PrimaryHistorySidecar)0;
    }
    const bool diagnosticPixel = (optical.flags & 2u) != 0u
        && pixel.x == optical.renderWidth / 2u
        && pixel.y == optical.renderHeight / 2u;
    if (diagnosticPixel)
    {
        gUpt46GlassDiagnostic[0] = 0x47314631u; // "G1F1"
        gUpt46GlassDiagnostic[1] = 0u;
        gUpt46GlassDiagnostic[2] = index;
    }

    gUpt46TransmissionOutput[pixel] = float4(0.0f);
    gUpt46ReflectionOutput[pixel] = float4(0.0f);
    const Upt04PrimarySurfaceRecord interaction = gUpt04PrimarySurfaces[index];
    if (!Upt04PrimarySurfaceRecordValid(interaction))
    {
        if (diagnosticPixel)
            gUpt46GlassDiagnostic[1] = 1u;
        return;
    }

    // The compact primary record's materialAndSurface.w is allowed to carry
    // only the broad surface class. Recover the authoritative packed subtype
    // from the accepted geometry identity before deciding that a valid-but-
    // non-glass feature record is opaque. This is the same fallback contract
    // used by legacy visibility and transmission.
    Upt04HitFacts interactionHit = Upt04MissFacts(0.0f);
    interactionHit.status = kUpt04HitTriangle;
    interactionHit.instanceId = interaction.instancePrimitiveObject.x;
    interactionHit.geometryId = 0u;
    interactionHit.primitiveId = interaction.instancePrimitiveObject.y;
    interactionHit.frontFace = 1u;
    uint resolvedInteractionClassAndFlags = 0u;
    const bool resolvedInteractionClassValid =
        Upt04ResolveTriangleClassAndFlags(
            interactionHit, resolvedInteractionClassAndFlags);
    const bool resolvedInteractionGlass =
        resolvedInteractionClassValid
        && Upt04TriangleClassIsTransmittingGlass(
            resolvedInteractionClassAndFlags);

    float ior;
    const bool featureGlass = Upt46FeatureGlass(
        interaction.materialAndSurface.y, ior);
    const bool objectFallback = (interaction.materialAndSurface.z
        & kUpt46MaterialObjectGlassFallback) != 0u;
    const bool portalFallback = (interaction.materialAndSurface.z
        & kUpt46MaterialPortalWindowFallback) != 0u;
    const bool packedInteractionGlass = Upt04TriangleClassIsTransmittingGlass(
        interaction.materialAndSurface.w);
    const bool glass = featureGlass || objectFallback || portalFallback
        || packedInteractionGlass || resolvedInteractionGlass;
    const float roughness = saturate(
        interaction.geometricNormalAndRoughness.w);
    if (diagnosticPixel)
    {
        gUpt46GlassDiagnostic[1] = 2u;
        gUpt46GlassDiagnostic[3] = interaction.materialAndSurface.y;
        gUpt46GlassDiagnostic[4] = interaction.materialAndSurface.z;
        gUpt46GlassDiagnostic[5] = interaction.materialAndSurface.w;
        gUpt46GlassDiagnostic[6] = resolvedInteractionClassAndFlags;
        gUpt46GlassDiagnostic[7] =
            (featureGlass ? 1u << 0u : 0u)
            | (objectFallback ? 1u << 1u : 0u)
            | (portalFallback ? 1u << 2u : 0u)
            | (packedInteractionGlass ? 1u << 3u : 0u)
            | (resolvedInteractionClassValid ? 1u << 4u : 0u)
            | (resolvedInteractionGlass ? 1u << 5u : 0u)
            | (glass ? 1u << 6u : 0u);
        gUpt46GlassDiagnostic[19] = asuint(
            interaction.worldPositionAndViewDepth.x);
        gUpt46GlassDiagnostic[20] = asuint(
            interaction.worldPositionAndViewDepth.y);
        gUpt46GlassDiagnostic[21] = asuint(
            interaction.worldPositionAndViewDepth.z);
        gUpt46GlassDiagnostic[22] = asuint(
            interaction.worldPositionAndViewDepth.w);
        gUpt46GlassDiagnostic[23] = asuint(roughness);
    }
    // UPT46 owns transmitting panes only. Opaque zero-roughness materials are
    // already represented by the main UPT path and its normal RR guides; the
    // former mirror admission replaced that result with a flat one-ray mini
    // renderer and made opaque mirrors regress whenever glass optics ran.
    if (!glass)
        return;

    const float3 shadingNormal = Upt46SafeNormalize(
        interaction.shadingNormalAndOpacity.xyz,
        interaction.geometricNormalAndRoughness.xyz);
    const float3 viewDirection = Upt46SafeNormalize(
        interaction.viewDirectionAndReserved.xyz, shadingNormal);
    const float3 faceNormal = dot(shadingNormal, viewDirection) >= 0.0f
        ? shadingNormal : -shadingNormal;
    const float3 incident = -viewDirection;
    const float cosine = saturate(abs(dot(faceNormal, viewDirection)));

    const float3 reflectedDirection = Upt46SafeNormalize(
        reflect(incident, faceNormal), faceNormal);
    Upt04TraceRequest reflectedRay;
    reflectedRay.origin = interaction.worldPositionAndViewDepth.xyz
        + faceNormal * 0.75f + reflectedDirection * 0.25f;
    reflectedRay.tMin = 0.01f;
    reflectedRay.direction = reflectedDirection;
    reflectedRay.tMax = max(optical.rayTMax, 1.0f);
    float3 reflectedPassthroughEmission;
    const Upt04HitFacts reflectedHit = Upt46TraceOptical(
        reflectedRay, false, reflectedPassthroughEmission);
    if (diagnosticPixel)
    {
        gUpt46GlassDiagnostic[1] = 3u;
        gUpt46GlassDiagnostic[14] = reflectedHit.status;
    }
    float3 reflectedDiffuseAlbedo;
    float3 reflectedSpecularF0;
    float3 reflectedTerminalEmission;
    const float3 reflectedRadiance = Upt46ShadeFlatReflectionHit(
        reflectedHit,
        reflectedDirection,
        reflectedDiffuseAlbedo,
        reflectedSpecularF0,
        reflectedTerminalEmission);
    if (reflectionDomainPixel)
    {
        Upt46PublishReflectionReceiver(
            reflectionIndex,
            interaction,
            reflectedHit,
            reflectedDirection,
            reflectedTerminalEmission);
    }

    float3 transmissionWeight;
    float3 reflectionWeight;
    Upt46BuildThinGlassWeights(
        interaction.materialAndSurface.y,
        cosine,
        transmissionWeight,
        reflectionWeight);
    if (diagnosticPixel)
    {
        gUpt46GlassDiagnostic[1] = 5u;
        gUpt46GlassDiagnostic[11] = asuint(transmissionWeight.x);
        gUpt46GlassDiagnostic[12] = asuint(transmissionWeight.y);
        gUpt46GlassDiagnostic[13] = asuint(transmissionWeight.z);
    }
    const float3 reflectionContribution = max(
        reflectedRadiance * reflectionWeight
            * max(optical.reflectionComposeBoost, 0.0f),
        float3(0.0f));
    Upt46PublishReflectionRrGuides(
        pixel,
        reflectedHit,
        reflectedDiffuseAlbedo,
        reflectedSpecularF0,
        reflectionContribution);
    gUpt46ReflectionOutput[pixel] = float4(
        max(reflectedRadiance * reflectionWeight, float3(0.0f)),
        reflectionReuse
            ? -max(reflectionWeight.x,
                max(reflectionWeight.y, reflectionWeight.z))
            : 0.875f);

    // P0 already continued through supported thin glass and R0 contains its
    // fully reused/indirect-shaded result.  Do not replace that result with a
    // fresh one-shot secondary shade: doing so drops sky/emissive transport,
    // material coverage and T0/S0 history behind the pane.  Until a dedicated
    // transmitted receiver/reservoir exists, G2 must preserve R0 and apply the
    // accepted pane transmission weight.  The refraction strength remains a
    // future optical-direction control; it is deliberately not allowed to
    // degrade the canonical straight-through transport here.
    const Upt04PrimarySurfaceRecord canonicalReceiver =
        Upt04UnpackPrimarySurface32(
            gUpt46CanonicalPrimaryReceivers[index],
            float3(
                gUpt04Control.primaryCameraOriginX,
                gUpt04Control.primaryCameraOriginY,
                gUpt04Control.primaryCameraOriginZ));
    const bool canonicalReceiverValid =
        Upt04PrimarySurfaceRecordValid(canonicalReceiver);
    if (diagnosticPixel)
    {
        gUpt46GlassDiagnostic[8] = canonicalReceiverValid ? 1u : 0u;
        gUpt46GlassDiagnostic[9] = canonicalReceiver.materialAndSurface.y;
        gUpt46GlassDiagnostic[10] = canonicalReceiver.materialAndSurface.w;
    }
    if (canonicalReceiverValid)
    {
        gUpt46TransmissionOutput[pixel] = float4(
            transmissionWeight, 1.0f);
        if (diagnosticPixel)
        {
            gUpt46GlassDiagnostic[1] = 6u;
            gUpt46GlassDiagnostic[18] = 1u;
        }
        return;
    }

    // Compact32 intentionally cannot encode an environment endpoint.  Only
    // for that sparse unrepresented case, continue the camera direction
    // through supported panes.  A miss supplies resolved sky radiance; a
    // triangle remains on canonical attenuation ownership so this pass never
    // replaces an ordinary surface with a direct-only mini shade.
    Upt04TraceRequest transmittedRay;
    transmittedRay.origin = interaction.worldPositionAndViewDepth.xyz
        + incident * 0.75f;
    transmittedRay.tMin = 0.01f;
    transmittedRay.direction = incident;
    transmittedRay.tMax = max(optical.rayTMax, 1.0f);
    float3 transmittedPassthroughEmission;
    const Upt04HitFacts transmittedHit = Upt46TraceOptical(
        transmittedRay, true, transmittedPassthroughEmission);
    if (diagnosticPixel)
    {
        gUpt46GlassDiagnostic[1] = 7u;
        gUpt46GlassDiagnostic[15] = transmittedHit.status;
    }
    if (transmittedHit.status == kUpt04HitMiss)
    {
        gUpt46TransmissionOutput[pixel] = float4(
            (Upt46Sky(incident) + transmittedPassthroughEmission)
                * transmissionWeight,
            2.0f);
        if (diagnosticPixel)
        {
            gUpt46GlassDiagnostic[1] = 8u;
            gUpt46GlassDiagnostic[18] = 2u;
        }
        return;
    }

    // Doom sky is represented by committed sky-shell geometry, not only by a
    // hardware miss. Compact32 cannot encode that terminal, so resolve it to
    // the environment cube here. Emissive terminals have the same compact32
    // ownership gap and are evaluated through the shared UPT material path.
    uint transmittedMaterialIndex;
    if (Upt04ResolveTriangleMaterialIndex(
            transmittedHit, transmittedMaterialIndex)
        && transmittedMaterialIndex < gUpt04Control.materialCount)
    {
        const uint transmittedFlags =
            Upt04LoadMaterialFlags(transmittedMaterialIndex);
        if (diagnosticPixel)
        {
            gUpt46GlassDiagnostic[16] = transmittedMaterialIndex;
            gUpt46GlassDiagnostic[17] = transmittedFlags;
        }
        if ((transmittedFlags & kUpt46MaterialSkyEnvironment) != 0u)
        {
            gUpt46TransmissionOutput[pixel] = float4(
                (Upt46Sky(incident) + transmittedPassthroughEmission)
                    * transmissionWeight,
                2.0f);
            if (diagnosticPixel)
            {
                gUpt46GlassDiagnostic[1] = 9u;
                gUpt46GlassDiagnostic[18] = 2u;
            }
            return;
        }
        if ((transmittedFlags & kUpt46MaterialEmissive) != 0u)
        {
            gUpt46TransmissionOutput[pixel] = float4(
                (Upt46ShadeTransmissionTerminal(
                    transmittedHit, incident)
                    + transmittedPassthroughEmission)
                    * transmissionWeight,
                2.0f);
            if (diagnosticPixel)
            {
                gUpt46GlassDiagnostic[1] = 10u;
                gUpt46GlassDiagnostic[18] = 2u;
            }
            return;
        }
    }
    gUpt46TransmissionOutput[pixel] = float4(
        transmissionWeight, 1.0f);
    if (diagnosticPixel)
    {
        gUpt46GlassDiagnostic[1] = 11u;
        gUpt46GlassDiagnostic[18] = 1u;
    }
}
