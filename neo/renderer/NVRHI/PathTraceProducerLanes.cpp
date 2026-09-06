#if !defined(RT_PT_PRODUCER_LANES_HARNESS)
#include "precompiled.h"
#pragma hdrstop
#endif

#include "PathTraceProducerLanes.h"
#include "PathTraceCommittedBaseline.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>

#if defined(RT_PT_PRODUCER_LANES_HARNESS)
#define OPTICK_EVENT(name) ((void)0)
#define OPTICK_THREAD(name) ((void)0)
namespace
{
std::uint64_t ProducerMicroseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
#else
namespace
{
std::uint64_t ProducerMicroseconds()
{
    return Sys_Microseconds();
}
}
#endif

namespace
{

struct LaneASlot
{
    std::atomic<RtPathTraceProducerSlotState> state{
        RtPathTraceProducerSlotState::Free};
    std::atomic<bool> oracleReady{false};
    std::atomic<std::size_t> inFlightCandidateBytes{0};
    std::mutex ownershipMutex;
    std::uint64_t generation = 0;
    RtPathTraceCaptureOwnerSnapshot snapshot;
    RtPathTraceCaptureProduct product;
    RtPathTraceCaptureOracle oracle;
};

struct LaneBSlot
{
    std::atomic<RtPathTraceProducerSlotState> state{
        RtPathTraceProducerSlotState::Free};
    std::atomic<bool> oracleReady{false};
    std::mutex ownershipMutex;
    std::uint64_t generation = 0;
    RtPathTraceAccelCpuSnapshot snapshot;
    RtPathTraceAccelCpuProduct product;
    RtPathTraceAccelCpuOracle oracle;
};

struct ProducerLaneThread
{
    std::thread thread;
    std::mutex mutex;
    std::condition_variable condition;
    std::uint8_t pendingSlots = 0;
    bool stop = false;
};

struct ProducerHost
{
    LaneASlot slots[RT_PT_PRODUCER_LANE_A_SLOT_COUNT];
    LaneBSlot laneBSlots[RT_PT_PRODUCER_LANE_A_SLOT_COUNT];
    ProducerLaneThread lanes[RT_PT_PRODUCER_LANE_COUNT];
    std::mutex telemetryMutex;
    std::mutex lifecycleMutex;
    RtPathTraceProducerLaneTelemetry telemetry;
    std::size_t slotResidentBytes[RT_PT_PRODUCER_LANE_A_SLOT_COUNT] = {};
    std::size_t laneBSlotResidentBytes[RT_PT_PRODUCER_LANE_A_SLOT_COUNT] = {};
    std::atomic<bool> running{false};
    std::atomic<int> mode{0};
    std::atomic<int> laneMask{1};
    std::uint32_t startedMask = 0;
    bool firstMismatchPrinted = false;
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
    int startFailureAfter = -1;
    int requiredLaneMaskOverride = -1;
    std::atomic<bool> pauseAfterCommit{false};
    std::atomic<bool> commitPaused{false};
    std::atomic<bool> pauseDuringAccounting{false};
    std::atomic<bool> accountingPaused{false};
#endif
};

ProducerHost g_host;

bool HostLateConsumeTokensCompatible(
    const RtPathTraceLateConsumeToken& product,
    const RtPathTraceLateConsumeToken& current)
{
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
    return product.mapTimeStamp == current.mapTimeStamp &&
        product.mapLoadSerial == current.mapLoadSerial &&
        std::strncmp(product.mapName, current.mapName,
            RT_PT_PLANNING_MAP_NAME_CAPACITY) == 0 &&
        product.capturedAfterBeginFrame && current.capturedAfterBeginFrame &&
        product.capturedAfterStaticPreload && current.capturedAfterStaticPreload &&
        product.configFingerprint == current.configFingerprint;
#else
    return CompatibleForLateConsume(product, current);
#endif
}

bool HostMembershipReceiptsMatch(
    const RtPathTraceCaptureMembershipReceipt& product,
    const RtPathTraceCaptureMembershipReceipt& current)
{
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
    return product.complete && current.complete &&
        product.hash == current.hash &&
        product.surfaceCount == current.surfaceCount &&
        product.skinnedSurfaceCount == current.skinnedSurfaceCount &&
        product.cpuSkinnedAcceptedCount == current.cpuSkinnedAcceptedCount;
#else
    return PathTraceCaptureMembershipReceiptsMatch(product, current);
#endif
}

bool HostAccelCpuTokensCompatible(
    const RtPathTraceAccelCpuCompatibilityToken& product,
    const RtPathTraceAccelCpuCompatibilityToken& current)
{
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
    return product.mapTimeStamp == current.mapTimeStamp &&
        product.mapLoadSerial == current.mapLoadSerial &&
        std::strncmp(product.mapName, current.mapName,
            RT_PT_PLANNING_MAP_NAME_CAPACITY) == 0 &&
        product.lifecycleEpoch == current.lifecycleEpoch &&
        product.configFingerprint == current.configFingerprint;
#else
    return PathTraceAccelCpuTokensCompatible(product, current);
#endif
}

bool CaptureFirstFailureIsEarly(RtPathTraceCaptureFirstFailure failure)
{
    return failure == RtPathTraceCaptureFirstFailure::Incomplete ||
        failure == RtPathTraceCaptureFirstFailure::Epoch ||
        failure == RtPathTraceCaptureFirstFailure::ViewIdentity;
}

void CaptureSaturatingAdd(std::uint64_t& destination, std::uint64_t value)
{
    destination = destination > UINT64_MAX - value
        ? UINT64_MAX : destination + value;
}

void CaptureSaturatingAdd(std::int64_t& destination, std::int64_t value)
{
    if (value > 0 && destination > INT64_MAX - value)
    {
        destination = INT64_MAX;
    }
    else if (value < 0 && destination < INT64_MIN - value)
    {
        destination = INT64_MIN;
    }
    else
    {
        destination += value;
    }
}

void AccumulateCaptureCardinality(
    RtPathTraceCaptureCardinalityAttribution& destination,
    const RtPathTraceCaptureCardinalityAttribution& value)
{
#define RT_PT_CAPTURE_ADD(field) \
    CaptureSaturatingAdd(destination.field, value.field)
    RT_PT_CAPTURE_ADD(live.skinnedOmittedSurfaces);
    RT_PT_CAPTURE_ADD(live.skinnedOmittedVertices);
    RT_PT_CAPTURE_ADD(live.skinnedOmittedIndexes);
    RT_PT_CAPTURE_ADD(live.rigidRemovedSurfaces);
    RT_PT_CAPTURE_ADD(live.rigidRemovedVertices);
    RT_PT_CAPTURE_ADD(live.rigidRemovedIndexes);
    RT_PT_CAPTURE_ADD(productAcceptedSurfaces);
    RT_PT_CAPTURE_ADD(productAcceptedVertices);
    RT_PT_CAPTURE_ADD(oracleAcceptedSurfaces);
    RT_PT_CAPTURE_ADD(oracleAcceptedVertices);
    RT_PT_CAPTURE_ADD(afterSkinnedVertexDelta);
    RT_PT_CAPTURE_ADD(unattributedVertexDelta);
#undef RT_PT_CAPTURE_ADD
}

#if !defined(RT_PT_PRODUCER_LANES_HARNESS)
const char* CaptureFirstFailureName(RtPathTraceCaptureFirstFailure failure)
{
    switch (failure)
    {
        case RtPathTraceCaptureFirstFailure::None: return "None";
        case RtPathTraceCaptureFirstFailure::Incomplete: return "Incomplete";
        case RtPathTraceCaptureFirstFailure::Epoch: return "Epoch";
        case RtPathTraceCaptureFirstFailure::ViewIdentity: return "ViewIdentity";
        case RtPathTraceCaptureFirstFailure::Vertices: return "Vertices";
        case RtPathTraceCaptureFirstFailure::Indexes: return "Indexes";
        case RtPathTraceCaptureFirstFailure::TriangleClasses: return "TriangleClasses";
        case RtPathTraceCaptureFirstFailure::TriangleMaterials: return "TriangleMaterials";
        case RtPathTraceCaptureFirstFailure::TriangleInstances: return "TriangleInstances";
        case RtPathTraceCaptureFirstFailure::TriangleIdentities: return "TriangleIdentities";
        case RtPathTraceCaptureFirstFailure::MaterialInfoIntents: return "MaterialInfoIntents";
        case RtPathTraceCaptureFirstFailure::MaterialVariants: return "MaterialVariants";
        case RtPathTraceCaptureFirstFailure::InstanceObservations: return "InstanceObservations";
        case RtPathTraceCaptureFirstFailure::RigidCandidates: return "RigidCandidates";
        case RtPathTraceCaptureFirstFailure::StaticMembership: return "StaticMembership";
        case RtPathTraceCaptureFirstFailure::RoutedReadySkip: return "RoutedReadySkip";
        case RtPathTraceCaptureFirstFailure::SurfaceCount: return "SurfaceCount";
        case RtPathTraceCaptureFirstFailure::SurfaceDecision: return "SurfaceDecision";
        default: return "Invalid";
    }
}

void CaptureFirstFailureVectorSizes(
    RtPathTraceCaptureFirstFailure failure,
    const RtPathTraceCaptureProduct& product,
    const RtPathTraceCaptureOracle& oracle,
    std::size_t& productSize,
    std::size_t& oracleSize)
{
    productSize = 0;
    oracleSize = 0;
#define RT_PT_CAPTURE_FAILURE_SIZES(member) \
    productSize = product.member.size(); \
    oracleSize = oracle.member.size(); \
    break
    switch (failure)
    {
        case RtPathTraceCaptureFirstFailure::Vertices: RT_PT_CAPTURE_FAILURE_SIZES(vertices);
        case RtPathTraceCaptureFirstFailure::Indexes: RT_PT_CAPTURE_FAILURE_SIZES(indexes);
        case RtPathTraceCaptureFirstFailure::TriangleClasses: RT_PT_CAPTURE_FAILURE_SIZES(triangleClasses);
        case RtPathTraceCaptureFirstFailure::TriangleMaterials: RT_PT_CAPTURE_FAILURE_SIZES(triangleMaterials);
        case RtPathTraceCaptureFirstFailure::TriangleInstances: RT_PT_CAPTURE_FAILURE_SIZES(triangleInstances);
        case RtPathTraceCaptureFirstFailure::TriangleIdentities: RT_PT_CAPTURE_FAILURE_SIZES(triangleIdentities);
        case RtPathTraceCaptureFirstFailure::MaterialInfoIntents: RT_PT_CAPTURE_FAILURE_SIZES(materialInfoIntents);
        case RtPathTraceCaptureFirstFailure::MaterialVariants: RT_PT_CAPTURE_FAILURE_SIZES(materialVariants);
        case RtPathTraceCaptureFirstFailure::InstanceObservations: RT_PT_CAPTURE_FAILURE_SIZES(instanceObservations);
        case RtPathTraceCaptureFirstFailure::RigidCandidates: RT_PT_CAPTURE_FAILURE_SIZES(rigidCandidates);
        case RtPathTraceCaptureFirstFailure::StaticMembership: RT_PT_CAPTURE_FAILURE_SIZES(staticMembershipSurfaces);
        case RtPathTraceCaptureFirstFailure::RoutedReadySkip: RT_PT_CAPTURE_FAILURE_SIZES(routedReadySkipSurfaces);
        case RtPathTraceCaptureFirstFailure::SurfaceCount:
        case RtPathTraceCaptureFirstFailure::SurfaceDecision: RT_PT_CAPTURE_FAILURE_SIZES(surfaces);
        default: break;
    }
#undef RT_PT_CAPTURE_FAILURE_SIZES
}
#endif

void ResetSlot(LaneASlot& slot)
{
    slot.snapshot.ResetAndRelease();
    slot.product.ResetAndRelease();
    slot.oracle.ResetAndRelease();
    slot.generation = 0;
    slot.oracleReady.store(false, std::memory_order_release);
    slot.inFlightCandidateBytes.store(0, std::memory_order_release);
    slot.state.store(RtPathTraceProducerSlotState::Free,
        std::memory_order_release);
}

void ResetLaneBSlot(LaneBSlot& slot)
{
    slot.snapshot.ResetAndRelease();
    slot.product.ResetAndRelease();
    slot.oracle = RtPathTraceAccelCpuOracle();
    slot.generation = 0;
    slot.oracleReady.store(false, std::memory_order_release);
    slot.state.store(RtPathTraceProducerSlotState::Free,
        std::memory_order_release);
}

void RunLaneA(ProducerLaneThread& lane)
{
    OPTICK_THREAD("PT Producer Lane A");
    for (;;)
    {
        std::size_t slotIndex = 0;
        {
            std::unique_lock<std::mutex> lock(lane.mutex);
            lane.condition.wait(lock, [&lane]() {
                return lane.stop || lane.pendingSlots != 0;
            });
            if (lane.stop)
            {
                return;
            }
            while ((lane.pendingSlots & (1u << slotIndex)) == 0)
            {
                ++slotIndex;
            }
            lane.pendingSlots &= static_cast<std::uint8_t>(~(1u << slotIndex));
        }
        const std::uint64_t laneStartUs = ProducerMicroseconds();
        OPTICK_EVENT("PT Lane A");
        LaneASlot& slot = g_host.slots[slotIndex];
        const std::uint64_t generation = slot.generation;
        RtPathTraceProducerSlotState expected =
            RtPathTraceProducerSlotState::Queued;
        if (slot.generation == generation &&
            slot.state.compare_exchange_strong(expected,
                RtPathTraceProducerSlotState::Running,
                std::memory_order_acq_rel))
        {
            bool built = false;
            std::size_t peakSlotBytes = 0;
            RtPathTraceCaptureProduct builtProduct;
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
            if (slot.snapshot.buildDelayUs != 0)
            {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(slot.snapshot.buildDelayUs));
            }
            try
            {
                if (slot.snapshot.buildFailureKind == 1)
                {
                    throw std::bad_alloc();
                }
                if (slot.snapshot.buildFailureKind == 2)
                {
                    throw std::length_error("injected Lane A length failure");
                }
                RtPathTraceCompleteSlotLayout layout = slot.snapshot.capacityLayout;
                if (layout.snapshotFixed == 0)
                {
                    layout.snapshotFixed = slot.snapshot.ownedBytes;
                }
                if (layout.productFixed == 0)
                {
                    layout.productFixed = slot.snapshot.ownedBytes;
                }
                RtPathTraceCompleteSlotPlan capacityPlan;
                const bool capacityPlanned = RtPathTracePlanCompleteSlot(
                    slot.snapshot.capacityCardinality, layout,
                    RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES, capacityPlan);
                const std::size_t candidateBytes = capacityPlanned
                    ? capacityPlan.candidateBytes + capacityPlan.finalProductBytes
                    : RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES + 1;
                const std::size_t oracleBytes = slot.oracleReady.load(
                    std::memory_order_acquire) ? slot.oracle.OwnedBytes() : 0;
                peakSlotBytes = capacityPlanned
                    ? capacityPlan.peakBytes + oracleBytes
                    : RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES + 1;
                if (peakSlotBytes <= RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES)
                {
                    slot.inFlightCandidateBytes.store(
                        candidateBytes, std::memory_order_release);
                    built = slot.snapshot.buildSucceeds;
                }
                if (built)
                {
                    builtProduct.epoch = slot.snapshot.epoch;
                    builtProduct.lateConsumeToken =
                        slot.snapshot.lateConsumeToken;
                    builtProduct.membershipReceipt =
                        slot.snapshot.buildMembershipReceipt;
                    builtProduct.viewIdentity = slot.snapshot.viewIdentity;
                    builtProduct.ownedBytes = slot.snapshot.ownedBytes;
                    builtProduct.complete = true;
                    builtProduct.exact = slot.snapshot.buildExact;
                    builtProduct.acceptedSurfaces =
                        slot.snapshot.buildAcceptedSurfaces;
                    builtProduct.acceptedVertices =
                        slot.snapshot.buildAcceptedVertices;
                    builtProduct.acceptedSetDiff =
                        slot.snapshot.buildAcceptedSetDiff;
                }
            }
            catch (const std::bad_alloc&) {}
            catch (const std::length_error&) {}
#else
            try
            {
                std::size_t oracleBytes = 0;
                bool capacityPlanned = false;
                {
                    std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
                    oracleBytes = slot.oracleReady.load(std::memory_order_acquire)
                        ? slot.oracle.OwnedBytes()
                        : slot.snapshot.reservedOracleBytes;
                    RtPathTraceCaptureProductCapacityPlan capacityPlan;
                    if (PlanPathTraceCaptureProductCapacity(
                            slot.snapshot, oracleBytes, capacityPlan))
                    {
                        slot.inFlightCandidateBytes.store(
                            capacityPlan.candidateBytes +
                                capacityPlan.finalProductBytes,
                            std::memory_order_release);
                        peakSlotBytes = capacityPlan.peakSlotBytes;
                        capacityPlanned = true;
                    }
                }
                if (capacityPlanned)
                {
                    built = BuildPathTraceCaptureProduct(
                        slot.snapshot, builtProduct, oracleBytes,
                        &peakSlotBytes);
                }
            }
            catch (const std::bad_alloc&) {}
            catch (const std::length_error&) {}
#endif
            {
                std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
                if (built)
                {
                    slot.product = std::move(builtProduct);
                }
                else
                {
                    builtProduct.ResetAndRelease();
                    slot.product.ResetAndRelease();
                }
                slot.inFlightCandidateBytes.store(0, std::memory_order_release);
            }
            if (!built)
            {
                builtProduct.ResetAndRelease();
            }
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
            if (g_host.pauseAfterCommit.load(std::memory_order_acquire))
            {
                g_host.commitPaused.store(true, std::memory_order_release);
                while (g_host.pauseAfterCommit.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                g_host.commitPaused.store(false, std::memory_order_release);
            }
#endif
            {
                // Reacquire slot ownership after the build commit.  Oracle
                // publication may win the intentional gap above; computing
                // resident bytes and publishing telemetry under the same lock
                // makes that interleaving visible instead of under-reporting it.
                std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
                const std::size_t owned = slot.snapshot.OwnedBytes() +
                    slot.product.OwnedBytes() +
                    (slot.oracleReady.load(std::memory_order_acquire)
                        ? slot.oracle.OwnedBytes() : 0);
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
                if (g_host.pauseDuringAccounting.load(std::memory_order_acquire))
                {
                    g_host.accountingPaused.store(true, std::memory_order_release);
                    while (g_host.pauseDuringAccounting.load(std::memory_order_acquire))
                    {
                        std::this_thread::yield();
                    }
                    g_host.accountingPaused.store(false, std::memory_order_release);
                }
#endif
                std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
                ++g_host.telemetry.completed;
                ++g_host.telemetry.laneJobs[0];
                g_host.telemetry.laneCpuUs[0] +=
                    ProducerMicroseconds() - laneStartUs;
                if (!built || owned > RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES)
                {
                    ++g_host.telemetry.capFallback;
                }
                g_host.telemetry.slotHighWater[slotIndex] = std::max(
                    g_host.telemetry.slotHighWater[slotIndex],
                    peakSlotBytes <= RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES
                        ? std::max(owned, peakSlotBytes) : owned);
                g_host.slotResidentBytes[slotIndex] = owned;
            }
            slot.state.store(RtPathTraceProducerSlotState::Ready,
                std::memory_order_release);
        }
    }
}

void RunLaneB(ProducerLaneThread& lane)
{
    OPTICK_THREAD("PT Producer Lane B");
    for (;;)
    {
        std::size_t slotIndex = 0;
        {
            std::unique_lock<std::mutex> lock(lane.mutex);
            lane.condition.wait(lock, [&lane]() {
                return lane.stop || lane.pendingSlots != 0;
            });
            if (lane.stop) return;
            while ((lane.pendingSlots & (1u << slotIndex)) == 0) ++slotIndex;
            lane.pendingSlots &= static_cast<std::uint8_t>(~(1u << slotIndex));
        }

        const std::uint64_t laneStartUs = ProducerMicroseconds();
        OPTICK_EVENT("PT Lane B");
        LaneBSlot& slot = g_host.laneBSlots[slotIndex];
        const std::uint64_t generation = slot.generation;
        RtPathTraceProducerSlotState expected = RtPathTraceProducerSlotState::Queued;
        if (slot.generation != generation ||
            !slot.state.compare_exchange_strong(expected,
                RtPathTraceProducerSlotState::Running,
                std::memory_order_acq_rel))
        {
            continue;
        }

        bool built = false;
        std::size_t peakBytes = slot.snapshot.OwnedBytes();
        RtPathTraceAccelCpuProduct product;
        try
        {
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
            if (slot.snapshot.buildDelayUs != 0)
            {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(slot.snapshot.buildDelayUs));
            }
            if (slot.snapshot.buildFailureKind == 1) throw std::bad_alloc();
            if (slot.snapshot.buildFailureKind == 2)
                throw std::length_error("injected Lane B length failure");
            if (slot.snapshot.complete && slot.snapshot.inputReceipt != 0 &&
                slot.snapshot.OwnedBytes() <= RT_PT_ACCEL_CPU_SLOT_MAX_BYTES)
            {
                product.epoch = slot.snapshot.epoch;
                product.compatibility = slot.snapshot.compatibility;
                product.inputReceipt = slot.snapshot.inputReceipt;
                product.ownedBytes = slot.snapshot.ownedBytes;
                product.rigidSignature = slot.snapshot.rigidSignature;
                product.accelerationSignature = slot.snapshot.accelerationSignature;
                product.staticSignature = slot.snapshot.staticSignature;
                product.complete = true;
                peakBytes = slot.snapshot.OwnedBytes() + product.OwnedBytes();
                built = peakBytes <= RT_PT_ACCEL_CPU_SLOT_MAX_BYTES;
            }
#else
            built = BuildPathTraceAccelCpuProduct(
                slot.snapshot, product, &peakBytes);
#endif
        }
        catch (const std::bad_alloc&) {}
        catch (const std::length_error&) {}

        {
            std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
            if (built) slot.product = std::move(product);
            else slot.product.ResetAndRelease();
            const std::size_t resident = slot.snapshot.OwnedBytes() +
                slot.product.OwnedBytes();
            std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
            ++g_host.telemetry.laneBCompleted;
            ++g_host.telemetry.laneJobs[1];
            const std::uint64_t buildUs = ProducerMicroseconds() - laneStartUs;
            g_host.telemetry.laneCpuUs[1] += buildUs;
            g_host.telemetry.laneBBuildUs += buildUs;
            if (!built || resident > RT_PT_ACCEL_CPU_SLOT_MAX_BYTES)
                ++g_host.telemetry.laneBCapFallback;
            g_host.telemetry.laneBAttemptedHighWater = std::max(
                g_host.telemetry.laneBAttemptedHighWater, peakBytes);
            g_host.telemetry.laneBSlotHighWater[slotIndex] = std::max(
                g_host.telemetry.laneBSlotHighWater[slotIndex],
                peakBytes <= RT_PT_ACCEL_CPU_SLOT_MAX_BYTES
                    ? std::max(resident, peakBytes) : resident);
            g_host.laneBSlotResidentBytes[slotIndex] = resident;
        }
        slot.state.store(RtPathTraceProducerSlotState::Ready,
            std::memory_order_release);
    }
}

#if defined(RT_PT_PRODUCER_LANES_HARNESS)
void RunIdleLane(ProducerLaneThread& lane, const char* eventName)
{
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(lane.mutex);
            lane.condition.wait(lock, [&lane]() {
                return lane.stop;
            });
            if (lane.stop)
            {
                return;
            }
        }
    }
}
#endif

