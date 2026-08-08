public struct Upt08DuplicationControl
{
    public uint renderWidth;
    public uint renderHeight;
    public uint surfaceCount;
    public uint packedRowPitch;
};

public [[vk::binding(0, 0)]] StructuredBuffer<uint> gUpt08SampleIds;
public [[vk::binding(1, 0)]] RWStructuredBuffer<uint> gUpt08PackedDuplication;
public [[vk::push_constant]] ConstantBuffer<Upt08DuplicationControl> gUpt08Control;

groupshared uint gUpt08Tile[24u * 24u];
groupshared uint gUpt08Scores[64u];

[numthreads(8, 8, 1)]
void main(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint groupIndex : SV_GroupIndex)
{
    const int2 groupOrigin = int2(groupId.xy * 8u) - int2(8, 8);
    [loop]
    for (uint tileIndex = groupIndex; tileIndex < 24u * 24u; tileIndex += 64u)
    {
        const int2 tilePixel = groupOrigin
            + int2(int(tileIndex % 24u), int(tileIndex / 24u));
        uint sampleId = 0u;
        if (tilePixel.x >= 0 && tilePixel.y >= 0
            && uint(tilePixel.x) < gUpt08Control.renderWidth
            && uint(tilePixel.y) < gUpt08Control.renderHeight)
        {
            const uint sourceIndex = uint(tilePixel.y) * gUpt08Control.renderWidth
                + uint(tilePixel.x);
            if (sourceIndex < gUpt08Control.surfaceCount)
                sampleId = gUpt08SampleIds[sourceIndex];
        }
        gUpt08Tile[tileIndex] = sampleId;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint2 pixel = groupId.xy * 8u + groupThreadId.xy;
    uint duplicateCount = 0u;
    if (pixel.x < gUpt08Control.renderWidth
        && pixel.y < gUpt08Control.renderHeight)
    {
        const uint centerTile = (groupThreadId.y + 8u) * 24u
            + groupThreadId.x + 8u;
        const uint sampleId = gUpt08Tile[centerTile];
        if (sampleId != 0u)
        {
            [unroll]
            for (int dy = -8; dy <= 8; ++dy)
            {
                [unroll]
                for (int dx = -8; dx <= 8; ++dx)
                {
                    if (dx == 0 && dy == 0)
                        continue;
                    const uint neighborTile = uint(
                        int(centerTile) + dy * 24 + dx);
                    duplicateCount += gUpt08Tile[neighborTile] == sampleId
                        ? 1u : 0u;
                }
            }
        }
    }
    // UNORM8 of c=count/288. The quantized map is page-local and fully
    // overwritten, so it needs neither a wide sidecar nor a per-frame clear.
    gUpt08Scores[groupIndex] = min(
        (duplicateCount * 255u + 144u) / 288u, 255u);
    GroupMemoryBarrierWithGroupSync();

    if ((groupThreadId.x & 3u) == 0u
        && pixel.x < gUpt08Control.renderWidth
        && pixel.y < gUpt08Control.renderHeight)
    {
        const uint base = groupIndex;
        const uint packed = gUpt08Scores[base]
            | (gUpt08Scores[base + 1u] << 8u)
            | (gUpt08Scores[base + 2u] << 16u)
            | (gUpt08Scores[base + 3u] << 24u);
        gUpt08PackedDuplication[pixel.y * gUpt08Control.packedRowPitch
            + pixel.x / 4u] = packed;
    }
}
