#pragma once

// A8-S1 observation-only canonical identity proof helpers.
// W remains legacy. P/R are canonical. No helper in this file compares their
// uint64 hashes numerically or participates in a renderer decision.

#include "PathTraceCanonicalGeometryIdentity.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <algorithm>
#include <string>
#include <vector>

enum class PtA8S1RouteProducer : std::uint32_t
{
    VisibleDrawSurf = 0,
    RoutedReadyProbe,
    CaptureWalkProduct,
    MergedCompanion,
    EntityFeed,
    AreaResidency,
    Count
};

inline bool PtA8S1ProducerCoverageRowReconciles(
    std::uint64_t opportunities,
    std::uint64_t observed,
    std::uint64_t suppressedDiagnosticUnavailable)
{
    return opportunities == observed + suppressedDiagnosticUnavailable;
}

struct PtA8S1NormalizedRouteKey
{
    std::uint64_t worldGeneration = 0;
    std::uint32_t renderDefIndex = UINT32_MAX;
    std::uint32_t renderDefGeneration = 0;
    std::string modelName;
    std::int32_t modelSurfaceIndex = -1;
};

inline bool PtA8S1NormalizedRouteKeyEqual(
    const PtA8S1NormalizedRouteKey& lhs,
    const PtA8S1NormalizedRouteKey& rhs)
{
    return lhs.worldGeneration == rhs.worldGeneration &&
        lhs.renderDefIndex == rhs.renderDefIndex &&
        lhs.renderDefGeneration == rhs.renderDefGeneration &&
        lhs.modelName == rhs.modelName &&
        lhs.modelSurfaceIndex == rhs.modelSurfaceIndex;
}

struct PtA8S1RouteObservation
{
    PtA8S1NormalizedRouteKey route;
    PtCanonicalInstanceKey canonicalInstanceKey;
    std::uint64_t legacyMeshHash = 0;
    bool surfaceIndexValid = false;
    PtA8S1RouteProducer producer = PtA8S1RouteProducer::VisibleDrawSurf;
};

struct PtA8S1PresentView
{
    PtCanonicalInstanceKey instanceKey;
    std::uint64_t instanceHash = 0;
    PtCanonicalMeshKey meshKey;
    std::uint64_t meshHash = 0;
    std::uint64_t lastUpsertSequence = 0;
    std::uint64_t worldGeneration = 0;
    std::uint64_t publicationGeneration = 0;
    std::string modelName;
};

struct PtA8S1BindingView
{
    bool found = false;
    PtCanonicalInstanceKey instanceKey;
    std::uint64_t instanceHash = 0;
    PtCanonicalMeshKey meshKey;
    std::uint64_t meshHash = 0;
    std::uint64_t lastEventSequence = 0;
    std::uint64_t worldGeneration = 0;
    std::uint64_t publicationGeneration = 0;
};

enum PtA8S1RouteAssociationMismatch : std::uint32_t
{
    PT_A8_S1_ROUTE_WORLD_MISMATCH = 1u << 0,
    PT_A8_S1_ROUTE_RENDER_DEF_INDEX_MISMATCH = 1u << 1,
    PT_A8_S1_ROUTE_RENDER_DEF_GENERATION_MISMATCH = 1u << 2,
    PT_A8_S1_ROUTE_MODEL_NAME_MISMATCH = 1u << 3,
    PT_A8_S1_ROUTE_SURFACE_INDEX_MISMATCH = 1u << 4
};

inline std::uint32_t PtA8S1CompareRouteAssociation(
    const PtA8S1NormalizedRouteKey& route,
    const PtA8S1PresentView& present)
{
    std::uint32_t mismatch = 0;
    if (route.worldGeneration != present.instanceKey.worldGeneration)
    {
        mismatch |= PT_A8_S1_ROUTE_WORLD_MISMATCH;
    }
    if (route.renderDefIndex != present.instanceKey.renderDefIndex)
    {
        mismatch |= PT_A8_S1_ROUTE_RENDER_DEF_INDEX_MISMATCH;
    }
    if (route.renderDefGeneration != present.instanceKey.renderDefGeneration)
    {
        mismatch |= PT_A8_S1_ROUTE_RENDER_DEF_GENERATION_MISMATCH;
    }
    if (route.modelName != present.modelName)
    {
        mismatch |= PT_A8_S1_ROUTE_MODEL_NAME_MISMATCH;
    }
    if (route.modelSurfaceIndex < 0 ||
        static_cast<std::uint32_t>(route.modelSurfaceIndex) !=
            present.instanceKey.modelSurfaceIndex)
    {
        mismatch |= PT_A8_S1_ROUTE_SURFACE_INDEX_MISMATCH;
    }
    return mismatch;
}