void StopThreads()
{
    g_host.running.store(false, std::memory_order_release);
    for (std::size_t laneIndex = 0; laneIndex < RT_PT_PRODUCER_LANE_COUNT;
        ++laneIndex)
    {
        if ((g_host.startedMask & (1u << laneIndex)) == 0)
        {
            continue;
        }
        ProducerLaneThread& lane = g_host.lanes[laneIndex];
        {
            std::lock_guard<std::mutex> lock(lane.mutex);
            lane.stop = true;
            lane.pendingSlots = 0;
        }
        lane.condition.notify_all();
    }
    for (std::size_t laneIndex = 0; laneIndex < RT_PT_PRODUCER_LANE_COUNT;
        ++laneIndex)
    {
        ProducerLaneThread& lane = g_host.lanes[laneIndex];
        if (lane.thread.joinable())
        {
            lane.thread.join();
        }
    }
    g_host.startedMask = 0;
}

bool StartThreads(std::uint32_t requiredMask)
{
    try
    {
        for (std::size_t laneIndex = 0; laneIndex < RT_PT_PRODUCER_LANE_COUNT;
            ++laneIndex)
        {
            if ((requiredMask & (1u << laneIndex)) == 0)
            {
                continue;
            }
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
            if (g_host.startFailureAfter >= 0 &&
                static_cast<int>(g_host.startedMask == 0 ? 0 :
                    ((g_host.startedMask & 1u) != 0) +
                    ((g_host.startedMask & 2u) != 0) +
                    ((g_host.startedMask & 4u) != 0)) >= g_host.startFailureAfter)
            {
                throw std::runtime_error("injected producer lane start failure");
            }
#endif
            ProducerLaneThread& lane = g_host.lanes[laneIndex];
            std::lock_guard<std::mutex> lock(lane.mutex);
            lane.stop = false;
            lane.pendingSlots = 0;
            if (laneIndex == 0)
            {
                lane.thread = std::thread([]() { RunLaneA(g_host.lanes[0]); });
            }
            else if (laneIndex == 1)
            {
                lane.thread = std::thread([]() { RunLaneB(g_host.lanes[1]); });
            }
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
            else
            {
                lane.thread = std::thread([laneIndex]() {
                    RunIdleLane(g_host.lanes[laneIndex],
                        laneIndex == 1 ? "PT Lane B" : "PT Lane C");
                });
            }
#endif
            g_host.startedMask |= 1u << laneIndex;
        }
        g_host.running.store(true, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        StopThreads();
        return false;
    }
}

} // namespace

