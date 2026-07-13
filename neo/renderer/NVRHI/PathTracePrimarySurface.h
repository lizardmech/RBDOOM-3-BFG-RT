#pragma once

// CPU-side primary-surface contract for the RT/PT path.
//
// The shader-side mirror lives in
// neo/shaders/builtin/pathtracing/PathTracePrimarySurface.hlsli. Keep the
// record version, stride, and field order in sync when adding denoiser,
// motion-vector, or richer material fields.

#include <cstdint>

static constexpr uint32_t RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION = 2;
static constexpr uint32_t RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_STRIDE = 176;
static constexpr uint32_t RT_PATH_TRACE_MATERIAL_FEATURE_RECORD_ABI_VERSION = 3;

enum RtPathTracePrimarySurfaceValidFlags : uint32_t
{
    RT_PRIMARY_SURFACE_VALID = 1u << 0,
    RT_PRIMARY_SURFACE_HAS_CAMERA_REPROJECTION = 1u << 1,
    RT_PRIMARY_SURFACE_HAS_OBJECT_MOTION = 1u << 2,
    RT_PRIMARY_SURFACE_HAS_PREVIOUS_POSITION = 1u << 3
};

enum RtPathTracePrimarySurfaceDebugStatus : uint32_t
{
    RT_PRIMARY_SURFACE_DEBUG_OK = 0,
    RT_PRIMARY_SURFACE_DEBUG_MISSING_CURRENT = 1,
    RT_PRIMARY_SURFACE_DEBUG_MISSING_PREVIOUS = 2,
    RT_PRIMARY_SURFACE_DEBUG_REJECTED_PREVIOUS = 3,
    RT_PRIMARY_SURFACE_DEBUG_MATERIAL_MISMATCH = 4,
    RT_PRIMARY_SURFACE_DEBUG_NORMAL_MISMATCH = 5,
    RT_PRIMARY_SURFACE_DEBUG_ROUGHNESS_MISMATCH = 6,
    RT_PRIMARY_SURFACE_DEBUG_NO_OBJECT_MOTION = 7,
    RT_PRIMARY_SURFACE_DEBUG_SKINNED_MISSING_PREVIOUS = 8,
    RT_PRIMARY_SURFACE_DEBUG_SKINNED_RANGE_MISMATCH = 9,
    RT_PRIMARY_SURFACE_DEBUG_SKINNED_PREVIOUS_OUT_OF_RANGE = 10,
    RT_PRIMARY_SURFACE_DEBUG_RIGID_MISSING_PREVIOUS = 11,
    RT_PRIMARY_SURFACE_DEBUG_RIGID_RANGE_MISMATCH = 12
};

enum RtPathTraceMaterialKind : uint32_t
{
    RT_PATH_TRACE_MATERIAL_KIND_UNKNOWN = 0,
    RT_PATH_TRACE_MATERIAL_KIND_OPAQUE = 1,
    RT_PATH_TRACE_MATERIAL_KIND_ALPHA_TESTED = 2,
    RT_PATH_TRACE_MATERIAL_KIND_DECAL_MODIFIER = 3,
    RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER = 4,
    RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS = 5,
    RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_PARTICLE = 6,
    RT_PATH_TRACE_MATERIAL_KIND_GUI_SCREEN = 7,
    RT_PATH_TRACE_MATERIAL_KIND_EMISSIVE_SPECIAL = 8,
    RT_PATH_TRACE_MATERIAL_KIND_SKY_OR_ENVIRONMENT = 9
};

enum RtPathTraceMaterialCaps : uint32_t
{
    RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_DIRECT = 1u << 0,
    RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_GI = 1u << 1,
    RT_PATH_TRACE_MATERIAL_CAP_PATH_DIFFUSE = 1u << 2,
    RT_PATH_TRACE_MATERIAL_CAP_PATH_SPECULAR_REFLECTION = 1u << 3,
    RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION = 1u << 4,
    RT_PATH_TRACE_MATERIAL_CAP_SHADOW_OCCLUSION = 1u << 5,
    RT_PATH_TRACE_MATERIAL_CAP_VISIBILITY_RAY_ALPHA_TEST = 1u << 6,
    RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER = 1u << 7,
    RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND = 1u << 8,
    RT_PATH_TRACE_MATERIAL_CAP_RR_DIFFUSE_GUIDE = 1u << 9,
    RT_PATH_TRACE_MATERIAL_CAP_RR_SPECULAR_GUIDE = 1u << 10,
    RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED = 1u << 31
};

enum RtPathTraceMaterialLobeCaps : uint32_t
{
    RT_PATH_TRACE_MATERIAL_LOBE_DIFFUSE_REFLECTION = 1u << 0,
    RT_PATH_TRACE_MATERIAL_LOBE_SPECULAR_REFLECTION = 1u << 1,
    RT_PATH_TRACE_MATERIAL_LOBE_GLOSSY_REFLECTION = 1u << 2,
    RT_PATH_TRACE_MATERIAL_LOBE_SPECULAR_TRANSMISSION = 1u << 3,
    RT_PATH_TRACE_MATERIAL_LOBE_DIFFUSE_TRANSMISSION = 1u << 4,
    RT_PATH_TRACE_MATERIAL_LOBE_EMISSIVE = 1u << 5,
    RT_PATH_TRACE_MATERIAL_LOBE_ABSORB = 1u << 6
};

