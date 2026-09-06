#pragma once

#include "PathTraceRigidIdentityKernel.h"
#include "PathTraceGeometryLifecycle.h"
#include "PathTraceInstanceUniverse.h"

class idRenderModel;
struct srfTriangles_t;

struct RtPathTraceRigidInstanceSnapshot
{
    RtPathTraceMeshKey meshKey;
    const idRenderModel* model = nullptr;
    PtRenderDefKey renderDefKey;
    uint32_t modelEpoch = 0;
    int entityIndex = -1;
    int renderEntityNum = -1;
    int modelSurfaceIndex = -1;
	bool modelSurfaceIndexValid = false;
    int jointIndex = -1;
    uint32_t materialId = 0;
    uint32_t materialClassSignature = 0;
    uint32_t sourceFlags = 0;
    uint64 meshHash = 0;
    uint64 instanceId = 0;
};

struct RtPathTraceRigidMeshIdentityPod
{
    uint64 modelIdentity = 0;
    uint32_t modelEpoch = 0;
    int modelSurfaceIndex = -1;
    int jointIndex = -1;
    uintptr_t vertexBufferIdentity = 0;
    uintptr_t indexBufferIdentity = 0;
    int numVerts = 0;
    int numIndexes = 0;
    uint32_t vertexFormat = 0;
    uint32_t materialId = 0;
    uint32_t materialClassSignature = 0;
    uint32_t sourceKind = 0;
};

struct RtPathTraceRigidInstanceIdentityPod
{
    uint64 meshHash = 0;
    uint64 renderWorldIdentity = 0;
    int renderDefIndex = -1;
    uint32_t renderDefGeneration = 0;
    uint32_t modelEpoch = 0;
    int entityIndex = -1;
    int renderEntityNum = -1;
    int modelSurfaceIndex = -1;
    uint32_t materialId = 0;
    int jointIndex = -1;
};

uint64 BuildPathTraceRigidMeshHashFromPod(
    const RtPathTraceRigidMeshIdentityPod& input);
uint64 BuildPathTraceRigidInstanceIdFromPod(
    const RtPathTraceRigidInstanceIdentityPod& input);

int ResolvePathTraceRigidModelSurfaceIndex(
    const idRenderModel* model,
    const srfTriangles_t* tri,
    int requestedModelSurfaceIndex);

void FillPathTraceRigidRouteMeshKey(
    RtPathTraceMeshKey& meshKey,
    const srfTriangles_t* tri,
    uint32_t materialId,
    uint32_t materialClassSignature,
    uint32_t sourceKind);

uint64 BuildPathTraceRigidMeshHash(
    const RtPathTraceMeshKey& key,
    const idRenderModel* model,
    uint32_t modelEpoch,
    int modelSurfaceIndex,
    int jointIndex = -1);

uint64 BuildPathTraceRigidInstanceId(
    uint64 meshHash,
    const PtRenderDefKey& renderDefKey,
    uint32_t modelEpoch,
    int entityIndex,
    int renderEntityNum,
    int modelSurfaceIndex,
    uint32_t materialId,
    int jointIndex = -1);

RtPathTraceRigidInstanceSnapshot BuildPathTraceRigidInstanceSnapshot(
    const RtPathTraceMeshKey& key,
    const idRenderModel* model,
    const srfTriangles_t* tri,
    const PtRenderDefKey& renderDefKey,
    uint32_t modelEpoch,
    int entityIndex,
    int renderEntityNum,
    int requestedModelSurfaceIndex,
    uint32_t sourceFlags,
    int jointIndex = -1);
