#ifndef RB_PATH_TRACE_MATERIAL_FEATURE_HLSLI
#define RB_PATH_TRACE_MATERIAL_FEATURE_HLSLI

// Shared material feature contract for PT shader consumers.
//
// This layer deliberately derives features from the current rbdoom surface and
// material fields. It does not implement glass or liquid behavior; unsupported
// kinds fail closed until a later task opens their pass contract.

static const uint RT_PATH_TRACE_MATERIAL_KIND_UNKNOWN = 0u;
static const uint RT_PATH_TRACE_MATERIAL_KIND_OPAQUE = 1u;
static const uint RT_PATH_TRACE_MATERIAL_KIND_ALPHA_TESTED = 2u;
static const uint RT_PATH_TRACE_MATERIAL_KIND_DECAL_MODIFIER = 3u;
static const uint RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER = 4u;
static const uint RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS = 5u;
static const uint RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_PARTICLE = 6u;
static const uint RT_PATH_TRACE_MATERIAL_KIND_GUI_SCREEN = 7u;
static const uint RT_PATH_TRACE_MATERIAL_KIND_EMISSIVE_SPECIAL = 8u;
static const uint RT_PATH_TRACE_MATERIAL_KIND_SKY_OR_ENVIRONMENT = 9u;

static const uint RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_DIRECT = 0x00000001u;
static const uint RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_GI = 0x00000002u;
static const uint RT_PATH_TRACE_MATERIAL_CAP_PATH_DIFFUSE = 0x00000004u;
static const uint RT_PATH_TRACE_MATERIAL_CAP_PATH_SPECULAR_REFLECTION = 0x00000008u;
static const uint RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION = 0x00000010u;
static const uint RT_PATH_TRACE_MATERIAL_CAP_SHADOW_OCCLUSION = 0x00000020u;
static const uint RT_PATH_TRACE_MATERIAL_CAP_VISIBILITY_RAY_ALPHA_TEST = 0x00000040u;
static const uint RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER = 0x00000080u;
static const uint RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND = 0x00000100u;
static const uint RT_PATH_TRACE_MATERIAL_CAP_RR_DIFFUSE_GUIDE = 0x00000200u;
static const uint RT_PATH_TRACE_MATERIAL_CAP_RR_SPECULAR_GUIDE = 0x00000400u;
static const uint RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED = 0x80000000u;

static const uint RT_PATH_TRACE_MATERIAL_LOBE_DIFFUSE_REFLECTION = 0x00000001u;
static const uint RT_PATH_TRACE_MATERIAL_LOBE_SPECULAR_REFLECTION = 0x00000002u;
static const uint RT_PATH_TRACE_MATERIAL_LOBE_GLOSSY_REFLECTION = 0x00000004u;
static const uint RT_PATH_TRACE_MATERIAL_LOBE_SPECULAR_TRANSMISSION = 0x00000008u;
static const uint RT_PATH_TRACE_MATERIAL_LOBE_DIFFUSE_TRANSMISSION = 0x00000010u;
static const uint RT_PATH_TRACE_MATERIAL_LOBE_EMISSIVE = 0x00000020u;
static const uint RT_PATH_TRACE_MATERIAL_LOBE_ABSORB = 0x00000040u;

static const uint RT_PATH_TRACE_MATERIAL_MODIFIER_NONE = 0u;
static const uint RT_PATH_TRACE_MATERIAL_MODIFIER_OVER = 1u;
static const uint RT_PATH_TRACE_MATERIAL_MODIFIER_MODULATE_FILTER = 2u;
static const uint RT_PATH_TRACE_MATERIAL_MODIFIER_ADDITIVE_EMISSIVE = 3u;
static const uint RT_PATH_TRACE_MATERIAL_MODIFIER_DIFFUSE_LIT = 4u;
static const uint RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION = 5u;