enum RtPathTraceMaterialPassSupport : uint32_t
{
    RT_PATH_TRACE_MATERIAL_PASS_PRIMARY_SURFACE = 1u << 0,
    RT_PATH_TRACE_MATERIAL_PASS_PATH_INTEGRATOR = 1u << 1,
    RT_PATH_TRACE_MATERIAL_PASS_DIRECT_RESERVOIR = 1u << 2,
    RT_PATH_TRACE_MATERIAL_PASS_GI_RESERVOIR = 1u << 3,
    RT_PATH_TRACE_MATERIAL_PASS_REFLECTION_PRODUCER = 1u << 4,
    RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER = 1u << 5,
    RT_PATH_TRACE_MATERIAL_PASS_RR_GUIDE_EXPORT = 1u << 6,
    RT_PATH_TRACE_MATERIAL_PASS_DEBUG_VISUALIZER = 1u << 7
};

enum RtPathTraceMaterialModifierKind : uint32_t
{
    RT_PATH_TRACE_MATERIAL_MODIFIER_NONE = 0,
    RT_PATH_TRACE_MATERIAL_MODIFIER_OVER = 1,
    RT_PATH_TRACE_MATERIAL_MODIFIER_MODULATE_FILTER = 2,
    RT_PATH_TRACE_MATERIAL_MODIFIER_ADDITIVE_EMISSIVE = 3,
    RT_PATH_TRACE_MATERIAL_MODIFIER_DIFFUSE_LIT = 4,
    RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION = 5
};

struct RtPathTracePrimarySurfaceRecord
{
    uint32_t header[4];                     // version, valid flags, debug/status flags, reserved
    float worldPositionAndViewDepth[4];     // xyz = world position, w = view depth
    float geometricNormalAndRoughness[4];   // xyz = geometric normal, w = roughness
    float shadingNormalAndOpacity[4];       // xyz = shading normal, w = opacity
    float viewDirectionAndReserved[4];      // xyz = view direction, w = reserved
    float albedoAndAlphaCutoff[4];          // xyz = diffuse albedo, w = alpha cutoff
    float specularF0AndReserved[4];         // xyz = F0/specular, w = reserved
    float emissiveAndHeight[4];             // xyz = emissive radiance, w = height/parallax/displacement placeholder
    float previousPositionOrMotion[4];      // xyz = previous world position or motion placeholder, w = valid selector
    uint32_t materialAndSurface[4];         // material id, material index, material flags, surface class
    uint32_t instancePrimitiveObject[4];    // instance id, primitive id, object/entity id placeholder, emissive texture index
};
static_assert(sizeof(RtPathTracePrimarySurfaceRecord) == RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_STRIDE, "Primary surface CPU/shader record stride mismatch");

struct RtPathTraceMaterialFeatureRecord
{
    uint32_t materialKind = RT_PATH_TRACE_MATERIAL_KIND_UNKNOWN;
    uint32_t materialCaps = RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED;
    uint32_t lobeCaps = 0;
    uint32_t passSupport = RT_PATH_TRACE_MATERIAL_PASS_DEBUG_VISUALIZER;
    uint32_t modifierKind = RT_PATH_TRACE_MATERIAL_MODIFIER_NONE;
    uint32_t parameterRecordIndex = UINT32_MAX;
    uint32_t recordAbiVersion = RT_PATH_TRACE_MATERIAL_FEATURE_RECORD_ABI_VERSION;
    uint32_t reserved1 = 0;
};
static_assert(sizeof(RtPathTraceMaterialFeatureRecord) == 32, "Material feature CPU/shader record stride mismatch");

struct RtPathTraceMaterialFeatureParameterRecord
{
    float params0[4] = {};
    float params1[4] = {};
    uint32_t orderedStageWords[8] = {};
    uint32_t orderedStageTextureWords[8] = {};
};
static_assert(sizeof(RtPathTraceMaterialFeatureParameterRecord) == 96, "Material feature parameter CPU/shader record stride mismatch");

struct RtPathTracePrimarySurfaceHistoryState
{
    bool currentValid = false;
    bool previousValid = false;
    bool samePixelHistoryValid = false;
    bool cameraReprojectionAvailable = false;
    bool objectMotionAvailable = false;
    uint32_t resetReasonFlags = 0;

    void Reset(uint32_t reasons = 0)
    {
        currentValid = false;
        previousValid = false;
        samePixelHistoryValid = false;
        cameraReprojectionAvailable = false;
        objectMotionAvailable = false;
        resetReasonFlags = reasons;
    }
};
