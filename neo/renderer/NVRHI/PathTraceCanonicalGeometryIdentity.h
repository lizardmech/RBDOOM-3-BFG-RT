#pragma once

// Passive value contract for canonical PT geometry identity.
//
// These types deliberately contain no renderer pointers, cache handles,
// material IDs, transforms, storage offsets, or AS handles. They are not wired
// into live scene routing yet; GEO-05/06 supply durable world and asset IDs.

#include <cstdint>

enum class PtCanonicalMeshSourceDomain : std::uint32_t
{
    Invalid = 0,
    StaticWorldMap,
    RegisteredRenderModel,
    SkinnedBindSource,
    GeneratedPersistent,
    UnsupportedTransient
};

enum class PtCanonicalDeformationClass : std::uint32_t
{
    Invalid = 0,
    Static,
    Rigid,
    SingleBoneRigid,
    Skinned
};

enum class PtCanonicalSubInstanceKind : std::uint32_t
{
    Invalid = 0,
    StaticSurface,
    RigidSurface,
    SkinnedSurface,
    JointSubmesh
};

enum class PtCanonicalHistoryOwnerKind : std::uint32_t
{
    Invalid = 0,
    RenderTarget
};

enum class PtCanonicalHistoryOwnerRole : std::uint32_t
{
    Invalid = 0,
    PrimaryGameplay,
    Subview
};

enum class PtCanonicalIdentityProducer : std::uint32_t
{
    Invalid = 0,
    VisibleDrawSurf,
    LifecycleFeed,
    AreaDiscovery
};

struct PtCanonicalWorldKey
{
    std::uint64_t worldGeneration = 0;
};

struct PtCanonicalMeshKey
{
    std::uint64_t sourceAssetId = 0;
    std::uint64_t sourceAssetGeneration = 0;
    std::uint64_t topologySignature = 0;
    PtCanonicalMeshSourceDomain sourceDomain = PtCanonicalMeshSourceDomain::Invalid;
    std::uint32_t modelSurfaceIndex = UINT32_MAX;
    std::uint32_t vertexFormat = 0;
    PtCanonicalDeformationClass deformationClass = PtCanonicalDeformationClass::Invalid;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::int32_t jointSubmeshIndex = -1;
};

struct PtCanonicalInstanceKey
{
    std::uint64_t worldGeneration = 0;
    std::uint32_t renderDefIndex = UINT32_MAX;
    std::uint32_t renderDefGeneration = 0;
    PtCanonicalSubInstanceKind subInstanceKind = PtCanonicalSubInstanceKind::Invalid;
    std::uint32_t modelSurfaceIndex = UINT32_MAX;
    std::int32_t jointSubmeshIndex = -1;
};

struct PtCanonicalPrimitiveKey
{
    PtCanonicalMeshKey mesh;
    std::uint32_t localPrimitiveIndex = UINT32_MAX;
};

struct PtCanonicalHistoryOwnerKey
{
    std::uint64_t worldGeneration = 0;
    std::uint64_t ownerGeneration = 0;
    PtCanonicalHistoryOwnerKind ownerKind = PtCanonicalHistoryOwnerKind::Invalid;
    PtCanonicalHistoryOwnerRole ownerRole = PtCanonicalHistoryOwnerRole::Invalid;
};

bool operator==(const PtCanonicalWorldKey& lhs, const PtCanonicalWorldKey& rhs);
bool operator!=(const PtCanonicalWorldKey& lhs, const PtCanonicalWorldKey& rhs);
bool operator==(const PtCanonicalMeshKey& lhs, const PtCanonicalMeshKey& rhs);
bool operator!=(const PtCanonicalMeshKey& lhs, const PtCanonicalMeshKey& rhs);
bool operator==(const PtCanonicalInstanceKey& lhs, const PtCanonicalInstanceKey& rhs);
bool operator!=(const PtCanonicalInstanceKey& lhs, const PtCanonicalInstanceKey& rhs);
bool operator==(const PtCanonicalPrimitiveKey& lhs, const PtCanonicalPrimitiveKey& rhs);
bool operator!=(const PtCanonicalPrimitiveKey& lhs, const PtCanonicalPrimitiveKey& rhs);
bool operator==(const PtCanonicalHistoryOwnerKey& lhs, const PtCanonicalHistoryOwnerKey& rhs);
bool operator!=(const PtCanonicalHistoryOwnerKey& lhs, const PtCanonicalHistoryOwnerKey& rhs);

