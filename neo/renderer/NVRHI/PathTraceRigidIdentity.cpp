#include "precompiled.h"
#pragma hdrstop

#include "PathTraceRigidIdentity.h"
#include "PathTraceAcceleration.h"
#include "PathTraceCaptureProduct.h"
#include "PathTraceRigidMeshIdentity.h"

namespace {

uint64 HashRigidIdentityBytes(uint64 hash, const void* data, size_t size)
{
    return HashSmokeBytes(hash, data, size);
}

}

int ResolvePathTraceRigidModelSurfaceIndex(
    const idRenderModel* model,
    const srfTriangles_t* tri,
    int requestedModelSurfaceIndex)
{
    if (requestedModelSurfaceIndex >= 0 || !model || !tri)
    {
        return ResolvePathTraceModelSurfaceIndexFromPod(
            requestedModelSurfaceIndex,
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(tri)),
            nullptr, 0);
    }

    const int surfaceCount = model->NumSurfaces();
    if (surfaceCount <= 0)
    {
        return -1;
    }
    return ResolvePathTraceModelSurfaceIndexFromOrderedTokens(
        requestedModelSurfaceIndex,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(tri)),
        static_cast<std::size_t>(surfaceCount),
        [model](std::size_t surfaceIndex)
        {
            const modelSurface_t* surface = model->Surface(
                static_cast<int>(surfaceIndex));
            return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                surface ? surface->geometry : nullptr));
        });
}

void FillPathTraceRigidRouteMeshKey(
    RtPathTraceMeshKey& meshKey,
    const srfTriangles_t* tri,
    uint32_t materialId,
    uint32_t materialClassSignature,
    uint32_t sourceKind)
{
    meshKey.tri = tri;
    meshKey.vertexBufferIdentity = static_cast<uintptr_t>(
        cpu_producer_publish::RigidMeshVertexCacheIdentity(
            static_cast<uint64_t>(tri ? tri->ambientCache : 0)));
    meshKey.indexBufferIdentity = static_cast<uintptr_t>(
        cpu_producer_publish::RigidMeshIndexCacheIdentity(
            static_cast<uint64_t>(tri ? tri->indexCache : 0)));
    meshKey.numVerts = tri ? tri->numVerts : 0;
    meshKey.numIndexes = tri ? tri->numIndexes : 0;
    meshKey.vertexFormat = static_cast<uint32_t>(RtSmokeGeometryBufferFormat::LegacySmokeVertex);
    meshKey.materialId = materialId;
    meshKey.materialClassSignature = materialClassSignature;
    meshKey.sourceKind = sourceKind;
}

uint64 BuildPathTraceRigidMeshHash(
    const RtPathTraceMeshKey& key,
    const idRenderModel* model,
    uint32_t modelEpoch,
    int modelSurfaceIndex,
    int jointIndex)
{
    RtPathTraceRigidMeshIdentityPod pod;
    pod.modelIdentity = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(model));
    pod.modelEpoch = modelEpoch;
    pod.modelSurfaceIndex = modelSurfaceIndex;
    pod.jointIndex = jointIndex;
    pod.vertexBufferIdentity = key.vertexBufferIdentity;
    pod.indexBufferIdentity = key.indexBufferIdentity;
    pod.numVerts = key.numVerts;
    pod.numIndexes = key.numIndexes;
    pod.vertexFormat = key.vertexFormat;
    pod.materialId = key.materialId;
    pod.materialClassSignature = key.materialClassSignature;
    pod.sourceKind = key.sourceKind;
    return BuildPathTraceRigidMeshHashFromPod(pod);
}

uint64 BuildPathTraceRigidMeshHashFromPod(
    const RtPathTraceRigidMeshIdentityPod& input)
{
    const cpu_producer_publish::RigidMeshIdentityInputs in =
        cpu_producer_publish::MakeRigidMeshIdentity(
            input.modelIdentity,
            input.modelEpoch,
            input.modelSurfaceIndex,
            static_cast<uint64_t>(input.vertexBufferIdentity),
            static_cast<uint64_t>(input.indexBufferIdentity),
            input.numVerts,
            input.numIndexes,
            input.vertexFormat,
            input.materialId,
            input.materialClassSignature,
            input.sourceKind,
            input.jointIndex);
    return static_cast<uint64>(cpu_producer_publish::HashRigidMeshIdentity(in));
}

