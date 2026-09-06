#pragma once

// Small acceleration-structure helpers for the RT smoke scene.
//
// Prepares BLAS/TLAS geometry descriptors, computes the static BLAS cache
// signature, uploads build-input buffers, and submits NVRHI acceleration builds.
// Long-lived cache ownership remains with PathTracePrimaryPass state.

#include "PathTraceGeometry.h"
#include "PathTraceAccelerationPlan.h"

#include <nvrhi/nvrhi.h>

#include <vector>

struct RtSmokeBlasCreateDesc
{
    nvrhi::IDevice* device = nullptr;
    nvrhi::BufferHandle vertexBuffer;
    nvrhi::BufferHandle indexBuffer;
    int vertexCount = 0;
    int indexCount = 0;
    const uint32_t* triangleMaterialIds = nullptr;
    int triangleMaterialCount = 0;
    bool enableOpaqueGeometry = false;
    const char* debugName = nullptr;
    nvrhi::rt::AccelStructBuildFlags buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
};

struct RtSmokeBlasCreateResult
{
    nvrhi::rt::AccelStructDesc accelStructDesc;
    nvrhi::rt::AccelStructHandle accelStruct;
    RtSmokeBlasCreateStatus status = RtSmokeBlasCreateStatus::InvalidInput;
    const char* errorMessage = nullptr;
    int geometryCount = 0;
    int opaqueGeometryCount = 0;
    int nonOpaqueGeometryCount = 0;

    bool Succeeded() const
    {
        return status == RtSmokeBlasCreateStatus::Success &&
            accelStruct && errorMessage == nullptr;
    }
};

struct RtSmokeAccelSubmitDesc
{
    nvrhi::ICommandList* commandList = nullptr;
    nvrhi::rt::AccelStructHandle tlas;
    nvrhi::rt::AccelStructHandle staticBlas;
    nvrhi::rt::AccelStructHandle dynamicBlas;
    nvrhi::rt::AccelStructDesc staticBlasDesc;
    nvrhi::rt::AccelStructDesc dynamicBlasDesc;
    nvrhi::TimerQueryHandle dynamicBlasTimerQuery;
    const std::vector<nvrhi::rt::InstanceDesc>* extraTlasInstances = nullptr;
    uint32_t tlasMaxInstances = 0;
    bool hasStaticBlas = false;
    bool hasDynamicBlas = false;
    bool staticBlasCacheHit = false;
    bool dynamicBlasCacheHit = false;
    bool includeStaticBlasInTlas = true;
    bool diagnosticMarkers = false;
};

struct RtSmokeAccelSubmitTiming
{
    int blasSubmitMs = 0;
    int tlasSubmitMs = 0;
    int accelSubmitMs = 0;
    uint64_t blasSubmitMicroseconds = 0;
    uint64_t tlasSubmitMicroseconds = 0;
    uint64_t accelSubmitMicroseconds = 0;
    int instanceCount = 0;
    bool staticBlasBuildSubmitted = false;
    bool staticBlasBuildSkipped = false;
    bool dynamicBlasBuildSubmitted = false;
    bool dynamicBlasBuildSkipped = false;
    bool dynamicBlasTimerRecorded = false;
};

struct RtSmokeBufferUploadItem
{
    nvrhi::BufferHandle buffer;
    const void* data = nullptr;
    const char* profileName = nullptr;
    size_t byteSize = 0;
    nvrhi::ResourceStates finalState = nvrhi::ResourceStates::ShaderResource;
    bool skip = false;
    size_t sourceOffsetBytes = 0;
    uint64_t destOffsetBytes = 0;
};

struct RtSmokeBufferUploadBatchDesc
{
    nvrhi::ICommandList* commandList = nullptr;
    const RtSmokeBufferUploadItem* items = nullptr;
    int itemCount = 0;
};

void InitSmokeTriangleGeometry(nvrhi::rt::GeometryTriangles& triangleGeometry, nvrhi::IBuffer* vertexBuffer, nvrhi::IBuffer* indexBuffer, int totalVertexCount, int indexOffset, int indexCount);
bool ValidateSmokeTriangleGeometryBufferRanges(
    const nvrhi::rt::GeometryTriangles& triangleGeometry);
RtSmokeBlasCreateResult CreateSmokeBlas(const RtSmokeBlasCreateDesc& desc);
uint64 ComputeSmokeBlasOpacitySignature(
    const uint32_t* triangleMaterialIds,
    int triangleMaterialCount,
    bool enableOpaqueGeometry);
int UploadSmokeAccelerationBuffers(const RtSmokeBufferUploadBatchDesc& desc);
bool SubmitSmokeAccelerationBuilds(const RtSmokeAccelSubmitDesc& desc, RtSmokeAccelSubmitTiming& timing);
uint64 HashSmokeBytes(uint64 hash, const void* data, size_t size);
uint64 HashSmokeFloatQuantized(uint64 hash, float value, float scale);