struct PtA8S1TransportIntegrity
{
    bool instanceKey = false;
    bool instanceHash = false;
    bool meshKey = false;
    bool meshHash = false;

    bool All() const
    {
        return instanceKey && instanceHash && meshKey && meshHash;
    }
};

inline bool PtA8S1CanonicalInstanceKeyEqual(
    const PtCanonicalInstanceKey& lhs,
    const PtCanonicalInstanceKey& rhs)
{
    return lhs.worldGeneration == rhs.worldGeneration &&
        lhs.renderDefIndex == rhs.renderDefIndex &&
        lhs.renderDefGeneration == rhs.renderDefGeneration &&
        lhs.subInstanceKind == rhs.subInstanceKind &&
        lhs.modelSurfaceIndex == rhs.modelSurfaceIndex &&
        lhs.jointSubmeshIndex == rhs.jointSubmeshIndex;
}

inline bool PtA8S1CanonicalMeshKeyEqual(
    const PtCanonicalMeshKey& lhs,
    const PtCanonicalMeshKey& rhs)
{
    return lhs.sourceAssetId == rhs.sourceAssetId &&
        lhs.sourceAssetGeneration == rhs.sourceAssetGeneration &&
        lhs.topologySignature == rhs.topologySignature &&
        lhs.sourceDomain == rhs.sourceDomain &&
        lhs.modelSurfaceIndex == rhs.modelSurfaceIndex &&
        lhs.vertexFormat == rhs.vertexFormat &&
        lhs.deformationClass == rhs.deformationClass &&
        lhs.vertexCount == rhs.vertexCount &&
        lhs.indexCount == rhs.indexCount &&
        lhs.jointSubmeshIndex == rhs.jointSubmeshIndex;
}

inline PtA8S1TransportIntegrity PtA8S1CompareTransportIntegrity(
    const PtA8S1PresentView& present,
    const PtA8S1BindingView& binding)
{
    PtA8S1TransportIntegrity out;
    out.instanceKey = PtA8S1CanonicalInstanceKeyEqual(
        present.instanceKey, binding.instanceKey);
    out.instanceHash = present.instanceHash == binding.instanceHash;
    out.meshKey = PtA8S1CanonicalMeshKeyEqual(
        present.meshKey, binding.meshKey);
    out.meshHash = present.meshHash == binding.meshHash;
    return out;
}

struct PtA8S1Freshness
{
    bool epochEqual = false;
    bool sequenceFresh = false;

    bool Fresh() const
    {
        return epochEqual && sequenceFresh;
    }
};

inline PtA8S1Freshness PtA8S1CompareFreshness(
    const PtA8S1PresentView& present,
    const PtA8S1BindingView& binding)
{
    PtA8S1Freshness out;
    out.epochEqual =
        present.worldGeneration == binding.worldGeneration &&
        present.publicationGeneration == binding.publicationGeneration;
    if (out.epochEqual)
    {
        out.sequenceFresh =
            binding.lastEventSequence >= present.lastUpsertSequence;
    }
    return out;
}

class PtA8S1AliasSidecar
{
public:
    struct Update
    {
        bool available = false;
        bool inserted = false;
        bool hit = false;
        bool refreshed = false;
        bool collision = false;
    };

    void BeginFrame(std::uint64_t worldGeneration, std::uint64_t frameIndex)
    {
        m_frameIndex = frameIndex;
        if (m_worldGeneration != worldGeneration)
        {
            m_entries.clear();
            m_worldGeneration = worldGeneration;
            m_available = true;
        }
    }

