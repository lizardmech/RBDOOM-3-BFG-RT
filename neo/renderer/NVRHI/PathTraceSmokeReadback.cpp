#include "precompiled.h"
#pragma hdrstop

// Optional center-pixel/full-frame debug readback for the smoke output texture.
//
// Readback is deliberately delayed and throttled so diagnostic logging can sample
// GPU output without becoming part of the normal frame path.

#include "PathTraceCVars.h"
#include "PathTraceCleanRtxdiDiGui.h"
#include "PathTracePrimaryPass.h"
#include "PathTraceDebugDumps.h"
#include "../Image.h"
#include "../../sys/DeviceManager.h"

#include <cmath>
#include <unordered_set>

extern DeviceManager* deviceManager;

namespace {

const int RT_SMOKE_READBACK_INTERVAL_FRAMES = 120;
const int RT_SMOKE_EMISSIVE_AUDIT_MAX_RECORDS = 65536;
const int CLEAN_TEMPORAL_AUDIT_FLAG_SCALE = 262143;
const uint32_t CLEAN_TEMPORAL_DIAG_CURRENT_VALID = 1u << 0u;
const uint32_t CLEAN_TEMPORAL_DIAG_CURRENT_SURFACE_VALID = 1u << 3u;
const uint32_t CLEAN_TEMPORAL_DIAG_MOTION_VALID = 1u << 4u;
const uint32_t CLEAN_TEMPORAL_DIAG_PREVIOUS_SURFACE_VALID = 1u << 5u;
const uint32_t CLEAN_TEMPORAL_DIAG_PREVIOUS_RESERVOIR_VALID = 1u << 6u;
const uint32_t CLEAN_TEMPORAL_DIAG_PREVIOUS_LIGHT_MAPPED = 1u << 7u;
const uint32_t CLEAN_TEMPORAL_DIAG_TEMPORAL_RESERVOIR_VALID = 1u << 8u;
const uint32_t CLEAN_TEMPORAL_DIAG_SDK_REUSED_PREVIOUS = 1u << 9u;
const uint32_t CLEAN_TEMPORAL_DIAG_CURRENT_CANDIDATE = 1u << 10u;
const uint32_t CLEAN_TEMPORAL_DIAG_CAMERA_REPROJECTED = 1u << 11u;
const uint32_t CLEAN_TEMPORAL_DIAG_SDK_CALLED = 1u << 12u;
const uint32_t CLEAN_TEMPORAL_DIAG_PREVIOUS_TARGET_AT_CURRENT = 1u << 13u;
const uint32_t CLEAN_TEMPORAL_DIAG_SDK_SELECTED_PREVIOUS_SAMPLE = 1u << 14u;
const uint32_t CLEAN_TEMPORAL_DIAG_TEMPORAL_OUTPUT_CHANGED = 1u << 15u;
const uint32_t CLEAN_TEMPORAL_DIAG_TEMPORAL_SAMPLE_PIXEL_VALID = 1u << 16u;
const uint32_t CLEAN_TEMPORAL_DIAG_PREVIOUS_PIXEL_IN_BOUNDS = 1u << 17u;
int g_smokeLastReadbackTimingLogMs = -1000000;
int g_liquidPoolProbeX = -1;
int g_liquidPoolProbeY = -1;
int g_liquidPoolProbeWidth = 0;
int g_liquidPoolProbeHeight = 0;
uint32_t g_liquidPoolProbeStatus = 0u;

const uint32_t LIQUID_POOL_STATUS_RECEIVER_VALID = 1u << 1u;
const uint32_t LIQUID_POOL_STATUS_APPLIED = 1u << 2u;

enum class RigidRouteOverlapBucket
{
    Match,
    MaterialMismatch,
    ClassMismatch,
    RigidOnly,
    RigidInFront,
    FallbackInFront,
    FallbackOnly,
    Neither,
    Unknown
};

struct RigidRouteOverlapCounts
{
    int match = 0;
    int materialMismatch = 0;
    int classMismatch = 0;
    int rigidOnly = 0;
    int rigidInFront = 0;
    int fallbackInFront = 0;
    int fallbackOnly = 0;
    int neither = 0;
    int unknown = 0;
};

static uint32_t DecodeCleanTemporalAuditFlags(const float* rgba)
{
    const int decoded = static_cast<int>(rgba[0] * static_cast<float>(CLEAN_TEMPORAL_AUDIT_FLAG_SCALE) + 0.5f);
    return static_cast<uint32_t>(idMath::ClampInt(0, CLEAN_TEMPORAL_AUDIT_FLAG_SCALE, decoded));
}

static void AccumulateCleanTemporalAuditFlag(unsigned int& count, uint32_t flags, uint32_t flag)
{
    if ((flags & flag) != 0u)
    {
        ++count;
    }
}

RigidRouteOverlapBucket ClassifyRigidRouteOverlapColor(const float* rgba)
{
    const bool rHigh = rgba[0] > 0.75f;
    const bool gHigh = rgba[1] > 0.75f;
    const bool bHigh = rgba[2] > 0.75f;
    const bool gMid = rgba[1] > 0.30f;
    const bool bMid = rgba[2] > 0.35f;
    const bool dimGray = rgba[0] > 0.08f && rgba[0] < 0.32f && rgba[1] > 0.08f && rgba[1] < 0.32f && rgba[2] > 0.08f && rgba[2] < 0.32f;
    const bool black = rgba[0] < 0.04f && rgba[1] < 0.04f && rgba[2] < 0.04f;

    if (!rHigh && gHigh && !bHigh)
    {
        return RigidRouteOverlapBucket::Match;
    }
    if (rHigh && gHigh && !bHigh)
    {
        return RigidRouteOverlapBucket::MaterialMismatch;
    }
    if (rHigh && !gHigh && bHigh)
    {
        return RigidRouteOverlapBucket::ClassMismatch;
    }
    if (!rHigh && gHigh && bHigh)
    {
        return RigidRouteOverlapBucket::RigidOnly;
    }
    if (!rHigh && !gHigh && bHigh)
    {
        return RigidRouteOverlapBucket::RigidInFront;
    }
    if (rHigh && gMid && !bMid)
    {
        return RigidRouteOverlapBucket::FallbackInFront;
    }
    if (dimGray)
    {
        return RigidRouteOverlapBucket::FallbackOnly;
    }
    if (black)
    {
        return RigidRouteOverlapBucket::Neither;
    }
    return RigidRouteOverlapBucket::Unknown;
}

const char* RigidRouteOverlapBucketName(RigidRouteOverlapBucket bucket)
{
    switch (bucket)
    {
        case RigidRouteOverlapBucket::Match: return "match/green";
        case RigidRouteOverlapBucket::MaterialMismatch: return "materialMismatch/yellow";
        case RigidRouteOverlapBucket::ClassMismatch: return "classMismatch/magenta";
        case RigidRouteOverlapBucket::RigidOnly: return "rigidOnly/cyan";
        case RigidRouteOverlapBucket::RigidInFront: return "rigidInFront/blue";
        case RigidRouteOverlapBucket::FallbackInFront: return "fallbackInFront/orange";
        case RigidRouteOverlapBucket::FallbackOnly: return "fallbackOnly/gray";
        case RigidRouteOverlapBucket::Neither: return "neither/black";
        default: return "unknown";
    }
}

void AccumulateRigidRouteOverlapBucket(RigidRouteOverlapCounts& counts, RigidRouteOverlapBucket bucket)
{
    switch (bucket)
    {
        case RigidRouteOverlapBucket::Match: ++counts.match; break;
        case RigidRouteOverlapBucket::MaterialMismatch: ++counts.materialMismatch; break;
        case RigidRouteOverlapBucket::ClassMismatch: ++counts.classMismatch; break;
        case RigidRouteOverlapBucket::RigidOnly: ++counts.rigidOnly; break;
        case RigidRouteOverlapBucket::RigidInFront: ++counts.rigidInFront; break;
        case RigidRouteOverlapBucket::FallbackInFront: ++counts.fallbackInFront; break;
        case RigidRouteOverlapBucket::FallbackOnly: ++counts.fallbackOnly; break;
        case RigidRouteOverlapBucket::Neither: ++counts.neither; break;
        default: ++counts.unknown; break;
    }
}

const char* StaticContractRejectReasonName(uint32_t reason)
{
    switch (reason)
    {
        case RT_STATIC_CONTRACT_REJECT_NONE: return "none";
        case RT_STATIC_CONTRACT_REJECT_GUI_ALPHA: return "gui_alpha";
        case RT_STATIC_CONTRACT_REJECT_PARTICLE_DITHER: return "particle_dither";
        case RT_STATIC_CONTRACT_REJECT_GLASS_FALLBACK: return "glass_fallback";
        case RT_STATIC_CONTRACT_REJECT_ADDITIVE_DECAL: return "additive_decal";
        case RT_STATIC_CONTRACT_REJECT_FILTER_DECAL: return "filter_decal";
        case RT_STATIC_CONTRACT_REJECT_ALPHA_TEST: return "alpha_test";
        case RT_STATIC_CONTRACT_REJECT_RIGID_INSTANCE_RANGE: return "rigid_instance_range";
        case RT_STATIC_CONTRACT_REJECT_RIGID_PRIMITIVE_RANGE: return "rigid_primitive_range";
        case RT_STATIC_CONTRACT_REJECT_RIGID_INDEX_RANGE: return "rigid_index_range";
        case RT_STATIC_CONTRACT_REJECT_RIGID_VERTEX_RANGE: return "rigid_vertex_range";
        case RT_STATIC_CONTRACT_REJECT_TRIANGLE_RANGE: return "triangle_range";
        case RT_STATIC_CONTRACT_REJECT_VERTEX_RANGE: return "vertex_range";
        case RT_STATIC_CONTRACT_REJECT_MISS: return "miss";
        case RT_STATIC_CONTRACT_REJECT_GUI_PRIMARY: return "gui_primary";
        default: return "unknown";
    }
}

bool GpuSkinningPositionFinite(const float position[4])
{
    return std::isfinite(position[0]) &&
        std::isfinite(position[1]) &&
        std::isfinite(position[2]) &&
        std::isfinite(position[3]);
}

float GpuSkinningPositionMaxError(const float expected[4], const float actual[4])
{
    float error = 0.0f;
    for (int component = 0; component < 3; ++component)
    {
        error = Max(error, idMath::Fabs(expected[component] - actual[component]));
    }
    return error;
}

bool GpuSkinningPositionWithinTolerance(
    const float expected[4],
    const float actual[4],
    float absoluteTolerance,
    float relativeTolerance)
{
    if (!GpuSkinningPositionFinite(expected) || !GpuSkinningPositionFinite(actual))
    {
        return false;
    }
    for (int component = 0; component < 3; ++component)
    {
        const float scale = Max(1.0f, Max(idMath::Fabs(expected[component]), idMath::Fabs(actual[component])));
        if (idMath::Fabs(expected[component] - actual[component]) >
            absoluteTolerance + relativeTolerance * scale)
        {
            return false;
        }
    }
    return true;
}

}

void PathTracePrimaryPass::ReadBackSkyCubeProbe()
{
    if (!m_smokeSkyCubeProbeReadbackQueued || !m_smokeSkyCubeProbeReadbackTexture)
    {
        return;
    }
    if (m_smokeSkyCubeProbeReadbackDelayFrames > 0)
    {
        --m_smokeSkyCubeProbeReadbackDelayFrames;
        return;
    }

    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!device)
    {
        return;
    }

    device->waitForIdle();
    size_t rowPitch = 0;
    void* readbackData = device->mapStagingTexture(
        m_smokeSkyCubeProbeReadbackTexture,
        nvrhi::TextureSlice(),
        nvrhi::CpuAccessMode::Read,
        &rowPitch);
    if (!readbackData)
    {
        common->Printf("PathTracePrimaryPass: isolated sky-cube compute probe readback map failed\n");
        m_smokeSkyCubeProbeReadbackQueued = false;
        return;
    }

    static const char* const faceLabels[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
    const float* samples = reinterpret_cast<const float*>(readbackData);
    common->Printf(
        "PathTracePrimaryPass: isolated sky-cube compute probe result source='%s' rowPitch=%llu\n",
        m_smokeSkyEnvironmentSourceName.c_str(),
        static_cast<unsigned long long>(rowPitch));
    for (int faceIndex = 0; faceIndex < 6; ++faceIndex)
    {
        const float* rgba = samples + faceIndex * 4;
        common->Printf(
            "PathTracePrimaryPass: isolated sky-cube face %s rgba=(%.6f %.6f %.6f %.6f)\n",
            faceLabels[faceIndex],
            rgba[0], rgba[1], rgba[2], rgba[3]);
    }

    device->unmapStagingTexture(m_smokeSkyCubeProbeReadbackTexture);
    m_smokeSkyCubeProbeReadbackQueued = false;
}

void PathTracePrimaryPass::ReadBackLiquidPoolStatus()
{
    if (!m_liquidPoolStatusReadbackQueued || !m_liquidPoolStatusReadbackBuffer)
    {
        return;
    }
    if (m_liquidPoolStatusReadbackDelayFrames > 0)
    {
        --m_liquidPoolStatusReadbackDelayFrames;
        return;
    }

    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!device)
    {
        return;
    }
    const uint32_t* counters = static_cast<const uint32_t*>(
        device->mapBuffer(m_liquidPoolStatusReadbackBuffer, nvrhi::CpuAccessMode::Read));
    if (!counters)
    {
        common->Printf("PathTracePrimaryPass: liquid-pool status readback map failed\n");
        m_liquidPoolStatusReadbackQueued = false;
        return;
    }

    for (uint32_t source = 0u; source < 8u; ++source)
    {
        const uint32_t exceptionalMask = counters[source];
        const uint32_t overflowCount = counters[8u + source];
        if ((exceptionalMask != 0u || overflowCount != 0u) &&
            (exceptionalMask != m_liquidPoolLastExceptionalMask[source] ||
                overflowCount != m_liquidPoolLastOverflowCount[source]))
        {
            common->Printf(
                "PathTracePrimaryPass: liquid-pool status source=%u exceptionalMask=0x%08x overflowRays=%u\n",
                source,
                exceptionalMask,
                overflowCount);
        }
        m_liquidPoolLastExceptionalMask[source] = exceptionalMask;
        m_liquidPoolLastOverflowCount[source] = overflowCount;
    }
    device->unmapBuffer(m_liquidPoolStatusReadbackBuffer);
    m_liquidPoolStatusReadbackQueued = false;
}

