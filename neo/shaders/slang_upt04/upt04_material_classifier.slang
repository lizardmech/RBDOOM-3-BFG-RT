import upt04_material_live_contract;

public static const uint kUpt04MaterialOverrideZeroRoughness = 0x00000001u;
public static const uint kUpt04MaterialClassifierDriveLegacySpec = 0x00000002u;
public static const uint kUpt04MaterialOverrideFullMetal = 0x00001000u;
public static const uint kUpt04MatClassRouteRealPbrRmao = 1u;
public static const uint kUpt04MatClassRouteLegacySpecGloss = 2u;
public static const uint kUpt04MatClassRouteSurfaceFallback = 3u;
public static const uint kUpt04MatClassSurfaceMetal = 1u;
public static const uint kUpt04MatClassSurfaceRicochet = 9u;
public static const uint kUpt04MatClassSurfaceSpecial = 10u;

public uint Upt04MatClassRoute(Upt04SmokeMaterialRecord material)
{
    return (material.padding1 >> 10u) & 0x0fu;
}

public uint Upt04MatClassSurfaceClass(Upt04SmokeMaterialRecord material)
{
    return material.padding1 & 0x0fu;
}

public float Upt04MatClassUnit8(uint packed, uint shift)
{
    return float((packed >> shift) & 0xffu) * (1.0f / 255.0f);
}

public float3 Upt04MatClassMetallicF0(
    Upt04SmokeMaterialRecord material,
    float3 baseColor)
{
    const float metallic = Upt04MatClassUnit8(material.padding2, 8u);
    const float f0 = Upt04MatClassUnit8(material.padding2, 24u);
    return lerp(float3(f0), max(saturate(baseColor), float3(f0)), metallic);
}

// Shared by D0 and every replay provider.  A reservoir may never change its
// material interpretation merely because it crossed the initial/reuse pass
// boundary.
public void Upt04ApplyMaterialClassifier(
    Upt04SmokeMaterialRecord material,
    inout float3 baseColor,
    float3 specularTexel,
    bool hasSpecularTexel,
    inout float3 specularF0,
    inout float roughness)
{
    const uint route = Upt04MatClassRoute(material);
    const uint surfaceClass = Upt04MatClassSurfaceClass(material);
    if (hasSpecularTexel && route == kUpt04MatClassRouteRealPbrRmao)
    {
        roughness = max(0.05f, saturate(specularTexel.x));
        const float metallic = saturate(specularTexel.y);
        specularF0 = lerp(float3(0.04f), saturate(baseColor), metallic);
        baseColor *= 1.0f - metallic;
    }
    else if ((material.padding0 & kUpt04MaterialClassifierDriveLegacySpec) != 0u
        && route == kUpt04MatClassRouteLegacySpecGloss
        && surfaceClass == kUpt04MatClassSurfaceRicochet)
    {
        specularF0 = max(
            specularF0,
            Upt04MatClassMetallicF0(material, baseColor));
    }
    else if (route == kUpt04MatClassRouteSurfaceFallback
        && surfaceClass != kUpt04MatClassSurfaceSpecial)
    {
        const bool incomingTexturedRoughness =
            material.specularTextureIndex != 0xffffffffu
            && roughness < 0.999f;
        if (!incomingTexturedRoughness)
            roughness = saturate(Upt04MatClassUnit8(material.padding2, 0u));
        const float metallic = saturate(
            Upt04MatClassUnit8(material.padding2, 8u));
        specularF0 = Upt04MatClassMetallicF0(material, baseColor);
        baseColor *= 1.0f - metallic;
    }

    const bool fullMetal =
        (material.padding0 & kUpt04MaterialOverrideFullMetal) != 0u;
    if (fullMetal)
    {
        const float peak = max(baseColor.x, max(baseColor.y, baseColor.z));
        const float3 chroma = peak > 1.0e-4f
            ? saturate(baseColor / peak) : float3(1.0f);
        specularF0 = max(
            specularF0,
            float3(0.85f) * lerp(float3(1.0f), chroma, 0.45f));
        baseColor = float3(0.0f);
    }
    if ((material.padding0 & kUpt04MaterialOverrideZeroRoughness) != 0u)
    {
        roughness = 0.0f;
        if (!fullMetal)
            specularF0 = max(specularF0, float3(0.85f));
    }
}
