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

extern DeviceManager* deviceManager;

namespace {

const int RT_SMOKE_READBACK_INTERVAL_FRAMES = 120;
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

    common->Printf(
        "PathTracePrimaryPass: PT static contract shaderSample frame=%llu pixel=%d/%d dimensions=%d/%d version=%u validFlags=0x%08x status=%u instance=%u primitive=%u material(id/index)=%u/%u surfaceClass=%u reject=%u(%s) expected(instance/primitiveFirst/count/materialId/materialIndex)=%u/%u/%u/%u/%u match(valid/instance/primitive/materialId/materialIndex)=%d/%d/%d/%d/%d\n",
        static_cast<unsigned long long>(m_staticContractShaderSampleFrame),
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

    device->unmapBuffer(m_staticContractShaderReadbackBuffer);
    m_staticContractShaderReadbackQueued = false;
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

void PathTracePrimaryPass::ReadBackRayTracingSmokeTest()
{
    ReadBackSkyCubeProbe();
    ReadBackLiquidPoolStatus();
    ReadBackStaticContractShaderSample();
    ReadBackStaticContractGeometrySample();

    const int debugMode = NormalizePathTraceDebugMode(idMath::ClampInt(0, 57, r_pathTracingDebugMode.GetInteger()));
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
