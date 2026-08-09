import upt04_geometry_contract;
import upt04_geometry_decode;
import upt04_hit_contract;
import upt04_initial_bindings;
import upt04_initial_control;
import upt04_material_live_contract;

public static const uint kUpt04AlphaClipTest = 0x00000001u;
public static const uint kUpt04AlphaClipDiffuseYCoCg = 0x00000002u;
public static const uint kUpt04AlphaClipFromDiffuseLuma = 0x00000040u;
public static const uint kUpt04AlphaClipForceDebugAlbedo = 0x00000080u;
public static const uint kUpt04AlphaClipFromDiffuseDarkKey = 0x00000100u;
public static const uint kUpt04AlphaClipFromDiffuseMagentaKey = 0x00001000u;

public float3 Upt04AlphaClipYCoCgToRgb(float4 ycocg)
{
    ycocg.z = ycocg.z * 31.875f + 1.0f;
    ycocg.z = 1.0f / ycocg.z;
    ycocg.xy *= ycocg.z;
    return saturate(float3(
        dot(ycocg, float4(1.0f, -1.0f, 0.0f, 1.0f)),
        dot(ycocg, float4(0.0f, 1.0f, -0.50196078f, 1.0f)),
        dot(ycocg, float4(-1.0f, -1.0f, 1.00392156f, 1.0f))));
}

public bool Upt04AlphaClipSampleTexture(
    uint textureIndex,
    float2 texCoord,
    out float4 texel)
{
    texel = float4(1.0f);
    if (textureIndex == kUpt04InvalidIdentity
        || textureIndex >= gUpt04Control.logicalTextureCount
        || !all(isfinite(texCoord))
        || any(abs(texCoord) > 65536.0f))
        return false;
    texel = gUpt04MaterialTextures[NonUniformResourceIndex(textureIndex)]
        .SampleLevel(gUpt04MaterialSampler, frac(texCoord), 0.0f);
    return all(isfinite(texel)) && !any(abs(texel) > 65504.0f);
}

public bool Upt04AlphaClipDiffuseTexel(
    Upt04SmokeMaterialRecord material,
    float2 texCoord,
    out float4 texel)
{
    texel = material.debugAlbedo;
    if ((material.flags & kUpt04AlphaClipForceDebugAlbedo) != 0u)
        return true;
    if (!Upt04AlphaClipSampleTexture(
            material.diffuseTextureIndex, texCoord, texel))
        return material.diffuseTextureIndex == kUpt04InvalidIdentity;
    if ((gUpt04Control.availabilityFlags & kUpt04MaterialDecodeTextures) != 0u
        && (material.flags & kUpt04AlphaClipDiffuseYCoCg) != 0u)
        texel.xyz = Upt04AlphaClipYCoCgToRgb(texel);
    return true;
}

// Returns true only for a proven discarded texel. Missing route, material, or
// texture state remains solid so traversal cannot manufacture geometry holes.
public bool Upt04MaterialRejectsAlphaClipCandidate(Upt04HitFacts candidate)
{
    uint materialIndex;
    if (!Upt04ResolveTriangleMaterialIndex(candidate, materialIndex)
        || materialIndex >= gUpt04Control.materialCount
        || (Upt04LoadMaterialFlags(materialIndex)
            & kUpt04AlphaClipTest) == 0u)
        return false;

    Upt04ResolvedTriangle triangle;
    if (!Upt04ResolveTriangle(candidate, triangle)
        || triangle.materialIndex != materialIndex
        || triangle.materialIndex >= gUpt04Control.materialCount)
        return false;

    const Upt04SmokeMaterialRecord material =
        Upt04LoadMaterialRecord(triangle.materialIndex);
    if ((material.flags & kUpt04AlphaClipTest) == 0u
        || !isfinite(material.alphaCutoff))
        return false;

    const float3 barycentrics = float3(
        1.0f - candidate.barycentrics.x - candidate.barycentrics.y,
        candidate.barycentrics.x,
        candidate.barycentrics.y);
    if (!all(isfinite(barycentrics))
        || any(barycentrics < -1.0e-4f)
        || any(barycentrics > 1.0001f))
        return false;
    const float2 texCoord =
        triangle.vertex0.texCoord.xy * barycentrics.x
        + triangle.vertex1.texCoord.xy * barycentrics.y
        + triangle.vertex2.texCoord.xy * barycentrics.z;

    float4 diffuseTexel;
    const bool diffuseValid = Upt04AlphaClipDiffuseTexel(
        material, texCoord, diffuseTexel);
    float coverage = 1.0f;
    if ((material.flags & kUpt04AlphaClipFromDiffuseDarkKey) != 0u)
    {
        if (!diffuseValid)
            return false;
        coverage = 1.0f - max(
            max(diffuseTexel.x, diffuseTexel.y), diffuseTexel.z);
    }
    else if ((material.flags & kUpt04AlphaClipFromDiffuseLuma) != 0u)
    {
        if (!diffuseValid)
            return false;
        coverage = max(max(diffuseTexel.x, diffuseTexel.y), diffuseTexel.z);
    }
    else if ((material.flags & kUpt04AlphaClipFromDiffuseMagentaKey) != 0u)
    {
        if (!diffuseValid)
            return false;
        const float keyDistance = max(
            abs(diffuseTexel.x - 1.0f),
            max(abs(diffuseTexel.y), abs(diffuseTexel.z - 1.0f)));
        coverage = keyDistance <= 0.08f ? 0.0f : 1.0f;
    }
    else if (material.alphaTextureIndex != kUpt04InvalidIdentity)
    {
        float4 alphaTexel;
        if (!Upt04AlphaClipSampleTexture(
                material.alphaTextureIndex, texCoord, alphaTexel))
            return false;
        coverage = alphaTexel.w;
    }
    else
    {
        if (!diffuseValid)
            return false;
        coverage = diffuseTexel.w;
    }
    return isfinite(coverage)
        && saturate(coverage) < saturate(material.alphaCutoff);
}