void PathTracePrimaryPass::QueueStaticContractShaderSample(nvrhi::ICommandList* commandList)
{
    if (!m_staticContractShaderReadbackRequested || m_staticContractShaderReadbackQueued)
    {
        return;
    }

    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    const RtRestirPTPrimarySurfaceHistoryBufferHandles& history = m_frameResources.primarySurfaceHistoryBuffers;
    if (!commandList || !device || !history.current ||
        m_staticContractShaderSampleWidth <= 0 || m_staticContractShaderSampleHeight <= 0 ||
        m_staticContractShaderSampleX < 0 || m_staticContractShaderSampleY < 0 ||
        m_staticContractShaderSampleX >= m_staticContractShaderSampleWidth ||
        m_staticContractShaderSampleY >= m_staticContractShaderSampleHeight)
    {
        common->Printf("PathTracePrimaryPass: PT static contract shader sample unavailable before copy\n");
        m_staticContractShaderReadbackRequested = false;
        m_canonicalRigidHitSample = false;
        m_canonicalRigidHitRouteContexts.clear();
        m_canonicalRigidHitEmissiveContexts.clear();
        return;
    }

    if (!m_staticContractShaderReadbackBuffer)
    {
        nvrhi::BufferDesc desc;
        desc.byteSize = sizeof(RtPathTracePrimarySurfaceRecord);
        desc.structStride = sizeof(uint32_t);
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
        desc.debugName = "PathTraceStaticContractShaderReadback";
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        m_staticContractShaderReadbackBuffer = device->createBuffer(desc);
    }
    if (!m_staticContractShaderReadbackBuffer)
    {
        common->Printf("PathTracePrimaryPass: PT static contract shader readback buffer creation failed\n");
        m_staticContractShaderReadbackRequested = false;
        m_canonicalRigidHitSample = false;
        m_canonicalRigidHitRouteContexts.clear();
        m_canonicalRigidHitEmissiveContexts.clear();
        return;
    }

    const uint64_t pixelIndex =
        static_cast<uint64_t>(m_staticContractShaderSampleY) * static_cast<uint64_t>(m_staticContractShaderSampleWidth) +
        static_cast<uint64_t>(m_staticContractShaderSampleX);
    const uint64_t sourceOffset = pixelIndex * sizeof(RtPathTracePrimarySurfaceRecord);
    if (sourceOffset + sizeof(RtPathTracePrimarySurfaceRecord) > history.current->getDesc().byteSize)
    {
        common->Printf(
            "PathTracePrimaryPass: PT static contract shader sample offset out of range pixel=%llu offset=%llu bytes=%llu\n",
            static_cast<unsigned long long>(pixelIndex),
            static_cast<unsigned long long>(sourceOffset),
            static_cast<unsigned long long>(history.current->getDesc().byteSize));
        m_staticContractShaderReadbackRequested = false;
        m_canonicalRigidHitSample = false;
        m_canonicalRigidHitRouteContexts.clear();
        m_canonicalRigidHitEmissiveContexts.clear();
        return;
    }

    commandList->setBufferState(history.current, nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(m_staticContractShaderReadbackBuffer, nvrhi::ResourceStates::CopyDest);
    commandList->commitBarriers();
    commandList->copyBuffer(
        m_staticContractShaderReadbackBuffer,
        0,
        history.current,
        sourceOffset,
        sizeof(RtPathTracePrimarySurfaceRecord));
    commandList->setBufferState(history.current, nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    m_staticContractShaderReadbackRequested = false;
    m_staticContractShaderReadbackQueued = true;
    m_staticContractShaderReadbackDelayFrames = 3;
    common->Printf(
        "PathTracePrimaryPass: PT static contract shader sample queued frame=%llu pixel=%d/%d dimensions=%d/%d\n",
        static_cast<unsigned long long>(m_staticContractShaderSampleFrame),
        m_staticContractShaderSampleX,
        m_staticContractShaderSampleY,
        m_staticContractShaderSampleWidth,
        m_staticContractShaderSampleHeight);
}

void PathTracePrimaryPass::ReadBackStaticContractShaderSample()
{
    if (!m_staticContractShaderReadbackQueued || !m_staticContractShaderReadbackBuffer)
    {
        return;
    }
    if (m_staticContractShaderReadbackDelayFrames > 0)
    {
        --m_staticContractShaderReadbackDelayFrames;
        return;
    }

    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!device)
    {
        return;
    }
    const RtPathTracePrimarySurfaceRecord* record = static_cast<const RtPathTracePrimarySurfaceRecord*>(
        device->mapBuffer(m_staticContractShaderReadbackBuffer, nvrhi::CpuAccessMode::Read));
    if (!record)
    {
        common->Printf("PathTracePrimaryPass: PT static contract shader sample readback map failed\n");
        m_staticContractShaderReadbackQueued = false;
        m_canonicalRigidHitSample = false;
        m_canonicalRigidHitRouteContexts.clear();
        m_canonicalRigidHitEmissiveContexts.clear();
        return;
    }

    const uint32_t validFlags = record->header[1];
    const uint32_t instanceId = record->instancePrimitiveObject[0];
    const uint32_t primitiveIndex = record->instancePrimitiveObject[1];
    const uint32_t rejectReason = record->instancePrimitiveObject[2];
    const bool valid = (validFlags & 1u) != 0u;
    const bool instanceMatches = valid && instanceId == m_staticContractExpectedInstance;
    const bool primitiveMatches =
        valid &&
        m_staticContractExpectedPrimitiveFirst != UINT32_MAX &&
        primitiveIndex >= m_staticContractExpectedPrimitiveFirst &&
        primitiveIndex - m_staticContractExpectedPrimitiveFirst < m_staticContractExpectedPrimitiveCount;
    const bool materialIdMatches = valid && record->materialAndSurface[0] == m_staticContractExpectedMaterialId;
    const bool materialIndexMatches = valid && record->materialAndSurface[1] == m_staticContractExpectedMaterialIndex;

    if (m_canonicalRigidHitSample)
    {
        const bool rigidInstance =
            valid &&
            instanceId >= m_canonicalRigidHitFirstInstance &&
            instanceId - m_canonicalRigidHitFirstInstance <
                m_canonicalRigidHitInstanceCount;
        common->Printf(
            "PathTracePrimaryPass: GEO06 canonical rigid hit frame=%llu traversal=%s pixel=%d/%d dimensions=%d/%d version=%u validFlags=0x%08x status=%u triangleFlags=0x%08x rigid=%d rigidRange(first/count)=%u/%u instance=%u primitive=%u material(id/index/flags/class)=%u/%u/0x%08x/%u emissive(texture/rgb)=0x%08x/%.9g:%.9g:%.9g currentXYZ=%.9g:%.9g:%.9g previousXYZW=%.9g:%.9g:%.9g:%.9g reject=%u(%s)\n",
            static_cast<unsigned long long>(
                m_staticContractShaderSampleFrame),
            m_canonicalRigidHitTraversalSelected
                ? "canonical"
                : "legacy",
            m_staticContractShaderSampleX,
            m_staticContractShaderSampleY,
            m_staticContractShaderSampleWidth,
            m_staticContractShaderSampleHeight,
            record->header[0],
            validFlags,
            record->header[2],
            record->header[3],
            rigidInstance ? 1 : 0,
            m_canonicalRigidHitFirstInstance,
            m_canonicalRigidHitInstanceCount,
            instanceId,
            primitiveIndex,
            record->materialAndSurface[0],
            record->materialAndSurface[1],
            record->materialAndSurface[2],
            record->materialAndSurface[3],
            record->instancePrimitiveObject[3],
            record->emissiveAndHeight[0],
            record->emissiveAndHeight[1],
            record->emissiveAndHeight[2],
            record->worldPositionAndViewDepth[0],
            record->worldPositionAndViewDepth[1],
            record->worldPositionAndViewDepth[2],
            record->previousPositionOrMotion[0],
            record->previousPositionOrMotion[1],
            record->previousPositionOrMotion[2],
            record->previousPositionOrMotion[3],
            rejectReason,
            StaticContractRejectReasonName(rejectReason));
        const CanonicalRigidHitRouteContext* routeContext = nullptr;
        for (const CanonicalRigidHitRouteContext& candidate :
            m_canonicalRigidHitRouteContexts)
        {
            if (candidate.instanceId == instanceId)
            {
                routeContext = &candidate;
                break;
            }
        }
        const CanonicalRigidHitEmissiveContext* emissiveContext =
            nullptr;
        for (const CanonicalRigidHitEmissiveContext& candidate :
            m_canonicalRigidHitEmissiveContexts)
        {
            if (candidate.instanceId == instanceId &&
                candidate.primitiveIndex == primitiveIndex)
            {
                emissiveContext = &candidate;
                break;
            }
        }
        common->Printf(
            "PathTracePrimaryPass: GEO06 canonical rigid hit context traversal=%s route(found/sourceInstance/legacyMesh/canonicalMesh/record/canonicalBlas/transform/previous/flags)= %d/%llu/%llu/%llu/%u/%u/%llu/%llu/0x%08x emissive(found/identity/material/index/texture)=%d/%llu/%u/%u/0x%08x\n",
            m_canonicalRigidHitTraversalSelected
                ? "canonical"
                : "legacy",
            routeContext != nullptr ? 1 : 0,
            static_cast<unsigned long long>(
                routeContext != nullptr
                    ? routeContext->sourceInstanceId
                    : 0),
            static_cast<unsigned long long>(
                routeContext != nullptr
                    ? routeContext->legacyMeshHash
                    : 0),
            static_cast<unsigned long long>(
                routeContext != nullptr
                    ? routeContext->canonicalMeshHash
                    : 0),
            routeContext != nullptr
                ? routeContext->routeRecordIndex
                : UINT32_MAX,
            routeContext != nullptr
                ? routeContext->canonicalBlasRecordIndex
                : UINT32_MAX,
            static_cast<unsigned long long>(
                routeContext != nullptr
                    ? routeContext->currentTransformHash
                    : 0),
            static_cast<unsigned long long>(
                routeContext != nullptr
                    ? routeContext->previousTransformHash
                    : 0),
            routeContext != nullptr ? routeContext->flags : 0u,
            emissiveContext != nullptr ? 1 : 0,
            static_cast<unsigned long long>(
                emissiveContext != nullptr
                    ? emissiveContext->identity
                    : 0),
            emissiveContext != nullptr
                ? emissiveContext->materialId
                : 0u,
            emissiveContext != nullptr
                ? emissiveContext->materialIndex
                : UINT32_MAX,
            emissiveContext != nullptr
                ? emissiveContext->emissiveTextureIndex
                : UINT32_MAX);
    }
    else
    {
        common->Printf(
            "PathTracePrimaryPass: PT static contract shaderSample frame=%llu pixel=%d/%d dimensions=%d/%d version=%u validFlags=0x%08x status=%u instance=%u primitive=%u material(id/index)=%u/%u surfaceClass=%u reject=%u(%s) expected(instance/primitiveFirst/count/materialId/materialIndex)=%u/%u/%u/%u/%u match(valid/instance/primitive/materialId/materialIndex)=%d/%d/%d/%d/%d\n",
            static_cast<unsigned long long>(
                m_staticContractShaderSampleFrame),
            m_staticContractShaderSampleX,
            m_staticContractShaderSampleY,
            m_staticContractShaderSampleWidth,
            m_staticContractShaderSampleHeight,
            record->header[0],
            validFlags,
            record->header[2],
            instanceId,
            primitiveIndex,
            record->materialAndSurface[0],
            record->materialAndSurface[1],
            record->materialAndSurface[3],
            rejectReason,
            StaticContractRejectReasonName(rejectReason),
            m_staticContractExpectedInstance,
            m_staticContractExpectedPrimitiveFirst,
            m_staticContractExpectedPrimitiveCount,
            m_staticContractExpectedMaterialId,
            m_staticContractExpectedMaterialIndex,
            valid ? 1 : 0,
            instanceMatches ? 1 : 0,
            primitiveMatches ? 1 : 0,
            materialIdMatches ? 1 : 0,
            materialIndexMatches ? 1 : 0);
    }

    device->unmapBuffer(m_staticContractShaderReadbackBuffer);
    m_staticContractShaderReadbackQueued = false;
    m_canonicalRigidHitSample = false;
    m_canonicalRigidHitRouteContexts.clear();
    m_canonicalRigidHitEmissiveContexts.clear();
}

void PathTracePrimaryPass::QueueStaticContractGeometrySample(
    nvrhi::ICommandList* commandList,
    nvrhi::IBuffer* staticVertexBuffer,
    nvrhi::IBuffer* staticIndexBuffer,
    const PathTraceSmokeVertex* staticVertices,
    int vertexOffset,
    int vertexCount,
    const uint32_t* staticIndexes,
    int indexOffset,
    int indexCount,
    uint64 frameIndex)
{
    if (m_staticContractGeometryReadbackQueued)
    {
        return;
    }

    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!commandList || !device || !staticVertexBuffer || !staticIndexBuffer ||
        !staticVertices || !staticIndexes ||
        vertexOffset < 0 || vertexCount <= 0 ||
        indexOffset < 0 || indexCount <= 0)
    {
        common->Printf("PathTracePrimaryPass: PT static contract GPU geometry sample unavailable before copy\n");
        return;
    }

    const uint64_t sourceVertexOffset =
        static_cast<uint64_t>(vertexOffset) * sizeof(PathTraceSmokeVertex);
    const uint64_t vertexBytes =
        static_cast<uint64_t>(vertexCount) * sizeof(PathTraceSmokeVertex);
    const uint64_t sourceIndexOffset =
        static_cast<uint64_t>(indexOffset) * sizeof(uint32_t);
    const uint64_t indexBytes =
        static_cast<uint64_t>(indexCount) * sizeof(uint32_t);
    const uint64_t totalBytes = vertexBytes + indexBytes;
    if (sourceVertexOffset + vertexBytes > staticVertexBuffer->getDesc().byteSize ||
        sourceIndexOffset + indexBytes > staticIndexBuffer->getDesc().byteSize)
    {
        common->Printf(
            "PathTracePrimaryPass: PT static contract GPU geometry sample range out of bounds frame=%llu range(v/i)=%d/%d %d/%d sourceBytes(v/i)=%llu/%llu bufferBytes(v/i)=%llu/%llu\n",
            static_cast<unsigned long long>(frameIndex),
            vertexOffset,
            vertexCount,
            indexOffset,
            indexCount,
            static_cast<unsigned long long>(sourceVertexOffset + vertexBytes),
            static_cast<unsigned long long>(sourceIndexOffset + indexBytes),
            static_cast<unsigned long long>(staticVertexBuffer->getDesc().byteSize),
            static_cast<unsigned long long>(staticIndexBuffer->getDesc().byteSize));
        return;
    }

    if (!m_staticContractGeometryReadbackBuffer ||
        m_staticContractGeometryReadbackBuffer->getDesc().byteSize < totalBytes)
    {
        m_staticContractGeometryReadbackBuffer = nullptr;
        nvrhi::BufferDesc desc;
        desc.byteSize = totalBytes;
        desc.structStride = sizeof(uint32_t);
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
        desc.debugName = "PathTraceStaticContractGeometryReadback";
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        m_staticContractGeometryReadbackBuffer = device->createBuffer(desc);
    }
    if (!m_staticContractGeometryReadbackBuffer)
    {
        common->Printf("PathTracePrimaryPass: PT static contract GPU geometry readback buffer creation failed\n");
        return;
    }

    m_staticContractGeometryCpuVertices.assign(
        staticVertices + vertexOffset,
        staticVertices + vertexOffset + vertexCount);
    m_staticContractGeometryCpuIndexes.assign(
        staticIndexes + indexOffset,
        staticIndexes + indexOffset + indexCount);
    m_staticContractGeometrySampleFrame = frameIndex;
    m_staticContractGeometryVertexOffset = vertexOffset;
    m_staticContractGeometryIndexOffset = indexOffset;

    commandList->setBufferState(staticVertexBuffer, nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(staticIndexBuffer, nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(m_staticContractGeometryReadbackBuffer, nvrhi::ResourceStates::CopyDest);
    commandList->commitBarriers();
    commandList->copyBuffer(
        m_staticContractGeometryReadbackBuffer,
        0,
        staticVertexBuffer,
        sourceVertexOffset,
        vertexBytes);
    commandList->copyBuffer(
        m_staticContractGeometryReadbackBuffer,
        vertexBytes,
        staticIndexBuffer,
        sourceIndexOffset,
        indexBytes);
    commandList->setBufferState(staticVertexBuffer, nvrhi::ResourceStates::AccelStructBuildInput);
    commandList->setBufferState(staticIndexBuffer, nvrhi::ResourceStates::AccelStructBuildInput);
    commandList->commitBarriers();

    m_staticContractGeometryReadbackQueued = true;
    m_staticContractGeometryReadbackDelayFrames = 3;
    common->Printf(
        "PathTracePrimaryPass: PT static contract GPU geometry sample queued frame=%llu range(v/i)=%d/%d %d/%d bytes(v/i)=%llu/%llu\n",
        static_cast<unsigned long long>(frameIndex),
        vertexOffset,
        vertexCount,
        indexOffset,
        indexCount,
        static_cast<unsigned long long>(vertexBytes),
        static_cast<unsigned long long>(indexBytes));
}

void PathTracePrimaryPass::ReadBackStaticContractGeometrySample()
{
    if (!m_staticContractGeometryReadbackQueued || !m_staticContractGeometryReadbackBuffer)
    {
        return;
    }
    if (m_staticContractGeometryReadbackDelayFrames > 0)
    {
        --m_staticContractGeometryReadbackDelayFrames;
        return;
    }

    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!device)
    {
        return;
    }
    const uint8_t* readbackBytes = static_cast<const uint8_t*>(
        device->mapBuffer(m_staticContractGeometryReadbackBuffer, nvrhi::CpuAccessMode::Read));
    if (!readbackBytes)
    {
        common->Printf("PathTracePrimaryPass: PT static contract GPU geometry sample readback map failed\n");
        m_staticContractGeometryReadbackQueued = false;
        return;
    }

    const size_t vertexCount = m_staticContractGeometryCpuVertices.size();
    const size_t indexCount = m_staticContractGeometryCpuIndexes.size();
    const size_t vertexBytes = vertexCount * sizeof(PathTraceSmokeVertex);
    const PathTraceSmokeVertex* gpuVertices =
        reinterpret_cast<const PathTraceSmokeVertex*>(readbackBytes);
    const uint32_t* gpuIndexes =
        reinterpret_cast<const uint32_t*>(readbackBytes + vertexBytes);
    int vertexMismatchCount = 0;
    int positionMismatchCount = 0;
    int indexMismatchCount = 0;
    int firstVertexMismatch = -1;
    int firstPositionMismatch = -1;
    int firstIndexMismatch = -1;
    for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
        if (std::memcmp(
                &m_staticContractGeometryCpuVertices[vertexIndex],
                &gpuVertices[vertexIndex],
                sizeof(PathTraceSmokeVertex)) != 0)
        {
            ++vertexMismatchCount;
            if (firstVertexMismatch < 0)
            {
                firstVertexMismatch = static_cast<int>(vertexIndex);
            }
        }
        if (std::memcmp(
                m_staticContractGeometryCpuVertices[vertexIndex].position,
                gpuVertices[vertexIndex].position,
                sizeof(gpuVertices[vertexIndex].position)) != 0)
        {
            ++positionMismatchCount;
            if (firstPositionMismatch < 0)
            {
                firstPositionMismatch = static_cast<int>(vertexIndex);
            }
        }
    }
    for (size_t index = 0; index < indexCount; ++index)
    {
        if (m_staticContractGeometryCpuIndexes[index] != gpuIndexes[index])
        {
            ++indexMismatchCount;
            if (firstIndexMismatch < 0)
            {
                firstIndexMismatch = static_cast<int>(index);
            }
        }
    }

    common->Printf(
        "PathTracePrimaryPass: PT static contract gpuGeometry frame=%llu range(v/i)=%d/%llu %d/%llu match(v/i)=%d/%d mismatches(vertices/positions/indexes)=%d/%d/%d first(v/position/i)=%d/%d/%d\n",
        static_cast<unsigned long long>(m_staticContractGeometrySampleFrame),
        m_staticContractGeometryVertexOffset,
        static_cast<unsigned long long>(vertexCount),
        m_staticContractGeometryIndexOffset,
        static_cast<unsigned long long>(indexCount),
        vertexMismatchCount == 0 ? 1 : 0,
        indexMismatchCount == 0 ? 1 : 0,
        vertexMismatchCount,
        positionMismatchCount,
        indexMismatchCount,
        firstVertexMismatch,
        firstPositionMismatch,
        firstIndexMismatch);

    if (firstPositionMismatch >= 0)
    {
        const PathTraceSmokeVertex& cpu =
            m_staticContractGeometryCpuVertices[static_cast<size_t>(firstPositionMismatch)];
        const PathTraceSmokeVertex& gpu = gpuVertices[static_cast<size_t>(firstPositionMismatch)];
        common->Printf(
            "PathTracePrimaryPass: PT static contract gpuGeometry firstPosition globalVertex=%d cpu=(%.9g %.9g %.9g %.9g) gpu=(%.9g %.9g %.9g %.9g)\n",
            m_staticContractGeometryVertexOffset + firstPositionMismatch,
            cpu.position[0],
            cpu.position[1],
            cpu.position[2],
            cpu.position[3],
            gpu.position[0],
            gpu.position[1],
            gpu.position[2],
            gpu.position[3]);
    }
    if (firstIndexMismatch >= 0)
    {
        common->Printf(
            "PathTracePrimaryPass: PT static contract gpuGeometry firstIndex globalIndex=%d cpu=%u gpu=%u\n",
            m_staticContractGeometryIndexOffset + firstIndexMismatch,
            m_staticContractGeometryCpuIndexes[static_cast<size_t>(firstIndexMismatch)],
            gpuIndexes[static_cast<size_t>(firstIndexMismatch)]);
    }

    device->unmapBuffer(m_staticContractGeometryReadbackBuffer);
    m_staticContractGeometryReadbackQueued = false;
}

