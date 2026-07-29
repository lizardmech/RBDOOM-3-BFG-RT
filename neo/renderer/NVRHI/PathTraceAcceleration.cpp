#include "precompiled.h"
#pragma hdrstop

#include "PathTraceAcceleration.h"
#include "PathTraceAccelerationPlan.h"

#include <algorithm>
#include <chrono>
#include <nvrhi/utils.h>

namespace
{
    void MarkSmokeTlasBlasesForRayTracing(nvrhi::ICommandList* commandList, const std::vector<nvrhi::rt::InstanceDesc>& instanceDescs)
    {
        if (!commandList || instanceDescs.empty())
        {
            return;
        }

        std::vector<nvrhi::rt::IAccelStruct*> markedBlases;
        markedBlases.reserve(instanceDescs.size());
        for (const nvrhi::rt::InstanceDesc& instanceDesc : instanceDescs)
        {
            nvrhi::rt::IAccelStruct* blas = instanceDesc.bottomLevelAS;
            if (!blas || std::find(markedBlases.begin(), markedBlases.end(), blas) != markedBlases.end())
            {
                continue;
            }

            commandList->setAccelStructState(blas, nvrhi::ResourceStates::AccelStructRead);
            markedBlases.push_back(blas);
        }

        if (!markedBlases.empty())
        {
            commandList->commitBarriers();
        }
    }
}

void InitSmokeTriangleGeometry(nvrhi::rt::GeometryTriangles& triangleGeometry, nvrhi::IBuffer* vertexBuffer, nvrhi::IBuffer* indexBuffer, int totalVertexCount, int indexOffset, int indexCount)
{
    triangleGeometry.indexBuffer = indexBuffer;
    triangleGeometry.vertexBuffer = vertexBuffer;
    triangleGeometry.indexFormat = nvrhi::Format::R32_UINT;
    triangleGeometry.vertexFormat = nvrhi::Format::RGB32_FLOAT;
    triangleGeometry.indexOffset = static_cast<uint64_t>(indexOffset) * sizeof(uint32_t);
    triangleGeometry.vertexOffset = 0;
    triangleGeometry.indexCount = static_cast<uint32_t>(indexCount);
    triangleGeometry.vertexCount = static_cast<uint32_t>(totalVertexCount);
    triangleGeometry.vertexStride = sizeof(PathTraceSmokeVertex);
}

RtSmokeBlasCreateResult CreateSmokeBlas(const RtSmokeBlasCreateDesc& desc)
{
    RtSmokeBlasCreateResult result;
    if (!desc.device || !desc.vertexBuffer || !desc.indexBuffer || desc.vertexCount <= 0 || desc.indexCount <= 0)
    {
        result.errorMessage = "invalid RT smoke BLAS create inputs";
        return result;
    }

    nvrhi::rt::GeometryTriangles triangleGeometry;
    InitSmokeTriangleGeometry(triangleGeometry, desc.vertexBuffer, desc.indexBuffer, desc.vertexCount, 0, desc.indexCount);

    nvrhi::rt::GeometryDesc geometryDesc;
    geometryDesc.setTriangles(triangleGeometry);

    result.accelStructDesc = nvrhi::rt::AccelStructDesc()
        .addBottomLevelGeometry(geometryDesc)
        .setBuildFlags(nvrhi::rt::AccelStructBuildFlags::PreferFastTrace)
        .setDebugName(desc.debugName);
    result.accelStruct = desc.device->createAccelStruct(result.accelStructDesc);
    if (!result.accelStruct)
    {
        result.errorMessage = "failed to create RT smoke BLAS";
    }
    return result;
}

