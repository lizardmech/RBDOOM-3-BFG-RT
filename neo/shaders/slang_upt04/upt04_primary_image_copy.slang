// Host-side copy of a 32-byte StructuredBuffer page (two uint4 words per
// pixel) into a 2-layer RGBA32UI Texture2DArray. Produce/consume/resolve
// then fetch through L1TEX. Separate layout from UPT-04 estimator.
struct Upt04PrimaryImageCopyControl
{
    uint renderWidth;
    uint renderHeight;
    uint reserved0;
    uint reserved1;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint4> gUpt04PrimaryCopySrc;
[[vk::binding(1, 0)]]
[[vk::image_format("rgba32ui")]]
RWTexture2DArray<uint4> gUpt04PrimaryCopyDst;
[[vk::push_constant]] Upt04PrimaryImageCopyControl gUpt04PrimaryCopyControl;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gUpt04PrimaryCopyControl.renderWidth
        || dispatchThreadId.y >= gUpt04PrimaryCopyControl.renderHeight)
        return;
    const uint index =
        dispatchThreadId.y * gUpt04PrimaryCopyControl.renderWidth
        + dispatchThreadId.x;
    const uint wordBase = index * 2u;
    gUpt04PrimaryCopyDst[uint3(dispatchThreadId.xy, 0)] =
        gUpt04PrimaryCopySrc[wordBase + 0u];
    gUpt04PrimaryCopyDst[uint3(dispatchThreadId.xy, 1)] =
        gUpt04PrimaryCopySrc[wordBase + 1u];
}
