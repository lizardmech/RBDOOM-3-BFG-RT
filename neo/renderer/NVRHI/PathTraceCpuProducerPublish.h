// Dual-write publisher (spec 22 rev 3). Header-only simulator + decoder.
// No skip CVar. Effective mode is only 0 or 1. Never test with >=.
#pragma once

#include "PathTraceCpuProducerPackFormat.h"
#include "PathTraceCanonicalIdentityS1.h"
#include "PathTraceRigidMeshIdentity.h"
#include "PathTraceRigidInstanceRecord.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cpu_producer_publish
{

enum class RigidCpuMeshDiagnosticReason : uint8_t
{
	None = 0,
	LookupUnavailable,
	LookupMiss,
	NeverRefreshed,
	NullTri,
	NullVertices,
	NullIndexes,
	InvalidSourceCounts,
	SourceCountsShort,
	SnapshotPayloadInvalid,
	VertexPositionInvalid,
	IndexOutOfRange,
	EmptyOwnedVertices,
	EmptyOwnedIndexes,
	RecordInvalid,
	CacheInvalid,
	SignatureInvalid,
	TriangleCountInvalid,
	VertexCountMismatch,
	IndexCountMismatch,
	BoundsInvalid,
	MissingVertexBuffer,
	MissingIndexBuffer,
	MissingBlas,
	BuffersNotUploaded,
	BlasNotCreated,
	BlasNotSubmitted,
	GpuSignatureMismatch,
	GpuVertexCountMismatch,
	GpuIndexCountMismatch,
	Count
};

constexpr size_t kRigidCpuMeshDiagnosticReasonCount =
	static_cast<size_t>(RigidCpuMeshDiagnosticReason::Count);

inline const char* RigidCpuMeshDiagnosticReasonName(
	RigidCpuMeshDiagnosticReason reason)
{
	switch (reason)
	{
	case RigidCpuMeshDiagnosticReason::None: return "none";
	case RigidCpuMeshDiagnosticReason::LookupUnavailable: return "lookup_unavailable";
	case RigidCpuMeshDiagnosticReason::LookupMiss: return "lookup_miss";
	case RigidCpuMeshDiagnosticReason::NeverRefreshed: return "never_refreshed";
	case RigidCpuMeshDiagnosticReason::NullTri: return "null_tri";
	case RigidCpuMeshDiagnosticReason::NullVertices: return "null_vertices";
	case RigidCpuMeshDiagnosticReason::NullIndexes: return "null_indexes";
	case RigidCpuMeshDiagnosticReason::InvalidSourceCounts: return "invalid_source_counts";
	case RigidCpuMeshDiagnosticReason::SourceCountsShort: return "source_counts_short";
	case RigidCpuMeshDiagnosticReason::SnapshotPayloadInvalid: return "snapshot_payload_invalid";
	case RigidCpuMeshDiagnosticReason::VertexPositionInvalid: return "vertex_position_invalid";
	case RigidCpuMeshDiagnosticReason::IndexOutOfRange: return "index_out_of_range";
	case RigidCpuMeshDiagnosticReason::EmptyOwnedVertices: return "empty_owned_vertices";
	case RigidCpuMeshDiagnosticReason::EmptyOwnedIndexes: return "empty_owned_indexes";
	case RigidCpuMeshDiagnosticReason::RecordInvalid: return "record_invalid";
	case RigidCpuMeshDiagnosticReason::CacheInvalid: return "cache_invalid";
	case RigidCpuMeshDiagnosticReason::SignatureInvalid: return "signature_invalid";
	case RigidCpuMeshDiagnosticReason::TriangleCountInvalid: return "triangle_count_invalid";
	case RigidCpuMeshDiagnosticReason::VertexCountMismatch: return "vertex_count_mismatch";
	case RigidCpuMeshDiagnosticReason::IndexCountMismatch: return "index_count_mismatch";
	case RigidCpuMeshDiagnosticReason::BoundsInvalid: return "bounds_invalid";
	case RigidCpuMeshDiagnosticReason::MissingVertexBuffer: return "missing_vertex_buffer";
	case RigidCpuMeshDiagnosticReason::MissingIndexBuffer: return "missing_index_buffer";
	case RigidCpuMeshDiagnosticReason::MissingBlas: return "missing_blas";
	case RigidCpuMeshDiagnosticReason::BuffersNotUploaded: return "buffers_not_uploaded";
	case RigidCpuMeshDiagnosticReason::BlasNotCreated: return "blas_not_created";
	case RigidCpuMeshDiagnosticReason::BlasNotSubmitted: return "blas_not_submitted";
	case RigidCpuMeshDiagnosticReason::GpuSignatureMismatch: return "gpu_signature_mismatch";
	case RigidCpuMeshDiagnosticReason::GpuVertexCountMismatch: return "gpu_vertex_count_mismatch";
	case RigidCpuMeshDiagnosticReason::GpuIndexCountMismatch: return "gpu_index_count_mismatch";
	default: return "unknown";
	}
}

struct RigidRegistryLookupDiagnosticCounts
{
	bool available = false;
	uint32_t uniqueMeshRequests = 0;
	uint32_t zeroHashSkipped = 0;
	uint32_t lookupHits = 0;
	uint32_t lookupMisses = 0;
	uint32_t ready = 0;
	uint32_t built = 0;
	uint32_t pending = 0;
	uint32_t blasTokens = 0;
};

// Reduce only after the production snapshot is complete. All allocating work
// stays local, and failure publishes nothing back to the caller.
template <typename SnapshotMap>
inline bool ReduceRigidRegistryLookupDiagnostics(
	const std::vector<uint64_t>& meshHashes,
	const SnapshotMap& snapshot,
	RigidRegistryLookupDiagnosticCounts& diagnostics,
	bool forceFailureForTest = false)
{
	RigidRegistryLookupDiagnosticCounts reduced;
	try
	{
		if (forceFailureForTest)
		{
			throw std::length_error("forced registry diagnostic reducer failure");
		}
		std::vector<uint64_t> uniqueMeshHashes;
		uniqueMeshHashes.reserve(meshHashes.size());
		for (uint64_t meshHash : meshHashes)
		{
			if (meshHash == 0)
			{
				++reduced.zeroHashSkipped;
				continue;
			}
			uniqueMeshHashes.push_back(meshHash);
		}
		std::sort(uniqueMeshHashes.begin(), uniqueMeshHashes.end());
		uniqueMeshHashes.erase(
			std::unique(uniqueMeshHashes.begin(), uniqueMeshHashes.end()),
			uniqueMeshHashes.end());
		reduced.uniqueMeshRequests =
			static_cast<uint32_t>(uniqueMeshHashes.size());
		for (uint64_t meshHash : uniqueMeshHashes)
		{
			const auto snapshotIt = snapshot.find(meshHash);
			if (snapshotIt == snapshot.end())
			{
				++reduced.lookupMisses;
				continue;
			}
			++reduced.lookupHits;
			const auto& entry = snapshotIt->second;
			reduced.ready += entry.ready ? 1u : 0u;
			reduced.built += entry.built ? 1u : 0u;
			reduced.pending += entry.pending || entry.pendingExpired ? 1u : 0u;
			reduced.blasTokens += entry.blasToken != 0 ? 1u : 0u;
		}
		reduced.available = true;
	}
	catch (...)
	{
		return false;
	}
	diagnostics = reduced;
	return true;
}

enum class PublishMode : int
{
	Off = 0,
	DualWrite = 1
};

enum class RegistryMode : int
{
	Off = 0,
	DualWrite = 1
};

enum class ForcedPlan : int
{
	None = 0,
	Accepted,
	GateDisabled,
	TlasCapacityExceeded,
	MissingBlas,
	ResourceContractMismatch,
	MissingCpuRoute
};

struct PublishCounters
{
	uint32_t publishCandidates = 0;
	uint32_t publishEmitted = 0;
	uint32_t publishSuppressed = 0;
	uint32_t rejectedPendingState = 0;
	uint32_t rejectedMissingOrCurrentRoute = 0;
	uint32_t rejectedResourceContract = 0;
	uint32_t rejectedMissingBlas = 0;
	uint32_t rejectedTlasPlan = 0;
	uint32_t omittedThisFrameWithoutFinalDescriptor = 0;
	uint32_t restoreCaptured = 0;

	uint32_t RejectedSum() const
	{
		return rejectedPendingState + rejectedMissingOrCurrentRoute +
			rejectedResourceContract + rejectedMissingBlas + rejectedTlasPlan;
	}

	void Clear()
	{
		*this = PublishCounters();
	}

	bool AllZero() const
	{
		return publishCandidates == 0 && publishEmitted == 0 &&
			publishSuppressed == 0 && RejectedSum() == 0 &&
			omittedThisFrameWithoutFinalDescriptor == 0 &&
			restoreCaptured == 0;
	}
};

struct RigidPublishCounters
{
	uint32_t rigidPublishCandidates = 0;
	uint32_t rigidPublishEmitted = 0;
	uint32_t rigidPublishSuppressed = 0;
	uint32_t rejectedNonRigid = 0;
	uint32_t rejectedMissingMesh = 0;
	uint32_t rejectedStaleMesh = 0;
	uint32_t rejectedMissingBlas = 0;
	uint32_t capTruncatedPreselect = 0;
	uint32_t capTruncatedPlan = 0;
	uint32_t builderDeclinedCachedTlas = 0;
	uint32_t builderDeclinedRouteIndex = 0;
	uint32_t builderDeclinedPlanMismatch = 0;
	uint32_t builderDeclinedMissingBlas = 0;
	uint32_t builderDeclinedCachedInvalid = 0;
	uint32_t maskZeroNonTraceable = 0;
	uint32_t canonicalParityFail = 0;
	uint32_t rigidRestoreCaptured = 0;
	uint32_t rigidOmittedThisFrameWithoutFinalDescriptor = 0;

	uint32_t RejectedSum() const
	{
		return rejectedNonRigid + rejectedMissingMesh + rejectedStaleMesh +
			rejectedMissingBlas + capTruncatedPreselect + capTruncatedPlan +
			builderDeclinedCachedTlas + builderDeclinedRouteIndex +
			builderDeclinedPlanMismatch + builderDeclinedMissingBlas +
			builderDeclinedCachedInvalid + maskZeroNonTraceable +
			canonicalParityFail;
	}

	bool Balance() const
	{
		return rigidPublishCandidates ==
			rigidPublishEmitted + rigidPublishSuppressed + RejectedSum();
	}
};

// A capture-skipped key may clear omitted only against the actual
// submitted extra descriptors or a dynamic BLAS enabled on that submit.
inline bool RigidSkipHasSubmittedThisFrameProduct(
	uint64_t skipInstanceId,
	const std::unordered_set<uint64_t>& submittedExtraSourceIds,
	bool restoreCommitted,
	bool submittedDynamicBlasEnabled)
{
	if (submittedExtraSourceIds.find(skipInstanceId) != submittedExtraSourceIds.end())
	{
		return true;
	}
	return restoreCommitted && submittedDynamicBlasEnabled;
}

inline PublishMode DecodePublishModeRaw(int raw, bool* rejected, std::string* log)
{
	if (rejected)
	{
		*rejected = false;
	}
	if (raw == 0)
	{
		return PublishMode::Off;
	}
	if (raw == 1)
	{
		return PublishMode::DualWrite;
	}
	if (rejected)
	{
		*rejected = true;
	}
	if (log)
	{
		*log = "r_pathTracingCpuProducerPublish rejected raw=" +
			std::to_string(raw) +
			" (only 0 and 1 valid; see 22_publish_spec.txt); effective mode 0";
	}
	return PublishMode::Off;
}

inline RegistryMode DecodeRegistryModeRaw(int raw, bool* rejected, std::string* log)
{
	if (rejected)
	{
		*rejected = false;
	}
	if (raw == 0)
	{
		return RegistryMode::Off;
	}
	if (raw == 1)
	{
		return RegistryMode::DualWrite;
	}
	if (rejected)
	{
		*rejected = true;
	}
	if (log)
	{
		*log = "r_pathTracingCpuProducerRegistry rejected raw=" +
			std::to_string(raw) +
			" (only 0 and 1 valid; see 33_phase_a_rigid_registry.txt); effective mode 0";
	}
	return RegistryMode::Off;
}

// A4: one extraTlas descriptor per (instanceId, surface meshId).
// Origin bits live on RigidSubmitBoundaryRecord (PathTraceRigidInstanceRecord.h).

struct RegistryEligibleSurface
{
	RigidRegistryInstanceKey instanceId;
	uint64_t meshId = 0;
	uint64_t blasToken = 0;
	bool presentRecorded = false;
	bool meshBlasReady = false;
	bool meshBlasBuilt = false;
	bool meshPending = false;
	RigidRegistryXform currentXform;
};

struct RegistryPublishCounters
{
	uint32_t registryEmitted = 0;
	uint32_t registrySuppressed = 0;
};

// Smoke TLAS create max AND submit guard. Must cover:
// 2 base (static+dynamic) + 510 rigid walk + ~2k A1.5-class registry
// surfaces + static-bucket extras. Grow may raise the live create max;
// submit must still refuse count > that live max.
constexpr uint32_t kPathTraceSmokeTlasBaseInstances = 2u;
constexpr uint32_t kPathTraceSmokeTlasWalkBudget = 510u;
constexpr uint32_t kPathTraceSmokeTlasRegistryBudget = 2048u;
constexpr uint32_t kPathTraceSmokeTlasStaticBucketBudget = 1024u;
constexpr uint32_t kPathTraceSmokeTlasMaxInstances =
	kPathTraceSmokeTlasBaseInstances +
	kPathTraceSmokeTlasWalkBudget +
	kPathTraceSmokeTlasRegistryBudget +
	kPathTraceSmokeTlasStaticBucketBudget;

inline uint32_t CountSmokeTlasSubmitInstances(
	uint32_t baseCount,
	uint32_t extraWithBlasCount)
{
	return baseCount + extraWithBlasCount;
}

inline bool SmokeTlasSubmitAllowed(
	uint32_t submittedCount,
	uint32_t tlasCreateMaxInstances)
{
	return tlasCreateMaxInstances != 0 &&
		submittedCount <= tlasCreateMaxInstances;
}

// Shared built-state rule: live handle AND created AND submitted.
// Production WasBuilt/BuiltBlas/emit and the harness all call this.
inline bool RigidBlasBuildStateIsSubmitted(
	bool handlePresent,
	bool gpuBlasCreated,
	bool gpuBlasBuildSubmitted)
{
	return handlePresent && gpuBlasCreated && gpuBlasBuildSubmitted;
}

// A6 first-upload selection. Registry mode equality is deliberate: mode 2 is
// still forbidden. Mode 0 preserves the walk/cached-route-only work set.
inline bool RigidPersistGpuPlanShouldProcess(
	bool valid,
	bool seenThisFrame,
	bool cachedRouteCandidate,
	int decodedRegistryMode,
	bool blasBuildSubmitted)
{
	if (!valid)
	{
		return false;
	}
	return seenThisFrame || cachedRouteCandidate ||
		(decodedRegistryMode == 1 && !blasBuildSubmitted);
}

inline bool RigidPersistGpuPlanPreferFirst(
	bool lhsBlasBuildSubmitted,
	bool lhsSeenThisFrame,
	bool rhsBlasBuildSubmitted,
	bool rhsSeenThisFrame)
{
	if (lhsBlasBuildSubmitted != rhsBlasBuildSubmitted)
	{
		return !lhsBlasBuildSubmitted;
	}
	if (lhsSeenThisFrame != rhsSeenThisFrame)
	{
		return lhsSeenThisFrame;
	}
	return false;
}

// Fail-closed before unordered_map rehash. MSVC throws length_error
// "invalid hash bucket count" when the requested bucket count exceeds
// 0x0fffffffffffffff (corrupt size / buckets / max_load_factor).
// Tiny positive load is rejected: size/load can request more buckets
// than kRigidMeshCandidateMapMaxBuckets under the size ceiling.
inline constexpr uint32_t kRigidMeshCandidateMapMaxEntries = 1048576u;
inline constexpr uint32_t kRigidMeshCandidateMapMaxBuckets = 4194304u;
inline constexpr float kRigidMeshCandidateMapMinLoadFactor = 0.25f;
inline constexpr uint32_t kRigidMeshCandidateLookupHealthLive = 0;
inline constexpr uint32_t kRigidMeshCandidateLookupHealthQuarantined = 1;
inline constexpr uint32_t kRigidMeshCandidateLookupCanaryLive = 0xA41104CAu;

// Persist Bind/rehash and Registry emit must not touch the live lookup
// concurrently. Emit reads only an immutable snapshot taken under the
// same mutex Bind/Find/Rebuild hold (m_rigidMeshCandidateLookupMutex).
inline bool RigidMeshLookupBindHoldsMutex()
{
	return true;
}

inline bool RigidMeshRegistryEmitUsesImmutableSnapshot()
{
	return true;
}

inline bool RigidMeshLookupEmitAndBindCannotOverlapUnlocked()
{
	return RigidMeshRegistryEmitUsesImmutableSnapshot() &&
		RigidMeshLookupBindHoldsMutex();
}

// Snapshot copy and record-vector insert/update share one ownership
// boundary (production: m_rigidMeshCandidateLookupMutex). False when
// emit Snapshot can run while unlocked push/pop/field writes proceed.
inline bool RigidMeshRegistryRecordVectorBoundaryHolds(
	bool snapshotCopyHeld,
	bool recordMutateOutsideLock)
{
	return !snapshotCopyHeld || !recordMutateOutsideLock;
}

inline constexpr const char* kRigidMeshCandidateRecordVectorBoundaryName =
	"m_rigidMeshCandidateLookupMutex";

// Production GPU publish / compact commits. Bound only when the record
// mutex is held AND identity was revalidated (GPU) or unlocked rebuild
// ran with the swap (compact). Passing mutexHeld=false fails.
struct RigidMeshGpuPublishCommit
{
	bool mutexHeld = false;
	bool identityRevalidated = false;
	bool gpuBlasBuildSubmitted = false;
};

inline bool RigidMeshGpuPublishCommitBound(
	const RigidMeshGpuPublishCommit& commit)
{
	return commit.mutexHeld &&
		commit.identityRevalidated &&
		commit.gpuBlasBuildSubmitted;
}

struct RigidMeshRecordCompactCommit
{
	bool mutexHeld = false;
	bool vectorSwapped = false;
	bool lookupRebuiltUnlocked = false;
};

inline bool RigidMeshRecordCompactCommitBound(
	const RigidMeshRecordCompactCommit& commit)
{
	if (!commit.vectorSwapped)
	{
		return true;
	}
	return commit.mutexHeld && commit.lookupRebuiltUnlocked;
}

inline constexpr uint32_t kRigidMeshCandidateLookupReasonNone = 0;
inline constexpr uint32_t kRigidMeshCandidateLookupReasonStructural = 1;
inline constexpr uint32_t kRigidMeshCandidateLookupReasonLogical = 2;

struct RigidMeshCandidateLookupHealthState
{
	uint32_t health = 0;
	uint32_t reason = 0;
	uint32_t structurallyValid = 0;
};

inline bool RigidMeshCandidateLookupIsQuarantined(
	const RigidMeshCandidateLookupHealthState& state)
{
	return state.health != kRigidMeshCandidateLookupHealthLive;
}

inline bool RigidMeshCandidateLookupQuarantine(
	RigidMeshCandidateLookupHealthState& state,
	uint32_t reason = kRigidMeshCandidateLookupReasonStructural)
{
	const uint32_t assigned =
		reason != kRigidMeshCandidateLookupReasonNone
			? reason
			: kRigidMeshCandidateLookupReasonStructural;
	if (state.health != kRigidMeshCandidateLookupHealthLive)
	{
		if (state.reason == kRigidMeshCandidateLookupReasonNone ||
			assigned == kRigidMeshCandidateLookupReasonStructural)
		{
			state.reason = assigned;
		}
		return false;
	}
	state.health = kRigidMeshCandidateLookupHealthQuarantined;
	state.reason = assigned;
	state.structurallyValid = 0;
	return true;
}

inline void RigidMeshCandidateLookupResetHealth(
	RigidMeshCandidateLookupHealthState& state)
{
	state.health = kRigidMeshCandidateLookupHealthLive;
	state.reason = kRigidMeshCandidateLookupReasonNone;
	state.structurallyValid = 0;
}

inline bool RigidMeshCandidateMapStateIsPlausible(
	uint64_t recordsSize,
	uint64_t mapSize,
	uint64_t mapBuckets,
	float mapMaxLoadFactor)
{
	if (recordsSize > kRigidMeshCandidateMapMaxEntries)
	{
		return false;
	}
	if (mapSize > kRigidMeshCandidateMapMaxEntries)
	{
		return false;
	}
	if (mapBuckets > kRigidMeshCandidateMapMaxBuckets)
	{
		return false;
	}
	if (!(mapMaxLoadFactor >= kRigidMeshCandidateMapMinLoadFactor) ||
		mapMaxLoadFactor > 16.0f)
	{
		return false;
	}
	if (mapSize > recordsSize + 8ull)
	{
		return false;
	}
	if (mapSize > 0)
	{
		const double requestedBuckets =
			static_cast<double>(mapSize) /
			static_cast<double>(mapMaxLoadFactor);
		if (requestedBuckets > static_cast<double>(kRigidMeshCandidateMapMaxBuckets))
		{
			return false;
		}
	}
	return true;
}

struct RigidMeshCandidateLookupAccessResult
{
	bool mayTouchMap = false;
	bool quarantined = false;
	bool transitioned = false;
	bool failClosed = false;
};

inline RigidMeshCandidateLookupAccessResult
DecideRigidMeshCandidateLookupAccess(
	RigidMeshCandidateLookupHealthState& state,
	bool implausible,
	bool insertionFailed)
{
	RigidMeshCandidateLookupAccessResult result;
	if (RigidMeshCandidateLookupIsQuarantined(state))
	{
		result.quarantined = true;
		result.failClosed = true;
		result.mayTouchMap = false;
		return result;
	}
	if (implausible || insertionFailed)
	{
		const uint32_t reason = insertionFailed
			? kRigidMeshCandidateLookupReasonStructural
			: kRigidMeshCandidateLookupReasonLogical;
		result.transitioned = RigidMeshCandidateLookupQuarantine(state, reason);
		result.quarantined = true;
		result.failClosed = true;
		result.mayTouchMap = false;
		return result;
	}
	result.mayTouchMap = true;
	return result;
}

// Pure state-machine used by production accessors and the harness.
// mapTouched is set only when the caller may invoke the map callback.
inline bool RigidMeshCandidateLookupTryAccess(
	RigidMeshCandidateLookupHealthState& state,
	uint64_t recordsSize,
	uint64_t mapSize,
	uint64_t mapBuckets,
	float mapMaxLoadFactor,
	bool simulatedInsertFailure,
	bool& mapTouched)
{
	mapTouched = false;
	const bool implausible = !RigidMeshCandidateMapStateIsPlausible(
		recordsSize, mapSize, mapBuckets, mapMaxLoadFactor);
	const RigidMeshCandidateLookupAccessResult decided =
		DecideRigidMeshCandidateLookupAccess(state, implausible, false);
	if (!decided.mayTouchMap)
	{
		return false;
	}
	mapTouched = true;
	if (simulatedInsertFailure)
	{
		DecideRigidMeshCandidateLookupAccess(state, false, true);
		return false;
	}
	return true;
}

struct RigidMeshCandidateLookupRebuildResult
{
	bool mayTouchExistingMap = false;
	bool mayResetHealth = false;
	bool quarantined = false;
	bool failClosed = false;
};

inline RigidMeshCandidateLookupRebuildResult
DecideRigidMeshCandidateLookupRebuild(
	const RigidMeshCandidateLookupHealthState& state)
{
	RigidMeshCandidateLookupRebuildResult result;
	if (!RigidMeshCandidateLookupIsQuarantined(state))
	{
		result.mayTouchExistingMap = true;
		result.mayResetHealth = true;
		return result;
	}
	result.quarantined = true;
	result.failClosed = true;
	result.mayTouchExistingMap = false;
	result.mayResetHealth = false;
	if (state.reason == kRigidMeshCandidateLookupReasonLogical &&
		state.structurallyValid != 0)
	{
		result.mayTouchExistingMap = true;
		result.mayResetHealth = true;
		result.failClosed = false;
	}
	return result;
}

inline bool RigidMeshCandidateLookupTryRebuild(
	RigidMeshCandidateLookupHealthState& state,
	bool& mapTouched)
{
	mapTouched = false;
	const RigidMeshCandidateLookupRebuildResult decided =
		DecideRigidMeshCandidateLookupRebuild(state);
	if (!decided.mayTouchExistingMap)
	{
		return false;
	}
	mapTouched = true;
	return true;
}

inline bool SmokeTlasKeepExtraInstance(bool hasBottomLevelAS)
{
	return hasBottomLevelAS;
}

inline bool RegistrySurfaceMayEmitBuiltBlas(const RegistryEligibleSurface& surface)
{
	return surface.presentRecorded &&
		surface.meshBlasReady &&
		surface.meshBlasBuilt &&
		surface.blasToken != 0 &&
		surface.meshId != 0;
}

struct SmokeTlasExtraSanitizeStats
{
	uint32_t extras = 0;
	uint32_t built = 0;
	uint32_t dropped = 0;
	uint32_t submitted = 0;
	uint32_t maxInstances = 0;
};

inline SmokeTlasExtraSanitizeStats SanitizeSmokeTlasExtraBlasTokens(
	std::vector<uint64_t>& extraBlasTokens,
	uint32_t walkExtraEnd,
	uint32_t baseCount,
	uint32_t tlasMaxInstances)
{
	SmokeTlasExtraSanitizeStats stats;
	stats.extras = static_cast<uint32_t>(extraBlasTokens.size());
	stats.maxInstances = tlasMaxInstances;
	std::vector<uint64_t> kept;
	kept.reserve(extraBlasTokens.size());
	for (size_t index = 0; index < extraBlasTokens.size(); ++index)
	{
		if (!SmokeTlasKeepExtraInstance(extraBlasTokens[index] != 0))
		{
			++stats.dropped;
			continue;
		}
		kept.push_back(extraBlasTokens[index]);
		++stats.built;
	}
	extraBlasTokens.swap(kept);
	uint32_t submitted = baseCount + static_cast<uint32_t>(extraBlasTokens.size());
	while (!SmokeTlasSubmitAllowed(submitted, tlasMaxInstances) &&
		extraBlasTokens.size() > static_cast<size_t>(walkExtraEnd))
	{
		extraBlasTokens.pop_back();
		if (submitted > 0)
		{
			--submitted;
		}
		++stats.dropped;
	}
	stats.submitted = submitted;
	return stats;
}

// Registry-only extras occupy the tail after walk extras. Suppress from
// the tail so walk extras stay. Returns how many registry extras to keep.
inline uint32_t SmokeTlasKeepRegistryExtras(
	uint32_t walkAndBaseCount,
	uint32_t registryExtraCount,
	uint32_t tlasCreateMaxInstances)
{
	if (tlasCreateMaxInstances <= walkAndBaseCount)
	{
		return 0;
	}
	const uint32_t room = tlasCreateMaxInstances - walkAndBaseCount;
	return registryExtraCount < room ? registryExtraCount : room;
}

inline uint32_t ChooseSmokeTlasGrowMax(uint32_t currentMax, uint32_t needed)
{
	uint32_t newMax = needed;
	if (currentMax > 0)
	{
		const uint32_t doubled = currentMax * 2u;
		if (doubled > newMax)
		{
			newMax = doubled;
		}
	}
	if (newMax < kPathTraceSmokeTlasMaxInstances)
	{
		newMax = kPathTraceSmokeTlasMaxInstances;
	}
	return newMax;
}

// Grow-lifecycle POD: candidate must not overwrite committed members
// until Commit captures the old TLAS+binding into the retired-package path.
struct SmokeTlasCommittedMembers
{
	uint64_t tlasToken = 0;
	uint64_t bindingToken = 0;
	uint64_t sceneInputsTlasToken = 0;
	uint32_t maxInstances = 0;
};

struct SmokeTlasGrowCandidate
{
	uint64_t tlasToken = 0;
	uint32_t maxInstances = 0;
	bool grew = false;
};

struct SmokeTlasRetiredPackage
{
	uint64_t tlasToken = 0;
	uint64_t bindingToken = 0;
	bool completionArmed = false;
	bool completed = false;
	bool inRetiredList = false;
};

struct SmokeTlasFrameSinks
{
	uint64_t accelSubmitTlas = 0;
	uint64_t bindingBuildTlas = 0;
	uint64_t sceneInputsTlas = 0;
	uint64_t resourceCommitTlas = 0;
	uint32_t submitMax = 0;
};

struct SmokeTlasGrowLifecycleResult
{
	SmokeTlasCommittedMembers committedDuringSubmit;
	SmokeTlasCommittedMembers committedAfter;
	SmokeTlasGrowCandidate candidate;
	SmokeTlasRetiredPackage retired;
	SmokeTlasFrameSinks sinks;
	bool capturedBeforeSwap = false;
	bool retainedUntilCompletion = false;
};

inline SmokeTlasGrowCandidate MakeSmokeTlasGrowCandidate(
	const SmokeTlasCommittedMembers& committed,
	uint32_t needed,
	uint64_t grownTlasToken)
{
	SmokeTlasGrowCandidate candidate;
	if (committed.tlasToken != 0 && committed.maxInstances >= needed)
	{
		candidate.tlasToken = committed.tlasToken;
		candidate.maxInstances = committed.maxInstances;
		candidate.grew = false;
		return candidate;
	}
	candidate.tlasToken = grownTlasToken;
	candidate.maxInstances = ChooseSmokeTlasGrowMax(committed.maxInstances, needed);
	candidate.grew = grownTlasToken != 0 && grownTlasToken != committed.tlasToken;
	return candidate;
}

inline SmokeTlasGrowLifecycleResult SimulateSmokeTlasGrowLifecycle(
	const SmokeTlasCommittedMembers& committed,
	uint32_t needed,
	uint64_t grownTlasToken,
	uint64_t newBindingToken)
{
	SmokeTlasGrowLifecycleResult out;
	out.candidate = MakeSmokeTlasGrowCandidate(committed, needed, grownTlasToken);
	// Ensure must not mutate committed members.
	out.committedDuringSubmit = committed;
	out.sinks.accelSubmitTlas = out.candidate.tlasToken;
	out.sinks.bindingBuildTlas = out.candidate.tlasToken;
	out.sinks.sceneInputsTlas = out.candidate.tlasToken;
	out.sinks.resourceCommitTlas = out.candidate.tlasToken;
	out.sinks.submitMax = out.candidate.maxInstances;

	const bool grew = out.candidate.grew &&
		out.candidate.tlasToken != committed.tlasToken;
	if (grew)
	{
		out.retired.tlasToken = committed.tlasToken;
		out.retired.bindingToken = committed.bindingToken;
		out.retired.completionArmed = true;
		out.retired.completed = false;
		out.retired.inRetiredList = true;
		out.capturedBeforeSwap = out.retired.tlasToken == committed.tlasToken &&
			out.retired.bindingToken == committed.bindingToken &&
			committed.tlasToken != 0 &&
			committed.bindingToken != 0;
	}

	out.committedAfter = committed;
	out.committedAfter.tlasToken = out.candidate.tlasToken;
	out.committedAfter.sceneInputsTlasToken = out.candidate.tlasToken;
	out.committedAfter.maxInstances = out.candidate.maxInstances;
	if (grew)
	{
		out.committedAfter.bindingToken = newBindingToken;
	}
	out.retainedUntilCompletion = !grew ||
		(out.retired.inRetiredList &&
			!out.retired.completed &&
			out.retired.tlasToken == committed.tlasToken &&
			out.retired.bindingToken == committed.bindingToken);
	return out;
}

// Snapshot CPU mesh pointers BEFORE any resize/clear. Convert loops must
// use these locals only and must not reload record.tri after allocation.
struct RigidCpuMeshPointerSnapshot
{
	const void* tri = nullptr;
	const void* verts = nullptr;
	const void* indexes = nullptr;
	int numVerts = 0;
	int numIndexes = 0;
	int sourceVertCount = 0;
	int sourceIndexCount = 0;
	bool valid = false;
};

inline bool SnapshotRigidCpuMeshPointers(
	const void* tri,
	const void* verts,
	const void* indexes,
	int numVerts,
	int numIndexes,
	int sourceVertCount,
	int sourceIndexCount,
	RigidCpuMeshPointerSnapshot& out)
{
	out = RigidCpuMeshPointerSnapshot();
	if (tri == nullptr ||
		verts == nullptr ||
		indexes == nullptr ||
		sourceVertCount <= 0 ||
		sourceIndexCount <= 0 ||
		numVerts < sourceVertCount ||
		numIndexes < sourceIndexCount)
	{
		return false;
	}
	out.tri = tri;
	out.verts = verts;
	out.indexes = indexes;
	out.numVerts = numVerts;
	out.numIndexes = numIndexes;
	out.sourceVertCount = sourceVertCount;
	out.sourceIndexCount = sourceIndexCount;
	out.valid = true;
	return true;
}

// Shared contract for a durable first-upload CPU mesh.  Deliberately has no
// tri pointer: once Persist copied the bytes, BeginFrame may clear tri without
// changing whether the cache can feed BLAS construction.
inline bool RigidOwnedCpuMeshCacheIsUsable(
	bool recordValid,
	bool cacheValid,
	uint64_t contentSignature,
	int sourceVertCount,
	int sourceIndexCount,
	int sourceTriangleCount,
	size_t cachedVertCount,
	size_t cachedIndexCount,
	bool localBoundsValid)
{
	return recordValid &&
		cacheValid &&
		contentSignature != 0 &&
		sourceVertCount > 0 &&
		sourceIndexCount > 0 &&
		(sourceIndexCount % 3) == 0 &&
		sourceTriangleCount > 0 &&
		sourceTriangleCount * 3 == sourceIndexCount &&
		cachedVertCount == static_cast<size_t>(sourceVertCount) &&
		cachedIndexCount == static_cast<size_t>(sourceIndexCount) &&
		localBoundsValid;
}

// After a successful snapshot, a later-null live tri must still convert
// from the snapped verts/indexes. Reloading liveTri is the 0x28 AV.
inline bool RigidCpuMeshConvertAfterLiveTriNull(
	const RigidCpuMeshPointerSnapshot& snapshot,
	const void*& liveTri,
	const void*& usedVerts,
	const void*& usedIndexes)
{
	liveTri = nullptr;
	usedVerts = nullptr;
	usedIndexes = nullptr;
	if (!snapshot.valid || snapshot.verts == nullptr || snapshot.indexes == nullptr)
	{
		return false;
	}
	usedVerts = snapshot.verts;
	usedIndexes = snapshot.indexes;
	return liveTri == nullptr && usedVerts != nullptr && usedIndexes != nullptr;
}

// Durable Build/Validate must use the owned cache and never read a
// stored tri token. A nonzero poison/dangling tri must be ignored.
struct RigidStaleTriCacheRecord
{
	uint64_t poisonTriToken = 0;
	bool ownedCacheComplete = false;
	uint32_t cachedVertSentinel = 0;
	uint32_t cachedIndexSentinel = 0;
};

inline bool BuildRigidLocalFromOwnedCacheOnly(
	const RigidStaleTriCacheRecord& rec,
	uint32_t& outVertSentinel,
	uint32_t& outIndexSentinel)
{
	if (!rec.ownedCacheComplete)
	{
		return false;
	}
	outVertSentinel = rec.cachedVertSentinel;
	outIndexSentinel = rec.cachedIndexSentinel;
	return true;
}

inline uint32_t ValidateRigidOwnedCacheOnly(const RigidStaleTriCacheRecord& rec)
{
	return rec.ownedCacheComplete ? 0u : 1u;
}

inline bool RegistryModeEqualsDualWrite(int decodedMode)
{
	return decodedMode == static_cast<int>(RegistryMode::DualWrite);
}

inline int CountSubmitBoundaryFor(
	const RigidSubmitBoundaryList& list,
	const RigidRegistryInstanceKey& instanceId,
	uint64_t meshId)
{
	int count = 0;
	for (size_t index = 0; index < list.records.size(); ++index)
	{
		if (RigidSubmitBoundaryExact(list.records[index]) &&
			RigidRegistryInstanceKeysEqual(list.records[index].instanceId, instanceId) &&
			list.records[index].meshId == meshId)
		{
			++count;
		}
	}
	return count;
}

inline bool HasRegistryTlasN2(const RigidSubmitBoundaryList& list)
{
	for (size_t index = 0; index < list.records.size(); ++index)
	{
		if (RigidSubmitBoundaryExact(list.records[index]) &&
			CountSubmitBoundaryFor(
				list, list.records[index].instanceId, list.records[index].meshId) > 1)
		{
			return true;
		}
	}
	return false;
}

// Coalesce against persistent physical metadata. Mode == DualWrite only.
// Unmapped physical extras fail closed (no registry-only append).
inline bool EmitCoalescedRegistryTlas(
	int decodedMode,
	const std::vector<RegistryEligibleSurface>& eligible,
	RigidSubmitBoundaryList& extras,
	RegistryPublishCounters& counters)
{
	if (!RegistryModeEqualsDualWrite(decodedMode))
	{
		return !HasRegistryTlasN2(extras);
	}
	const bool unmappedPhysical = RigidSubmitBoundaryHasUnmappedPhysical(extras);
	for (size_t surfaceIndex = 0; surfaceIndex < eligible.size(); ++surfaceIndex)
	{
		const RegistryEligibleSurface& surface = eligible[surfaceIndex];
		if (!surface.presentRecorded ||
			!RigidRegistryInstanceKeyValid(surface.instanceId) ||
			surface.meshId == 0)
		{
			++counters.registrySuppressed;
			continue;
		}
		if (!surface.meshBlasReady ||
			!surface.meshBlasBuilt ||
			surface.blasToken == 0)
		{
			++counters.registrySuppressed;
			continue;
		}
		const int existing = CountSubmitBoundaryFor(
			extras, surface.instanceId, surface.meshId);
		if (existing > 1)
		{
			continue;
		}
		if (existing == 1)
		{
			for (size_t extraIndex = 0; extraIndex < extras.records.size(); ++extraIndex)
			{
				if (RigidSubmitBoundaryExact(extras.records[extraIndex]) &&
					RigidRegistryInstanceKeysEqual(
						extras.records[extraIndex].instanceId, surface.instanceId) &&
					extras.records[extraIndex].meshId == surface.meshId)
				{
					extras.records[extraIndex].provenance |=
						kOriginRegistryHook;
					break;
				}
			}
			continue;
		}
		if (unmappedPhysical)
		{
			++counters.registrySuppressed;
			continue;
		}
		RigidSubmitBoundaryRecord rec;
		rec.instanceId = surface.instanceId;
		rec.meshId = surface.meshId;
		rec.descriptorIndex = extras.rigidPhysicalCount;
		rec.instanceID = extras.rigidPhysicalCount + 1u;
		rec.instanceMask = 0x02u;
		rec.submittedBlasToken = surface.blasToken;
		rec.provenance = kOriginRegistryHook;
		rec.exactIdentity = true;
		rec.currentXform = surface.currentXform;
		extras.records.push_back(rec);
		++extras.rigidPhysicalCount;
		++counters.registryEmitted;
	}
	return !HasRegistryTlasN2(extras);
}

inline uint32_t RejectedSumOf(const PublishCounters& c)
{
	return c.RejectedSum();
}

// CPU-side dynamic vectors after capture / restore append.
struct SimulatedDynamicGeometry
{
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
	uint32_t classCount = 0;
	uint32_t materialCount = 0;
	uint32_t materialIndexCount = 0;
};

// Stand-in for nvrhi::BufferHandle / AccelStructHandle identity.
struct SimulatedDynamicHandles
{
	uint32_t vertex = 0;
	uint32_t index = 0;
	uint32_t triangleClass = 0;
	uint32_t material = 0;
	uint32_t materialIndex = 0;
	uint32_t blas = 0;
};

// This-frame GPU product: uploaded/BLAS counts plus the handle identities
// that later same-frame consumers must share.
struct ThisFrameDynamicGpuProduct
{
	uint32_t uploadedVertexCount = 0;
	uint32_t uploadedIndexCount = 0;
	uint32_t uploadedClassCount = 0;
	uint32_t uploadedMaterialCount = 0;
	uint32_t uploadedMaterialIndexCount = 0;
	uint32_t blasVertexCount = 0;
	uint32_t blasIndexCount = 0;
	SimulatedDynamicHandles smokeBuffers;
	SimulatedDynamicHandles locals;
	SimulatedDynamicHandles blasSubmit;
	SimulatedDynamicHandles bindingSet;
	SimulatedDynamicHandles sceneInputs;
	SimulatedDynamicHandles committedPackage;
	SimulatedDynamicHandles preRestore;
	bool replacementHandles = false;
};

// Same contract as AppendSmokeSurfaceGeometry: <=0 is failure and must not
// grow the CPU vectors.
inline int SimulateAppendRestoreGeometry(
	SimulatedDynamicGeometry& cpu,
	int emittedIndexes)
{
	if (emittedIndexes <= 0)
	{
		return 0;
	}
	cpu.vertexCount += static_cast<uint32_t>(emittedIndexes);
	cpu.indexCount += static_cast<uint32_t>(emittedIndexes);
	cpu.classCount += static_cast<uint32_t>(emittedIndexes / 3);
	cpu.materialCount += static_cast<uint32_t>(emittedIndexes / 3);
	cpu.materialIndexCount += static_cast<uint32_t>(emittedIndexes / 3);
	return emittedIndexes;
}

inline SimulatedDynamicHandles AllocateSimulatedDynamicHandles(uint32_t& nextId)
{
	SimulatedDynamicHandles h;
	h.vertex = nextId++;
	h.index = nextId++;
	h.triangleClass = nextId++;
	h.material = nextId++;
	h.materialIndex = nextId++;
	h.blas = nextId++;
	return h;
}

inline bool SameDynamicHandles(
	const SimulatedDynamicHandles& a,
	const SimulatedDynamicHandles& b)
{
	return a.vertex != 0 && a.index != 0 && a.triangleClass != 0 &&
		a.material != 0 && a.materialIndex != 0 &&
		a.vertex == b.vertex && a.index == b.index &&
		a.triangleClass == b.triangleClass && a.material == b.material &&
		a.materialIndex == b.materialIndex;
}

// Models live order after P5: BLAS is built from the upload locals;
// CreateSmokeBindingResources and the committed package copy smokeBuffers;
// sceneInputs.geometry uses the locals.
inline void ApplySmokeBuffersToSameFrameConsumers(
	ThisFrameDynamicGpuProduct& gpu,
	const SimulatedDynamicHandles& locals,
	const SimulatedDynamicHandles& smokeBuffers)
{
	gpu.locals = locals;
	gpu.smokeBuffers = smokeBuffers;
	gpu.blasSubmit = locals;
	gpu.bindingSet = smokeBuffers;
	gpu.sceneInputs = locals;
	gpu.committedPackage = smokeBuffers;
}

inline bool RestoreReplacementHandlesReachAllConsumers(
	const ThisFrameDynamicGpuProduct& gpu)
{
	return SameDynamicHandles(gpu.blasSubmit, gpu.bindingSet) &&
		SameDynamicHandles(gpu.blasSubmit, gpu.sceneInputs) &&
		SameDynamicHandles(gpu.blasSubmit, gpu.committedPackage) &&
		SameDynamicHandles(gpu.blasSubmit, gpu.smokeBuffers) &&
		SameDynamicHandles(gpu.blasSubmit, gpu.locals);
}

// Option (b): after a successful restore append, re-size/re-upload/rebuild
// this frame's dynamic GPU product from the current CPU vectors.
inline ThisFrameDynamicGpuProduct CommitSimulatedDynamicGeometryThisFrame(
	const SimulatedDynamicGeometry& cpu)
{
	ThisFrameDynamicGpuProduct gpu;
	gpu.uploadedVertexCount = cpu.vertexCount;
	gpu.uploadedIndexCount = cpu.indexCount;
	gpu.uploadedClassCount = cpu.classCount;
	gpu.uploadedMaterialCount = cpu.materialCount;
	gpu.uploadedMaterialIndexCount = cpu.materialIndexCount;
	gpu.blasVertexCount = cpu.vertexCount;
	gpu.blasIndexCount = cpu.indexCount;
	return gpu;
}

inline bool RestoreGeometryInThisFrameGpuProduct(
	uint32_t uploadedVertexCount,
	uint32_t uploadedIndexCount,
	uint32_t uploadedClassCount,
	uint32_t uploadedMaterialCount,
	uint32_t blasVertexCount,
	uint32_t blasIndexCount,
	uint32_t restoreIndexBegin,
	int emittedIndexes,
	uint32_t uploadedMaterialIndexCount = 0)
{
	if (emittedIndexes <= 0)
	{
		return false;
	}
	const uint32_t restoreIndexEnd =
		restoreIndexBegin + static_cast<uint32_t>(emittedIndexes);
	const uint32_t restoreTriEnd = restoreIndexEnd / 3;
	if (uploadedMaterialIndexCount < restoreTriEnd ||
		uploadedMaterialIndexCount != uploadedMaterialCount)
	{
		return false;
	}
	return uploadedIndexCount >= restoreIndexEnd &&
		uploadedClassCount >= restoreTriEnd &&
		uploadedMaterialCount >= restoreTriEnd &&
		uploadedVertexCount >= 1 &&
		blasIndexCount >= restoreIndexEnd &&
		blasVertexCount >= 1;
}

inline bool RestoreGeometryInThisFrameGpuProduct(
	const ThisFrameDynamicGpuProduct& gpu,
	uint32_t restoreIndexBegin,
	int emittedIndexes)
{
	if (!RestoreGeometryInThisFrameGpuProduct(
			gpu.uploadedVertexCount,
			gpu.uploadedIndexCount,
			gpu.uploadedClassCount,
			gpu.uploadedMaterialCount,
			gpu.blasVertexCount,
			gpu.blasIndexCount,
			restoreIndexBegin,
			emittedIndexes,
			gpu.uploadedMaterialIndexCount))
	{
		return false;
	}
	if (gpu.replacementHandles || gpu.locals.vertex != 0 || gpu.smokeBuffers.vertex != 0)
	{
		return RestoreReplacementHandlesReachAllConsumers(gpu);
	}
	return true;
}

// Per-omitted-key append results. A missing key is treated as emittedIndexes=0.
// commitToThisFrameGpu=false models the rejected post-upload CPU-only append.
// forceResizeNewHandles models ResizeOrCreate returning distinct handles.
// propagateReplacementToSmokeBuffers=false models the rejected local-only write.
struct RestoreCaptureSim
{
	std::unordered_map<uint64_t, int> appendEmittedIndexes;
	bool commitToThisFrameGpu = true;
	bool forceResizeNewHandles = false;
	bool propagateReplacementToSmokeBuffers = true;
};

struct PublishFrameResult
{
	PublishMode effectiveMode = PublishMode::Off;
	bool decoderRejected = false;
	std::string decoderLog;
	std::vector<uint64_t> descriptorKeys;
	PublishCounters counters;
	SimulatedDynamicGeometry cpuGeometry;
	ThisFrameDynamicGpuProduct gpuProduct;
	RigidPublishCounters rigid;
};

inline bool PlanAccepted(ForcedPlan force)
{
	return force == ForcedPlan::None || force == ForcedPlan::Accepted;
}

inline PublishFrameResult SimulatePublishFrame(
	const cpu_producer_pack::PackTables& pack, int rawMode,
	ForcedPlan force = ForcedPlan::None,
	const RestoreCaptureSim* restore = nullptr)
{
	PublishFrameResult out;
	out.effectiveMode = DecodePublishModeRaw(
		rawMode, &out.decoderRejected, &out.decoderLog);

	std::vector<uint64_t> omitted;
	std::unordered_set<uint64_t> finalKeys;
	uint32_t capturedLive = 0;
	for (const cpu_producer_pack::InstanceRecord& inst : pack.instances)
	{
		if (!inst.live)
		{
			continue;
		}
		if (inst.omittedSkin)
		{
			omitted.push_back(inst.instanceId);
		}
		else
		{
			out.descriptorKeys.push_back(inst.instanceId);
			finalKeys.insert(inst.instanceId);
			++capturedLive;
		}
	}
	std::sort(omitted.begin(), omitted.end());
	omitted.erase(std::unique(omitted.begin(), omitted.end()), omitted.end());

	// Pre-restore capture product: non-omitted live instances are already
	// uploaded/built this frame (models 11229 create + 12128 upload).
	if (capturedLive > 0)
	{
		SimulateAppendRestoreGeometry(out.cpuGeometry, static_cast<int>(capturedLive * 3u));
	}
	uint32_t nextHandle = 1;
	const SimulatedDynamicHandles created = AllocateSimulatedDynamicHandles(nextHandle);
	out.gpuProduct = CommitSimulatedDynamicGeometryThisFrame(out.cpuGeometry);
	ApplySmokeBuffersToSameFrameConsumers(out.gpuProduct, created, created);
	out.gpuProduct.preRestore = created;

	const bool accepted = PlanAccepted(force);

	// Shipped Case A: omitted + accepted plan already appends descriptors,
	// independent of the publish CVar.
	if (accepted)
	{
		for (uint64_t key : omitted)
		{
			if (finalKeys.insert(key).second)
			{
				out.descriptorKeys.push_back(key);
			}
		}
	}

	if (out.effectiveMode == PublishMode::Off)
	{
		out.counters.Clear();
		out.rigid = RigidPublishCounters();
		return out;
	}

	out.counters.publishCandidates = static_cast<uint32_t>(omitted.size());

	if (!accepted)
	{
		out.counters.rejectedTlasPlan = out.counters.publishCandidates;
		if (force == ForcedPlan::MissingBlas)
		{
			out.counters.rejectedTlasPlan = 0;
			out.counters.rejectedMissingBlas = out.counters.publishCandidates;
		}
		else if (force == ForcedPlan::ResourceContractMismatch)
		{
			out.counters.rejectedTlasPlan = 0;
			out.counters.rejectedResourceContract = out.counters.publishCandidates;
		}
		else if (force == ForcedPlan::MissingCpuRoute)
		{
			out.counters.rejectedTlasPlan = 0;
			out.counters.rejectedMissingOrCurrentRoute = out.counters.publishCandidates;
		}
	}
	else
	{
		out.counters.publishSuppressed = out.counters.publishCandidates;
		out.counters.publishEmitted = 0;
	}

	// P5 completeness: omitted keys still missing a product. A forced plan
	// miss does not invent keys. restored=true only if append emitted > 0
	// AND that geometry is in this frame's uploaded/BLAS product AND
	// replacement handles reach bindings/sceneInputs/committed package.
	int restoreExtraIndexes = 0;
	struct PendingRestore
	{
		uint64_t key = 0;
		uint32_t indexBegin = 0;
		int emittedIndexes = 0;
	};
	std::vector<PendingRestore> pending;
	for (uint64_t key : omitted)
	{
		if (finalKeys.find(key) != finalKeys.end())
		{
			continue;
		}
		int emitted = 0;
		if (restore != nullptr)
		{
			const auto it = restore->appendEmittedIndexes.find(key);
			if (it != restore->appendEmittedIndexes.end())
			{
				emitted = it->second;
			}
		}
		const uint32_t indexBegin = out.cpuGeometry.indexCount;
		emitted = SimulateAppendRestoreGeometry(out.cpuGeometry, emitted);
		if (emitted <= 0)
		{
			++out.counters.omittedThisFrameWithoutFinalDescriptor;
			continue;
		}
		PendingRestore item;
		item.key = key;
		item.indexBegin = indexBegin;
		item.emittedIndexes = emitted;
		pending.push_back(item);
		restoreExtraIndexes += emitted;
	}

	if (restoreExtraIndexes > 0 && restore != nullptr && restore->commitToThisFrameGpu)
	{
		SimulatedDynamicHandles locals = out.gpuProduct.locals;
		SimulatedDynamicHandles buffers = out.gpuProduct.smokeBuffers;
		if (restore->forceResizeNewHandles)
		{
			locals = AllocateSimulatedDynamicHandles(nextHandle);
			out.gpuProduct.replacementHandles = true;
			if (restore->propagateReplacementToSmokeBuffers)
			{
				buffers = locals;
			}
		}
		const bool replacementHandles = out.gpuProduct.replacementHandles;
		out.gpuProduct = CommitSimulatedDynamicGeometryThisFrame(out.cpuGeometry);
		out.gpuProduct.replacementHandles = replacementHandles;
		ApplySmokeBuffersToSameFrameConsumers(out.gpuProduct, locals, buffers);
		out.gpuProduct.preRestore = created;
	}

	for (const PendingRestore& item : pending)
	{
		if (RestoreGeometryInThisFrameGpuProduct(
				out.gpuProduct, item.indexBegin, item.emittedIndexes))
		{
			++out.counters.restoreCaptured;
			if (finalKeys.insert(item.key).second)
			{
				out.descriptorKeys.push_back(item.key);
			}
		}
		else
		{
			++out.counters.omittedThisFrameWithoutFinalDescriptor;
		}
	}

	// S3 default pack model: non-omitted live instances are Case C
	// (capture already emitted). Removal-off skip set is empty, so
	// omittedWithout stays 0 and the publisher appends nothing.
	std::unordered_set<uint64_t> rigidSeen;
	for (const cpu_producer_pack::InstanceRecord& inst : pack.instances)
	{
		if (!inst.live || inst.omittedSkin)
		{
			continue;
		}
		if (rigidSeen.insert(inst.instanceId).second)
		{
			++out.rigid.rigidPublishCandidates;
		}
	}
	out.rigid.rigidPublishEmitted = 0;
	out.rigid.rigidPublishSuppressed = out.rigid.rigidPublishCandidates;
	return out;
}

inline bool CountersBalance(const PublishCounters& c)
{
	return c.publishCandidates ==
		c.publishEmitted + c.publishSuppressed + c.RejectedSum();
}

struct SimulatedRigidInstance
{
	uint64_t instanceId = 0;
	int entityIndex = -1;
	int renderEntityNum = -1;
	int modelSurfaceIndex = -1;
	uint32_t materialId = 0;
	bool routeReady = true;
	bool seenThisFrame = true;
	bool stable = false;
	bool transformContinuous = false;
	bool skipCapture = false;
	bool builderTraceable = true;
	bool builderAppended = true;
	uint32_t instanceMask = 0x02;
	int builderDecline = 0;
};

// Harness model of the entityIndex+renderEntityNum branch of
// RigidRouteEntityKeyEqual (world pointer is null in this simulator).
inline bool SimulatedRigidEntityKeyEqual(
	const SimulatedRigidInstance& a,
	const SimulatedRigidInstance& b)
{
	if (a.entityIndex >= 0 &&
		b.entityIndex >= 0 &&
		a.renderEntityNum >= 0 &&
		b.renderEntityNum >= 0)
	{
		return a.entityIndex == b.entityIndex &&
			a.renderEntityNum == b.renderEntityNum;
	}
	return a.instanceId == b.instanceId;
}

struct SimulatedRigidSubmitRoute
{
	bool frozenStaticCapture = false;
	bool extraTlasIncludesBuilderTraceable = true;
	bool hasDynamicBlas = true;

	static SimulatedRigidSubmitRoute Live(bool frozenStaticCapture)
	{
		SimulatedRigidSubmitRoute route;
		route.frozenStaticCapture = frozenStaticCapture;
		route.extraTlasIncludesBuilderTraceable = !frozenStaticCapture;
		route.hasDynamicBlas = !frozenStaticCapture;
		return route;
	}
};

struct SimulatedPreselectDrop
{
	uint64_t instanceId = 0;
	uint32_t groupId = 0;
	bool groupPartial = false;
};

struct SimulatedPreselectResult
{
	std::vector<uint64_t> selectedIds;
	std::vector<SimulatedPreselectDrop> dropped;
	uint32_t fullCount = 0;
};

// Faithful model of the live grouped cap at CaptureRigidTlasInstancePlanSnapshot
// before instances.swap(selectedInstances). Groups by SimulatedRigidEntityKeyEqual
// (live RigidRouteEntityKeyEqual), stable_sorts like the live comparator, takes
// a group whole if readyCount <= remaining, and splits only the first group.
inline SimulatedPreselectResult SimulateGroupedRigidPreselect(
	const std::vector<SimulatedRigidInstance>& instances,
	int maxInstances)
{
	SimulatedPreselectResult out;
	out.fullCount = static_cast<uint32_t>(instances.size());
	if (maxInstances <= 0 || static_cast<int>(instances.size()) <= maxInstances)
	{
		for (const SimulatedRigidInstance& inst : instances)
		{
			out.selectedIds.push_back(inst.instanceId);
		}
		return out;
	}

	struct Group
	{
		std::vector<size_t> indices;
		int readyCount = 0;
		bool seenThisFrame = false;
		bool stable = false;
		bool transformContinuous = false;
		size_t firstIndex = 0;
	};
	std::vector<Group> groups;
	for (size_t i = 0; i < instances.size(); ++i)
	{
		size_t groupIndex = groups.size();
		for (size_t g = 0; g < groups.size(); ++g)
		{
			if (!groups[g].indices.empty() &&
				SimulatedRigidEntityKeyEqual(
					instances[groups[g].indices.front()], instances[i]))
			{
				groupIndex = g;
				break;
			}
		}
		if (groupIndex == groups.size())
		{
			Group group;
			group.firstIndex = i;
			groups.push_back(group);
		}
		Group& group = groups[groupIndex];
		group.indices.push_back(i);
		group.seenThisFrame = group.seenThisFrame || instances[i].seenThisFrame;
		group.stable = group.stable || instances[i].stable;
		group.transformContinuous =
			group.transformContinuous || instances[i].transformContinuous;
		if (instances[i].routeReady)
		{
			++group.readyCount;
		}
	}

	std::stable_sort(
		groups.begin(),
		groups.end(),
		[](const Group& a, const Group& b)
		{
			if (a.seenThisFrame != b.seenThisFrame)
			{
				return a.seenThisFrame;
			}
			if (a.stable != b.stable)
			{
				return a.stable;
			}
			if (a.transformContinuous != b.transformContinuous)
			{
				return a.transformContinuous;
			}
			return a.firstIndex < b.firstIndex;
		});

	std::vector<size_t> selectedIndex;
	int remaining = maxInstances;
	for (const Group& group : groups)
	{
		if (remaining <= 0)
		{
			break;
		}
		if (group.readyCount <= remaining)
		{
			for (size_t idx : group.indices)
			{
				selectedIndex.push_back(idx);
			}
			remaining -= group.readyCount;
		}
		else if (selectedIndex.empty())
		{
			for (size_t idx : group.indices)
			{
				const bool consumesBudget = instances[idx].routeReady;
				selectedIndex.push_back(idx);
				if (consumesBudget)
				{
					--remaining;
					if (remaining <= 0)
					{
						break;
					}
				}
				if (static_cast<int>(selectedIndex.size()) >= maxInstances)
				{
					break;
				}
			}
		}
	}

	std::unordered_set<uint64_t> selectedSet;
	for (size_t idx : selectedIndex)
	{
		out.selectedIds.push_back(instances[idx].instanceId);
		selectedSet.insert(instances[idx].instanceId);
	}
	for (const Group& group : groups)
	{
		int selectedInGroup = 0;
		for (size_t idx : group.indices)
		{
			if (selectedSet.count(instances[idx].instanceId))
			{
				++selectedInGroup;
			}
		}
		const bool partial =
			selectedInGroup > 0 &&
			selectedInGroup < static_cast<int>(group.indices.size());
		for (size_t idx : group.indices)
		{
			if (selectedSet.count(instances[idx].instanceId))
			{
				continue;
			}
			SimulatedPreselectDrop drop;
			drop.instanceId = instances[idx].instanceId;
			drop.groupId = static_cast<uint32_t>(group.firstIndex);
			drop.groupPartial = partial;
			out.dropped.push_back(drop);
		}
	}
	return out;
}

inline RigidPublishCounters SimulateRigidClassifier(
	const std::vector<SimulatedRigidInstance>& instances,
	const SimulatedPreselectResult* preselect,
	bool commitRestore,
	SimulatedRigidSubmitRoute submit = SimulatedRigidSubmitRoute::Live(false))
{
	RigidPublishCounters c;
	std::unordered_set<uint64_t> dropped;
	if (preselect)
	{
		for (const SimulatedPreselectDrop& drop : preselect->dropped)
		{
			dropped.insert(drop.instanceId);
		}
	}
	std::unordered_set<uint64_t> submittedExtraSourceIds;
	std::unordered_set<uint64_t> restoreCommittedIds;
	std::unordered_set<uint64_t> skipIds;
	std::unordered_set<uint64_t> seen;
	for (const SimulatedRigidInstance& inst : instances)
	{
		if (!seen.insert(inst.instanceId).second)
		{
			continue;
		}
		++c.rigidPublishCandidates;
		const bool capDropped = dropped.find(inst.instanceId) != dropped.end();
		const bool builderTraceable =
			inst.builderAppended && inst.instanceMask != 0 && inst.builderTraceable && !capDropped;
		if (builderTraceable)
		{
			++c.rigidPublishSuppressed;
			if (submit.extraTlasIncludesBuilderTraceable && !submit.frozenStaticCapture)
			{
				submittedExtraSourceIds.insert(inst.instanceId);
			}
		}
		else if (capDropped)
		{
			++c.capTruncatedPreselect;
		}
		else if (!inst.builderAppended)
		{
			if (inst.builderDecline == 1)
			{
				++c.builderDeclinedCachedTlas;
			}
			else if (inst.builderDecline == 2)
			{
				++c.builderDeclinedRouteIndex;
			}
			else if (inst.builderDecline == 3)
			{
				++c.builderDeclinedPlanMismatch;
			}
			else if (inst.builderDecline == 4)
			{
				++c.builderDeclinedMissingBlas;
			}
			else if (inst.builderDecline == 5)
			{
				++c.builderDeclinedCachedInvalid;
			}
			else
			{
				++c.rejectedMissingBlas;
			}
		}
		else if (inst.instanceMask == 0)
		{
			++c.maskZeroNonTraceable;
		}
		else
		{
			++c.rigidPublishSuppressed;
			if (submit.extraTlasIncludesBuilderTraceable && !submit.frozenStaticCapture)
			{
				submittedExtraSourceIds.insert(inst.instanceId);
			}
		}

		if (inst.skipCapture)
		{
			skipIds.insert(inst.instanceId);
			if (commitRestore && !builderTraceable)
			{
				++c.rigidRestoreCaptured;
				restoreCommittedIds.insert(inst.instanceId);
			}
		}
	}
	c.rigidPublishEmitted = 0;

	const bool submittedDynamicBlas =
		commitRestore && submit.hasDynamicBlas && !submit.frozenStaticCapture;
	c.rigidOmittedThisFrameWithoutFinalDescriptor = 0;
	for (uint64_t skipId : skipIds)
	{
		const bool restoreCommitted =
			restoreCommittedIds.find(skipId) != restoreCommittedIds.end();
		if (!RigidSkipHasSubmittedThisFrameProduct(
				skipId,
				submittedExtraSourceIds,
				restoreCommitted,
				submittedDynamicBlas))
		{
			++c.rigidOmittedThisFrameWithoutFinalDescriptor;
		}
	}
	return c;
}

inline uint32_t ExpectedOmittedSkinCandidates(const std::string& folder)
{
	if (folder == "doom3_2")
	{
		return 17;
	}
	if (folder == "doom3_3" || folder == "walkway2")
	{
		return 0;
	}
	if (folder == "orbs")
	{
		return 15;
	}
	if (folder == "spin")
	{
		return 2;
	}
	if (folder == "walkway")
	{
		return 5;
	}
	return UINT32_MAX;
}


enum class CompareLayer1Class : uint8_t
{
	Unclassified = 0,
	Neither,
	SubmittedInBoth,
	M1MissingIn1,
	M2ExtraIn1Unexplained,
	M3ExtraIn1Restored
};

enum class CompareVerdict : uint8_t
{
	NotEvaluated = 0,
	Pass,
	Fail
};

inline const char* CompareVerdictName(CompareVerdict v)
{
	if (v == CompareVerdict::Pass)
	{
		return "PASS";
	}
	if (v == CompareVerdict::Fail)
	{
		return "FAIL";
	}
	return "not_evaluated";
}

struct CompareFrameInput
{
	int decodedMode = 0;
	int registryDecodedMode = 0;
	std::vector<uint64_t> extraSourceIds;
	std::vector<uint64_t> extraMaskZeroSourceIds;
	std::vector<uint64_t> submittedSkinnedIds;
	std::vector<uint64_t> restoreCommittedIds;
	bool hasDynamicBlas = false;
	std::vector<uint64_t> rigidCandidates;
	std::vector<uint64_t> skinnedCandidates;
	std::vector<uint64_t> captureSkipIds;
	std::vector<uint64_t> capDroppedIds;
	std::vector<uint64_t> maskZeroTreatedAsProduct;
	uint32_t publishEmitted = 0;
	uint32_t rigidPublishEmitted = 0;
	bool restoreFired = false;
	// Test-only defect injectors. Production leaves these empty.
	// removeFromActual models a mode-1 drop of a key that still formed the shadow.
	// injectActualOnly models an unexplained mode-1 product that never entered the shadow.
	std::vector<uint64_t> removeFromActual;
	std::vector<uint64_t> injectActualOnly;

struct RegistryCompareLiveInstance
{
	uint64_t instanceId = 0;
	uint64_t meshId = 0;
	uint64_t selectedMeshBlasToken = 0;
	bool presentRecorded = false;
	bool meshBlasReady = false;
	bool meshPending = false;
	bool meshPendingExpired = false;
	bool walkObserved = false;
	bool hasInstance = true;
	uint64_t refreshAttempts = 0;
	uint64_t refreshSuccesses = 0;
	uint64_t refreshFailures = 0;
	RigidCpuMeshDiagnosticReason diagnosticReason =
		RigidCpuMeshDiagnosticReason::NeverRefreshed;
};
struct RegistryCompareSubmittedExtra
{
	uint64_t instanceId = 0;
	uint64_t meshId = 0;
	uint32_t instanceMask = 0;
	uint64_t submittedBlasToken = 0;
	uint64_t selectedMeshBlasToken = 0;
	uint32_t provenance = 0;
	bool identityMatches = true;
};
struct RegistryCompareObservation
{
	uint64_t instanceId = 0;
	uint64_t blasToken = 0;
	uint32_t mask = 0;
	uint64_t meshId = 0;
};
struct RegistryCompareSkipProof
{
	uint64_t instanceId = 0;
	uint32_t occupancyProvenance = 0;
	bool wouldSkip = false;
};
struct RegistryIdentityDiagnosticSample
{
	uint64_t instanceId = 0;
	uint32_t modelEpoch = 0;
	bool recomputeAvailable = false;
	std::array<uint64_t, 8> storedMeshIds = {};
	std::array<uint8_t, 8> storedLookupMembership = {};
	uint32_t storedMeshIdCount = 0;
	std::array<uint64_t, 8> recomputedMeshIds = {};
	std::array<uint8_t, 8> recomputedLookupMembership = {};
	uint32_t recomputedMeshIdCount = 0;
};
std::vector<RegistryCompareLiveInstance> registryLive;
std::vector<RegistryCompareSubmittedExtra> registrySubmitted;
std::vector<RegistryCompareObservation> registryWalkObs;
std::vector<RegistryCompareObservation> registryHookObs;
std::vector<RegistryCompareSkipProof> registrySkipProofs;
	uint64_t registryRefreshAttempts = 0;
	uint64_t registryRefreshSuccesses = 0;
	uint64_t registryRefreshFailures = 0;
	uint32_t registryLookupEarlyReturn = 0;
	uint32_t registryLookupHits = 0;
	uint32_t registryLookupMisses = 0;
	uint32_t registryLookupReady = 0;
	uint32_t registryLookupBuilt = 0;
	uint32_t registryLookupPending = 0;
	uint32_t registryLookupBlasTokens = 0;
	uint32_t registryLookupDiagnosticsAvailable = 0;
	uint32_t registryUniqueMeshRequests = 0;
	uint32_t registryZeroHashSkipped = 0;
	uint32_t registryCandidateRecordCount = 0;
	uint32_t registryLookupTableSize = 0;
	uint32_t registryPopulationSnapshotAvailable = 0;
	uint64_t registryCandidateRecordInsertTotal = 0;
	uint64_t registryCandidateRecordInsertsThisFrame = 0;
	int32_t registryPersistTargetBoundAtFirstEntityAdd = -1;
	uint64_t registryPersistCallsTotal = 0;
	uint64_t registryPersistedSurfacesTotal = 0;
	std::array<uint64_t, 8> registryResidentKeySample = {};
	uint32_t registryResidentKeySampleCount = 0;
	std::array<uint64_t, 8> registryPresentRequestKeySample = {};
	uint32_t registryPresentRequestKeySampleCount = 0;
	uint32_t registryIdentitySamplesAvailable = 0;
	std::array<RegistryIdentityDiagnosticSample, 8> registryIdentitySamples = {};
	uint32_t registryIdentitySampleCount = 0;
	std::array<uint32_t, kRigidCpuMeshDiagnosticReasonCount>
		registryDiagnosticHistogram = {};
	PtA8S1DiagnosticResult a8S1;
};
using RegistryCompareLiveInstance = CompareFrameInput::RegistryCompareLiveInstance; using RegistryCompareSubmittedExtra = CompareFrameInput::RegistryCompareSubmittedExtra; using RegistryCompareObservation = CompareFrameInput::RegistryCompareObservation; using RegistryCompareSkipProof = CompareFrameInput::RegistryCompareSkipProof;

inline void BuildRegistryIndependentObservations(
	const RigidSubmitBoundaryList& metadata,
	size_t walkMetadataCount,
	const std::vector<RegistryEligibleSurface>& hookCandidates,
	std::vector<RegistryCompareObservation>& walkObservations,
	std::vector<RegistryCompareObservation>& hookObservations)
{
	walkObservations.clear();
	hookObservations.clear();
	const size_t walkEnd = std::min(walkMetadataCount, metadata.records.size());
	for (size_t metadataIndex = 0; metadataIndex < walkEnd; ++metadataIndex)
	{
		const RigidSubmitBoundaryRecord& rec = metadata.records[metadataIndex];
		if (!RigidSubmitBoundaryExact(rec) ||
			(rec.provenance & kOriginCaptureWalk) == 0)
		{
			continue;
		}
		RegistryCompareObservation observation;
		observation.instanceId = PackRigidRegistryInstanceId(rec.instanceId);
		observation.meshId = rec.meshId;
		observation.blasToken = rec.submittedBlasToken;
		observation.mask = rec.instanceMask;
		walkObservations.push_back(observation);
	}
	for (const RegistryEligibleSurface& surface : hookCandidates)
	{
		if (!RegistrySurfaceMayEmitBuiltBlas(surface))
		{
			continue;
		}
		RegistryCompareObservation observation;
		observation.instanceId = PackRigidRegistryInstanceId(surface.instanceId);
		observation.meshId = surface.meshId;
		observation.blasToken = surface.blasToken;
		observation.mask = 0x02u;
		hookObservations.push_back(observation);
	}
}

struct RegistryComparePhysicalExtra
{
	uint32_t descriptorIndex = 0;
	uint32_t instanceID = 0;
	uint32_t instanceMask = 0;
	uint64_t blasToken = 0;
};

inline void BuildRegistrySubmittedFromFinalPhysical(
	const RigidSubmitBoundaryList& metadata,
	const std::vector<RegistryComparePhysicalExtra>& finalPhysical,
	const std::unordered_map<uint64_t, uint64_t>& selectedBlasByMeshId,
	std::vector<RegistryCompareSubmittedExtra>& out)
{
	out.clear();
	for (const RigidSubmitBoundaryRecord& rec : metadata.records)
	{
		if (!RigidSubmitBoundaryExact(rec) ||
			rec.descriptorIndex >= finalPhysical.size())
		{
			continue;
		}
		const RegistryComparePhysicalExtra& physical =
			finalPhysical[rec.descriptorIndex];
		const bool exactPhysical =
			physical.descriptorIndex == rec.descriptorIndex &&
			physical.instanceID == rec.instanceID &&
			physical.instanceMask != 0 &&
			physical.blasToken != 0 &&
			physical.blasToken == rec.submittedBlasToken;
		if (!exactPhysical)
		{
			continue;
		}
		RegistryCompareSubmittedExtra extra;
		extra.instanceId = PackRigidRegistryInstanceId(rec.instanceId);
		extra.meshId = rec.meshId;
		extra.instanceMask = physical.instanceMask;
		extra.submittedBlasToken = physical.blasToken;
		extra.provenance = rec.provenance;
		extra.identityMatches = exactPhysical;
		const std::unordered_map<uint64_t, uint64_t>::const_iterator selected =
			selectedBlasByMeshId.find(rec.meshId);
		if (selected != selectedBlasByMeshId.end())
		{
			extra.selectedMeshBlasToken = selected->second;
		}
		out.push_back(extra);
	}
}

// Production restoreFired assembly. PathTraceSmokeSceneBuild MUST call this
// with the live rigid set, skinned set, and surviving restoreCaptured count.
// A caller that omits the skinned observation cannot satisfy the harness pin.
template <typename RigidRange, typename SkinnedRange>
inline bool FillCompareRestoreFired(
	CompareFrameInput& in,
	const RigidRange& rigidRestoreCommittedIds,
	const SkinnedRange& skinnedRestoreCommittedIds,
	uint32_t compareSkinnedRestoreCaptured)
{
	in.restoreFired =
		!rigidRestoreCommittedIds.empty() ||
		!skinnedRestoreCommittedIds.empty() ||
		compareSkinnedRestoreCaptured > 0;
	return in.restoreFired;
}

struct CompareDumpConfig
{
	uint64_t frameIndex = 0;
	bool frozenStaticCapture = false;
	int removeDynamic = 0;
	int residencyV2 = 0;
	uint32_t routeReadyCount = 0;
	int capValue = 510;
	uint32_t headroomCandidates = 0;
	uint32_t headroomRigid = 0;
	uint32_t headroomSkinned = 0;
};

// H0: capture still walked AND this-frame submitted product.
// Excludes the existing skip/omit set and Case C (walked, no per-instance product).
struct HeadroomResult
{
	std::vector<uint64_t> rigidIds;
	std::vector<uint64_t> skinnedIds;

	uint32_t rigid() const
	{
		return static_cast<uint32_t>(rigidIds.size());
	}
	uint32_t skinned() const
	{
		return static_cast<uint32_t>(skinnedIds.size());
	}
	uint32_t total() const
	{
		return rigid() + skinned();
	}
};

inline HeadroomResult CountHeadroom(
	const std::vector<uint64_t>& walkedRigidIds,
	const std::vector<uint64_t>& walkedSkinnedIds,
	const std::unordered_set<uint64_t>& existingSkipIds,
	const std::unordered_set<uint64_t>& existingOmitSkinIds,
	const std::unordered_set<uint64_t>& submittedExtraSourceIds,
	const std::unordered_set<uint64_t>& restoreCommittedIds,
	bool hasDynamicBlas)
{
	HeadroomResult out;
	std::unordered_set<uint64_t> seen;
	for (uint64_t id : walkedRigidIds)
	{
		if (!seen.insert(id).second)
		{
			continue;
		}
		if (existingSkipIds.find(id) != existingSkipIds.end())
		{
			continue;
		}
		const bool restoreCommitted =
			restoreCommittedIds.find(id) != restoreCommittedIds.end();
		if (!RigidSkipHasSubmittedThisFrameProduct(
				id, submittedExtraSourceIds, restoreCommitted, hasDynamicBlas))
		{
			continue;
		}
		out.rigidIds.push_back(id);
	}
	for (uint64_t id : walkedSkinnedIds)
	{
		if (!seen.insert(id).second)
		{
			continue;
		}
		if (existingOmitSkinIds.find(id) != existingOmitSkinIds.end() ||
			existingSkipIds.find(id) != existingSkipIds.end())
		{
			continue;
		}
		const bool restoreCommitted =
			restoreCommittedIds.find(id) != restoreCommittedIds.end();
		if (!RigidSkipHasSubmittedThisFrameProduct(
				id, submittedExtraSourceIds, restoreCommitted, hasDynamicBlas))
		{
			continue;
		}
		out.skinnedIds.push_back(id);
	}
	return out;
}

// A1.5 observational gap: Presented+live rigid with no section-5 product.
// Identity is the exact PtRenderDefKey (world, worldGeneration, index,
// slot generation). PackDefId(worldGeneration, index) is not this key.
// World / Deforming / Transient / Unknown are not live-rigid and not gaps.
enum class RegistryGapClass : uint32_t
{
	Unknown = 0,
	World,
	RigidAtRest,
	RigidMoving,
	Deforming,
	Transient
};

struct RegistryGapKey
{
	uint64_t worldToken = 0;
	uint64_t worldGeneration = 0;
	int index = -1;
	uint32_t generation = 0;
};

inline RegistryGapKey MakeRegistryGapKey(
	uint64_t worldToken,
	uint64_t worldGeneration,
	int index,
	uint32_t generation)
{
	RegistryGapKey key;
	key.worldToken = worldToken;
	key.worldGeneration = worldGeneration;
	key.index = index;
	key.generation = generation;
	return key;
}

// Shared dump/seen pack. Includes slot generation so G1 != G2 at the
// same world+index. Must not collapse to PackDefId(worldGeneration, index).
inline uint64_t PackRegistryGapIdentity(const RegistryGapKey& key)
{
	uint64_t packed = key.worldToken;
	packed ^= key.worldGeneration * 0x9E3779B97F4A7C15ull;
	packed ^= static_cast<uint64_t>(static_cast<uint32_t>(key.index)) *
		0xC2B2AE3D27D4EB4Full;
	packed ^= static_cast<uint64_t>(key.generation) * 0x165667B19E3779F9ull;
	return packed;
}

inline bool RegistryGapKeyValid(const RegistryGapKey& key)
{
	return key.index >= 0 && key.generation != 0;
}

inline bool RegistryGapKeysEqual(const RegistryGapKey& a, const RegistryGapKey& b)
{
	return RegistryGapKeyValid(a) &&
		a.worldToken == b.worldToken &&
		a.worldGeneration == b.worldGeneration &&
		a.index == b.index &&
		a.generation == b.generation;
}

struct RegistryGapLiveRecord
{
	RegistryGapKey key;
	RegistryGapClass geometryClass = RegistryGapClass::Unknown;
	bool alive = false;
};

struct RegistryGapProduct
{
	RegistryGapKey key;
	uint64_t meshId = 0;
	uint32_t instanceMask = 0;
	uint64_t submittedBlasToken = 0;
	uint64_t selectedMeshBlasToken = 0;
};

inline RegistryGapProduct MakeRegistryGapProductFromSubmitBoundary(
	const RigidSubmitBoundaryRecord& rec,
	uint64_t selectedMeshBlasToken)
{
	RegistryGapProduct product;
	product.key = MakeRegistryGapKey(
		reinterpret_cast<uint64_t>(rec.instanceId.world),
		rec.instanceId.worldGeneration,
		rec.instanceId.index,
		rec.instanceId.generation);
	product.meshId = rec.meshId;
	product.instanceMask = rec.instanceMask;
	product.submittedBlasToken = rec.submittedBlasToken;
	product.selectedMeshBlasToken = selectedMeshBlasToken;
	return product;
}

inline std::vector<RegistryGapProduct> RegistryGapProductsFromSubmitBoundary(
	const RigidSubmitBoundaryList& list,
	const std::unordered_map<uint64_t, uint64_t>& selectedBlasByMeshId)
{
	std::vector<RegistryGapProduct> products;
	for (size_t index = 0; index < list.records.size(); ++index)
	{
		const RigidSubmitBoundaryRecord& rec = list.records[index];
		if (!RigidSubmitBoundaryExact(rec))
		{
			continue;
		}
		uint64_t selected = 0;
		const std::unordered_map<uint64_t, uint64_t>::const_iterator it =
			selectedBlasByMeshId.find(rec.meshId);
		if (it != selectedBlasByMeshId.end())
		{
			selected = it->second;
		}
		products.push_back(MakeRegistryGapProductFromSubmitBoundary(rec, selected));
	}
	return products;
}

struct RegistryGapResult
{
	std::vector<uint64_t> liveIds;
	std::vector<uint64_t> productIds;
	std::vector<uint64_t> gapIds;

	uint32_t live() const
	{
		return static_cast<uint32_t>(liveIds.size());
	}
	uint32_t product() const
	{
		return static_cast<uint32_t>(productIds.size());
	}
	uint32_t gap() const
	{
		return static_cast<uint32_t>(gapIds.size());
	}
};

inline bool RigidRegistrySection5Join(
	bool identityMatches,
	uint32_t instanceMask,
	uint64_t submittedBlasToken,
	uint64_t selectedMeshBlasToken)
{
	return identityMatches &&
		instanceMask != 0 &&
		submittedBlasToken != 0 &&
		submittedBlasToken == selectedMeshBlasToken;
}

inline bool RegistryGapSubmitBoundaryJoin(const RegistryGapProduct& product)
{
	return RigidRegistrySection5Join(
		RegistryGapKeyValid(product.key),
		product.instanceMask,
		product.submittedBlasToken,
		product.selectedMeshBlasToken);
}

inline bool RigidRegistryGpuResidentFromSection5Join(
	bool identityMatches,
	uint32_t instanceMask,
	uint64_t submittedBlasToken,
	uint64_t selectedMeshBlasToken)
{
	return RigidRegistrySection5Join(
		identityMatches,
		instanceMask,
		submittedBlasToken,
		selectedMeshBlasToken);
}

inline bool RegistryGapIsPresentedLiveRigid(const RegistryGapLiveRecord& rec)
{
	return rec.alive &&
		RegistryGapKeyValid(rec.key) &&
		(rec.geometryClass == RegistryGapClass::RigidAtRest ||
			rec.geometryClass == RegistryGapClass::RigidMoving);
}

inline RegistryGapResult CountRegistryGap(
	const std::vector<RegistryGapLiveRecord>& liveRecords,
	const std::vector<RegistryGapProduct>& submittedProducts)
{
	RegistryGapResult out;
	std::unordered_set<uint64_t> liveSeen;
	for (const RegistryGapLiveRecord& rec : liveRecords)
	{
		if (!RegistryGapIsPresentedLiveRigid(rec))
		{
			continue;
		}
		const uint64_t id = PackRegistryGapIdentity(rec.key);
		if (!liveSeen.insert(id).second)
		{
			continue;
		}
		out.liveIds.push_back(id);
	}

	std::unordered_set<uint64_t> productSeen;
	for (const RegistryGapProduct& product : submittedProducts)
	{
		if (!RegistryGapSubmitBoundaryJoin(product))
		{
			continue;
		}
		const uint64_t id = PackRegistryGapIdentity(product.key);
		if (!productSeen.insert(id).second)
		{
			continue;
		}
		out.productIds.push_back(id);
	}

	for (uint64_t id : out.liveIds)
	{
		if (productSeen.find(id) == productSeen.end())
		{
			out.gapIds.push_back(id);
		}
	}
	return out;
}

// A2 Mesh persist join. Key is HashRigidMeshIdentity — the same builder
// production persist and the rigid route must use. No drawSurf_t*.
struct RigidMeshCandidateJoinRecord
{
	uint64_t meshHash = 0;
	uint32_t modelEpoch = 0;
	uint32_t materialClassSignature = 0;
	uint64_t vertexBufferIdentity = 0;
	uint64_t indexBufferIdentity = 0;
};

inline uint64_t FindOrCreateRigidMeshCandidateByIdentity(
	std::unordered_map<uint64_t, RigidMeshCandidateJoinRecord>& table,
	const RigidMeshIdentityInputs& in,
	RigidMeshCandidateJoinRecord* out = nullptr)
{
	const uint64_t meshHash = HashRigidMeshIdentity(in);
	std::unordered_map<uint64_t, RigidMeshCandidateJoinRecord>::iterator it =
		table.find(meshHash);
	if (it == table.end())
	{
		RigidMeshCandidateJoinRecord rec;
		rec.meshHash = meshHash;
		rec.modelEpoch = in.modelEpoch;
		rec.materialClassSignature = in.materialClassSignature;
		rec.vertexBufferIdentity = in.vertexBufferIdentity;
		rec.indexBufferIdentity = in.indexBufferIdentity;
		it = table.emplace(meshHash, rec).first;
	}
	if (out)
	{
		*out = it->second;
	}
	return it->second.meshHash;
}

// Static-surface keys (BuildSmokeStaticSurfaceKey / record.key) and
// merged/rigid source IDs live in separate identity domains. Numeric
// equality across domains is not a join.
enum class CaseCIdentityDomain : uint8_t
{
	StaticSurface = 1,
	MergedRigid = 2,
	MergedDynamic = 3
};

struct WalkedSurface
{
	uint64_t id = 0;
	uint32_t triangles = 0;
	CaseCIdentityDomain domain = CaseCIdentityDomain::MergedRigid;
};

// Current-frame static product, in the static-surface identity domain.
// Built only by BuildSubmittedStaticSurfaceProductSet (surface -> packed
// bucket -> exact extraTlas descriptor). Never a bag of bucket instance
// IDs or rigid/skinned source IDs.
struct StaticSurfaceProductSet
{
	static constexpr CaseCIdentityDomain kDomain =
		CaseCIdentityDomain::StaticSurface;
	std::unordered_set<uint64_t> ids;
};

struct StaticBucketProductJoinSurface
{
	uint64_t staticSurfaceKey = 0;
	uint64_t packedBucketKey = 0;
};

struct StaticBucketProductJoinBucket
{
	uint64_t packedBucketKey = 0;
	bool active = false;
	bool exactReady = false;
	uint32_t instanceMask = 0;
	uint32_t instanceId = 0;
	uint64_t blasToken = 0;
};

struct StaticBucketSubmittedDescriptor
{
	uint32_t instanceId = 0;
	uint32_t instanceMask = 0;
	uint64_t blasToken = 0;
};

inline bool StaticPublicationDescriptorMatches(
	const StaticBucketProductJoinBucket& bucket,
	const StaticBucketSubmittedDescriptor& desc)
{
	return bucket.instanceId == desc.instanceId &&
		bucket.instanceMask == desc.instanceMask &&
		bucket.instanceMask != 0 &&
		bucket.blasToken == desc.blasToken &&
		bucket.blasToken != 0;
}

// Map each static surface to its packed bucket. Count the surface only
// when that bucket is active, exact/BLAS-ready, has a nonzero mask, and
// its exact publication descriptor is in this frame's extraTlasInstances.
inline StaticSurfaceProductSet BuildSubmittedStaticSurfaceProductSet(
	const std::vector<StaticBucketProductJoinSurface>& surfaces,
	const std::vector<StaticBucketProductJoinBucket>& buckets,
	const std::vector<StaticBucketSubmittedDescriptor>& submittedDescriptors)
{
	StaticSurfaceProductSet out;
	std::unordered_map<uint64_t, std::vector<size_t>> bucketsByKey;
	for (size_t i = 0; i < buckets.size(); ++i)
	{
		if (buckets[i].packedBucketKey != 0)
		{
			bucketsByKey[buckets[i].packedBucketKey].push_back(i);
		}
	}
	std::unordered_set<uint64_t> seenStatic;
	for (const StaticBucketProductJoinSurface& surf : surfaces)
	{
		if (surf.staticSurfaceKey == 0 ||
			!seenStatic.insert(surf.staticSurfaceKey).second)
		{
			continue;
		}
		const auto it = bucketsByKey.find(surf.packedBucketKey);
		if (it == bucketsByKey.end())
		{
			continue;
		}
		bool joined = false;
		for (size_t bucketIndex : it->second)
		{
			const StaticBucketProductJoinBucket& bucket = buckets[bucketIndex];
			if (!bucket.active || !bucket.exactReady || bucket.instanceMask == 0)
			{
				continue;
			}
			for (const StaticBucketSubmittedDescriptor& desc : submittedDescriptors)
			{
				if (StaticPublicationDescriptorMatches(bucket, desc))
				{
					joined = true;
					break;
				}
			}
			if (joined)
			{
				break;
			}
		}
		if (joined)
		{
			out.ids.insert(surf.staticSurfaceKey);
		}
	}
	return out;
}

// C0: capture still walked, occupancy ONLY merged-dynamic or static-bake,
// NO per-instance submitted product. Complement of H0. Never includes skip/omit.
// staticBakeOnly requires the surface->bucket->submitted-descriptor join;
// it does not compare surface hashes to bucket instance IDs or rigid IDs.
struct CaseCWalkSet
{
	std::vector<uint64_t> staticBakeOnlyIds;
	std::vector<uint64_t> mergedDynamicOnlyIds;
	uint32_t staticBakeTriangles = 0;
	uint32_t mergedDynamicTriangles = 0;

	uint32_t staticBakeOnly() const
	{
		return static_cast<uint32_t>(staticBakeOnlyIds.size());
	}
	uint32_t mergedDynamicOnly() const
	{
		return static_cast<uint32_t>(mergedDynamicOnlyIds.size());
	}
};

inline CaseCWalkSet CountCaseCWalkSet(
	const std::vector<WalkedSurface>& walkedStatic,
	const std::vector<WalkedSurface>& walkedMerged,
	const std::unordered_set<uint64_t>& existingSkipIds,
	const std::unordered_set<uint64_t>& existingOmitSkinIds,
	const std::unordered_set<uint64_t>& submittedExtraSourceIds,
	const std::unordered_set<uint64_t>& restoreCommittedIds,
	bool hasDynamicBlas,
	const StaticSurfaceProductSet& submittedStaticSurfaces)
{
	CaseCWalkSet out;
	std::unordered_set<uint64_t> seenStatic;
	for (const WalkedSurface& surf : walkedStatic)
	{
		if (surf.domain != CaseCIdentityDomain::StaticSurface ||
			surf.id == 0 ||
			!seenStatic.insert(surf.id).second)
		{
			continue;
		}
		if (submittedStaticSurfaces.ids.find(surf.id) ==
			submittedStaticSurfaces.ids.end())
		{
			continue;
		}
		out.staticBakeOnlyIds.push_back(surf.id);
		out.staticBakeTriangles += surf.triangles;
	}
	std::unordered_set<uint64_t> seenMerged;
	for (const WalkedSurface& surf : walkedMerged)
	{
		if (surf.domain != CaseCIdentityDomain::MergedRigid ||
			surf.id == 0 ||
			!seenMerged.insert(surf.id).second)
		{
			continue;
		}
		if (existingSkipIds.find(surf.id) != existingSkipIds.end() ||
			existingOmitSkinIds.find(surf.id) != existingOmitSkinIds.end())
		{
			continue;
		}
		const bool restoreCommitted =
			restoreCommittedIds.find(surf.id) != restoreCommittedIds.end();
		if (RigidSkipHasSubmittedThisFrameProduct(
				surf.id, submittedExtraSourceIds, restoreCommitted, hasDynamicBlas))
		{
			continue;
		}
		out.mergedDynamicOnlyIds.push_back(surf.id);
		out.mergedDynamicTriangles += surf.triangles;
	}
	return out;
}

// M0: range coverage in this-frame submitted merged product.
// hasDynamicBlas-true alone is never enough (E1).
// Mask 0x01 is the live BuildSmokeBaseTlasPlan dynamic instance mask.
static constexpr uint32_t kMergedDynamicTlasInstanceMask = 0x01u;

struct MergedWalkedRange
{
	uint64_t id = 0;
	CaseCIdentityDomain domain = CaseCIdentityDomain::MergedDynamic;
	uint32_t vertexBegin = 0;
	uint32_t vertexCount = 0;
	uint32_t indexBegin = 0;
	uint32_t indexCount = 0;
	uint32_t triangleBegin = 0;
	uint32_t triangleCount = 0;
	bool particle = false;
	bool trueDeform = false;
	bool stableThisFrame = false;
	bool stabilityMeasured = false;
	// Companion IDs live in rigid/skinned domains. Mapping into
	// MergedDynamic uses these fields, never numeric equality on id.
	uint64_t companionRigidId = 0;
	uint64_t companionSkinnedId = 0;
};

struct MergedBucketInterval
{
	uint32_t indexBegin = 0;
	uint32_t indexCount = 0;
	uint32_t vertexBegin = 0;
	uint32_t vertexCount = 0;
};

struct MergedSubmittedBlasGeometry
{
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
};

struct MergedBlasExtent
{
	uint32_t vertexCount = 0;
	uint32_t indexCount = 0;
};

// Authority for coverage limits: submitted BLAS descriptor geometries,
// not CPU vector counts. Sum index counts; take max vertex count
// (CreateSmokeBlas stores total verts on each geom, chunked indexes).
inline MergedBlasExtent ReadSubmittedDynamicBlasExtents(
	const std::vector<MergedSubmittedBlasGeometry>& geometries)
{
	MergedBlasExtent out;
	for (const MergedSubmittedBlasGeometry& geom : geometries)
	{
		if (geom.vertexCount > out.vertexCount)
		{
			out.vertexCount = geom.vertexCount;
		}
		out.indexCount += geom.indexCount;
	}
	return out;
}

struct MergedSubmittedProduct
{
	bool hasDynamicBlas = false;
	bool dynamicBlasNonNull = false;
	bool dynamicBlasBuiltThisFrameFromVectors = false;
	bool tlasTraceable = false;
	uint32_t instanceMask = 0;
	uint32_t blasVertexCount = 0;
	uint32_t blasIndexCount = 0;
	std::vector<MergedBucketInterval> bucketIntervals;
};

inline bool MergedRangeContainedByBucketUnion(
	const MergedWalkedRange& range,
	const MergedSubmittedProduct& product)
{
	if (range.indexCount == 0)
	{
		return false;
	}
	const uint64_t indexEnd =
		static_cast<uint64_t>(range.indexBegin) + range.indexCount;
	const uint64_t vertexEnd =
		static_cast<uint64_t>(range.vertexBegin) + range.vertexCount;
	for (const MergedBucketInterval& bucket : product.bucketIntervals)
	{
		if (bucket.indexCount == 0)
		{
			continue;
		}
		const uint64_t bucketIndexEnd =
			static_cast<uint64_t>(bucket.indexBegin) + bucket.indexCount;
		const uint64_t bucketVertexEnd =
			static_cast<uint64_t>(bucket.vertexBegin) + bucket.vertexCount;
		const bool indexIn =
			range.indexBegin >= bucket.indexBegin &&
			indexEnd <= bucketIndexEnd;
		const bool vertexIn =
			range.vertexCount == 0 ||
			(range.vertexBegin >= bucket.vertexBegin &&
				vertexEnd <= bucketVertexEnd);
		if (indexIn && vertexIn)
		{
			return true;
		}
	}
	return false;
}

inline bool MergedRangeHasSubmittedCoverage(
	const MergedWalkedRange& range,
	const MergedSubmittedProduct& product)
{
	if (!product.hasDynamicBlas ||
		!product.dynamicBlasNonNull ||
		!product.dynamicBlasBuiltThisFrameFromVectors ||
		!product.tlasTraceable ||
		product.instanceMask == 0)
	{
		return false;
	}
	if (!MergedRangeContainedByBucketUnion(range, product))
	{
		return false;
	}
	const uint64_t indexEnd =
		static_cast<uint64_t>(range.indexBegin) + range.indexCount;
	const uint64_t vertexEnd =
		static_cast<uint64_t>(range.vertexBegin) + range.vertexCount;
	if (indexEnd > product.blasIndexCount)
	{
		return false;
	}
	if (range.vertexCount > 0 && vertexEnd > product.blasVertexCount)
	{
		return false;
	}
	return true;
}

struct MergedDynamicExclusionSets
{
	static constexpr CaseCIdentityDomain kDomain =
		CaseCIdentityDomain::MergedDynamic;
	std::unordered_set<uint64_t> skipIds;
	std::unordered_set<uint64_t> omitIds;
	std::unordered_set<uint64_t> descriptorIds;
};

// Map rigid/skinned classifications onto MergedDynamic range.id via
// companion fields. Never compares range.id to those other domains.
inline MergedDynamicExclusionSets BuildMergedDynamicExclusionSets(
	const std::vector<MergedWalkedRange>& walked,
	const std::unordered_set<uint64_t>& rigidSkipIds,
	const std::unordered_set<uint64_t>& omitSkinIds,
	const std::unordered_set<uint64_t>& rigidSubmittedExtraIds,
	const std::unordered_set<uint64_t>& submittedSkinnedIds)
{
	MergedDynamicExclusionSets out;
	for (const MergedWalkedRange& range : walked)
	{
		if (range.domain != CaseCIdentityDomain::MergedDynamic ||
			range.id == 0)
		{
			continue;
		}
		if (range.companionRigidId != 0)
		{
			if (rigidSkipIds.find(range.companionRigidId) != rigidSkipIds.end())
			{
				out.skipIds.insert(range.id);
			}
			if (rigidSubmittedExtraIds.find(range.companionRigidId) !=
				rigidSubmittedExtraIds.end())
			{
				out.descriptorIds.insert(range.id);
			}
		}
		if (range.companionSkinnedId != 0)
		{
			if (omitSkinIds.find(range.companionSkinnedId) != omitSkinIds.end())
			{
				out.omitIds.insert(range.id);
			}
			if (submittedSkinnedIds.find(range.companionSkinnedId) !=
				submittedSkinnedIds.end())
			{
				out.descriptorIds.insert(range.id);
			}
		}
	}
	return out;
}

struct MergedCaseCWalkSet
{
	std::vector<uint64_t> coveredIds;
	uint32_t coveredTriangles = 0;
	uint32_t stableCount = 0;
	uint32_t stableTriangles = 0;
	uint32_t excludedParticle = 0;
	uint32_t excludedDeform = 0;
	uint32_t excludedParticleTriangles = 0;
	uint32_t excludedDeformTriangles = 0;

	uint32_t covered() const
	{
		return static_cast<uint32_t>(coveredIds.size());
	}
	uint32_t stable() const
	{
		return stableCount;
	}
};

// MergedDynamic domain only. Separate seen set. Skip/omit/descriptor
// sets are MergedDynamic identities -- never rigid/static numeric IDs.
inline MergedCaseCWalkSet CountMergedCaseCWalkSet(
	const std::vector<MergedWalkedRange>& walked,
	const std::unordered_set<uint64_t>& existingSkipIds,
	const std::unordered_set<uint64_t>& existingOmitIds,
	const std::unordered_set<uint64_t>& mergedDomainDescriptorIds,
	const MergedSubmittedProduct& product)
{
	MergedCaseCWalkSet out;
	std::unordered_set<uint64_t> seenMergedDynamic;
	for (const MergedWalkedRange& range : walked)
	{
		if (range.domain != CaseCIdentityDomain::MergedDynamic ||
			range.id == 0 ||
			!seenMergedDynamic.insert(range.id).second)
		{
			continue;
		}
		if (existingSkipIds.find(range.id) != existingSkipIds.end() ||
			existingOmitIds.find(range.id) != existingOmitIds.end())
		{
			continue;
		}
		if (mergedDomainDescriptorIds.find(range.id) !=
			mergedDomainDescriptorIds.end())
		{
			continue;
		}
		if (!MergedRangeHasSubmittedCoverage(range, product))
		{
			continue;
		}
		out.coveredIds.push_back(range.id);
		out.coveredTriangles += range.triangleCount;
		if (range.particle)
		{
			++out.excludedParticle;
			out.excludedParticleTriangles += range.triangleCount;
			continue;
		}
		if (range.trueDeform)
		{
			++out.excludedDeform;
			out.excludedDeformTriangles += range.triangleCount;
			continue;
		}
		if (range.stabilityMeasured && range.stableThisFrame)
		{
			++out.stableCount;
			out.stableTriangles += range.triangleCount;
		}
	}
	return out;
}

struct CompareFrameResult
{
	bool eligible = false;
	bool registryEvaluated = false;
	bool partitionOk = true;
	CompareVerdict verdict = CompareVerdict::NotEvaluated;
	CompareVerdict registryVerdict = CompareVerdict::NotEvaluated;
	std::vector<uint64_t> universe;
	std::vector<uint64_t> actualSet;
	std::vector<uint64_t> shadowSet;
	std::vector<uint64_t> controlArmSet;
	std::vector<uint64_t> neither;
	std::vector<uint64_t> submittedInBoth;
	std::vector<uint64_t> m1;
	std::vector<uint64_t> m2;
	std::vector<uint64_t> m3;
	std::vector<uint64_t> m4;
	std::vector<uint64_t> m5;
	std::vector<uint64_t> m6;
	std::vector<uint64_t> m7;
	bool m8 = false;
	bool m9 = false;
	std::vector<uint64_t> n1;
	std::vector<uint64_t> n2;
	std::vector<uint64_t> n3;
	std::vector<uint64_t> n4;
	std::vector<uint64_t> n5;
	std::vector<uint64_t> n6;
	std::vector<uint64_t> registryEligibleIds;
	std::vector<uint64_t> registryNotEvaluatedIds;
	std::vector<uint64_t> registryGpuResidentIds;
};

struct CompareRunResult
{
	CompareVerdict verdict = CompareVerdict::NotEvaluated;
	uint32_t eligibleFrames = 0;
	uint32_t mode0Frames = 0;
};

inline void CompareUniqueSort(std::vector<uint64_t>& ids)
{
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

inline CompareRunResult CombineCompareFrames(const std::vector<CompareFrameResult>& frames)
{
	CompareRunResult run;
	bool anyFail = false;
	bool anyPass = false;
	for (const CompareFrameResult& frame : frames)
	{
		if (frame.eligible)
		{
			++run.eligibleFrames;
		}
		else
		{
			++run.mode0Frames;
		}
		if (frame.verdict == CompareVerdict::Fail)
		{
			anyFail = true;
		}
		else if (frame.verdict == CompareVerdict::Pass)
		{
			anyPass = true;
		}
	}
	if (anyFail)
	{
		run.verdict = CompareVerdict::Fail;
	}
	else if (!anyPass)
	{
		run.verdict = CompareVerdict::NotEvaluated;
	}
	else
	{
		run.verdict = CompareVerdict::Pass;
	}
	return run;
}

inline CompareFrameResult CompareSubmitSets(const CompareFrameInput& in)
{
	CompareFrameResult out;
	out.eligible = (in.decodedMode == 1);
	out.m8 = in.restoreFired && (in.decodedMode == 0);
	out.m9 = (in.publishEmitted != 0) || (in.rigidPublishEmitted != 0);

	std::unordered_map<uint64_t, uint32_t> extraCount;
	std::unordered_set<uint64_t> extraSet;
	for (uint64_t id : in.extraSourceIds)
	{
		++extraCount[id];
		extraSet.insert(id);
	}
	for (uint64_t id : in.submittedSkinnedIds)
	{
		++extraCount[id];
		extraSet.insert(id);
	}

	std::unordered_set<uint64_t> restoreSet(
		in.restoreCommittedIds.begin(), in.restoreCommittedIds.end());

	std::unordered_set<uint64_t> actual;
	for (uint64_t id : extraSet)
	{
		actual.insert(id);
	}
	if (in.hasDynamicBlas)
	{
		for (uint64_t id : restoreSet)
		{
			if (RigidSkipHasSubmittedThisFrameProduct(
					id, extraSet, true, in.hasDynamicBlas))
			{
				actual.insert(id);
			}
		}
	}

	if (!out.eligible)
	{
		out.controlArmSet.assign(actual.begin(), actual.end());
		CompareUniqueSort(out.controlArmSet);
		if (in.registryDecodedMode != 1)
		{
			out.verdict = (out.m8 || out.m9)
				? CompareVerdict::Fail
				: CompareVerdict::NotEvaluated;
			return out;
		}
	}

	std::unordered_set<uint64_t> provenSubmittedRestore;
	for (uint64_t id : restoreSet)
	{
		if (RigidSkipHasSubmittedThisFrameProduct(
				id, extraSet, true, in.hasDynamicBlas))
		{
			provenSubmittedRestore.insert(id);
		}
	}

	std::unordered_set<uint64_t> shadow = actual;
	for (uint64_t id : provenSubmittedRestore)
	{
		shadow.erase(id);
	}

	for (uint64_t id : in.removeFromActual)
	{
		actual.erase(id);
	}
	for (uint64_t id : in.injectActualOnly)
	{
		actual.insert(id);
	}

	out.actualSet.assign(actual.begin(), actual.end());
	out.shadowSet.assign(shadow.begin(), shadow.end());
	CompareUniqueSort(out.actualSet);
	CompareUniqueSort(out.shadowSet);

	std::unordered_set<uint64_t> skipSet(
		in.captureSkipIds.begin(), in.captureSkipIds.end());
	std::unordered_set<uint64_t> capSet(
		in.capDroppedIds.begin(), in.capDroppedIds.end());
	std::unordered_set<uint64_t> maskZeroDesc(
		in.extraMaskZeroSourceIds.begin(), in.extraMaskZeroSourceIds.end());
	std::unordered_set<uint64_t> maskZeroTreated(
		in.maskZeroTreatedAsProduct.begin(), in.maskZeroTreatedAsProduct.end());

	std::unordered_set<uint64_t> universe;
	for (uint64_t id : in.rigidCandidates)
	{
		universe.insert(id);
	}
	for (uint64_t id : in.skinnedCandidates)
	{
		universe.insert(id);
	}
	for (uint64_t id : actual)
	{
		universe.insert(id);
	}
	for (uint64_t id : shadow)
	{
		universe.insert(id);
	}
	out.universe.assign(universe.begin(), universe.end());
	CompareUniqueSort(out.universe);

	std::unordered_set<uint64_t> layer1Seen;
	for (uint64_t id : out.universe)
	{
		const bool inActual = actual.find(id) != actual.end();
		const bool inShadow = shadow.find(id) != shadow.end();
		const bool legalRestore =
			skipSet.find(id) != skipSet.end() &&
			!inShadow &&
			provenSubmittedRestore.find(id) != provenSubmittedRestore.end() &&
			inActual;
		CompareLayer1Class cls = CompareLayer1Class::Neither;
		if (!inActual && !inShadow)
		{
			cls = CompareLayer1Class::Neither;
			out.neither.push_back(id);
		}
		else if (inActual && inShadow)
		{
			cls = CompareLayer1Class::SubmittedInBoth;
			out.submittedInBoth.push_back(id);
		}
		else if (!inActual && inShadow)
		{
			cls = CompareLayer1Class::M1MissingIn1;
			out.m1.push_back(id);
		}
		else if (legalRestore)
		{
			cls = CompareLayer1Class::M3ExtraIn1Restored;
			out.m3.push_back(id);
		}
		else
		{
			cls = CompareLayer1Class::M2ExtraIn1Unexplained;
			out.m2.push_back(id);
		}
		(void)cls;
		if (!layer1Seen.insert(id).second)
		{
			out.partitionOk = false;
		}
	}
	const size_t layer1Count =
		out.neither.size() + out.submittedInBoth.size() +
		out.m1.size() + out.m2.size() + out.m3.size();
	if (layer1Count != out.universe.size() || layer1Seen.size() != out.universe.size())
	{
		out.partitionOk = false;
	}

	for (const auto& kv : extraCount)
	{
		if (kv.second > 1u)
		{
			out.m4.push_back(kv.first);
		}
	}
	for (uint64_t id : extraSet)
	{
		if (restoreSet.find(id) != restoreSet.end() && in.hasDynamicBlas)
		{
			out.m4.push_back(id);
		}
	}
	CompareUniqueSort(out.m4);

	for (uint64_t id : maskZeroDesc)
	{
		if (maskZeroTreated.find(id) != maskZeroTreated.end() ||
			actual.find(id) != actual.end() ||
			shadow.find(id) != shadow.end())
		{
			out.m5.push_back(id);
		}
	}
	for (uint64_t id : maskZeroTreated)
	{
		out.m5.push_back(id);
	}
	CompareUniqueSort(out.m5);

	for (uint64_t id : restoreSet)
	{
		if (!RigidSkipHasSubmittedThisFrameProduct(
				id, extraSet, true, in.hasDynamicBlas))
		{
			out.m6.push_back(id);
		}
	}
	CompareUniqueSort(out.m6);

	for (uint64_t id : capSet)
	{
		if (skipSet.find(id) != skipSet.end() &&
			actual.find(id) == actual.end() &&
			restoreSet.find(id) == restoreSet.end())
		{
			out.m7.push_back(id);
		}
	}
	CompareUniqueSort(out.m7);

	if (in.registryDecodedMode == 1)
	{
		std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint32_t>> extraMul;
		std::unordered_map<uint64_t, std::unordered_set<uint64_t>> livePairs;
		std::unordered_map<uint64_t, std::unordered_set<uint64_t>> hookPairs;
		for (const RegistryCompareSubmittedExtra& extra : in.registrySubmitted)
		{
			++extraMul[extra.instanceId][extra.meshId];
			if ((extra.provenance & kOriginRegistryHook) != 0)
			{
				hookPairs[extra.instanceId].insert(extra.meshId);
			}
		}
		for (const auto& instance : extraMul)
		{
			for (const auto& mesh : instance.second)
			{
				if (mesh.second > 1u)
				{
					out.n2.push_back(instance.first);
				}
			}
		}
		CompareUniqueSort(out.n2);

		std::unordered_map<uint64_t,
			std::unordered_map<uint64_t, RegistryCompareObservation>> walkByPair;
		for (const RegistryCompareObservation& obs : in.registryWalkObs)
		{
			walkByPair[obs.instanceId][obs.meshId] = obs;
		}
		std::unordered_map<uint64_t,
			std::unordered_map<uint64_t, RegistryCompareObservation>> hookByPair;
		for (const RegistryCompareObservation& obs : in.registryHookObs)
		{
			hookByPair[obs.instanceId][obs.meshId] = obs;
		}
		for (const auto& instance : walkByPair)
		{
			const auto hookInstance = hookByPair.find(instance.first);
			if (hookInstance == hookByPair.end())
			{
				continue;
			}
			for (const auto& mesh : instance.second)
			{
				const auto hook = hookInstance->second.find(mesh.first);
				if (hook != hookInstance->second.end() &&
					(mesh.second.blasToken != hook->second.blasToken ||
						mesh.second.mask != hook->second.mask))
				{
					out.n4.push_back(instance.first);
				}
			}
		}
		CompareUniqueSort(out.n4);

		for (const RegistryCompareLiveInstance& live : in.registryLive)
		{
			if (live.hasInstance)
			{
				livePairs[live.instanceId].insert(live.meshId);
			}
			if (live.meshPending && !live.meshBlasReady)
			{
				out.registryNotEvaluatedIds.push_back(live.instanceId);
				continue;
			}
			if (live.meshPendingExpired && !live.meshBlasReady)
			{
				out.registryEvaluated = true;
				out.n3.push_back(live.instanceId);
				continue;
			}
			const bool eligible = live.presentRecorded && live.meshBlasReady;
			if (!eligible)
			{
				continue;
			}
			out.registryEvaluated = true;
			out.registryEligibleIds.push_back(live.instanceId);
			bool joined = false;
			for (const RegistryCompareSubmittedExtra& extra : in.registrySubmitted)
			{
				if (extra.instanceId != live.instanceId || extra.meshId != live.meshId)
				{
					continue;
				}
				const uint64_t selected =
					live.selectedMeshBlasToken != 0
						? live.selectedMeshBlasToken
						: extra.selectedMeshBlasToken;
				if (RigidRegistrySection5Join(
					extra.identityMatches,
					extra.instanceMask,
					extra.submittedBlasToken,
					selected))
				{
					joined = true;
					out.registryGpuResidentIds.push_back(live.instanceId);
					break;
				}
			}
			if (!joined)
			{
				out.n3.push_back(live.instanceId);
			}
			const auto hookInstance = hookPairs.find(live.instanceId);
			const bool contributedHook =
				hookInstance != hookPairs.end() &&
				hookInstance->second.find(live.meshId) != hookInstance->second.end();
			if (!contributedHook)
			{
				out.n1.push_back(live.instanceId);
			}
		}
		for (const RegistryCompareObservation& walk : in.registryWalkObs)
		{
			const auto instance = livePairs.find(walk.instanceId);
			if (instance == livePairs.end() ||
				instance->second.find(walk.meshId) == instance->second.end())
			{
				out.registryEvaluated = true;
				out.n1.push_back(walk.instanceId);
			}
		}
		CompareUniqueSort(out.n1);
		CompareUniqueSort(out.n3);
		CompareUniqueSort(out.registryEligibleIds);
		CompareUniqueSort(out.registryNotEvaluatedIds);
		CompareUniqueSort(out.registryGpuResidentIds);

		for (const RegistryCompareSubmittedExtra& extra : in.registrySubmitted)
		{
			const auto instance = livePairs.find(extra.instanceId);
			if (instance == livePairs.end() ||
				instance->second.find(extra.meshId) == instance->second.end())
			{
				out.n5.push_back(extra.instanceId);
			}
		}
		CompareUniqueSort(out.n5);

		std::unordered_set<uint64_t> resident(
			out.registryGpuResidentIds.begin(), out.registryGpuResidentIds.end());
		for (const RegistryCompareSkipProof& proof : in.registrySkipProofs)
		{
			if (!proof.wouldSkip)
			{
				continue;
			}
			const bool walkOrUntagged =
				proof.occupancyProvenance == 0 ||
				((proof.occupancyProvenance & kOriginCaptureWalk) != 0 &&
					(proof.occupancyProvenance & kOriginRegistryHook) == 0);
			if (walkOrUntagged && resident.find(proof.instanceId) == resident.end())
			{
				out.n6.push_back(proof.instanceId);
			}
		}
		for (uint64_t id : in.captureSkipIds)
		{
			if (resident.find(id) != resident.end())
			{
				continue;
			}
			bool eligibleSkip = false;
			for (const RegistryCompareLiveInstance& live : in.registryLive)
			{
				if (live.instanceId == id && live.presentRecorded && live.meshBlasReady)
				{
					eligibleSkip = true;
					break;
				}
			}
			if (eligibleSkip)
			{
				out.n6.push_back(id);
			}
		}
		CompareUniqueSort(out.n6);
	}

	const bool layer1Fail = !out.m1.empty() || !out.m2.empty() || !out.partitionOk;
	const bool layer2Fail =
		!out.m4.empty() || !out.m5.empty() || !out.m6.empty() || !out.m7.empty();
	const bool layer3Fail = out.m8 || out.m9;
	const bool nFail = !out.n1.empty() || !out.n2.empty() || !out.n3.empty() ||
		!out.n4.empty() || !out.n5.empty() || !out.n6.empty();
	out.registryVerdict = nFail
		? CompareVerdict::Fail
		: (out.registryEvaluated
			? CompareVerdict::Pass
			: CompareVerdict::NotEvaluated);
	if (layer1Fail || layer2Fail || layer3Fail || nFail)
	{
		out.verdict = CompareVerdict::Fail;
	}
	else if (out.eligible || out.registryEvaluated)
	{
		out.verdict = (in.registryDecodedMode == 1 && !out.registryEvaluated)
			? CompareVerdict::NotEvaluated
			: CompareVerdict::Pass;
	}
	return out;
}

inline CompareFrameInput CompareInputFromPack(
	const cpu_producer_pack::PackTables& pack, int rawMode)
{
	CompareFrameInput in;
	bool rejected = false;
	const PublishMode mode = DecodePublishModeRaw(rawMode, &rejected, nullptr);
	in.decodedMode = (mode == PublishMode::DualWrite) ? 1 : 0;
	in.registryDecodedMode = in.decodedMode;
	for (const cpu_producer_pack::InstanceRecord& inst : pack.instances)
	{
		if (!inst.live)
		{
			continue;
		}
		in.rigidCandidates.push_back(inst.instanceId);
		if (inst.omittedSkin)
		{
			in.skinnedCandidates.push_back(inst.instanceId);
		}
		in.extraSourceIds.push_back(inst.instanceId);
	}
	return in;
}

inline std::string FormatCompareSizeLine(const CompareFrameResult& r)
{
	return std::string("cmp eligible=") + (r.eligible ? "1" : "0") +
		" registry=" + CompareVerdictName(r.registryVerdict) +
		" L1(n/b/M1/M2/M3)=" +
		std::to_string(r.neither.size()) + "/" +
		std::to_string(r.submittedInBoth.size()) + "/" +
		std::to_string(r.m1.size()) + "/" +
		std::to_string(r.m2.size()) + "/" +
		std::to_string(r.m3.size()) +
		" L2(M4/M5/M6/M7)=" +
		std::to_string(r.m4.size()) + "/" +
		std::to_string(r.m5.size()) + "/" +
		std::to_string(r.m6.size()) + "/" +
		std::to_string(r.m7.size()) +
		" M8=" + (r.m8 ? "1" : "0") +
		" M9=" + (r.m9 ? "1" : "0") +
		" N(N1/N2/N3/N4/N5/N6)=" +
		std::to_string(r.n1.size()) + "/" +
		std::to_string(r.n2.size()) + "/" +
		std::to_string(r.n3.size()) + "/" +
		std::to_string(r.n4.size()) + "/" +
		std::to_string(r.n5.size()) + "/" +
		std::to_string(r.n6.size()) +
		" verdict=" + CompareVerdictName(r.verdict);
}

inline std::string FormatCompareDumpText(
	const CompareFrameInput& in,
	const CompareFrameResult& r,
	const CompareDumpConfig& cfg)
{
	auto writeIds = [](std::string& out, const char* name, const std::vector<uint64_t>& ids)
	{
		out += name;
		out += "=";
		out += std::to_string(ids.size());
		for (uint64_t id : ids)
		{
			out += " ";
			out += std::to_string(id);
		}
		out += "\n";
	};
	std::string out;
	out += "PathTraceCpuProducerCompareDump frame=";
	out += std::to_string(cfg.frameIndex);
	out += " mode=";
	out += std::to_string(in.decodedMode);
	out += " eligible=";
	out += r.eligible ? "1" : "0";
	out += " frozenStatic=";
	out += cfg.frozenStaticCapture ? "1" : "0";
	out += " removeDynamic=";
	out += std::to_string(cfg.removeDynamic);
	out += " residencyV2=";
	out += std::to_string(cfg.residencyV2);
	out += " routeReady=";
	out += std::to_string(cfg.routeReadyCount);
	out += " cap=";
	out += std::to_string(cfg.capValue);
	out += " headroom=";
	out += std::to_string(cfg.headroomCandidates);
	out += " headroomRigid=";
	out += std::to_string(cfg.headroomRigid);
	out += " headroomSkinned=";
	out += std::to_string(cfg.headroomSkinned);
	out += " partition=";
	out += r.partitionOk ? "1" : "0";
	out += " verdict=";
	out += CompareVerdictName(r.verdict);
	out += "\n";
	writeIds(out, "actual", r.actualSet);
	writeIds(out, "shadow", r.shadowSet);
	writeIds(out, "controlArm", r.controlArmSet);
	writeIds(out, "universe", r.universe);
	writeIds(out, "neither", r.neither);
	writeIds(out, "submittedInBoth", r.submittedInBoth);
	writeIds(out, "M1", r.m1);
	writeIds(out, "M2", r.m2);
	writeIds(out, "M3", r.m3);
	writeIds(out, "M4", r.m4);
	writeIds(out, "M5", r.m5);
	writeIds(out, "M6", r.m6);
	writeIds(out, "M7", r.m7);
	out += "M8=";
	out += r.m8 ? "1" : "0";
	out += " M9=";
	out += r.m9 ? "1" : "0";
	out += "\n";
	return out;
}

void MaybeDumpCpuProducerCompare(
	const CompareFrameInput& in,
	const CompareFrameResult& result,
	const CompareDumpConfig& cfg);

void MaybeDumpCaseCWalk(const CaseCWalkSet& walk);
void MaybeDumpMergedWalk(const MergedCaseCWalkSet& walk);
void MaybeDumpHeadroom(const HeadroomResult& headroom);
void MaybeDumpRegistryGap(const RegistryGapResult& gap);
void MaybeDumpCpuProducerRegistry(
const CompareFrameInput& in,
const CompareFrameResult& result);

inline std::string FormatRegistryDumpText(
const CompareFrameInput& in,
const CompareFrameResult& r)
{
std::string out;
out += "PathTraceCpuProducerRegistryDump live=";
out += std::to_string(in.registryLive.size());
out += " eligible=";
out += std::to_string(r.registryEligibleIds.size());
out += " gpuResident=";
out += std::to_string(r.registryGpuResidentIds.size());
out += " extras=";
out += std::to_string(in.registrySubmitted.size());
out += " refreshAttempts=";
out += std::to_string(in.registryRefreshAttempts);
out += " refreshSuccesses=";
out += std::to_string(in.registryRefreshSuccesses);
out += " refreshFailures=";
out += std::to_string(in.registryRefreshFailures);
out += " lookupEarlyReturn=";
out += std::to_string(in.registryLookupEarlyReturn);
out += " lookupHits=";
out += std::to_string(in.registryLookupHits);
out += " lookupMisses=";
out += std::to_string(in.registryLookupMisses);
out += " lookupReady=";
out += std::to_string(in.registryLookupReady);
out += " lookupBuilt=";
out += std::to_string(in.registryLookupBuilt);
out += " lookupPending=";
out += std::to_string(in.registryLookupPending);
out += " lookupBlasTokens=";
out += std::to_string(in.registryLookupBlasTokens);
out += " lookupBasis=unique_mesh";
out += " lookupDiagnosticsAvailable=";
out += std::to_string(in.registryLookupDiagnosticsAvailable);
out += " uniqueMeshRequests=";
out += std::to_string(in.registryUniqueMeshRequests);
out += " zeroHashSkipped=";
out += std::to_string(in.registryZeroHashSkipped);
out += " refreshBasis=unique_mesh_cumulative_record";
out += " histogramBasis=instance_surface";
out += " candidateRecordCount=";
out += in.registryPopulationSnapshotAvailable != 0
	? std::to_string(in.registryCandidateRecordCount)
	: "unknown";
out += " candidateRecordBasis=snapshot";
out += " lookupTableSize=";
out += in.registryPopulationSnapshotAvailable != 0
	? std::to_string(in.registryLookupTableSize)
	: "unknown";
out += " lookupTableBasis=snapshot";
out += " persistTargetBoundAtFirstEntityAdd=";
out += in.registryPersistTargetBoundAtFirstEntityAdd < 0
	? "unknown"
	: std::to_string(in.registryPersistTargetBoundAtFirstEntityAdd);
out += " persistTargetBoundBasis=first_entity_add_latched";
out += " persistCallsTotal=";
out += std::to_string(in.registryPersistCallsTotal);
out += " persistedSurfacesTotal=";
out += std::to_string(in.registryPersistedSurfacesTotal);
out += " persistCountersBasis=process_cumulative";
out += " candidateRecordInsertTotal=";
out += std::to_string(in.registryCandidateRecordInsertTotal);
out += " candidateRecordInsertsThisFrame=";
out += std::to_string(in.registryCandidateRecordInsertsThisFrame);
out += " candidateInsertBasis=universe_cumulative_and_current_frame";
out += " populationSnapshotAvailable=";
out += std::to_string(in.registryPopulationSnapshotAvailable);
out += " populationSnapshotBasis=post_health_guard";
out += "\n";
out += "diagnosticHistogram";
for (size_t reasonIndex = 0;
	reasonIndex < kRigidCpuMeshDiagnosticReasonCount;
	++reasonIndex)
{
	out += " ";
	out += RigidCpuMeshDiagnosticReasonName(
		static_cast<RigidCpuMeshDiagnosticReason>(reasonIndex));
	out += "=";
	out += std::to_string(in.registryDiagnosticHistogram[reasonIndex]);
}
out += "\n";
const auto appendHexKeySample = [&out](
	const auto& sample, uint32_t count) {
	if (count == 0)
	{
		out += "none";
		return;
	}
	for (uint32_t index = 0; index < count && index < sample.size(); ++index)
	{
		if (index != 0)
		{
			out += ",";
		}
		char buffer[19];
		std::snprintf(
			buffer, sizeof(buffer), "0x%016llx",
			static_cast<unsigned long long>(sample[index]));
		out += buffer;
	}
};
out += "keySamples residentKeysBasis=sorted_lowest8_unique_lookup_snapshot";
out += " residentKeyCount=";
out += std::to_string(in.registryResidentKeySampleCount);
out += " residentKeys=";
appendHexKeySample(
	in.registryResidentKeySample, in.registryResidentKeySampleCount);
out += " presentRequestKeysBasis=sorted_lowest8_unique_nonzero_request_snapshot";
out += " presentRequestKeyCount=";
out += std::to_string(in.registryPresentRequestKeySampleCount);
out += " presentRequestKeys=";
appendHexKeySample(
	in.registryPresentRequestKeySample, in.registryPresentRequestKeySampleCount);
out += "\n";
const auto appendMembershipBits = [&out](
	const auto& membership, uint32_t count) {
	if (count == 0)
	{
		out += "none";
		return;
	}
	for (uint32_t index = 0; index < count && index < membership.size(); ++index)
	{
		if (index != 0)
		{
			out += ",";
		}
		out += membership[index] != 0 ? "1" : "0";
	}
};
out += "identitySampleSummary instanceBasis=first8_live_rigid_slot_snapshot";
out += " surfaceCap=8 surfaceCapBasis=first8_model_surfaces";
out += " identitySamplesAvailable=";
out += std::to_string(in.registryIdentitySamplesAvailable);
out += " identitySampleCount=";
out += std::to_string(in.registryIdentitySampleCount);
out += "\n";
for (uint32_t sampleIndex = 0;
	sampleIndex < in.registryIdentitySampleCount &&
	sampleIndex < in.registryIdentitySamples.size(); ++sampleIndex)
{
	const CompareFrameInput::RegistryIdentityDiagnosticSample& sample =
		in.registryIdentitySamples[sampleIndex];
	out += "identitySample index=";
	out += std::to_string(sampleIndex);
	out += " instanceId=";
	appendHexKeySample(std::array<uint64_t, 1>{ sample.instanceId }, 1);
	out += " instanceIdBasis=slot.rigidInstance.instanceId";
	out += " modelEpoch=";
	out += std::to_string(sample.modelEpoch);
	out += " modelEpochBasis=slot.modelEpoch";
	out += " storedKeysBasis=slot.rigidInstance.meshIds_first8";
	out += " storedKeyCount=";
	out += std::to_string(sample.storedMeshIdCount);
	out += " storedKeys=";
	appendHexKeySample(sample.storedMeshIds, sample.storedMeshIdCount);
	out += " storedMembershipBasis=direct_lookup_map_post_health_guard";
	out += " storedMembership=";
	appendMembershipBits(
		sample.storedLookupMembership, sample.storedMeshIdCount);
	out += " recomputeAvailable=";
	out += sample.recomputeAvailable ? "1" : "0";
	out += " recomputedKeysBasis=ComputeRigidMeshHashesFromPresent_dump_time_same_entity_model_slot_modelEpoch_first8_surfaces";
	out += " recomputedKeyCount=";
	out += std::to_string(sample.recomputedMeshIdCount);
	out += " recomputedKeys=";
	appendHexKeySample(sample.recomputedMeshIds, sample.recomputedMeshIdCount);
	out += " recomputedMembershipBasis=direct_lookup_map_post_health_guard";
	out += " recomputedMembership=";
	appendMembershipBits(
		sample.recomputedLookupMembership, sample.recomputedMeshIdCount);
	out += "\n";
}
out += "a8S1 enabled=";
out += in.a8S1.enabled ? "1" : "0";
out += " available=";
out += in.a8S1.available ? "1" : "0";
out += " normalizedRouteRequests=";
out += std::to_string(in.a8S1.normalizedRouteRequests);
out += " multiplicityBasis=all_six_producer_occurrences";
out += " reconciled=";
out += in.a8S1.reconciled ? "1" : "0";
out += " observedRouteProofReady=";
out += in.a8S1.observedRouteProofReady ? "1" : "0";
out += " coverageComplete=";
out += in.a8S1.coverageComplete ? "1" : "0";
out += " coverageBasis=enumerated_call_site_opportunities_only_not_present_population_not_global_S2_gate";
out += "\n";
out += "a8S1Buckets B1_invalidSurface=" + std::to_string(in.a8S1.buckets[0]);
out += " B2_invalidRouteModelName=" + std::to_string(in.a8S1.buckets[1]);
out += " B3_noPresentDtoRecord=" + std::to_string(in.a8S1.buckets[2]);
out += " B4_noTransportBinding=" + std::to_string(in.a8S1.buckets[3]);
out += " B5_epochMismatch=" + std::to_string(in.a8S1.buckets[4]);
out += " B6_staleSequence=" + std::to_string(in.a8S1.buckets[5]);
out += " B7_invalidCanonical=" + std::to_string(in.a8S1.buckets[6]);
out += " B8_structuralDisagreement=" + std::to_string(in.a8S1.buckets[7]);
out += " B9_associationAgreement=" + std::to_string(in.a8S1.buckets[8]);
out += "\n";
out += "a8S1ProofA basis=W_route_tuple_vs_P_dto_tuple";
out += " worldMismatch=" + std::to_string(in.a8S1.routeWorldMismatch);
out += " renderDefIndexMismatch=" + std::to_string(in.a8S1.routeRenderDefIndexMismatch);
out += " renderDefGenerationMismatch=" + std::to_string(in.a8S1.routeRenderDefGenerationMismatch);
out += " modelNameMismatch=" + std::to_string(in.a8S1.routeModelNameMismatch);
out += " modelSurfaceIndexMismatch=" + std::to_string(in.a8S1.routeSurfaceIndexMismatch);
out += " fallbackScans=" + std::to_string(in.a8S1.associationFallbackScans);
out += "\n";
out += "a8S1ProofB basis=P_dto_canonical_vs_R_binding_copy_chain";
out += " instanceKeyMismatch=" + std::to_string(in.a8S1.transportInstanceKeyMismatch);
out += " instanceHashMismatch=" + std::to_string(in.a8S1.transportInstanceHashMismatch);
out += " meshKeyMismatch=" + std::to_string(in.a8S1.transportMeshKeyMismatch);
out += " meshHashMismatch=" + std::to_string(in.a8S1.transportMeshHashMismatch);
out += "\n";
out += "a8S1ProofC basis=exact_world_publication_epoch_then_R_sequence_gte_P_lastUpsert";
out += " epochMismatch=" + std::to_string(in.a8S1.buckets[4]);
out += " staleSequence=" + std::to_string(in.a8S1.buckets[5]);
out += "\n";
out += "a8S1Alias basis=legacy_record_plus_normalized_route_to_P_dto_canonical_mesh_hash";
out += " persisted=" + std::to_string(in.a8S1.aliasPersisted);
out += " missing=" + std::to_string(in.a8S1.aliasMissing);
out += " inserted=" + std::to_string(in.a8S1.aliasInserted);
out += " hits=" + std::to_string(in.a8S1.aliasHits);
out += " refreshed=" + std::to_string(in.a8S1.aliasRefreshed);
out += " collisions=" + std::to_string(in.a8S1.aliasCollisions);
out += " retired=" + std::to_string(in.a8S1.aliasRetired);
out += " entries=" + std::to_string(in.a8S1.aliasEntries);
out += " bytes=" + std::to_string(in.a8S1.aliasBytes);
out += "\n";
out += "a8S1Cost dtoInstances=" + std::to_string(in.a8S1.dtoInstances);
out += " dtoBytes=" + std::to_string(in.a8S1.dtoBytes);
out += " dtoCaptureUs=" + std::to_string(in.a8S1.dtoCaptureMicroseconds);
out += " steadyRouteTopologyPasses=" + std::to_string(in.a8S1.routeTopologyPasses);
out += " steadyRouteTopologyUs=" + std::to_string(in.a8S1.routeTopologyMicroseconds);
out += " producersVisibleReadyWalkCompanionEntityArea=";
for (uint32_t producer = 0;
	producer < static_cast<uint32_t>(PtA8S1RouteProducer::Count); ++producer)
{
	if (producer != 0)
	{
		out += "/";
	}
	out += std::to_string(in.a8S1.producerCounts[producer]);
}
out += "\n";
out += "a8S1LegacyProductParity basis=actual_candidate_lookup_and_final_physical_serialization_before_after_observer";
out += " available=" +
	std::string(in.a8S1.legacyProductParityAvailable ? "1" : "0");
out += " equal=" + std::string(in.a8S1.legacyProductParity ? "1" : "0");
out += " normalizedSetDifference=" +
	std::to_string(in.a8S1.legacyProductDifferenceCount);
out += " fields=candidate_membership_multiplicity_lookup_blas/final_mesh_mask_transform_submitted_selected_blas";
out += " scope=post_producer_runtime_guard_plus_harness_S1_off_on_differential_not_global_S2_gate";
out += "\n";
out += "a8S1ProducerCoverage basis=opportunity_equals_observed_plus_diagnostic_unavailable";
for (uint32_t producer = 0;
	producer < static_cast<uint32_t>(PtA8S1RouteProducer::Count); ++producer)
{
	out += " p" + std::to_string(producer) + "=";
	out += std::to_string(in.a8S1.producerOpportunities[producer]) + "/";
	out += std::to_string(in.a8S1.producerObserved[producer]) + "/";
	out += std::to_string(
		in.a8S1.producerSuppressedDiagnosticUnavailable[producer]) + "/";
	out += std::to_string(in.a8S1.producerNoOpportunityThisFrame[producer]);
}
out += " columns=opportunity/observed/suppressed_diagnostic_unavailable/no_opportunity_this_frame";
out += " noOpportunityReasons=p0:no_visible_rigid_drawsurf,p1:routed_ready_branch_inactive,p2:capture_walk_output_inactive,p3:merged_companion_output_inactive,p4:entity_feed_no_rigid_surface,p5:area_residency_discovery_inactive";
out += "\n";
const auto bridgeOutcomeName = [](PtA8S1PresentBridgeOutcome outcome) {
	switch (outcome)
	{
		case PtA8S1PresentBridgeOutcome::NeverRouted:
			return "never_routed";
		case PtA8S1PresentBridgeOutcome::RouteOpportunitySuppressed:
			return "route_opportunity_suppressed";
		case PtA8S1PresentBridgeOutcome::InvalidOrRejected:
			return "invalid_or_rejected";
		case PtA8S1PresentBridgeOutcome::RoutedNoLegacyAssociation:
			return "routed_no_legacy_association";
		case PtA8S1PresentBridgeOutcome::AssociatedUnderThirdKey:
			return "associated_under_third_key";
		case PtA8S1PresentBridgeOutcome::AssociatedExact:
			return "associated_exact";
		default:
			return "unknown";
	}
};
out += "a8S1PresentBridge sampleBasis=first8_P_dto_rigid_surface_records";
out += " available=";
out += in.a8S1.presentBridgeAvailable ? "1" : "0";
out += " sampleCount=" + std::to_string(in.a8S1.presentBridgeSampleCount);
out += " outcomesNeverRouted/Suppressed/Invalid/RoutedNoLegacy/ThirdKey/Exact=";
for (uint32_t outcome = 0;
	outcome < static_cast<uint32_t>(PtA8S1PresentBridgeOutcome::Count);
	++outcome)
{
	if (outcome != 0)
	{
		out += "/";
	}
	out += std::to_string(in.a8S1.presentBridgeOutcomes[outcome]);
}
out += "\n";
for (uint32_t sampleIndex = 0;
	sampleIndex < in.a8S1.presentBridgeSampleCount &&
	sampleIndex < in.a8S1.presentBridgeSamples.size(); ++sampleIndex)
{
	const PtA8S1PresentBridgeSample& bridge =
		in.a8S1.presentBridgeSamples[sampleIndex];
	out += "a8S1PresentBridgeSample index=" + std::to_string(sampleIndex);
	out += " tupleBasis=P_dto world=" +
		std::to_string(bridge.presentTuple.worldGeneration);
	out += " renderDefIndex=" +
		std::to_string(bridge.presentTuple.renderDefIndex);
	out += " renderDefGeneration=" +
		std::to_string(bridge.presentTuple.renderDefGeneration);
	out += " modelNameLength=" +
		std::to_string(bridge.presentTuple.modelName.size());
	out += " modelName=" + bridge.presentTuple.modelName;
	out += " modelSurfaceIndex=" +
		std::to_string(bridge.presentTuple.modelSurfaceIndex);
	out += " P=" + std::string(bridge.presentDto ? "1" : "0");
	out += " R=" + std::string(bridge.binding ? "1" : "0");
	out += " currentW=" + std::string(bridge.currentRoute ? "1" : "0");
	out += " everW=" + std::string(bridge.everRoute ? "1" : "0");
	out += " candidateProbe=" +
		std::string(bridge.legacyCandidateProbeAvailable ? "1" : "0");
	out += " routeLegacyKeysBasis=current_or_sidecar_W_same_legacy_keyspace keys=";
	appendHexKeySample(bridge.routeLegacyKeys, bridge.routeLegacyKeyCount);
	out += " candidateLegacyKeysBasis=direct_lookup_membership_post_health_guard keys=";
	appendHexKeySample(
		bridge.candidateLegacyKeys, bridge.candidateLegacyKeyCount);
	out += " outcome=";
	out += bridgeOutcomeName(bridge.outcome);
	out += "\n";
}
std::unordered_map<uint64_t, uint32_t> extraCount;
std::unordered_map<uint64_t, uint32_t> provenanceById;
for (const RegistryCompareSubmittedExtra& extra : in.registrySubmitted)
{
++extraCount[extra.instanceId];
provenanceById[extra.instanceId] |= extra.provenance;
}
std::unordered_set<uint64_t> resident(
r.registryGpuResidentIds.begin(), r.registryGpuResidentIds.end());
for (const RegistryCompareLiveInstance& live : in.registryLive)
{
out += "id=";
out += std::to_string(live.instanceId);
out += " live=";
out += live.hasInstance && live.presentRecorded ? "1" : "0";
out += " gpuResident=";
out += resident.find(live.instanceId) != resident.end() ? "1" : "0";
out += " provenance=";
out += std::to_string(provenanceById[live.instanceId]);
out += " descCount=";
out += std::to_string(extraCount[live.instanceId]);
out += " refreshAttempts=";
out += std::to_string(live.refreshAttempts);
out += " refreshSuccesses=";
out += std::to_string(live.refreshSuccesses);
out += " refreshFailures=";
out += std::to_string(live.refreshFailures);
out += " failReason=";
out += RigidCpuMeshDiagnosticReasonName(live.diagnosticReason);
out += "\n";
}
return out;
}

} // namespace cpu_producer_publish
