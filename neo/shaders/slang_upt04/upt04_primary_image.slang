// Optional L1TEX path for W x H packed32 / sidecar pages. Two RGBA32UI
// layers are one 32-byte record. Hardware-tiled; pixel = (index % width,
// index / width). Bindings 42-44 sit immediately before PushConstants on
// produce/consume. Do not add these images to upt04_initial_bindings.slang.
import upt04_initial_control;
import upt04_initial_bindings;
import upt04_primary_surface_live_contract;
import upt04_primary_history_sidecar;
#if UPT04_PACKED_PRIMARY_RECEIVER == 1
import upt04_packed_primary_surface;
#elif UPT04_PACKED_PRIMARY_RECEIVER == 2
import upt04_packed_primary_surface32;
#endif

[[vk::binding(42, 0)]]
[[vk::image_format("rgba32ui")]]
Texture2DArray<uint4> gUpt04PrimaryTex;

[[vk::binding(43, 0)]]
[[vk::image_format("rgba32ui")]]
Texture2DArray<uint4> gUpt04PreviousPrimaryTex;

[[vk::binding(44, 0)]]
[[vk::image_format("rgba32ui")]]
Texture2DArray<uint4> gUpt04PreviousPrimarySidecarTex;

public bool Upt04PrimaryTextureEnabled()
{
    if ((gUpt04Control.indirectPolicyFlags & 0x80000000u) != 0u)
    {
        const uint4 keep = gUpt04PrimaryTex[uint3(0, 0, 0)]
            + gUpt04PreviousPrimaryTex[uint3(0, 0, 0)]
            + gUpt04PreviousPrimarySidecarTex[uint3(0, 0, 0)];
        return keep.x == 0xdeadbeefu;
    }
    return (gUpt04Control.indirectPolicyFlags & kUpt04PrimaryTexture) != 0u;
}

public uint2 Upt04PrimaryTexel(uint surfaceIndex)
{
    const uint width = max(gUpt04Control.renderWidth, 1u);
    return uint2(surfaceIndex % width, surfaceIndex / width);
}

#if UPT04_PACKED_PRIMARY_RECEIVER == 2
public Upt04PackedPrimarySurfaceRecord32 Upt04Packed32FromTexels(uint4 a, uint4 b)
{
    Upt04PackedPrimarySurfaceRecord32 packed;
    packed.geometry = a;
    packed.material = b;
    return packed;
}

public Upt04PrimaryHistorySidecar Upt04SidecarFromTexels(uint4 a, uint4 b)
{
    Upt04PrimaryHistorySidecar sidecar;
    sidecar.metadata = a;
    sidecar.previousPositionAndAlphaCutoff = asfloat(b);
    return sidecar;
}
#endif

public Upt04PrimarySurfaceRecord Upt04LoadPrimarySurfaceRecord(uint index)
{
#if UPT04_PACKED_PRIMARY_RECEIVER == 2
    if (Upt04PrimaryTextureEnabled())
    {
        const uint2 p = Upt04PrimaryTexel(index);
        return Upt04UnpackPrimarySurface32(
            Upt04Packed32FromTexels(
                gUpt04PrimaryTex[uint3(p, 0)],
                gUpt04PrimaryTex[uint3(p, 1)]),
            float3(
                gUpt04Control.primaryCameraOriginX,
                gUpt04Control.primaryCameraOriginY,
                gUpt04Control.primaryCameraOriginZ));
    }
#endif
    return Upt04LoadPrimarySurfaceRecordFromBuffer(index);
}

public Upt04PrimarySurfaceRecord Upt04LoadPreviousPrimarySurface(uint index)
{
#if UPT04_PACKED_PRIMARY_RECEIVER == 2
    if (Upt04PrimaryTextureEnabled())
    {
        const uint2 p = Upt04PrimaryTexel(index);
        return Upt04ApplyPrimaryHistorySidecar(
            Upt04UnpackPrimarySurface32(
                Upt04Packed32FromTexels(
                    gUpt04PreviousPrimaryTex[uint3(p, 0)],
                    gUpt04PreviousPrimaryTex[uint3(p, 1)]),
                float3(
                    gUpt04Control.previousCameraOriginX,
                    gUpt04Control.previousCameraOriginY,
                    gUpt04Control.previousCameraOriginZ)),
            Upt04SidecarFromTexels(
                gUpt04PreviousPrimarySidecarTex[uint3(p, 0)],
                gUpt04PreviousPrimarySidecarTex[uint3(p, 1)]));
    }
#endif
    return Upt04LoadPreviousPrimarySurfaceFromBuffer(index);
}