int UploadSmokeAccelerationBuffers(const RtSmokeBufferUploadBatchDesc& desc)
{
    OPTICK_EVENT("PT Upload Scene Buffers Detail");

    if (!desc.commandList || !desc.items || desc.itemCount <= 0)
    {
        return 0;
    }

    {
        OPTICK_GPU_EVENT("PT GPU Begin Upload Tracking");
        for (int itemIndex = 0; itemIndex < desc.itemCount; ++itemIndex)
        {
            const RtSmokeBufferUploadItem& item = desc.items[itemIndex];
            if (!item.skip && item.buffer)
            {
                desc.commandList->beginTrackingBufferState(item.buffer, nvrhi::ResourceStates::Common);
            }
        }
    }

    const int bufferUploadStartMs = Sys_Milliseconds();
    {
        OPTICK_GPU_EVENT("PT GPU Write Scene Buffers");
        for (int itemIndex = 0; itemIndex < desc.itemCount; ++itemIndex)
        {
            const RtSmokeBufferUploadItem& item = desc.items[itemIndex];
            if (!item.skip && item.buffer && item.data && item.byteSize > 0)
            {
                const byte* sourceBytes = static_cast<const byte*>(item.data) + item.sourceOffsetBytes;
                desc.commandList->writeBuffer(item.buffer, sourceBytes, item.byteSize, item.destOffsetBytes);
            }
        }
    }

    {
        OPTICK_GPU_EVENT("PT GPU Upload Buffer Barriers");
        for (int itemIndex = 0; itemIndex < desc.itemCount; ++itemIndex)
        {
            const RtSmokeBufferUploadItem& item = desc.items[itemIndex];
            if (!item.skip && item.buffer)
            {
                desc.commandList->setBufferState(item.buffer, item.finalState);
            }
        }
        desc.commandList->commitBarriers();
    }

    return Sys_Milliseconds() - bufferUploadStartMs;
}

