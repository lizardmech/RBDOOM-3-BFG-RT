#include "global_inc.hlsl"

struct ParticleCompositeConstants
{
    float4 cameraOriginAndTanX;
    float4 cameraForwardAndTanY;
    float4 cameraLeftAndAmbient;
    float4 cameraUpAndEmissiveScale;
    float4 outputAndRenderSize;
    float4 batchInfo;
    float4 modelInfo;
};

#ifdef SPIRV
[[vk::push_constant]] ConstantBuffer<ParticleCompositeConstants> ParticleConstants;
#else
ConstantBuffer<ParticleCompositeConstants> ParticleConstants : register(b0);
#endif

Texture2D ParticleTexture : register(t1);
Texture2D<float4> GuidePositionTexture : register(t2);
SamplerState ParticleSampler : register(s0);

struct PS_IN
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
    float4 color : COLOR0;
    float viewDepth : TEXCOORD1;
};

float4 main(PS_IN input) : SV_Target0
{
    float softFade = 1.0;
    const uint depthPolicy = uint(ParticleConstants.batchInfo.x + 0.5);
    const uint blendClass = uint(ParticleConstants.batchInfo.y + 0.5);
    const bool debugTint = ParticleConstants.batchInfo.w > 0.5;
    // Diagnostic mode must expose captured geometry independently of the
    // manual guide-depth policy. Otherwise a missing card cannot be separated
    // from an over-aggressive occlusion rejection.
    if (!debugTint)
    {
        const float2 outputSize = max(ParticleConstants.outputAndRenderSize.xy, float2(1.0, 1.0));
        const float2 renderSize = max(ParticleConstants.outputAndRenderSize.zw, float2(1.0, 1.0));
        const int2 guidePixel = int2(clamp(input.position.xy * renderSize / outputSize, float2(0.0, 0.0), renderSize - float2(1.0, 1.0)));
        const float4 guidePosition = GuidePositionTexture.Load(int3(guidePixel, 0));
        if (guidePosition.w > 0.0)
        {
            const float guideDepth = dot(
                guidePosition.xyz - ParticleConstants.cameraOriginAndTanX.xyz,
                ParticleConstants.cameraForwardAndTanY.xyz);
            const float depthGap = guideDepth - input.viewDepth;
            if (depthPolicy == 2u)
            {
                const float zNear = max(ParticleConstants.modelInfo.y, 1.0e-4);
                const float particleProjectionDepth =
                    0.999 - zNear / max(input.viewDepth, zNear) -
                    ParticleConstants.modelInfo.x / max(input.viewDepth, zNear);
                const float guideProjectionDepth = 0.999 - zNear / max(guideDepth, zNear);
                if (particleProjectionDepth > guideProjectionDepth + 1.0e-4)
                {
                    discard;
                }
            }
            else
            {
                const bool weaponProjection = depthPolicy == 1u;
                const bool coverageBlend = blendClass == 0u || blendClass == 3u;
                // Additive projectile cards commonly surround a small rigid
                // core. Permit world-space cards to sit slightly behind that
                // core while retaining ordinary world occlusion at larger
                // separations. First-person weapon cards need a tight compare
                // against the weapon guide surface so a muzzle card behind the
                // barrel cannot paint over the already-resolved gun.
                const float occlusionTolerance = weaponProjection
                    ? 0.02
                    : (coverageBlend ? 0.02 : max(ParticleConstants.batchInfo.z, 0.02));
                if (depthGap < -occlusionTolerance)
                {
                    discard;
                }
                // Only smoke-style alpha cards use soft intersections. Emissive
                // projectile shells commonly coincide with their rigid core;
                // fading every blend class here erases the visible projectile.
                // Attached weapon cards are ordered against the gun, but do not
                // fade merely because the barrel is close behind them.
                if (coverageBlend && !weaponProjection)
                {
                    softFade = saturate(depthGap / max(ParticleConstants.batchInfo.z, 1.0e-3));
                }
            }
        }
    }

    // Particle RGB is authored in sRGB space, but particle opacity/fade values
    // are linear coverage. Converting alpha through the sRGB curve makes smoke
    // disappear long before its authored fade completes.
    const float4 sampledTexel = ParticleTexture.Sample(ParticleSampler, input.texCoord);
    const float4 authoredTexel = sampledTexel * input.color;
    // Additive and multiplicative particle vertex colors are intensity/fade
    // controls. Applying the sRGB curve after multiplying them makes a 0.25
    // smoke tint contribute only about 0.05 and erases most of its lifetime.
    // Decode the authored texture first, then apply that linear control.
    const bool rgbCoverageBlend = blendClass == 2u || blendClass == 3u;
    const float3 linearRgb = rgbCoverageBlend
        ? sRGBToLinearRGB(sampledTexel.rgb) * input.color.rgb
        : sRGBToLinearRGB(authoredTexel.rgb);
    float4 texel = float4(linearRgb, authoredTexel.a);
    if (blendClass != 2u)
    {
        if (blendClass == 3u)
        {
            texel.rgb *= softFade * ParticleConstants.modelInfo.z;
        }
        else
        {
            texel.a *= softFade * ParticleConstants.modelInfo.z;
        }
    }
    const bool emptyAlpha = blendClass < 2u && texel.a <= (1.0 / 255.0);
    const bool emptyRgbCoverage = blendClass >= 2u &&
        max(max(abs(texel.r), abs(texel.g)), abs(texel.b)) <= (1.0 / 255.0);
    if (!debugTint && (emptyAlpha || emptyRgbCoverage))
    {
        discard;
    }

    float3 rgb;
    if (blendClass == 0u)
    {
        rgb = texel.rgb * ParticleConstants.cameraLeftAndAmbient.w;
    }
    else if (blendClass == 1u)
    {
        rgb = texel.rgb * ParticleConstants.cameraUpAndEmissiveScale.w;
    }
    else
    {
        rgb = blendClass == 2u
            ? texel.rgb * ParticleConstants.cameraUpAndEmissiveScale.w
            : texel.rgb;
    }
    if (debugTint)
    {
        rgb = float3(1.0, 0.0, 1.0);
    }
    return float4(rgb, debugTint ? 1.0 : (blendClass >= 2u ? 0.0 : texel.a));
}