void PathTracePrimaryPass::QueueGpuSkinningParitySamples(
    nvrhi::ICommandList* commandList,
    nvrhi::IBuffer* currentOutputBuffer,
    nvrhi::IBuffer* previousPositionBuffer,
    nvrhi::ResourceStates currentRestoreState,
    const std::vector<GpuSkinningParitySample>& samples,
    int mode,
    uint64 frameIndex)
{
    if (m_gpuSkinningParityReadbackQueued)
    {
        common->Printf("PathTracePrimaryPass: PT GPU skinning parity request ignored because a readback is already queued\n");
        return;
    }

    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!commandList || !device || !currentOutputBuffer || !previousPositionBuffer || samples.empty())
    {
        common->Printf("PathTracePrimaryPass: PT GPU skinning parity sample unavailable before copy\n");
        return;
    }

    const uint64 currentBytes =
        static_cast<uint64>(samples.size()) * sizeof(PathTraceSmokeVertex);
    const uint64 previousBytes =
        static_cast<uint64>(samples.size()) * sizeof(PathTraceSkinnedPreviousPosition);
    const uint64 totalBytes = currentBytes + previousBytes;
    if (!m_gpuSkinningParityReadbackBuffer ||
        m_gpuSkinningParityReadbackBuffer->getDesc().byteSize < totalBytes)
    {
        m_gpuSkinningParityReadbackBuffer = nullptr;
        nvrhi::BufferDesc desc;
        desc.byteSize = totalBytes;
        desc.structStride = sizeof(uint32_t);
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
        desc.debugName = "PathTraceGpuSkinningParityReadback";
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        m_gpuSkinningParityReadbackBuffer = device->createBuffer(desc);
    }
    if (!m_gpuSkinningParityReadbackBuffer)
    {
        common->Printf("PathTracePrimaryPass: PT GPU skinning parity readback buffer creation failed\n");
        return;
    }

    for (const GpuSkinningParitySample& sample : samples)
    {
        if (sample.currentByteOffset + sizeof(PathTraceSmokeVertex) >
                currentOutputBuffer->getDesc().byteSize ||
            (sample.hasPrevious &&
                sample.previousByteOffset + sizeof(PathTraceSkinnedPreviousPosition) >
                    previousPositionBuffer->getDesc().byteSize))
        {
            common->Printf(
                "PathTracePrimaryPass: PT GPU skinning parity sample range out of bounds entity/surface/vertex=%d/%d/%d\n",
                sample.entityIndex,
                sample.drawSurfIndex,
                sample.vertexIndex);
            return;
        }
    }

    m_gpuSkinningParitySamples = samples;
    m_gpuSkinningParityMode = mode;
    m_gpuSkinningParityFrame = frameIndex;

    commandList->setBufferState(currentOutputBuffer, nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(previousPositionBuffer, nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(m_gpuSkinningParityReadbackBuffer, nvrhi::ResourceStates::CopyDest);
    commandList->commitBarriers();
    for (size_t sampleIndex = 0; sampleIndex < samples.size(); ++sampleIndex)
    {
        const GpuSkinningParitySample& sample = samples[sampleIndex];
        commandList->copyBuffer(
            m_gpuSkinningParityReadbackBuffer,
            sampleIndex * sizeof(PathTraceSmokeVertex),
            currentOutputBuffer,
            sample.currentByteOffset,
            sizeof(PathTraceSmokeVertex));
        if (sample.hasPrevious)
        {
            commandList->copyBuffer(
                m_gpuSkinningParityReadbackBuffer,
                currentBytes + sampleIndex * sizeof(PathTraceSkinnedPreviousPosition),
                previousPositionBuffer,
                sample.previousByteOffset,
                sizeof(PathTraceSkinnedPreviousPosition));
        }
    }
    commandList->setBufferState(currentOutputBuffer, currentRestoreState);
    commandList->setBufferState(previousPositionBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();

    m_gpuSkinningParityReadbackQueued = true;
    m_gpuSkinningParityReadbackDelayFrames = 3;
    common->Printf(
        "PathTracePrimaryPass: PT GPU skinning parity queued frame=%llu mode=%d samples=%llu tolerance(abs/rel)=0.001/0.00001 finiteRequired=1\n",
        static_cast<unsigned long long>(frameIndex),
        mode,
        static_cast<unsigned long long>(samples.size()));
}

void PathTracePrimaryPass::ReadBackGpuSkinningParitySamples()
{
    if (!m_gpuSkinningParityReadbackQueued || !m_gpuSkinningParityReadbackBuffer)
    {
        return;
    }
    if (m_gpuSkinningParityReadbackDelayFrames > 0)
    {
        --m_gpuSkinningParityReadbackDelayFrames;
        return;
    }

    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!device)
    {
        return;
    }
    const uint8_t* readbackBytes = static_cast<const uint8_t*>(
        device->mapBuffer(m_gpuSkinningParityReadbackBuffer, nvrhi::CpuAccessMode::Read));
    if (!readbackBytes)
    {
        common->Printf("PathTracePrimaryPass: PT GPU skinning parity readback map failed\n");
        m_gpuSkinningParityReadbackQueued = false;
        return;
    }

    constexpr float absoluteTolerance = 1.0e-3f;
    constexpr float relativeTolerance = 1.0e-5f;
    const size_t currentBytes =
        m_gpuSkinningParitySamples.size() * sizeof(PathTraceSmokeVertex);
    const PathTraceSmokeVertex* gpuCurrent =
        reinterpret_cast<const PathTraceSmokeVertex*>(readbackBytes);
    const PathTraceSkinnedPreviousPosition* gpuPrevious =
        reinterpret_cast<const PathTraceSkinnedPreviousPosition*>(readbackBytes + currentBytes);
    int currentFailures = 0;
    int previousFailures = 0;
    int motionFailures = 0;
    int nonFiniteOutputs = 0;
    float maxCurrentError = 0.0f;
    float maxPreviousError = 0.0f;
    float maxMotionError = 0.0f;

    for (size_t sampleIndex = 0; sampleIndex < m_gpuSkinningParitySamples.size(); ++sampleIndex)
    {
        const GpuSkinningParitySample& sample = m_gpuSkinningParitySamples[sampleIndex];
        const float* cpuCurrent = sample.cpuCurrent.position;
        const float* actualCurrent = gpuCurrent[sampleIndex].position;
        const bool currentFinite =
            GpuSkinningPositionFinite(cpuCurrent) &&
            GpuSkinningPositionFinite(actualCurrent);
        const float currentError =
            currentFinite ? GpuSkinningPositionMaxError(cpuCurrent, actualCurrent) : 1.0e30f;
        const bool currentPass = GpuSkinningPositionWithinTolerance(
            cpuCurrent,
            actualCurrent,
            absoluteTolerance,
            relativeTolerance);
        maxCurrentError = Max(maxCurrentError, currentError);
        currentFailures += currentPass ? 0 : 1;
        nonFiniteOutputs += GpuSkinningPositionFinite(actualCurrent) ? 0 : 1;

        bool previousPass = true;
        bool motionPass = true;
        float previousError = 0.0f;
        float motionError = 0.0f;
        float actualPreviousForLog[4] = {};
        if (sample.hasPrevious)
        {
            const float* cpuPrevious = sample.cpuPrevious.previousPosition;
            const float* actualPrevious = gpuPrevious[sampleIndex].previousPosition;
            memcpy(actualPreviousForLog, actualPrevious, sizeof(actualPreviousForLog));
            const bool previousFinite =
                GpuSkinningPositionFinite(cpuPrevious) &&
                GpuSkinningPositionFinite(actualPrevious);
            previousError =
                previousFinite ? GpuSkinningPositionMaxError(cpuPrevious, actualPrevious) : 1.0e30f;
            previousPass = GpuSkinningPositionWithinTolerance(
                cpuPrevious,
                actualPrevious,
                absoluteTolerance,
                relativeTolerance);
            float cpuMotion[4] = {};
            float gpuMotion[4] = {};
            for (int component = 0; component < 3; ++component)
            {
                cpuMotion[component] = cpuPrevious[component] - cpuCurrent[component];
                gpuMotion[component] = actualPrevious[component] - actualCurrent[component];
            }
            cpuMotion[3] = 1.0f;
            gpuMotion[3] = 1.0f;
            motionError = GpuSkinningPositionMaxError(cpuMotion, gpuMotion);
            motionPass = GpuSkinningPositionWithinTolerance(
                cpuMotion,
                gpuMotion,
                absoluteTolerance,
                relativeTolerance);
            maxPreviousError = Max(maxPreviousError, previousError);
            maxMotionError = Max(maxMotionError, motionError);
            previousFailures += previousPass ? 0 : 1;
            motionFailures += motionPass ? 0 : 1;
            nonFiniteOutputs += GpuSkinningPositionFinite(actualPrevious) ? 0 : 1;
        }

        common->Printf(
            "PathTracePrimaryPass: PT GPU skinning parity sample=%llu entity/model/surface/record/vertex=%d/'%s'/%d/%d/%d source=(%.9g %.9g %.9g) joints=%u,%u,%u,%u weights=%.9g,%.9g,%.9g,%.9g cpuCurrent=(%.9g %.9g %.9g) gpuCurrent=(%.9g %.9g %.9g) current(finite/error/pass)=%d/%.9g/%d hasPrevious=%d invalid=0x%08x temporal=0x%08x cpuPrevious=(%.9g %.9g %.9g) gpuPrevious=(%.9g %.9g %.9g) previous(error/pass)=%.9g/%d motion(error/pass)=%.9g/%d\n",
            static_cast<unsigned long long>(sampleIndex),
            sample.entityIndex,
            sample.modelName.c_str(),
            sample.drawSurfIndex,
            sample.surfaceRecordIndex,
            sample.vertexIndex,
            sample.source.localPosition[0],
            sample.source.localPosition[1],
            sample.source.localPosition[2],
            sample.source.jointIndices[0],
            sample.source.jointIndices[1],
            sample.source.jointIndices[2],
            sample.source.jointIndices[3],
            sample.source.jointWeights[0],
            sample.source.jointWeights[1],
            sample.source.jointWeights[2],
            sample.source.jointWeights[3],
            cpuCurrent[0],
            cpuCurrent[1],
            cpuCurrent[2],
            actualCurrent[0],
            actualCurrent[1],
            actualCurrent[2],
            currentFinite ? 1 : 0,
            currentError,
            currentPass ? 1 : 0,
            sample.hasPrevious ? 1 : 0,
            sample.previousInvalidReasonFlags,
            sample.temporalStateFlags,
            sample.cpuPrevious.previousPosition[0],
            sample.cpuPrevious.previousPosition[1],
            sample.cpuPrevious.previousPosition[2],
            actualPreviousForLog[0],
            actualPreviousForLog[1],
            actualPreviousForLog[2],
            previousError,
            previousPass ? 1 : 0,
            motionError,
            motionPass ? 1 : 0);
    }

    common->Printf(
        "PathTracePrimaryPass: PT GPU skinning parity summary frame=%llu mode=%d samples=%llu currentFailures=%d previousFailures=%d motionFailures=%d nonFiniteOutputs=%d maxError(current/previous/motion)=%.9g/%.9g/%.9g tolerance(abs/rel)=%.9g/%.9g pass=%d\n",
        static_cast<unsigned long long>(m_gpuSkinningParityFrame),
        m_gpuSkinningParityMode,
        static_cast<unsigned long long>(m_gpuSkinningParitySamples.size()),
        currentFailures,
        previousFailures,
        motionFailures,
        nonFiniteOutputs,
        maxCurrentError,
        maxPreviousError,
        maxMotionError,
        absoluteTolerance,
        relativeTolerance,
        currentFailures == 0 && previousFailures == 0 && motionFailures == 0 && nonFiniteOutputs == 0 ? 1 : 0);

    device->unmapBuffer(m_gpuSkinningParityReadbackBuffer);
    m_gpuSkinningParityReadbackQueued = false;
    m_gpuSkinningParitySamples.clear();
}

void PathTracePrimaryPass::QueueSkinnedEmissiveAudit(
    nvrhi::ICommandList* commandList,
    nvrhi::IBuffer* currentOutputBuffer,
    nvrhi::IBuffer* previousPositionBuffer,
    nvrhi::ResourceStates currentRestoreState,
    const std::vector<PtSkinnedEmissiveAuditTriangle>& triangles,
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeMaterial>& materials,
    const std::vector<PathTraceSmokeVertex>& cpuCurrentVertices,
    const std::vector<PathTraceSkinnedPreviousPosition>& cpuPreviousPositions,
    uint64 frameIndex,
    int forcedMaterialCount,
    int productionEligibleMaterialCount)
{
    if (m_skinnedEmissiveAuditReadbackQueued)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO09 skinned emissive GPU-output audit ignored because a readback is already queued\n");
        return;
    }
    nvrhi::IDevice* device =
        deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!commandList || !device ||
        !currentOutputBuffer || !previousPositionBuffer ||
        triangles.empty() || cpuCurrentVertices.empty())
    {
        common->Printf(
            "PathTracePrimaryPass: GEO09 skinned emissive GPU-output audit unavailable before copy\n");
        return;
    }

    const uint64 currentBytes =
        static_cast<uint64>(cpuCurrentVertices.size()) *
        sizeof(PathTraceSmokeVertex);
    const uint64 previousBytes =
        static_cast<uint64>(cpuPreviousPositions.size()) *
        sizeof(PathTraceSkinnedPreviousPosition);
    if (currentBytes > currentOutputBuffer->getDesc().byteSize ||
        previousBytes > previousPositionBuffer->getDesc().byteSize)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO09 skinned emissive GPU-output audit range invalid current=%llu/%llu previous=%llu/%llu\n",
            static_cast<unsigned long long>(currentBytes),
            static_cast<unsigned long long>(
                currentOutputBuffer->getDesc().byteSize),
            static_cast<unsigned long long>(previousBytes),
            static_cast<unsigned long long>(
                previousPositionBuffer->getDesc().byteSize));
        return;
    }

    const uint64 totalBytes = currentBytes + previousBytes;
    if (!m_skinnedEmissiveAuditReadbackBuffer ||
        m_skinnedEmissiveAuditReadbackBuffer->
            getDesc().byteSize < totalBytes)
    {
        m_skinnedEmissiveAuditReadbackBuffer = nullptr;
        nvrhi::BufferDesc desc;
        desc.byteSize = totalBytes;
        desc.structStride = sizeof(uint32_t);
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
        desc.debugName =
            "PathTraceSkinnedEmissiveAuditReadback";
        desc.initialState =
            nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        m_skinnedEmissiveAuditReadbackBuffer =
            device->createBuffer(desc);
    }
    if (!m_skinnedEmissiveAuditReadbackBuffer)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO09 skinned emissive GPU-output audit readback buffer creation failed\n");
        return;
    }

    m_skinnedEmissiveAuditTriangles = triangles;
    m_skinnedEmissiveAuditMaterialIds = materialIds;
    m_skinnedEmissiveAuditMaterials = materials;
    m_skinnedEmissiveAuditExpectedCurrentVertices =
        cpuCurrentVertices;
    m_skinnedEmissiveAuditExpectedPreviousPositions =
        cpuPreviousPositions;
    m_skinnedEmissiveAuditExpected =
        BuildSmokeCanonicalSkinnedEmissiveAuditInventory(
            materialIds,
            materials,
            cpuCurrentVertices,
            cpuPreviousPositions,
            triangles,
            RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE,
            RT_SMOKE_EMISSIVE_AUDIT_MAX_RECORDS);
    m_skinnedEmissiveAuditFrame = frameIndex;
    m_skinnedEmissiveAuditCurrentBytes = currentBytes;
    m_skinnedEmissiveAuditPreviousBytes = previousBytes;
    m_skinnedEmissiveAuditForcedMaterialCount =
        forcedMaterialCount;
    m_skinnedEmissiveAuditProductionEligibleMaterialCount =
        productionEligibleMaterialCount;

    commandList->setBufferState(
        currentOutputBuffer,
        nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(
        previousPositionBuffer,
        nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(
        m_skinnedEmissiveAuditReadbackBuffer,
        nvrhi::ResourceStates::CopyDest);
    commandList->commitBarriers();
    commandList->copyBuffer(
        m_skinnedEmissiveAuditReadbackBuffer,
        0,
        currentOutputBuffer,
        0,
        currentBytes);
    if (previousBytes > 0)
    {
        commandList->copyBuffer(
            m_skinnedEmissiveAuditReadbackBuffer,
            currentBytes,
            previousPositionBuffer,
            0,
            previousBytes);
    }
    commandList->setBufferState(
        currentOutputBuffer,
        currentRestoreState);
    commandList->setBufferState(
        previousPositionBuffer,
        nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();

    m_skinnedEmissiveAuditReadbackQueued = true;
    m_skinnedEmissiveAuditReadbackDelayFrames = 3;
    common->Printf(
        "PathTracePrimaryPass: GEO09 skinned emissive GPU-output audit queued frame=%llu triangles=%llu bytes(current/previous)=%llu/%llu materials(productionEligible/validationForced)=%d/%d expected(current/previous)=%llu/%llu productionBehaviorChanged=0\n",
        static_cast<unsigned long long>(frameIndex),
        static_cast<unsigned long long>(triangles.size()),
        static_cast<unsigned long long>(currentBytes),
        static_cast<unsigned long long>(previousBytes),
        productionEligibleMaterialCount,
        forcedMaterialCount,
        static_cast<unsigned long long>(
            m_skinnedEmissiveAuditExpected.current.size()),
        static_cast<unsigned long long>(
            m_skinnedEmissiveAuditExpected.previous.size()));
}

void PathTracePrimaryPass::ReadBackSkinnedEmissiveAudit()
{
    if (!m_skinnedEmissiveAuditReadbackQueued ||
        !m_skinnedEmissiveAuditReadbackBuffer)
    {
        return;
    }
    if (m_skinnedEmissiveAuditReadbackDelayFrames > 0)
    {
        --m_skinnedEmissiveAuditReadbackDelayFrames;
        return;
    }
    nvrhi::IDevice* device =
        deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!device)
    {
        return;
    }
    const uint8_t* readbackBytes =
        static_cast<const uint8_t*>(
            device->mapBuffer(
                m_skinnedEmissiveAuditReadbackBuffer,
                nvrhi::CpuAccessMode::Read));
    if (!readbackBytes)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO09 skinned emissive GPU-output audit readback map failed\n");
        m_skinnedEmissiveAuditReadbackQueued = false;
        return;
    }

    std::vector<PathTraceSmokeVertex> gpuCurrent(
        static_cast<size_t>(
            m_skinnedEmissiveAuditCurrentBytes /
            sizeof(PathTraceSmokeVertex)));
    std::vector<PathTraceSkinnedPreviousPosition> gpuPrevious(
        static_cast<size_t>(
            m_skinnedEmissiveAuditPreviousBytes /
            sizeof(PathTraceSkinnedPreviousPosition)));
    if (!gpuCurrent.empty())
    {
        memcpy(
            gpuCurrent.data(),
            readbackBytes,
            static_cast<size_t>(
                m_skinnedEmissiveAuditCurrentBytes));
    }
    if (!gpuPrevious.empty())
    {
        memcpy(
            gpuPrevious.data(),
            readbackBytes +
                m_skinnedEmissiveAuditCurrentBytes,
            static_cast<size_t>(
                m_skinnedEmissiveAuditPreviousBytes));
    }

    const PtSkinnedEmissiveAuditInventory actual =
        BuildSmokeCanonicalSkinnedEmissiveAuditInventory(
            m_skinnedEmissiveAuditMaterialIds,
            m_skinnedEmissiveAuditMaterials,
            gpuCurrent,
            gpuPrevious,
            m_skinnedEmissiveAuditTriangles,
            RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE,
            RT_SMOKE_EMISSIVE_AUDIT_MAX_RECORDS);
    const std::vector<PathTraceEmissiveLightRemap>
        expectedRemap = BuildSmokeCanonicalEmissiveLightRemap(
            m_skinnedEmissiveAuditExpected.current,
            m_skinnedEmissiveAuditExpected.previous);
    const std::vector<PathTraceEmissiveLightRemap>
        actualRemap = BuildSmokeCanonicalEmissiveLightRemap(
            actual.current,
            actual.previous);

    constexpr float vertexAbsoluteTolerance = 1.0e-3f;
    constexpr float aggregateAbsoluteTolerance = 1.0e-2f;
    constexpr float relativeTolerance = 1.0e-5f;
    uint64 currentVertexMismatch = 0;
    uint64 previousVertexMismatch = 0;
    float maxCurrentVertexError = 0.0f;
    float maxPreviousVertexError = 0.0f;
    std::unordered_set<uint32_t> comparedCurrentVertices;
    std::unordered_set<uint32_t> comparedPreviousPositions;
    for (const PtSkinnedEmissiveAuditTriangle& triangle :
        m_skinnedEmissiveAuditTriangles)
    {
        for (int corner = 0; corner < 3; ++corner)
        {
            const uint32_t currentIndex =
                triangle.currentVertexIndexes[corner];
            if (currentIndex <
                    gpuCurrent.size() &&
                currentIndex <
                    m_skinnedEmissiveAuditExpectedCurrentVertices.
                        size() &&
                comparedCurrentVertices.insert(
                    currentIndex).second)
            {
                const float* expectedPosition =
                    m_skinnedEmissiveAuditExpectedCurrentVertices[
                        currentIndex].position;
                const float* actualPosition =
                    gpuCurrent[currentIndex].position;
                maxCurrentVertexError =
                    Max(maxCurrentVertexError,
                        GpuSkinningPositionMaxError(
                            expectedPosition,
                            actualPosition));
                currentVertexMismatch +=
                    GpuSkinningPositionWithinTolerance(
                        expectedPosition,
                        actualPosition,
                        vertexAbsoluteTolerance,
                        relativeTolerance)
                        ? 0u
                        : 1u;
            }
            if (triangle.hasPrevious)
            {
                const uint32_t previousIndex =
                    triangle.previousPositionIndexes[corner];
                if (previousIndex <
                        gpuPrevious.size() &&
                    previousIndex <
                        m_skinnedEmissiveAuditExpectedPreviousPositions.
                            size() &&
                    comparedPreviousPositions.insert(
                        previousIndex).second)
                {
                    const float* expectedPosition =
                        m_skinnedEmissiveAuditExpectedPreviousPositions[
                            previousIndex].previousPosition;
                    const float* actualPosition =
                        gpuPrevious[previousIndex].
                            previousPosition;
                    maxPreviousVertexError =
                        Max(maxPreviousVertexError,
                            GpuSkinningPositionMaxError(
                                expectedPosition,
                                actualPosition));
                    previousVertexMismatch +=
                        GpuSkinningPositionWithinTolerance(
                            expectedPosition,
                            actualPosition,
                            vertexAbsoluteTolerance,
                            relativeTolerance)
                            ? 0u
                            : 1u;
                }
            }
        }
    }
    uint64 identityMismatch = 0;
    uint64 metadataMismatch = 0;
    uint64 numericMismatch = 0;
    uint64 remapMismatch = 0;
    uint64 zeroIdentity = 0;
    uint64 identityCollisions = 0;
    uint64 remapValid = 0;
    uint64 remapPreviousMissing = 0;
    float maxNumericError = 0.0f;
    std::unordered_set<uint64> identities;
    std::unordered_set<uint32_t> emissiveInstances;
    std::unordered_set<uint32_t> emissiveMaterials;

    auto compareRecords =
        [&](const std::vector<PathTraceSmokeEmissiveTriangle>& expected,
            const std::vector<PathTraceSmokeEmissiveTriangle>& observed)
        {
            const size_t compareCount =
                Min(expected.size(), observed.size());
            for (size_t recordIndex = 0;
                recordIndex < compareCount;
                ++recordIndex)
            {
                const PathTraceSmokeEmissiveTriangle& lhs =
                    expected[recordIndex];
                const PathTraceSmokeEmissiveTriangle& rhs =
                    observed[recordIndex];
                if (lhs.identityHashLo != rhs.identityHashLo ||
                    lhs.identityHashHi != rhs.identityHashHi)
                {
                    ++identityMismatch;
                }
                if (lhs.materialIndex != rhs.materialIndex ||
                    lhs.instanceId != rhs.instanceId ||
                    lhs.primitiveIndex != rhs.primitiveIndex ||
                    lhs.flags != rhs.flags ||
                    lhs.emissiveTextureIndex !=
                        rhs.emissiveTextureIndex ||
                    lhs.materialId != rhs.materialId ||
                    lhs.universeMaterialIndex !=
                        rhs.universeMaterialIndex ||
                    lhs.padding0 != rhs.padding0)
                {
                    ++metadataMismatch;
                }
                const float* lhsFloats =
                    lhs.centerAndArea;
                const float* rhsFloats =
                    rhs.centerAndArea;
                for (int component = 0;
                    component < 24;
                    ++component)
                {
                    const float delta =
                        idMath::Fabs(
                            lhsFloats[component] -
                            rhsFloats[component]);
                    const float tolerance =
                        aggregateAbsoluteTolerance +
                        relativeTolerance *
                            Max(idMath::Fabs(
                                    lhsFloats[component]),
                                idMath::Fabs(
                                    rhsFloats[component]));
                    maxNumericError =
                        Max(maxNumericError, delta);
                    if (!std::isfinite(
                            lhsFloats[component]) ||
                        !std::isfinite(
                            rhsFloats[component]) ||
                        delta > tolerance)
                    {
                        ++numericMismatch;
                    }
                }
            }
            if (expected.size() != observed.size())
            {
                metadataMismatch +=
                    static_cast<uint64>(
                        expected.size() > observed.size()
                            ? expected.size() - observed.size()
                            : observed.size() - expected.size());
            }
        };
    compareRecords(
        m_skinnedEmissiveAuditExpected.current,
        actual.current);
    compareRecords(
        m_skinnedEmissiveAuditExpected.previous,
        actual.previous);

    for (const PathTraceSmokeEmissiveTriangle& record :
        actual.current)
    {
        emissiveInstances.insert(record.instanceId);
        emissiveMaterials.insert(record.materialId);
        const uint64 identity =
            (static_cast<uint64>(record.identityHashHi) <<
                32ull) |
            record.identityHashLo;
        if (identity == 0)
        {
            ++zeroIdentity;
        }
        else if (!identities.insert(identity).second)
        {
            ++identityCollisions;
        }
    }
    const size_t remapCompareCount =
        Min(expectedRemap.size(), actualRemap.size());
    for (size_t remapIndex = 0;
        remapIndex < remapCompareCount;
        ++remapIndex)
    {
        const PathTraceEmissiveLightRemap& lhs =
            expectedRemap[remapIndex];
        const PathTraceEmissiveLightRemap& rhs =
            actualRemap[remapIndex];
        if (memcmp(&lhs, &rhs, sizeof(lhs)) != 0)
        {
            ++remapMismatch;
        }
        remapValid +=
            (rhs.flags & RT_SMOKE_EMISSIVE_REMAP_VALID) != 0u
                ? 1u
                : 0u;
        remapPreviousMissing +=
            (rhs.flags &
                RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_MISSING) !=
                    0u
                ? 1u
                : 0u;
    }
    if (expectedRemap.size() != actualRemap.size())
    {
        remapMismatch +=
            static_cast<uint64>(
                expectedRemap.size() > actualRemap.size()
                    ? expectedRemap.size() -
                        actualRemap.size()
                    : actualRemap.size() -
                        expectedRemap.size());
    }

    bool stableRemapTest = false;
    bool spawnRemapTest = false;
    bool despawnRemapTest = false;
    bool identicalInstanceRemapTest = false;
    if (!actual.current.empty())
    {
        const std::vector<PathTraceEmissiveLightRemap>
            stableRemap =
                BuildSmokeCanonicalEmissiveLightRemap(
                    actual.current,
                    actual.current);
        stableRemapTest =
            stableRemap.size() == actual.current.size();
        for (size_t recordIndex = 0;
            stableRemapTest &&
            recordIndex < stableRemap.size();
            ++recordIndex)
        {
            stableRemapTest =
                (stableRemap[recordIndex].flags &
                    RT_SMOKE_EMISSIVE_REMAP_VALID) != 0u &&
                stableRemap[recordIndex].
                    currentToPreviousIndex ==
                    static_cast<int32_t>(recordIndex) &&
                stableRemap[recordIndex].
                    previousToCurrentIndex ==
                    static_cast<int32_t>(recordIndex);
        }

        std::vector<PathTraceSmokeEmissiveTriangle>
            spawnedCurrent = actual.current;
        PathTraceSmokeEmissiveTriangle spawnedRecord =
            actual.current.front();
        uint64 spawnedIdentity =
            (static_cast<uint64>(
                spawnedRecord.identityHashHi) << 32ull) |
            spawnedRecord.identityHashLo;
        spawnedIdentity ^= 0x9e3779b97f4a7c15ull;
        while (spawnedIdentity == 0 ||
            identities.find(spawnedIdentity) !=
                identities.end())
        {
            ++spawnedIdentity;
        }
        spawnedRecord.identityHashLo =
            static_cast<uint32_t>(
                spawnedIdentity & 0xffffffffu);
        spawnedRecord.identityHashHi =
            static_cast<uint32_t>(
                spawnedIdentity >> 32);
        ++spawnedRecord.instanceId;
        spawnedCurrent.push_back(spawnedRecord);
        const std::vector<PathTraceEmissiveLightRemap>
            spawnRemap =
                BuildSmokeCanonicalEmissiveLightRemap(
                    spawnedCurrent,
                    actual.current);
        const size_t spawnedIndex =
            spawnedCurrent.size() - 1;
        spawnRemapTest =
            spawnRemap.size() == spawnedCurrent.size() &&
            (spawnRemap[spawnedIndex].flags &
                RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_MISSING) !=
                0u &&
            (spawnRemap[spawnedIndex].flags &
                RT_SMOKE_EMISSIVE_REMAP_VALID) == 0u;

        std::vector<PathTraceSmokeEmissiveTriangle>
            despawnedCurrent = actual.current;
        despawnedCurrent.pop_back();
        const std::vector<PathTraceEmissiveLightRemap>
            despawnRemap =
                BuildSmokeCanonicalEmissiveLightRemap(
                    despawnedCurrent,
                    actual.current);
        const size_t removedIndex =
            actual.current.size() - 1;
        despawnRemapTest =
            despawnRemap.size() == actual.current.size() &&
            (despawnRemap[removedIndex].flags &
                RT_SMOKE_EMISSIVE_REMAP_CURRENT_MISSING) !=
                0u &&
            (despawnRemap[removedIndex].flags &
                RT_SMOKE_EMISSIVE_REMAP_VALID) == 0u;

        std::vector<PathTraceSmokeEmissiveTriangle>
            identicalInstances;
        identicalInstances.push_back(
            actual.current.front());
        identicalInstances.push_back(spawnedRecord);
        const std::vector<PathTraceEmissiveLightRemap>
            identicalRemap =
                BuildSmokeCanonicalEmissiveLightRemap(
                    identicalInstances,
                    identicalInstances);
        const uint32_t duplicateFlags =
            RT_SMOKE_EMISSIVE_REMAP_CURRENT_DUPLICATE |
            RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_DUPLICATE;
        identicalInstanceRemapTest =
            identicalRemap.size() == 2 &&
            (identicalRemap[0].flags &
                RT_SMOKE_EMISSIVE_REMAP_VALID) != 0u &&
            (identicalRemap[1].flags &
                RT_SMOKE_EMISSIVE_REMAP_VALID) != 0u &&
            (identicalRemap[0].flags & duplicateFlags) == 0u &&
            (identicalRemap[1].flags & duplicateFlags) == 0u;
    }

    const bool countersExact =
        actual.inputTriangles ==
            m_skinnedEmissiveAuditExpected.inputTriangles &&
        actual.invalidTriangles ==
            m_skinnedEmissiveAuditExpected.invalidTriangles &&
        actual.nonEmissiveTriangles ==
            m_skinnedEmissiveAuditExpected.nonEmissiveTriangles &&
        actual.runtimeInactiveTriangles ==
            m_skinnedEmissiveAuditExpected.runtimeInactiveTriangles &&
        actual.zeroIdentityTriangles ==
            m_skinnedEmissiveAuditExpected.zeroIdentityTriangles &&
        actual.zeroAreaCurrentTriangles ==
            m_skinnedEmissiveAuditExpected.zeroAreaCurrentTriangles &&
        actual.zeroAreaPreviousTriangles ==
            m_skinnedEmissiveAuditExpected.zeroAreaPreviousTriangles &&
        actual.missingPreviousTriangles ==
            m_skinnedEmissiveAuditExpected.missingPreviousTriangles;
    const bool accepted =
        !actual.current.empty() &&
        m_skinnedEmissiveAuditForcedMaterialCount > 0 &&
        countersExact &&
        identityMismatch == 0 &&
        metadataMismatch == 0 &&
        numericMismatch == 0 &&
        remapMismatch == 0 &&
        currentVertexMismatch == 0 &&
        previousVertexMismatch == 0 &&
        zeroIdentity == 0 &&
        identityCollisions == 0 &&
        stableRemapTest &&
        spawnRemapTest &&
        despawnRemapTest &&
        identicalInstanceRemapTest;
    common->Printf(
        "PathTracePrimaryPass: GEO09 skinned emissive GPU-output audit summary frame=%llu accepted=%d productionBehaviorChanged=0 materials(productionEligible/validationForced)=%d/%d triangles(input/nonEmissive/inactive/invalid/zeroIdentity)=%llu/%llu/%llu/%llu/%llu inventory(expectedCurrent/actualCurrent/expectedPrevious/actualPrevious/instances/materials)=%llu/%llu/%llu/%llu/%llu/%llu geometry(zeroAreaCurrent/zeroAreaPrevious/missingPrevious)=%llu/%llu/%llu vertices(comparedCurrent/comparedPrevious/mismatchCurrent/mismatchPrevious/maxCurrent/maxPrevious)=%llu/%llu/%llu/%llu/%.9g/%.9g compare(identity/metadata/numeric/remap/countersExact/maxAggregateError)= %llu/%llu/%llu/%llu/%d/%.9g identity(zero/collisions)=%llu/%llu remap(valid/previousMissing/total)=%llu/%llu/%llu transition(stable/spawn/despawn/identicalInstance)=%d/%d/%d/%d tolerance(vertexAbs/aggregateAbs/rel)=%.9g/%.9g/%.9g\n",
        static_cast<unsigned long long>(
            m_skinnedEmissiveAuditFrame),
        accepted ? 1 : 0,
        m_skinnedEmissiveAuditProductionEligibleMaterialCount,
        m_skinnedEmissiveAuditForcedMaterialCount,
        static_cast<unsigned long long>(
            actual.inputTriangles),
        static_cast<unsigned long long>(
            actual.nonEmissiveTriangles),
        static_cast<unsigned long long>(
            actual.runtimeInactiveTriangles),
        static_cast<unsigned long long>(
            actual.invalidTriangles),
        static_cast<unsigned long long>(
            actual.zeroIdentityTriangles),
        static_cast<unsigned long long>(
            m_skinnedEmissiveAuditExpected.current.size()),
        static_cast<unsigned long long>(
            actual.current.size()),
        static_cast<unsigned long long>(
            m_skinnedEmissiveAuditExpected.previous.size()),
        static_cast<unsigned long long>(
            actual.previous.size()),
        static_cast<unsigned long long>(
            emissiveInstances.size()),
        static_cast<unsigned long long>(
            emissiveMaterials.size()),
        static_cast<unsigned long long>(
            actual.zeroAreaCurrentTriangles),
        static_cast<unsigned long long>(
            actual.zeroAreaPreviousTriangles),
        static_cast<unsigned long long>(
            actual.missingPreviousTriangles),
        static_cast<unsigned long long>(
            comparedCurrentVertices.size()),
        static_cast<unsigned long long>(
            comparedPreviousPositions.size()),
        static_cast<unsigned long long>(
            currentVertexMismatch),
        static_cast<unsigned long long>(
            previousVertexMismatch),
        maxCurrentVertexError,
        maxPreviousVertexError,
        static_cast<unsigned long long>(identityMismatch),
        static_cast<unsigned long long>(metadataMismatch),
        static_cast<unsigned long long>(numericMismatch),
        static_cast<unsigned long long>(remapMismatch),
        countersExact ? 1 : 0,
        maxNumericError,
        static_cast<unsigned long long>(zeroIdentity),
        static_cast<unsigned long long>(identityCollisions),
        static_cast<unsigned long long>(remapValid),
        static_cast<unsigned long long>(
            remapPreviousMissing),
        static_cast<unsigned long long>(actualRemap.size()),
        stableRemapTest ? 1 : 0,
        spawnRemapTest ? 1 : 0,
        despawnRemapTest ? 1 : 0,
        identicalInstanceRemapTest ? 1 : 0,
        vertexAbsoluteTolerance,
        aggregateAbsoluteTolerance,
        relativeTolerance);

    device->unmapBuffer(
        m_skinnedEmissiveAuditReadbackBuffer);
    m_skinnedEmissiveAuditReadbackQueued = false;
    m_skinnedEmissiveAuditTriangles.clear();
    m_skinnedEmissiveAuditMaterialIds.clear();
    m_skinnedEmissiveAuditMaterials.clear();
    m_skinnedEmissiveAuditExpectedCurrentVertices.clear();
    m_skinnedEmissiveAuditExpectedPreviousPositions.clear();
    m_skinnedEmissiveAuditExpected =
        PtSkinnedEmissiveAuditInventory();
}