bool SubmitSmokeAccelerationBuilds(const RtSmokeAccelSubmitDesc& desc, RtSmokeAccelSubmitTiming& timing)
{
    OPTICK_EVENT("PT Submit Acceleration Builds Detail");

    timing = RtSmokeAccelSubmitTiming();
    const bool hasExtraTlasInstances =
        desc.extraTlasInstances &&
        !desc.extraTlasInstances->empty();
    if (!desc.commandList ||
        !desc.tlas ||
        (!desc.hasStaticBlas &&
            !desc.hasDynamicBlas &&
            !hasExtraTlasInstances))
    {
        return false;
    }

    RtSmokeAccelerationSubmitPlanInput submitPlanInput;
    submitPlanInput.hasStaticBlas = desc.hasStaticBlas;
    submitPlanInput.hasDynamicBlas = desc.hasDynamicBlas;
    submitPlanInput.staticBlasCacheHit = desc.staticBlasCacheHit;
    submitPlanInput.includeStaticBlasInTlas =
        desc.includeStaticBlasInTlas;
    submitPlanInput.hasExtraTlasInstances =
        hasExtraTlasInstances;
    const RtSmokeAccelerationSubmitPlan submitPlan =
        BuildSmokeAccelerationSubmitPlan(submitPlanInput);
    if (!submitPlan.submitTlas)
    {
        return false;
    }

    const int accelSubmitStartMs = Sys_Milliseconds();
    const int blasSubmitStartMs = Sys_Milliseconds();
    const auto accelSubmitStart =
        std::chrono::steady_clock::now();
    const auto blasSubmitStart = accelSubmitStart;
    timing.staticBlasBuildSkipped = submitPlanInput.hasStaticBlas && !submitPlan.buildStaticBlas;
    timing.dynamicBlasBuildSkipped = submitPlanInput.hasDynamicBlas && !submitPlan.buildDynamicBlas;
    if (submitPlan.buildStaticBlas)
    {
        OPTICK_GPU_EVENT("PT GPU Build Static BLAS");
        if (desc.diagnosticMarkers)
        {
            desc.commandList->beginMarker("GEO10.Accel StaticBLAS Build");
        }
        nvrhi::utils::BuildBottomLevelAccelStruct(desc.commandList, desc.staticBlas, desc.staticBlasDesc);
        if (desc.diagnosticMarkers)
        {
            desc.commandList->endMarker();
        }
        timing.staticBlasBuildSubmitted = true;
        timing.staticBlasBuildSkipped = false;
    }

    if (submitPlan.buildDynamicBlas)
    {
        OPTICK_GPU_EVENT("PT GPU Build Dynamic BLAS");
        if (desc.diagnosticMarkers)
        {
            desc.commandList->beginMarker("GEO10.Accel DynamicBLAS Build");
        }
        if (desc.dynamicBlasTimerQuery)
        {
            desc.commandList->beginTimerQuery(
                desc.dynamicBlasTimerQuery);
        }
        nvrhi::utils::BuildBottomLevelAccelStruct(desc.commandList, desc.dynamicBlas, desc.dynamicBlasDesc);
        if (desc.dynamicBlasTimerQuery)
        {
            desc.commandList->endTimerQuery(
                desc.dynamicBlasTimerQuery);
            timing.dynamicBlasTimerRecorded = true;
        }
        if (desc.diagnosticMarkers)
        {
            desc.commandList->endMarker();
        }
        timing.dynamicBlasBuildSubmitted = true;
        timing.dynamicBlasBuildSkipped = false;
    }
    timing.blasSubmitMs = Sys_Milliseconds() - blasSubmitStartMs;
    const auto blasSubmitEnd =
        std::chrono::steady_clock::now();
    timing.blasSubmitMicroseconds =
        static_cast<uint64_t>(
            std::chrono::duration_cast<
                std::chrono::microseconds>(
                blasSubmitEnd - blasSubmitStart).count());

    std::vector<nvrhi::rt::InstanceDesc> instanceDescs;
    instanceDescs.reserve(2 + (desc.extraTlasInstances ? desc.extraTlasInstances->size() : 0));
    for (int plannedIndex = 0; plannedIndex < submitPlan.baseTlasPlan.instanceCount; ++plannedIndex)
    {
        const RtSmokePlanTlasInstance& plannedInstance = submitPlan.baseTlasPlan.instances[plannedIndex];
        nvrhi::rt::InstanceDesc instanceDesc;
        instanceDesc
            .setInstanceID(plannedInstance.instanceId)
            .setInstanceMask(plannedInstance.instanceMask)
            .setInstanceContributionToHitGroupIndex(plannedInstance.hitGroupContribution)
            .setFlags(nvrhi::rt::InstanceFlags::TriangleCullDisable)
            .setBLAS(plannedInstance.kind == RT_SMOKE_PLAN_TLAS_STATIC_BLAS ? desc.staticBlas : desc.dynamicBlas);
        instanceDescs.push_back(instanceDesc);
    }
    if (desc.extraTlasInstances)
    {
        for (const nvrhi::rt::InstanceDesc& instanceDesc : *desc.extraTlasInstances)
        {
            if (instanceDesc.bottomLevelAS)
            {
                instanceDescs.push_back(instanceDesc);
            }
        }
    }

    const int tlasSubmitStartMs = Sys_Milliseconds();
    const auto tlasSubmitStart =
        std::chrono::steady_clock::now();
    {
        OPTICK_GPU_EVENT("PT GPU Build TLAS");
        if (desc.diagnosticMarkers)
        {
            desc.commandList->beginMarker(
                desc.includeStaticBlasInTlas
                    ? "GEO10.Accel TLAS Build monolithic-static"
                    : "GEO10.Accel TLAS Build bucket-static");
        }
        desc.commandList->buildTopLevelAccelStruct(desc.tlas, instanceDescs.data(), instanceDescs.size(), nvrhi::rt::AccelStructBuildFlags::PreferFastTrace);
        if (desc.diagnosticMarkers)
        {
            desc.commandList->endMarker();
        }
    }
    {
        OPTICK_GPU_EVENT("PT GPU Mark TLAS BLAS For Ray Tracing");
        MarkSmokeTlasBlasesForRayTracing(desc.commandList, instanceDescs);
    }
    timing.tlasSubmitMs = Sys_Milliseconds() - tlasSubmitStartMs;
    timing.accelSubmitMs = Sys_Milliseconds() - accelSubmitStartMs;
    const auto accelSubmitEnd =
        std::chrono::steady_clock::now();
    timing.tlasSubmitMicroseconds =
        static_cast<uint64_t>(
            std::chrono::duration_cast<
                std::chrono::microseconds>(
                accelSubmitEnd - tlasSubmitStart).count());
    timing.accelSubmitMicroseconds =
        static_cast<uint64_t>(
            std::chrono::duration_cast<
                std::chrono::microseconds>(
                accelSubmitEnd - accelSubmitStart).count());
    timing.instanceCount = static_cast<int>(instanceDescs.size());
    return true;
}

uint64 HashSmokeBytes(uint64 hash, const void* data, size_t size)
{
    const byte* bytes = static_cast<const byte*>(data);
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= static_cast<uint64>(bytes[index]);
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64 HashSmokeFloatQuantized(uint64 hash, float value, float scale)
{
    const int quantized = idMath::Ftoi(value * scale);
    return HashSmokeBytes(hash, &quantized, sizeof(quantized));
}
