#include "PathTraceCommittedReferencedSetReceipt.h"

#include <cstring>

namespace
{
bool GeometryWitnessesMatch(
    const RtPathTraceCommittedReferencedSetReceiptHeader& left,
    const RtPathTraceCommittedReferencedSetReceiptHeader& right) noexcept
{
    return left.geometryUniverseGeneration == right.geometryUniverseGeneration &&
        left.staticMaterialGeneration == right.staticMaterialGeneration &&
        left.canonicalSourceIndexPoolGeneration ==
            right.canonicalSourceIndexPoolGeneration &&
        left.staticResidentPayloadGeneration ==
            right.staticResidentPayloadGeneration;
}
}

RtPathTraceCommittedReferencedSetValidationResult
ValidatePathTraceCommittedReferencedSetReceiptP2c3b(
    const RtPathTraceCommittedReferencedSetReceiptHeader& receipt,
    const RtPathTraceCommittedReferencedSurfaceReceiptRow* receiptRows,
    std::size_t receiptRowCount,
    const RtPathTraceCommittedReferencedSetReceiptHeader& current,
    const RtPathTraceCommittedReferencedSurfaceReceiptRow* currentRows,
    std::size_t currentRowCount,
    const RtPathTraceCommittedReferencedSetFactsProvider& currentFacts,
    RtPathTraceCommittedReferencedSetValidationStats* stats) noexcept
{
    RtPathTraceCommittedReferencedSetValidationResult result;
    const auto reject = [&result](RtPathTraceCommittedReferencedSetReject reason)
    {
        result.rejection = reason;
        return result;
    };
    if (!receipt.complete)
        return reject(RtPathTraceCommittedReferencedSetReject::ReceiptIncomplete);
    if (!current.complete)
        return reject(RtPathTraceCommittedReferencedSetReject::AuthorityIncomplete);
    if (receipt.sealedPrimaryViewToken == 0 ||
        receipt.sealedPrimaryViewToken != current.sealedPrimaryViewToken)
        return reject(RtPathTraceCommittedReferencedSetReject::SealedTokenMismatch);
    if (receipt.ownerUniverseFrameIndex != current.ownerUniverseFrameIndex)
        return reject(RtPathTraceCommittedReferencedSetReject::OwnerFrameMismatch);
    if (receipt.baselineFrameIndex != current.baselineFrameIndex)
        return reject(RtPathTraceCommittedReferencedSetReject::BaselineFrameMismatch);
    if (receipt.worldLifecycleGeneration == 0 ||
        receipt.worldLifecycleGeneration != current.worldLifecycleGeneration)
        return reject(RtPathTraceCommittedReferencedSetReject::WorldLifecycleMismatch);
    if (std::memcmp(receipt.mapName, current.mapName,
            sizeof(receipt.mapName)) != 0)
        return reject(RtPathTraceCommittedReferencedSetReject::MapNameMismatch);
    if (receipt.mapTimeStamp != current.mapTimeStamp)
        return reject(RtPathTraceCommittedReferencedSetReject::MapTimeMismatch);
    if (receipt.mapLoadSerial != current.mapLoadSerial)
        return reject(RtPathTraceCommittedReferencedSetReject::MapLoadSerialMismatch);
    if (receipt.barrierGeneration == 0 ||
        receipt.barrierGeneration != current.barrierGeneration)
        return reject(RtPathTraceCommittedReferencedSetReject::BarrierMismatch);
    if (receipt.materialRegistryGeneration == 0 ||
        receipt.materialRegistryGeneration != current.materialRegistryGeneration)
        return reject(RtPathTraceCommittedReferencedSetReject::MaterialRegistryGenerationMismatch);
    if (receipt.residentMaterialFactsGeneration == 0 ||
        receipt.residentMaterialFactsGeneration != current.residentMaterialFactsGeneration)
        return reject(RtPathTraceCommittedReferencedSetReject::ResidentFactsGenerationMismatch);
    if (receipt.configFingerprint == 0 ||
        receipt.configFingerprint != current.configFingerprint)
        return reject(RtPathTraceCommittedReferencedSetReject::ConfigFingerprintMismatch);
    if (receipt.instanceUniverseGeneration == 0 ||
        receipt.instanceUniverseGeneration != current.instanceUniverseGeneration)
        return reject(RtPathTraceCommittedReferencedSetReject::InstanceOwnerGenerationMismatch);
    if (receipt.recordAllInstanceClasses && !current.recordAllInstanceClasses)
        return reject(RtPathTraceCommittedReferencedSetReject::RecordAllTrueToFalse);
    if (!receipt.recordAllInstanceClasses && current.recordAllInstanceClasses)
        return reject(RtPathTraceCommittedReferencedSetReject::RecordAllFalseToTrue);
    if (receipt.surfaceCount != current.surfaceCount ||
        receiptRowCount != receipt.surfaceCount ||
        currentRowCount != current.surfaceCount)
        return reject(RtPathTraceCommittedReferencedSetReject::SurfaceCountMismatch);
    if ((receiptRowCount != 0 && receiptRows == nullptr) ||
        (currentRowCount != 0 && currentRows == nullptr))
        return reject(RtPathTraceCommittedReferencedSetReject::MissingRows);
    if (receipt.geometryUniverseGeneration == 0 ||
        receipt.staticMaterialGeneration == 0 ||
        receipt.canonicalSourceIndexPoolGeneration == 0 ||
        receipt.staticResidentPayloadGeneration == 0 ||
        current.geometryUniverseGeneration == 0 ||
        current.staticMaterialGeneration == 0 ||
        current.canonicalSourceIndexPoolGeneration == 0 ||
        current.staticResidentPayloadGeneration == 0)
        return reject(RtPathTraceCommittedReferencedSetReject::GeometryWitnessInvalid);

    if (GeometryWitnessesMatch(receipt, current))
    {
        result.geometryPrefilterHit = true;
        result.accepted = true;
        if (stats != nullptr)
            ++stats->geometryPrefilterHits;
        return result;
    }
    if (!currentFacts.Complete())
        return reject(RtPathTraceCommittedReferencedSetReject::FactsProviderIncomplete);
    for (std::size_t index = 0; index < receiptRowCount; ++index)
    {
        const auto& row = receiptRows[index];
        const auto& expected = currentRows[index];
        ++result.rowsCompared;
        if (stats != nullptr)
            ++stats->rowComparisons;
        if (row.surfaceOrdinal != index || expected.surfaceOrdinal != index)
            return reject(RtPathTraceCommittedReferencedSetReject::RowOrdinalMismatch);
        if (row.persistedMeshHash != expected.persistedMeshHash)
            return reject(RtPathTraceCommittedReferencedSetReject::RowMeshHashMismatch);
        if (row.persistedChosenMaterialId != expected.persistedChosenMaterialId)
            return reject(RtPathTraceCommittedReferencedSetReject::RowChosenMaterialMismatch);
        if (row.entityIndex != expected.entityIndex)
            return reject(RtPathTraceCommittedReferencedSetReject::RowEntityIndexMismatch);
        if (row.renderEntityNum != expected.renderEntityNum)
            return reject(RtPathTraceCommittedReferencedSetReject::RowRenderEntityNumMismatch);
        if (row.rigidIdentityPresent != expected.rigidIdentityPresent)
            return reject(RtPathTraceCommittedReferencedSetReject::RowRigidIdentityMismatch);
        if (row.derivedRuntimeMaterialPresent !=
                expected.derivedRuntimeMaterialPresent)
            return reject(RtPathTraceCommittedReferencedSetReject::RowRuntimeMaterialPresenceMismatch);
        if (!row.rigidIdentityPresent)
            continue;
        if (row.expectedReadyByMesh !=
                currentFacts.IsRigidRouteReady(row.persistedMeshHash))
            return reject(RtPathTraceCommittedReferencedSetReject::RowReadyByMeshMismatch);
        if (row.expectedReadyByResident !=
                currentFacts.IsRigidRouteResidentReadyForEntityMaterial(
                    row.entityIndex, row.renderEntityNum,
                    row.persistedChosenMaterialId))
            return reject(RtPathTraceCommittedReferencedSetReject::RowReadyByResidentMismatch);
    }
    result.accepted = true;
    return result;
}