bool ConfigurePathTraceProducerLanes(int mode, int laneMask)
{
    std::lock_guard<std::mutex> lifecycleLock(g_host.lifecycleMutex);
    const int normalizedMask = laneMask & 0x7;
    const bool implemented = RtPathTraceProducerModeImplemented(mode);
    const int normalizedMode = implemented ? mode : 0;
    if (g_host.mode.load(std::memory_order_acquire) == normalizedMode &&
        g_host.laneMask.load(std::memory_order_acquire) == normalizedMask &&
        (normalizedMode == 0 || g_host.running.load(std::memory_order_acquire)))
    {
        return implemented;
    }
    StopThreads();
    for (LaneASlot& slot : g_host.slots)
    {
        ResetSlot(slot);
    }
    for (LaneBSlot& slot : g_host.laneBSlots)
    {
        ResetLaneBSlot(slot);
    }
    {
        std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
        for (std::size_t& bytes : g_host.slotResidentBytes)
        {
            bytes = 0;
        }
        for (std::size_t& bytes : g_host.laneBSlotResidentBytes) bytes = 0;
    }
    g_host.mode.store(normalizedMode, std::memory_order_release);
    g_host.laneMask.store(normalizedMask, std::memory_order_release);
    if (normalizedMode == 0)
    {
        return implemented;
    }
    // Lane C remains intentionally unauthorized.  Lane B is the independent
    // acceleration CPU pack lane selected by mask bit 1.
    std::uint32_t requiredMask = static_cast<std::uint32_t>(normalizedMask & 3);
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
    if (g_host.requiredLaneMaskOverride >= 0)
    {
        requiredMask = static_cast<std::uint32_t>(
            g_host.requiredLaneMaskOverride & 0x7);
    }
#endif
    if (!StartThreads(requiredMask))
    {
        g_host.mode.store(0, std::memory_order_release);
        std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
        ++g_host.telemetry.lifecycleFallback;
        return false;
    }
    return implemented;
}

bool PathTraceProducerLaneAActive()
{
    const int mode = g_host.mode.load(std::memory_order_acquire);
    return g_host.running.load(std::memory_order_acquire) &&
        (mode == 1 || mode == 2) &&
        (g_host.laneMask.load(std::memory_order_acquire) & 1) != 0;
}

bool PathTraceProducerLaneBActive()
{
    const int mode = g_host.mode.load(std::memory_order_acquire);
    return g_host.running.load(std::memory_order_acquire) &&
        (mode == 1 || mode == 2) &&
        (g_host.startedMask & 2u) != 0 &&
        (g_host.laneMask.load(std::memory_order_acquire) & 2) != 0;
}

int PathTraceProducerLaneEffectiveMode()
{
    return PathTraceProducerLaneAActive()
        ? g_host.mode.load(std::memory_order_acquire) : 0;
}

int PathTraceProducerLaneBEffectiveMode()
{
    return PathTraceProducerLaneBActive()
        ? g_host.mode.load(std::memory_order_acquire) : 0;
}

int PathTraceProducerLaneEffectiveMask()
{
    return g_host.running.load(std::memory_order_acquire)
        ? g_host.laneMask.load(std::memory_order_acquire) & 0x7 : 0;
}

