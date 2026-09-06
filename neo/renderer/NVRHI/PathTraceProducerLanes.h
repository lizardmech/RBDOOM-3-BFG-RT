#pragma once

#if defined(RT_PT_PRODUCER_LANES_HARNESS)
#include "PathTraceUniversePlanningSnapshot.h"
#else
#include "PathTraceAccelCpuPack.h"
#include "PathTraceCaptureProduct.h"
#endif
#include "PathTraceProducerLaneContract.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(RT_PT_PRODUCER_LANES_HARNESS)
constexpr std::size_t RT_PT_ACCEL_CPU_SLOT_MAX_BYTES = 64u * 1024u * 1024u;
// Dependency-light stand-ins let the CPU harness execute the real host state
// machine without linking renderer capture semantics.  The host only consumes
// this common lifecycle surface; production uses the full CaptureProduct types.
struct RtPathTraceLateConsumeToken
{
    std::uint64_t mapTimeStamp = 0;
    std::uint64_t mapLoadSerial = 0;
    char mapName[RT_PT_PLANNING_MAP_NAME_CAPACITY] = {};
    std::uint64_t registryGeneration = 0;
    std::uint64_t instanceUniverseGeneration = 0;
    std::uint64_t geometryUniverseGeneration = 0;
    std::uint64_t configFingerprint = 0;
    bool capturedAfterBeginFrame = false;
    bool capturedAfterStaticPreload = false;
};

struct RtPathTraceCaptureMembershipReceipt
{
    std::uint64_t hash = 0;
    std::uint32_t surfaceCount = 0;
    std::uint32_t skinnedSurfaceCount = 0;
    std::uint32_t cpuSkinnedAcceptedCount = 0;
    bool complete = false;
};

struct RtPathTraceProducerHarnessInstanceSnapshot
{
    std::uint64_t historyRows = 0;
    std::uint64_t historyBytes = 0;
    std::uint64_t historyCopyUs = 0;
};

struct RtPathTraceCaptureOwnerSnapshot
{
    RtPathTracePlanningSnapshotEpoch epoch;
    RtPathTraceLateConsumeToken lateConsumeToken;
    RtPathTraceCaptureMembershipReceipt buildMembershipReceipt;
    std::uint64_t viewIdentity = 0;
    RtPathTraceProducerHarnessInstanceSnapshot instanceUniverse;
    std::size_t ownedBytes = 0;
    std::uint32_t buildDelayUs = 0;
    std::uint32_t buildFailureKind = 0;
    RtPathTraceCompleteSlotCardinality capacityCardinality;
    RtPathTraceCompleteSlotLayout capacityLayout;
    bool complete = false;
    bool buildSucceeds = true;
    bool buildExact = true;
    std::uint64_t buildAcceptedSurfaces = 0;
    std::uint64_t buildAcceptedVertices = 0;
    RtPathTraceCaptureAcceptedSetDiff buildAcceptedSetDiff;

    void ResetAndRelease() { *this = {}; }
    std::size_t OwnedBytes() const { return ownedBytes; }
};

struct RtPathTraceCaptureProduct
{
    RtPathTracePlanningSnapshotEpoch epoch;
    RtPathTraceLateConsumeToken lateConsumeToken;
    RtPathTraceCaptureMembershipReceipt membershipReceipt;
    std::uint64_t viewIdentity = 0;
    std::size_t ownedBytes = 0;
    bool complete = false;
    bool exact = true;
    std::uint64_t acceptedSurfaces = 0;
    std::uint64_t acceptedVertices = 0;
    RtPathTraceCaptureAcceptedSetDiff acceptedSetDiff;
    RtPathTraceCaptureInstanceObservationDiff instanceObservationDiff;

    void ResetAndRelease() { *this = {}; }
    std::size_t OwnedBytes() const { return ownedBytes; }
};

struct RtPathTraceCaptureOracle
{
    RtPathTracePlanningSnapshotEpoch epoch;
    std::size_t ownedBytes = 64;
    bool complete = false;
    bool exact = true;
    std::uint64_t acceptedSurfaces = 0;
    std::uint64_t acceptedVertices = 0;
    RtPathTraceCaptureLiveCardinalityAttribution liveCardinality;

    void ResetAndRelease() { *this = {}; }
    std::size_t OwnedBytes() const { return ownedBytes; }
};

struct RtPathTraceCaptureComparison
{
    std::uint64_t generation = 0;
    std::uint32_t geometryMismatches = 0;
    std::uint32_t proposalMismatches = 0;
    RtPathTraceCaptureFirstFailure firstFail =
        RtPathTraceCaptureFirstFailure::None;
    std::uint32_t firstSurfaceOrdinal = UINT32_MAX;
    RtPathTraceCaptureCardinalityAttribution cardinality;
    RtPathTraceCaptureAcceptedSetDiff acceptedSetDiff;
    RtPathTraceCaptureInstanceObservationDiff instanceObservationDiff;
    bool cardinalityAvailable = false;
    bool exact = false;
};