void PathTracePrimaryPass::QueueSkinnedHitRouteReadback(
    nvrhi::ICommandList* commandList,
    nvrhi::IBuffer* recordBuffer,
    nvrhi::IBuffer* triangleBuffer,
    const PtSkinnedHitRouteGpuUpload& expected,
    uint64 frameIndex)
{
    if (m_skinnedHitRouteReadbackQueued ||
        expected.records.empty() ||
        expected.triangles.empty())
    {
        return;
    }
    nvrhi::IDevice* device =
        deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!commandList || !device ||
        !recordBuffer || !triangleBuffer)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 skinned hit-route GPU readback unavailable before copy\n");
        return;
    }

    const uint64 recordBytes =
        expected.records.size() *
        sizeof(PathTraceSkinnedHitRouteGpuRecord);
    const uint64 triangleBytes =
        expected.triangles.size() *
        sizeof(PathTraceSkinnedHitRouteGpuTriangle);
    const uint64 totalBytes = recordBytes + triangleBytes;
    if (recordBytes > recordBuffer->getDesc().byteSize ||
        triangleBytes > triangleBuffer->getDesc().byteSize)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 skinned hit-route GPU readback range invalid records=%llu/%llu triangles=%llu/%llu\n",
            static_cast<unsigned long long>(recordBytes),
            static_cast<unsigned long long>(
                recordBuffer->getDesc().byteSize),
            static_cast<unsigned long long>(triangleBytes),
            static_cast<unsigned long long>(
                triangleBuffer->getDesc().byteSize));
        return;
    }
    if (!m_skinnedHitRouteReadbackBuffer ||
        m_skinnedHitRouteReadbackBuffer->getDesc().byteSize <
            totalBytes)
    {
        m_skinnedHitRouteReadbackBuffer = nullptr;
        nvrhi::BufferDesc desc;
        desc.byteSize = totalBytes;
        desc.structStride = sizeof(uint32_t);
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
        desc.debugName =
            "PathTraceSkinnedHitRouteReadback";
        desc.initialState =
            nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        m_skinnedHitRouteReadbackBuffer =
            device->createBuffer(desc);
    }
    if (!m_skinnedHitRouteReadbackBuffer)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 skinned hit-route GPU readback buffer creation failed\n");
        return;
    }

    commandList->setBufferState(
        recordBuffer,
        nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(
        triangleBuffer,
        nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(
        m_skinnedHitRouteReadbackBuffer,
        nvrhi::ResourceStates::CopyDest);
    commandList->commitBarriers();
    commandList->copyBuffer(
        m_skinnedHitRouteReadbackBuffer,
        0,
        recordBuffer,
        0,
        recordBytes);
    commandList->copyBuffer(
        m_skinnedHitRouteReadbackBuffer,
        recordBytes,
        triangleBuffer,
        0,
        triangleBytes);
    commandList->setBufferState(
        recordBuffer,
        nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(
        triangleBuffer,
        nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();

    m_skinnedHitRouteReadbackExpected = expected;
    m_skinnedHitRouteReadbackFrame = frameIndex;
    m_skinnedHitRouteReadbackDelayFrames = 3;
    m_skinnedHitRouteReadbackQueued = true;
    common->Printf(
        "PathTracePrimaryPass: GEO08 skinned hit-route GPU readback queued frame=%llu records=%llu triangles=%llu bytes=%llu signature=%016llx tlas=excluded\n",
        static_cast<unsigned long long>(frameIndex),
        static_cast<unsigned long long>(
            expected.records.size()),
        static_cast<unsigned long long>(
            expected.triangles.size()),
        static_cast<unsigned long long>(totalBytes),
        static_cast<unsigned long long>(expected.signature));
}

void PathTracePrimaryPass::ReadBackSkinnedHitRoute()
{
    if (!m_skinnedHitRouteReadbackQueued ||
        !m_skinnedHitRouteReadbackBuffer)
    {
        return;
    }
    if (m_skinnedHitRouteReadbackDelayFrames > 0)
    {
        --m_skinnedHitRouteReadbackDelayFrames;
        return;
    }
    nvrhi::IDevice* device =
        deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!device)
    {
        return;
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(
        device->mapBuffer(
            m_skinnedHitRouteReadbackBuffer,
            nvrhi::CpuAccessMode::Read));
    if (!bytes)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 skinned hit-route GPU readback map failed\n");
        m_skinnedHitRouteReadbackQueued = false;
        return;
    }

    const size_t recordBytes =
        m_skinnedHitRouteReadbackExpected.records.size() *
        sizeof(PathTraceSkinnedHitRouteGpuRecord);
    const size_t triangleBytes =
        m_skinnedHitRouteReadbackExpected.triangles.size() *
        sizeof(PathTraceSkinnedHitRouteGpuTriangle);
    const bool recordsExact =
        memcmp(
            bytes,
            m_skinnedHitRouteReadbackExpected.records.data(),
            recordBytes) == 0;
    const bool trianglesExact =
        memcmp(
            bytes + recordBytes,
            m_skinnedHitRouteReadbackExpected.triangles.data(),
            triangleBytes) == 0;
    const PathTraceSkinnedHitRouteGpuRecord& header =
        m_skinnedHitRouteReadbackExpected.records.front();
    const bool headerExact =
        header.routeCount ==
            m_skinnedHitRouteReadbackExpected.records.size() &&
        header.triangleMetadataCount ==
            m_skinnedHitRouteReadbackExpected.triangles.size();
    const bool pass =
        recordsExact && trianglesExact && headerExact;
    common->Printf(
        "PathTracePrimaryPass: GEO08 skinned hit-route GPU readback summary frame=%llu records=%llu triangles=%llu firstInstance=%u counts(route/metadata)=%u/%u exact(records/triangles/header)=%d/%d/%d signature=%016llx pass=%d tlas=excluded\n",
        static_cast<unsigned long long>(
            m_skinnedHitRouteReadbackFrame),
        static_cast<unsigned long long>(
            m_skinnedHitRouteReadbackExpected.records.size()),
        static_cast<unsigned long long>(
            m_skinnedHitRouteReadbackExpected.triangles.size()),
        header.shaderInstanceId,
        header.routeCount,
        header.triangleMetadataCount,
        recordsExact ? 1 : 0,
        trianglesExact ? 1 : 0,
        headerExact ? 1 : 0,
        static_cast<unsigned long long>(
            m_skinnedHitRouteReadbackExpected.signature),
        pass ? 1 : 0);
    device->unmapBuffer(m_skinnedHitRouteReadbackBuffer);
    m_skinnedHitRouteReadbackQueued = false;
    m_skinnedHitRouteReadbackCompleted = pass;
    m_skinnedHitRouteReadbackExpected =
        PtSkinnedHitRouteGpuUpload();
}

void PathTracePrimaryPass::QueueSkinnedHitAuditSamples(
    nvrhi::ICommandList* commandList)
{
    if (!m_skinnedHitAuditRequested ||
        m_skinnedHitAuditReadbackQueued)
    {
        return;
    }

    nvrhi::IDevice* device =
        deviceManager ? deviceManager->GetDevice() : nullptr;
    const RtRestirPTPrimarySurfaceHistoryBufferHandles& history =
        m_frameResources.primarySurfaceHistoryBuffers;
    const int width = m_frameResources.width;
    const int height = m_frameResources.height;
    const int samplePairWidth =
        width >= 2 ? (width + 6) / 8 : 0;
    const int sampleHeight =
        height > 0 ? (height + 7) / 8 : 0;
    const uint64_t recordCount =
        samplePairWidth > 0 && sampleHeight > 0
            ? static_cast<uint64_t>(samplePairWidth) *
                static_cast<uint64_t>(sampleHeight) *
                2ull
            : 0;
    const uint64_t readbackBytes =
        recordCount *
        sizeof(RtPathTracePrimarySurfaceRecord);
    if (!commandList || !device || !history.current ||
        width < 2 || height <= 0 ||
        readbackBytes == 0 ||
        readbackBytes > history.current->getDesc().byteSize)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO09 skinned hit audit unavailable before copy dimensions=%d/%d bytes=%llu historyBytes=%llu\n",
            width,
            height,
            static_cast<unsigned long long>(readbackBytes),
            static_cast<unsigned long long>(
                history.current
                    ? history.current->getDesc().byteSize
                    : 0));
        return;
    }

    if (!m_skinnedHitAuditReadbackBuffer ||
        m_skinnedHitAuditReadbackBuffer->getDesc().byteSize <
            readbackBytes)
    {
        m_skinnedHitAuditReadbackBuffer = nullptr;
        nvrhi::BufferDesc desc;
        desc.byteSize = readbackBytes;
        desc.structStride = sizeof(uint32_t);
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
        desc.debugName = "PathTraceSkinnedHitAuditReadback";
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        m_skinnedHitAuditReadbackBuffer =
            device->createBuffer(desc);
    }
    if (!m_skinnedHitAuditReadbackBuffer)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO09 skinned hit audit readback buffer creation failed bytes=%llu\n",
            static_cast<unsigned long long>(readbackBytes));
        return;
    }

    commandList->setBufferState(
        history.current,
        nvrhi::ResourceStates::CopySource);
    commandList->setBufferState(
        m_skinnedHitAuditReadbackBuffer,
        nvrhi::ResourceStates::CopyDest);
    commandList->commitBarriers();
    commandList->copyBuffer(
        m_skinnedHitAuditReadbackBuffer,
        0,
        history.current,
        0,
        readbackBytes);
    commandList->setBufferState(
        history.current,
        nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    m_skinnedHitAuditWidth = width;
    m_skinnedHitAuditHeight = height;
    m_skinnedHitAuditRequested = false;
    m_skinnedHitAuditReadbackQueued = true;
    m_skinnedHitAuditReadbackDelayFrames = 3;
    common->Printf(
        "PathTracePrimaryPass: GEO09 skinned hit audit queued frame=%llu dimensions=%d/%d sampleGrid=%d/%d pairs=%llu bytes=%llu\n",
        static_cast<unsigned long long>(m_skinnedHitAuditFrame),
        width,
        height,
        samplePairWidth,
        sampleHeight,
        static_cast<unsigned long long>(
            static_cast<uint64_t>(samplePairWidth) *
            static_cast<uint64_t>(sampleHeight)),
        static_cast<unsigned long long>(readbackBytes));
}

