#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

constexpr std::size_t RT_PT_COMMITTED_REFERENCED_SET_MAP_NAME_CAPACITY = 256u;

enum class RtPathTraceCaptureProductProvenance : std::uint8_t
{
    None,
    OwnerDecisions,
    PureSemantic
};

class RtPathTraceCommittedReferencedSetFactsProvider
{
public:
    virtual ~RtPathTraceCommittedReferencedSetFactsProvider() = default;
    virtual bool Complete() const noexcept = 0;
    virtual bool IsRigidRouteReady(std::uint64_t meshHash) const = 0;
    virtual bool IsRigidRouteResidentReadyForEntityMaterial(
        std::int32_t entityIndex, std::int32_t renderEntityNum,
        std::uint32_t materialId) const = 0;
};

struct RtPathTraceCommittedReferencedSetReceiptHeader
{
    std::uint64_t sealedPrimaryViewToken = 0;
    std::uint64_t ownerUniverseFrameIndex = 0;
    std::uint64_t baselineFrameIndex = 0;
    std::uint64_t worldLifecycleGeneration = 0;
    std::uint64_t mapLoadSerial = 0;
    std::uint64_t mapTimeStamp = 0;
    char mapName[RT_PT_COMMITTED_REFERENCED_SET_MAP_NAME_CAPACITY] = {};
    std::uint64_t barrierGeneration = 0;
    std::uint64_t materialRegistryGeneration = 0;
    std::uint64_t residentMaterialFactsGeneration = 0;
    std::uint64_t configFingerprint = 0;
    std::uint64_t instanceUniverseGeneration = 0;
    std::uint64_t geometryUniverseGeneration = 0;
    std::uint64_t staticMaterialGeneration = 0;
    std::uint64_t canonicalSourceIndexPoolGeneration = 0;
    std::uint64_t staticResidentPayloadGeneration = 0;
    std::uint64_t finalizedMaterialIdHash = 0;
    std::uint32_t surfaceCount = 0;
    std::uint32_t finalizedMaterialIdCount = 0;
    bool recordAllInstanceClasses = false;
    bool complete = false;
};
static_assert(std::is_trivially_copyable<
    RtPathTraceCommittedReferencedSetReceiptHeader>::value,
    "referenced-set receipt header must remain pointer-free POD");

struct RtPathTraceCommittedReferencedSurfaceReceiptRow
{
    std::uint64_t persistedMeshHash = 0;
    std::uint32_t persistedChosenMaterialId = 0;
    std::uint32_t surfaceOrdinal = 0;
    std::int32_t entityIndex = -1;
    std::int32_t renderEntityNum = -1;
    bool expectedReadyByMesh = false;
    bool expectedReadyByResident = false;
    bool rigidIdentityPresent = false;
    bool derivedRuntimeMaterialPresent = false;
};
static_assert(std::is_trivially_copyable<
    RtPathTraceCommittedReferencedSurfaceReceiptRow>::value,
    "referenced-set receipt rows must remain pointer-free POD");

inline bool RtPathTraceCommittedReferencedSetReceiptStorageShapeValid(
    const RtPathTraceCommittedReferencedSetReceiptHeader& header,
    const RtPathTraceCommittedReferencedSurfaceReceiptRow* rows,
    std::size_t rowCount) noexcept
{
    if (header.surfaceCount != rowCount ||
        (rowCount != 0 && rows == nullptr))
    {
        return false;
    }
    for (std::size_t index = 0; index < rowCount; ++index)
    {
        if (rows[index].surfaceOrdinal != index)
        {
            return false;
        }
    }
    return true;
}

enum class RtPathTraceCommittedReferencedSetReject : std::uint8_t
{
    None,
    ReceiptIncomplete,
    AuthorityIncomplete,
    SealedTokenMismatch,
    OwnerFrameMismatch,
    BaselineFrameMismatch,
    WorldLifecycleMismatch,
    MapNameMismatch,
    MapTimeMismatch,
    MapLoadSerialMismatch,
    BarrierMismatch,
    MaterialRegistryGenerationMismatch,
    ResidentFactsGenerationMismatch,
    ConfigFingerprintMismatch,
    InstanceOwnerGenerationMismatch,
    RecordAllTrueToFalse,
    RecordAllFalseToTrue,
    SurfaceCountMismatch,
    MissingRows,
    GeometryWitnessInvalid,
    FactsProviderIncomplete,
    RowOrdinalMismatch,
    RowMeshHashMismatch,
    RowChosenMaterialMismatch,
    RowEntityIndexMismatch,
    RowRenderEntityNumMismatch,
    RowRigidIdentityMismatch,
    RowRuntimeMaterialPresenceMismatch,
    RowReadyByMeshMismatch,
    RowReadyByResidentMismatch
};

struct RtPathTraceCommittedReferencedSetValidationStats
{
    std::size_t geometryPrefilterHits = 0;
    std::size_t rowComparisons = 0;
};

struct RtPathTraceCommittedReferencedSetValidationResult
{
    RtPathTraceCommittedReferencedSetReject rejection =
        RtPathTraceCommittedReferencedSetReject::None;
    std::size_t rowsCompared = 0;
    bool geometryPrefilterHit = false;
    bool accepted = false;
};

RtPathTraceCommittedReferencedSetValidationResult
ValidatePathTraceCommittedReferencedSetReceiptP2c3b(
    const RtPathTraceCommittedReferencedSetReceiptHeader& receipt,
    const RtPathTraceCommittedReferencedSurfaceReceiptRow* receiptRows,
    std::size_t receiptRowCount,
    const RtPathTraceCommittedReferencedSetReceiptHeader& currentAuthority,
    const RtPathTraceCommittedReferencedSurfaceReceiptRow* currentRows,
    std::size_t currentRowCount,
    const RtPathTraceCommittedReferencedSetFactsProvider& currentFacts,
    RtPathTraceCommittedReferencedSetValidationStats* stats = nullptr) noexcept;