struct RtPathTraceAccelCpuCompatibilityToken
{
    std::uint64_t mapTimeStamp = 0;
    std::uint64_t mapLoadSerial = 0;
    char mapName[RT_PT_PLANNING_MAP_NAME_CAPACITY] = {};
    std::uint64_t lifecycleEpoch = 0;
    std::uint64_t configFingerprint = 0;
};

struct RtPathTraceAccelCpuSnapshot
{
    RtPathTracePlanningSnapshotEpoch epoch;
    RtPathTraceAccelCpuCompatibilityToken compatibility;
    std::uint64_t inputReceipt = 0;
    std::size_t ownedBytes = 0;
    std::uint32_t buildDelayUs = 0;
    std::uint32_t buildFailureKind = 0;
    std::uint64_t rigidSignature = 0;
    std::uint64_t accelerationSignature = 0;
    std::uint64_t staticSignature = 0;
    bool complete = false;
    void ResetAndRelease() { *this = {}; }
    std::size_t OwnedBytes() const { return ownedBytes; }
};

struct RtPathTraceAccelCpuProduct
{
    RtPathTracePlanningSnapshotEpoch epoch;
    RtPathTraceAccelCpuCompatibilityToken compatibility;
    std::uint64_t inputReceipt = 0;
    std::size_t ownedBytes = 0;
    std::uint64_t rigidSignature = 0;
    std::uint64_t accelerationSignature = 0;
    std::uint64_t staticSignature = 0;
    std::uint32_t consumerSlotIndex = UINT32_MAX;
    std::uint64_t consumerGeneration = 0;
    bool complete = false;
    void ResetAndRelease() { *this = {}; }
    std::size_t OwnedBytes() const { return ownedBytes; }
};

struct RtPathTraceAccelCpuOracle
{
    RtPathTracePlanningSnapshotEpoch epoch;
    std::uint64_t inputReceipt = 0;
    std::uint64_t rigidSignature = 0;
    std::uint64_t accelerationSignature = 0;
    std::uint64_t staticSignature = 0;
    bool complete = false;
};
#endif

struct RtPathTraceProducerLaneTelemetry
{
    std::uint64_t dispatched = 0;
    std::uint64_t completed = 0;
    std::uint64_t compared = 0;
    std::uint64_t exact = 0;
    std::uint64_t mismatch = 0;
    std::uint64_t jobsEarly = 0;
    std::uint64_t jobsGeom = 0;
    std::uint64_t jobsProp = 0;
    std::uint64_t firstFailHistogram[RT_PT_CAPTURE_FIRST_FAILURE_COUNT] = {};
    std::uint64_t cardinalitySamples = 0;
    RtPathTraceCaptureCardinalityAttribution cardinalitySums;
    std::uint64_t firstMismatchGeneration = 0;
    RtPathTraceCaptureCardinalityAttribution firstMismatchCardinality;
    RtPathTraceCaptureAcceptedSetDiff firstMismatchAcceptedSetDiff;
    RtPathTraceCaptureInstanceObservationDiff firstMismatchInstanceObservationDiff;
    std::uint64_t acceptedSetDiffs = 0;
    std::uint64_t productOnlyAccepted = 0;
    std::uint64_t oracleOnlyAccepted = 0;
    std::uint64_t shadowSummaryCadences = 0;
    std::uint64_t late = 0;
    std::uint64_t retired = 0;
    std::uint64_t consumeHit = 0;
    std::uint64_t consumeMiss = 0;
    std::uint64_t staleReject = 0;
    std::uint64_t receiptMismatch = 0;
    std::uint64_t skinnedEligibilityFail = 0;
    std::uint64_t consumedAge[2] = {};
    std::uint64_t rigidDeferred = 0;
    std::uint64_t rigidReplayed = 0;
    std::uint64_t rigidApplied = 0;
    std::uint64_t rigidApplyFallback = 0;
    std::uint64_t busyFallback = 0;
    std::uint64_t capFallback = 0;
    std::uint64_t lifecycleFallback = 0;
    std::uint64_t historyRows = 0;
    std::uint64_t historyBytes = 0;
    std::uint64_t historyCopyUs = 0;
    std::uint64_t snapshotUs = 0;
    std::uint64_t harvestUs = 0;
    std::uint64_t dispatchUs = 0;
    std::uint64_t coordinatorUs = 0;
    std::uint64_t mainThreadRemainderUs = 0;
    std::size_t slotHighWater[RT_PT_PRODUCER_LANE_A_SLOT_COUNT] = {};
    std::size_t laneARingResidentBytes = 0;
    std::uint64_t laneJobs[RT_PT_PRODUCER_LANE_COUNT] = {};
    std::uint64_t laneCpuUs[RT_PT_PRODUCER_LANE_COUNT] = {};
    std::uint64_t laneBDispatched = 0;
    std::uint64_t laneBCompleted = 0;
    std::uint64_t laneBCompared = 0;
    std::uint64_t laneBExact = 0;
    std::uint64_t laneBMismatch = 0;
    std::uint64_t laneBConsumeHit = 0;
    std::uint64_t laneBConsumeMiss = 0;
    // 0, 1, and 2+ generations. Resident payload identity, not frame age,
    // governs Lane-B compatibility after the first two buckets.
    std::uint64_t laneBConsumedAge[3] = {};
    std::uint64_t laneBStaleReject = 0;
    std::uint64_t laneBReceiptReject = 0;
    std::uint64_t laneBBusyFallback = 0;
    std::uint64_t laneBCapFallback = 0;
    std::uint64_t laneBLifecycleFallback = 0;
    std::uint64_t laneBLate = 0;
    std::uint64_t laneBRetired = 0;
    std::uint64_t laneBOwnershipTransitions = 0;
    std::uint64_t laneBSnapshotUs = 0;
    std::uint64_t laneBBuildUs = 0;
    std::uint64_t laneBApplyUs = 0;
    std::uint64_t laneBFallbackUs = 0;
    std::uint64_t laneBRigidProductApply = 0;
    std::uint64_t laneBRigidSerialPreserve = 0;
    std::size_t laneBSlotHighWater[RT_PT_PRODUCER_LANE_A_SLOT_COUNT] = {};
    std::size_t laneBRingResidentBytes = 0;
    std::size_t laneBAttemptedHighWater = 0;
};