uint64 BuildPathTraceRigidInstanceId(
    uint64 meshHash,
    const PtRenderDefKey& renderDefKey,
    uint32_t modelEpoch,
    int entityIndex,
    int renderEntityNum,
    int modelSurfaceIndex,
    uint32_t materialId,
    int jointIndex)
{
    RtPathTraceRigidInstanceIdentityPod pod;
    pod.meshHash = meshHash;
    pod.renderWorldIdentity = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(renderDefKey.world));
    pod.renderDefIndex = renderDefKey.index;
    pod.renderDefGeneration = renderDefKey.generation;
    pod.modelEpoch = modelEpoch;
    pod.entityIndex = entityIndex;
    pod.renderEntityNum = renderEntityNum;
    pod.modelSurfaceIndex = modelSurfaceIndex;
    pod.materialId = materialId;
    pod.jointIndex = jointIndex;
    return BuildPathTraceRigidInstanceIdFromPod(pod);
}

uint64 BuildPathTraceRigidInstanceIdFromPod(
    const RtPathTraceRigidInstanceIdentityPod& input)
{
    uint64 hash = 14695981039346656037ull;
    hash = HashRigidIdentityBytes(hash, &input.renderWorldIdentity, sizeof(input.renderWorldIdentity));
    hash = HashRigidIdentityBytes(hash, &input.renderDefIndex, sizeof(input.renderDefIndex));
    hash = HashRigidIdentityBytes(hash, &input.renderDefGeneration, sizeof(input.renderDefGeneration));
    hash = HashRigidIdentityBytes(hash, &input.modelEpoch, sizeof(input.modelEpoch));
    hash = HashRigidIdentityBytes(hash, &input.entityIndex, sizeof(input.entityIndex));
    hash = HashRigidIdentityBytes(hash, &input.renderEntityNum, sizeof(input.renderEntityNum));
    hash = HashRigidIdentityBytes(hash, &input.modelSurfaceIndex, sizeof(input.modelSurfaceIndex));
    hash = HashRigidIdentityBytes(hash, &input.materialId, sizeof(input.materialId));
    if (input.jointIndex >= 0)
    {
        hash = HashRigidIdentityBytes(hash, &input.jointIndex, sizeof(input.jointIndex));
    }
    hash = HashRigidIdentityBytes(hash, &input.meshHash, sizeof(input.meshHash));
    return hash;
}

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
    int jointIndex)
{
    RtPathTraceRigidInstanceSnapshot snapshot;
    snapshot.meshKey = key;
    snapshot.model = model;
    snapshot.renderDefKey = renderDefKey;
    snapshot.modelEpoch = modelEpoch;
    snapshot.entityIndex = entityIndex;
    snapshot.renderEntityNum = renderEntityNum;
	snapshot.jointIndex = jointIndex;
	bool comparedCall = false;
	bool snapshotSurfaceMatches = false;
	const bool useResolvedSurface = requestedModelSurfaceIndex >= 0 ||
		RtPathTraceSourceFlagsAreDurableRigid(sourceFlags);
	if (useResolvedSurface)
	{
		const bool usedSnapshot =
			ResolvePathTraceCaptureSerialRigidIdentitySurfaceFromSnapshot(
				static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(model)),
				modelEpoch, requestedModelSurfaceIndex,
				static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(tri)),
				snapshot.modelSurfaceIndex, snapshotSurfaceMatches, comparedCall);
		if (!usedSnapshot && !comparedCall)
		{
			snapshot.modelSurfaceIndex = ResolvePathTraceRigidModelSurfaceIndex(
				model, tri, requestedModelSurfaceIndex);
		}
	}
	if (comparedCall)
	{
		snapshot.modelSurfaceIndexValid = snapshotSurfaceMatches;
	}
	else if (model != nullptr && tri != nullptr &&
		snapshot.modelSurfaceIndex >= 0 &&
		snapshot.modelSurfaceIndex < model->NumSurfaces())
	{
		const modelSurface_t* resolvedSurface =
			model->Surface(snapshot.modelSurfaceIndex);
		snapshot.modelSurfaceIndexValid =
			resolvedSurface != nullptr && resolvedSurface->geometry == tri;
	}
    snapshot.materialId = key.materialId;
    snapshot.materialClassSignature = key.materialClassSignature;
    snapshot.sourceFlags = sourceFlags;
    snapshot.meshHash = BuildPathTraceRigidMeshHash(snapshot.meshKey, model, modelEpoch, snapshot.modelSurfaceIndex, snapshot.jointIndex);
    snapshot.instanceId = BuildPathTraceRigidInstanceId(
        snapshot.meshHash,
        renderDefKey,
        modelEpoch,
        entityIndex,
        renderEntityNum,
        snapshot.modelSurfaceIndex,
        snapshot.materialId,
        snapshot.jointIndex);
    return snapshot;
}