bool PtCanonicalWorldKeyIsValid(const PtCanonicalWorldKey& key);
bool PtCanonicalMeshKeyIsValid(const PtCanonicalMeshKey& key);
bool PtCanonicalInstanceKeyIsValid(const PtCanonicalInstanceKey& key);
bool PtCanonicalPrimitiveKeyIsValid(const PtCanonicalPrimitiveKey& key);
bool PtCanonicalHistoryOwnerKeyIsValid(const PtCanonicalHistoryOwnerKey& key);

std::uint64_t PtHashCanonicalWorldKey(const PtCanonicalWorldKey& key);
std::uint64_t PtHashCanonicalMeshKey(const PtCanonicalMeshKey& key);
std::uint64_t PtHashCanonicalInstanceKey(const PtCanonicalInstanceKey& key);
std::uint64_t PtHashCanonicalPrimitiveKey(const PtCanonicalPrimitiveKey& key);
std::uint64_t PtHashCanonicalHistoryOwnerKey(const PtCanonicalHistoryOwnerKey& key);

enum PtCanonicalIdentityDisagreementFlags : std::uint32_t
{
    PT_CANONICAL_IDENTITY_EXACT = 0,
    PT_CANONICAL_IDENTITY_MISSING_PRODUCER = 1u << 0,
    PT_CANONICAL_IDENTITY_MISSING_DURABLE_WORLD = 1u << 1,
    PT_CANONICAL_IDENTITY_MISSING_DURABLE_ASSET = 1u << 2,
    PT_CANONICAL_IDENTITY_UNSUPPORTED_TRANSIENT = 1u << 3,
    PT_CANONICAL_IDENTITY_WORLD_MISMATCH = 1u << 4,
    PT_CANONICAL_IDENTITY_INSTANCE_SLOT_MISMATCH = 1u << 5,
    PT_CANONICAL_IDENTITY_SURFACE_ORDINAL_MISMATCH = 1u << 6,
    PT_CANONICAL_IDENTITY_SOURCE_ASSET_MISMATCH = 1u << 7,
    PT_CANONICAL_IDENTITY_TOPOLOGY_MISMATCH = 1u << 8,
    PT_CANONICAL_IDENTITY_LAYOUT_OR_DEFORMATION_MISMATCH = 1u << 9,
    PT_CANONICAL_IDENTITY_MESH_HASH_COLLISION = 1u << 10,
    PT_CANONICAL_IDENTITY_INSTANCE_HASH_COLLISION = 1u << 11,
    PT_CANONICAL_IDENTITY_MATERIAL_ONLY_DIFFERENCE = 1u << 12,
    PT_CANONICAL_IDENTITY_TRANSIENT_STORAGE_ONLY_DIFFERENCE = 1u << 13
};

struct PtCanonicalIdentityObservation
{
    PtCanonicalIdentityProducer producer = PtCanonicalIdentityProducer::Invalid;
    std::uint64_t observationFrame = 0;
    bool present = false;
    bool registryEligible = false;
    bool hasDurableWorld = false;
    bool hasDurableAsset = false;
    bool hasDerivedHashes = false;
    PtCanonicalWorldKey world;
    PtCanonicalMeshKey mesh;
    PtCanonicalInstanceKey instance;
    std::uint64_t meshHash = 0;
    std::uint64_t instanceHash = 0;
    std::uint64_t materialBindingRevision = 0;
    std::uint64_t storageGeneration = 0;
};

std::uint32_t PtCompareCanonicalIdentityObservations(
    const PtCanonicalIdentityObservation& lhs,
    const PtCanonicalIdentityObservation& rhs);

bool PtCheckedAddU64(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result);
bool PtCheckedMulU64(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result);

enum class PtCanonicalShaderNarrowResult : std::uint32_t
{
    Success = 0,
    EmptyRange,
    InvalidElementStride,
    ArithmeticOverflow,
    BeforePage,
    Misaligned,
    OutsidePage,
    ShaderWidthExceeded
};

struct PtCanonicalShaderPageRange
{
    std::uint64_t pageBaseBytes = 0;
    std::uint64_t pageSizeBytes = 0;
    std::uint64_t absoluteOffsetBytes = 0;
    std::uint64_t byteSize = 0;
    std::uint64_t elementStrideBytes = 0;
};

struct PtCanonicalShaderRange32
{
    std::uint32_t elementOffset = 0;
    std::uint32_t elementCount = 0;
};

PtCanonicalShaderNarrowResult PtNarrowCanonicalRangeToShaderPage(
    const PtCanonicalShaderPageRange& input,
    PtCanonicalShaderRange32& output);
