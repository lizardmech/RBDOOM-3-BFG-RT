public struct Upt45GlassComposeControl
{
    public uint renderWidth;
    public uint renderHeight;
    public uint distortionEnabled;
    public uint sourceModeAndFrameIndex;
    public float reflectionBoost;
    public float transmissionFloor;
    public float distortionScale;
    public float distortionMaxPixels;
};

[[vk::binding(0, 0)]] Texture2D<float4> gUpt45Source;
[[vk::binding(1, 0)]] Texture2D<float4> gUpt45Transmission;
[[vk::binding(2, 0)]] Texture2D<float4> gUpt45Reflection;
[[vk::binding(3, 0)]] Texture2D<float4> gUpt45Distortion;
[[vk::binding(4, 0)]]
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> gUpt45Output;
[[vk::binding(5, 0)]] Texture2D<float4> gUpt45RrAlbedoSource;
[[vk::binding(6, 0)]] Texture2D<float4> gUpt45RrSpecularSource;
[[vk::binding(7, 0)]] Texture2D<float4> gUpt45RrNormalRoughnessSource;
[[vk::binding(8, 0)]] Texture2D<float4> gUpt45RrPositionSource;
[[vk::binding(9, 0)]] Texture2D<float> gUpt45RrDepthSource;
[[vk::binding(10, 0)]] Texture2D<float2> gUpt45RrMotionSource;
[[vk::binding(11, 0)]]
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> gUpt45RrAlbedoOutput;
[[vk::binding(12, 0)]]
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> gUpt45RrSpecularOutput;
[[vk::binding(13, 0)]]
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> gUpt45RrNormalRoughnessOutput;
[[vk::binding(14, 0)]]
[[vk::image_format("rgba32f")]]
RWTexture2D<float4> gUpt45RrPositionOutput;
[[vk::binding(15, 0)]]
[[vk::image_format("r32f")]]
RWTexture2D<float> gUpt45RrDepthOutput;
[[vk::binding(16, 0)]]
[[vk::image_format("rg16f")]]
RWTexture2D<float2> gUpt45RrMotionOutput;
[[vk::binding(17, 0)]] Texture2D<float4> gUpt45ReflectionReuse;
[[vk::push_constant]] ConstantBuffer<Upt45GlassComposeControl> gUpt45Control;

public int2 Upt45ClampPixel(int2 pixel)
{
    return clamp(
        pixel,
        int2(0, 0),
        int2(gUpt45Control.renderWidth, gUpt45Control.renderHeight) - 1);
}

public float4 Upt45LoadSourceBilinear(float2 pixel)
{
    const float2 baseFloat = floor(pixel);
    const int2 basePixel = int2(baseFloat);
    const float2 fraction = pixel - baseFloat;
    const float4 c00 = gUpt45Source.Load(int3(Upt45ClampPixel(basePixel), 0));
    const float4 c10 = gUpt45Source.Load(int3(Upt45ClampPixel(basePixel + int2(1, 0)), 0));
    const float4 c01 = gUpt45Source.Load(int3(Upt45ClampPixel(basePixel + int2(0, 1)), 0));
    const float4 c11 = gUpt45Source.Load(int3(Upt45ClampPixel(basePixel + int2(1, 1)), 0));
    return lerp(
        lerp(c00, c10, fraction.x),
        lerp(c01, c11, fraction.x),
        fraction.y);
}

public float4 Upt45LoadTransmissionBilinear(float2 pixel)
{
    const float2 baseFloat = floor(pixel);
    const int2 basePixel = int2(baseFloat);
    const float2 fraction = pixel - baseFloat;
    const float4 c00 = gUpt45Transmission.Load(int3(Upt45ClampPixel(basePixel), 0));
    const float4 c10 = gUpt45Transmission.Load(int3(Upt45ClampPixel(basePixel + int2(1, 0)), 0));
    const float4 c01 = gUpt45Transmission.Load(int3(Upt45ClampPixel(basePixel + int2(0, 1)), 0));
    const float4 c11 = gUpt45Transmission.Load(int3(Upt45ClampPixel(basePixel + int2(1, 1)), 0));
    return lerp(
        lerp(c00, c10, fraction.x),
        lerp(c01, c11, fraction.x),
        fraction.y);
}

