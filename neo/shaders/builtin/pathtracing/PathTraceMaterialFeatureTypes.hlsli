#ifndef RB_PATH_TRACE_MATERIAL_FEATURE_TYPES_HLSLI
#define RB_PATH_TRACE_MATERIAL_FEATURE_TYPES_HLSLI

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

static const uint RT_PATH_TRACE_MATERIAL_FEATURE_RECORD_ABI_VERSION = 1u;

static const uint RT_PATH_TRACE_MATERIAL_FEATURE_RUNTIME_WRITES_OUTPUT_COLOR = 0u;
static const uint RT_PATH_TRACE_MATERIAL_FEATURE_RUNTIME_READY = 1u;
static const uint RT_PATH_TRACE_MATERIAL_FEATURE_RUNTIME_DEBUG_MODE = 2u;
static const uint RT_PATH_TRACE_MATERIAL_FEATURE_RUNTIME_FRAME_INDEX = 3u;

struct PathTraceMaterialFeatureRuntimeInfo
{
    bool writesOutputColor;
    bool ready;
    float debugMode;
    float frameIndex;
};

PathTraceMaterialFeatureRuntimeInfo LoadPathTraceMaterialFeatureRuntimeInfo(float4 packedRuntimeInfo)
{
    PathTraceMaterialFeatureRuntimeInfo runtimeInfo;
    runtimeInfo.writesOutputColor = packedRuntimeInfo[RT_PATH_TRACE_MATERIAL_FEATURE_RUNTIME_WRITES_OUTPUT_COLOR] >= 0.5;
    runtimeInfo.ready = packedRuntimeInfo[RT_PATH_TRACE_MATERIAL_FEATURE_RUNTIME_READY] >= 0.5;
    runtimeInfo.debugMode = packedRuntimeInfo[RT_PATH_TRACE_MATERIAL_FEATURE_RUNTIME_DEBUG_MODE];
    runtimeInfo.frameIndex = packedRuntimeInfo[RT_PATH_TRACE_MATERIAL_FEATURE_RUNTIME_FRAME_INDEX];
    return runtimeInfo;
}

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
static const uint RT_PATH_TRACE_FEATURE_MATERIAL_DETAIL_DECAL_LIQUID_POOL = 0x00010000u;

struct PathTraceMaterialFeature
{
    uint materialKind;
    uint materialCaps;
    uint lobeCaps;
    uint passSupport;
    uint modifierKind;
    uint parameterRecordIndex;
};

struct PathTraceMaterialFeatureRecord
{
    uint materialKind;
    uint materialCaps;
    uint lobeCaps;
    uint passSupport;
    uint modifierKind;
    uint parameterRecordIndex;
    uint recordAbiVersion;
    uint reserved1;
};

struct PathTraceMaterialFeatureParameterRecord
{
    float4 params0;
    float4 params1;
};

PathTraceMaterialFeature PathTraceMaterialFeatureFromRecord(PathTraceMaterialFeatureRecord record)
{
    PathTraceMaterialFeature feature;
    feature.materialKind = record.materialKind;
    feature.materialCaps = record.materialCaps;
    feature.lobeCaps = record.lobeCaps;
    feature.passSupport = record.passSupport;
    feature.modifierKind = record.modifierKind;
    feature.parameterRecordIndex = record.parameterRecordIndex;
    return feature;
}

#endif