bool DispatchPathTraceProducerLaneA(RtPathTraceCaptureOwnerSnapshot&& snapshot)
{
    OPTICK_EVENT("PT Producer Lanes Dispatch");
    const std::uint64_t startUs = ProducerMicroseconds();
    if (!g_host.running.load(std::memory_order_acquire) ||
        (g_host.mode.load(std::memory_order_acquire) != 1 &&
            g_host.mode.load(std::memory_order_acquire) != 2) ||
        (g_host.laneMask.load(std::memory_order_acquire) & 1) == 0 ||
        !snapshot.complete || snapshot.epoch.generation == 0)
    {
        return false;
    }
    const std::uint64_t generation = snapshot.epoch.generation;
    LaneASlot& slot = g_host.slots[
        generation % RT_PT_PRODUCER_LANE_A_SLOT_COUNT];
    if (slot.state.load(std::memory_order_acquire) ==
            RtPathTraceProducerSlotState::Ready)
    {
        std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
        if (slot.state.load(std::memory_order_acquire) ==
                RtPathTraceProducerSlotState::Ready &&
            slot.generation != 0 && generation > slot.generation + 1)
        {
            ResetSlot(slot);
            std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
            ++g_host.telemetry.staleReject;
            ++g_host.telemetry.retired;
            g_host.slotResidentBytes[
                generation % RT_PT_PRODUCER_LANE_A_SLOT_COUNT] = 0;
        }
    }
    RtPathTraceProducerSlotState expected = RtPathTraceProducerSlotState::Free;
    if (!slot.state.compare_exchange_strong(expected,
            RtPathTraceProducerSlotState::Queued,
            std::memory_order_acq_rel))
    {
        std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
        ++g_host.telemetry.busyFallback;
        return false;
    }
    slot.generation = generation;
    slot.snapshot = std::move(snapshot);
    slot.product.ResetAndRelease();
    slot.oracle.ResetAndRelease();
    slot.oracleReady.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
        ++g_host.telemetry.dispatched;
        g_host.telemetry.historyRows += slot.snapshot.instanceUniverse.historyRows;
        g_host.telemetry.historyBytes += slot.snapshot.instanceUniverse.historyBytes;
        g_host.telemetry.historyCopyUs += slot.snapshot.instanceUniverse.historyCopyUs;
        g_host.telemetry.dispatchUs += ProducerMicroseconds() - startUs;
        g_host.slotResidentBytes[
            generation % RT_PT_PRODUCER_LANE_A_SLOT_COUNT] =
            slot.snapshot.OwnedBytes();
    }
    {
        ProducerLaneThread& lane = g_host.lanes[0];
        {
            std::lock_guard<std::mutex> lock(lane.mutex);
            lane.pendingSlots |= static_cast<std::uint8_t>(
                1u << (generation % RT_PT_PRODUCER_LANE_A_SLOT_COUNT));
        }
        lane.condition.notify_one();
    }
    return true;
}

bool RecordPathTraceProducerSerialOracle(RtPathTraceCaptureOracle&& oracle)
{
    if (!oracle.complete || oracle.epoch.generation == 0)
    {
        return false;
    }
    LaneASlot& slot = g_host.slots[
        oracle.epoch.generation % RT_PT_PRODUCER_LANE_A_SLOT_COUNT];
    if (slot.generation != oracle.epoch.generation ||
        !RtPathTracePlanningEpochsMatch(slot.snapshot.epoch, oracle.epoch))
    {
        return false;
    }
    std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
    const std::size_t oracleBytes = oracle.OwnedBytes();
    std::size_t owned = 0;
    if (!RtPathTraceProducerCheckedAddBytes(owned,
            slot.snapshot.OwnedBytes(), RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES) ||
        !RtPathTraceProducerCheckedAddBytes(owned,
            slot.product.OwnedBytes(), RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES) ||
        !RtPathTraceProducerCheckedAddBytes(owned,
            slot.inFlightCandidateBytes.load(std::memory_order_acquire),
            RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES) ||
        !RtPathTraceProducerCheckedAddBytes(owned,
            oracleBytes, RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES))
    {
        std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
        ++g_host.telemetry.capFallback;
        return false;
    }
    slot.oracle = std::move(oracle);
    slot.oracleReady.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
        const std::size_t slotIndex = slot.generation %
            RT_PT_PRODUCER_LANE_A_SLOT_COUNT;
        g_host.telemetry.slotHighWater[slotIndex] = std::max(
            g_host.telemetry.slotHighWater[slotIndex], owned);
        g_host.slotResidentBytes[slotIndex] = owned;
    }
    return true;
}

bool TryConsumePathTraceProducerLaneA(
    const RtPathTracePlanningSnapshotEpoch& epoch,
    std::uint64_t viewIdentity,
    RtPathTraceCaptureProduct& product)
{
    product.ResetAndRelease();
    if (PathTraceProducerLaneEffectiveMode() != 2 ||
        !RtPathTracePlanningEpochValid(epoch) || epoch.generation == 0)
    {
        return false;
    }
    const std::size_t slotIndex = epoch.generation %
        RT_PT_PRODUCER_LANE_A_SLOT_COUNT;
    LaneASlot& slot = g_host.slots[slotIndex];
    if (slot.state.load(std::memory_order_acquire) !=
            RtPathTraceProducerSlotState::Ready ||
        slot.generation != epoch.generation)
    {
        return false;
    }
    std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
    if (slot.state.load(std::memory_order_acquire) !=
            RtPathTraceProducerSlotState::Ready ||
        slot.generation != epoch.generation || !slot.product.complete ||
        slot.product.viewIdentity != viewIdentity ||
        !RtPathTracePlanningEpochsMatch(slot.product.epoch, epoch))
    {
        return false;
    }
    product = std::move(slot.product);
    slot.snapshot.ResetAndRelease();
    slot.oracle.ResetAndRelease();
    slot.generation = 0;
    slot.oracleReady.store(false, std::memory_order_release);
    slot.inFlightCandidateBytes.store(0, std::memory_order_release);
    slot.state.store(RtPathTraceProducerSlotState::Free,
        std::memory_order_release);
    {
        std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
        g_host.slotResidentBytes[slotIndex] = 0;
    }
    return true;
}

bool TryConsumeLatestPathTraceProducerLaneA(
    std::uint64_t currentGeneration,
    const RtPathTraceLateConsumeToken& currentToken,
    const RtPathTraceCaptureMembershipReceipt& currentReceipt,
    RtPathTraceCaptureProduct& product,
    std::uint32_t& productAge)
{
    product.ResetAndRelease();
    productAge = UINT32_MAX;
    if (PathTraceProducerLaneEffectiveMode() != 2 || currentGeneration == 0 ||
        !currentReceipt.complete || currentReceipt.cpuSkinnedAcceptedCount != 0)
    {
        std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
        ++g_host.telemetry.consumeMiss;
        if (currentReceipt.cpuSkinnedAcceptedCount != 0)
        {
            ++g_host.telemetry.skinnedEligibilityFail;
        }
        return false;
    }

    std::size_t selectedIndex = RT_PT_PRODUCER_LANE_A_SLOT_COUNT;
    std::uint64_t selectedGeneration = 0;
    for (std::size_t slotIndex = 0;
        slotIndex < RT_PT_PRODUCER_LANE_A_SLOT_COUNT; ++slotIndex)
    {
        LaneASlot& slot = g_host.slots[slotIndex];
        if (slot.state.load(std::memory_order_acquire) !=
                RtPathTraceProducerSlotState::Ready)
        {
            continue;
        }
        std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
        if (slot.state.load(std::memory_order_acquire) !=
                RtPathTraceProducerSlotState::Ready ||
            slot.generation == 0 || !slot.product.complete)
        {
            continue;
        }
        if (slot.generation > currentGeneration ||
            currentGeneration - slot.generation > 1)
        {
            ResetSlot(slot);
            std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
            ++g_host.telemetry.staleReject;
            ++g_host.telemetry.retired;
            g_host.slotResidentBytes[slotIndex] = 0;
            continue;
        }
        if (!HostLateConsumeTokensCompatible(
                slot.product.lateConsumeToken, currentToken))
        {
            ResetSlot(slot);
            std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
            ++g_host.telemetry.staleReject;
            ++g_host.telemetry.retired;
            g_host.slotResidentBytes[slotIndex] = 0;
            continue;
        }
        if (!HostMembershipReceiptsMatch(
                slot.product.membershipReceipt, currentReceipt))
        {
            ResetSlot(slot);
            std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
            ++g_host.telemetry.receiptMismatch;
            ++g_host.telemetry.retired;
            g_host.slotResidentBytes[slotIndex] = 0;
            continue;
        }
        if (slot.product.membershipReceipt.cpuSkinnedAcceptedCount != 0)
        {
            ResetSlot(slot);
            std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
            ++g_host.telemetry.skinnedEligibilityFail;
            ++g_host.telemetry.retired;
            g_host.slotResidentBytes[slotIndex] = 0;
            continue;
        }
        if (slot.generation > selectedGeneration)
        {
            selectedGeneration = slot.generation;
            selectedIndex = slotIndex;
        }
    }

    if (selectedIndex == RT_PT_PRODUCER_LANE_A_SLOT_COUNT)
    {
        std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
        ++g_host.telemetry.consumeMiss;
        return false;
    }

    LaneASlot& selected = g_host.slots[selectedIndex];
    std::lock_guard<std::mutex> ownershipLock(selected.ownershipMutex);
    if (selected.state.load(std::memory_order_acquire) !=
            RtPathTraceProducerSlotState::Ready ||
        selected.generation != selectedGeneration ||
        !selected.product.complete)
    {
        std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
        ++g_host.telemetry.consumeMiss;
        return false;
    }
    productAge = static_cast<std::uint32_t>(
        currentGeneration - selectedGeneration);
    product = std::move(selected.product);
    selected.snapshot.ResetAndRelease();
    selected.oracle.ResetAndRelease();
    selected.generation = 0;
    selected.oracleReady.store(false, std::memory_order_release);
    selected.inFlightCandidateBytes.store(0, std::memory_order_release);
    selected.state.store(RtPathTraceProducerSlotState::Free,
        std::memory_order_release);
    {
        std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
        ++g_host.telemetry.consumeHit;
        ++g_host.telemetry.consumedAge[productAge];
        g_host.slotResidentBytes[selectedIndex] = 0;
    }
    return true;
}