static const uint RT_PATH_TRACE_MATERIAL_PASS_PRIMARY_SURFACE = 0x00000001u;
static const uint RT_PATH_TRACE_MATERIAL_PASS_PATH_INTEGRATOR = 0x00000002u;
static const uint RT_PATH_TRACE_MATERIAL_PASS_DIRECT_RESERVOIR = 0x00000004u;
static const uint RT_PATH_TRACE_MATERIAL_PASS_GI_RESERVOIR = 0x00000008u;
static const uint RT_PATH_TRACE_MATERIAL_PASS_REFLECTION_PRODUCER = 0x00000010u;
static const uint RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER = 0x00000020u;
static const uint RT_PATH_TRACE_MATERIAL_PASS_RR_GUIDE_EXPORT = 0x00000040u;
static const uint RT_PATH_TRACE_MATERIAL_PASS_DEBUG_VISUALIZER = 0x00000080u;

static const uint RT_PATH_TRACE_FEATURE_SURFACE_CLASS_TRANSLUCENT = 3u;
static const uint RT_PATH_TRACE_FEATURE_TRANSLUCENT_SUBTYPE_SHIFT = 24u;
static const uint RT_PATH_TRACE_FEATURE_TRANSLUCENT_SUBTYPE_MASK = 0x0f000000u;
static const uint RT_PATH_TRACE_FEATURE_TRANSLUCENT_SUBTYPE_OBJECT_GLASS = 1u;
static const uint RT_PATH_TRACE_FEATURE_TRANSLUCENT_SUBTYPE_SMOKE_PARTICLE = 2u;
static const uint RT_PATH_TRACE_FEATURE_TRANSLUCENT_SUBTYPE_PORTAL_WINDOW = 4u;
static const uint RT_PATH_TRACE_FEATURE_TRANSLUCENT_SUBTYPE_GUI_SCREEN = 5u;

static const uint RT_PATH_TRACE_FEATURE_MATERIAL_ALPHA_TEST = 0x00000001u;
static const uint RT_PATH_TRACE_FEATURE_MATERIAL_ADDITIVE_DECAL = 0x00000004u;
static const uint RT_PATH_TRACE_FEATURE_MATERIAL_EMISSIVE = 0x00000008u;
static const uint RT_PATH_TRACE_FEATURE_MATERIAL_FILTER_DECAL = 0x00000010u;
static const uint RT_PATH_TRACE_FEATURE_MATERIAL_PORTAL_WINDOW_FALLBACK = 0x00000200u;
static const uint RT_PATH_TRACE_FEATURE_MATERIAL_OBJECT_GLASS_FALLBACK = 0x00000400u;
static const uint RT_PATH_TRACE_FEATURE_MATERIAL_DETAIL_DECAL = 0x00002000u;
static const uint RT_PATH_TRACE_FEATURE_MATERIAL_DETAIL_DECAL_DIFFUSE_LIT = 0x00008000u;

struct PathTraceMaterialFeature
{
    uint materialKind;
    uint materialCaps;
    uint lobeCaps;
    uint passSupport;
    uint modifierKind;
};

uint PathTraceMaterialFeatureTranslucentSubtype(RAB_Surface surface)
{
    return (surface.flags & RT_PATH_TRACE_FEATURE_TRANSLUCENT_SUBTYPE_MASK) >> RT_PATH_TRACE_FEATURE_TRANSLUCENT_SUBTYPE_SHIFT;
}

bool PathTraceMaterialFeatureIsTranslucent(RAB_Surface surface)
{
    return surface.surfaceClass == RT_PATH_TRACE_FEATURE_SURFACE_CLASS_TRANSLUCENT;
}

bool PathTraceMaterialFeatureIsGlassLike(RAB_Surface surface)
{
    const uint subtype = PathTraceMaterialFeatureTranslucentSubtype(surface);
    const bool translucentGlass =
        PathTraceMaterialFeatureIsTranslucent(surface) &&
        (subtype == RT_PATH_TRACE_FEATURE_TRANSLUCENT_SUBTYPE_OBJECT_GLASS ||
         subtype == RT_PATH_TRACE_FEATURE_TRANSLUCENT_SUBTYPE_PORTAL_WINDOW);
    const bool fallbackGlass =
        (surface.material.flags &
            (RT_PATH_TRACE_FEATURE_MATERIAL_OBJECT_GLASS_FALLBACK |
             RT_PATH_TRACE_FEATURE_MATERIAL_PORTAL_WINDOW_FALLBACK)) != 0u;
    return translucentGlass || fallbackGlass;
}