bool ConfigurePathTraceProducerLanes(int mode, int laneMask);
bool PathTraceProducerLaneAActive();
bool PathTraceProducerLaneBActive();
int PathTraceProducerLaneEffectiveMode();
int PathTraceProducerLaneBEffectiveMode();
int PathTraceProducerLaneEffectiveMask();
bool DispatchPathTraceProducerLaneA(RtPathTraceCaptureOwnerSnapshot&& snapshot);
bool RecordPathTraceProducerSerialOracle(RtPathTraceCaptureOracle&& oracle);
bool TryConsumePathTraceProducerLaneA(
    const RtPathTracePlanningSnapshotEpoch& epoch,
    std::uint64_t viewIdentity,
    RtPathTraceCaptureProduct& product);
bool TryConsumeLatestPathTraceProducerLaneA(
    std::uint64_t currentGeneration,
    const RtPathTraceLateConsumeToken& currentToken,
    const RtPathTraceCaptureMembershipReceipt& currentReceipt,
    RtPathTraceCaptureProduct& product,
    std::uint32_t& productAge);
bool DispatchPathTraceProducerLaneB(RtPathTraceAccelCpuSnapshot&& snapshot);
bool RecordPathTraceProducerLaneBSerialOracle(
    const RtPathTraceAccelCpuOracle& oracle);
bool TryConsumeLatestPathTraceProducerLaneB(
    std::uint64_t currentGeneration,
    const RtPathTraceAccelCpuCompatibilityToken& currentToken,
    std::uint64_t currentReceipt,
    RtPathTraceAccelCpuProduct& product,
    std::uint32_t& productAge);
void ReleasePathTraceProducerLaneBProduct(RtPathTraceAccelCpuProduct& product);
void RecordPathTraceProducerLaneBTiming(
    std::uint64_t snapshotUs,
    std::uint64_t applyUs,
    std::uint64_t fallbackUs);
void RecordPathTraceProducerLaneBRigidApplyOutcome(bool productApplied);
void RecordPathTraceProducerLaneBOwnershipTransition();
void RecordPathTraceProducerLaneBCapAttempt(std::size_t attemptedBytes);
void RecordPathTraceProducerCoordinatorTiming(
    std::uint64_t snapshotUs,
    std::uint64_t mainThreadRemainderUs);
void RecordPathTraceProducerHarvestTiming(std::uint64_t harvestUs);
void RecordPathTraceProducerSnapshotFallback(std::uint64_t snapshotUs);
void RecordPathTraceProducerLaneARigidOutcome(
    std::size_t deferred, std::size_t replayed,
    std::size_t applied, bool fallback);
void PollPathTraceProducerLanes(std::uint64_t currentGeneration, bool logTelemetry);
void ResetPathTraceProducerLanes();
void ShutdownPathTraceProducerLanes();
RtPathTraceProducerLaneTelemetry PathTraceProducerLanesTelemetry();

#if defined(RT_PT_PRODUCER_LANES_HARNESS)
struct RtPathTraceProducerLaneHostTestState
{
    bool running = false;
    int mode = 0;
    int laneMask = 0;
    std::uint32_t startedMask = 0;
    RtPathTraceProducerSlotContract slots[RT_PT_PRODUCER_LANE_A_SLOT_COUNT];
    RtPathTraceProducerSlotContract laneBSlots[RT_PT_PRODUCER_LANE_A_SLOT_COUNT];
};

void PathTraceProducerLanesTestSetStartFailureAfter(int startedThreadCount);
void PathTraceProducerLanesTestSetRequiredLaneMaskOverride(int laneMask);
void PathTraceProducerLanesTestPauseAfterCommit(bool pause);
bool PathTraceProducerLanesTestCommitPaused();
void PathTraceProducerLanesTestPauseDuringAccounting(bool pause);
bool PathTraceProducerLanesTestAccountingPaused();
RtPathTraceProducerLaneHostTestState PathTraceProducerLanesTestState();
#endif