bool DispatchPathTraceProducerLaneB(RtPathTraceAccelCpuSnapshot&& snapshot)
{
    const std::uint64_t startUs = ProducerMicroseconds();
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
    const bool epochValid = snapshot.epoch.generation != 0 &&
        snapshot.epoch.capturedAfterBeginFrame &&
        snapshot.epoch.capturedAfterStaticPreload &&
        !snapshot.epoch.capturedBeforeSerialMutate;
#else
    const bool epochValid = PathTraceAccelCpuEpochValid(snapshot.epoch);
#endif
    if (!PathTraceProducerLaneBActive() || !snapshot.complete ||
        !epochValid ||
        snapshot.epoch.generation == 0 || snapshot.inputReceipt == 0 ||
        snapshot.OwnedBytes() > RT_PT_ACCEL_CPU_SLOT_MAX_BYTES)
    {
        std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
        ++g_host.telemetry.laneBCapFallback;
        return false;
    }
    const std::uint64_t generation = snapshot.epoch.generation;
    const std::size_t slotIndex = generation % RT_PT_PRODUCER_LANE_A_SLOT_COUNT;
    LaneBSlot& slot = g_host.laneBSlots[slotIndex];
    if (slot.state.load(std::memory_order_acquire) ==
            RtPathTraceProducerSlotState::Ready)
    {
        std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
        if (slot.state.load(std::memory_order_acquire) ==
                RtPathTraceProducerSlotState::Ready &&
            !slot.product.complete)
        {
            ResetLaneBSlot(slot);
            std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
            ++g_host.telemetry.laneBRetired;
            g_host.laneBSlotResidentBytes[slotIndex] = 0;
        }
    }
    RtPathTraceProducerSlotState expected = RtPathTraceProducerSlotState::Free;
    if (!slot.state.compare_exchange_strong(expected,
            RtPathTraceProducerSlotState::Queued,
            std::memory_order_acq_rel))
    {
        std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
        ++g_host.telemetry.laneBBusyFallback;
        return false;
    }
    {
        std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
        slot.generation = generation;
        slot.snapshot = std::move(snapshot);
        slot.product.ResetAndRelease();
        slot.oracle = RtPathTraceAccelCpuOracle();
        slot.oracleReady.store(false, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
        ++g_host.telemetry.laneBDispatched;
        g_host.telemetry.laneBSnapshotUs += ProducerMicroseconds() - startUs;
        g_host.laneBSlotResidentBytes[slotIndex] = slot.snapshot.OwnedBytes();
    }
    ProducerLaneThread& lane = g_host.lanes[1];
    {
        std::lock_guard<std::mutex> lock(lane.mutex);
        lane.pendingSlots |= static_cast<std::uint8_t>(1u << slotIndex);
    }
    lane.condition.notify_one();
    return true;
}

bool RecordPathTraceProducerLaneBSerialOracle(
    const RtPathTraceAccelCpuOracle& oracle)
{
    if (!oracle.complete || oracle.epoch.generation == 0) return false;
    const std::size_t slotIndex = oracle.epoch.generation %
        RT_PT_PRODUCER_LANE_A_SLOT_COUNT;
    LaneBSlot& slot = g_host.laneBSlots[slotIndex];
    if (slot.generation != oracle.epoch.generation) return false;
    std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
    if (slot.generation != oracle.epoch.generation) return false;
    slot.oracle = oracle;
    slot.oracleReady.store(true, std::memory_order_release);
    return true;
}

bool TryConsumeLatestPathTraceProducerLaneB(
    std::uint64_t currentGeneration,
    const RtPathTraceAccelCpuCompatibilityToken& currentToken,
    std::uint64_t currentReceipt,
    RtPathTraceAccelCpuProduct& product,
    std::uint32_t& productAge)
{
    product.ResetAndRelease();
    productAge = UINT32_MAX;
    if (!PathTraceProducerLaneBActive() ||
        g_host.mode.load(std::memory_order_acquire) != 2 ||
        currentGeneration == 0 || currentReceipt == 0)
    {
        std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
        ++g_host.telemetry.laneBConsumeMiss;
        return false;
    }

    std::size_t selectedIndex = RT_PT_PRODUCER_LANE_A_SLOT_COUNT;
    std::uint64_t selectedGeneration = 0;
    for (std::size_t slotIndex = 0;
         slotIndex < RT_PT_PRODUCER_LANE_A_SLOT_COUNT; ++slotIndex)
    {
        LaneBSlot& slot = g_host.laneBSlots[slotIndex];
        if (slot.state.load(std::memory_order_acquire) !=
                RtPathTraceProducerSlotState::Ready)
            continue;
        std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
        if (slot.state.load(std::memory_order_acquire) !=
                RtPathTraceProducerSlotState::Ready ||
            slot.generation == 0 || !slot.product.complete)
            continue;
        if (slot.generation > currentGeneration)
        {
            ResetLaneBSlot(slot);
            std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
            ++g_host.telemetry.laneBStaleReject;
            ++g_host.telemetry.laneBRetired;
            g_host.laneBSlotResidentBytes[slotIndex] = 0;
            continue;
        }
        if (!HostAccelCpuTokensCompatible(
                slot.product.compatibility, currentToken))
        {
            ResetLaneBSlot(slot);
            std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
            ++g_host.telemetry.laneBStaleReject;
            ++g_host.telemetry.laneBRetired;
            g_host.laneBSlotResidentBytes[slotIndex] = 0;
            continue;
        }
        if (slot.product.inputReceipt != currentReceipt)
        {
            ResetLaneBSlot(slot);
            std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
            ++g_host.telemetry.laneBReceiptReject;
            ++g_host.telemetry.laneBRetired;
            g_host.laneBSlotResidentBytes[slotIndex] = 0;
            continue;
        }
        if (slot.generation > selectedGeneration)
        {
            selectedGeneration = slot.generation;
            selectedIndex = slotIndex;
        }
    }

    if (selectedIndex == RT_PT_PRODUCER_LANE_A_SLOT_COUNT)
    {
        std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
        ++g_host.telemetry.laneBConsumeMiss;
        return false;
    }
    LaneBSlot& selected = g_host.laneBSlots[selectedIndex];
    std::lock_guard<std::mutex> ownershipLock(selected.ownershipMutex);
    if (selected.state.load(std::memory_order_acquire) !=
            RtPathTraceProducerSlotState::Ready ||
        selected.generation != selectedGeneration ||
        !selected.product.complete)
    {
        std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
        ++g_host.telemetry.laneBConsumeMiss;
        return false;
    }
#if !defined(RT_PT_PRODUCER_LANES_HARNESS)
    if (!ValidatePathTraceAccelCpuProduct(selected.product))
    {
        ResetLaneBSlot(selected);
        std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
        ++g_host.telemetry.laneBLifecycleFallback;
        ++g_host.telemetry.laneBConsumeMiss;
        g_host.laneBSlotResidentBytes[selectedIndex] = 0;
        return false;
    }
#endif
    productAge = static_cast<std::uint32_t>(
        currentGeneration - selectedGeneration);
    product = std::move(selected.product);
    product.consumerSlotIndex = static_cast<std::uint32_t>(selectedIndex);
    product.consumerGeneration = selectedGeneration;
    selected.snapshot.ResetAndRelease();
    selected.oracle = RtPathTraceAccelCpuOracle();
    selected.oracleReady.store(false, std::memory_order_release);
    selected.state.store(RtPathTraceProducerSlotState::Consuming,
        std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
        ++g_host.telemetry.laneBConsumeHit;
        ++g_host.telemetry.laneBConsumedAge[std::min<std::uint32_t>(
            productAge, 2u)];
        g_host.laneBSlotResidentBytes[selectedIndex] = product.OwnedBytes();
    }
    return true;
}

void ReleasePathTraceProducerLaneBProduct(RtPathTraceAccelCpuProduct& product)
{
    const std::uint32_t slotIndex = product.consumerSlotIndex;
    const std::uint64_t generation = product.consumerGeneration;
    if (slotIndex < RT_PT_PRODUCER_LANE_A_SLOT_COUNT && generation != 0)
    {
        LaneBSlot& slot = g_host.laneBSlots[slotIndex];
        std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
        if (slot.generation == generation &&
            slot.state.load(std::memory_order_acquire) ==
                RtPathTraceProducerSlotState::Consuming)
        {
            ResetLaneBSlot(slot);
            std::lock_guard<std::mutex> telemetryLock(g_host.telemetryMutex);
            g_host.laneBSlotResidentBytes[slotIndex] = 0;
        }
    }
    product.ResetAndRelease();
}

void RecordPathTraceProducerLaneBTiming(
    std::uint64_t snapshotUs,
    std::uint64_t applyUs,
    std::uint64_t fallbackUs)
{
    std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
    g_host.telemetry.laneBSnapshotUs += snapshotUs;
    g_host.telemetry.laneBApplyUs += applyUs;
    g_host.telemetry.laneBFallbackUs += fallbackUs;
}

void RecordPathTraceProducerLaneBRigidApplyOutcome(bool productApplied)
{
    std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
    if (productApplied) ++g_host.telemetry.laneBRigidProductApply;
    else ++g_host.telemetry.laneBRigidSerialPreserve;
}

void RecordPathTraceProducerLaneBOwnershipTransition()
{
    std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
    ++g_host.telemetry.laneBOwnershipTransitions;
}

void RecordPathTraceProducerLaneBCapAttempt(std::size_t attemptedBytes)
{
    std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
    ++g_host.telemetry.laneBCapFallback;
    g_host.telemetry.laneBAttemptedHighWater = std::max(
        g_host.telemetry.laneBAttemptedHighWater, attemptedBytes);
}

void RecordPathTraceProducerCoordinatorTiming(
    std::uint64_t snapshotUs,
    std::uint64_t mainThreadRemainderUs)
{
    std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
    g_host.telemetry.snapshotUs += snapshotUs;
    g_host.telemetry.mainThreadRemainderUs += mainThreadRemainderUs;
    g_host.telemetry.coordinatorUs += snapshotUs + mainThreadRemainderUs;
}

void RecordPathTraceProducerHarvestTiming(std::uint64_t harvestUs)
{
    std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
    g_host.telemetry.harvestUs += harvestUs;
    g_host.telemetry.coordinatorUs += harvestUs;
}

void RecordPathTraceProducerSnapshotFallback(std::uint64_t snapshotUs)
{
    std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
    ++g_host.telemetry.capFallback;
    g_host.telemetry.snapshotUs += snapshotUs;
    g_host.telemetry.coordinatorUs += snapshotUs;
}

void RecordPathTraceProducerLaneARigidOutcome(
    std::size_t deferred, std::size_t replayed,
    std::size_t applied, bool fallback)
{
    std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
    g_host.telemetry.rigidDeferred += deferred;
    g_host.telemetry.rigidReplayed += replayed;
    g_host.telemetry.rigidApplied += applied;
    g_host.telemetry.rigidApplyFallback += fallback ? 1u : 0u;
}

void PollPathTraceProducerLanes(
    std::uint64_t currentGeneration, bool logTelemetry)
{
    OPTICK_EVENT("PT Producer Lanes Commit Compare");
    const bool shadowModeActive =
        g_host.mode.load(std::memory_order_acquire) == 1;
    for (std::size_t slotIndex = 0;
        slotIndex < RT_PT_PRODUCER_LANE_A_SLOT_COUNT; ++slotIndex)
    {
        LaneASlot& slot = g_host.slots[slotIndex];
        const RtPathTraceProducerSlotState state =
            slot.state.load(std::memory_order_acquire);
        const bool oracleReady = slot.oracleReady.load(std::memory_order_acquire);
        if (state == RtPathTraceProducerSlotState::Ready && oracleReady)
        {
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
            RtPathTraceCaptureComparison comparison;
            comparison.generation = slot.product.epoch.generation;
            if (!slot.product.complete || !slot.oracle.complete)
            {
                comparison.geometryMismatches = 1;
                comparison.proposalMismatches = 1;
                comparison.firstFail = RtPathTraceCaptureFirstFailure::Incomplete;
            }
            else if (!RtPathTracePlanningEpochsMatch(
                    slot.product.epoch, slot.oracle.epoch))
            {
                comparison.geometryMismatches = 1;
                comparison.proposalMismatches = 1;
                comparison.firstFail = RtPathTraceCaptureFirstFailure::Epoch;
            }
            else
            {
                comparison.cardinality =
                    RtPathTraceBuildCaptureCardinalityAttribution(
                        slot.product.acceptedSurfaces,
                        slot.product.acceptedVertices,
                        slot.oracle.acceptedSurfaces,
                        slot.oracle.acceptedVertices,
                        slot.oracle.liveCardinality);
                comparison.cardinalityAvailable = true;
                comparison.acceptedSetDiff = slot.product.acceptedSetDiff;
                if (slot.product.exact != slot.oracle.exact)
                {
                    comparison.proposalMismatches = 1;
                    comparison.firstFail =
                        RtPathTraceCaptureFirstFailure::SurfaceDecision;
                    comparison.firstSurfaceOrdinal = 0;
                }
            }
            comparison.exact = comparison.geometryMismatches == 0 &&
                comparison.proposalMismatches == 0;
#else
            const RtPathTraceCaptureComparison comparison =
                ComparePathTraceCaptureProduct(slot.product, slot.oracle);
#endif
            bool printFirstMismatch = false;
            std::size_t firstProductSize = 0;
            std::size_t firstOracleSize = 0;
            {
                std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
                ++g_host.telemetry.compared;
                if (shadowModeActive && comparison.cardinalityAvailable)
                {
                    ++g_host.telemetry.cardinalitySamples;
                    AccumulateCaptureCardinality(
                        g_host.telemetry.cardinalitySums,
                        comparison.cardinality);
                }
                if (shadowModeActive && comparison.acceptedSetDiff.available &&
                    comparison.acceptedSetDiff.found)
                {
                    ++g_host.telemetry.acceptedSetDiffs;
                    const bool productAccepted =
                        comparison.acceptedSetDiff.product.present &&
                        comparison.acceptedSetDiff.product.terminal ==
                            RT_PT_CAPTURE_TERMINAL_ACCEPTED_SCALAR;
                    const bool oracleAccepted =
                        comparison.acceptedSetDiff.oracle.present &&
                        comparison.acceptedSetDiff.oracle.terminal ==
                            RT_PT_CAPTURE_TERMINAL_ACCEPTED_SCALAR;
                    g_host.telemetry.productOnlyAccepted +=
                        productAccepted && !oracleAccepted;
                    g_host.telemetry.oracleOnlyAccepted +=
                        oracleAccepted && !productAccepted;
                }
                if (comparison.exact)
                {
                    ++g_host.telemetry.exact;
                }
                else
                {
                    ++g_host.telemetry.mismatch;
                    if (shadowModeActive)
                    {
                        const std::size_t failureIndex = static_cast<std::size_t>(
                            comparison.firstFail);
                        if (failureIndex < RT_PT_CAPTURE_FIRST_FAILURE_COUNT)
                        {
                            ++g_host.telemetry.firstFailHistogram[failureIndex];
                        }
                        if (CaptureFirstFailureIsEarly(comparison.firstFail))
                        {
                            ++g_host.telemetry.jobsEarly;
                        }
                        else
                        {
                            g_host.telemetry.jobsGeom +=
                                comparison.geometryMismatches != 0;
                            g_host.telemetry.jobsProp +=
                                comparison.proposalMismatches != 0;
                        }
                        if (!g_host.firstMismatchPrinted)
                        {
                            g_host.firstMismatchPrinted = true;
                            g_host.telemetry.firstMismatchGeneration =
                                comparison.generation;
                            g_host.telemetry.firstMismatchCardinality =
                                comparison.cardinality;
                            g_host.telemetry.firstMismatchAcceptedSetDiff =
                                comparison.acceptedSetDiff;
                            g_host.telemetry.firstMismatchInstanceObservationDiff =
                                comparison.instanceObservationDiff;
                            printFirstMismatch = true;
#if !defined(RT_PT_PRODUCER_LANES_HARNESS)
                            CaptureFirstFailureVectorSizes(comparison.firstFail,
                                slot.product, slot.oracle,
                                firstProductSize, firstOracleSize);
#endif
                        }
                    }
                }
            }
#if !defined(RT_PT_PRODUCER_LANES_HARNESS)
            if (printFirstMismatch)
            {
                const RtPathTraceCaptureCardinalityAttribution& cardinality =
                    comparison.cardinality;
                common->Printf("PathTracePrimaryPass: producerLanes firstMismatch generation=%llu category=%s firstSurfaceOrdinal=%u relevantSize(product/oracle)=%llu/%llu geometryMismatches=%u proposalMismatches=%u accepted(product/oracle surfaces/verts)=%llu/%llu,%llu/%llu liveSkinnedOmitted(s/v/i)=%llu/%llu/%llu liveRigidRemoved(s/v/i)=%llu/%llu/%llu vertexDelta(afterSkinned/unattributed)=%lld/%lld acceptedSet(found/ordinal)=%d/%u product(present/terminal/class/flags/v/i/mesh/resident/omit/admission)=%d/%u/%u/%u/%u/%u/%d/%d/%d/%u oracle=%d/%u/%u/%u/%u/%u/%d/%d/%d/%u observationDiff(found/ordinal/productPrev/productContinuous/oraclePrev/oracleContinuous)=%d/%u/%d/%d/%d/%d\n",
                    static_cast<unsigned long long>(comparison.generation),
                    CaptureFirstFailureName(comparison.firstFail),
                    comparison.firstSurfaceOrdinal,
                    static_cast<unsigned long long>(firstProductSize),
                    static_cast<unsigned long long>(firstOracleSize),
                    comparison.geometryMismatches,
                    comparison.proposalMismatches,
                    static_cast<unsigned long long>(
                        cardinality.productAcceptedSurfaces),
                    static_cast<unsigned long long>(
                        cardinality.productAcceptedVertices),
                    static_cast<unsigned long long>(
                        cardinality.oracleAcceptedSurfaces),
                    static_cast<unsigned long long>(
                        cardinality.oracleAcceptedVertices),
                    static_cast<unsigned long long>(
                        cardinality.live.skinnedOmittedSurfaces),
                    static_cast<unsigned long long>(
                        cardinality.live.skinnedOmittedVertices),
                    static_cast<unsigned long long>(
                        cardinality.live.skinnedOmittedIndexes),
                    static_cast<unsigned long long>(
                        cardinality.live.rigidRemovedSurfaces),
                    static_cast<unsigned long long>(
                        cardinality.live.rigidRemovedVertices),
                    static_cast<unsigned long long>(
                        cardinality.live.rigidRemovedIndexes),
                    static_cast<long long>(cardinality.afterSkinnedVertexDelta),
                    static_cast<long long>(cardinality.unattributedVertexDelta),
                    comparison.acceptedSetDiff.found ? 1 : 0,
                    comparison.acceptedSetDiff.ordinal,
                    comparison.acceptedSetDiff.product.present ? 1 : 0,
                    comparison.acceptedSetDiff.product.terminal,
                    comparison.acceptedSetDiff.product.surfaceClass,
                    comparison.acceptedSetDiff.product.sourceFlags,
                    comparison.acceptedSetDiff.product.vertexCount,
                    comparison.acceptedSetDiff.product.indexCount,
                    comparison.acceptedSetDiff.product.rigidReadyByMesh ? 1 : 0,
                    comparison.acceptedSetDiff.product.rigidReadyByResident ? 1 : 0,
                    comparison.acceptedSetDiff.product.skinnedCaptureOmitted ? 1 : 0,
                    comparison.acceptedSetDiff.product.skinnedAdmission,
                    comparison.acceptedSetDiff.oracle.present ? 1 : 0,
                    comparison.acceptedSetDiff.oracle.terminal,
                    comparison.acceptedSetDiff.oracle.surfaceClass,
                    comparison.acceptedSetDiff.oracle.sourceFlags,
                    comparison.acceptedSetDiff.oracle.vertexCount,
                    comparison.acceptedSetDiff.oracle.indexCount,
                    comparison.acceptedSetDiff.oracle.rigidReadyByMesh ? 1 : 0,
                    comparison.acceptedSetDiff.oracle.rigidReadyByResident ? 1 : 0,
                    comparison.acceptedSetDiff.oracle.skinnedCaptureOmitted ? 1 : 0,
                    comparison.acceptedSetDiff.oracle.skinnedAdmission,
                    comparison.instanceObservationDiff.found ? 1 : 0,
                    comparison.instanceObservationDiff.ordinal,
                    comparison.instanceObservationDiff.productHasPrevious ? 1 : 0,
                    comparison.instanceObservationDiff.productTransformContinuous ? 1 : 0,
                    comparison.instanceObservationDiff.oracleHasPrevious ? 1 : 0,
                    comparison.instanceObservationDiff.oracleTransformContinuous ? 1 : 0);
            }
#else
            (void)printFirstMismatch;
            (void)firstProductSize;
            (void)firstOracleSize;
#endif
            ResetSlot(slot);
            std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
            g_host.slotResidentBytes[slotIndex] = 0;
        }
        else if (slot.generation != 0 && currentGeneration > slot.generation +
            (shadowModeActive ? RT_PT_PRODUCER_LANE_A_SLOT_COUNT : 1u))
        {
            if (state == RtPathTraceProducerSlotState::Ready)
            {
                std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
                ++g_host.telemetry.late;
                ++g_host.telemetry.retired;
                ResetSlot(slot);
                g_host.slotResidentBytes[slotIndex] = 0;
            }
        }
    }

    if (PathTraceProducerLaneBActive())
    {
        for (std::size_t slotIndex = 0;
             slotIndex < RT_PT_PRODUCER_LANE_A_SLOT_COUNT; ++slotIndex)
        {
            LaneBSlot& slot = g_host.laneBSlots[slotIndex];
            const RtPathTraceProducerSlotState state =
                slot.state.load(std::memory_order_acquire);
            if (shadowModeActive && state == RtPathTraceProducerSlotState::Ready &&
                slot.oracleReady.load(std::memory_order_acquire))
            {
                std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
                bool exact = false;
#if defined(RT_PT_PRODUCER_LANES_HARNESS)
                exact = slot.product.complete && slot.oracle.complete &&
                    slot.product.epoch.generation == slot.oracle.epoch.generation &&
                    slot.product.epoch.frameIndex == slot.oracle.epoch.frameIndex &&
                    slot.product.epoch.mapTimeStamp == slot.oracle.epoch.mapTimeStamp &&
                    slot.product.epoch.mapLoadSerial == slot.oracle.epoch.mapLoadSerial &&
                    slot.product.inputReceipt == slot.oracle.inputReceipt &&
                    slot.product.rigidSignature == slot.oracle.rigidSignature &&
                    slot.product.accelerationSignature ==
                        slot.oracle.accelerationSignature &&
                    slot.product.staticSignature == slot.oracle.staticSignature;
#else
                exact = ComparePathTraceAccelCpuProduct(
                    slot.product, slot.oracle).exact;
#endif
                {
                    std::lock_guard<std::mutex> telemetryLock(
                        g_host.telemetryMutex);
                    ++g_host.telemetry.laneBCompared;
                    if (exact) ++g_host.telemetry.laneBExact;
                    else ++g_host.telemetry.laneBMismatch;
                    g_host.laneBSlotResidentBytes[slotIndex] = 0;
                }
                ResetLaneBSlot(slot);
            }
            else if (slot.generation != 0 &&
                currentGeneration > slot.generation +
                    (shadowModeActive
                        ? RT_PT_PRODUCER_LANE_A_SLOT_COUNT : 1u) &&
                state == RtPathTraceProducerSlotState::Ready)
            {
                std::lock_guard<std::mutex> ownershipLock(slot.ownershipMutex);
                ResetLaneBSlot(slot);
                std::lock_guard<std::mutex> telemetryLock(
                    g_host.telemetryMutex);
                ++g_host.telemetry.laneBLate;
                ++g_host.telemetry.laneBRetired;
                g_host.laneBSlotResidentBytes[slotIndex] = 0;
            }
        }
    }
    if (logTelemetry && shadowModeActive)
    {
        {
            std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
            ++g_host.telemetry.shadowSummaryCadences;
        }
        const RtPathTraceProducerLaneTelemetry t = PathTraceProducerLanesTelemetry();
#if !defined(RT_PT_PRODUCER_LANES_HARNESS)
        const int effectiveMask = PathTraceProducerLaneEffectiveMask();
        common->Printf("PathTracePrimaryPass: producerLanes mode=shadow lanes=A(%s),B(%s),C(not-started) dispatched=%llu complete=%llu compare=%llu exact=%llu mismatch=%llu compareFail(early/geom/prop)=%llu/%llu/%llu firstFail(i/e/v/vtx/idx/tc/tm/ti/tid/mi/mv/io/rc/sm/rr/sc/sd)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu attr(samples=%llu acceptedProduct(s/v)=%llu/%llu acceptedOracle(s/v)=%llu/%llu skinnedOmitted(s/v/i)=%llu/%llu/%llu rigidRemoved(s/v/i)=%llu/%llu/%llu vertexDelta(afterSkinned/unattributed)=%lld/%lld acceptedSet(diff/productOnly/oracleOnly)=%llu/%llu/%llu) late=%llu retired=%llu fallback(busy/cap/lifecycle)=%llu/%llu/%llu timing(snapshot/dispatch/coordinator/mainRemainderUs)=%llu/%llu/%llu/%llu laneJobs=%llu/%llu/%llu laneCpuUs=%llu/%llu/%llu history(rows/bytes/copyUs)=%llu/%llu/%llu slotHighWater=%llu,%llu,%llu ringResident=%llu\n",
            (effectiveMask & 1) != 0 ? "active" : "not-started",
            (effectiveMask & 2) != 0 ? "active" : "not-started",
            static_cast<unsigned long long>(t.dispatched),
            static_cast<unsigned long long>(t.completed),
            static_cast<unsigned long long>(t.compared),
            static_cast<unsigned long long>(t.exact),
            static_cast<unsigned long long>(t.mismatch),
            static_cast<unsigned long long>(t.jobsEarly),
            static_cast<unsigned long long>(t.jobsGeom),
            static_cast<unsigned long long>(t.jobsProp),
            static_cast<unsigned long long>(t.firstFailHistogram[1]),
            static_cast<unsigned long long>(t.firstFailHistogram[2]),
            static_cast<unsigned long long>(t.firstFailHistogram[3]),
            static_cast<unsigned long long>(t.firstFailHistogram[4]),
            static_cast<unsigned long long>(t.firstFailHistogram[5]),
            static_cast<unsigned long long>(t.firstFailHistogram[6]),
            static_cast<unsigned long long>(t.firstFailHistogram[7]),
            static_cast<unsigned long long>(t.firstFailHistogram[8]),
            static_cast<unsigned long long>(t.firstFailHistogram[9]),
            static_cast<unsigned long long>(t.firstFailHistogram[10]),
            static_cast<unsigned long long>(t.firstFailHistogram[11]),
            static_cast<unsigned long long>(t.firstFailHistogram[12]),
            static_cast<unsigned long long>(t.firstFailHistogram[13]),
            static_cast<unsigned long long>(t.firstFailHistogram[14]),
            static_cast<unsigned long long>(t.firstFailHistogram[15]),
            static_cast<unsigned long long>(t.firstFailHistogram[16]),
            static_cast<unsigned long long>(t.firstFailHistogram[17]),
            static_cast<unsigned long long>(t.cardinalitySamples),
            static_cast<unsigned long long>(
                t.cardinalitySums.productAcceptedSurfaces),
            static_cast<unsigned long long>(
                t.cardinalitySums.productAcceptedVertices),
            static_cast<unsigned long long>(
                t.cardinalitySums.oracleAcceptedSurfaces),
            static_cast<unsigned long long>(
                t.cardinalitySums.oracleAcceptedVertices),
            static_cast<unsigned long long>(
                t.cardinalitySums.live.skinnedOmittedSurfaces),
            static_cast<unsigned long long>(
                t.cardinalitySums.live.skinnedOmittedVertices),
            static_cast<unsigned long long>(
                t.cardinalitySums.live.skinnedOmittedIndexes),
            static_cast<unsigned long long>(
                t.cardinalitySums.live.rigidRemovedSurfaces),
            static_cast<unsigned long long>(
                t.cardinalitySums.live.rigidRemovedVertices),
            static_cast<unsigned long long>(
                t.cardinalitySums.live.rigidRemovedIndexes),
            static_cast<long long>(
                t.cardinalitySums.afterSkinnedVertexDelta),
            static_cast<long long>(
                t.cardinalitySums.unattributedVertexDelta),
            static_cast<unsigned long long>(t.acceptedSetDiffs),
            static_cast<unsigned long long>(t.productOnlyAccepted),
            static_cast<unsigned long long>(t.oracleOnlyAccepted),
            static_cast<unsigned long long>(t.late),
            static_cast<unsigned long long>(t.retired),
            static_cast<unsigned long long>(t.busyFallback),
            static_cast<unsigned long long>(t.capFallback),
            static_cast<unsigned long long>(t.lifecycleFallback),
            static_cast<unsigned long long>(t.snapshotUs),
            static_cast<unsigned long long>(t.dispatchUs),
            static_cast<unsigned long long>(t.coordinatorUs),
            static_cast<unsigned long long>(t.mainThreadRemainderUs),
            static_cast<unsigned long long>(t.laneJobs[0]),
            static_cast<unsigned long long>(t.laneJobs[1]),
            static_cast<unsigned long long>(t.laneJobs[2]),
            static_cast<unsigned long long>(t.laneCpuUs[0]),
            static_cast<unsigned long long>(t.laneCpuUs[1]),
            static_cast<unsigned long long>(t.laneCpuUs[2]),
            static_cast<unsigned long long>(t.historyRows),
            static_cast<unsigned long long>(t.historyBytes),
            static_cast<unsigned long long>(t.historyCopyUs),
            static_cast<unsigned long long>(t.slotHighWater[0]),
            static_cast<unsigned long long>(t.slotHighWater[1]),
            static_cast<unsigned long long>(t.slotHighWater[2]),
            static_cast<unsigned long long>(t.laneARingResidentBytes));
#else
        (void)t;
#endif
        if (PathTraceProducerLaneBActive())
        {
#if !defined(RT_PT_PRODUCER_LANES_HARNESS)
            common->Printf("PathTracePrimaryPass: producerLaneB mode=shadow dispatched/complete/compare/exact/mismatch=%llu/%llu/%llu/%llu/%llu late/retired=%llu/%llu fallback(busy/cap/lifecycle)=%llu/%llu/%llu timing(snapshot/build/apply/fallbackUs)=%llu/%llu/%llu/%llu rigid(apply/preserve)=%llu/%llu age(hit/miss/age0/age1)=%llu/%llu/%llu/%llu reject(stale/receipt)=%llu/%llu transitions=%llu attemptedHighWater=%llu slotHighWater=%llu,%llu,%llu ringResident=%llu\n",
                static_cast<unsigned long long>(t.laneBDispatched),
                static_cast<unsigned long long>(t.laneBCompleted),
                static_cast<unsigned long long>(t.laneBCompared),
                static_cast<unsigned long long>(t.laneBExact),
                static_cast<unsigned long long>(t.laneBMismatch),
                static_cast<unsigned long long>(t.laneBLate),
                static_cast<unsigned long long>(t.laneBRetired),
                static_cast<unsigned long long>(t.laneBBusyFallback),
                static_cast<unsigned long long>(t.laneBCapFallback),
                static_cast<unsigned long long>(t.laneBLifecycleFallback),
                static_cast<unsigned long long>(t.laneBSnapshotUs),
                static_cast<unsigned long long>(t.laneBBuildUs),
                static_cast<unsigned long long>(t.laneBApplyUs),
                static_cast<unsigned long long>(t.laneBFallbackUs),
                static_cast<unsigned long long>(t.laneBRigidProductApply),
                static_cast<unsigned long long>(t.laneBRigidSerialPreserve),
                static_cast<unsigned long long>(t.laneBConsumeHit),
                static_cast<unsigned long long>(t.laneBConsumeMiss),
                static_cast<unsigned long long>(t.laneBConsumedAge[0]),
                static_cast<unsigned long long>(t.laneBConsumedAge[1]),
                static_cast<unsigned long long>(t.laneBStaleReject),
                static_cast<unsigned long long>(t.laneBReceiptReject),
                static_cast<unsigned long long>(t.laneBOwnershipTransitions),
                static_cast<unsigned long long>(t.laneBAttemptedHighWater),
                static_cast<unsigned long long>(t.laneBSlotHighWater[0]),
                static_cast<unsigned long long>(t.laneBSlotHighWater[1]),
                static_cast<unsigned long long>(t.laneBSlotHighWater[2]),
                static_cast<unsigned long long>(t.laneBRingResidentBytes));
#endif
        }
    }
    else if (logTelemetry &&
        g_host.mode.load(std::memory_order_acquire) == 2)
    {
        const RtPathTraceProducerLaneTelemetry t =
            PathTraceProducerLanesTelemetry();
#if !defined(RT_PT_PRODUCER_LANES_HARNESS)
        const int effectiveMask = PathTraceProducerLaneEffectiveMask();
        common->Printf("PathTracePrimaryPass: producerLanes mode=consume lanes=A(%s),B(%s),C(not-started) consume(hit/miss/age0/age1)=%llu/%llu/%llu/%llu reject(stale/receipt/skinned)=%llu/%llu/%llu late=%llu retired=%llu fallback(busy/cap/lifecycle)=%llu/%llu/%llu rigid(deferred/replayed/applied/fallback)=%llu/%llu/%llu/%llu timing(snapshot/harvest/dispatch/coordinator/mainRemainderUs)=%llu/%llu/%llu/%llu/%llu slotHighWater=%llu,%llu,%llu ringResident=%llu\n",
            (effectiveMask & 1) != 0 ? "active" : "not-started",
            (effectiveMask & 2) != 0 ? "active" : "not-started",
            static_cast<unsigned long long>(t.consumeHit),
            static_cast<unsigned long long>(t.consumeMiss),
            static_cast<unsigned long long>(t.consumedAge[0]),
            static_cast<unsigned long long>(t.consumedAge[1]),
            static_cast<unsigned long long>(t.staleReject),
            static_cast<unsigned long long>(t.receiptMismatch),
            static_cast<unsigned long long>(t.skinnedEligibilityFail),
            static_cast<unsigned long long>(t.late),
            static_cast<unsigned long long>(t.retired),
            static_cast<unsigned long long>(t.busyFallback),
            static_cast<unsigned long long>(t.capFallback),
            static_cast<unsigned long long>(t.lifecycleFallback),
            static_cast<unsigned long long>(t.rigidDeferred),
            static_cast<unsigned long long>(t.rigidReplayed),
            static_cast<unsigned long long>(t.rigidApplied),
            static_cast<unsigned long long>(t.rigidApplyFallback),
            static_cast<unsigned long long>(t.snapshotUs),
            static_cast<unsigned long long>(t.harvestUs),
            static_cast<unsigned long long>(t.dispatchUs),
            static_cast<unsigned long long>(t.coordinatorUs),
            static_cast<unsigned long long>(t.mainThreadRemainderUs),
            static_cast<unsigned long long>(t.slotHighWater[0]),
            static_cast<unsigned long long>(t.slotHighWater[1]),
            static_cast<unsigned long long>(t.slotHighWater[2]),
            static_cast<unsigned long long>(t.laneARingResidentBytes));
        if (PathTraceProducerLaneBActive())
        {
            common->Printf("PathTracePrimaryPass: producerLaneB mode=consume dispatched/complete=%llu/%llu consume(hit/miss/age0/age1/age2plus)=%llu/%llu/%llu/%llu/%llu reject(stale/receipt)=%llu/%llu late/retired=%llu/%llu fallback(busy/cap/lifecycle)=%llu/%llu/%llu timing(snapshot/build/apply/fallbackUs)=%llu/%llu/%llu/%llu rigid(apply/preserve)=%llu/%llu transitions=%llu attemptedHighWater=%llu slotHighWater=%llu,%llu,%llu ringResident=%llu\n",
                static_cast<unsigned long long>(t.laneBDispatched),
                static_cast<unsigned long long>(t.laneBCompleted),
                static_cast<unsigned long long>(t.laneBConsumeHit),
                static_cast<unsigned long long>(t.laneBConsumeMiss),
                static_cast<unsigned long long>(t.laneBConsumedAge[0]),
                static_cast<unsigned long long>(t.laneBConsumedAge[1]),
                static_cast<unsigned long long>(t.laneBConsumedAge[2]),
                static_cast<unsigned long long>(t.laneBStaleReject),
                static_cast<unsigned long long>(t.laneBReceiptReject),
                static_cast<unsigned long long>(t.laneBLate),
                static_cast<unsigned long long>(t.laneBRetired),
                static_cast<unsigned long long>(t.laneBBusyFallback),
                static_cast<unsigned long long>(t.laneBCapFallback),
                static_cast<unsigned long long>(t.laneBLifecycleFallback),
                static_cast<unsigned long long>(t.laneBSnapshotUs),
                static_cast<unsigned long long>(t.laneBBuildUs),
                static_cast<unsigned long long>(t.laneBApplyUs),
                static_cast<unsigned long long>(t.laneBFallbackUs),
                static_cast<unsigned long long>(t.laneBRigidProductApply),
                static_cast<unsigned long long>(t.laneBRigidSerialPreserve),
                static_cast<unsigned long long>(t.laneBOwnershipTransitions),
                static_cast<unsigned long long>(t.laneBAttemptedHighWater),
                static_cast<unsigned long long>(t.laneBSlotHighWater[0]),
                static_cast<unsigned long long>(t.laneBSlotHighWater[1]),
                static_cast<unsigned long long>(t.laneBSlotHighWater[2]),
                static_cast<unsigned long long>(t.laneBRingResidentBytes));
        }
#endif
    }
}

void ResetPathTraceProducerLanes()
{
    DrainPathTraceCommittedBaselinePhase1(
        RtPathTraceCommittedBaselineDrainReason::TopologyBarrier);
    std::lock_guard<std::mutex> lifecycleLock(g_host.lifecycleMutex);
    StopThreads();
    for (LaneASlot& slot : g_host.slots)
    {
        ResetSlot(slot);
    }
    for (LaneBSlot& slot : g_host.laneBSlots)
    {
        ResetLaneBSlot(slot);
    }
    std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
    for (std::size_t& bytes : g_host.slotResidentBytes)
    {
        bytes = 0;
    }
    for (std::size_t& bytes : g_host.laneBSlotResidentBytes) bytes = 0;
}

void ShutdownPathTraceProducerLanes()
{
    ResetPathTraceProducerLanes();
    DrainPathTraceCommittedBaselinePhase1(
        RtPathTraceCommittedBaselineDrainReason::Shutdown);
    g_host.mode.store(0, std::memory_order_release);
}

RtPathTraceProducerLaneTelemetry PathTraceProducerLanesTelemetry()
{
    std::lock_guard<std::mutex> lock(g_host.telemetryMutex);
    RtPathTraceProducerLaneTelemetry telemetry = g_host.telemetry;
    telemetry.laneARingResidentBytes = 0;
    for (std::size_t slotIndex = 0;
        slotIndex < RT_PT_PRODUCER_LANE_A_SLOT_COUNT; ++slotIndex)
    {
        telemetry.laneARingResidentBytes += g_host.slotResidentBytes[slotIndex];
        telemetry.laneBRingResidentBytes +=
            g_host.laneBSlotResidentBytes[slotIndex];
    }
    return telemetry;
}

#if defined(RT_PT_PRODUCER_LANES_HARNESS)
void PathTraceProducerLanesTestSetStartFailureAfter(int startedThreadCount)
{
    std::lock_guard<std::mutex> lock(g_host.lifecycleMutex);
    g_host.startFailureAfter = startedThreadCount;
}

void PathTraceProducerLanesTestSetRequiredLaneMaskOverride(int laneMask)
{
    std::lock_guard<std::mutex> lock(g_host.lifecycleMutex);
    g_host.requiredLaneMaskOverride = laneMask;
}

void PathTraceProducerLanesTestPauseAfterCommit(bool pause)
{
    g_host.pauseAfterCommit.store(pause, std::memory_order_release);
}

bool PathTraceProducerLanesTestCommitPaused()
{
    return g_host.commitPaused.load(std::memory_order_acquire);
}

void PathTraceProducerLanesTestPauseDuringAccounting(bool pause)
{
    g_host.pauseDuringAccounting.store(pause, std::memory_order_release);
}

bool PathTraceProducerLanesTestAccountingPaused()
{
    return g_host.accountingPaused.load(std::memory_order_acquire);
}

RtPathTraceProducerLaneHostTestState PathTraceProducerLanesTestState()
{
    std::lock_guard<std::mutex> lock(g_host.lifecycleMutex);
    RtPathTraceProducerLaneHostTestState result;
    result.running = g_host.running.load(std::memory_order_acquire);
    result.mode = g_host.mode.load(std::memory_order_acquire);
    result.laneMask = g_host.laneMask.load(std::memory_order_acquire);
    result.startedMask = g_host.startedMask;
    for (std::size_t slotIndex = 0;
        slotIndex < RT_PT_PRODUCER_LANE_A_SLOT_COUNT; ++slotIndex)
    {
        const LaneASlot& slot = g_host.slots[slotIndex];
        result.slots[slotIndex].generation = slot.generation;
        result.slots[slotIndex].state =
            slot.state.load(std::memory_order_acquire);
        result.slots[slotIndex].oracleReady =
            slot.oracleReady.load(std::memory_order_acquire);
        const LaneBSlot& laneBSlot = g_host.laneBSlots[slotIndex];
        result.laneBSlots[slotIndex].generation = laneBSlot.generation;
        result.laneBSlots[slotIndex].state =
            laneBSlot.state.load(std::memory_order_acquire);
        result.laneBSlots[slotIndex].oracleReady =
            laneBSlot.oracleReady.load(std::memory_order_acquire);
    }
    return result;
}
#endif