public float4 Upt45LoadGuide4Bilinear(Texture2D<float4> texture, float2 pixel)
{
    const float2 baseFloat = floor(pixel);
    const int2 basePixel = int2(baseFloat);
    const float2 fraction = pixel - baseFloat;
    const float4 c00 = texture.Load(int3(Upt45ClampPixel(basePixel), 0));
    const float4 c10 = texture.Load(int3(Upt45ClampPixel(basePixel + int2(1, 0)), 0));
    const float4 c01 = texture.Load(int3(Upt45ClampPixel(basePixel + int2(0, 1)), 0));
    const float4 c11 = texture.Load(int3(Upt45ClampPixel(basePixel + int2(1, 1)), 0));
    return lerp(lerp(c00, c10, fraction.x), lerp(c01, c11, fraction.x), fraction.y);
}

public float Upt45LoadGuide1Bilinear(Texture2D<float> texture, float2 pixel)
{
    const float2 baseFloat = floor(pixel);
    const int2 basePixel = int2(baseFloat);
    const float2 fraction = pixel - baseFloat;
    const float c00 = texture.Load(int3(Upt45ClampPixel(basePixel), 0));
    const float c10 = texture.Load(int3(Upt45ClampPixel(basePixel + int2(1, 0)), 0));
    const float c01 = texture.Load(int3(Upt45ClampPixel(basePixel + int2(0, 1)), 0));
    const float c11 = texture.Load(int3(Upt45ClampPixel(basePixel + int2(1, 1)), 0));
    return lerp(lerp(c00, c10, fraction.x), lerp(c01, c11, fraction.x), fraction.y);
}

public float2 Upt45LoadGuide2Bilinear(Texture2D<float2> texture, float2 pixel)
{
    const float2 baseFloat = floor(pixel);
    const int2 basePixel = int2(baseFloat);
    const float2 fraction = pixel - baseFloat;
    const float2 c00 = texture.Load(int3(Upt45ClampPixel(basePixel), 0));
    const float2 c10 = texture.Load(int3(Upt45ClampPixel(basePixel + int2(1, 0)), 0));
    const float2 c01 = texture.Load(int3(Upt45ClampPixel(basePixel + int2(0, 1)), 0));
    const float2 c11 = texture.Load(int3(Upt45ClampPixel(basePixel + int2(1, 1)), 0));
    return lerp(lerp(c00, c10, fraction.x), lerp(c01, c11, fraction.x), fraction.y);
}

public float4 Upt45LoadReflectionReuseBilinear(uint2 fullResolutionPixel)
{
    uint width;
    uint height;
    gUpt45ReflectionReuse.GetDimensions(width, height);
    const float2 fullDimensions = float2(
        max(gUpt45Control.renderWidth, 1u),
        max(gUpt45Control.renderHeight, 1u));
    const float2 reuseScale = float2(
        max(width, 1u), max(height, 1u)) / fullDimensions;
    const float2 reflectionPixel =
        (float2(fullResolutionPixel) + 0.5f) * reuseScale - 0.5f;
    const float2 baseFloat = floor(reflectionPixel);
    const int2 basePixel = int2(baseFloat);
    const float2 fraction = reflectionPixel - baseFloat;
    const int2 maximum = int2(max(width, 1u), max(height, 1u)) - 1;
    const int2 p00 = clamp(basePixel, int2(0), maximum);
    const int2 p10 = clamp(basePixel + int2(1, 0), int2(0), maximum);
    const int2 p01 = clamp(basePixel + int2(0, 1), int2(0), maximum);
    const int2 p11 = clamp(basePixel + int2(1, 1), int2(0), maximum);
    const float4 c00 = gUpt45ReflectionReuse.Load(int3(p00, 0));
    const float4 c10 = gUpt45ReflectionReuse.Load(int3(p10, 0));
    const float4 c01 = gUpt45ReflectionReuse.Load(int3(p01, 0));
    const float4 c11 = gUpt45ReflectionReuse.Load(int3(p11, 0));
    return lerp(
        lerp(c00, c10, fraction.x),
        lerp(c01, c11, fraction.x),
        fraction.y);
}

