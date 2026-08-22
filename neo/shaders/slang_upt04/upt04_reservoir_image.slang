// Optional L1TEX path for the 64-byte reservoir page. Four RGBA32UI layers
// are one packed reservoir. Hardware-tiled; pixel = (index % width, index / width).
import upt03_reservoir_contract;
import upt04_initial_control;
import upt04_initial_bindings;

[[vk::binding(40, 0)]]
[[vk::image_format("rgba32ui")]]
RWTexture2DArray<uint4> gUpt04ReservoirTex;

[[vk::binding(41, 0)]]
[[vk::image_format("rgba32ui")]]
Texture2DArray<uint4> gUpt04PreviousReservoirTex;

public bool Upt04ReservoirTextureEnabled()
{
    return (gUpt04Control.indirectPolicyFlags & kUpt04ReservoirTexture) != 0u;
}

public uint2 Upt04ReservoirTexel(uint surfaceIndex)
{
    const uint width = max(gUpt04Control.renderWidth, 1u);
    return uint2(surfaceIndex % width, surfaceIndex / width);
}

public Upt03PackedReservoir Upt04PackedFromTexels(
    uint4 a, uint4 b, uint4 c, uint4 d)
{
    Upt03PackedReservoir packed;
    packed.word0 = a.x; packed.word1 = a.y; packed.word2 = a.z; packed.word3 = a.w;
    packed.word4 = b.x; packed.word5 = b.y; packed.word6 = b.z; packed.word7 = b.w;
    packed.word8 = c.x; packed.word9 = c.y; packed.word10 = c.z; packed.word11 = c.w;
    packed.word12 = d.x; packed.word13 = d.y; packed.word14 = d.z; packed.word15 = d.w;
    return packed;
}

public Upt03PackedReservoir Upt04LoadPackedReservoir(uint surfaceIndex)
{
    if (Upt04ReservoirTextureEnabled())
    {
        const uint2 p = Upt04ReservoirTexel(surfaceIndex);
        return Upt04PackedFromTexels(
            gUpt04ReservoirTex[uint3(p, 0)],
            gUpt04ReservoirTex[uint3(p, 1)],
            gUpt04ReservoirTex[uint3(p, 2)],
            gUpt04ReservoirTex[uint3(p, 3)]);
    }
    return gUpt04Reservoirs[Upt04SurfaceBufferIndex(surfaceIndex)];
}

public void Upt04StorePackedReservoir(
    uint surfaceIndex,
    Upt03PackedReservoir packed)
{
    if (Upt04ReservoirTextureEnabled())
    {
        const uint2 p = Upt04ReservoirTexel(surfaceIndex);
        gUpt04ReservoirTex[uint3(p, 0)] = uint4(
            packed.word0, packed.word1, packed.word2, packed.word3);
        gUpt04ReservoirTex[uint3(p, 1)] = uint4(
            packed.word4, packed.word5, packed.word6, packed.word7);
        gUpt04ReservoirTex[uint3(p, 2)] = uint4(
            packed.word8, packed.word9, packed.word10, packed.word11);
        gUpt04ReservoirTex[uint3(p, 3)] = uint4(
            packed.word12, packed.word13, packed.word14, packed.word15);
        return;
    }
    gUpt04Reservoirs[Upt04SurfaceBufferIndex(surfaceIndex)] = packed;
}

public Upt03PackedReservoir Upt04LoadPreviousPackedReservoir(uint surfaceIndex)
{
    if (Upt04ReservoirTextureEnabled())
    {
        const uint2 p = Upt04ReservoirTexel(surfaceIndex);
        return Upt04PackedFromTexels(
            gUpt04PreviousReservoirTex[uint3(p, 0)],
            gUpt04PreviousReservoirTex[uint3(p, 1)],
            gUpt04PreviousReservoirTex[uint3(p, 2)],
            gUpt04PreviousReservoirTex[uint3(p, 3)]);
    }
    return gUpt04PreviousReservoirs[Upt04SurfaceBufferIndex(surfaceIndex)];
}