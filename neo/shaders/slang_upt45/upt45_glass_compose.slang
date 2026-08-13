public struct Upt45ComposeControl
{
    public uint width;
    public uint height;
    public float overlayStrength;
    public uint reserved0;
};

[[vk::binding(0, 0)]] Texture2D<float4> gUpt45ResolvedColor;
[[vk::binding(1, 0)]] Texture2D<float4> gUpt45CompositionToken;
[[vk::binding(2, 0)]]
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> gUpt45ComposedColor;
[[vk::push_constant]] ConstantBuffer<Upt45ComposeControl> gUpt45ComposeControl;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gUpt45ComposeControl.width ||
        pixel.y >= gUpt45ComposeControl.height)
        return;
    const float4 source = gUpt45ResolvedColor.Load(int3(pixel, 0));
    const float4 token = gUpt45CompositionToken.Load(int3(pixel, 0));
    if (token.a < 0.5f)
    {
        gUpt45ComposedColor[pixel] = source;
        return;
    }
    const float overlay = saturate(gUpt45ComposeControl.overlayStrength);
    const float3 color = max(source.rgb, float3(0.0f)) * max(token.rgb, float3(0.0f)) +
        saturate(token.rgb) * overlay;
    gUpt45ComposedColor[pixel] = float4(color, source.a);
}