    Update Observe(
        std::uint64_t legacyMeshHash,
        const PtA8S1NormalizedRouteKey& route,
        std::uint64_t canonicalMeshHash)
    {
        Update out;
        out.available = m_available;
        if (!m_available || legacyMeshHash == 0 || canonicalMeshHash == 0)
        {
            return out;
        }
        for (Entry& entry : m_entries)
        {
            if (entry.legacyMeshHash != legacyMeshHash ||
                !PtA8S1NormalizedRouteKeyEqual(entry.route, route))
            {
                continue;
            }
            entry.lastSeenFrame = m_frameIndex;
            if (entry.canonicalMeshHash == canonicalMeshHash)
            {
                out.hit = true;
            }
            else
            {
                entry.canonicalMeshHash = canonicalMeshHash;
                out.refreshed = true;
            }
            return out;
        }

        for (const Entry& entry : m_entries)
        {
            if (entry.legacyMeshHash == legacyMeshHash &&
                !PtA8S1NormalizedRouteKeyEqual(entry.route, route))
            {
                out.collision = true;
                break;
            }
        }

        try
        {
            Entry added;
            added.legacyMeshHash = legacyMeshHash;
            added.route = route;
            added.canonicalMeshHash = canonicalMeshHash;
            added.lastSeenFrame = m_frameIndex;
            m_entries.push_back(added);
            out.inserted = true;
        }
        catch (...)
        {
            m_available = false;
            out = Update();
        }
        return out;
    }

    bool Contains(
        std::uint64_t legacyMeshHash,
        const PtA8S1NormalizedRouteKey& route,
        std::uint64_t canonicalMeshHash) const
    {
        if (!m_available)
        {
            return false;
        }
        for (const Entry& entry : m_entries)
        {
            if (entry.legacyMeshHash == legacyMeshHash &&
                entry.canonicalMeshHash == canonicalMeshHash &&
                PtA8S1NormalizedRouteKeyEqual(entry.route, route))
            {
                return true;
            }
        }
        return false;
    }

    std::uint32_t CollectLegacyKeysForRoute(
        const PtA8S1NormalizedRouteKey& route,
        std::array<std::uint64_t, 8>& keys) const
    {
        std::uint32_t count = 0;
        if (!m_available)
        {
            return 0;
        }
        for (const Entry& entry : m_entries)
        {
            if (PtA8S1NormalizedRouteKeyEqual(entry.route, route) &&
                count < keys.size())
            {
                keys[count++] = entry.legacyMeshHash;
            }
        }
        return count;
    }

    std::uint64_t Retire(const PtCanonicalInstanceKey& instanceKey)
    {
        std::uint64_t retired = 0;
        for (size_t index = 0; index < m_entries.size();)
        {
            const PtA8S1NormalizedRouteKey& route = m_entries[index].route;
            if (route.worldGeneration == instanceKey.worldGeneration &&
                route.renderDefIndex == instanceKey.renderDefIndex &&
                route.renderDefGeneration == instanceKey.renderDefGeneration &&
                route.modelSurfaceIndex >= 0 &&
                static_cast<std::uint32_t>(route.modelSurfaceIndex) ==
                    instanceKey.modelSurfaceIndex)
            {
                m_entries.erase(m_entries.begin() + index);
                ++retired;
                continue;
            }
            ++index;
        }
        return retired;
    }

    bool Available() const { return m_available; }
    std::uint64_t EntryCount() const { return m_entries.size(); }
    std::uint64_t ApproxBytes() const
    {
        std::uint64_t bytes =
            static_cast<std::uint64_t>(m_entries.capacity()) * sizeof(Entry);
        for (const Entry& entry : m_entries)
        {
            bytes += entry.route.modelName.capacity();
        }
        return bytes;
    }

private:
    struct Entry
    {
        std::uint64_t legacyMeshHash = 0;
        PtA8S1NormalizedRouteKey route;
        std::uint64_t canonicalMeshHash = 0;
        std::uint64_t lastSeenFrame = 0;
    };

    bool m_available = true;
    std::uint64_t m_worldGeneration = 0;
    std::uint64_t m_frameIndex = 0;
    std::vector<Entry> m_entries;
};

