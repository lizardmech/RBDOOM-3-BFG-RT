// Exact field order of the existing PathTraceSmokeMaterial table.  It remains
// owned by the selected MaterialSet; proposal and reservoir modules never see it.
public struct Upt04SmokeMaterialRecord
{
    public float4 debugAlbedo;
    public float4 emissiveColor;
    public uint diffuseTextureIndex;
    public uint alphaTextureIndex;
    public uint normalTextureIndex;
    public uint specularTextureIndex;
    public uint emissiveTextureIndex;
    public float alphaCutoff;
    public uint flags;
    public uint textureWidth;
    public uint textureHeight;
    public uint alphaTextureWidth;
    public uint alphaTextureHeight;
    public uint normalTextureWidth;
    public uint normalTextureHeight;
    public uint specularTextureWidth;
    public uint specularTextureHeight;
    public uint emissiveTextureWidth;
    public uint emissiveTextureHeight;
    public uint padding0;
    public uint padding1;
    public uint padding2;
};

// UPT-only D0 sidecar. The authoritative 112-byte material table remains
// unchanged; the admitted material set needs only these full-precision values.
public struct Upt04CompactMaterialRecord
{
    public uint4 debugAlbedo;
    public uint4 emissiveColor;
    public uint4 textureIndicesAndFlags;
};

public Upt04CompactMaterialRecord Upt04PackCompactMaterial(
    Upt04SmokeMaterialRecord material)
{
    Upt04CompactMaterialRecord packed = (Upt04CompactMaterialRecord)0;
    // UPT shading consumes only the RGB components of these two colors. Keep
    // the accepted 48-byte record and use the otherwise-dead alpha lanes for
    // the alpha-test data required while traversing continuation candidates.
    packed.debugAlbedo = uint4(
        asuint(material.debugAlbedo.xyz),
        material.alphaTextureIndex);
    packed.emissiveColor = uint4(
        asuint(material.emissiveColor.xyz),
        asuint(material.alphaCutoff));
    packed.textureIndicesAndFlags = uint4(
        material.diffuseTextureIndex,
        material.specularTextureIndex,
        material.emissiveTextureIndex,
        material.flags);
    return packed;
}

public Upt04SmokeMaterialRecord Upt04UnpackCompactMaterial(
    Upt04CompactMaterialRecord packed)
{
    Upt04SmokeMaterialRecord material = (Upt04SmokeMaterialRecord)0;
    material.debugAlbedo = float4(asfloat(packed.debugAlbedo.xyz), 1.0f);
    material.emissiveColor = float4(asfloat(packed.emissiveColor.xyz), 1.0f);
    material.diffuseTextureIndex = packed.textureIndicesAndFlags.x;
    material.specularTextureIndex = packed.textureIndicesAndFlags.y;
    material.emissiveTextureIndex = packed.textureIndicesAndFlags.z;
    material.flags = packed.textureIndicesAndFlags.w;
    material.alphaTextureIndex = packed.debugAlbedo.w;
    material.alphaCutoff = asfloat(packed.emissiveColor.w);
    material.normalTextureIndex = 0xffffffffu;
    return material;
}
