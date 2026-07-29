#pragma once

// Small acceleration-structure helpers for the RT smoke scene.
//
// Prepares BLAS/TLAS geometry descriptors, computes the static BLAS cache
// signature, uploads build-input buffers, and submits NVRHI acceleration builds.
// Long-lived cache ownership remains with PathTracePrimaryPass state.

#include "PathTraceGeometry.h"

#include <nvrhi/nvrhi.h>

#include <vector>

struct RtSmokeBlasCreateDesc
{
    nvrhi::IDevice* device = nullptr;
    nvrhi::BufferHandle vertexBuffer;
    nvrhi::BufferHandle indexBuffer;
    int vertexCount = 0;
    int indexCount = 0;
    const char* debugName = nullptr;
};

struct RtSmokeBlasCreateResult
{
    nvrhi::rt::AccelStructDesc accelStructDesc;
    nvrhi::rt::AccelStructHandle accelStruct;
    const char* errorMessage = nullptr;

    bool Succeeded() const { return accelStruct && errorMessage == nullptr; }
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
    bool hasStaticBlas = false;
    bool hasDynamicBlas = false;
    bool staticBlasCacheHit = false;
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
RtSmokeBlasCreateResult CreateSmokeBlas(const RtSmokeBlasCreateDesc& desc);
int UploadSmokeAccelerationBuffers(const RtSmokeBufferUploadBatchDesc& desc);
bool SubmitSmokeAccelerationBuilds(const RtSmokeAccelSubmitDesc& desc, RtSmokeAccelSubmitTiming& timing);
uint64 HashSmokeBytes(uint64 hash, const void* data, size_t size);
uint64 HashSmokeFloatQuantized(uint64 hash, float value, float scale);