void PathTracePrimaryPass::ReadBackSkinnedHitAuditSamples()
{
    if (!m_skinnedHitAuditReadbackQueued ||
        !m_skinnedHitAuditReadbackBuffer)
    {
        return;
    }
    if (m_skinnedHitAuditReadbackDelayFrames > 0)
    {
        --m_skinnedHitAuditReadbackDelayFrames;
        return;
    }

    nvrhi::IDevice* device =
        deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!device)
    {
        return;
    }
    const RtPathTracePrimarySurfaceRecord* records =
        static_cast<const RtPathTracePrimarySurfaceRecord*>(
            device->mapBuffer(
                m_skinnedHitAuditReadbackBuffer,
                nvrhi::CpuAccessMode::Read));
    if (!records)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO09 skinned hit audit readback map failed\n");
        m_skinnedHitAuditReadbackQueued = false;
        return;
    }

    constexpr float tupleTolerance = 1.0e-3f;
    constexpr float basisTolerance = 1.0e-2f;
    uint64_t totalPairs = 0;
    uint64_t canonicalSkinnedHits = 0;
    uint64_t primitiveRangeMissing = 0;
    uint64_t legacyMisses = 0;
    uint64_t legacyCloserOccluders = 0;
    uint64_t comparable = 0;
    uint64_t nonFinite = 0;
    uint64_t primitiveMismatch = 0;
    uint64_t distanceMismatch = 0;
    uint64_t positionMismatch = 0;
    uint64_t geometricNormalMismatch = 0;
    uint64_t shadingNormalMismatch = 0;
    uint64_t tangentMismatch = 0;
    uint64_t bitangentMismatch = 0;
    uint64_t uvMismatch = 0;
    uint64_t normalUvMismatch = 0;
    uint64_t barycentricMismatch = 0;
    uint64_t materialMismatch = 0;
    uint64_t triangleFlagsMismatch = 0;
    uint64_t legacyMotionValid = 0;
    uint64_t canonicalMotionValid = 0;
    uint64_t motionComparable = 0;
    uint64_t motionInvalidBoth = 0;
    uint64_t motionValidityMismatch = 0;
    uint64_t motionStatusMismatch = 0;
    uint64_t previousPositionMismatch = 0;
    float maxHitTDelta = 0.0f;
    float maxPositionDelta = 0.0f;
    float maxGeometricNormalDelta = 0.0f;
    float maxShadingNormalDelta = 0.0f;
    float maxTangentDelta = 0.0f;
    float maxBitangentDelta = 0.0f;
    float maxUvDelta = 0.0f;
    float maxNormalUvDelta = 0.0f;
    float maxBarycentricDelta = 0.0f;
    float maxPreviousPositionDelta = 0.0f;
    int mismatchDetailsLogged = 0;

    auto recordValid =
        [](const RtPathTracePrimarySurfaceRecord& record)
        {
            return record.header[0] ==
                    RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION &&
                (record.header[1] &
                    RT_PRIMARY_SURFACE_VALID) != 0u;
        };
    auto maxFloatDelta =
        [](const float* a, const float* b, int count)
        {
            float delta = 0.0f;
            for (int component = 0;
                 component < count;
                 ++component)
            {
                delta = Max(
                    delta,
                    idMath::Fabs(
                        a[component] - b[component]));
            }
            return delta;
        };
    auto finiteFloats =
        [](const float* values, int count)
        {
            for (int component = 0;
                 component < count;
                 ++component)
            {
                if (!std::isfinite(values[component]))
                {
                    return false;
                }
            }
            return true;
        };

    const int samplePairWidth =
        m_skinnedHitAuditWidth >= 2
            ? (m_skinnedHitAuditWidth + 6) / 8
            : 0;
    const int sampleHeight =
        m_skinnedHitAuditHeight > 0
            ? (m_skinnedHitAuditHeight + 7) / 8
            : 0;
    for (int sampleY = 0;
         sampleY < sampleHeight;
         ++sampleY)
    {
        for (int sampleX = 0;
             sampleX < samplePairWidth;
             ++sampleX)
        {
            ++totalPairs;
            const int x = sampleX * 8;
            const int y = sampleY * 8;
            const size_t legacyIndex =
                (static_cast<size_t>(sampleY) *
                    static_cast<size_t>(samplePairWidth) +
                    static_cast<size_t>(sampleX)) *
                2u;
            const RtPathTracePrimarySurfaceRecord& legacy =
                records[legacyIndex];
            const RtPathTracePrimarySurfaceRecord& canonical =
                records[legacyIndex + 1];
            if (!recordValid(canonical))
            {
                continue;
            }

            const uint32_t canonicalInstance =
                canonical.instancePrimitiveObject[0];
            const uint32_t canonicalPrimitive =
                canonical.instancePrimitiveObject[1];
            const PtSkinnedHitRouteRecord* route = nullptr;
            for (const PtSkinnedHitRouteRecord& candidate :
                m_skinnedHitAuditLegacyShadow.records)
            {
                if (candidate.shaderInstanceId ==
                    canonicalInstance)
                {
                    route = &candidate;
                    break;
                }
            }
            if (!route)
            {
                continue;
            }
            ++canonicalSkinnedHits;

            if (canonicalPrimitive >= route->triangleCount ||
                static_cast<uint64_t>(
                    route->triangleMetadataOffset) +
                    canonicalPrimitive >=
                    m_skinnedHitAuditLegacyShadow.triangles.size())
            {
                ++primitiveRangeMissing;
                continue;
            }
            const PtSkinnedHitRouteTriangle& triangle =
                m_skinnedHitAuditLegacyShadow.triangles[
                    static_cast<size_t>(
                        route->triangleMetadataOffset) +
                    canonicalPrimitive];
            if (triangle.sourcePrimitiveIndex !=
                    canonicalPrimitive ||
                triangle.legacyPrimitiveIndex ==
                    PT_SKINNED_HIT_ROUTE_INVALID_INDEX)
            {
                ++primitiveRangeMissing;
                continue;
            }
            if (!recordValid(legacy))
            {
                ++legacyMisses;
                continue;
            }

            const float legacyHitT =
                legacy.worldPositionAndViewDepth[3];
            const float canonicalHitT =
                canonical.worldPositionAndViewDepth[3];
            if (legacyHitT + tupleTolerance <
                canonicalHitT)
            {
                ++legacyCloserOccluders;
                continue;
            }
            ++comparable;

            const bool primitiveMatches =
                legacy.instancePrimitiveObject[0] == 1u &&
                legacy.instancePrimitiveObject[1] ==
                    triangle.legacyPrimitiveIndex;
            if (!primitiveMatches)
            {
                ++primitiveMismatch;
                if (mismatchDetailsLogged < 8)
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO09 skinned hit primitive mismatch pixel=%d/%d canonical(instance/primitive)=%u/%u legacy(instance/primitive/expected)=%u/%u/%u hitT=%.9g/%.9g\n",
                        x,
                        y,
                        canonicalInstance,
                        canonicalPrimitive,
                        legacy.instancePrimitiveObject[0],
                        legacy.instancePrimitiveObject[1],
                        triangle.legacyPrimitiveIndex,
                        legacyHitT,
                        canonicalHitT);
                    ++mismatchDetailsLogged;
                }
                continue;
            }

            const bool finite =
                finiteFloats(
                    legacy.worldPositionAndViewDepth,
                    4) &&
                finiteFloats(
                    canonical.worldPositionAndViewDepth,
                    4) &&
                finiteFloats(
                    legacy.geometricNormalAndRoughness,
                    3) &&
                finiteFloats(
                    canonical.geometricNormalAndRoughness,
                    3) &&
                finiteFloats(
                    legacy.shadingNormalAndOpacity,
                    3) &&
                finiteFloats(
                    canonical.shadingNormalAndOpacity,
                    3) &&
                finiteFloats(
                    legacy.viewDirectionAndReserved,
                    3) &&
                finiteFloats(
                    canonical.viewDirectionAndReserved,
                    3) &&
                finiteFloats(
                    legacy.albedoAndAlphaCutoff,
                    3) &&
                finiteFloats(
                    canonical.albedoAndAlphaCutoff,
                    3) &&
                finiteFloats(
                    legacy.specularF0AndReserved,
                    4) &&
                finiteFloats(
                    canonical.specularF0AndReserved,
                    4) &&
                finiteFloats(
                    legacy.emissiveAndHeight,
                    2) &&
                finiteFloats(
                    canonical.emissiveAndHeight,
                    2);
            if (!finite)
            {
                ++nonFinite;
                continue;
            }

            const float hitTDelta =
                idMath::Fabs(
                    legacyHitT - canonicalHitT);
            const float positionDelta =
                maxFloatDelta(
                    legacy.worldPositionAndViewDepth,
                    canonical.worldPositionAndViewDepth,
                    3);
            const float geometricNormalDelta =
                maxFloatDelta(
                    legacy.geometricNormalAndRoughness,
                    canonical.geometricNormalAndRoughness,
                    3);
            const float shadingNormalDelta =
                maxFloatDelta(
                    legacy.shadingNormalAndOpacity,
                    canonical.shadingNormalAndOpacity,
                    3);
            const float tangentDelta =
                maxFloatDelta(
                    legacy.viewDirectionAndReserved,
                    canonical.viewDirectionAndReserved,
                    3);
            const float bitangentDelta =
                maxFloatDelta(
                    legacy.albedoAndAlphaCutoff,
                    canonical.albedoAndAlphaCutoff,
                    3);
            const float uvDelta =
                maxFloatDelta(
                    legacy.specularF0AndReserved,
                    canonical.specularF0AndReserved,
                    2);
            const float normalUvDelta =
                maxFloatDelta(
                    legacy.specularF0AndReserved + 2,
                    canonical.specularF0AndReserved + 2,
                    2);
            const float barycentricDelta =
                maxFloatDelta(
                    legacy.emissiveAndHeight,
                    canonical.emissiveAndHeight,
                    2);
            maxHitTDelta = Max(maxHitTDelta, hitTDelta);
            maxPositionDelta =
                Max(maxPositionDelta, positionDelta);
            maxGeometricNormalDelta =
                Max(
                    maxGeometricNormalDelta,
                    geometricNormalDelta);
            maxShadingNormalDelta =
                Max(
                    maxShadingNormalDelta,
                    shadingNormalDelta);
            maxTangentDelta =
                Max(maxTangentDelta, tangentDelta);
            maxBitangentDelta =
                Max(maxBitangentDelta, bitangentDelta);
            maxUvDelta = Max(maxUvDelta, uvDelta);
            maxNormalUvDelta =
                Max(maxNormalUvDelta, normalUvDelta);
            maxBarycentricDelta =
                Max(
                    maxBarycentricDelta,
                    barycentricDelta);

            const bool materialMatches =
                legacy.materialAndSurface[0] ==
                    canonical.materialAndSurface[0] &&
                legacy.materialAndSurface[1] ==
                    canonical.materialAndSurface[1] &&
                legacy.materialAndSurface[2] ==
                    canonical.materialAndSurface[2] &&
                legacy.materialAndSurface[3] ==
                    canonical.materialAndSurface[3];
            const bool triangleFlagsMatch =
                legacy.header[3] ==
                canonical.header[3];
            const uint32_t requiredMotionFlags =
                RT_PRIMARY_SURFACE_HAS_OBJECT_MOTION |
                RT_PRIMARY_SURFACE_HAS_PREVIOUS_POSITION;
            const bool legacyHasMotion =
                (legacy.header[1] & requiredMotionFlags) ==
                    requiredMotionFlags &&
                legacy.previousPositionOrMotion[3] >= 0.5f;
            const bool canonicalHasMotion =
                (canonical.header[1] & requiredMotionFlags) ==
                    requiredMotionFlags &&
                canonical.previousPositionOrMotion[3] >= 0.5f;
            legacyMotionValid += legacyHasMotion ? 1u : 0u;
            canonicalMotionValid += canonicalHasMotion ? 1u : 0u;
            motionValidityMismatch +=
                legacyHasMotion == canonicalHasMotion ? 0u : 1u;
            motionStatusMismatch +=
                legacy.instancePrimitiveObject[3] ==
                        canonical.instancePrimitiveObject[3]
                    ? 0u
                    : 1u;
            float previousPositionDelta = 0.0f;
            if (legacyHasMotion && canonicalHasMotion)
            {
                ++motionComparable;
                const bool finitePrevious =
                    finiteFloats(
                        legacy.previousPositionOrMotion,
                        3) &&
                    finiteFloats(
                        canonical.previousPositionOrMotion,
                        3);
                if (!finitePrevious)
                {
                    ++nonFinite;
                    previousPositionDelta =
                        std::numeric_limits<float>::infinity();
                }
                else
                {
                    previousPositionDelta =
                        maxFloatDelta(
                            legacy.previousPositionOrMotion,
                            canonical.previousPositionOrMotion,
                            3);
                    maxPreviousPositionDelta =
                        Max(
                            maxPreviousPositionDelta,
                            previousPositionDelta);
                    previousPositionMismatch +=
                        previousPositionDelta <= tupleTolerance
                            ? 0u
                            : 1u;
                }
            }
            else if (!legacyHasMotion && !canonicalHasMotion)
            {
                ++motionInvalidBoth;
            }
            distanceMismatch +=
                hitTDelta <= tupleTolerance ? 0u : 1u;
            positionMismatch +=
                positionDelta <= tupleTolerance ? 0u : 1u;
            geometricNormalMismatch +=
                geometricNormalDelta <= tupleTolerance
                    ? 0u
                    : 1u;
            shadingNormalMismatch +=
                shadingNormalDelta <= basisTolerance
                    ? 0u
                    : 1u;
            tangentMismatch +=
                tangentDelta <= basisTolerance ? 0u : 1u;
            bitangentMismatch +=
                bitangentDelta <= basisTolerance ? 0u : 1u;
            uvMismatch +=
                uvDelta <= tupleTolerance ? 0u : 1u;
            normalUvMismatch +=
                normalUvDelta <= tupleTolerance ? 0u : 1u;
            barycentricMismatch +=
                barycentricDelta <= tupleTolerance ? 0u : 1u;
            materialMismatch +=
                materialMatches ? 0u : 1u;
            triangleFlagsMismatch +=
                triangleFlagsMatch ? 0u : 1u;

            const bool tupleMatches =
                hitTDelta <= tupleTolerance &&
                positionDelta <= tupleTolerance &&
                geometricNormalDelta <= tupleTolerance &&
                shadingNormalDelta <= basisTolerance &&
                tangentDelta <= basisTolerance &&
                bitangentDelta <= basisTolerance &&
                uvDelta <= tupleTolerance &&
                normalUvDelta <= tupleTolerance &&
                barycentricDelta <= tupleTolerance &&
                materialMatches &&
                triangleFlagsMatch &&
                legacyHasMotion == canonicalHasMotion &&
                legacy.instancePrimitiveObject[3] ==
                    canonical.instancePrimitiveObject[3] &&
                previousPositionDelta <= tupleTolerance;
            if (!tupleMatches && mismatchDetailsLogged < 8)
            {
                common->Printf(
                    "PathTracePrimaryPass: GEO09 skinned hit mismatch pixel=%d/%d canonical(instance/primitive)=%u/%u legacy(instance/primitive/expected)=%u/%u/%u materialLegacy=%u/%u/0x%08x/%u materialCanonical=%u/%u/0x%08x/%u flags=0x%08x/0x%08x motion(valid/status)=%d/%u:%d/%u delta(t/position/geo/shading/tangent/bitangent/uv/normalUv/bary/previous)=%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g\n",
                    x,
                    y,
                    canonicalInstance,
                    canonicalPrimitive,
                    legacy.instancePrimitiveObject[0],
                    legacy.instancePrimitiveObject[1],
                    triangle.legacyPrimitiveIndex,
                    legacy.materialAndSurface[0],
                    legacy.materialAndSurface[1],
                    legacy.materialAndSurface[2],
                    legacy.materialAndSurface[3],
                    canonical.materialAndSurface[0],
                    canonical.materialAndSurface[1],
                    canonical.materialAndSurface[2],
                    canonical.materialAndSurface[3],
                    legacy.header[3],
                    canonical.header[3],
                    legacyHasMotion ? 1 : 0,
                    legacy.instancePrimitiveObject[3],
                    canonicalHasMotion ? 1 : 0,
                    canonical.instancePrimitiveObject[3],
                    hitTDelta,
                    positionDelta,
                    geometricNormalDelta,
                    shadingNormalDelta,
                    tangentDelta,
                    bitangentDelta,
                    uvDelta,
                    normalUvDelta,
                    barycentricDelta,
                    previousPositionDelta);
                ++mismatchDetailsLogged;
            }
        }
    }

    const uint64_t mismatchTotal =
        nonFinite +
        primitiveMismatch +
        distanceMismatch +
        positionMismatch +
        geometricNormalMismatch +
        shadingNormalMismatch +
        tangentMismatch +
        bitangentMismatch +
        uvMismatch +
        normalUvMismatch +
        barycentricMismatch +
        materialMismatch +
        triangleFlagsMismatch +
        motionValidityMismatch +
        motionStatusMismatch +
        previousPositionMismatch;
    const bool accepted =
        canonicalSkinnedHits > 0 &&
        comparable > 0 &&
        primitiveRangeMissing == 0 &&
        mismatchTotal == 0;
    const bool motionAccepted =
        motionComparable > 0 &&
        motionValidityMismatch == 0 &&
        motionStatusMismatch == 0 &&
        previousPositionMismatch == 0;
    common->Printf(
        "PathTracePrimaryPass: GEO09 skinned hit audit frame=%llu accepted=%d motionAccepted=%d dimensions=%d/%d pairs=%llu canonicalSkinned=%llu primitiveRangeMissing=%llu legacyMiss=%llu legacyCloserOccluder=%llu comparable/samePrimitive=%llu/%llu tolerance(tuple/basis)=%.9g/%.9g mismatches(nonFinite/primitive/hitT/position/geoNormal/shadingNormal/tangent/bitangent/uv/normalUv/bary/material/triangleFlags/motionValid/motionStatus/previousPosition)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu motion(legacyValid/canonicalValid/comparable/invalidBoth)=%llu/%llu/%llu/%llu maxima(hitT/position/geoNormal/shadingNormal/tangent/bitangent/uv/normalUv/bary/previousPosition)=%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g/%.9g\n",
        static_cast<unsigned long long>(m_skinnedHitAuditFrame),
        accepted ? 1 : 0,
        motionAccepted ? 1 : 0,
        m_skinnedHitAuditWidth,
        m_skinnedHitAuditHeight,
        static_cast<unsigned long long>(totalPairs),
        static_cast<unsigned long long>(
            canonicalSkinnedHits),
        static_cast<unsigned long long>(
            primitiveRangeMissing),
        static_cast<unsigned long long>(legacyMisses),
        static_cast<unsigned long long>(
            legacyCloserOccluders),
        static_cast<unsigned long long>(comparable),
        static_cast<unsigned long long>(
            comparable - primitiveMismatch),
        tupleTolerance,
        basisTolerance,
        static_cast<unsigned long long>(nonFinite),
        static_cast<unsigned long long>(primitiveMismatch),
        static_cast<unsigned long long>(distanceMismatch),
        static_cast<unsigned long long>(positionMismatch),
        static_cast<unsigned long long>(
            geometricNormalMismatch),
        static_cast<unsigned long long>(
            shadingNormalMismatch),
        static_cast<unsigned long long>(tangentMismatch),
        static_cast<unsigned long long>(bitangentMismatch),
        static_cast<unsigned long long>(uvMismatch),
        static_cast<unsigned long long>(normalUvMismatch),
        static_cast<unsigned long long>(
            barycentricMismatch),
        static_cast<unsigned long long>(materialMismatch),
        static_cast<unsigned long long>(
            triangleFlagsMismatch),
        static_cast<unsigned long long>(
            motionValidityMismatch),
        static_cast<unsigned long long>(
            motionStatusMismatch),
        static_cast<unsigned long long>(
            previousPositionMismatch),
        static_cast<unsigned long long>(legacyMotionValid),
        static_cast<unsigned long long>(
            canonicalMotionValid),
        static_cast<unsigned long long>(motionComparable),
        static_cast<unsigned long long>(motionInvalidBoth),
        maxHitTDelta,
        maxPositionDelta,
        maxGeometricNormalDelta,
        maxShadingNormalDelta,
        maxTangentDelta,
        maxBitangentDelta,
        maxUvDelta,
        maxNormalUvDelta,
        maxBarycentricDelta,
        maxPreviousPositionDelta);

    device->unmapBuffer(m_skinnedHitAuditReadbackBuffer);
    m_skinnedHitAuditReadbackQueued = false;
    m_skinnedHitAuditLegacyShadow =
        PtSkinnedHitRouteBuild();
}