enum class PtA8S1PresentBridgeOutcome : std::uint32_t
{
    NeverRouted = 0,
    RouteOpportunitySuppressed,
    InvalidOrRejected,
    RoutedNoLegacyAssociation,
    AssociatedUnderThirdKey,
    AssociatedExact,
    Count
};

struct PtA8S1LegacyCandidateProbe
{
    bool available = false;
    std::array<std::uint64_t, 8> legacyKeys = {};
    std::uint32_t legacyKeyCount = 0;
};

struct PtA8S1LegacyCandidateProductRecord
{
    std::uint64_t legacyMeshHash = 0;
    std::uint64_t multiplicity = 0;
    std::uint64_t blasToken = 0;
    bool lookupMember = false;
};

struct PtA8S1LegacySubmittedProductRecord
{
    std::uint64_t legacyMeshHash = 0;
    std::uint32_t instanceMask = 0;
    std::array<std::uint32_t, 12> transformBits = {};
    std::uint64_t submittedBlasToken = 0;
    std::uint64_t selectedBlasToken = 0;
};

struct PtA8S1LegacyProductSnapshot
{
    bool available = false;
    std::vector<PtA8S1LegacyCandidateProductRecord> candidates;
    std::vector<PtA8S1LegacySubmittedProductRecord> submitted;
};

inline void PtA8S1NormalizeLegacyProductSnapshot(
    PtA8S1LegacyProductSnapshot& snapshot)
{
    std::sort(snapshot.candidates.begin(), snapshot.candidates.end(),
        [](const PtA8S1LegacyCandidateProductRecord& lhs,
            const PtA8S1LegacyCandidateProductRecord& rhs) {
            if (lhs.legacyMeshHash != rhs.legacyMeshHash)
            {
                return lhs.legacyMeshHash < rhs.legacyMeshHash;
            }
            if (lhs.multiplicity != rhs.multiplicity)
            {
                return lhs.multiplicity < rhs.multiplicity;
            }
            if (lhs.lookupMember != rhs.lookupMember)
            {
                return lhs.lookupMember < rhs.lookupMember;
            }
            return lhs.blasToken < rhs.blasToken;
        });
    std::sort(snapshot.submitted.begin(), snapshot.submitted.end(),
        [](const PtA8S1LegacySubmittedProductRecord& lhs,
            const PtA8S1LegacySubmittedProductRecord& rhs) {
            if (lhs.legacyMeshHash != rhs.legacyMeshHash)
            {
                return lhs.legacyMeshHash < rhs.legacyMeshHash;
            }
            if (lhs.instanceMask != rhs.instanceMask)
            {
                return lhs.instanceMask < rhs.instanceMask;
            }
            if (lhs.transformBits != rhs.transformBits)
            {
                return lhs.transformBits < rhs.transformBits;
            }
            if (lhs.submittedBlasToken != rhs.submittedBlasToken)
            {
                return lhs.submittedBlasToken < rhs.submittedBlasToken;
            }
            return lhs.selectedBlasToken < rhs.selectedBlasToken;
        });
}

inline std::uint64_t PtA8S1LegacyProductDifferenceCount(
    PtA8S1LegacyProductSnapshot lhs,
    PtA8S1LegacyProductSnapshot rhs)
{
    PtA8S1NormalizeLegacyProductSnapshot(lhs);
    PtA8S1NormalizeLegacyProductSnapshot(rhs);
    std::uint64_t differences = lhs.available == rhs.available ? 0u : 1u;
    const std::size_t candidateCount =
        std::max(lhs.candidates.size(), rhs.candidates.size());
    for (std::size_t index = 0; index < candidateCount; ++index)
    {
        if (index >= lhs.candidates.size() || index >= rhs.candidates.size())
        {
            ++differences;
            continue;
        }
        const auto& a = lhs.candidates[index];
        const auto& b = rhs.candidates[index];
        differences += a.legacyMeshHash != b.legacyMeshHash ||
            a.multiplicity != b.multiplicity ||
            a.lookupMember != b.lookupMember ||
            a.blasToken != b.blasToken;
    }
    const std::size_t submittedCount =
        std::max(lhs.submitted.size(), rhs.submitted.size());
    for (std::size_t index = 0; index < submittedCount; ++index)
    {
        if (index >= lhs.submitted.size() || index >= rhs.submitted.size())
        {
            ++differences;
            continue;
        }
        const auto& a = lhs.submitted[index];
        const auto& b = rhs.submitted[index];
        differences += a.legacyMeshHash != b.legacyMeshHash ||
            a.instanceMask != b.instanceMask ||
            a.transformBits != b.transformBits ||
            a.submittedBlasToken != b.submittedBlasToken ||
            a.selectedBlasToken != b.selectedBlasToken;
    }
    return differences;
}