public uint Upt45DeclusterHash(uint2 pixel, uint frameIndex)
{
    uint value = pixel.x * 0x8da6b343u;
    value ^= pixel.y * 0xd8163841u;
    value ^= frameIndex * 0xcb1ab31fu;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

public bool Upt45DeclusterLess(
    uint centerHash,
    uint centerIndex,
    uint neighborHash,
    uint neighborIndex)
{
    return centerHash < neighborHash ||
        (centerHash == neighborHash && centerIndex < neighborIndex);
}

public bool Upt45AdmitReusedReflection(uint2 pixel, uint mode, uint frameIndex)
{
    if (mode == 0u)
        return true;

    const uint width = gUpt45Control.renderWidth;
    const uint height = gUpt45Control.renderHeight;
    const uint centerIndex = pixel.y * width + pixel.x;
    const uint centerHash = Upt45DeclusterHash(pixel, frameIndex);
    const int2 offsets[8] = {
        int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
        int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1)
    };
    const uint neighborCount = mode == 1u ? 8u : 4u;
    [unroll]
    for (uint neighborOrdinal = 0u; neighborOrdinal < 8u; ++neighborOrdinal)
    {
        if (neighborOrdinal >= neighborCount)
            break;
        const int2 neighborSigned = int2(pixel) + offsets[neighborOrdinal];
        if (neighborSigned.x < 0 || neighborSigned.y < 0 ||
            neighborSigned.x >= int(width) || neighborSigned.y >= int(height))
            continue;
        const uint2 neighbor = uint2(neighborSigned);
        const uint neighborIndex = neighbor.y * width + neighbor.x;
        const uint neighborHash = Upt45DeclusterHash(neighbor, frameIndex);
        if (!Upt45DeclusterLess(
                centerHash, centerIndex, neighborHash, neighborIndex))
            return false;
    }
    return true;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt45Control.renderWidth ||
        pixel.y >= gUpt45Control.renderHeight)
        return;

    const float4 source = gUpt45Source.Load(int3(pixel, 0));
    const float4 transmission = gUpt45Transmission.Load(int3(pixel, 0));
    // Resolved and reflection-owned encodings are both above 0.5. UPT-46
    // producer ownership forbids reflection-owned output, but accepting the
    // historical range keeps this first cutover mechanically compatible.
    if (!(transmission.a > 0.5f))
    {
        gUpt45Output[pixel] = source;
        return;
    }

    // UPT-46 transport encoding:
    //   1 = straight-through UPT source plus attenuation
    //   2 = resolved refracted radiance (replace the source)
    //   3 = exact-mirror marker (replace from reflection)
    // The exact legacy view-16 producer packs attenuation in RGB and
    // reflection energy in alpha 1..2. Native UPT46 instead uses alpha 2 as a
    // resolved-radiance replacement and alpha 3 as an exact-mirror marker.
    // Keep the diagnostic source explicit so legacy energy can be compared
    // without accidentally decoding a bright legacy pane as UPT refraction.
    const bool legacySidecarEncoding =
        (gUpt45Control.sourceModeAndFrameIndex & 1u) != 0u;
    const uint reflectionDeclusterMode =
        (gUpt45Control.sourceModeAndFrameIndex >> 1u) & 3u;
    const uint frameSampleIndex =
        (gUpt45Control.sourceModeAndFrameIndex >> 3u) & 0x01ffffffu;
    const uint resolveView =
        (gUpt45Control.sourceModeAndFrameIndex >> 28u) & 0x0fu;
    // Views 10 and 11 isolate the already-reused reservoir domains. Preserve
    // pane attenuation and screen-space distortion, but do not inject native
    // UPT-46 radiance that has no direct/global reservoir classification.
    // View 12 starts from a black UPT-05 source and deliberately retains those
    // late optical sidecars as their own complementary diagnostic.
    const bool reservoirComponentView = resolveView == 10u || resolveView == 11u;
    const bool nativeOpticalReplacement = !legacySidecarEncoding &&
        transmission.a >= 1.5f;
    const bool resolvedRefraction = !reservoirComponentView &&
        nativeOpticalReplacement &&
        transmission.a >= 1.5f && transmission.a < 2.5f;
    const bool exactMirror = !reservoirComponentView &&
        !legacySidecarEncoding && transmission.a >= 2.5f;
    float4 base = resolvedRefraction
        ? float4(max(transmission.rgb, float3(0.0f)), source.a)
        : source;
    float2 rrGuideOffset = float2(0.0f);
    float rrGuideDistortionWeight = 0.0f;
    if (gUpt45Control.distortionEnabled != 0u)
    {
        const float4 distortion = gUpt45Distortion.Load(int3(pixel, 0));
        if (distortion.a >= 0.5f && distortion.z > 0.0f &&
            all(isfinite(distortion)))
        {
            const float maxOffset = max(
                gUpt45Control.distortionMaxPixels *
                    gUpt45Control.distortionScale,
                0.0f);
            const float2 offset = clamp(
                distortion.xy * gUpt45Control.distortionScale,
                float2(-maxOffset),
                float2(maxOffset));
            rrGuideOffset = offset;
            rrGuideDistortionWeight = saturate(distortion.z);
            if (resolvedRefraction)
            {
                // Sky/emissive terminals live in the transmission image, not
                // in the canonical screen source. Distort the resolved
                // optical image itself so the pane cannot turn black again.
                const float4 distortedTransmission =
                    Upt45LoadTransmissionBilinear(float2(pixel) + offset);
                if (distortedTransmission.a >= 1.5f
                    && all(isfinite(distortedTransmission)))
                    base.rgb = lerp(
                        base.rgb,
                        max(distortedTransmission.rgb, float3(0.0f)),
                        saturate(distortion.z));
            }
            else
            {
                const float4 distorted =
                    Upt45LoadSourceBilinear(float2(pixel) + offset);
                base = lerp(base, distorted, saturate(distortion.z));
            }
        }
    }

    // Native modes 2/3 store replacement radiance, not transmittance. A
    // component view cannot multiply by that unclassified radiance without
    // corrupting the split, so retain the masked reservoir source and leave
    // the complete replacement to view 12.
    const float3 throughput = reservoirComponentView && nativeOpticalReplacement
        ? float3(1.0f)
        : max(
            max(transmission.rgb, float3(0.0f)),
            float3(max(gUpt45Control.transmissionFloor, 0.0f)));
    float3 composed = resolvedRefraction
        ? base.rgb
        : base.rgb * throughput;
    const float4 reflection = gUpt45Reflection.Load(int3(pixel, 0));
    float3 reflectionSpecularSignal = float3(0.0f);
    bool reflectionSpecularSignalValid = false;
    if (!reservoirComponentView && exactMirror && reflection.a >= 0.95f &&
        all(isfinite(reflection.rgb)))
    {
        composed = max(reflection.rgb, float3(0.0f));
        reflectionSpecularSignal = composed;
        reflectionSpecularSignalValid = true;
    }
    else if (!reservoirComponentView && reflection.a < 0.0f &&
        all(isfinite(reflection.rgb)))
    {
        const float4 reusedReflection =
            Upt45LoadReflectionReuseBilinear(pixel);
        const bool reusedReflectionValid = reusedReflection.a > 0.5f
            && all(isfinite(reusedReflection.rgb))
            && Upt45AdmitReusedReflection(
                pixel, reflectionDeclusterMode, frameSampleIndex);
        const float reflectionBoost =
            max(gUpt45Control.reflectionBoost, 0.0f);
        const float3 resolvedReflection = reusedReflectionValid
            ? max(reusedReflection.rgb, float3(0.0f))
            : max(reflection.rgb, float3(0.0f));
        const float3 reflectionContribution = reusedReflectionValid
            ? resolvedReflection * (-reflection.a) * reflectionBoost
            : resolvedReflection * reflectionBoost;
        composed += reflectionContribution;
        // Beauty remains physically weighted by the pane. The practical RR
        // classifier input must instead expose the resolved reflection domain:
        // feeding only Fresnel-weighted radiance made ordinary 4--10% glass
        // reflections disappear from SpecularAlbedo while only extreme HDR
        // samples survived. Preserve the old exact contribution on a failed
        // child resolve. The normal mode publishes a valid private R0's full
        // reflected signal. De-clustered presentation instead publishes the
        // exact admitted beauty contribution so color and guide sparsity match.
        reflectionSpecularSignal = reflectionDeclusterMode != 0u
            ? reflectionContribution
            : (reusedReflectionValid
                ? resolvedReflection * reflectionBoost
                : reflectionContribution);
        reflectionSpecularSignalValid = true;
    }
    else if (!reservoirComponentView &&
        reflection.a >= 0.75f && reflection.a < 0.95f &&
        all(isfinite(reflection.rgb)))
    {
        const float3 reflectionContribution =
            max(reflection.rgb, float3(0.0f)) *
            max(gUpt45Control.reflectionBoost, 0.0f);
        composed += reflectionContribution;
        reflectionSpecularSignal = reflectionContribution;
        reflectionSpecularSignalValid = true;
    }

    // Beauty distortion changes which behind-glass receiver a screen pixel
    // represents. DLSS RR must receive the same shifted material, geometry and
    // motion tuple; leaving normal/depth/motion at the undistorted pixel makes
    // the color and guides describe different surfaces. Source and destination
    // textures are intentionally distinct, so this cannot diffuse through
    // neighboring pixels during the dispatch.
    if (!exactMirror && rrGuideDistortionWeight > 0.0f)
    {
        const float2 guidePixel = float2(pixel) + rrGuideOffset;
        const float4 albedo = Upt45LoadGuide4Bilinear(
            gUpt45RrAlbedoSource, guidePixel);
        const float4 specular = Upt45LoadGuide4Bilinear(
            gUpt45RrSpecularSource, guidePixel);
        float4 normalRoughness = Upt45LoadGuide4Bilinear(
            gUpt45RrNormalRoughnessSource, guidePixel);
        const float normalLengthSquared = dot(
            normalRoughness.xyz, normalRoughness.xyz);
        if (all(isfinite(normalRoughness)) && normalLengthSquared > 1.0e-12f)
            normalRoughness.xyz *= rsqrt(normalLengthSquared);
        const float4 position = Upt45LoadGuide4Bilinear(
            gUpt45RrPositionSource, guidePixel);
        const float depth = Upt45LoadGuide1Bilinear(
            gUpt45RrDepthSource, guidePixel);
        const float2 motion = Upt45LoadGuide2Bilinear(
            gUpt45RrMotionSource, guidePixel);
        if (all(isfinite(albedo)))
            gUpt45RrAlbedoOutput[pixel] = albedo;
        if (all(isfinite(specular)))
            gUpt45RrSpecularOutput[pixel] = specular;
        if (all(isfinite(normalRoughness)))
            gUpt45RrNormalRoughnessOutput[pixel] = normalRoughness;
        if (all(isfinite(position)))
            gUpt45RrPositionOutput[pixel] = position;
        if (isfinite(depth))
            gUpt45RrDepthOutput[pixel] = depth;
        if (all(isfinite(motion)))
            gUpt45RrMotionOutput[pixel] = motion;
    }
    if (reflectionSpecularSignalValid)
    {
        // This write must be last: the distortion-consistent guide copy above
        // otherwise replaces it with the transmitted receiver's material F0.
        // RR uses one combined noisy-color input and this guide to classify
        // that exact beauty contribution as specular reconstruction work.
        gUpt45RrSpecularOutput[pixel] = float4(
            max(reflectionSpecularSignal, float3(0.0f)), 1.0f);
    }
    gUpt45Output[pixel] = float4(max(composed, float3(0.0f)), base.a);
}
