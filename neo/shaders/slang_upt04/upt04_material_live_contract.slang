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
