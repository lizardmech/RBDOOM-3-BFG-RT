// UPT is Vulkan-only. Keep binary16 as a packed storage encoding without
// introducing native 16-bit SPIR-V arithmetic types: the device contract does
// not enable shaderInt16 or shaderFloat16, and neither is required by the
// GLSL.std.450 pack/unpack instructions.

[ForceInline]
public uint Upt04PackBinary16Pair(float2 value)
{
    return packHalf2x16(value);
}

[ForceInline]
public float2 Upt04UnpackBinary16Pair(uint packed)
{
    return spirv_asm
    {
        result:$$vector<float,2> = OpExtInst glsl450 UnpackHalf2x16 $packed
    };
}