bool PathTraceMaterialFeatureIsGuiScreen(RAB_Surface surface)
{
    return PathTraceMaterialFeatureIsTranslucent(surface) &&
        PathTraceMaterialFeatureTranslucentSubtype(surface) == RT_PATH_TRACE_FEATURE_TRANSLUCENT_SUBTYPE_GUI_SCREEN;
}

bool PathTraceMaterialFeatureIsParticle(RAB_Surface surface)
{
    return PathTraceMaterialFeatureIsTranslucent(surface) &&
        PathTraceMaterialFeatureTranslucentSubtype(surface) == RT_PATH_TRACE_FEATURE_TRANSLUCENT_SUBTYPE_SMOKE_PARTICLE;
}

uint PathTraceMaterialFeatureModifierKind(RAB_Surface surface)
{
    if ((surface.material.flags & RT_PATH_TRACE_FEATURE_MATERIAL_DETAIL_DECAL_DIFFUSE_LIT) != 0u)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_DIFFUSE_LIT;
    }
    if ((surface.material.flags & RT_PATH_TRACE_FEATURE_MATERIAL_FILTER_DECAL) != 0u)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_MODULATE_FILTER;
    }
    if ((surface.material.flags & RT_PATH_TRACE_FEATURE_MATERIAL_ADDITIVE_DECAL) != 0u)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_ADDITIVE_EMISSIVE;
    }
    if ((surface.material.flags & RT_PATH_TRACE_FEATURE_MATERIAL_DETAIL_DECAL) != 0u)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_OVER;
    }
    return RT_PATH_TRACE_MATERIAL_MODIFIER_NONE;
}

uint PathTraceMaterialFeatureKind(RAB_Surface surface)
{
    if (!RAB_IsSurfaceValid(surface))
    {
        return RT_PATH_TRACE_MATERIAL_KIND_UNKNOWN;
    }
    if (PathTraceMaterialFeatureIsGuiScreen(surface))
    {
        return RT_PATH_TRACE_MATERIAL_KIND_GUI_SCREEN;
    }
    if (PathTraceMaterialFeatureIsParticle(surface))
    {
        return RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_PARTICLE;
    }
    if (PathTraceMaterialFeatureIsGlassLike(surface))
    {
        return RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS;
    }
    if (PathTraceMaterialFeatureIsTranslucent(surface))
    {
        return RT_PATH_TRACE_MATERIAL_KIND_UNKNOWN;
    }
    if (PathTraceMaterialFeatureModifierKind(surface) != RT_PATH_TRACE_MATERIAL_MODIFIER_NONE)
    {
        return RT_PATH_TRACE_MATERIAL_KIND_DECAL_MODIFIER;
    }
    if ((surface.material.flags & RT_PATH_TRACE_FEATURE_MATERIAL_ALPHA_TEST) != 0u)
    {
        return RT_PATH_TRACE_MATERIAL_KIND_ALPHA_TESTED;
    }
    return RT_PATH_TRACE_MATERIAL_KIND_OPAQUE;
}