struct PtA8S1PresentBridgeSample
{
    PtA8S1NormalizedRouteKey presentTuple;
    bool presentDto = false;
    bool binding = false;
    bool currentRoute = false;
    bool everRoute = false;
    bool legacyCandidateProbeAvailable = false;
    std::array<std::uint64_t, 8> routeLegacyKeys = {};
    std::uint32_t routeLegacyKeyCount = 0;
    std::array<std::uint64_t, 8> candidateLegacyKeys = {};
    std::uint32_t candidateLegacyKeyCount = 0;
    PtA8S1PresentBridgeOutcome outcome =
        PtA8S1PresentBridgeOutcome::NeverRouted;
};

struct PtA8S1DiagnosticResult
{
    bool enabled = false;
    bool available = false;
    bool reconciled = false;
    bool observedRouteProofReady = false;
    bool coverageComplete = false;
    std::uint64_t normalizedRouteRequests = 0;
    std::uint64_t buckets[9] = {};
    std::uint64_t routeWorldMismatch = 0;
    std::uint64_t routeRenderDefIndexMismatch = 0;
    std::uint64_t routeRenderDefGenerationMismatch = 0;
    std::uint64_t routeModelNameMismatch = 0;
    std::uint64_t routeSurfaceIndexMismatch = 0;
    std::uint64_t transportInstanceKeyMismatch = 0;
    std::uint64_t transportInstanceHashMismatch = 0;
    std::uint64_t transportMeshKeyMismatch = 0;
    std::uint64_t transportMeshHashMismatch = 0;
    std::uint64_t aliasPersisted = 0;
    std::uint64_t aliasMissing = 0;
    std::uint64_t aliasInserted = 0;
    std::uint64_t aliasHits = 0;
    std::uint64_t aliasRefreshed = 0;
    std::uint64_t aliasCollisions = 0;
    std::uint64_t aliasRetired = 0;
    std::uint64_t aliasEntries = 0;
    std::uint64_t aliasBytes = 0;
    std::uint64_t dtoInstances = 0;
    std::uint64_t dtoBytes = 0;
    std::uint64_t dtoCaptureMicroseconds = 0;
    std::uint64_t routeTopologyPasses = 0;
    std::uint64_t routeTopologyMicroseconds = 0;
    std::uint64_t associationFallbackScans = 0;
    bool presentBridgeAvailable = false;
    std::uint32_t presentBridgeSampleCount = 0;
    std::array<PtA8S1PresentBridgeSample, 8> presentBridgeSamples = {};
    std::uint64_t presentBridgeOutcomes[
        static_cast<std::uint32_t>(PtA8S1PresentBridgeOutcome::Count)] = {};
    bool legacyProductParityAvailable = false;
    bool legacyProductParity = false;
    std::uint64_t legacyProductDifferenceCount = 0;
    std::uint64_t producerCounts[
        static_cast<std::uint32_t>(PtA8S1RouteProducer::Count)] = {};
    std::uint64_t producerOpportunities[
        static_cast<std::uint32_t>(PtA8S1RouteProducer::Count)] = {};
    std::uint64_t producerObserved[
        static_cast<std::uint32_t>(PtA8S1RouteProducer::Count)] = {};
    std::uint64_t producerSuppressedDiagnosticUnavailable[
        static_cast<std::uint32_t>(PtA8S1RouteProducer::Count)] = {};
    std::uint64_t producerNoOpportunityThisFrame[
        static_cast<std::uint32_t>(PtA8S1RouteProducer::Count)] = {};
};
