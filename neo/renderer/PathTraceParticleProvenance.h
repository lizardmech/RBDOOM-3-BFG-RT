#pragma once

#include <stdint.h>

// Compact provenance shared by game-side smoke emission and renderer-side
// particle capture. Generated smoke vertices carry this value in color2, which
// is otherwise unused by the non-skinned global smoke model.
enum class RtPathTraceParticleDepthPolicy : uint32_t
{
    World = 0,
    WeaponProjection = 1,
    ModelProjection = 2,
    WorldMuzzleNearPlane = 3
};

enum class RtPathTraceParticleSourceClass : uint32_t
{
    World = 0,
    LocalWeapon = 1,
    AttachedWeaponEmitter = 2,
    ProjectileTrail = 3,
    Impact = 4,
    Unknown = 5
};

struct RtPathTraceParticleProvenance
{
    RtPathTraceParticleSourceClass sourceClass = RtPathTraceParticleSourceClass::World;
    RtPathTraceParticleDepthPolicy depthPolicy = RtPathTraceParticleDepthPolicy::World;
    int sourceEntityId = -1;
    int allowSurfaceInViewId = 0;

    RtPathTraceParticleProvenance() = default;
    RtPathTraceParticleProvenance(
        RtPathTraceParticleSourceClass source,
        RtPathTraceParticleDepthPolicy depth,
        int entityId = -1,
        int allowViewId = 0)
        : sourceClass(source),
          depthPolicy(depth),
          sourceEntityId(entityId),
          allowSurfaceInViewId(allowViewId)
    {
    }
};

static constexpr uint32_t RT_PATH_TRACE_PARTICLE_STABLE_ID_MASK = 0x00ffffffu;
static constexpr uint32_t RT_PATH_TRACE_PARTICLE_STABLE_ID_VALUE_MASK = 0x007fffffu;
static constexpr uint32_t RT_PATH_TRACE_PARTICLE_PARAMETRIC_ID_NAMESPACE = 0x00800000u;
static constexpr uint32_t RT_PATH_TRACE_PARTICLE_SOURCE_SHIFT = 24u;
static constexpr uint32_t RT_PATH_TRACE_PARTICLE_DEPTH_SHIFT = 27u;
static constexpr uint32_t RT_PATH_TRACE_PARTICLE_METADATA_MAGIC_MASK = 0xe0000000u;
static constexpr uint32_t RT_PATH_TRACE_PARTICLE_METADATA_MAGIC = 0xa0000000u;

inline uint32_t PackRtPathTraceParticleMetadata(
    uint32_t stableId,
    RtPathTraceParticleSourceClass sourceClass,
    RtPathTraceParticleDepthPolicy depthPolicy)
{
    return RT_PATH_TRACE_PARTICLE_METADATA_MAGIC |
        (stableId & RT_PATH_TRACE_PARTICLE_STABLE_ID_MASK) |
        ((static_cast<uint32_t>(sourceClass) & 0x7u) << RT_PATH_TRACE_PARTICLE_SOURCE_SHIFT) |
        ((static_cast<uint32_t>(depthPolicy) & 0x3u) << RT_PATH_TRACE_PARTICLE_DEPTH_SHIFT);
}

inline bool IsRtPathTraceParticleMetadata(uint32_t metadata)
{
    return (metadata & RT_PATH_TRACE_PARTICLE_METADATA_MAGIC_MASK) == RT_PATH_TRACE_PARTICLE_METADATA_MAGIC;
}

inline uint32_t RtPathTraceParticleStableId(uint32_t metadata)
{
    return metadata & RT_PATH_TRACE_PARTICLE_STABLE_ID_MASK;
}

inline RtPathTraceParticleSourceClass RtPathTraceParticleMetadataSource(uint32_t metadata)
{
    return static_cast<RtPathTraceParticleSourceClass>((metadata >> RT_PATH_TRACE_PARTICLE_SOURCE_SHIFT) & 0x7u);
}

inline RtPathTraceParticleDepthPolicy RtPathTraceParticleMetadataDepth(uint32_t metadata)
{
    return static_cast<RtPathTraceParticleDepthPolicy>((metadata >> RT_PATH_TRACE_PARTICLE_DEPTH_SHIFT) & 0x3u);
}