void PathTracePrimaryPass::ReadBackRayTracingSmokeTest()
{
    ReadBackSkyCubeProbe();
    ReadBackLiquidPoolStatus();
    ReadBackStaticContractShaderSample();
    ReadBackStaticContractGeometrySample();
    ReadBackGpuSkinningParitySamples();
    ReadBackSkinnedEmissiveAudit();
    ReadBackSkinnedHitRoute();
    ReadBackSkinnedHitAuditSamples();

    const int debugMode = NormalizePathTraceDebugMode(idMath::ClampInt(0, 58, r_pathTracingDebugMode.GetInteger()));
    const bool overlapDumpRequested = debugMode == 24 && r_pathTracingRigidRouteOverlapDump.GetInteger() != 0;
    const bool cleanTemporalAuditRequested =
        r_pathTracingCleanRtxdiDiView.GetInteger() != 16 &&
        r_pathTracingCleanRtxdiDiTemporalAudit.GetInteger() != 0;
    if (r_pathTracingReadbackEnable.GetInteger() == 0 && !overlapDumpRequested && !cleanTemporalAuditRequested)
    {
        m_frameResources.readbackQueued = false;
        m_frameResources.readbackDelayFrames = 0;
        m_frameResources.readbackCooldownFrames = RT_SMOKE_READBACK_INTERVAL_FRAMES;
        return;
    }

    if (!m_frameResources.readbackQueued || !m_frameResources.readbackTexture)
    {
        if (m_frameResources.readbackCooldownFrames > 0)
        {
            --m_frameResources.readbackCooldownFrames;
        }
        return;
    }

    if (m_frameResources.readbackDelayFrames > 0)
    {
        --m_frameResources.readbackDelayFrames;
        return;
    }

    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!device)
    {
        return;
    }

    const int readbackStartMs = Sys_Milliseconds();
    device->waitForIdle();
    const int waitForIdleMs = Sys_Milliseconds() - readbackStartMs;

    size_t rowPitch = 0;
    void* readbackData = device->mapStagingTexture(m_frameResources.readbackTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
    if (!readbackData)
    {
        common->Printf("PathTracePrimaryPass: RT smoke UAV readback map failed\n");
        m_frameResources.readbackQueued = false;
        m_frameResources.readbackCooldownFrames = RT_SMOKE_READBACK_INTERVAL_FRAMES;
        return;
    }
    m_frameResources.RecordReadbackMapped();

    const int sampleX = m_frameResources.width / 2;
    const int sampleY = m_frameResources.height / 2;
    const byte* readbackBytes = static_cast<const byte*>(readbackData);
    const float* centerRgba = reinterpret_cast<const float*>(readbackBytes + rowPitch * sampleY + sizeof(float) * 4 * sampleX);
    const int liquidPoolMode = idMath::ClampInt(0, 3, r_pathTracingLiquidPoolMode.GetInteger());
    const int liquidPoolDebug = idMath::ClampInt(0, 6, r_pathTracingLiquidPoolDebug.GetInteger());
    const int liquidPoolPage = idMath::ClampInt(0, 3, r_pathTracingLiquidPoolDebugPage.GetInteger());
    if (liquidPoolMode != 0 && liquidPoolDebug != 0)
    {
        uint32_t centerWords[4] = {};
        memcpy(centerWords, centerRgba, sizeof(centerWords));
        common->Printf(
            "PathTracePrimaryPass: liquid-pool raw mode=%d debug=%d page=%d center=(%.9g %.9g %.9g %.9g) bits=(0x%08x 0x%08x 0x%08x 0x%08x)\n",
            liquidPoolMode,
            liquidPoolDebug,
            liquidPoolPage,
            centerRgba[0], centerRgba[1], centerRgba[2], centerRgba[3],
            centerWords[0], centerWords[1], centerWords[2], centerWords[3]);
    }

    int greenHits = 0;
    int redMisses = 0;
    int liquidProbeX = -1;
    int liquidProbeY = -1;
    int liquidProbeDistanceSquared = 0x7fffffff;
    uint32_t liquidProbeStatus = 0u;
    float liquidProbeRgba[4] = {};
    RigidRouteOverlapCounts fullFrameOverlap;
    RigidRouteOverlapCounts centerRegionOverlap;
    int centerRegionPixels = 0;
    const int centerRegionRadius = 16;
    const int centerRegionMinX = idMath::ClampInt(0, Max(0, m_frameResources.width - 1), sampleX - centerRegionRadius);
    const int centerRegionMaxX = idMath::ClampInt(0, Max(0, m_frameResources.width - 1), sampleX + centerRegionRadius);
    const int centerRegionMinY = idMath::ClampInt(0, Max(0, m_frameResources.height - 1), sampleY - centerRegionRadius);
    const int centerRegionMaxY = idMath::ClampInt(0, Max(0, m_frameResources.height - 1), sampleY + centerRegionRadius);
    PathTraceCleanRtxdiDiGuiSnapshot cleanTemporalAudit;
    double cleanTemporalAuditPreviousMSum = 0.0;
    double cleanTemporalAuditOutputMSum = 0.0;
    cleanTemporalAudit.temporalAuditValid = cleanTemporalAuditRequested;
    for (int y = 0; y < m_frameResources.height; ++y)
    {
        const float* row = reinterpret_cast<const float*>(readbackBytes + rowPitch * y);
        for (int x = 0; x < m_frameResources.width; ++x)
        {
            const float* rgba = row + x * 4;
            if (rgba[1] > 0.5f)
            {
                ++greenHits;
            }
            else if (rgba[0] > 0.5f)
            {
                ++redMisses;
            }

            if (liquidPoolMode != 0 && liquidPoolDebug != 0 && liquidPoolDebug != 5)
            {
                uint32_t statusMask = 0u;
                bool statusAvailable = false;
                if (liquidPoolDebug == 4 && liquidPoolPage <= 1)
                {
                    memcpy(&statusMask, &rgba[3], sizeof(statusMask));
                    statusAvailable = true;
                }
                else if (liquidPoolDebug == 1 || liquidPoolDebug == 2 ||
                    liquidPoolDebug == 3 || liquidPoolDebug == 4)
                {
                    statusMask = rgba[3] >= 0.0f && rgba[3] <= 255.0f
                        ? static_cast<uint32_t>(rgba[3] + 0.5f)
                        : 0u;
                    statusAvailable = true;
                }
                else if (liquidPoolDebug == 6)
                {
                    const float statusValue = x == sampleX && y == sampleY ? rgba[1] : rgba[3];
                    statusMask = statusValue >= 0.0f && statusValue <= 255.0f
                        ? static_cast<uint32_t>(statusValue + 0.5f)
                        : 0u;
                    statusAvailable = true;
                }

                if (statusAvailable &&
                    (statusMask & (LIQUID_POOL_STATUS_RECEIVER_VALID | LIQUID_POOL_STATUS_APPLIED)) != 0u)
                {
                    const int dx = x - sampleX;
                    const int dy = y - sampleY;
                    const int distanceSquared = dx * dx + dy * dy;
                    if (distanceSquared < liquidProbeDistanceSquared)
                    {
                        liquidProbeX = x;
                        liquidProbeY = y;
                        liquidProbeDistanceSquared = distanceSquared;
                        liquidProbeStatus = statusMask;
                        if (liquidPoolDebug == 6)
                        {
                            liquidProbeRgba[0] = 2.0f;
                            liquidProbeRgba[1] = static_cast<float>(statusMask);
                            liquidProbeRgba[2] =
                                (statusMask & LIQUID_POOL_STATUS_APPLIED) != 0u ? 1.0f : 0.0f;
                            liquidProbeRgba[3] = 0.0f;
                        }
                        else
                        {
                            memcpy(liquidProbeRgba, rgba, sizeof(liquidProbeRgba));
                        }
                    }
                }
            }

            if (cleanTemporalAuditRequested)
            {
                const uint32_t auditFlags = DecodeCleanTemporalAuditFlags(rgba);
                ++cleanTemporalAudit.temporalAuditPixels;
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditCurrentValid, auditFlags, CLEAN_TEMPORAL_DIAG_CURRENT_VALID);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditCurrentCandidate, auditFlags, CLEAN_TEMPORAL_DIAG_CURRENT_CANDIDATE);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditSurfaceValid, auditFlags, CLEAN_TEMPORAL_DIAG_CURRENT_SURFACE_VALID);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditMotionValid, auditFlags, CLEAN_TEMPORAL_DIAG_MOTION_VALID);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditCameraFallback, auditFlags, CLEAN_TEMPORAL_DIAG_CAMERA_REPROJECTED);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditPreviousPixelInBounds, auditFlags, CLEAN_TEMPORAL_DIAG_PREVIOUS_PIXEL_IN_BOUNDS);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditPreviousSurfaceValid, auditFlags, CLEAN_TEMPORAL_DIAG_PREVIOUS_SURFACE_VALID);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditPreviousReservoirValid, auditFlags, CLEAN_TEMPORAL_DIAG_PREVIOUS_RESERVOIR_VALID);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditPreviousLightMapped, auditFlags, CLEAN_TEMPORAL_DIAG_PREVIOUS_LIGHT_MAPPED);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditPreviousTargetAtCurrent, auditFlags, CLEAN_TEMPORAL_DIAG_PREVIOUS_TARGET_AT_CURRENT);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditSdkCalled, auditFlags, CLEAN_TEMPORAL_DIAG_SDK_CALLED);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditSdkTemporalSamplePixelValid, auditFlags, CLEAN_TEMPORAL_DIAG_TEMPORAL_SAMPLE_PIXEL_VALID);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditOutputReservoirValid, auditFlags, CLEAN_TEMPORAL_DIAG_TEMPORAL_RESERVOIR_VALID);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditSdkSelectedPrevious, auditFlags, CLEAN_TEMPORAL_DIAG_SDK_SELECTED_PREVIOUS_SAMPLE);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditSdkReusedPrevious, auditFlags, CLEAN_TEMPORAL_DIAG_SDK_REUSED_PREVIOUS);
                AccumulateCleanTemporalAuditFlag(cleanTemporalAudit.temporalAuditOutputChanged, auditFlags, CLEAN_TEMPORAL_DIAG_TEMPORAL_OUTPUT_CHANGED);
                cleanTemporalAuditPreviousMSum += static_cast<double>(idMath::ClampFloat(0.0f, 1.0f, rgba[1]) * 64.0f);
                cleanTemporalAuditOutputMSum += static_cast<double>(idMath::ClampFloat(0.0f, 1.0f, rgba[2]) * 64.0f);
            }

            if (debugMode == 24)
            {
                const RigidRouteOverlapBucket bucket = ClassifyRigidRouteOverlapColor(rgba);
                AccumulateRigidRouteOverlapBucket(fullFrameOverlap, bucket);
                if (x >= centerRegionMinX && x <= centerRegionMaxX && y >= centerRegionMinY && y <= centerRegionMaxY)
                {
                    AccumulateRigidRouteOverlapBucket(centerRegionOverlap, bucket);
                    ++centerRegionPixels;
                }
            }

        }
    }

    if (liquidPoolMode != 0 && liquidPoolDebug != 0)
    {
        if (liquidPoolDebug != 5)
        {
            if (liquidProbeX >= 0 && liquidProbeY >= 0)
            {
                g_liquidPoolProbeX = liquidProbeX;
                g_liquidPoolProbeY = liquidProbeY;
                g_liquidPoolProbeWidth = m_frameResources.width;
                g_liquidPoolProbeHeight = m_frameResources.height;
                g_liquidPoolProbeStatus = liquidProbeStatus;
            }
            else
            {
                g_liquidPoolProbeX = -1;
                g_liquidPoolProbeY = -1;
            }
        }
        else if (g_liquidPoolProbeX >= 0 && g_liquidPoolProbeY >= 0 &&
            g_liquidPoolProbeWidth == m_frameResources.width &&
            g_liquidPoolProbeHeight == m_frameResources.height)
        {
            liquidProbeX = g_liquidPoolProbeX;
            liquidProbeY = g_liquidPoolProbeY;
            liquidProbeStatus = g_liquidPoolProbeStatus;
            const float* probeRow = reinterpret_cast<const float*>(
                readbackBytes + rowPitch * liquidProbeY);
            memcpy(liquidProbeRgba, probeRow + liquidProbeX * 4, sizeof(liquidProbeRgba));
        }

        if (liquidProbeX >= 0 && liquidProbeY >= 0)
        {
            uint32_t probeWords[4] = {};
            memcpy(probeWords, liquidProbeRgba, sizeof(probeWords));
            common->Printf(
                "PathTracePrimaryPass: liquid-pool probe mode=%d debug=%d page=%d xy=(%d %d) value=(%.9g %.9g %.9g %.9g) bits=(0x%08x 0x%08x 0x%08x 0x%08x) status=0x%02x\n",
                liquidPoolMode,
                liquidPoolDebug,
                liquidPoolPage,
                liquidProbeX,
                liquidProbeY,
                liquidProbeRgba[0], liquidProbeRgba[1], liquidProbeRgba[2], liquidProbeRgba[3],
                probeWords[0], probeWords[1], probeWords[2], probeWords[3],
                liquidProbeStatus);
        }
        else
        {
            common->Printf(
                "PathTracePrimaryPass: liquid-pool probe mode=%d debug=%d page=%d none\n",
                liquidPoolMode,
                liquidPoolDebug,
                liquidPoolPage);
        }
    }

    const int readbackMs = Sys_Milliseconds() - readbackStartMs;
    if (r_pathTracingSmokeLog.GetInteger() != 0 || ShouldLogSmokeTiming(readbackMs, Sys_Milliseconds(), g_smokeLastReadbackTimingLogMs))
    {
        common->Printf("PathTracePrimaryPass: RT smoke UAV readback %dx%d center rgba=(%.3f, %.3f, %.3f, %.3f), hits=%d, misses=%d, rowPitch=%u, total=%d ms, waitForIdle=%d ms\n",
            m_frameResources.width, m_frameResources.height,
            centerRgba[0], centerRgba[1], centerRgba[2], centerRgba[3],
            greenHits, redMisses, static_cast<unsigned int>(rowPitch),
            readbackMs, waitForIdleMs);
    }
    if (cleanTemporalAuditRequested)
    {
        const double auditPixels = static_cast<double>(Max(1u, cleanTemporalAudit.temporalAuditPixels));
        cleanTemporalAudit.temporalAuditAvgPreviousM = static_cast<float>(cleanTemporalAuditPreviousMSum / auditPixels);
        cleanTemporalAudit.temporalAuditAvgOutputM = static_cast<float>(cleanTemporalAuditOutputMSum / auditPixels);
        PathTraceCleanRtxdiDiPublishTemporalAudit(cleanTemporalAudit);
    }
    if (debugMode == 24 && (overlapDumpRequested || r_pathTracingSmokeLog.GetInteger() != 0))
    {
        const int totalPixels = Max(1, m_frameResources.width * m_frameResources.height);
        common->Printf("PathTracePrimaryPass: PT rigid route overlap pixels total=%d match=%d(%.2f%%) materialMismatch=%d(%.2f%%) classMismatch=%d(%.2f%%) rigidOnly=%d(%.2f%%) rigidInFront=%d(%.2f%%) fallbackInFront=%d(%.2f%%) fallbackOnly=%d(%.2f%%) neither=%d(%.2f%%) unknown=%d(%.2f%%) colorCode green/yellow/magenta/cyan/blue/orange/gray/black tolerance=1.5pxRayT\n",
            totalPixels,
            fullFrameOverlap.match, 100.0f * static_cast<float>(fullFrameOverlap.match) / static_cast<float>(totalPixels),
            fullFrameOverlap.materialMismatch, 100.0f * static_cast<float>(fullFrameOverlap.materialMismatch) / static_cast<float>(totalPixels),
            fullFrameOverlap.classMismatch, 100.0f * static_cast<float>(fullFrameOverlap.classMismatch) / static_cast<float>(totalPixels),
            fullFrameOverlap.rigidOnly, 100.0f * static_cast<float>(fullFrameOverlap.rigidOnly) / static_cast<float>(totalPixels),
            fullFrameOverlap.rigidInFront, 100.0f * static_cast<float>(fullFrameOverlap.rigidInFront) / static_cast<float>(totalPixels),
            fullFrameOverlap.fallbackInFront, 100.0f * static_cast<float>(fullFrameOverlap.fallbackInFront) / static_cast<float>(totalPixels),
            fullFrameOverlap.fallbackOnly, 100.0f * static_cast<float>(fullFrameOverlap.fallbackOnly) / static_cast<float>(totalPixels),
            fullFrameOverlap.neither, 100.0f * static_cast<float>(fullFrameOverlap.neither) / static_cast<float>(totalPixels),
            fullFrameOverlap.unknown, 100.0f * static_cast<float>(fullFrameOverlap.unknown) / static_cast<float>(totalPixels));
        const int regionPixels = Max(1, centerRegionPixels);
        const RigidRouteOverlapBucket centerBucket = ClassifyRigidRouteOverlapColor(centerRgba);
        common->Printf("PathTracePrimaryPass: PT rigid route center bucket=%s rgba=(%.3f, %.3f, %.3f, %.3f) roi=%dx%d match=%d(%.2f%%) materialMismatch=%d(%.2f%%) classMismatch=%d(%.2f%%) rigidOnly=%d(%.2f%%) rigidInFront=%d(%.2f%%) fallbackInFront=%d(%.2f%%) fallbackOnly=%d(%.2f%%) neither=%d(%.2f%%) unknown=%d(%.2f%%)\n",
            RigidRouteOverlapBucketName(centerBucket),
            centerRgba[0], centerRgba[1], centerRgba[2], centerRgba[3],
            centerRegionMaxX - centerRegionMinX + 1,
            centerRegionMaxY - centerRegionMinY + 1,
            centerRegionOverlap.match, 100.0f * static_cast<float>(centerRegionOverlap.match) / static_cast<float>(regionPixels),
            centerRegionOverlap.materialMismatch, 100.0f * static_cast<float>(centerRegionOverlap.materialMismatch) / static_cast<float>(regionPixels),
            centerRegionOverlap.classMismatch, 100.0f * static_cast<float>(centerRegionOverlap.classMismatch) / static_cast<float>(regionPixels),
            centerRegionOverlap.rigidOnly, 100.0f * static_cast<float>(centerRegionOverlap.rigidOnly) / static_cast<float>(regionPixels),
            centerRegionOverlap.rigidInFront, 100.0f * static_cast<float>(centerRegionOverlap.rigidInFront) / static_cast<float>(regionPixels),
            centerRegionOverlap.fallbackInFront, 100.0f * static_cast<float>(centerRegionOverlap.fallbackInFront) / static_cast<float>(regionPixels),
            centerRegionOverlap.fallbackOnly, 100.0f * static_cast<float>(centerRegionOverlap.fallbackOnly) / static_cast<float>(regionPixels),
            centerRegionOverlap.neither, 100.0f * static_cast<float>(centerRegionOverlap.neither) / static_cast<float>(regionPixels),
            centerRegionOverlap.unknown, 100.0f * static_cast<float>(centerRegionOverlap.unknown) / static_cast<float>(regionPixels));
        r_pathTracingRigidRouteOverlapDump.SetInteger(0);
    }
    device->unmapStagingTexture(m_frameResources.readbackTexture);
    m_frameResources.RecordReadbackUnmapped();
    m_frameResources.readbackLogged = true;
    m_frameResources.readbackQueued = false;
    m_frameResources.readbackCooldownFrames = RT_SMOKE_READBACK_INTERVAL_FRAMES;
}