PathTraceMaterialFeature BuildMaterialFeatureFromPrimarySurface(RAB_Surface surface)
{
    PathTraceMaterialFeature feature = (PathTraceMaterialFeature)0;
    feature.materialKind = PathTraceMaterialFeatureKind(surface);
    feature.modifierKind = PathTraceMaterialFeatureModifierKind(surface);
    feature.passSupport = RT_PATH_TRACE_MATERIAL_PASS_DEBUG_VISUALIZER;

    if (!RAB_IsSurfaceValid(surface))
    {
        feature.materialCaps = RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED;
        return feature;
    }

    const bool opacityVisible = surface.material.opacity > 0.0;
    const bool opaqueCompatible = opacityVisible && !PathTraceMaterialFeatureIsTranslucent(surface);
    if (opaqueCompatible)
    {
        feature.materialCaps =
            RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_DIRECT |
            RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_GI |
            RT_PATH_TRACE_MATERIAL_CAP_PATH_DIFFUSE |
            RT_PATH_TRACE_MATERIAL_CAP_SHADOW_OCCLUSION |
            RT_PATH_TRACE_MATERIAL_CAP_RR_DIFFUSE_GUIDE;
        feature.lobeCaps = RT_PATH_TRACE_MATERIAL_LOBE_DIFFUSE_REFLECTION;
        feature.passSupport |=
            RT_PATH_TRACE_MATERIAL_PASS_PRIMARY_SURFACE |
            RT_PATH_TRACE_MATERIAL_PASS_PATH_INTEGRATOR |
            RT_PATH_TRACE_MATERIAL_PASS_DIRECT_RESERVOIR |
            RT_PATH_TRACE_MATERIAL_PASS_GI_RESERVOIR |
            RT_PATH_TRACE_MATERIAL_PASS_RR_GUIDE_EXPORT;
        if ((surface.material.flags & RT_PATH_TRACE_FEATURE_MATERIAL_ALPHA_TEST) != 0u)
        {
            feature.materialCaps |= RT_PATH_TRACE_MATERIAL_CAP_VISIBILITY_RAY_ALPHA_TEST;
        }
        if ((surface.material.flags & RT_PATH_TRACE_FEATURE_MATERIAL_EMISSIVE) != 0u)
        {
            feature.lobeCaps |= RT_PATH_TRACE_MATERIAL_LOBE_EMISSIVE;
        }
        if (feature.modifierKind != RT_PATH_TRACE_MATERIAL_MODIFIER_NONE)
        {
            feature.materialCaps |= RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER;
        }
    }
    else
    {
        feature.materialCaps = RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED;
    }

    return feature;
}

PathTraceMaterialFeature LoadPathTraceMaterialFeature(RAB_Surface surface)
{
    return BuildMaterialFeatureFromPrimarySurface(surface);
}

PathTraceMaterialFeature ResolveReceiverModifiers(PathTraceMaterialFeature receiverFeature)
{
    return receiverFeature;
}

bool MaterialSupportsOpaqueDirect(RAB_Surface surface)
{
    const PathTraceMaterialFeature feature = BuildMaterialFeatureFromPrimarySurface(surface);
    return (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_DIRECT) != 0u;
}

bool MaterialSupportsPathEvent(RAB_Surface surface, uint lobeMask)
{
    const PathTraceMaterialFeature feature = BuildMaterialFeatureFromPrimarySurface(surface);
    return (feature.lobeCaps & lobeMask) != 0u &&
        (feature.passSupport & RT_PATH_TRACE_MATERIAL_PASS_PATH_INTEGRATOR) != 0u;
}

bool MaterialSupportsTransmission(RAB_Surface surface)
{
    const PathTraceMaterialFeature feature = BuildMaterialFeatureFromPrimarySurface(surface);
    return (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION) != 0u;
}

bool MaterialSupportedByPass(RAB_Surface surface, uint passKind)
{
    const PathTraceMaterialFeature feature = BuildMaterialFeatureFromPrimarySurface(surface);
    return (feature.passSupport & passKind) != 0u;
}

float4 MaterialFailClosedDebugColor(RAB_Surface surface, uint passKind)
{
    const PathTraceMaterialFeature feature = BuildMaterialFeatureFromPrimarySurface(surface);
    if ((feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED) == 0u &&
        (feature.passSupport & passKind) != 0u)
    {
        return float4(0.03, 0.26, 0.08, 1.0);
    }
    if (feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS)
    {
        return float4(0.05, 0.75, 1.0, 1.0);
    }
    if (feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_PARTICLE)
    {
        return float4(1.0, 0.45, 0.05, 1.0);
    }
    if (feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_GUI_SCREEN)
    {
        return float4(0.95, 0.10, 0.95, 1.0);
    }
    return float4(0.85, 0.02, 0.08, 1.0);
}

#endif
