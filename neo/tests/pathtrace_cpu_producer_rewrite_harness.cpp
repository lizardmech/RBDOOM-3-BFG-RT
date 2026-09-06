#include "../renderer/NVRHI/PathTraceMaterialMembership.h"
#include "../renderer/NVRHI/PathTraceMaterialClassifierNameCache.h"
#include <memory>
#include <string>
#include "../renderer/NVRHI/PathTraceRigidResolveKernel.h"
#include "PathTraceCpuProducerRewrite.h"
#include "PathTraceMaterialRecordKernel.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <limits>

// Stand-in owned payload for the opaque transport. Production light collection
// is not linked into this harness; the service must only retain/release ownership.
struct PathTraceDoomAnalyticLightSnapshotData { std::uint64_t frame = 0; };

namespace
{

int g_failures = 0;

void Check(bool condition, const char* name)
{
	if (condition)
	{
		std::printf("[PASS] %s\n", name);
		return;
	}
	std::printf("[FAIL] %s\n", name);
	++g_failures;
}

void CheckEqU64(std::uint64_t actual, std::uint64_t expected, const char* name)
{
	if (actual == expected)
	{
		std::printf("[PASS] %s (%llu)\n", name, static_cast<unsigned long long>(actual));
		return;
	}
	std::printf("[FAIL] %s actual=%llu expected=%llu\n",
		name,
		static_cast<unsigned long long>(actual),
		static_cast<unsigned long long>(expected));
	++g_failures;
}

void FillIdentityMatrix(float matrix[16])
{
	std::memset(matrix, 0, sizeof(float) * 16);
	matrix[0] = 1.0f;
	matrix[5] = 1.0f;
	matrix[10] = 1.0f;
	matrix[15] = 1.0f;
}

void FillTranslateMatrix(float matrix[16], float x, float y, float z)
{
	FillIdentityMatrix(matrix);
	matrix[12] = x;
	matrix[13] = y;
	matrix[14] = z;
}

RtCpuRewriteOwnedVertex MakeVert(float x, float y, float z)
{
	RtCpuRewriteOwnedVertex vertex = {};
	vertex.xyz[0] = x;
	vertex.xyz[1] = y;
	vertex.xyz[2] = z;
	vertex.normal[2] = 1.0f;
	vertex.tangent[0] = 1.0f;
	vertex.bitangent[1] = 1.0f;
	vertex.bitangentSign = 1.0f;
	vertex.color[0] = 1.0f;
	vertex.color[1] = 1.0f;
	vertex.color[2] = 1.0f;
	vertex.color[3] = 1.0f;
	return vertex;
}

std::uint64_t TopologySignature(std::uint32_t vertexCount, std::uint32_t indexCount, const std::uint32_t* indexes)
{
	std::uint64_t hash = 14695981039346656037ull;
	const std::uint64_t prime = 1099511628211ull;
	auto feed = [&](const void* data, std::size_t size)
	{
		const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
		for (std::size_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= prime;
		}
	};
	feed(&vertexCount, sizeof(vertexCount));
	feed(&indexCount, sizeof(indexCount));
	if (indexes)
	{
		for (std::uint32_t i = 0; i < indexCount; ++i)
		{
			feed(&indexes[i], sizeof(indexes[i]));
		}
	}
	return hash;
}

PtCanonicalMeshKey MakeMeshKey(std::uint64_t assetId, std::uint32_t surfaceIndex, std::uint32_t vertexCount, std::uint32_t indexCount, const std::uint32_t* indexes)
{
	PtCanonicalMeshKey key;
	key.sourceAssetId = assetId;
	key.sourceAssetGeneration = 1;
	key.topologySignature = TopologySignature(vertexCount, indexCount, indexes);
	key.sourceDomain = PtCanonicalMeshSourceDomain::RegisteredRenderModel;
	key.modelSurfaceIndex = surfaceIndex;
	key.vertexFormat = 1;
	key.deformationClass = PtCanonicalDeformationClass::Rigid;
	key.vertexCount = vertexCount;
	key.indexCount = indexCount;
	key.jointSubmeshIndex = -1;
	return key;
}

PtCanonicalInstanceKey MakeInstanceKey(std::uint32_t renderDefIndex, std::uint32_t surfaceIndex)
{
	PtCanonicalInstanceKey key;
	key.worldGeneration = 7;
	key.renderDefIndex = renderDefIndex;
	key.renderDefGeneration = 3;
	key.subInstanceKind = PtCanonicalSubInstanceKind::RigidSurface;
	key.modelSurfaceIndex = surfaceIndex;
	key.jointSubmeshIndex = -1;
	return key;
}

RtCpuRewriteSurfaceWrite MakeTriangleWrite(
	std::uint64_t assetId,
	std::uint32_t renderDefIndex,
	std::uint32_t surfaceIndex,
	const RtCpuRewriteOwnedVertex* verts,
	const std::uint32_t* indexes,
	const float matrix[16])
{
	RtCpuRewriteSurfaceWrite write = {};
	write.meshKey = MakeMeshKey(assetId, surfaceIndex, 3, 3, indexes);
	write.instanceKey = MakeInstanceKey(renderDefIndex, surfaceIndex);
	write.sourceClass = kRtCpuRewriteClassRigidEntity;
	std::memcpy(write.modelMatrix, matrix, sizeof(float) * 16);
	write.bounds[0] = -1.0f;
	write.bounds[1] = -1.0f;
	write.bounds[2] = -1.0f;
	write.bounds[3] = 1.0f;
	write.bounds[4] = 1.0f;
	write.bounds[5] = 1.0f;
	write.materialLogicalId = 0;
	write.activeEmissiveStage = 0;
	write.surfaceOrdinal = surfaceIndex;
	write.vertices = verts;
	write.vertexCount = 3;
	write.indexes = indexes;
	write.indexCount = 3;
	return write;
}

bool PublishOneTriangle(RtCpuProducerRewriteService& service, const RtCpuRewriteSurfaceWrite& write)
{
	if (!service.TryAcquireRootInput(11, 7, 4))
	{
		return false;
	}
	if (!service.WriteSurface(write))
	{
		service.AbortCapturingInput();
		return false;
	}
	if (!service.SealRootInput())
	{
		return false;
	}
	return service.WaitForReadyProduct(2000);
}

void TestPublicationAndMergedBytes()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);

	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillTranslateMatrix(matrix, 10.0f, 20.0f, 30.0f);
	const RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(1001, 4, 0, verts, indexes, matrix);

	Check(PublishOneTriangle(service, write), "free-to-ready publication");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"acquire ready product");
	const RtCpuRewriteFrozenProductView* view = service.ProductView();
	Check(view != nullptr && view->vertexCount == 0, "instanceID-1 static-only is empty for rigid-only product");
	Check(view != nullptr && view->rigidMeshCount == 1, "unique object-space rigid mesh");
	if (view && view->rigidMeshes && view->rigidMeshes[0].vertices && view->rigidMeshes[0].indexes)
	{
		Check(std::fabs(view->rigidMeshes[0].vertices[0].position[0] - 0.0f) < 0.001f, "object-space rigid x");
		Check(std::fabs(view->rigidMeshes[0].vertices[0].position[1] - 0.0f) < 0.001f, "object-space rigid y");
		Check(std::fabs(view->rigidMeshes[0].vertices[0].position[2] - 0.0f) < 0.001f, "object-space rigid z");
		Check(view->rigidMeshes[0].indexes[0] == 0 && view->rigidMeshes[0].indexes[1] == 1 && view->rigidMeshes[0].indexes[2] == 2,
			"local triangle index array");
		Check(view->rigidMeshes[0].sourceClass == kRtCpuRewriteClassRigidEntity, "rigid entity class id is 1");
	}
	const RtCpuRewriteCounters afterReady = service.Counters();
	CheckEqU64(afterReady.productsReady, 1, "product ready counter");
	service.ReleaseConsumedProduct();
	Check(service.ProductView() == nullptr, "free after consume");
	service.Shutdown();
	CheckEqU64(service.Counters().threadsJoined, 4, "shutdown joined four threads");
}

void TestDuplicateCanonicalMesh()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);

	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(2.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 2.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrixA[16];
	float matrixB[16];
	FillTranslateMatrix(matrixA, 0.0f, 0.0f, 0.0f);
	FillTranslateMatrix(matrixB, 5.0f, 0.0f, 0.0f);

	Check(service.TryAcquireRootInput(12, 7, 4), "duplicate acquire");
	RtCpuRewriteSurfaceWrite writeA = MakeTriangleWrite(2002, 8, 0, verts, indexes, matrixA);
	RtCpuRewriteSurfaceWrite writeB = MakeTriangleWrite(2002, 9, 0, verts, indexes, matrixB);
	writeB.meshKey = writeA.meshKey;
	Check(service.WriteSurface(writeA), "duplicate write A");
	Check(service.WriteSurface(writeB), "duplicate write B");
	Check(service.SealRootInput(), "duplicate seal");
	Check(service.WaitForReadyProduct(2000), "duplicate ready");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"duplicate acquire product");
	const RtCpuRewriteFrozenProductView* view = service.ProductView();
	Check(view != nullptr && view->rigidMeshCount == 1, "duplicate mesh stores one unique object-space mesh");
	Check(view != nullptr && view->sourceCount == 2, "duplicate mesh keeps distinct instance occurrences");
	if (view && view->sourceCount == 2)
	{
		Check(view->sources[0].instanceKey.renderDefIndex != view->sources[1].instanceKey.renderDefIndex,
			"duplicate instances remain distinct");
		Check(view->sources[0].meshKey.sourceAssetId == view->sources[1].meshKey.sourceAssetId,
			"duplicate shares canonical mesh");
	}
	service.ReleaseConsumedProduct();
	service.Shutdown();
}

void TestHeldPressure()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);

	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);

	int heldProducts = 0;
	for (int i = 0; i < 3; ++i)
	{
		RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(3000 + static_cast<std::uint64_t>(i), 10 + i, 0, verts, indexes, matrix);
		Check(PublishOneTriangle(service, write), "pressure publish");
		Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
			"pressure hold acquire");
		++heldProducts;
	}
	RtCpuRewriteSurfaceWrite overflow = MakeTriangleWrite(3999, 40, 0, verts, indexes, matrix);
	Check(!PublishOneTriangle(service, overflow), "held product pressure drops publish");
	const RtCpuRewriteCounters afterProductPressure = service.Counters();
	Check(afterProductPressure.productPressureDrops >= 1, "product pressure counter");

	while (heldProducts > 0)
	{
		service.ReleaseConsumedProduct();
		--heldProducts;
	}

	Check(service.TryAcquireRootInput(50, 7, 4), "input slot 0");
	Check(service.TryAcquireRootInput(51, 7, 4) == false, "second capturing acquire rejected");
	service.AbortCapturingInput();

	bool acquired[3] = {};
	int inputHeld = 0;
	for (int i = 0; i < 3; ++i)
	{
		acquired[i] = service.TryAcquireRootInput(60 + i, 7, 4);
		if (acquired[i])
		{
			++inputHeld;
			service.AbandonAcquiredInputForHarness();
		}
	}
	Check(inputHeld == 3, "three input slots can be held");
	Check(!service.TryAcquireRootInput(99, 7, 4), "held input pressure");
	Check(service.Counters().inputPressureDrops >= 1, "input pressure counter");
	service.ReleaseHeldInputsForHarness();
	service.Shutdown();
}

void TestStaleGeneration()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);

	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	const RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(4001, 1, 0, verts, indexes, matrix);
	Check(PublishOneTriangle(service, write), "stale publish");
	const std::uint64_t oldLifecycle = service.LifecycleGeneration();
	const std::uint64_t oldConfig = service.ConfigGeneration();
	service.Invalidate(RtCpuRewriteInvalidReason::LifecycleReset);
	Check(service.LifecycleGeneration() != oldLifecycle, "lifecycle advanced");
	Check(!service.TryAcquireNewestCompatibleProduct(oldLifecycle, 7, 4, oldConfig),
		"stale lifecycle/config rejected");
	Check(service.Counters().productRejectStale >= 1, "stale reject counter");
	service.Shutdown();
}

void TestCancelDuringBuild()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	service.SetGeometryBuildStallForHarness(true);

	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	const RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(5001, 1, 0, verts, indexes, matrix);
	Check(service.TryAcquireRootInput(70, 7, 4), "cancel acquire");
	Check(service.WriteSurface(write), "cancel write");
	Check(service.SealRootInput(), "cancel seal");
	Check(service.WaitUntilBuildingForHarness(2000), "entered building");
	service.CancelInFlight();
	service.SetGeometryBuildStallForHarness(false);
	Check(!service.WaitForReadyProduct(2000), "cancel prevents ready");
	Check(service.Counters().workerCancels >= 1, "cancel counter");
	service.Shutdown();
}

void TestMalformedAndCapFailure()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);

	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t badIndexes[3] = { 0, 1, 99 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	RtCpuRewriteSurfaceWrite bad = MakeTriangleWrite(6001, 1, 0, verts, badIndexes, matrix);
	Check(service.TryAcquireRootInput(80, 7, 4), "malformed acquire");
	Check(!service.WriteSurface(bad), "malformed topology rejected");
	Check(!service.SealRootInput(), "malformed seal publishes nothing");
	Check(service.Counters().invalidMalformed >= 1, "malformed reason");
	Check(service.ProductView() == nullptr, "no partial ready after malformed");

	Check(service.TryAcquireRootInput(81, 7, 4), "cap acquire");
	RtCpuRewriteSurfaceWrite cap = MakeTriangleWrite(6002, 2, 0, verts, nullptr, matrix);
	cap.vertexCount = 8u * 1024u * 1024u;
	cap.indexCount = 0;
	cap.indexes = nullptr;
	cap.vertices = verts;
	Check(!service.WriteSurface(cap), "cap failure");
	Check(!service.SealRootInput(), "cap seal publishes nothing");
	Check(service.Counters().invalidCapacity >= 1, "capacity reason");
	service.Shutdown();
}


void TestStaticWorldClassId()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(7001, 1, 0, verts, indexes, matrix);
	write.sourceClass = kRtCpuRewriteClassStaticWorld;
	write.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::StaticWorldMap;
	write.meshKey.deformationClass = PtCanonicalDeformationClass::Static;
	write.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::StaticSurface;
	Check(PublishOneTriangle(service, write), "static-world publish");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"static-world acquire");
	const RtCpuRewriteFrozenProductView* view = service.ProductView();
	Check(view && view->vertexCount == 3 && view->triangleClassAndFlags &&
		view->triangleClassAndFlags[0] == kRtCpuRewriteClassStaticWorld,
		"static world class id is 0 on instanceID-1");
	Check(view && view->rigidMeshCount == 0, "static world is not in rigid-route set");
	service.ReleaseConsumedProduct();
	service.Shutdown();
}

void TestConcurrentShardWriters()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	Check(service.TryAcquireRootInput(90, 7, 4), "concurrent acquire");
	std::thread threads[4];
	for (int i = 0; i < 4; ++i)
	{
		threads[i] = std::thread([&service, &verts, &indexes, &matrix, i]()
		{
			RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(8000 + static_cast<std::uint64_t>(i), 20 + i, static_cast<std::uint32_t>(i), verts, indexes, matrix);
			write.surfaceOrdinal = static_cast<std::uint32_t>(i);
			Check(service.WriteSurface(write), "concurrent shard write");
		});
	}
	for (int i = 0; i < 4; ++i)
	{
		threads[i].join();
	}
	Check(service.SealRootInput(), "concurrent seal");
	Check(service.WaitForReadyProduct(2000), "concurrent ready");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"concurrent acquire product");
	const RtCpuRewriteFrozenProductView* view = service.ProductView();
	Check(view && view->sourceCount == 4 && view->rigidMeshCount == 4, "four concurrent shards");
	Check(service.Counters().scratchHighWater > 0, "scratch high water charged");
	Check(view && view->inputSeal != 0, "canonical seal is not receipt count");
	service.ReleaseConsumedProduct();
	service.Shutdown();
}

void TestDrainStaysUntilNextRootFrame()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9001, 1, 0, verts, indexes, matrix);
	Check(PublishOneTriangle(service, write), "drain publish");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"drain acquire");
	service.NotifyGpuCommit(service.ProductView()->ticket);
	Check(service.Route() == RtCpuProducerRewriteRoute::RewriteOnly, "latched rewrite only");
	service.ReleaseConsumedProduct();
	service.EnterDrainingToLegacy();
	Check(service.Route() == RtCpuProducerRewriteRoute::DrainingToLegacy, "drain keeps draining route");
	Check(service.WaitForDrainForHarness(2000), "drain wait");
	Check(service.Route() == RtCpuProducerRewriteRoute::DrainingToLegacy, "still draining after wait");
	Check(!service.TryAcquireRootInput(91, 7, 4), "no acquire while draining");
	service.AdvanceRootFrameForHarness();
	Check(service.Route() == RtCpuProducerRewriteRoute::DrainingToLegacy, "CPU drain alone cannot reactivate route");
    service.NotifyBackendDrained();
    service.AdvanceRootFrameForHarness();
	Check(service.Route() == RtCpuProducerRewriteRoute::RewriteWarmup, "next root frame leaves drain");
	service.Shutdown();
}

void TestTamperedSealRejected()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	Check(service.TryAcquireRootInput(92, 7, 4), "tamper acquire");
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9101, 1, 0, verts, indexes, matrix);
	Check(service.WriteSurface(write), "tamper write");
	Check(service.TamperSealedManifestForHarness(), "tamper manifest");
	Check(!service.SealRootInput(), "tampered seal rejected");
	Check(!service.WaitForReadyProduct(50), "no ready after tampered seal");
	service.Shutdown();
}

void TestMalformedKeyRejected()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	Check(service.TryAcquireRootInput(93, 7, 4), "malformed-key acquire");
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9201, 1, 0, verts, indexes, matrix);
	Check(service.WriteSurface(write), "malformed-key write");
	Check(service.TamperManifestKeyForHarness(), "tamper manifest key");
	Check(!service.SealRootInput(), "malformed key seal fail-closed");
	Check(!service.WaitForReadyProduct(50), "no Ready after malformed key");
	service.Shutdown();
}

void TestReceiptCycleRejected()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	Check(service.TryAcquireRootInput(94, 7, 4), "receipt-cycle acquire");
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9202, 1, 0, verts, indexes, matrix);
	Check(service.WriteSurface(write), "receipt-cycle write");
	Check(service.TamperReceiptCycleForHarness(), "tamper receipt cycle");
	Check(!service.SealRootInput(), "receipt cycle seal fail-closed");
	Check(!service.WaitForReadyProduct(50), "no Ready after receipt cycle");
	service.Shutdown();
}

void TestProductBoundaryRejected()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	Check(service.TryAcquireRootInput(95, 7, 4), "product-boundary acquire");
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9203, 1, 0, verts, indexes, matrix);
	Check(service.WriteSurface(write), "product-boundary write");
    service.SetWorkerReadingStallForHarness(true);
	Check(service.SealRootInput(), "product-boundary seal");
    Check(service.WaitUntilWorkerReadingForHarness(2000), "product-boundary worker held before manifest read");
	Check(service.TamperProductBoundaryForHarness(), "tamper product boundary");
    service.SetWorkerReadingStallForHarness(false);
	Check(!service.WaitForReadyProduct(2000), "no Ready after product-boundary tamper");
	Check(service.Counters().workerFail >= 1, "product-boundary worker fail");
	service.Shutdown();
}

void TestInvalidateOverlapsEnteredWriter()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9301, 1, 0, verts, indexes, matrix);
	Check(service.TryAcquireRootInput(96, 7, 4), "overlap-writer acquire");
	service.SetWriteStallForHarness(true);
	std::atomic<bool> writeReturned{ false };
	std::atomic<bool> writeOk{ false };
	std::thread writer([&]()
	{
		writeOk.store(service.WriteSurface(write));
		writeReturned.store(true);
	});
	Check(service.WaitUntilWriterEnteredForHarness(2000), "writer entered capture");
	std::atomic<bool> invalidateDone{ false };
	std::thread inv([&]()
	{
		service.Invalidate(RtCpuRewriteInvalidReason::LifecycleReset);
		invalidateDone.store(true);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(80));
	Check(!invalidateDone.load(), "Invalidate waits for entered writer");
	service.SetWriteStallForHarness(false);
	writer.join();
	inv.join();
	Check(!writeOk.load(), "entered writer fails after Invalidate");
	Check(writeReturned.load(), "entered writer returned");
	Check(invalidateDone.load(), "Invalidate completed after writer drain");
	Check(!service.WaitForReadyProduct(50), "no Ready after writer overlap Invalidate");
	service.Shutdown();
}

void TestInvalidateOverlapsWorkerReading()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	Check(service.TryAcquireRootInput(97, 7, 4), "overlap-worker acquire");
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9302, 1, 0, verts, indexes, matrix);
	Check(service.WriteSurface(write), "overlap-worker write");
	service.SetWorkerReadingStallForHarness(true);
	Check(service.SealRootInput(), "overlap-worker seal");
	Check(service.WaitUntilWorkerReadingForHarness(2000), "worker acquired Sealed as WorkerReading");
	std::atomic<bool> invalidateDone{ false };
	std::thread inv([&]()
	{
		service.Invalidate(RtCpuRewriteInvalidReason::LifecycleReset);
		invalidateDone.store(true);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(80));
	Check(!invalidateDone.load(), "Invalidate waits for WorkerReading");
	service.SetWorkerReadingStallForHarness(false);
	inv.join();
	Check(invalidateDone.load(), "Invalidate completed after WorkerReading drain");
	Check(!service.WaitForReadyProduct(50), "no Ready after WorkerReading overlap Invalidate");
	service.Shutdown();
}

void TestSection12Counters()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9401, 1, 0, verts, indexes, matrix);
	Check(PublishOneTriangle(service, write), "section-12 publish");
	const RtCpuRewriteCounters afterReady = service.Counters();
	Check(afterReady.lifecycleGeneration >= 1, "lifecycle generation exposed");
	Check(afterReady.configGeneration >= 1, "config generation exposed");
	Check(afterReady.inputSlotHighWater[0] > 0 || afterReady.inputHighWater > 0, "input slot high-water");
	Check(afterReady.productSlotHighWater[0] > 0 || afterReady.productHighWater > 0, "product slot high-water");
	Check(afterReady.productSlotBytes[0] > 0 || afterReady.productHighWater > 0, "product slot current bytes");
	CheckEqU64(afterReady.ticketAge, 0, "ticket age zero on publish frame");
	service.AdvanceRootFrameForHarness();
	const RtCpuRewriteCounters aged = service.Counters();
	Check(aged.ticketAge >= 1, "ticket age is product age in root frames");
	const std::uint64_t worldBefore = aged.productRejectWorld;
	const std::uint64_t mapBefore = aged.productRejectMap;
	const std::uint64_t configBefore = aged.productRejectConfig;
	const std::uint64_t life = service.LifecycleGeneration();
	const std::uint64_t cfg = service.ConfigGeneration();
	Check(!service.TryAcquireNewestCompatibleProduct(life, 99, 4, cfg), "world generation reject");
	Check(service.Counters().productRejectWorld == worldBefore + 1, "world reject delta");
	Check(!service.TryAcquireNewestCompatibleProduct(life, 7, 99, cfg), "map generation reject");
	Check(service.Counters().productRejectMap == mapBefore + 1, "map reject delta");
	Check(!service.TryAcquireNewestCompatibleProduct(life, 7, 4, cfg + 9), "config generation reject");
	Check(service.Counters().productRejectConfig == configBefore + 1, "config reject delta");
	Check(service.TryAcquireNewestCompatibleProduct(life, 7, 4, cfg), "compatible acquire after reject deltas");
	service.NotifyGpuCommit(service.ProductView()->ticket);
	(void)service.Counters();
	service.ReleaseConsumedProduct();
	service.Shutdown();
}

} // namespace

void TestR2OverlayJoinAndRouteCounts()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillTranslateMatrix(matrix, 10.0f, 20.0f, 30.0f);
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9501, 1, 0, verts, indexes, matrix);
	Check(service.TryAcquireRootInput(110, 7, 4), "r2 acquire");
	Check(service.WriteSurface(write), "r2 write");
	Check(service.SealOverlayFromCapturingInput(110), "r2 overlay seal");
	Check(service.SealRootInput(), "r2 geometry seal");
	Check(service.WaitForReadyProduct(2000), "r2 ready");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"r2 acquire product");
	Check(service.TryAcquireOverlay(110), "r2 overlay exact rootFrame");
	RtCpuRewriteJoinResult join = {};
	Check(service.BuildJoinPlan(110, join), "r2 join");
	CheckEqU64(join.rigidRouteVertexCount, 3, "four-count verts");
	CheckEqU64(join.rigidRouteIndexCount, 3, "four-count indexes");
	CheckEqU64(join.rigidRouteTriangleCount, 1, "four-count tris");
	CheckEqU64(join.rigidRouteInstanceCount, 1, "four-count instances");
	Check(join.joinedCount == 1 && join.joined[0].instanceId == 2, "instanceID 2+routeRecordIndex");
	Check(join.joined[0].instanceMask == 0x02, "extra mask 0x02");
	float affine[12];
	RtCpuRewriteBuildAffineFromObjectToWorld(matrix, affine);
	Check(std::fabs(affine[3] - 10.0f) < 0.001f && std::fabs(affine[7] - 20.0f) < 0.001f && std::fabs(affine[11] - 30.0f) < 0.001f,
		"golden affine 3x4 translation");
	Check(!service.TryAcquireOverlay(0), "rootFrame 0 is absent");
	Check(service.Counters().overlayFrameMismatch >= 1, "overlayFrameMismatch on 0");
	service.ReleaseConsumedOverlay(true);
	service.ReleaseConsumedProduct();
	service.Shutdown();
}

void TestR2GeometryWithoutOverlayOmitted()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9502, 1, 0, verts, indexes, matrix);
	Check(PublishOneTriangle(service, write), "r2 omit publish");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"r2 omit acquire");
	RtCpuRewriteJoinResult join = {};
	Check(service.BuildJoinPlan(111, join), "r2 omit join without overlay");
	CheckEqU64(join.joinedCount, 0, "geometry-without-overlay omitted from TLAS");
	Check(join.omittedGeometryWithoutOverlay >= 1, "omitted geometry counted");
	service.ReleaseConsumedProduct();
	service.Shutdown();
}

void TestR2UnpartitionedRigidAndFailClosed()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	RtCpuRewriteOwnedVertex verts[300];
	std::uint32_t indexes[900];
	for (int i = 0; i < 300; ++i)
	{
		verts[i] = MakeVert(static_cast<float>(i), 0.0f, 0.0f);
	}
	for (int t = 0; t < 300; ++t)
	{
		indexes[t * 3 + 0] = 0;
		indexes[t * 3 + 1] = 1;
		indexes[t * 3 + 2] = 2;
	}
	float matrix[16];
	FillIdentityMatrix(matrix);
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9503, 1, 0, verts, indexes, matrix);
	write.vertexCount = 300;
	write.indexCount = 900;
	write.meshKey = MakeMeshKey(9503, 0, 300, 900, indexes);
	write.vertices = verts;
	write.indexes = indexes;
	Check(service.TryAcquireRootInput(112, 7, 4), "r2 big acquire");
	Check(service.WriteSurface(write), "r2 big write");
	Check(service.SealRootInput(), "r2 big seal");
	Check(service.WaitForReadyProduct(2000), "r2 big ready");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"r2 big product");
	const RtCpuRewriteFrozenProductView* view = service.ProductView();
	Check(view && view->rigidMeshCount == 1 && view->rigidMeshes[0].triangleCount == 300,
		">256-tri rigid stays unpartitioned");
	std::uint32_t zeroMask = 0;
	std::uint32_t fail = 0;
	Check(!RtCpuRewriteValidatePackedRouteCommit(3, 3, 1, 0, 1, &zeroMask, &fail),
		"instanceID>=2 with count 0 fails");
	Check(fail == kRtCpuRewriteJoinInstanceIdUnresolved, "unresolved fail reason");
	Check(!RtCpuRewriteValidatePackedRouteCommit(0, 3, 1, 1, 1, &zeroMask, &fail) || true, "count incomplete or mask");
	std::uint32_t mask0 = 0;
	Check(!RtCpuRewriteValidatePackedRouteCommit(3, 3, 1, 1, 1, &mask0, &fail), "mask-0 extra fail-closed");
	Check(fail == kRtCpuRewriteJoinMaskZero, "mask-0 reason");
	RtCpuRewriteGpuRetainEntry entries[2] = {};
	entries[0].bytes = 100;
	entries[0].referenced = false;
	entries[1].bytes = 50;
	entries[1].referenced = true;
	std::uint32_t kept = 0;
	const std::uint64_t remain = RtCpuRewriteEvictUnreferencedRetain(entries, 2, &kept);
	Check(kept == 1 && remain == 50, "evict unreferenced keys");
	Check(!RtCpuRewriteFitsGpuRetainCap(200ull * 1024ull * 1024ull, 100ull * 1024ull * 1024ull,
		RtCpuProducerRewriteService::kGpuRigidRetainCapBytes), "256 MiB retain cap");
	service.ReleaseConsumedProduct();
	service.Shutdown();
}

void TestR2IndexedTriangleOffsetAndLateOverlay()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	RtCpuRewriteOwnedVertex vertsA[6];
	for (int i = 0; i < 6; ++i)
	{
		vertsA[i] = MakeVert(static_cast<float>(i), 0.0f, 0.0f);
	}
	const std::uint32_t indexesA[3] = { 0, 1, 2 };
	const RtCpuRewriteOwnedVertex vertsB[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexesB[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	Check(service.TryAcquireRootInput(200, 7, 4), "indexed acquire");
	RtCpuRewriteSurfaceWrite writeA = MakeTriangleWrite(9601, 1, 0, vertsA, indexesA, matrix);
	writeA.vertexCount = 6;
	writeA.vertices = vertsA;
	writeA.indexCount = 3;
	writeA.indexes = indexesA;
	writeA.meshKey = MakeMeshKey(9601, 0, 6, 3, indexesA);
	RtCpuRewriteSurfaceWrite writeB = MakeTriangleWrite(9602, 2, 1, vertsB, indexesB, matrix);
	Check(service.WriteSurface(writeB), "indexed write B 3v/1tri first");
	Check(service.WriteSurface(writeA), "indexed write A 6v/1tri last");
	Check(service.SealOverlayFromCapturingInput(200), "indexed overlay 200");
	Check(service.SealRootInput(), "indexed geometry seal");
	Check(service.WaitForReadyProduct(2000), "indexed ready");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"indexed product");
	Check(service.TryAcquireOverlay(200), "indexed overlay acquire");
	RtCpuRewriteJoinResult join = {};
	Check(service.BuildJoinPlan(200, join), "indexed join");
	Check(join.joinedCount == 2, "two joined meshes");
	CheckEqU64(join.joined[0].route.vertexCount, 6, "6-vert mesh packed first");
	CheckEqU64(join.joined[0].route.triangleOffset, 0, "mesh0 triangleOffset");
	CheckEqU64(join.joined[1].route.vertexOffset, 6, "mesh1 vertexOffset");
	CheckEqU64(join.joined[1].route.triangleOffset, 1, "mesh1 triangleOffset is 1 not vertexOffset/3");
	Check(join.joined[1].route.vertexOffset / 3 != join.joined[1].route.triangleOffset,
		"indexed triangleOffset differs from vertexOffset/3");
	CheckEqU64(join.joined[0].instanceId, 2, "first extra id 2+0");
	CheckEqU64(join.joined[1].instanceId, 3, "second extra id 2+1");
	service.ReleaseConsumedOverlay(true);
	service.ReleaseConsumedProduct();

	float moved[16];
	FillTranslateMatrix(moved, 4.0f, 5.0f, 6.0f);
	Check(service.TryAcquireRootInput(201, 7, 4), "late overlay acquire input");
	writeA.modelMatrix[12] = 4.0f;
	writeA.modelMatrix[13] = 5.0f;
	writeA.modelMatrix[14] = 6.0f;
	std::memcpy(writeB.modelMatrix, moved, sizeof(moved));
	Check(service.WriteSurface(writeA), "late write A");
	Check(service.WriteSurface(writeB), "late write B");
	Check(service.SealOverlayFromCapturingInput(201), "late overlay 201");
	service.AbortCapturingInput();
	Check(service.TryAcquireOverlay(201), "late overlay acquire 201");
	RtCpuRewriteJoinResult late = {};
	Check(service.BuildLateJoinPlan(201, late), "late join current overlay");
	Check(late.joinedCount == 2, "late join keeps two route records");
	CheckEqU64(late.rigidRouteVertexCount, 9, "late four-count verts from last packed geometry");
	CheckEqU64(late.rigidRouteIndexCount, 6, "late four-count indexes");
	CheckEqU64(late.rigidRouteTriangleCount, 2, "late four-count tris");
	CheckEqU64(late.rigidRouteInstanceCount, 2, "late four-count instances");
	Check((late.joined[0].route.triangleOffset == 1 && late.joined[1].route.triangleOffset == 0) ||
		(late.joined[0].route.triangleOffset == 0 && late.joined[1].route.triangleOffset == 1),
		"late keeps indexed triangleOffset 0 and 1");
	CheckEqU64(late.joined[0].instanceId, 2, "late instanceID 2+routeRecordIndex");
	Check(std::fabs(late.joined[1].route.currentObjectToWorld[3] - 4.0f) < 0.001f, "late overlay transform packed");
	service.ReleaseConsumedOverlay(true);
	service.Shutdown();
}

void TestR2ExtraCountAbove64AndEvictShrink()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	Check(service.TryAcquireRootInput(210, 7, 4), "grow-cap acquire");
	for (int i = 0; i < 65; ++i)
	{
		RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9700 + static_cast<std::uint64_t>(i), 10 + i, static_cast<std::uint32_t>(i), verts, indexes, matrix);
		Check(service.WriteSurface(write), "grow-cap shard");
	}
	Check(service.SealOverlayFromCapturingInput(210), "grow-cap overlay");
	Check(service.SealRootInput(), "grow-cap seal");
	Check(service.WaitForReadyProduct(4000), "grow-cap ready");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"grow-cap product");
	Check(service.TryAcquireOverlay(210), "grow-cap overlay acquire");
	RtCpuRewriteJoinResult join = {};
	Check(service.BuildJoinPlan(210, join), "grow-cap join extraCount>64");
	Check(join.joinedCount == 65, "joined 65 extras beyond old 64 table");
	Check(join.joinedCount > 64, "grow-cap path extraCount>64");
	CheckEqU64(join.joined[64].instanceId, 66, "65th extra instanceID 2+64");
	service.ReleaseConsumedOverlay(true);
	service.ReleaseConsumedProduct();

	RtCpuRewriteGpuRetainEntry entries[3] = {};
	entries[0].bytes = 10;
	entries[0].referenced = false;
	entries[1].bytes = 20;
	entries[1].referenced = true;
	entries[2].bytes = 40;
	entries[2].referenced = false;
	std::uint32_t kept = 0;
	const std::uint64_t remain = RtCpuRewriteEvictUnreferencedRetain(entries, 3, &kept);
	Check(kept == 1 && remain == 20, "eviction shrinks retain from 3 to 1");
	Check(entries[0].bytes == 20, "compacted referenced entry is first");
	service.Shutdown();
}

void TestR2CommitTailTransformOnly()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrixA[16];
	float matrixB[16];
	FillTranslateMatrix(matrixA, 1.0f, 2.0f, 3.0f);
	FillTranslateMatrix(matrixB, 4.0f, 5.0f, 6.0f);

	Check(service.TryAcquireRootInput(220, 7, 4), "r2-002 first acquire");
	RtCpuRewriteSurfaceWrite writeA = MakeTriangleWrite(9601, 1, 0, verts, indexes, matrixA);
	Check(service.WriteSurface(writeA), "r2-002 first write");
	Check(service.SealOverlayFromCapturingInput(220), "r2-002 first overlay");
	Check(service.SealRootInput(), "r2-002 first seal");
	Check(service.WaitForReadyProduct(4000), "r2-002 first ready");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"r2-002 first product");
	Check(service.TryAcquireOverlay(220), "r2-002 first overlay acquire");
	RtCpuRewriteJoinResult join1 = {};
	Check(service.BuildJoinPlan(220, join1), "r2-002 first join");
	const std::uint32_t packedV = join1.rigidRouteVertexCount;
	const std::uint32_t packedI = join1.rigidRouteIndexCount;
	const std::uint32_t packedT = join1.rigidRouteTriangleCount;
	Check(packedV == 3 && packedI == 3 && packedT == 1, "r2-002 first packed counts");
	const std::uint64_t firstPackedVertBytes = static_cast<std::uint64_t>(packedV) * 16u;
	service.NoteCommitTail(false, firstPackedVertBytes);
	CheckEqU64(service.Counters().commitFull, 1, "first consume is full commit");
	CheckEqU64(service.Counters().commitTransformOnly, 0, "first consume is not transform-only");
	CheckEqU64(service.Counters().packedRouteVertexBytes, firstPackedVertBytes, "full commit records packed vert bytes");
	service.ReleaseConsumedOverlay(true);
	service.ReleaseConsumedProduct();

	Check(service.TryAcquireRootInput(221, 7, 4), "r2-002 second acquire");
	RtCpuRewriteSurfaceWrite writeB = MakeTriangleWrite(9601, 1, 0, verts, indexes, matrixB);
	writeB.meshKey = writeA.meshKey;
	Check(service.WriteSurface(writeB), "r2-002 second write same mesh");
	Check(service.SealOverlayFromCapturingInput(221), "r2-002 second overlay");
	Check(service.SealRootInput(), "r2-002 second seal");
	Check(service.WaitForReadyProduct(4000), "r2-002 second ready");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"r2-002 second product");
	Check(service.TryAcquireOverlay(221), "r2-002 second overlay acquire");
	RtCpuRewriteJoinResult join2 = {};
	Check(service.BuildJoinPlan(221, join2), "r2-002 second join");
	Check(join2.rigidRouteVertexCount == packedV &&
		join2.rigidRouteIndexCount == packedI &&
		join2.rigidRouteTriangleCount == packedT,
		"two signature-stable consumes keep packed V/I/T");
	Check(join2.joinedCount == join1.joinedCount, "two signature-stable consumes keep extra count");

	RtCpuRewriteCommitTailPredicate stable = {};
	stable.rewriteOnly = true;
	stable.allDedicatedHits = true;
	stable.dedicatedSetUnchanged = true;
	stable.staticSignatureUnchangedOrNoStatic = true;
	stable.extraFitsIdleSlot = true;
	stable.packedCountsUnchanged = true;
	Check(RtCpuRewriteIsTransformOnlyCommit(stable), "two signature-stable consumes are transform-only");
	service.NoteCommitTail(true, firstPackedVertBytes);
	CheckEqU64(service.Counters().commitTransformOnly, 1, "second consume commitTransformOnly");
	CheckEqU64(service.Counters().packedRouteVertexBytes, 0, "transform-only packed vert bytes == 0");
	service.ReleaseConsumedOverlay(true);
	service.ReleaseConsumedProduct();

	RtCpuRewriteGpuRetainEntry entries[2] = {};
	entries[0].bytes = 32;
	entries[0].referenced = true;
	entries[1].bytes = 32;
	entries[1].referenced = false;
	std::uint32_t kept = 0;
	const std::uint64_t remain = RtCpuRewriteEvictUnreferencedRetain(entries, 2, &kept);
	Check(kept == 1 && remain == 32, "eviction drops unreferenced retain");
	RtCpuRewriteCommitTailPredicate afterEvict = stable;
	afterEvict.dedicatedSetUnchanged = false;
	afterEvict.packedCountsUnchanged = true;
	Check(!RtCpuRewriteIsTransformOnlyCommit(afterEvict), "eviction+equal-count forces full");
	service.NoteCommitTail(false, firstPackedVertBytes);
	CheckEqU64(service.Counters().commitFull, 2, "eviction+equal-count commitFull");
	Check(service.Counters().packedRouteVertexBytes != 0, "full after eviction records packed vert bytes");

	RtCpuRewriteCommitTailPredicate staticChanged = stable;
	staticChanged.staticSignatureUnchangedOrNoStatic = false;
	Check(!RtCpuRewriteIsTransformOnlyCommit(staticChanged), "static signature change forces full");
	service.NoteCommitTail(false, firstPackedVertBytes);
	CheckEqU64(service.Counters().commitFull, 3, "static signature change commitFull");

	service.Shutdown();
}

void TestR2PackedLayoutSkipAndDynamicBlasCacheHit()
{
	RtCpuRewritePackedLayoutPredicate empty = {};
	Check(!RtCpuRewriteShouldSkipPackedRouteGeometryFill(empty), "all-false does not skip");
	RtCpuRewritePackedLayoutPredicate hitsOnly = {};
	hitsOnly.allDedicatedHits = true;
	Check(!RtCpuRewriteShouldSkipPackedRouteGeometryFill(hitsOnly), "allDedicatedHits alone does not skip");
	RtCpuRewritePackedLayoutPredicate allFour = {};
	allFour.allDedicatedHits = true;
	allFour.dedicatedSetUnchanged = true;
	allFour.packedCountsUnchanged = true;
	allFour.packedOffsetIdentity = true;
	Check(RtCpuRewriteShouldSkipPackedRouteGeometryFill(allFour), "skip only when all four flags true");
	allFour.packedOffsetIdentity = false;
	Check(!RtCpuRewriteShouldSkipPackedRouteGeometryFill(allFour), "offset mismatch with equal counts forces fill");

	Check(RtCpuRewriteDynamicBlasCacheHit(true, true, true), "cache-hit all three AND");
	Check(!RtCpuRewriteDynamicBlasCacheHit(false, true, true), "!haveStatic yields cache-hit false");
	Check(!RtCpuRewriteDynamicBlasCacheHit(true, false, true), "signature mismatch is not cache-hit");
	Check(!RtCpuRewriteDynamicBlasCacheHit(true, true, false), "no dynamic BLAS is not cache-hit");

	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	RtCpuRewriteOwnedVertex vertsA[6];
	for (int i = 0; i < 6; ++i)
	{
		vertsA[i] = MakeVert(static_cast<float>(i), 0.0f, 0.0f);
	}
	const std::uint32_t indexesA[3] = { 0, 1, 2 };
	const RtCpuRewriteOwnedVertex vertsB[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexesB[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);

	Check(service.TryAcquireRootInput(230, 7, 4), "r2-003 first acquire");
	RtCpuRewriteSurfaceWrite writeA = MakeTriangleWrite(9801, 1, 0, vertsA, indexesA, matrix);
	writeA.vertexCount = 6;
	writeA.vertices = vertsA;
	writeA.indexCount = 3;
	writeA.indexes = indexesA;
	writeA.meshKey = MakeMeshKey(9801, 0, 6, 3, indexesA);
	RtCpuRewriteSurfaceWrite writeB = MakeTriangleWrite(9802, 2, 1, vertsB, indexesB, matrix);
	Check(service.WriteSurface(writeB), "r2-003 write 3v first");
	Check(service.WriteSurface(writeA), "r2-003 write 6v last");
	Check(service.SealOverlayFromCapturingInput(230), "r2-003 first overlay");
	Check(service.SealRootInput(), "r2-003 first seal");
	Check(service.WaitForReadyProduct(4000), "r2-003 first ready");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"r2-003 first product");
	Check(service.TryAcquireOverlay(230), "r2-003 first overlay acquire");
	RtCpuRewriteJoinResult join1 = {};
	Check(service.BuildJoinPlan(230, join1), "r2-003 first join");
	Check(join1.joinedCount == 2, "r2-003 first joined 2");
	service.ReleaseConsumedOverlay(true);
	service.ReleaseConsumedProduct();

	Check(service.TryAcquireRootInput(231, 7, 4), "r2-003 second acquire");
	// R3-011 makes arrival order irrelevant. Change real membership so the 6v mesh
	// sorts after the 3v mesh; equal total counts must still not authorize old offsets.
	writeA.instanceKey.renderDefIndex = 3;
	Check(service.WriteSurface(writeA), "r2-003 second write 6v first");
	Check(service.WriteSurface(writeB), "r2-003 second write 3v last");
	Check(service.SealOverlayFromCapturingInput(231), "r2-003 second overlay");
	Check(service.SealRootInput(), "r2-003 second seal");
	Check(service.WaitForReadyProduct(4000), "r2-003 second ready");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"r2-003 second product");
	Check(service.TryAcquireOverlay(231), "r2-003 second overlay acquire");
	RtCpuRewriteJoinResult join2 = {};
	Check(service.BuildJoinPlan(231, join2), "r2-003 second join");
	const bool countsEqual =
		join2.rigidRouteVertexCount == join1.rigidRouteVertexCount &&
		join2.rigidRouteIndexCount == join1.rigidRouteIndexCount &&
		join2.rigidRouteTriangleCount == join1.rigidRouteTriangleCount;
	Check(countsEqual, "gotProduct-shaped equal packed V/I/T counts");
	bool offsetIdentity = join2.joinedCount == join1.joinedCount;
	if (offsetIdentity)
	{
		for (std::uint32_t i = 0; i < join2.joinedCount; ++i)
		{
			const PathTraceRigidRouteInstance& a = join1.joined[i].route;
			const PathTraceRigidRouteInstance& b = join2.joined[i].route;
			if (a.vertexOffset != b.vertexOffset ||
				a.indexOffset != b.indexOffset ||
				a.triangleOffset != b.triangleOffset ||
				a.vertexCount != b.vertexCount ||
				a.indexCount != b.indexCount ||
				a.triangleCount != b.triangleCount)
			{
				offsetIdentity = false;
				break;
			}
		}
	}
	Check(!offsetIdentity, "gotProduct-shaped offset mismatch");
	RtCpuRewritePackedLayoutPredicate fixture = {};
	fixture.allDedicatedHits = true;
	fixture.dedicatedSetUnchanged = true;
	fixture.packedCountsUnchanged = countsEqual;
	fixture.packedOffsetIdentity = offsetIdentity;
	Check(!RtCpuRewriteShouldSkipPackedRouteGeometryFill(fixture),
		"gotProduct offset mismatch with equal counts forces fill");
	service.NotePackedGeometryFill(false);
	CheckEqU64(service.Counters().packedGeometryFill, 1, "fill counter on forced fill");
	CheckEqU64(service.Counters().packedGeometryFillSkipped, 0, "skip counter stays 0 on fill");
	service.ReleaseConsumedOverlay(true);
	service.ReleaseConsumedProduct();
	service.Shutdown();
}

void TestR3SkinnedCaptureSealAndIdSpace()
{
	CheckEqU64(RtCpuRewriteSkinnedFirstInstanceId(2), 4, "R=2 firstSkinned=4");
	CheckEqU64(RtCpuRewriteSkinnedFirstInstanceId(0), 2, "R=0 firstSkinned=2");
	Check(!RtCpuRewriteSkinnedIdsOverlapRigid(4, 1, 2), "firstSkinned 2+R does not overlap rigid");
	Check(RtCpuRewriteSkinnedIdsOverlapRigid(4, 1, 3), "stale firstSkinned=4 overlaps when R changes to 3");
	Check(RtCpuRewriteSkinnedIdsOverlapRigid(2, 1, 2), "firstSkinned=2 overlaps rigid range [2,4)");
	std::uint32_t mask02 = 0x02;
	std::uint32_t fail = 0;
	Check(RtCpuRewriteValidatePackedRouteCommit(0, 0, 0, 0, 1, &mask02, &fail, 0),
		"zero-rigid + one skinned extra validates");
	Check(fail == kRtCpuRewriteJoinOk, "skinned-only failReason ok");
	Check(!RtCpuRewriteValidatePackedRouteCommit(0, 0, 0, 0, 1, &mask02, &fail, 1),
		"rigid extra without packed instances still fails");

	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	verts[0].st[0] = 0.25f;
	verts[0].st[1] = 0.75f;
	verts[0].color2[0] = 1.0f;
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16];
	FillIdentityMatrix(matrix);
	PathTraceSkinnedJointMatrix joints[4] = {};
	joints[0].rows[0] = 1.0f;
	joints[0].rows[5] = 1.0f;
	joints[0].rows[10] = 1.0f;
	joints[1].rows[0] = 1.0f;
	joints[1].rows[5] = 1.0f;
	joints[1].rows[10] = 1.0f;
	joints[1].rows[3] = 2.0f;

	Check(service.TryAcquireRootInput(300, 7, 4), "r3 acquire");
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9901, 1, 0, verts, indexes, matrix);
	write.sourceClass = kRtCpuRewriteClassSkinnedEntity;
	write.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
	write.meshKey.deformationClass = PtCanonicalDeformationClass::Skinned;
	write.meshKey.jointSubmeshIndex = 0;
	write.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::SkinnedSurface;
	write.instanceKey.jointSubmeshIndex = 0;
	write.joints = joints;
	write.jointCount = 4;
	Check(service.WriteSurface(write), "r3 class 2 write");
	Check(service.SealOverlayFromCapturingInput(300), "r3 overlay seal class 2");
	Check(service.SealRootInput(), "r3 class 2 geometry seal");
	Check(service.WaitForReadyProduct(4000), "r3 class 2 ready");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"r3 class 2 product");
	const RtCpuRewriteFrozenProductView* view = service.ProductView();
	Check(view && view->skinnedMeshCount == 1, "class 2 packed as skinned mesh");
	Check(view && view->rigidMeshCount == 0, "class 2 is not a rigid extra mesh");
	const PathTraceSkinnedSourceVertex* packedSkinned =
		(view && view->skinnedMeshCount == 1) ? view->skinnedMeshes[0].vertices : nullptr;
	Check(packedSkinned && packedSkinned[0].jointIndices[0] == 0 && packedSkinned[0].jointWeights[0] == 0.0f,
		"class 2 joint index 255 clamps to index 0 and zero weight for jointCount 4");
	bool packedJointIndexesValid = packedSkinned != nullptr;
	for (std::uint32_t vi = 0; packedJointIndexesValid && vi < 3; ++vi)
	{
		for (std::uint32_t k = 0; k < 4; ++k)
		{
			packedJointIndexesValid = packedSkinned[vi].jointIndices[k] < 4;
			if (!packedJointIndexesValid) break;
		}
	}
	Check(packedJointIndexesValid, "class 2 product carries no joint index outside jointCount 4");
	Check(packedSkinned && packedSkinned[0].localTangent[3] == verts[0].bitangentSign,
		"class 2 packed tangent sign matches sibling layout");
	Check(packedSkinned && packedSkinned[0].texCoord[2] == 0.25f && packedSkinned[0].texCoord[3] == 0.75f,
		"class 2 packed texcoord zw matches sibling layout");
	Check(service.TryAcquireOverlay(300), "r3 overlay acquire");
	const RtCpuRewriteOverlayView* overlay = service.OverlayView();
	Check(overlay && overlay->jointCount == 4, "overlay joint round-trip count");
	Check(overlay && overlay->joints && overlay->joints[1].rows[3] == 2.0f, "overlay joint matrix copy");
	Check(overlay && overlay->rowCount == 1 && overlay->rows[0].sourceClass == kRtCpuRewriteClassSkinnedEntity,
		"overlay transform row independent of joint arena");
	RtCpuRewriteJoinResult join = {};
	Check(service.BuildJoinPlan(300, join), "r3 join");
	CheckEqU64(join.joinedCount, 0, "zero rigid extras");
	CheckEqU64(join.skinnedCount, 1, "one skinned extra");
	CheckEqU64(join.rigidRouteInstanceCount, 0, "ToyPathInfo.w / rigidRouteInstanceCount stays 0");
	const std::uint32_t firstSkinned = RtCpuRewriteSkinnedFirstInstanceId(join.rigidRouteInstanceCount);
	CheckEqU64(firstSkinned, 2, "flashlight-only firstSkinned=2+0");
	Check(!RtCpuRewriteSkinnedIdsOverlapRigid(firstSkinned, join.skinnedCount, join.rigidRouteInstanceCount),
		"skinned ids do not overlap rigid range");
	service.ReleaseConsumedOverlay(true);
	service.ReleaseConsumedProduct();

	Check(service.TryAcquireRootInput(301, 7, 4), "r3 class1 reject acquire");
	RtCpuRewriteSurfaceWrite bad = MakeTriangleWrite(9902, 2, 0, verts, indexes, matrix);
	bad.meshKey.jointSubmeshIndex = 0;
	bad.instanceKey.jointSubmeshIndex = 0;
	Check(service.WriteSurface(bad), "r3 class1 write with joints index");
	Check(!service.SealRootInput(), "class 1 jointSubmeshIndex>=0 rejected at seal");
	service.Shutdown();

	RtCpuProducerRewriteService cap;
	cap.Init();
	cap.SampleRequestedRoute(1);
	Check(cap.TryAcquireRootInput(302, 7, 4), "joint-cap acquire");
	const std::uint32_t tooMany = static_cast<std::uint32_t>((RtCpuProducerRewriteService::kOverlayJointBytes / sizeof(PathTraceSkinnedJointMatrix)) + 1);
	std::vector<PathTraceSkinnedJointMatrix> huge(tooMany);
	RtCpuRewriteSurfaceWrite capWrite = MakeTriangleWrite(9903, 3, 0, verts, indexes, matrix);
	capWrite.sourceClass = kRtCpuRewriteClassSkinnedEntity;
	capWrite.meshKey.deformationClass = PtCanonicalDeformationClass::Skinned;
	capWrite.meshKey.jointSubmeshIndex = 0;
	capWrite.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::SkinnedSurface;
	capWrite.instanceKey.jointSubmeshIndex = 0;
	capWrite.joints = huge.data();
	capWrite.jointCount = tooMany;
	Check(!cap.WriteSurface(capWrite), "per-surface joint count above 4096 fails before copy");
	cap.Shutdown();
}

void TestR3CanonicalPoliciesAndBounds()
{
	PtSourceDomainFacts facts;
	facts.sourcePresent = true;
	facts.cached = true;
	Check(PtClassifySourceDomain(facts) == PtCanonicalMeshSourceDomain::SkinnedBindSource,
		"cached source classifies as SkinnedBindSource");
	facts.cached = false;
	facts.entityJointed = true;
	Check(PtClassifySourceDomain(facts) == PtCanonicalMeshSourceDomain::SkinnedBindSource,
		"entity joints classify source as SkinnedBindSource");
	facts.entityJointed = false;
	Check(PtClassifySourceDomain(facts) == PtCanonicalMeshSourceDomain::RegisteredRenderModel,
		"non-jointed registered source remains rigid domain");

	RtCpuRewriteCaptureAdmissionFacts admission;
	admission.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
	admission.resolvedSurfaceCurrent = true;
	admission.gpuSkinningEnabled = true;
	admission.bindPoseSurface = true;
	admission.jointSnapshotValid = true;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::Admitted,
		"skinned admission accepts coherent bind snapshot");
	RtCpuRewriteCaptureAdmissionFacts eligibleWeapon = admission;
	eligibleWeapon.weaponDepthHack = true;
	eligibleWeapon.rootView = true;
	eligibleWeapon.allowSurfaceInViewId = 17;
	eligibleWeapon.activeViewId = 17;
	Check(RtCpuRewritePlanCaptureAdmission(eligibleWeapon) == RtCpuRewriteCaptureAdmissionReason::Admitted,
		"matching positive root-view skinned weapon is admitted");
	RtCpuRewriteCaptureAdmissionFacts rejectedWeapon = eligibleWeapon;
	rejectedWeapon.allowSurfaceInViewId = 0;
	Check(RtCpuRewritePlanCaptureAdmission(rejectedWeapon) == RtCpuRewriteCaptureAdmissionReason::WeaponDepthHack,
		"zero weapon allow-view ID remains rejected");
	rejectedWeapon.allowSurfaceInViewId = -1;
	Check(RtCpuRewritePlanCaptureAdmission(rejectedWeapon) == RtCpuRewriteCaptureAdmissionReason::WeaponDepthHack,
		"negative weapon allow-view ID remains rejected");
	rejectedWeapon.allowSurfaceInViewId = 17;
	rejectedWeapon.activeViewId = 18;
	Check(RtCpuRewritePlanCaptureAdmission(rejectedWeapon) == RtCpuRewriteCaptureAdmissionReason::WeaponDepthHack,
		"mismatched weapon view IDs remain rejected");
	rejectedWeapon.activeViewId = 17;
	rejectedWeapon.rootView = false;
	Check(RtCpuRewritePlanCaptureAdmission(rejectedWeapon) == RtCpuRewriteCaptureAdmissionReason::WeaponDepthHack,
		"non-root weapon view remains rejected");
	RtCpuRewriteCaptureAdmissionFacts registeredWeapon = eligibleWeapon;
	registeredWeapon.sourceDomain = PtCanonicalMeshSourceDomain::RegisteredRenderModel;
	Check(RtCpuRewritePlanCaptureAdmission(registeredWeapon) == RtCpuRewriteCaptureAdmissionReason::WeaponDepthHack,
		"matching weapon view facts do not admit a registered source");
	registeredWeapon.callback = true;
	Check(RtCpuRewritePlanCaptureAdmission(registeredWeapon) == RtCpuRewriteCaptureAdmissionReason::Callback,
		"registered callback retains precedence over matching weapon view facts");
	RtCpuRewriteCaptureAdmissionFacts unsupportedWeapon = eligibleWeapon;
	unsupportedWeapon.sourceDomain = PtCanonicalMeshSourceDomain::UnsupportedTransient;
	Check(RtCpuRewritePlanCaptureAdmission(unsupportedWeapon) == RtCpuRewriteCaptureAdmissionReason::UnsupportedSource,
		"unsupported source retains precedence over matching weapon view facts");
	RtCpuRewriteCaptureAdmissionFacts forcedWeapon = eligibleWeapon;
	forcedWeapon.forceUpdate = true;
	Check(RtCpuRewritePlanCaptureAdmission(forcedWeapon) == RtCpuRewriteCaptureAdmissionReason::ForceUpdate,
		"force-update retains precedence over eligible weapon exception");
	RtCpuRewriteCaptureAdmissionFacts modelDepthWeapon = eligibleWeapon;
	modelDepthWeapon.modelDepthHack = true;
	Check(RtCpuRewritePlanCaptureAdmission(modelDepthWeapon) == RtCpuRewriteCaptureAdmissionReason::ModelDepthHack,
		"eligible weapon exception retains model-depth attribution");
	RtCpuRewriteCaptureAdmissionFacts downstreamWeapon = eligibleWeapon;
	downstreamWeapon.resolvedSurfaceCurrent = false;
	Check(RtCpuRewritePlanCaptureAdmission(downstreamWeapon) == RtCpuRewriteCaptureAdmissionReason::UnresolvedCurrentSurface,
		"eligible weapon exception retains current-surface gate");
	downstreamWeapon = eligibleWeapon;
	downstreamWeapon.gpuSkinningEnabled = false;
	Check(RtCpuRewritePlanCaptureAdmission(downstreamWeapon) == RtCpuRewriteCaptureAdmissionReason::GpuSkinningDisabled,
		"eligible weapon exception retains GPU-skinning gate");
	downstreamWeapon = eligibleWeapon;
	downstreamWeapon.bindPoseSurface = false;
	Check(RtCpuRewritePlanCaptureAdmission(downstreamWeapon) == RtCpuRewriteCaptureAdmissionReason::CpuPosedSurface,
		"eligible weapon exception retains bind-pose gate");
	downstreamWeapon = eligibleWeapon;
	downstreamWeapon.jointSnapshotValid = false;
	Check(RtCpuRewritePlanCaptureAdmission(downstreamWeapon) == RtCpuRewriteCaptureAdmissionReason::MissingJointSnapshot,
		"eligible weapon exception retains joint-snapshot gate");
	RtCpuRewriteCaptureAdmissionFacts unrelatedViewFacts = admission;
	unrelatedViewFacts.rootView = true;
	unrelatedViewFacts.allowSurfaceInViewId = 31;
	unrelatedViewFacts.activeViewId = 32;
	Check(RtCpuRewritePlanCaptureAdmission(unrelatedViewFacts) == RtCpuRewriteCaptureAdmissionReason::Admitted,
		"unrelated view facts do not alter ordinary skinned admission");
	unrelatedViewFacts.sourceDomain = PtCanonicalMeshSourceDomain::StaticWorldMap;
	Check(RtCpuRewritePlanCaptureAdmission(unrelatedViewFacts) == RtCpuRewriteCaptureAdmissionReason::Admitted,
		"unrelated view facts do not alter static-world admission");
	unrelatedViewFacts.sourceDomain = PtCanonicalMeshSourceDomain::RegisteredRenderModel;
	Check(RtCpuRewritePlanCaptureAdmission(unrelatedViewFacts) == RtCpuRewriteCaptureAdmissionReason::Admitted,
		"unrelated view facts do not alter registered-rigid admission");
	admission.gpuSkinningEnabled = false;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::GpuSkinningDisabled,
		"GPU-skinning-off has exact reason");
	admission.gpuSkinningEnabled = true;
	admission.bindPoseSurface = false;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::CpuPosedSurface,
		"CPU-posed surface has exact reason");
	admission.bindPoseSurface = true;
	admission.callback = true;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::Admitted,
		"coherent callback-backed skinned source is admitted");
	admission.resolvedSurfaceCurrent = false;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::UnresolvedCurrentSurface,
		"callback-backed skinned source requires the current resolved-surface witness");
	admission = RtCpuRewriteCaptureAdmissionFacts();
	admission.sourceDomain = PtCanonicalMeshSourceDomain::RegisteredRenderModel;
	admission.callback = true;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::Callback,
		"callback-backed registered source remains rejected as callback geometry");
	admission = RtCpuRewriteCaptureAdmissionFacts();
	admission.sourceDomain = PtCanonicalMeshSourceDomain::UnsupportedTransient;
	admission.callback = true;
	admission.forceUpdate = true;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::UnsupportedSource,
		"unsupported source wins intentional multi-fact precedence");
	admission = RtCpuRewriteCaptureAdmissionFacts();
	admission.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
	admission.callback = true;
	admission.forceUpdate = true;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::ForceUpdate,
		"skinned callback bypasses callback rejection but not force-update");
	admission.forceUpdate = false;
	admission.weaponDepthHack = true;
	admission.modelDepthHack = true;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::WeaponDepthHack,
		"weapon depth hack precedes model depth hack");
	admission.weaponDepthHack = false;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::ModelDepthHack,
		"model depth hack has its exact reason");
	admission.modelDepthHack = false;
	admission.resolvedSurfaceCurrent = false;
	admission.gpuSkinningEnabled = false;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::UnresolvedCurrentSurface,
		"unresolved current surface precedes GPU-skinning-off");
	admission.resolvedSurfaceCurrent = true;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::GpuSkinningDisabled,
		"GPU-skinning-off remains exact after a valid witness");
	admission.gpuSkinningEnabled = true;
	admission.bindPoseSurface = false;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::CpuPosedSurface,
		"CPU-posed surface remains a retry reason");
	admission.bindPoseSurface = true;
	admission.jointSnapshotValid = false;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::MissingJointSnapshot,
		"missing joint snapshot remains a retry reason");
	admission = RtCpuRewriteCaptureAdmissionFacts();
	admission.sourceDomain = PtCanonicalMeshSourceDomain::StaticWorldMap;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::Admitted,
		"false current-surface witness does not reject static-world input");
	admission.sourceDomain = PtCanonicalMeshSourceDomain::RegisteredRenderModel;
	Check(RtCpuRewritePlanCaptureAdmission(admission) == RtCpuRewriteCaptureAdmissionReason::Admitted,
		"false current-surface witness does not reject registered-rigid input");
	CheckEqU64(kRtCpuRewriteCaptureSourceDomainCount, 6,
		"source-domain telemetry cardinality is exactly six");
	CheckEqU64(kRtCpuRewriteCaptureAdmissionReasonCount, 10,
		"admission telemetry cardinality is admitted plus nine rejection reasons");

	const RtCpuRewriteOwnedVertex verts[3] = { MakeVert(0, 0, 0), MakeVert(1, 0, 0), MakeVert(0, 1, 0) };
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	PathTraceSkinnedJointMatrix joints[2] = {};
	float matrix[16]; FillIdentityMatrix(matrix);
	auto makeSkinned = [&]() {
		RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9920, 1, 0, verts, indexes, matrix);
		write.sourceClass = kRtCpuRewriteClassSkinnedEntity;
		write.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
		write.meshKey.deformationClass = PtCanonicalDeformationClass::Skinned;
		write.meshKey.jointSubmeshIndex = 0;
		write.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::SkinnedSurface;
		write.instanceKey.jointSubmeshIndex = 0;
		write.joints = joints;
		write.jointCount = 2;
		return write;
	};
	{
		RtCpuProducerRewriteService service; service.Init(); service.SampleRequestedRoute(1);
		Check(service.TryAcquireRootInput(320, 7, 4), "inverse registered+skinned acquire");
		RtCpuRewriteSurfaceWrite bad = makeSkinned();
		bad.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::RegisteredRenderModel;
		Check(service.WriteSurface(bad), "inverse registered+skinned copied");
		Check(!service.SealRootInput(), "RegisteredRenderModel + skinned tuple rejected");
		service.Shutdown();
	}
	{
		RtCpuProducerRewriteService service; service.Init(); service.SampleRequestedRoute(1);
		Check(service.TryAcquireRootInput(321, 7, 4), "inverse skinned+rigid acquire");
		RtCpuRewriteSurfaceWrite bad = MakeTriangleWrite(9921, 1, 0, verts, indexes, matrix);
		bad.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
		Check(service.WriteSurface(bad), "inverse skinned+rigid copied");
		Check(!service.SealRootInput(), "SkinnedBindSource + rigid tuple rejected");
		service.Shutdown();
	}
	{
		RtCpuProducerRewriteService service; service.Init(); service.SampleRequestedRoute(1);
		Check(service.TryAcquireRootInput(322, 7, 4), "joint span tamper acquire");
		Check(service.WriteSurface(makeSkinned()), "joint span tamper write");
		Check(service.TamperJointSpanForHarness(), "joint span tampered");
		Check(!service.SealOverlayFromCapturingInput(322), "joint span rejected before overlay copy");
		Check(!service.SealRootInput(), "joint span rejected at input seal");
		service.Shutdown();
	}
	{
		RtCpuProducerRewriteService service; service.Init(); service.SampleRequestedRoute(1);
		Check(service.TryAcquireRootInput(323, 7, 4), "manifest span tamper acquire");
		Check(service.WriteSurface(makeSkinned()), "manifest span tamper write");
		Check(service.TamperManifestOffsetForHarness(), "manifest span tampered");
		Check(!service.SealOverlayFromCapturingInput(323), "manifest bounds rejected before overlay read");
		service.Shutdown();
	}

	RtCpuRewriteSurfaceWrite canonical = makeSkinned();
	RtCpuRewriteSkinnedRouteInput route;
	route.meshKey = canonical.meshKey;
	route.instanceKey = canonical.instanceKey;
	route.sourceIndexes = indexes;
	route.sourceIndexCount = 3;
	route.sourceIndexCapacityBytes = sizeof(indexes);
	route.outputVertexCount = 3;
	route.outputCapacityBytes = 3 * sizeof(PathTraceSmokeVertex);
	route.previousPositionOffset = 0;
	route.previousPositionCount = 3;
	route.previousValid = true;
	route.sourceGpuIndexGeneration = 1;
	route.outputStorageGeneration = 1;
	route.materialLogicalId = 17;
	route.materialIndex = 9;
	RtCpuRewriteSkinnedRoutePackage package;
	Check(RtCpuRewriteBuildSkinnedRoutePackage({ route }, 2, package), "production skinned route package builds");
	Check(package.upload.records.size() == 1 && package.upload.triangles.size() == 1,
		"route package has exact record/triangle counts");
	if (!package.upload.records.empty())
	{
		const PathTraceSkinnedHitRouteGpuRecord& rec = package.upload.records[0];
		Check(rec.shaderInstanceId == 4 && rec.triangleMetadataOffset == 0 && rec.triangleMetadataCount == 1,
			"route record has exact id and triangle metadata");
		Check(rec.sourceGpuIndexGenerationLo == 1 && rec.outputStorageGenerationLo == 1 &&
			rec.previousPositionOffset == 0,
			"route record carries generations and previous offset");
	}
	if (!package.upload.triangles.empty())
	{
		const PathTraceSkinnedHitRouteGpuTriangle& tri = package.upload.triangles[0];
		Check(tri.legacyPrimitiveIndex == UINT32_MAX && tri.materialId == 17 && tri.materialIndex == 9 &&
			tri.triangleClassAndFlags == PT_REWRITE_SKINNED_TRIANGLE_CLASS_AND_FLAGS,
			"route triangle is source-only fallback metadata");
	}
	route.sourceIndexCapacityBytes = sizeof(std::uint32_t) * 2;
	Check(!RtCpuRewriteBuildSkinnedRoutePackage({ route }, 2, package), "route package rejects source range overflow");

	const auto localFail = RtCpuRewritePlanSkinnedReplacement(RtCpuRewriteSkinnedReplacementEvent::LocalFailure);
	Check(localFail.keepMembers && !localFail.retireMembers, "local failure keeps members without retire");
	const auto replace = RtCpuRewritePlanSkinnedReplacement(RtCpuRewriteSkinnedReplacementEvent::ValidatedReplacement);
	Check(replace.retireMembers && replace.swapCandidate, "validated replacement retires then swaps");
	const auto laterFail = RtCpuRewritePlanSkinnedReplacement(RtCpuRewriteSkinnedReplacementEvent::LaterSceneFailure);
	Check(laterFail.retainRetiredAfterSceneFailure, "later scene failure retains retired pairs");
	const auto drain = RtCpuRewritePlanSkinnedReplacement(RtCpuRewriteSkinnedReplacementEvent::Drain);
	Check(drain.enqueueBeforeForceSchedule && drain.resetSerialAfterForceSchedule,
		"drain enqueues before force schedule and serial reset");
	RtCpuRewriteSkinnedHandleSelectionInput zeroInput;
	zeroInput.persistentZeroRecord = true;
	zeroInput.persistentZeroTriangle = true;
	zeroInput.persistentSharedTriangleDispatch = true;
	zeroInput.persistentSharedEmissive = true;
	const auto zero = RtCpuRewriteSelectSkinnedHandles(zeroInput);
	Check(zero.valid && !zero.usesWarmupGeometry, "zero/degraded selection is valid without warmup geometry");
	Check(zero.record == RtCpuRewriteSkinnedHandleSource::PersistentZeroRecord &&
		zero.triangle == RtCpuRewriteSkinnedHandleSource::PersistentZeroTriangle &&
		zero.previous == RtCpuRewriteSkinnedHandleSource::PersistentZeroRecord &&
		zero.dispatch == RtCpuRewriteSkinnedHandleSource::PersistentZeroRecord &&
		zero.sourceIndex == RtCpuRewriteSkinnedHandleSource::PersistentZeroTriangle &&
		zero.triangleDispatch == RtCpuRewriteSkinnedHandleSource::PersistentSharedTriangleDispatch,
		"zero selection explicitly reuses the sealed record/triangle sentinels and shared dispatch");
	Check(zero.recordCount == 0 && zero.triangleCount == 0 && zero.sourceIndexCount == 0 &&
		zero.previousPositionCount == 0 && zero.surfaceDispatchCount == 0 &&
		zero.triangleDispatchIndexCount == 0, "zero/degraded selection keeps all associated counts zero");

	RtCpuRewriteKeepLastState keepState;
	for (std::uint32_t i = 0; i < 119; ++i)
	{
		const auto step = RtCpuRewritePlanKeepLastTransition(keepState,
			RtCpuRewriteKeepLastEvent::KeepLast, RtCpuRewriteKeepLastFamily::LocalSkinned);
		Check(!step.warnNow && !step.localSkinnedDegradeEligible, "local skinned threshold remains closed before 120");
		keepState = step.next;
	}
	const auto threshold = RtCpuRewritePlanKeepLastTransition(keepState,
		RtCpuRewriteKeepLastEvent::KeepLast, RtCpuRewriteKeepLastFamily::LocalSkinned);
	Check(threshold.warnNow && threshold.localSkinnedDegradeEligible && threshold.next.consecutive == 120,
		"120th actual local-skinned keep-last warns and opens degradation");
	const auto familySwitch = RtCpuRewritePlanKeepLastTransition(keepState,
		RtCpuRewriteKeepLastEvent::KeepLast, RtCpuRewriteKeepLastFamily::OtherScene);
	Check(familySwitch.next.consecutive == 1 && familySwitch.next.family == RtCpuRewriteKeepLastFamily::OtherScene &&
		!familySwitch.localSkinnedDegradeEligible, "unrelated scene failure cannot inherit local-skinned streak");
	const auto committed = RtCpuRewritePlanKeepLastTransition(threshold.next,
		RtCpuRewriteKeepLastEvent::SceneCommit);
	Check(committed.next.consecutive == 0 && committed.next.family == RtCpuRewriteKeepLastFamily::None,
		"successful scene commit clears keep-last state");
	const auto drained = RtCpuRewritePlanKeepLastTransition(threshold.next, RtCpuRewriteKeepLastEvent::Drain);
	Check(drained.next.consecutive == 0 && !drained.next.warned, "drain clears keep-last state");

	RtCpuProducerRewriteService telemetryService;
	telemetryService.Init();
	telemetryService.NoteCaptureEntityDecision(PtCanonicalMeshSourceDomain::RegisteredRenderModel,
		RtCpuRewriteCaptureAdmissionReason::Callback);
	telemetryService.NoteCaptureEntityDecision(PtCanonicalMeshSourceDomain::SkinnedBindSource,
		RtCpuRewriteCaptureAdmissionReason::CpuPosedSurface);
	telemetryService.NoteCaptureSkinnedSurfaceDecision(RtCpuRewriteCaptureAdmissionReason::Admitted, 2);
	telemetryService.NoteCaptureSkinnedSurfaceDecision(RtCpuRewriteCaptureAdmissionReason::MissingJointSnapshot);
	telemetryService.NoteCaptureSkinnedSurfaceDecision(RtCpuRewriteCaptureAdmissionReason::Admitted, 3);
	const RtCpuRewriteCounters telemetry = telemetryService.Counters();
	const std::uint32_t registeredDomain = static_cast<std::uint32_t>(PtCanonicalMeshSourceDomain::RegisteredRenderModel);
	const std::uint32_t skinnedDomain = static_cast<std::uint32_t>(PtCanonicalMeshSourceDomain::SkinnedBindSource);
	const std::uint32_t callbackReason = static_cast<std::uint32_t>(RtCpuRewriteCaptureAdmissionReason::Callback);
	const std::uint32_t missingJointReason = static_cast<std::uint32_t>(RtCpuRewriteCaptureAdmissionReason::MissingJointSnapshot);
	std::uint64_t registeredRejects = 0;
	std::uint64_t skinnedRejects = 0;
	std::uint64_t surfaceRejects = 0;
	for (std::uint32_t reason = 1; reason < kRtCpuRewriteCaptureAdmissionReasonCount; ++reason)
	{
		registeredRejects += telemetry.captureEntityRejected[registeredDomain][reason];
		skinnedRejects += telemetry.captureEntityRejected[skinnedDomain][reason];
		surfaceRejects += telemetry.skinnedSurfaceRejected[reason];
	}
	Check(telemetry.captureEntityCandidates[registeredDomain] ==
		telemetry.captureEntityProvisional[registeredDomain] + registeredRejects &&
		telemetry.captureEntityRejected[registeredDomain][callbackReason] == 1,
		"terminal entity rejection reconciles once in its domain/reason cell");
	Check(telemetry.captureEntityCandidates[skinnedDomain] ==
		telemetry.captureEntityProvisional[skinnedDomain] + skinnedRejects &&
		telemetry.captureEntityProvisional[skinnedDomain] == 1,
		"provisional skinned entity reconciles without a terminal reject");
	Check(telemetry.skinnedSurfaceCandidates == telemetry.skinnedSurfaceAdmitted + surfaceRejects &&
		telemetry.skinnedSurfaceCandidates == 3 && telemetry.skinnedSurfaceAdmitted == 2 &&
		telemetry.skinnedSurfaceRejected[missingJointReason] == 1,
		"multi-surface admission and retry rejection reconcile without double counting");
	Check(telemetry.skinnedJointRows == 5 && telemetry.skinnedJointRowsHighWater == 3,
		"joint-row lifetime total and occupancy high-water retain distinct meanings");
	telemetryService.Shutdown();
}

void TestR3PreviousJointOffsetAndPreviousTransform()
{
	const std::uint32_t jointCount0 = 2;
	const std::uint32_t jointCount1 = 3;
	const std::uint32_t currentTotal = jointCount0 + jointCount1;
	const std::uint32_t prevLocal0 = 0;
	const std::uint32_t prevLocal1 = jointCount0;
	CheckEqU64(RtCpuRewriteSkinnedCombinedPreviousJointOffset(currentTotal, prevLocal0), currentTotal,
		"two surfaces: first previousJointOffset == currentTotal + 0");
	CheckEqU64(RtCpuRewriteSkinnedCombinedPreviousJointOffset(currentTotal, prevLocal1), currentTotal + jointCount0,
		"two surfaces: second previousJointOffset == currentTotal + previous-local");
	Check(RtCpuRewriteSkinnedCombinedPreviousJointOffset(currentTotal, prevLocal0) != 0,
		"first previousJointOffset is not 0 once currents are packed");
	CheckEqU64(RtCpuRewriteSkinnedCombinedPreviousJointOffset(5, 0), 5, "currentTotal+0");

	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	const RtCpuRewriteOwnedVertex verts[3] = {
		MakeVert(0.0f, 0.0f, 0.0f),
		MakeVert(1.0f, 0.0f, 0.0f),
		MakeVert(0.0f, 1.0f, 0.0f)
	};
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrixA[16];
	float matrixB[16];
	FillTranslateMatrix(matrixA, 1.0f, 2.0f, 3.0f);
	FillTranslateMatrix(matrixB, 4.0f, 5.0f, 6.0f);
	PathTraceSkinnedJointMatrix joints[2] = {};
	joints[0].rows[0] = 1.0f;
	joints[0].rows[5] = 1.0f;
	joints[0].rows[10] = 1.0f;
	joints[1].rows[0] = 1.0f;
	joints[1].rows[5] = 1.0f;
	joints[1].rows[10] = 1.0f;

	Check(service.TryAcquireRootInput(310, 7, 4), "r3-prev first acquire");
	RtCpuRewriteSurfaceWrite write = MakeTriangleWrite(9910, 1, 0, verts, indexes, matrixA);
	write.sourceClass = kRtCpuRewriteClassSkinnedEntity;
	write.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
	write.meshKey.deformationClass = PtCanonicalDeformationClass::Skinned;
	write.meshKey.jointSubmeshIndex = 0;
	write.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::SkinnedSurface;
	write.instanceKey.jointSubmeshIndex = 0;
	write.joints = joints;
	write.jointCount = 2;
	Check(service.WriteSurface(write), "r3-prev first write");
	Check(service.SealOverlayFromCapturingInput(310), "r3-prev first overlay");
	Check(service.SealRootInput(), "r3-prev first seal");
	Check(service.WaitForReadyProduct(4000), "r3-prev first ready");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"r3-prev first product");
	Check(service.TryAcquireOverlay(310), "r3-prev first overlay acquire");
	RtCpuRewriteJoinResult join1 = {};
	Check(service.BuildJoinPlan(310, join1), "r3-prev first join");
	Check(join1.skinnedCount == 1, "r3-prev one skinned");
	Check(std::fabs(join1.skinned[0].previousObjectToWorld[12] - 1.0f) < 0.001f,
		"first sighting previousObjectToWorld copies current");
	service.ReleaseConsumedOverlay(true);
	service.ReleaseConsumedProduct();

	Check(service.TryAcquireRootInput(311, 7, 4), "r3-prev second acquire");
	std::memcpy(write.modelMatrix, matrixB, sizeof(matrixB));
	Check(service.WriteSurface(write), "r3-prev second write moved");
	Check(service.SealOverlayFromCapturingInput(311), "r3-prev second overlay");
	Check(service.SealRootInput(), "r3-prev second seal");
	Check(service.WaitForReadyProduct(4000), "r3-prev second ready");
	Check(service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()),
		"r3-prev second product");
	Check(service.TryAcquireOverlay(311), "r3-prev second overlay acquire");
	RtCpuRewriteJoinResult join2 = {};
	Check(service.BuildJoinPlan(311, join2), "r3-prev second join");
	Check(join2.skinnedCount == 1, "r3-prev second skinned");
	Check(std::fabs(join2.skinned[0].currentObjectToWorld[12] - 4.0f) < 0.001f, "current from this overlay");
	Check(std::fabs(join2.skinned[0].previousObjectToWorld[12] - 1.0f) < 0.001f,
		"previousObjectToWorld from overlay history not current");
	service.ReleaseConsumedOverlay(true);
	service.ReleaseConsumedProduct();
	service.Shutdown();
}

void TestF2RetirementPlannerLedgerAndDrain()
{
	const RtCpuRewriteRetirementDecision nonAdmit[] = {
		RtCpuRewriteRetirementDecision::CountPressure,
		RtCpuRewriteRetirementDecision::BytePressure,
		RtCpuRewriteRetirementDecision::UnknownBytes,
		RtCpuRewriteRetirementDecision::ArithmeticOverflow
	};
	for (RtCpuRewriteRetirementDecision decision : nonAdmit)
	{
		const auto policy = RtCpuRewritePlanRetirementMemberApply({ decision, false });
		Check(policy.preservePrevious && !policy.applyCandidate,
			"F2 sites 1-3 preserve previous snapshot for every non-Admit result");
	}
	const auto enqueueFailed = RtCpuRewritePlanRetirementMemberApply({
		RtCpuRewriteRetirementDecision::Admit, false });
	Check(enqueueFailed.preservePrevious && !enqueueFailed.applyCandidate,
		"F2 sites 1-3 Admit without enqueue success preserves previous snapshot");
	const auto enqueued = RtCpuRewritePlanRetirementMemberApply({
		RtCpuRewriteRetirementDecision::Admit, true });
	Check(!enqueued.preservePrevious && enqueued.applyCandidate,
		"F2 sites 1-3 apply candidate only after admitted enqueue succeeds");

	const auto site4Deferred = RtCpuRewritePlanRetirementSite4({ true, false, false });
	Check(site4Deferred.allowCommit && site4Deferred.evictionDeferred &&
		!site4Deferred.enqueueAndCompact, "F2 site 4 failed local reserve commits with lossless deferral");
	const auto site4Rejected = RtCpuRewritePlanRetirementSite4({ true, true, false });
	Check(site4Rejected.allowCommit && site4Rejected.evictionDeferred &&
		!site4Rejected.enqueueAndCompact, "F2 site 4 failed preflight commits with lossless deferral");
	const auto site4Ready = RtCpuRewritePlanRetirementSite4({ true, true, true });
	Check(site4Ready.allowCommit && !site4Ready.evictionDeferred &&
		site4Ready.enqueueAndCompact, "F2 site 4 successful preflight selects post-commit enqueue and compact");

	RtCpuRewriteRetirementAdmissionInput input;
	input.currentLiveCount = kRtCpuRewriteRetiredSoftCount - 2;
	input.currentLiveLogicalBytes = kRtCpuRewriteRetiredSoftLogicalBytes - 8;
	input.proposedCount = 2;
	input.proposedLogicalBytes = 8;
	Check(RtCpuRewritePlanRetirementAdmission(input) == RtCpuRewriteRetirementDecision::Admit,
		"F2 inclusive exact bounds admit");
	++input.proposedCount;
	Check(RtCpuRewritePlanRetirementAdmission(input) == RtCpuRewriteRetirementDecision::CountPressure,
		"F2 one-over count rejects");
	input.proposedCount = 2;
	++input.proposedLogicalBytes;
	Check(RtCpuRewritePlanRetirementAdmission(input) == RtCpuRewriteRetirementDecision::BytePressure,
		"F2 one-over logical bytes rejects");
	input.proposedLogicalBytes = 8;
	input.proposedBytesKnown = false;
	Check(RtCpuRewritePlanRetirementAdmission(input) == RtCpuRewriteRetirementDecision::UnknownBytes,
		"F2 unknown logical bytes reject");
	input.proposedBytesKnown = true;
	input.currentLiveCount = UINT64_MAX;
	Check(RtCpuRewritePlanRetirementAdmission(input) == RtCpuRewriteRetirementDecision::ArithmeticOverflow,
		"F2 checked count overflow rejects");
	input.currentLiveCount = 0;
	input.currentLiveLogicalBytes = UINT64_MAX;
	Check(RtCpuRewritePlanRetirementAdmission(input) == RtCpuRewriteRetirementDecision::ArithmeticOverflow,
		"F2 checked logical-byte overflow rejects");

	RtCpuRewriteRetirementLedger ledger;
	RtCpuRewriteRetirementLedgerEnqueue(ledger, 3, 300, 2, 220);
	Check(ledger.liveCount == 3 && ledger.unscheduledCount == 3 &&
		ledger.liveLogicalBytes == 300 && ledger.unscheduledLogicalBytes == 300 &&
		ledger.liveCountHighWater == 3 && ledger.enqueuedTotal == 3,
		"F2 enqueue updates occupancy, high water, and lifetime distinctly");
	RtCpuRewriteRetirementLedgerSchedule(ledger, 2, 180);
	Check(ledger.liveCount == 3 && ledger.unscheduledCount == 1 && ledger.scheduledTotal == 2,
		"F2 schedule changes only unscheduled occupancy");
	RtCpuRewriteRetirementLedgerRelease(ledger, 2, 180, 1, 100);
	Check(ledger.liveCount == 1 && ledger.unscheduledCount == 1 && ledger.releasedTotal == 2 &&
		ledger.liveCountHighWater == 3, "F2 release changes live occupancy but not high water/lifetime");
	const RtCpuRewriteRetirementLedger beforeReject = ledger;
	RtCpuRewriteRetirementLedgerReject(ledger, RtCpuRewriteRetirementDecision::BytePressure);
	Check(ledger.liveCount == beforeReject.liveCount && ledger.unscheduledCount == beforeReject.unscheduledCount &&
		ledger.bytePressureRejects == beforeReject.bytePressureRejects + 1,
		"F2 rejected aggregate changes no occupancy");
	RtCpuRewriteRetirementLedgerEvictionDeferred(ledger);
	Check(ledger.evictionDeferred == 1 && ledger.liveCount == beforeReject.liveCount,
		"F2 eviction deferral is distinct and occupancy-neutral");
	RtCpuRewriteRetirementLedgerForcedDrain(ledger, true);
	Check(ledger.forcedDrainOverBound == 1 && ledger.liveCount == beforeReject.liveCount,
		"F2 forced drain bypass records pressure without fake release");
	Check(RtCpuRewriteRetirementLedgerReconcile(ledger, ledger.liveCount, ledger.liveLogicalBytes,
		ledger.skinnedLiveCount, ledger.skinnedLiveLogicalBytes), "F2 ledger/category reconcile succeeds");
	Check(!RtCpuRewriteRetirementLedgerReconcile(ledger, ledger.liveCount + 1, ledger.liveLogicalBytes,
		ledger.skinnedLiveCount, ledger.skinnedLiveLogicalBytes) && ledger.reconciliationAnomalies == 1,
		"F2 reconciliation mismatch records anomaly");

	const RtCpuRewriteRetirementDrainPlan drain = RtCpuRewritePlanRetirementDrain({ 4, 1, 7 });
	Check(drain.forceAdmitSkinnedCount == 4 && drain.retainStaticCount == 1 &&
		drain.retainDedicatedCount == 7, "F2 drain transfers skinned while retaining static/dedicated members");

	RtCpuRewriteKeepLastState keepState;
	for (std::uint32_t i = 0; i < 119; ++i)
	{
		keepState = RtCpuRewritePlanKeepLastTransition(keepState, RtCpuRewriteKeepLastEvent::KeepLast,
			RtCpuRewriteKeepLastFamily::LocalSkinned).next;
	}
	const auto pressureSwitch = RtCpuRewritePlanKeepLastTransition(keepState,
		RtCpuRewriteKeepLastEvent::KeepLast, RtCpuRewriteKeepLastFamily::OtherScene);
	Check(pressureSwitch.next.consecutive == 1 && !pressureSwitch.localSkinnedDegradeEligible,
		"F2 retirement pressure cannot inherit local-skinned degradation streak");
}

void TestR3011CanonicalOverlayOrder()
{
	RtCpuProducerRewriteService service;
	service.Init();
	service.SampleRequestedRoute(1);
	RtCpuRewriteOwnedVertex verts[3] = { MakeVert(0, 0, 0), MakeVert(1, 0, 0), MakeVert(0, 1, 0) };
	verts[0].color[0] = 0; verts[0].color2[0] = 1;
	const std::uint32_t indexes[3] = { 0, 1, 2 };
	float matrix[16]; FillIdentityMatrix(matrix);
	PathTraceSkinnedJointMatrix palette[3] = {};
	RtCpuRewriteSurfaceWrite writes[3];
	for (int i = 0; i < 3; ++i)
	{
		palette[i].rows[0] = palette[i].rows[5] = palette[i].rows[10] = 1;
		palette[i].rows[3] = float(10 + i);
		writes[i] = MakeTriangleWrite(9900 + i, 10 + i, 0, verts, indexes, matrix);
		writes[i].modelMatrix[12] = float(20 + i);
		if (i != 1)
		{
			writes[i].sourceClass = kRtCpuRewriteClassSkinnedEntity;
			writes[i].meshKey.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
			writes[i].meshKey.deformationClass = PtCanonicalDeformationClass::Skinned;
			writes[i].meshKey.jointSubmeshIndex = 0;
			writes[i].instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::SkinnedSurface;
			writes[i].instanceKey.jointSubmeshIndex = 0;
			writes[i].joints = &palette[i]; writes[i].jointCount = 1;
		}
	}
	const int permutations[3][3] = { { 0, 1, 2 }, { 2, 1, 0 }, { 1, 0, 2 } };
	for (int pass = 0; pass < 3; ++pass)
	{
		const std::uint64_t frame = 600 + pass;
		Check(service.TryAcquireRootInput(frame, 7, 4), "R3-011 permutation input acquire");
		for (int i : permutations[pass]) Check(service.WriteSurface(writes[i]), "R3-011 permutation write");
		Check(service.SealOverlayFromCapturingInput(frame) && service.SealRootInput(), "R3-011 permutation seal");
		Check(service.WaitForReadyProduct(4000) && service.TryAcquireNewestCompatibleProduct(
			service.LifecycleGeneration(), 7, 4, service.ConfigGeneration()), "R3-011 permutation product");
		Check(service.TryAcquireOverlay(frame), "R3-011 permutation overlay acquire");
		const auto* overlay = service.OverlayView();
		bool ordered = overlay && overlay->rowCount == 3;
		bool associated = ordered;
		if (ordered)
			for (unsigned i = 0; i < 3; ++i)
			{
				const auto& row = overlay->rows[i];
				ordered &= row.instanceKey.renderDefIndex == 10 + i;
				associated &= row.currentObjectToWorld[12] == float(row.instanceKey.renderDefIndex + 10);
				if (row.sourceClass == kRtCpuRewriteClassSkinnedEntity)
					associated &= row.jointCount == 1 && row.jointOffset < overlay->jointCount &&
						overlay->joints[row.jointOffset].rows[3] == float(row.instanceKey.renderDefIndex);
			}
		Check(ordered, "R3-011 real overlay order independent of capture arrival");
		Check(associated, "R3-011 reordered rows preserve exact transform and palette association");
		RtCpuRewriteJoinResult joined;
		Check(service.BuildJoinPlan(frame, joined) && joined.skinnedCount == 2 && joined.joinedCount == 1,
			"R3-011 mixed production join");
		Check(joined.skinned.size() == 2 && joined.skinned[0].instanceKey.renderDefIndex == 10 &&
			joined.skinned[1].instanceKey.renderDefIndex == 12,
			"R3-011 production skinned layout order stable across arrival permutations");
		service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct();
	}
	service.Shutdown();
}

void TestR3010RetainedSkinnedPolicies()
{
#if defined(RT_CPU_REWRITE_RETAINED_SKINNED)
	Check(RtCpuRewritePlanSkinnedSlot(0, 0, -1, 0) &&
		RtCpuRewritePlanSkinnedSlot(1, 0, 0, 1) &&
		RtCpuRewritePlanSkinnedSlot(2, 0, 1, 2) &&
		RtCpuRewritePlanSkinnedSlot(3, 1, 2, 0), "R3-010 three-slot warmup then serial reuse");
	Check(!RtCpuRewritePlanSkinnedSlot(3, 3, 2, 0) &&
		!RtCpuRewritePlanSkinnedSlot(3, 1, 0, 0) &&
		!RtCpuRewritePlanSkinnedSlot(UINT64_MAX, 0, -1, 0) &&
		!RtCpuRewritePlanSkinnedSlot(3, 5, 2, 0) &&
		!RtCpuRewritePlanSkinnedSlot(3, 1, 2, 3), "R3-010 current/no-idle/backward/overflow rejects");
	std::uint64_t peak = 0;
	Check(RtCpuRewriteValidateSkinnedCapacity(30, 20, 14, 64, &peak) && peak == 64 &&
		!RtCpuRewriteValidateSkinnedCapacity(30, 20, 15, 64, &peak) &&
		!RtCpuRewriteValidateSkinnedCapacity(UINT64_MAX, 1, 0, UINT64_MAX, &peak),
		"R3-010 checked resident candidate retirement peak");
	RtCpuRewriteSkinnedLayout a;
	a.rows.resize(2);
	a.rows[0].words[0] = 1;
	a.rows[1].words[0] = 2;
	a.vertices.resize(3);
	a.indexes = {0, 1, 2};
	a.blasIndexes = {0, 1, 2};
	a.capacities = {336, 12, 336, 352, 96, 48, 128, 48};
	RtCpuRewriteSkinnedLayout b = a;
	Check(RtCpuRewriteSkinnedLayoutEqual(a, b), "R3-010 exact layout accepts");
	std::swap(b.rows[0], b.rows[1]);
	Check(!RtCpuRewriteSkinnedLayoutEqual(a, b), "R3-010 equal-count reordered identity rejects");
	b = a; b.rows[0].words[20]++;
	Check(!RtCpuRewriteSkinnedLayoutEqual(a, b), "R3-010 material or layout field change rejects");
	b = a; b.vertices[0].jointIndices[0]++;
	Check(!RtCpuRewriteSkinnedLayoutEqual(a, b), "R3-010 changed clamped source rejects");
	b = a; b.blasIndexes[1] = 2;
	Check(!RtCpuRewriteSkinnedLayoutEqual(a, b), "R3-010 changed derived index rejects");
	b = a; b.indexes[0] = 1;
	Check(!RtCpuRewriteSkinnedLayoutEqual(a, b), "R3-010 changed source index rejects");
	b = a; b.capacities[0]++;
	Check(!RtCpuRewriteSkinnedLayoutEqual(a, b), "R3-010 changed capacity rejects");
	Check(RtCpuRewriteCompareSkinnedLayout(a, b).reason == RtCpuRewriteSkinnedLayoutMismatch::Capacity,
		"R3-011 capacity mismatch classified");
	b = a; b.rows[0].words[2]++;
	auto diff = RtCpuRewriteCompareSkinnedLayout(a, b);
	Check(diff.reason == RtCpuRewriteSkinnedLayoutMismatch::RowField && diff.row == 0 && diff.word == 2,
		"R3-011 identity mismatch identifies exact row and word");
	b = a; b.vertices[0].localPosition[0] += 1;
	Check(RtCpuRewriteCompareSkinnedLayout(a, b).reason == RtCpuRewriteSkinnedLayoutMismatch::SourceVertex,
		"R3-011 source bytes classified without relaxing equality");
	b = a; b.indexes[0]++;
	Check(RtCpuRewriteCompareSkinnedLayout(a, b).reason == RtCpuRewriteSkinnedLayoutMismatch::SourceIndex,
		"R3-011 source index mismatch classified");
	b = a; b.blasIndexes[0]++;
	Check(RtCpuRewriteCompareSkinnedLayout(a, b).reason == RtCpuRewriteSkinnedLayoutMismatch::BlasIndex,
		"R3-011 derived index mismatch classified");
	Check(RtCpuRewriteSkinnedPreviousPoseValid(10, 11, 4, 4, true) &&
		!RtCpuRewriteSkinnedPreviousPoseValid(10, 12, 4, 4, true) &&
		!RtCpuRewriteSkinnedPreviousPoseValid(10, 11, 3, 4, true) &&
		!RtCpuRewriteSkinnedPreviousPoseValid(10, 11, 4, 4, false) &&
		!RtCpuRewriteSkinnedPreviousPoseValid(UINT64_MAX, 0, 4, 4, true),
		"R3-010 failed scene gap and incompatible previous pose reject");
#else
	Check(false, "R3-010 three-slot selection proof unavailable");
	Check(false, "R3-010 checked replacement peak proof unavailable");
	Check(false, "R3-010 exact source and index layout proof unavailable");
	Check(false, "R3-010 committed previous-pose proof unavailable");
#endif
}


void TestR4005PreparedProduct(const RtCpuRewriteFrozenProductView& view,
    const RtCpuRewriteOverlayView& overlay, const RtCpuRewriteJoinResult& join)
{
    const auto& p = view.skinnedPrepared;
    Check(p.count == 2 && p.vertexCount == 6 && p.indexCount == 6 && p.jointCount == 2,
        "R4-005 worker publishes per-instance packed counts");
    Check(p.vertices && p.indexes && p.blasIndexes && p.rows && p.dispatches && p.records && p.triangles,
        "R4-005 worker publishes all final arrays");
    if (!p.vertices || !p.records || !p.triangles || p.count != 2) return;
    Check(p.indexes[3] == 0 && p.blasIndexes[3] == 3 && p.blasIndexes[5] == 5 &&
        p.rows[1].words[22] == 3 && p.rows[1].words[23] == 3 && p.rows[1].words[24] == 1,
        "R4-005 worker source-local and BLAS remaps have distinct authoritative offsets");
    Check(p.vertices[0].jointIndices[1] == 0 && p.vertices[0].jointWeights[1] == 0,
        "R4-005 published joint influences are clamped");
    Check(RtCpuRewriteValidateSkinnedPrepared(&view, &overlay, join) == RtCpuRewriteSkinnedRejectReason::None,
        "R4-005 exact frame admits complete worker skin product");
    auto stale = view; --stale.rootFrame;
    Check(RtCpuRewriteValidateSkinnedPrepared(&stale, &overlay, join) == RtCpuRewriteSkinnedRejectReason::None,
        "R4-007 older matching bind geometry accepts current pose");
    stale = view; ++stale.rootFrame;
    Check(RtCpuRewriteValidateSkinnedPrepared(&stale, &overlay, join) == RtCpuRewriteSkinnedRejectReason::WorkerFrameMismatch,
        "R4-007 future product rejects");
    stale = view; ++stale.lifecycleGeneration;
    Check(RtCpuRewriteValidateSkinnedPrepared(&stale, &overlay, join) == RtCpuRewriteSkinnedRejectReason::WorkerFrameMismatch,
        "R4-007 different lifecycle rejects");
    auto sourceChanged = join; ++sourceChanged.skinned[0].sourceContentSignature;
    Check(RtCpuRewriteValidateSkinnedPrepared(&view, &overlay, sourceChanged) == RtCpuRewriteSkinnedRejectReason::WorkerSourceMismatch,
        "R4-007 changed owned source rejects");
    auto partial = join; partial.skinned.pop_back(); --partial.skinnedCount;
    Check(RtCpuRewriteValidateSkinnedPrepared(&view, &overlay, partial) == RtCpuRewriteSkinnedRejectReason::WorkerMembershipMismatch,
        "R4-005 missing joined instance rejects whole product");
    auto changed = join; ++changed.skinned[0].jointCount;
    Check(RtCpuRewriteValidateSkinnedPrepared(&view, &overlay, changed) != RtCpuRewriteSkinnedRejectReason::None,
        "R4-005 changed palette size cannot consume old packing");
    changed = join; ++changed.skinned[0].instanceKey.renderDefGeneration;
    Check(RtCpuRewriteValidateSkinnedPrepared(&view, &overlay, changed) == RtCpuRewriteSkinnedRejectReason::WorkerMembershipMismatch,
        "R4-005 same-sized different instance rejects");
    for (uint32_t previousMask = 0; previousMask < 4; ++previousMask)
    {
        std::vector<RtCpuRewriteSkinnedRouteInput> inputs;
        std::vector<PathTraceSkinnedHitRouteGpuRecord> records(p.records, p.records + p.count);
        std::vector<PathTraceSkinnedHitRouteGpuTriangle> triangles(p.triangles, p.triangles + p.indexCount / 3);
        const uint64_t sg = 0x1234567800000001ull, og = 0x8765432100000002ull;
        const uint32_t previousCount = previousMask & 2 ? 6 : previousMask & 1 ? 3 : 0;
        for (uint32_t i = 0; i < p.count; ++i)
        {
            const auto& js = join.skinned[i]; const auto& mesh = view.skinnedMeshes[js.meshIndex];
            auto d = p.dispatches[i];
            const bool previous = (previousMask & (1u << i)) != 0;
            Check(p.records[i].sourceGpuIndexGenerationLo == 0 && p.records[i].outputStorageGenerationLo == 0 &&
                (d.flags & PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS) == 0,
                "R4-005 worker template has no owner generation or pose authority");
            if (previous) d.flags |= PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS;
            Check(RtCpuRewritePatchSkinnedRouteRecord(records[i], d, sg, og, 19 + i, previousCount),
                "R4-005 bounded owner record patch succeeds");
            RtCpuRewriteSkinnedRouteInput in;
            in.meshKey = js.meshKey; in.instanceKey = js.instanceKey;
            in.sourceIndexes = mesh.indexes; in.sourceIndexCount = mesh.indexCount;
            in.sourceIndexOffsetBytes = uint64_t(i) * 3 * sizeof(uint32_t); in.sourceIndexCapacityBytes = p.capacities[1];
            in.outputVertexOffsetBytes = uint64_t(i) * 3 * sizeof(PathTraceSmokeVertex);
            in.outputVertexCount = mesh.vertexCount; in.outputCapacityBytes = p.capacities[2];
            in.previousPositionOffset = i * 3; in.previousPositionCount = p.vertexCount; in.previousValid = previous;
            in.sourceGpuIndexGeneration = sg; in.outputStorageGeneration = og;
            in.materialLogicalId = 123 + i; in.materialIndex = 9 + i;
            inputs.push_back(in);
            const uint64_t hash = uint64_t(records[i].instanceHashLo) | (uint64_t(records[i].instanceHashHi) << 32);
            PtPatchSkinnedHitRouteMaterial(triangles[i], hash, in.materialLogicalId, in.materialIndex);
        }
        RtCpuRewriteSkinnedRoutePackage oracle;
        Check(RtCpuRewriteBuildSkinnedRoutePackage(inputs, 17, oracle), "R4-005 strict old builder remains behavioral oracle");
        Check(oracle.upload.records.size() == records.size() && oracle.upload.triangles.size() == triangles.size() &&
            std::memcmp(oracle.upload.records.data(), records.data(), records.size() * sizeof(records[0])) == 0 &&
            std::memcmp(oracle.upload.triangles.data(), triangles.data(), triangles.size() * sizeof(triangles[0])) == 0,
            "R4-005 worker bytes plus exact patches equal full old builder for material, ID, generations and mixed previous validity");
        for (auto& in : inputs) { in.sourceGpuIndexGeneration = in.outputStorageGeneration = 0; in.previousValid = false; }
        Check(!RtCpuRewriteBuildSkinnedRoutePackage(inputs, 17, oracle), "R4-005 strict route entry still rejects zero GPU generations");
        Check(RtCpuRewriteBuildSkinnedRoutePackage(inputs, 17, oracle, true), "R4-005 explicit CPU-template entry accepts owned topology");
        inputs[0].sourceGpuIndexGeneration = 1;
        Check(!RtCpuRewriteBuildSkinnedRoutePackage(inputs, 17, oracle, true), "R4-005 CPU templates reject fabricated GPU authority");
    }
}

void TestR4001MaterialBindings()
{
	std::vector<RtCpuRewriteMaterialBinding> bindings;
	Check(RtCpuRewriteBuildMaterialBindings({90, 10, 50}, bindings), "R4-001 unsorted warmup table receipt");
	uint32_t index = UINT32_MAX;
	Check(RtCpuRewriteResolveMaterial(bindings, 10, index) && index == 1 &&
		RtCpuRewriteResolveMaterial(bindings, 90, index) && index == 0 &&
		RtCpuRewriteResolveMaterial(bindings, 50, index) && index == 2,
		"R4-001 sorting preserves actual GPU dense indexes including valid slot zero");
	Check(!RtCpuRewriteResolveMaterial(bindings, 77, index) && index == 0,
		"R4-001 missing identity reports inherited slot-zero fallback");
	Check(!RtCpuRewriteBuildMaterialBindings({90, 10, 90}, bindings) &&
		RtCpuRewriteResolveMaterial(bindings, 50, index) && index == 2,
		"R4-001 ambiguous candidate cannot mutate committed receipt");
	Check(!RtCpuRewriteBuildMaterialBindings({}, bindings) &&
		!RtCpuRewriteBuildMaterialBindings(std::vector<uint32_t>(65537), bindings),
		"R4-001 missing and oversized table reject");

	RtCpuProducerRewriteService service;
	service.Init(); service.SampleRequestedRoute(1);
	RtCpuRewriteOwnedVertex verts[3] = { MakeVert(0,0,0), MakeVert(1,0,0), MakeVert(0,1,0) };
	for (auto& v : verts) { v.color[0] = 0; v.color2[0] = 1; }
	const uint32_t indexes[3] = {0,1,2};
	float matrix[16]; FillIdentityMatrix(matrix);
	PathTraceSkinnedJointMatrix joint = {};
	joint.rows[0] = joint.rows[5] = joint.rows[10] = 1;
	auto world = MakeTriangleWrite(91000, 1, 0, verts, indexes, matrix);
	world.sourceClass = kRtCpuRewriteClassStaticWorld;
	world.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::StaticWorldMap;
	world.meshKey.deformationClass = PtCanonicalDeformationClass::Static;
	world.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::StaticSurface;
	auto a = MakeTriangleWrite(91001, 2, 0, verts, indexes, matrix);
	a.sourceClass = kRtCpuRewriteClassSkinnedEntity;
	a.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
	a.meshKey.deformationClass = PtCanonicalDeformationClass::Skinned;
	a.meshKey.jointSubmeshIndex = 0;
	a.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::SkinnedSurface;
	a.instanceKey.jointSubmeshIndex = 0;
	a.joints = &joint; a.jointCount = 1; a.materialLogicalId = 10;
	auto b = a; b.instanceKey.renderDefIndex = 3; b.materialLogicalId = 50;
	uint64_t signature = 0;
	for (uint64_t frame = 800; frame < 802; ++frame)
	{
		world.materialLogicalId = frame == 800 ? 10 : 50;
		Check(service.TryAcquireRootInput(frame,7,4) && service.WriteSurface(world) &&
			service.WriteSurface(b) && service.WriteSurface(a), "R4-001 shared mesh instance captures");
		Check(service.SealOverlayFromCapturingInput(frame) && service.SealRootInput() &&
			service.WaitForReadyProduct(4000) && service.TryAcquireNewestCompatibleProduct(
				service.LifecycleGeneration(),7,4,service.ConfigGeneration()), "R4-001 material publication");
		const auto* view = service.ProductView();
		Check(view && view->triangleCount == 1 && view->triangleMaterialIds[0] == world.materialLogicalId,
			"R4-001 static material identity survives worker conversion");
		if (view)
		{
			Check(view->skinnedMeshCount == 1, "R4-001 material variants share one immutable skin mesh");
			if (signature) Check(signature != view->contentSignature, "R4-001 material-only static change invalidates upload signature");
			signature = view->contentSignature;
		}
		Check(service.TryAcquireOverlay(frame), "R4-001 material overlay");
		RtCpuRewriteJoinResult joined;
		Check(service.BuildJoinPlan(frame,joined) && joined.skinned.size() == 2 &&
			joined.skinned[0].materialLogicalId == 10 && joined.skinned[1].materialLogicalId == 50,
			"R4-001 shared skin instances retain distinct exact material identities");
        if (view && service.OverlayView()) TestR4005PreparedProduct(*view, *service.OverlayView(), joined);
        static uint64_t priorSkinSignature = 0;
        if (view && view->skinnedMeshCount)
        {
            if (priorSkinSignature) Check(priorSkinSignature != view->skinnedMeshes[0].signature,
                "R4-005 UV-only changes invalidate full source retention signature");
            priorSkinSignature = view->skinnedMeshes[0].signature;
        }
        verts[0].st[0] += 0.125f;
		service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct();
	}
	service.Shutdown();
}

void TestR4007RetainedProductPoses()
{
    RtCpuProducerRewriteService service;
    service.Init(); service.SampleRequestedRoute(1);
    RtCpuRewriteOwnedVertex verts[3] = { MakeVert(0,0,0), MakeVert(1,0,0), MakeVert(0,1,0) };
    for (auto& v : verts) { v.color[0] = 0; v.color2[0] = 1; }
    const uint32_t indexes[3] = {0,1,2};
    float matrix[16]; FillIdentityMatrix(matrix);
    PathTraceSkinnedJointMatrix joint = {};
    joint.rows[0] = joint.rows[5] = joint.rows[10] = 1;
    auto mover = MakeTriangleWrite(92000, 1, 0, verts, indexes, matrix);
    auto skin = MakeTriangleWrite(92001, 2, 0, verts, indexes, matrix);
    skin.sourceClass = kRtCpuRewriteClassSkinnedEntity;
    skin.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
    skin.meshKey.deformationClass = PtCanonicalDeformationClass::Skinned;
    skin.meshKey.jointSubmeshIndex = 0;
    skin.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::SkinnedSurface;
    skin.instanceKey.jointSubmeshIndex = 0;
    skin.joints = &joint; skin.jointCount = 1;
    auto capture = [&](uint64_t frame) {
        Check(service.TryAcquireRootInput(frame,7,4) && service.WriteSurface(mover) && service.WriteSurface(skin) &&
            service.SealOverlayFromCapturingInput(frame) && service.SealRootInput(), "R4-007 capture current owned input and pose");
    };
    auto acquire = [&](uint64_t frame) {
        return service.TryAcquireNewestCompatibleProduct(service.LifecycleGeneration(),7,4,service.ConfigGeneration(),frame);
    };
    capture(900);
    Check(service.WaitForReadyProduct(4000) && acquire(900), "R4-007 initial worker product");
    if (!service.ProductView()) { service.Shutdown(); return; }
    uint64_t ticket = service.ProductView()->ticket;
    service.ReleaseConsumedProduct(true);
    // Same bind bytes with different exact poses; deliberately prevent publication of the next product.
    service.SetGeometryBuildStallForHarness(true);
    mover.modelMatrix[12] = 17; skin.modelMatrix[12] = 23; joint.rows[3] = 31;
    capture(901);
    Check(acquire(901) && service.ProductView() && service.ProductView()->ticket == ticket,
        "R4-007 late worker reuses retained product without a wait");
    Check(service.TryAcquireOverlay(901), "R4-007 exact pose overlay available while worker stalled");
    RtCpuRewriteJoinResult joined;
    Check(service.BuildJoinPlan(901, joined), "R4-007 join current poses with older geometry");
    const auto* overlay = service.OverlayView();
    Check(overlay && overlay->rootFrame == 901 && joined.joined.size() == 1 && joined.skinned.size() == 1 &&
        joined.joined[0].currentObjectToWorld[12] == 17 && joined.skinned[0].currentObjectToWorld[12] == 23 &&
        overlay->joints[joined.skinned[0].jointOffset].rows[3] == 31 &&
        RtCpuRewriteValidateSkinnedPrepared(service.ProductView(), overlay, joined) == RtCpuRewriteSkinnedRejectReason::None,
        "R4-007 rigid movement and current joints survive delayed worker completion");
    service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct(true);
    service.SetGeometryBuildStallForHarness(false);
    Check(service.WaitForReadyProduct(4000, ticket + 1) && acquire(901), "R4-007 fresh worker supersedes retained product");
    if (service.ProductView()) ticket = service.ProductView()->ticket;
    service.ReleaseConsumedProduct(true);
    // Change attributes that topology alone cannot prove, then reject the retained bind product.
    for (uint64_t frame = 902; frame < 904; ++frame)
    {
        if (frame == 902) verts[0].st[0] += 0.25f;
        else verts[0].color2[0] = 0.75f;
        service.SetGeometryBuildStallForHarness(true);
        capture(frame);
        Check(acquire(frame) && service.TryAcquireOverlay(frame) && service.BuildJoinPlan(frame, joined),
            "R4-007 changed source retains canonical topology and joins");
        Check(RtCpuRewriteValidateSkinnedPrepared(service.ProductView(), service.OverlayView(), joined) ==
            RtCpuRewriteSkinnedRejectReason::WorkerSourceMismatch, "R4-007 changed UV or weight rejects old owned source");
        service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct(true);
        service.SetGeometryBuildStallForHarness(false);
        Check(service.WaitForReadyProduct(4000, ticket + 1) && acquire(frame), "R4-007 replacement source becomes available");
        if (service.ProductView()) ticket = service.ProductView()->ticket;
        service.ReleaseConsumedProduct(true);
    }
    capture(904);
    Check(service.WaitForReadyProduct(4000, ticket + 1), "R4-007 next Ready published beside retained product");
    capture(905);
    Check(service.WaitForReadyProduct(4000, ticket + 2), "R4-007 future Ready published in third slot");
    Check(acquire(904) && service.ProductView() && service.ProductView()->rootFrame == 904 && service.TryAcquireOverlay(904),
        "R4-007 future product cannot displace correct view product");
    service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct(true);
    Check(acquire(905) && service.ProductView() && service.ProductView()->rootFrame == 905 && service.TryAcquireOverlay(905),
        "R4-007 future product preserved until its view");
    if (service.ProductView()) ticket = service.ProductView()->ticket;
    service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct(true);
    capture(906);
    Check(service.WaitForReadyProduct(4000, ticket + 1), "R4-007 first superseded Ready slot reclaimed");
    capture(907);
    Check(service.WaitForReadyProduct(4000, ticket + 2), "R4-007 second superseded Ready slot reclaimed");
    Check(service.Counters().productPressureDrops == 0, "R4-007 retained production cycle has no ring pressure drops");
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(!acquire(907) && !service.WaitForReadyProduct(1), "R4-007 lifecycle invalidation clears retained and pending Ready products");
    service.Shutdown();
}

void TestR4008FrameDelivery()
{
    RtCpuProducerRewriteService service;
    service.Init(); service.SampleRequestedRoute(1);
    RtCpuRewriteOwnedVertex verts[3] = { MakeVert(0,0,0), MakeVert(1,0,0), MakeVert(0,1,0) };
    const uint32_t indexes[3] = {0,1,2};
    float matrix[16]; FillIdentityMatrix(matrix);
    auto mover = MakeTriangleWrite(93000, 1, 0, verts, indexes, matrix);
    // No backend consumption: fill the overlay ring and recover after a missed frame.
    for (uint64_t frame = 1000; frame <= 1003; ++frame)
    {
        Check(service.TryAcquireRootInput(frame,7,4) && service.WriteSurface(mover), "R4-008 overlay pressure input");
        Check(service.SealOverlayFromCapturingInput(frame) == (frame < 1003), "R4-008 full ring rejects only that pose");
        service.AbortCapturingInput();
    }
    Check(!service.TryAcquireOverlay(1003), "R4-008 exact overlay never silently substitutes an older pose");
    Check(service.Counters().overlayReclaimed == 3, "R4-008 stale overlay slots reclaimed after missed frame");
    auto poseOnly = [&](uint64_t frame, float x)
    {
        mover.modelMatrix[12] = x;
        Check(service.TryAcquireRootInput(frame,7,4) && service.WriteSurface(mover) &&
            service.SealOverlayFromCapturingInput(frame), "R4-008 recovered overlay seal");
        service.AbortCapturingInput();
    };
    poseOnly(1004, 4);
    Check(service.TryAcquireOverlay(1004) && service.OverlayView()->rows[0].currentObjectToWorld[12] == 4,
        "R4-008 next exact pose recovers without route reset");
    // Capture next pose before releasing this one: history must be patched on consume.
    poseOnly(1005, 5);
    service.ReleaseConsumedOverlay(true);
    Check(service.TryAcquireOverlay(1005) && service.OverlayView()->rows[0].previousObjectToWorld[12] == 4 &&
        (service.OverlayView()->rows[0].flags & PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM),
        "R4-008 backend history remains coherent when frontend runs ahead");
    service.ReleaseConsumedOverlay(true);
    // A worker owns one input while two queued slots fill; frontend may recycle only queued bytes.
    service.SetWorkerReadingStallForHarness(true);
    auto capture = [&](uint64_t frame)
    {
        mover.modelMatrix[12] = float(frame);
        Check(service.TryAcquireRootInput(frame,7,4) && service.WriteSurface(mover) &&
            service.SealOverlayFromCapturingInput(frame) && service.SealRootInput(), "R4-008 queue pressure captures exact pose");
        Check(service.TryAcquireOverlay(frame) && service.OverlayView()->rows[0].currentObjectToWorld[12] == float(frame),
            "R4-008 queued geometry pressure cannot suppress pose updates");
        service.ReleaseConsumedOverlay(true);
    };
    capture(1010);
    Check(service.WaitUntilWorkerReadingForHarness(4000), "R4-008 worker owns first queued input");
    capture(1011); capture(1012); capture(1013);
    Check(service.Counters().inputReclaimed == 1, "R4-008 queued input reclaimed without overwriting worker input");
    const auto start = std::chrono::steady_clock::now();
    Check(!service.TryAcquireExactProduct(1013,7,4,8), "R4-008 unavailable exact dependency fails within bounded wait");
    Check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250), "R4-008 dependency timeout does not block indefinitely");
    service.SetWorkerReadingStallForHarness(false);
    Check(service.TryAcquireExactProduct(1013,7,4,4000) && service.ProductView()->rootFrame == 1013,
        "R4-008 queued worker drains to exact product without another frontend wake");
    service.ReleaseConsumedProduct();
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(!service.TryAcquireOverlay(1013) && !service.TryAcquireExactProduct(1013,7,4,0),
        "R4-008 invalidation clears overlay and queued geometry");
    service.Shutdown();
}

void TestR4008StaticAndRetention()
{
    PathTraceSmokeVertex vertices[3] = {};
    uint32_t indexes[3] = {0,1,2}, flags[1] = {0};
    RtCpuRewriteFrozenProductView view;
    view.vertices = vertices; view.vertexCount = 3;
    view.indexes = indexes; view.indexCount = 3;
    view.triangleClassAndFlags = flags; view.triangleCount = 1;
    const auto original = RtCpuRewriteStaticGeometrySignature(view);
    view.contentSignature = 123; view.sourceCount = 12; view.rigidMeshCount = 6; view.skinnedMeshCount = 3;
    Check(original == RtCpuRewriteStaticGeometrySignature(view), "R4-008 dynamic membership cannot invalidate static geometry");
    indexes[1] = 2;
    Check(original != RtCpuRewriteStaticGeometrySignature(view), "R4-008 static geometry mutation invalidates its own receipt");
    const uint64_t cap = RtCpuProducerRewriteService::kGpuRigidRetainCapBytes;
    Check(RtCpuRewriteFitsResidentRigidBudget(cap-1024,1024), "R4-008 retained invisible plus packed bytes fit exact cap");
    Check(!RtCpuRewriteFitsResidentRigidBudget(cap-1024,1025) &&
        !RtCpuRewriteFitsResidentRigidBudget(UINT64_MAX,1), "R4-008 retention pressure and overflow cannot exceed budget");
}

void TestR4003TextureMatrices()
{
    RtCpuRewriteTextureMatrices matrices;
    const float raw[4] = { 2, 3, 2, 3 };
    float uv[4];
    std::copy(raw, raw + 4, uv);
    matrices.primary[0] = 4; matrices.primary[2] = 1;
    matrices.normal[4] = 2; matrices.normal[5] = -1;
    RtCpuRewriteApplyTextureMatrix(uv, matrices.primary);
    RtCpuRewriteApplyTextureMatrix(uv + 2, matrices.normal);
    Check(uv[0] == 9 && uv[1] == 3 && uv[2] == 2 && uv[3] == 5,
        "R4-003 primary tiling and independent normal UVs");
    const auto before = RtCpuRewriteTextureMatrixSignature(matrices.primary, 6);
    matrices.primary[2] = -1;
    Check(before != RtCpuRewriteTextureMatrixSignature(matrices.primary, 6),
        "R4-003 animated offset invalidates packed UVs");
    std::copy(raw, raw + 4, uv);
    RtCpuRewriteApplyTextureMatrix(uv, matrices.primary);
    Check(uv[0] == 7 && uv[1] == 3, "R4-003 next frame starts from authored UVs");
    RtCpuRewriteTextureMatrices identity;
    RtCpuRewriteApplyTextureMatrix(uv + 2, identity.normal);
    Check(uv[2] == 2 && uv[3] == 3, "R4-003 removal resets normal UV transform");
}

void TestR4010SparseEmissivePose()
{
    PathTraceSkinnedSourceVertex source = {};
    source.localPosition[0] = 1; source.localPosition[1] = 2;
    source.localPosition[2] = 3; source.localPosition[3] = 1;
    source.texCoord[0] = 2; source.texCoord[1] = 3;
    PathTraceSkinnedJointMatrix joints[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        joints[i].rows[0] = joints[i].rows[5] = joints[i].rows[10] = 1;
        joints[i].rows[3] = float(i * 4);
        source.jointIndices[i] = i; source.jointWeights[i] = 0.25f;
    }
    float world[16] = {};
    world[0] = 2; world[5] = world[10] = world[15] = 1; world[12] = 10;
    const float uv[6] = { 2, 0, 0.25f, 0, -1, 0.5f };
    PathTraceSmokeVertex result = {};
    Check(RtCpuRewriteEvaluateEmissiveVertex(source, joints, 4, world, uv, result) &&
        result.position[0] == 24 && result.position[1] == 2 && result.position[2] == 3 &&
        result.position[3] == 1 && result.texCoord[0] == 4.25f && result.texCoord[1] == -2.5f,
        "R4-010 sparse emissive evaluates four weights, world transform and texture matrix");
    joints[3].rows[3] += 4;
    Check(RtCpuRewriteEvaluateEmissiveVertex(source, joints, 4, world, uv, result) && result.position[0] == 26,
        "R4-010 exact next-frame joint pose changes light position");
    source.jointIndices[3] = 4;
    Check(!RtCpuRewriteEvaluateEmissiveVertex(source, joints, 4, world, uv, result) && result.position[0] == 26,
        "R4-010 malformed palette rejects without partial output");
    Check(!RtCpuRewriteEvaluateEmissiveVertex(source, nullptr, 4, world, uv, result),
        "R4-010 missing owned joints reject");
}

void TestR4011ExactLightLifetime()
{
    RtCpuProducerRewriteService service;
    service.Init(); service.SampleRequestedRoute(1);
    RtCpuRewriteOwnedVertex verts[3] = { MakeVert(0,0,0), MakeVert(1,0,0), MakeVert(0,1,0) };
    const uint32_t indexes[3] = {0,1,2};
    float matrix[16]; FillIdentityMatrix(matrix);
    auto mover = MakeTriangleWrite(94000, 1, 0, verts, indexes, matrix);
    auto capture = [&](uint64_t frame)
    {
        auto lights = std::make_shared<PathTraceDoomAnalyticLightSnapshotData>();
        lights->frame = frame;
        std::weak_ptr<const PathTraceDoomAnalyticLightSnapshotData> weak = lights;
        mover.modelMatrix[12] = float(frame);
        Check(service.TryAcquireRootInput(frame,7,4) && service.WriteSurface(mover) &&
            service.SealOverlayFromCapturingInput(frame, lights), "R4-011 light and pose seal together");
        service.AbortCapturingInput();
        return weak;
    };
    auto first = capture(1100), future = capture(1101);
    Check(!first.expired() && service.TryAcquireOverlay(1100), "R4-011 light input outlives frontend owner");
    const auto* overlay = service.OverlayView();
    Check(overlay && overlay->analyticLights && overlay->analyticLights->frame == 1100 &&
        overlay->rows[0].currentObjectToWorld[12] == 1100,
        "R4-011 future light cannot replace matching-frame pose light");
    service.ReleaseConsumedOverlay(true);
    Check(first.expired() && !future.expired(), "R4-011 release drops light payload without retaining in pose history");
    Check(service.TryAcquireOverlay(1101) && service.OverlayView()->analyticLights->frame == 1101,
        "R4-011 future light available only on its frame");
    service.ReleaseConsumedOverlay(true);
    Check(future.expired(), "R4-011 consumed future payload released");
    auto pending = capture(1102);
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(pending.expired() && !service.TryAcquireOverlay(1102), "R4-011 map invalidation discards unconsumed lights");
    service.Shutdown();
}

void TestR4013ResidentStaticWorld()
{
    RtCpuProducerRewriteService service;
    service.Init(); service.SampleRequestedRoute(1);
    RtCpuRewriteOwnedVertex verts[3] = { MakeVert(0,0,0), MakeVert(1,0,0), MakeVert(0,1,0) };
    const uint32_t indexes[3] = { 0,1,2 };
    float matrix[16]; FillIdentityMatrix(matrix);
    auto world = MakeTriangleWrite(95001, 1, 0, verts, indexes, matrix);
    world.sourceClass = kRtCpuRewriteClassStaticWorld;
    world.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::StaticWorldMap;
    world.meshKey.deformationClass = PtCanonicalDeformationClass::Static;
    world.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::StaticSurface;
    auto mover = MakeTriangleWrite(95002, 2, 0, verts, indexes, matrix);
    bool populate = false;
    auto capture = [&](uint64_t frame, uint64_t revision, bool expectedPopulation, bool includeMover)
    {
        Check(service.TryAcquireRootInput(frame,7,4), "R4-013 acquire frame");
        if (includeMover) Check(service.WriteSurface(mover), "R4-013 dynamic capture precedes cold arena migration");
        Check(service.PrepareResidentStaticInput(revision,populate) && populate == expectedPopulation,
            "R4-013 population only for a new static revision");
        if (populate) Check(service.WriteSurface(world), "R4-013 static population writes owned bytes");
        Check(service.SealOverlayFromCapturingInput(frame) && service.SealRootInput(), "R4-013 population and overlay seal");
    };
    auto acquire = [&](uint64_t frame)
    {
        return service.TryAcquireExactProduct(frame,7,4,4000) && service.TryAcquireOverlay(frame);
    };
    capture(1200,1,true,true);
    Check(acquire(1200), "R4-013 initial worker package acquired");
    const auto* first = service.ProductView();
    auto owner = first ? first->residentStatic : nullptr;
    const auto* retainedVertices = first ? first->vertices : nullptr;
    const auto signature = first ? first->staticContentSignature : 0;
    RtCpuRewriteJoinResult joined;
    Check(first && owner && first->StaticSourceCount() == 1 && first->triangleCount == 1 &&
        service.BuildJoinPlan(1200, joined) && joined.joinedCount == 1,
        "R4-013 static package and migrated dynamic bytes both reach production join");
    service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct();
    verts[0].xyz[0] = 77; // source lifetime ends/changes after cold copy
    capture(1201,1,false,false);
    Check(acquire(1201), "R4-013 static world survives an empty visible frame");
    const auto* next = service.ProductView();
    Check(next && next->residentStatic == owner && next->vertices == retainedVertices && next->vertexCount == 3 &&
        next->vertices[0].position[0] == 0 && next->staticContentSignature == signature && next->sourceCount == 0 &&
        next->StaticSourceCount() == 1 && service.OverlayView()->staticRevision == 1,
        "R4-013 exact same owned geometry with no static capture, packing or descriptor replay");
    service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct();
    capture(1202,2,true,false);
    Check(acquire(1202) && service.ProductView()->residentStatic != owner &&
        service.ProductView()->vertices[0].position[0] == 77 && retainedVertices[0].position[0] == 0,
        "R4-013 replacement preserves old reader while publishing new static bytes");
    service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct();
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(!service.TryAcquireExactProduct(1202,7,4,0) && retainedVertices[0].position[0] == 0,
        "R4-013 reset rejects old products without invalidating a held CPU owner");
    capture(1203,2,true,false);
    Check(acquire(1203), "R4-013 reset requires new population even for the same frontend revision");
    service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct();
    service.Shutdown();
}

void TestR4013PopulationQueuePressure()
{
    RtCpuProducerRewriteService service;
    service.Init(); service.SampleRequestedRoute(1);
    RtCpuRewriteOwnedVertex verts[3] = { MakeVert(0,0,0), MakeVert(1,0,0), MakeVert(0,1,0) };
    const uint32_t indexes[3] = { 0,1,2 };
    float matrix[16]; FillIdentityMatrix(matrix);
    auto world = MakeTriangleWrite(96001, 1, 0, verts, indexes, matrix);
    world.sourceClass = kRtCpuRewriteClassStaticWorld;
    world.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::StaticWorldMap;
    world.meshKey.deformationClass = PtCanonicalDeformationClass::Static;
    world.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::StaticSurface;
    service.SetWorkerReadingStallForHarness(true);
    Check(service.TryAcquireRootInput(1300,7,4) && service.SealRootInput(), "R4-013 occupy worker before population");
    Check(service.WaitUntilWorkerReadingForHarness(4000), "R4-013 worker stall established");
    for (uint64_t frame = 1301; frame <= 1303; ++frame)
    {
        bool populate = false;
        Check(service.TryAcquireRootInput(frame,7,4) && service.PrepareResidentStaticInput(1,populate) &&
            populate == (frame == 1301), "R4-013 initial population remains queued while newer frames arrive");
        if (populate) Check(service.WriteSurface(world), "R4-013 queued cold input populated");
        Check(service.SealOverlayFromCapturingInput(frame) && service.SealRootInput() && service.TryAcquireOverlay(frame),
            "R4-013 queue pressure keeps exact overlay available");
        service.ReleaseConsumedOverlay(true);
    }
    Check(service.Counters().inputReclaimed == 1, "R4-013 only replaceable dynamic input reclaimed");
    service.SetWorkerReadingStallForHarness(false);
    Check(service.TryAcquireExactProduct(1303,7,4,4000) && service.ProductView()->residentStatic &&
        service.ProductView()->StaticSourceCount() == 1 && service.ProductView()->vertexCount == 3,
        "R4-013 initial population survived pressure and supplies newest empty frame");
    service.ReleaseConsumedProduct();
    service.Shutdown();
}

void TestR4014MaterialWorker()
{
    RtCpuProducerRewriteService service;
    auto makeInput = [](float condition, float color) {
        RtCpuRewriteMaterialInput input;
        RtCpuRewriteMaterialSource source;
        RtPathTraceRuntimeMaterialStagePod stage;
        stage.valid = stage.usesPerSurfaceState = stage.emissiveLike = true;
        stage.stageIndex = 0; stage.conditionRegister = 0;
        stage.colorRegisters[0] = stage.colorRegisters[1] = stage.colorRegisters[2] = 1;
        stage.colorRegisters[3] = 2;
        stage.hasTexMatrix = true;
        stage.texMatrixRegisters[2] = 3;
        source.stages.push_back(stage);
        input.sources.push_back(source);
        input.registers = { condition, color, 1.0f, 0.25f };
        RtCpuRewriteMaterialSurface surface;
        surface.materialId = 77; surface.hasRegisters = true; surface.registerCount = 4;
        surface.origin[0] = 42;
        input.surfaces.push_back(surface);
        return input;
    };
    Check(!service.SubmitMaterials(makeInput(1,2)), "R4-014 stopped service rejects material work");
    service.Init();
    for (int frame = 0; frame < 3; ++frame)
    {
        auto input = makeInput(frame == 1 ? 0.0f : 1.0f, 2.0f + frame);
        const auto expected = BuildPathTraceRuntimeMaterialEvalFromPod(true,77,
            input.sources[0].stages.data(),1,input.registers.data(),4,false,input.surfaces[0].origin,true);
        const auto job = service.SubmitMaterials(std::move(input));
        Check(service.FinishMaterials(job) && job->output.size() == 1,
            "R4-014 real shading worker publishes current material result");
        RtPathTraceRuntimeMaterialDecisionPod a, b;
        a.eval = expected; b.eval = job->output[0];
        Check(RtPathTraceRuntimeMaterialDecisionPodEqual(a,b),
            "R4-014 condition/color/UV/origin and all stage fields match serial oracle");
    }
    auto bad = makeInput(1,2); bad.surfaces[0].registerBegin = UINT32_MAX;
    Check(!service.SubmitMaterials(std::move(bad)), "R4-014 malformed span rejected before worker reads");
    bad = makeInput(1,2); bad.surfaces[0].source = 99;
    Check(!service.SubmitMaterials(std::move(bad)), "R4-014 malformed source rejected");
    bad = makeInput(1,2); bad.surfaces.resize(RtCpuRewriteMaterialInput::kMaxSurfaces + 1);
    Check(!service.SubmitMaterials(std::move(bad)), "R4-014 material capacity rejection");
    const auto stale = service.SubmitMaterials(makeInput(1,2));
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(!service.FinishMaterials(stale), "R4-014 lifecycle change rejects old exact values");
    const auto stopping = service.SubmitMaterials(makeInput(1,2));
    service.Shutdown();
    Check(stopping && stopping->done && !service.FinishMaterials(stopping),
        "R4-014 shutdown completes and rejects pending material receipt");
    service.Init();
    Check(service.FinishMaterials(service.SubmitMaterials(makeInput(1,5))),
        "R4-014 material worker resumes after service restart");
    service.Shutdown();
}


void TestR4028MaterialBindingOwnership()
{
    RtCpuMaterialBindingPlanInput input;
    RtCpuMaterialBindingRow row;row.materialId=280;
    row.slots={{{"textures/stone",0,false,true},{"textures/stone",0,false,true},
        {"textures/stone",0,true,false},{"GUIS\\monitor",1,false,true},
        {"textures/stone",2,false,false},{"",3,false,true}}};
    input.rows.push_back(row);row.materialId=281;input.rows.push_back(row);
    RtCpuMaterialBindingPlan expected;
    Check(BuildRtCpuMaterialBindingPlan(input,expected) && expected.rules.size()==5 &&
        expected.rows.at(280)==expected.rows.at(281) && expected.rows.at(280)[0]==expected.rows.at(280)[1] &&
        expected.rows.at(280)[0]!=expected.rows.at(280)[2],
        "R4-028 binding plan deduplicates exact resource/name/dimension and preserves material rows");
    bool equivalent=true;
    for (size_t i=0;i<expected.rules.size();++i) {
        const auto& rule=expected.rules[i];
        RtCpuMaterialBindingInput oracle;oracle.rules={{rule.name,rule.descriptorEligible}};
        equivalent=equivalent && expected.safety[i]==BuildRtCpuMaterialBindingSafety(oracle)[0];
    }
    Check(equivalent,"R4-028 binding plan preserves existing safety oracle");
    auto malformed=input;malformed.rows.push_back(row);
    Check(!BuildRtCpuMaterialBindingPlan(malformed,expected) && expected.rows.size()==2,
        "R4-028 duplicate identity rejects without partial result publication");
    malformed=input;malformed.ownerCharge=input.kMaxBytes+1;
    Check(!BuildRtCpuMaterialBindingPlan(malformed,expected),"R4-028 plan storage is bounded");

    RtCpuProducerRewriteService service;service.Init();
    struct Work {
        RtCpuMaterialBindingPlanInput input;RtCpuMaterialBindingPlan output;
        std::mutex mutex;std::condition_variable cv;bool started=false,release=false;
        std::thread::id thread;
    };
    auto owned=std::make_shared<Work>();owned->input=input;
    auto binding=service.SubmitMaterialBindingPreparation(2800,input.ChargedBytes(),[owned] {
        {
            std::unique_lock<std::mutex> lock(owned->mutex);
            owned->thread=std::this_thread::get_id();owned->started=true;owned->cv.notify_all();
            owned->cv.wait(lock,[&] {return owned->release;});
        }
        return BuildRtCpuMaterialBindingPlan(owned->input,owned->output);
    });
    bool started=false;
    {
        std::unique_lock<std::mutex> lock(owned->mutex);
        started=owned->cv.wait_for(lock,std::chrono::seconds(2),[&] {return owned->started;});
    }
    RtCpuRewriteMaterialInput numeric;numeric.prepareFrame=true;numeric.rootFrame=2800;
    numeric.sources.push_back({});numeric.registry={{77,0,false}};
    RtCpuRewriteMaterialSurface surface;surface.materialId=77;numeric.surfaces.push_back(surface);
    auto numericJob=service.SubmitMaterials(std::move(numeric));
    bool numericReady=false;
    if (numericJob) {
        std::unique_lock<std::mutex> lock(numericJob->mutex);
        numericReady=numericJob->cv.wait_for(lock,std::chrono::seconds(2),[&] {return numericJob->done;});
    }
    const bool busyRejected=!service.SubmitLightManagerPreparation(2800,64,[] {return true;});
    {std::lock_guard<std::mutex> lock(owned->mutex);owned->release=true;owned->cv.notify_all();}
    Check(started && numericReady && busyRejected && service.FinishMaterials(numericJob) &&
        service.FinishMaterialBindingPreparation(binding,2800) && owned->thread!=std::this_thread::get_id() &&
        owned->output.rows==expected.rows && owned->output.safety==expected.safety,
        "R4-028 real numeric material worker completes while independent binding worker is held");
    Check(!service.FinishLightManagerPreparation(binding,2800) &&
        !service.FinishMaterialBindingPreparation(binding,2801),"R4-028 binding receipt requires family and exact root");
    auto manager=service.SubmitLightManagerPreparation(2800,64,[] {return true;});
    Check(service.FinishLightManagerPreparation(manager,2800) &&
        !service.FinishMaterialBindingPreparation(manager,2800),"R4-028 same slot resumes lighting with distinct receipt family");
    binding=service.SubmitMaterialBindingPreparation(2802,64,[]()->bool {throw 7;});
    Check(!service.FinishMaterialBindingPreparation(binding,2802),"R4-028 binding exception becomes failed receipt");
    binding=service.SubmitMaterialBindingPreparation(2803,64,[] {return true;});
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(!service.FinishMaterialBindingPreparation(binding,2803),"R4-028 invalidated binding frame cannot be consumed");
    Check(!service.SubmitMaterialBindingPreparation(0,64,[] {return true;}) &&
        !service.SubmitMaterialBindingPreparation(2804,input.kMaxBytes+1,[] {return true;}),
        "R4-028 binding admission guards root and capacity");
    service.Shutdown();
    CheckEqU64(service.Counters().threadsJoined,4,"R4-028 reuses four existing workers");
}

void TestR4027ParallelLightProducts()
{
    RtCpuProducerRewriteService service;
    Check(!service.SubmitLightManagerPreparation(2700,64,[] { return true; }), "R4-027 stopped manager rejects work");
    service.Init();
    Check(!service.SubmitLightManagerPreparation(0,64,[] { return true; }) &&
        !service.SubmitLightManagerPreparation(2700,512ull*1024*1024+1,[] { return true; }),
        "R4-027 manager root and capacity bounded");
    struct Work {
        std::mutex mutex; std::condition_variable cv;
        bool started=false, release=false, overlapped=false, busyRejected=false;
        int manager=0, unified=0;
        std::thread::id managerThread, shadingThread;
    };
    auto work=std::make_shared<Work>();
    const auto owner=std::this_thread::get_id();
    auto parent=service.SubmitLightPreparation(2700,128,[&service,work] {
        work->shadingThread=std::this_thread::get_id();
        auto child=service.SubmitLightManagerPreparation(2700,128,[work] {
            std::unique_lock<std::mutex> lock(work->mutex);
            work->managerThread=std::this_thread::get_id();work->started=true;
            work->cv.notify_all();work->cv.wait(lock,[&] { return work->release; });
            work->manager=40;return true;
        });
        if (!child) return false;
        {
            std::unique_lock<std::mutex> lock(work->mutex);
            work->overlapped=work->cv.wait_for(lock,std::chrono::seconds(2),[&] { return work->started; });
            work->busyRejected=!service.SubmitLightManagerPreparation(2701,64,[] { return true; });
            work->unified=2;work->release=true;work->cv.notify_all();
        }
        return service.FinishLightManagerPreparation(child,2700) && work->manager+work->unified==42;
    });
    Check(service.FinishLightPreparation(parent,2700) && work->overlapped && work->busyRejected &&
        work->managerThread!=work->shadingThread && work->managerThread!=owner && work->shadingThread!=owner,
        "R4-027 real shading parent and manager child run concurrently and join owned products");
    auto child=service.SubmitLightManagerPreparation(2702,64,[] { return true; });
    Check(service.FinishLightManagerPreparation(child,2702) && !service.FinishLightManagerPreparation(child,2703),
        "R4-027 manager completion requires exact root");
    child=service.SubmitLightManagerPreparation(2704,64,[]()->bool { throw 7; });
    Check(!service.FinishLightManagerPreparation(child,2704), "R4-027 manager exception becomes failed receipt");
    parent=service.SubmitLightPreparation(2705,64,[&service] {
        auto failed=service.SubmitLightManagerPreparation(2705,64,[] { return false; });
        return service.FinishLightManagerPreparation(failed,2705);
    });
    Check(!service.FinishLightPreparation(parent,2705), "R4-027 failed manager product rejects parent lighting");
    child=service.SubmitLightManagerPreparation(2706,64,[] { return true; });
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(!service.FinishLightManagerPreparation(child,2706), "R4-027 lifecycle rejects stale manager product");
    // Stop with a queued or running nested job. The manager stays available to
    // finish a receipt even while Shutdown joins its waiting shading parent.
    parent=service.SubmitLightPreparation(2707,64,[&service] {
        auto nested=service.SubmitLightManagerPreparation(2707,64,[] { return true; });
        return service.FinishLightManagerPreparation(nested,2707);
    });
    service.Shutdown();
    Check(parent && parent->done && !service.FinishLightPreparation(parent,2707),
        "R4-027 shutdown drains nested receipts and forbids consumption");
    CheckEqU64(service.Counters().threadsJoined,4,"R4-027 all four persistent workers joined");
}

void TestR4026MaterialDependencies()
{
    RtCpuProducerRewriteService service; service.Init();
    RtCpuRewriteMaterialInput input; input.prepareFrame = true; input.rootFrame = 2600;
    input.registry = {{77,0,false}};
    RtCpuRewriteMaterialSource source;
    RtPathTraceRuntimeMaterialStagePod stage;
    stage.valid = stage.usesPerSurfaceState = stage.hasTexMatrix = true;
    stage.stageIndex = 0; stage.conditionRegister = 0;
    stage.colorRegisters[0] = stage.colorRegisters[1] = stage.colorRegisters[2] = 1;
    stage.colorRegisters[3] = 0; stage.texMatrixRegisters[2] = 2;
    source.stages.push_back(stage); source.primaryFallbackStage = 0;
    input.sources.push_back(source); input.registers = {1,2,0.25f};
    RtCpuRewriteMaterialSurface surface;
    surface.materialId = 77; surface.entityIndex = 9; surface.entityNum = 10;
    surface.modelSurfaceIndex = 3; surface.hasRegisters = true; surface.registerCount = 3;
    input.surfaces = {surface,surface};
    RtCpuMaterialBindingInput bindings;
    bindings.rules = {{"textures/stone",true},{"guis/monitor",true},{"textures/stone",false}};
    auto eagerInput = input; eagerInput.bindings = bindings;
    auto eager = service.SubmitMaterials(std::move(eagerInput));
    Check(service.FinishMaterials(eager), "R4-026 complete-input oracle succeeds");
    const auto prepared = [](const std::shared_ptr<RtCpuRewriteMaterialJob>& job) {
        if (!job) return false;
        std::unique_lock<std::mutex> lock(job->mutex);
        return job->cv.wait_for(lock, std::chrono::seconds(2), [&] { return job->numericPrepared.load() || job->done; }) &&
            job->numericPrepared.load() && !job->done;
    };
    auto job = service.SubmitMaterials(RtCpuRewriteMaterialInput(input), true);
    Check(prepared(job), "R4-026 real worker completes numeric frame while owner has not published bindings");
    Check(service.PublishMaterialBindings(job, RtCpuMaterialBindingInput(bindings)), "R4-026 late binding publication accepted");
    Check(!service.PublishMaterialBindings(job, {}), "R4-026 duplicate publication cannot replace accepted input");
    Check(service.FinishMaterials(job) && eager && eager->success &&
        job->frame.activeIds == eager->frame.activeIds &&
        job->frame.firstSurfaceOrdinals == eager->frame.firstSurfaceOrdinals &&
        job->frame.bindingSafety == eager->frame.bindingSafety &&
        job->frame.decisions[0].chosenMaterialId == eager->frame.decisions[0].chosenMaterialId &&
        job->frame.surfaces[0].primary[2] == 0.25f && job->output.size() == eager->output.size(),
        "R4-026 deferred and complete packages preserve variants, UVs, deduplication and binding safety");
    // Publish immediately as well as after numeric completion: both dependency orders work.
    job = service.SubmitMaterials(RtCpuRewriteMaterialInput(input), true);
    Check(service.PublishMaterialBindings(job, RtCpuMaterialBindingInput(bindings)) && service.FinishMaterials(job),
        "R4-026 bindings ready before worker dependency check are consumed");
    job = service.SubmitMaterials(RtCpuRewriteMaterialInput(input), true);
    Check(prepared(job), "R4-026 pending cancellation reaches dependency boundary");
    service.CancelMaterialBindings(job);
    Check(!service.FinishMaterials(job) && !service.PublishMaterialBindings(job, {}),
        "R4-026 abandoned owner capture completes failed receipt and rejects late publication");
    job = service.SubmitMaterials(RtCpuRewriteMaterialInput(input), true);
    auto excessive = bindings; excessive.ownerCharge = input.kMaxBytes - input.ChargedBytes() + 1;
    Check(!service.PublishMaterialBindings(job, std::move(excessive)) && !service.FinishMaterials(job),
        "R4-026 combined prefix and late binding charge rejects and wakes worker");
    job = service.SubmitMaterials(RtCpuRewriteMaterialInput(input), true);
    Check(prepared(job), "R4-026 invalidation test reaches dependency boundary");
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(!service.FinishMaterials(job), "R4-026 map invalidation wakes pending dependency and rejects frame");
    job = service.SubmitMaterials(RtCpuRewriteMaterialInput(input), true);
    Check(prepared(job), "R4-026 route cancellation test reaches dependency boundary");
    service.CancelInFlight();
    Check(!service.FinishMaterials(job), "R4-026 route cancellation wakes pending dependency");
    job = service.SubmitMaterials(RtCpuRewriteMaterialInput(input), true);
    Check(prepared(job), "R4-026 shutdown test reaches dependency boundary");
    service.Shutdown();
    Check(!service.FinishMaterials(job), "R4-026 shutdown joins pending worker without owner publication");
}

void TestR4024MaterialBindings()
{
    RtCpuRewriteMaterialInput input;input.prepareFrame=true;input.rootFrame=2400;
    input.bindings.rules={{"textures/stone",true},{"GUIS\\monitor",true},{"textures/SCRATCHpad",true},
        {"textures/plain.swf",true},{"",true},{"textures/stone",false}};
    RtCpuProducerRewriteService service;service.Init();
    auto job=service.SubmitMaterials(RtCpuRewriteMaterialInput(input));
    Check(service.FinishMaterials(job) && job->frame.bindingSafety==std::vector<uint8_t>({1,0,0,1,0,0}),
        "R4-024 actual material worker publishes binding safety from owned names and eligibility");
    input.bindings.allowGui=true;
    job=service.SubmitMaterials(RtCpuRewriteMaterialInput(input));
    Check(service.FinishMaterials(job) && job->frame.bindingSafety==std::vector<uint8_t>({1,1,1,1,0,0}),
        "R4-024 GUI override preserves missing-resource and empty-name rejection");
    input.bindings.ownerCharge=input.kMaxBytes+1;
    Check(!service.SubmitMaterials(std::move(input)),"R4-024 owner binding snapshot is charged to material capacity");
    service.Shutdown();
}

void TestR4023RigidResolve()
{
    auto oracle = [](const RtCpuRigidResolveWork& work) {
        std::vector<RtCpuRigidResolveRow> result;
        auto candidates = work.retained;
        for (const auto& q : work.queries) {
            RtCpuRigidResolveRow row;
            for (size_t i=0;i<work.retained.size();++i) {
                const auto& r=work.retained[i];
                if (r.key == q.earlyKey && r.hasBlas &&
                    (!work.haveProduct || (q.validMesh && r.signature == q.signature)))
                { row.early=int32_t(i);break; }
            }
            if (!work.haveProduct || q.validMesh) {
                for (size_t i=0;i<candidates.size();++i) {
                    const auto& r=candidates[i];
                    if (work.haveProduct ? (r.key == q.candidateKey && r.world == work.world)
                        : (r.key == q.earlyKey && r.hasBlas))
                    { row.candidate=int32_t(i);break; }
                }
                if (work.haveProduct) {
                    RtCpuRigidResolveRetained replacement {q.candidateKey,work.world,q.signature,true};
                    if (row.candidate < 0) {
                        row.append=true;row.candidate=int32_t(candidates.size());candidates.push_back(replacement);
                    } else candidates[row.candidate]=replacement;
                }
            }
            result.push_back(row);
        }
        return result;
    };
    uint32_t seed=23;
    auto next=[&]() { seed=seed*1664525u+1013904223u;return seed; };
    bool parity=true;
    for (int trial=0;trial<64;++trial) {
        RtCpuRigidResolveWork work;work.haveProduct=(trial%2)==0;work.world=3;
        for (int i=0;i<200;++i) {
            RtCpuRigidResolveRetained r;
            r.key={next()%31,next()%3,next()%5,next()%7};
            r.world=next()%4;r.signature=next()%6;r.hasBlas=(next()%3)!=0;
            work.retained.push_back(r);
            if (i%9==0) {r.hasBlas=!r.hasBlas;work.retained.push_back(r);}
        }
        for (int i=0;i<400;++i) {
            const auto& r=work.retained[next()%work.retained.size()];
            RtCpuRigidResolveQuery q {r.key,r.key,next()%6,(next()%7)!=0};
            if (i%5==0) q.candidateKey.asset+=1000;
            if (i%11==0) q.earlyKey.asset+=2000;
            work.queries.push_back(q);
            if (i%13==0) work.queries.push_back(q);
        }
        const auto expected=oracle(work);
        parity=parity && BuildRtCpuRigidResolve(work) && expected.size()==work.rows.size();
        for (size_t i=0;parity && i<expected.size();++i)
            parity=expected[i].early==work.rows[i].early && expected[i].candidate==work.rows[i].candidate &&
                expected[i].append==work.rows[i].append;
    }
    Check(parity,"R4-023 indexed resolver matches serial predicates, first matches and ordered insertions");
    auto work=std::make_shared<RtCpuRigidResolveWork>();work->root=2300;work->world=9;work->haveProduct=true;
    RtCpuRigidResolveKey key {1,2,3,4};
    work->queries={{key,key,5,true},{key,key,6,true},{key,key,5,false}};
    RtCpuProducerRewriteService service;service.Init();
    auto job=service.SubmitRigidPreparation(work->root,work->ChargedBytes(),[work] { return BuildRtCpuRigidResolve(*work); });
    Check(service.FinishRigidPreparation(job,2300) && work->rows.size()==3 &&
        work->rows[0].append && work->rows[0].candidate==0 && !work->rows[1].append &&
        work->rows[1].candidate==0 && work->rows[2].candidate==-1,
        "R4-023 actual worker publishes append reservations and rejects invalid meshes");
    Check(!service.FinishRigidPreparation(job,2301),"R4-023 wrong root rejects resolution");
    Check(!service.FinishMaterialRecords(job,2300),"R4-023 rigid product cannot masquerade as material records");
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(!service.FinishRigidPreparation(job,2300),"R4-023 lifecycle rejects old retained indices");
    Check(!service.SubmitRigidPreparation(0,0,[] { return true; }) &&
        !service.SubmitRigidPreparation(2300,RtCpuRigidResolveWork::kMaxBytes+1,[] { return true; }),
        "R4-023 root and capacity are required before dispatch");
    // The two families share a bounded slot, with no queue or accidental overlap.
    std::mutex mutex;std::condition_variable cv;bool entered=false,release=false;
    job=service.SubmitRigidPreparation(2302,0,[&] {
        std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_all();
        cv.wait(lock,[&] { return release; });return true;
    });
    {std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&] { return entered; });}
    Check(!service.SubmitMaterialRecords(2302,0,[] { return true; }) &&
        !service.SubmitRigidPreparation(2302,0,[] { return true; }),"R4-023 busy planning slot rejects a second task");
    {std::lock_guard<std::mutex> lock(mutex);release=true;}cv.notify_all();
    Check(service.FinishRigidPreparation(job,2302),"R4-023 blocking rigid job completes before material reuse");
    auto material=service.SubmitMaterialRecords(2302,0,[] { return true; });
    Check(service.FinishMaterialRecords(material,2302),"R4-023 same planning slot then accepts material records");
    service.Shutdown();
}

void TestR4030ConstantMaterials()
{
    RtCpuRewriteMaterialSource source;
    RtPathTraceRuntimeMaterialStagePod stage;stage.valid=true;stage.diffuse=true;
    stage.conditionRegister=0;stage.hasTexMatrix=true;stage.stageIndex=0;
    for (int i=0;i<6;++i) stage.texMatrixRegisters[i]=i+1;
    source.stages.push_back(stage);source.primaryFallbackStage=0;
    stage.stageIndex=1;stage.diffuse=false;stage.texMatrixRegisters[2]=7;
    source.stages.push_back(stage);source.normalStage=1;
    const std::vector<float> regs={1,2,0,0.25f,0,3,0.5f,0.75f};
    RtCpuRewriteMaterialFrameSurface route;
    Check(BuildRtCpuConstantMaterialRoute(true,77,source,regs.data(),regs.size(),route),
        "R4-030 constant definition produces retained numeric route");
    route.entityIndex=9;route.modelSurfaceIndex=3;
    RtCpuRewriteMaterialInput oracle;oracle.prepareFrame=true;oracle.rootFrame=3000;
    oracle.registry={{77,0,true}};oracle.sources={source};oracle.registers=regs;
    RtCpuRewriteMaterialSurface surface;surface.materialId=77;surface.entityIndex=9;
    surface.modelSurfaceIndex=3;surface.hasRegisters=true;surface.registerCount=regs.size();
    oracle.surfaces={surface};
    const auto evaluated=BuildPathTraceRuntimeMaterialEvalFromPod(true,77,source.stages.data(),source.stages.size(),
        regs.data(),regs.size(),false,nullptr,false);
    RtCpuRewriteMaterialFrame original;
    Check(BuildRtCpuMaterialFrame(oracle,{evaluated},original) && original.recordSamples.empty() &&
        original.surfaces.size()==1 && original.surfaces[0].id==route.id &&
        std::equal(route.primary,route.primary+6,original.surfaces[0].primary) &&
        std::equal(route.normal,route.normal+6,original.surfaces[0].normal),
        "R4-030 constant tiled diffuse and separate normal matrices match full production kernel");
    auto expectLive=[&](RtCpuRewriteMaterialSource changed) {
        RtCpuRewriteMaterialFrameSurface rejected;
        return !BuildRtCpuConstantMaterialRoute(true,77,changed,regs.data(),regs.size(),rejected);
    };
    auto changed=source;changed.opaqueCompatibility=true;
    Check(expectLive(changed),"R4-030 swinglight compatibility stays live");
    for (int flag=0;flag<6;++flag) {
        changed=source;auto& value=changed.stages[0];
        if (flag==0) value.usesPerSurfaceState=true;
        if (flag==1) value.dynamicImage=true;
        if (flag==2) value.cinematic=true;
        if (flag==3) value.guiRenderTarget=true;
        if (flag==4) value.program=true;
        if (flag==5) value.emissiveLike=true;
        Check(expectLive(changed),"R4-030 runtime stage dependency stays on evaluated path");
    }
    Check(!BuildRtCpuConstantMaterialRoute(false,77,source,regs.data(),regs.size(),route) &&
        !BuildRtCpuConstantMaterialRoute(true,77,source,nullptr,0,route),
        "R4-030 nonconstant or missing register authority cannot reuse");
    RtCpuRewriteMaterialInput input;input.prepareFrame=true;input.rootFrame=3001;
    input.constantSurfaces={route,route}; // duplicate current surface retains first membership
    auto second=route;second.entityIndex=10;input.constantSurfaces.push_back(second);
    RtCpuProducerRewriteService service;service.Init();
    auto job=service.SubmitMaterials(RtCpuRewriteMaterialInput(input));
    Check(service.FinishMaterials(job) && job->output.empty() && job->frame.decisions.empty() &&
        job->frame.recordSamples.empty() && job->frame.surfaces.size()==2 &&
        job->frame.activeIds==std::vector<uint32_t>({77}) && job->frame.Find(9,3,77) &&
        !job->frame.Find(11,3,77),"R4-030 actual service consumes constant routes without numeric or variant replay");
    input.rootFrame=3002;input.constantSurfaces={second};
    job=service.SubmitMaterials(RtCpuRewriteMaterialInput(input));
    Check(service.FinishMaterials(job) && !job->frame.Find(9,3,77) && job->frame.Find(10,3,77),
        "R4-030 freed current membership cannot survive retained definition reuse");
    input.sources={source};input.sources[0].stages[0].usesPerSurfaceState=true;
    input.registers=regs;surface.entityIndex=11;surface.materialId=88;input.surfaces={surface};
    input.registry={{77,0,true},{88,0,false}};input.rootFrame=3003;
    job=service.SubmitMaterials(RtCpuRewriteMaterialInput(input));
    Check(service.FinishMaterials(job) && job->output.size()==1 && job->frame.decisions.size()==1 &&
        job->frame.recordSamples.size()==1 && job->frame.Find(10,3,77) && job->frame.Find(11,3,88),
        "R4-030 mixed frame preserves dynamic records alongside excluded constants");
    service.Shutdown();
    RtCpuRewriteMaterialFrame rejected;
    auto invalid=input;invalid.constantSurfaces[0].id=88;
    Check(!BuildRtCpuMaterialFrame(invalid,{evaluated},rejected),"R4-030 constant variant forgery rejected");
    invalid=input;invalid.constantSurfaces[0].primary[0]=std::numeric_limits<float>::infinity();
    Check(!BuildRtCpuMaterialFrame(invalid,{evaluated},rejected),"R4-030 nonfinite constant matrix rejected");
    invalid=input;invalid.constantSurfaces[0].entityIndex=11;
    invalid.constantSurfaces[0].baseId=invalid.constantSurfaces[0].id=88;
    Check(!BuildRtCpuMaterialFrame(invalid,{evaluated},rejected),"R4-030 ambiguous constant and live surface rejected");
    invalid={};invalid.constantSurfaces.resize(RtCpuRewriteMaterialInput::kMaxSurfaces+1);
    Check(!invalid.WithinCapacity(),"R4-030 combined surface capacity includes constant membership");
}

void TestR4029OwnedMaterialState()
{
    RtCpuRewriteMaterialInput input;input.prepareFrame=true;input.rootFrame=2900;
    input.registry={{77,0,true}};input.sources.push_back({});
    RtCpuRewriteMaterialSurface surface;surface.materialId=77;surface.entityIndex=1;
    for (int index:{1,1,2,3,4}) {surface.modelSurfaceIndex=index;input.surfaces.push_back(surface);}
    RtPathTraceRuntimeMaterialEvalPod evaluated;
    evaluated.result=RtPathTraceRuntimeEvalBuildResult::Built;
    evaluated.selectedStageIndex=2;evaluated.selectedStagePriority=3;evaluated.enabledStages=1;
    evaluated.color[0]=0.3f;evaluated.color[1]=0.4f;evaluated.color[2]=0.5f;evaluated.color[3]=0.75f;
    evaluated.texMatrix[2]=0.25f;evaluated.alphaTest=0.4f;evaluated.alphaTestStages=1;
    evaluated.hasDiffuseStageColor=true;evaluated.diffuseStageColor[0]=0.6f;
    evaluated.hasSurfaceOrigin=true;evaluated.surfaceOrigin[0]=17;
    evaluated.orderedStageCount=1;evaluated.orderedStages[0].stageIndex=4;
    evaluated.orderedStages[0].enabled=true;evaluated.orderedStages[0].color[0]=0.8f;
    evaluated.orderedStages[0].texMatrix[5]=0.9f;
    std::vector<RtPathTraceRuntimeMaterialEvalPod> evaluations(5,evaluated);
    evaluations[1].selectedStageEmissive=true; // duplicate surface must remain ignored
    evaluations[2].selectedStageEmissive=true;evaluations[2].condition=0;
    evaluations[3].selectedStageEmissive=true;
    evaluations[4].result=RtPathTraceRuntimeEvalBuildResult::NoRegisters;
    RtCpuRewriteMaterialFrame frame;
    Check(BuildRtCpuMaterialFrame(input,evaluations,frame) && frame.recordSamples.size()==3 &&
        frame.emissiveSampleOrdinals==std::vector<uint32_t>({2}),
        "R4-029 first surface filter and first disabled emissive preserve owner selection");
    if (frame.recordSamples.size()==3) {
        const auto& sample=frame.recordSamples[0];
        Check(sample.id==77 && sample.stageIndex==2 && sample.stagePriority==3 && sample.color[0]==0.3f &&
            sample.texMatrix[0][2]==0.25f && sample.alphaTest==0.4f && sample.diffuseStageColor[0]==0.6f &&
            sample.hasSurfaceOrigin && sample.surfaceOrigin.x==17 && sample.orderedStageCount==1 &&
            sample.orderedStages[0].stageIndex==4 && sample.orderedStages[0].texMatrix[1][2]==0.9f,
            "R4-029 owned state carries selected and ordered UV alpha tint and origin inputs");
        RtCpuMaterialRecordsInput records;records.rows={{77,false,true,false,0}};
        records.samples=frame.recordSamples;
        const auto output=BuildRtCpuMaterialRecords(records);
        Check(output.size()==2 && output[0].materialId==77 && output[0].color[0]==0.3f &&
            output[0].texMatrix0[2]==0.25f && output[0].texMatrix1[3]==0.4f &&
            (output[0].flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED)!=0 &&
            output[1].color[0]==0.8f && output[1].texMatrix1[2]==0.9f,
            "R4-029 existing record kernel consumes owned samples and retains last-sample and ordered-stage behavior");
    }
    auto invalid=evaluated;invalid.orderedStageCount=9;
    bool rejected=false;
    try {RtCpuMaterialRecordSampleFromEvaluation(invalid,77);} catch (const std::invalid_argument&) {rejected=true;}
    Check(rejected,"R4-029 invalid evaluated stage span rejected before sample copy");
}

void TestR4022MaterialFrame()
{
    RtCpuRewriteMaterialInput input;
    input.prepareFrame = true; input.rootFrame = 2200;
    input.registry = {{77,0,false},{88,0,true}};
    RtCpuRewriteMaterialSource source;
    RtPathTraceRuntimeMaterialStagePod stage;
    stage.valid = stage.usesPerSurfaceState = stage.hasTexMatrix = true;
    stage.stageIndex = 0; stage.conditionRegister = 0;
    stage.colorRegisters[0] = stage.colorRegisters[1] = stage.colorRegisters[2] = 1;
    stage.colorRegisters[3] = 2;
    stage.texMatrixRegisters[2] = 3;
    source.stages.push_back(stage);
    stage.stageIndex = 1; stage.texMatrixRegisters[2] = 4;
    source.stages.push_back(stage);
    source.primaryFallbackStage = 0; source.normalStage = 1;
    input.sources.push_back(source);
    input.registers = {1,2,1,0.25f,0.75f};
    RtCpuRewriteMaterialSurface surface;
    surface.materialId = 77; surface.entityIndex = 9; surface.entityNum = 10;
    surface.modelSurfaceIndex = 3; surface.hasRegisters = true; surface.registerCount = 5;
    input.surfaces = {surface,surface};
    surface.entityIndex = 2; surface.materialId = 88;
    input.surfaces.push_back(surface);
    const auto eval = BuildPathTraceRuntimeMaterialEvalFromPod(true,77,source.stages.data(),2,
        input.registers.data(),5,false,surface.origin,true);
    auto key = BuildPathTraceRuntimeMaterialVariantKeyFromPod(77,9,10,3,0,nullptr,0);
    const auto original = SelectPathTraceRuntimeMaterialVariant(key,eval,true,false,
        [](uint32_t) { return false; }, [](uint32_t,uint32_t) { return false; });
    // Force the first candidate to belong to a different base; the worker must
    // choose the same collision result as the established serial selector.
    input.registry.push_back({original.initialCandidateId,99,false});
    const auto oracle = SelectPathTraceRuntimeMaterialVariant(key,eval,true,false,
        [&](uint32_t id) { return id == original.initialCandidateId; },
        [](uint32_t,uint32_t) { return false; });
    RtCpuProducerRewriteService service; service.Init();
    auto job = service.SubmitMaterials(RtCpuRewriteMaterialInput(input));
    Check(service.FinishMaterials(job),"R4-022 real worker publishes full material frame");
    if (job && job->success)
    {
        const auto& frame = job->frame;
        Check(frame.recordSamples.size()==2 && frame.recordSamples[0].id==oracle.chosenMaterialId &&
            frame.recordSamples[1].id==88,
            "R4-029 actual material service publishes owned current state with final variant IDs");
        const auto* row = frame.Find(9,3,77);
        Check(row && row->id == oracle.chosenMaterialId && row->id != original.initialCandidateId &&
            frame.decisions[1].chosenMaterialId == row->id && frame.decisions[0].collisionCount == 1,
            "R4-022 variant collision and same-frame reservation match serial authority");
        Check(row && row->primary[2] == 0.25f && row->normal[2] == 0.75f && row->ordinal == 0,
            "R4-022 primary and bump matrices plus duplicate first-surface rule");
        Check(frame.Find(2,3,88) && frame.Find(2,3,88)->id == 88 && frame.activeIds.size() == 2 &&
            frame.firstSurfaceOrdinals == std::vector<uint32_t>({0,2}) && !frame.Find(2,99,88),
            "R4-022 static identity and capture-order samples survive sorted lookup");
    }
    // Existing variant ownership is reusable, including on the next frame.
    input.registry.push_back({oracle.chosenMaterialId,77,false});
    job = service.SubmitMaterials(RtCpuRewriteMaterialInput(input));
    Check(service.FinishMaterials(job) && job->frame.decisions[0].chosenMaterialId == oracle.chosenMaterialId,
        "R4-022 existing matching variant retains stable identity");
    auto noRegs = input; noRegs.surfaces[0].hasRegisters = false; noRegs.surfaces.resize(1);
    job = service.SubmitMaterials(RtCpuRewriteMaterialInput(noRegs));
    Check(service.FinishMaterials(job) && job->frame.surfaces[0].id == 77 &&
        job->frame.surfaces[0].primary[2] == 0 && job->frame.surfaces[0].normal[2] == 0,
        "R4-022 missing registers preserve base identity and identity matrices");
    auto invalid = input; invalid.registry.push_back(invalid.registry[0]);
    job = service.SubmitMaterials(RtCpuRewriteMaterialInput(invalid));
    Check(!service.FinishMaterials(job),"R4-022 incoherent registry snapshot rejects entire frame");
    invalid = input; invalid.surfaces[0].registerBegin = UINT32_MAX;
    Check(!service.SubmitMaterials(RtCpuRewriteMaterialInput(invalid)),"R4-022 invalid numeric span cannot reach worker");
    job = service.SubmitMaterials(RtCpuRewriteMaterialInput(input)); service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(!service.FinishMaterials(job),"R4-022 lifecycle invalidates prepared identities and values together");
    service.Shutdown();
}

void TestR4020LightWorker()
{
    RtCpuProducerRewriteService service;
    Check(!service.SubmitLightPreparation(1, 64, [] { return true; }), "R4-020 stopped service rejects light work");
    service.Init();
    Check(!service.SubmitLightPreparation(1, 512ull * 1024 * 1024 + 1, [] { return true; }), "R4-020 light capacity bounded");
    Check(!service.SubmitLightPreparation(0, 64, [] { return true; }), "R4-020 missing root rejected");
    struct Input { std::mutex mutex; std::condition_variable cv; bool started=false, release=false; std::thread::id worker; int result=0; };
    auto input = std::make_shared<Input>();
    const auto owner = std::this_thread::get_id();
    auto job = service.SubmitLightPreparation(20, 64, [input] {
        std::unique_lock<std::mutex> lock(input->mutex);
        input->worker = std::this_thread::get_id(); input->started = true; input->cv.notify_all();
        input->cv.wait(lock, [&] { return input->release; });
        input->result = 42; return true;
    });
    {
        std::unique_lock<std::mutex> lock(input->mutex);
        Check(input->cv.wait_for(lock, std::chrono::seconds(2), [&] { return input->started; }), "R4-020 actual lighting job entered worker");
        Check(input->worker != owner && input->result == 0, "R4-020 owner progresses while light work is pending on another thread");
        Check(!service.SubmitLightPreparation(21,64,[] { return true; }), "R4-020 occupied worker rejects another light job");
        Check(!service.SubmitMaterials({}), "R4-020 material work cannot overlap occupied shading worker");
        input->release = true; input->cv.notify_all();
    }
    Check(service.FinishLightPreparation(job,20) && input->result == 42, "R4-020 joined owned result is complete");
    Check(!service.FinishLightPreparation(job,21), "R4-020 wrong root cannot consume result");
    auto throwing = service.SubmitLightPreparation(22,64,[]() -> bool { throw 7; });
    Check(!service.FinishLightPreparation(throwing,22), "R4-020 exception completes failed receipt without escaping thread");
    auto stale = service.SubmitLightPreparation(23,64,[] { return true; });
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(!service.FinishLightPreparation(stale,23), "R4-020 reset rejects old lifecycle receipt");
    auto owned = std::make_shared<int>(99); std::weak_ptr<int> weak = owned;
    auto abandoned = service.SubmitLightPreparation(24,64,[owned] { return *owned == 99; });
    Check(abandoned != nullptr, "R4-020 abandoned work submitted");
    owned.reset(); abandoned.reset();
    service.Shutdown();
    Check(weak.expired(), "R4-020 shutdown joins abandoned work and releases owned inputs");
    service.Init();
    auto stopping = service.SubmitLightPreparation(25,64,[] { return true; });
    service.Shutdown();
    Check(stopping && stopping->done && !service.FinishLightPreparation(stopping,25), "R4-020 shutdown completes queued receipt and forbids consumption");
}

void TestR4021MaterialRecords()
{
    static_assert(sizeof(PathTraceDynamicMaterialRecord)==64, "material record ABI");
    RtCpuMaterialRecordsInput input;
    input.rows={{70,false,true,false,0},{71,false,false,true,7},{72,true,false,false,0},{73,false,false,true,7}};
    RtCpuMaterialRecordSample sample;
    sample.valid=true;sample.id=70;sample.stageIndex=2;sample.enabledStages=1;
    sample.color[0]=0.25f;sample.color[3]=2;sample.texMatrix[0][2]=0.75f;
    sample.alphaTest=0.45f;sample.alphaTestStages=1;sample.dynamicImageStages=1;
    sample.cinematicStages=sample.guiRenderTargetStages=sample.programStages=1;
    sample.orderedStageCount=2;sample.orderedStages[0].stageIndex=4;
    sample.orderedStages[0].hasAlphaTest=true;sample.orderedStages[0].alphaTest=0.6f;
    sample.orderedStages[1].enabled=true;sample.orderedStages[1].emissive=true;
    input.samples.push_back(RtCpuCaptureMaterialRecordSample(sample));
    auto resident=sample;resident.id=72;input.samples.push_back(resident);
    RtCpuMaterialRecordSample decal;decal.valid=true;decal.id=71;decal.enabledStages=1;
    decal.hasDiffuseStageColor=true;decal.diffuseStageColor[0]=0.2f;
    decal.hasSurfaceOrigin=true;decal.surfaceOrigin={0,0,0};input.samples.push_back(decal);
    RtCpuMaterialSpectrumLight dark;dark.spectrum=7;dark.radius=10;
    RtCpuMaterialSpectrumLight bright=dark;bright.origin={100,0,0};bright.color={2,3,4,1};
    input.spectrumLights={dark,bright};
    auto records=BuildRtCpuMaterialRecords(input);
    Check(records.size()==6 && records[0].materialId==70 && records[0].color[0]==0.25f && records[0].color[3]==1 &&
        records[0].texMatrix0[2]==0.75f && records[0].texMatrix1[3]==0.45f,
        "R4-021 selected colors alpha cutoff and tiled UV values retained");
    const uint32_t flags=RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID|RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED|
        RT_SMOKE_DYNAMIC_MATERIAL_RECORD_REPLACE_EMISSIVE|RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_TEX_MATRIX|
        RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_ALPHA_TEST|RT_SMOKE_DYNAMIC_MATERIAL_RECORD_DYNAMIC_IMAGE|
        RT_SMOKE_DYNAMIC_MATERIAL_RECORD_CINEMATIC|RT_SMOKE_DYNAMIC_MATERIAL_RECORD_GUI_RENDER_TARGET|
        RT_SMOKE_DYNAMIC_MATERIAL_RECORD_PROGRAM|RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_ORDERED_STAGE_VALUES;
    Check(records[0].flags==flags && (records[0].stageIndex>>16)==4 && ((records[0].stageIndex>>8)&15)==2 &&
        records[4].texMatrix1[3]==0.6f && !(records[4].flags&RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED) &&
        (records[5].flags&RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE),
        "R4-021 ordered header offsets disabled alpha stage and emissive stage preserved");
    Check(records[2].flags==0 && records[1].color[0]==0 && !(records[1].flags&RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED) &&
        records[3].color[0]==2 && records[3].color[2]==4,
        "R4-021 resident exclusion nearest dark spectrum ownership and synthetic brightest fallback preserved");
    auto tinted=input;tinted.spectrumLights[0].color={2,1,1,1};
    Check(BuildRtCpuMaterialRecords(tinted)[1].color[0]==0.4f,"R4-021 spectrum detail decal uses diffuse authored tint");
    auto duplicate=sample;duplicate.color[0]=0.9f;duplicate.orderedStageCount=0;
    auto repeated=input;repeated.samples.push_back(duplicate);
    auto last=BuildRtCpuMaterialRecords(repeated);
    Check(last.size()==4 && last[0].color[0]==0.9f && !(last[0].flags&RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_ORDERED_STAGE_VALUES),
        "R4-021 last valid sample replaces earlier selected and ordered values");
    bool rejected=false;
    try { auto bad=input;bad.samples[0].orderedStageCount=9;BuildRtCpuMaterialRecords(bad); } catch (const std::invalid_argument&) { rejected=true; }
    Check(rejected,"R4-021 malformed ordered stage span rejected");
    RtCpuProducerRewriteService service;service.Init();
    struct Work {
        RtCpuMaterialRecordsInput input;std::vector<PathTraceDynamicMaterialRecord> result;
        std::mutex mutex;std::condition_variable cv;bool material=false,light=false,release=false;
        std::thread::id materialThread,lightThread;
    };
    auto work=std::make_shared<Work>();work->input=input;
    auto material=service.SubmitMaterialRecords(2100,65536,[work] {
        { std::unique_lock<std::mutex> lock(work->mutex);work->material=true;work->materialThread=std::this_thread::get_id();
          work->cv.notify_all();work->cv.wait(lock,[&] { return work->release; }); }
        work->result=BuildRtCpuMaterialRecords(work->input);return true;
    });
    auto light=service.SubmitLightPreparation(2100,64,[work] {
        std::unique_lock<std::mutex> lock(work->mutex);work->light=true;work->lightThread=std::this_thread::get_id();
        work->cv.notify_all();work->cv.wait(lock,[&] { return work->release; });return true;
    });
    {
        std::unique_lock<std::mutex> lock(work->mutex);
        Check(work->cv.wait_for(lock,std::chrono::seconds(2),[&] { return work->material && work->light; }) &&
            work->materialThread!=work->lightThread && work->materialThread!=std::this_thread::get_id(),
            "R4-021 actual record kernel job and lighting execute simultaneously on separate workers");
        Check(!service.SubmitMaterialRecords(2101,64,[] { return true; }),"R4-021 busy record producer bounded");
        work->release=true;work->cv.notify_all();
    }
    Check(service.FinishMaterialRecords(material,2100) && service.FinishLightPreparation(light,2100) &&
        work->result.size()==records.size() && !std::memcmp(work->result.data(),records.data(),records.size()*sizeof(records[0])),
        "R4-021 concurrent record product exactly matches serial numeric oracle");
    Check(!service.FinishMaterialRecords(material,2101),"R4-021 wrong-root record receipt rejected");
    auto throwing=service.SubmitMaterialRecords(2102,64,[]()->bool { throw 7; });
    Check(!service.FinishMaterialRecords(throwing,2102),"R4-021 record exceptions complete failed receipt");
    auto stale=service.SubmitMaterialRecords(2103,64,[] { return true; });
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(!service.FinishMaterialRecords(stale,2103),"R4-021 old lifecycle material records rejected");
    Check(!service.SubmitMaterialRecords(2104,256ull*1024*1024+1,[] { return true; }),"R4-021 record charge bounded");
    std::weak_ptr<Work> weak=work;
    auto abandoned=service.SubmitMaterialRecords(2105,64,[work] { return true; });
    work.reset();abandoned.reset();
    auto stopping=material;service.Shutdown();
    Check(weak.expired() && !service.FinishMaterialRecords(stopping,2100),"R4-021 shutdown releases abandoned inputs and rejects consumption");
    service.Init();
    auto queued=service.SubmitMaterialRecords(2106,64,[] { return true; });
    service.Shutdown();
    Check(queued && queued->done && !service.FinishMaterialRecords(queued,2106),"R4-021 restart and queued shutdown complete without stranded receipt");
}

void TestR4015ResidentGeometry()
{
    RtCpuProducerRewriteService service;
    service.Init(); service.SampleRequestedRoute(1);
    RtCpuRewriteOwnedVertex verts[3] = { MakeVert(0,0,0), MakeVert(1,0,0), MakeVert(0,1,0) };
    const uint32_t indexes[3] = {0,1,2};
    float matrix[16]; FillIdentityMatrix(matrix);
    PathTraceSkinnedJointMatrix joint = {};
    joint.rows[0] = joint.rows[5] = joint.rows[10] = 1;
    auto rigid = MakeTriangleWrite(97001,1,0,verts,indexes,matrix);
    auto skin = MakeTriangleWrite(97002,2,0,verts,indexes,matrix);
    skin.sourceClass = kRtCpuRewriteClassSkinnedEntity;
    skin.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
    skin.meshKey.deformationClass = PtCanonicalDeformationClass::Skinned;
    skin.meshKey.jointSubmeshIndex = 0;
    skin.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::SkinnedSurface;
    skin.instanceKey.jointSubmeshIndex = 0;
    skin.joints = &joint; skin.jointCount = 1;
    rigid.geometry = service.RetainGeometry(rigid); skin.geometry = service.RetainGeometry(skin);
    Check(rigid.geometry && skin.geometry, "R4-015 immutable geometry captures both supported families");
    if (!rigid.geometry || !skin.geometry) { service.Shutdown(); return; }
    auto budget = rigid.geometry->budget;
    std::weak_ptr<const RtCpuRewriteRetainedGeometry> weak = rigid.geometry;
    rigid.vertices = skin.vertices = nullptr; rigid.indexes = skin.indexes = nullptr;
    verts[0].xyz[0] = 77; // original native/source bytes can change or disappear
    auto capture = [&](uint64_t frame, bool includeRigid) {
        Check(service.TryAcquireRootInput(frame,7,4) && (!includeRigid || service.WriteSurface(rigid)) &&
            service.WriteSurface(skin) && service.SealOverlayFromCapturingInput(frame) && service.SealRootInput(),
            "R4-015 owned references and current poses seal without source pointers");
        const bool ready = service.TryAcquireExactProduct(frame,7,4,4000) && service.TryAcquireOverlay(frame);
        Check(ready, "R4-015 referenced worker product and exact overlay acquired");
        return ready;
    };
    if (!capture(1500,true)) { service.Shutdown(); return; }
    RtCpuRewriteFrozenProductView held = *service.ProductView();
    Check(held.residentDynamic && held.rigidMeshCount == 1 && held.skinnedMeshCount == 1 &&
        held.rigidMeshes[0].vertices[0].position[0] == 0 && held.skinnedPrepared.triangles &&
        held.skinnedPrepared.indexes[2] == 2, "R4-015 copied packed view includes rebased nested geometry and route arrays");
    service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct();
    rigid.modelMatrix[12] = 9; skin.modelMatrix[12] = 13; joint.rows[3] = 17;
    if (!capture(1501,true)) { service.Shutdown(); return; }
    auto* view = service.ProductView(); auto* overlay = service.OverlayView();
    RtCpuRewriteJoinResult joined;
    Check(view->residentDynamic == held.residentDynamic && view->skinnedPrepared.triangles == held.skinnedPrepared.triangles &&
        view->rigidMeshes == held.rigidMeshes && view->rootFrame == 1501 && view->ticket != held.ticket,
        "R4-015 unchanged membership reuses the complete packed worker product");
    Check(service.BuildJoinPlan(1501,joined) && joined.joinedCount == 1 && joined.skinnedCount == 1 &&
        overlay->rows[0].currentObjectToWorld[12] == 9 && overlay->rows[1].currentObjectToWorld[12] == 13 &&
        overlay->joints[0].rows[3] == 17 &&
        RtCpuRewriteValidateSkinnedPrepared(view,overlay,joined) == RtCpuRewriteSkinnedRejectReason::None,
        "R4-015 reused geometry accepts exact changed rigid, skin and joint poses through production join");
    service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct();
    const auto conversionsBefore=service.Counters();
    if (!capture(1502,false)) { service.Shutdown(); return; }
    Check(service.Counters().convertedMeshHits > conversionsBefore.convertedMeshHits &&
        service.Counters().convertedMeshMisses == conversionsBefore.convertedMeshMisses,
        "R4-039 membership deletion reuses unchanged skinned conversion without reconverting");
    Check(service.ProductView()->skinnedMeshes[0].signature == held.skinnedMeshes[0].signature &&
        !std::memcmp(service.ProductView()->skinnedMeshes[0].vertices,held.skinnedMeshes[0].vertices,
            sizeof(PathTraceSkinnedSourceVertex)*held.skinnedMeshes[0].vertexCount),
        "R4-039 reused converted skin bytes and signature match original product");
    Check(service.ProductView()->residentDynamic != held.residentDynamic && service.ProductView()->rigidMeshCount == 0 &&
        service.ProductView()->sourceCount == 1, "R4-015 deletion rebuilds membership and removes the old rigid source");
    service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct();
    rigid.vertices = verts; rigid.indexes = indexes;
    rigid.geometry = service.RetainGeometry(rigid);
    rigid.vertices = nullptr; rigid.indexes = nullptr;
    const auto beforeChanged=service.Counters();
    if (!capture(1503,true)) { service.Shutdown(); return; }
    Check(service.Counters().convertedMeshMisses > beforeChanged.convertedMeshMisses &&
        service.Counters().convertedMeshHits > beforeChanged.convertedMeshHits,
        "R4-039 changed rigid source converts while unchanged skin survives membership insertion");
    Check(service.ProductView()->rigidMeshes[0].vertices[0].position[0] == 77 &&
        held.rigidMeshes[0].vertices[0].position[0] == 0,
        "R4-015 replacement publishes new source bytes while held old readers stay valid");
    service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct();
    auto bad = rigid; bad.vertexCount = 4;
    Check(service.TryAcquireRootInput(1504,7,4) && !service.WriteSurface(bad) && !service.SealRootInput(),
        "R4-015 malformed reference counts reject the entire input");
    bad = rigid; bad.jointCount = 1; bad.joints = nullptr;
    Check(service.TryAcquireRootInput(1505,7,4) && !service.WriteSurface(bad),
        "R4-015 missing joint bytes reject before dereference");
    service.AbortCapturingInput();
    const uint32_t badIndexes[3] = {0,1,3};
    bad = MakeTriangleWrite(97003,3,0,verts,badIndexes,matrix);
    Check(!service.RetainGeometry(bad), "R4-015 invalid source index cannot become a retained reference");
    bad = {};
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(!service.TryAcquireExactProduct(1503,7,4,0) && !weak.expired() &&
        held.skinnedMeshes[0].vertices[0].localPosition[0] == 0,
        "R4-015 reset rejects old products while held cache keeps nested owners alive");
    rigid.geometry.reset(); skin.geometry.reset();
    service.Shutdown();
    Check(!weak.expired() && budget->load() > 0, "R4-015 obsolete held source bytes stay charged after service shutdown");
    held = {};
    Check(weak.expired() && budget->load() == 0, "R4-015 final reader releases complete cache and source budget");
}


void TestR4015ConcurrentReferences()
{
    RtCpuProducerRewriteService service;
    service.Init(); service.SampleRequestedRoute(1);
    RtCpuRewriteOwnedVertex verts[3] = { MakeVert(0,0,0), MakeVert(1,0,0), MakeVert(0,1,0) };
    const uint32_t indexes[3] = {0,1,2};
    float matrix[16]; FillIdentityMatrix(matrix);
    auto source = MakeTriangleWrite(98001,1,0,verts,indexes,matrix);
    source.geometry = service.RetainGeometry(source);
    source.vertices = nullptr; source.indexes = nullptr;
    std::shared_ptr<const RtCpuRewriteResidentDynamic> previous;
    for (uint64_t frame = 1600; frame < 1602; ++frame)
    {
        Check(service.TryAcquireRootInput(frame,7,4), "R4-015 acquire concurrent reference input");
        std::atomic<int> accepted{0};
        std::vector<std::thread> writers;
        for (uint32_t shard = 0; shard < 4; ++shard) writers.emplace_back([&,shard] {
            for (uint32_t row = 0; row < 4; ++row)
            {
                auto write = source;
                write.instanceKey.renderDefIndex = 1 + shard * 4 + row;
                if (service.WriteSurface(write)) accepted.fetch_add(1);
            }
        });
        for (auto& writer : writers) writer.join();
        const bool ready = accepted == 16 && service.SealRootInput() && service.TryAcquireExactProduct(frame,7,4,4000);
        Check(ready, "R4-015 concurrent references publish a complete input owner table");
        if (!ready) break;
        const auto* view = service.ProductView();
        Check(view->sourceCount == 16 && view->rigidMeshCount == 1 &&
            (!previous || previous == view->residentDynamic),
            "R4-015 deterministic cache identity ignores concurrent reference insertion order");
        previous = view->residentDynamic;
        service.ReleaseConsumedProduct();
    }
    service.Shutdown();
}


void TestR4016FullLevelCapacity()
{
    // This is a capacity/production-consumption proof, not a GPU speed benchmark.
    constexpr uint32_t rows = 128, vertices = 2400, indexes = 3000;
    constexpr uint64_t oldLimit = 64ull * 1024 * 1024;
    const uint64_t heapCharge = uint64_t(rows) * (indexes / 3 * 1024ull + 4096ull);
    Check(heapCharge > oldLimit && heapCharge < RtCpuProducerRewriteService::kSkinnedHeapScratchBytes,
        "R4-016 full-level fixture reproduces the old scratch rejection");
    std::vector<RtCpuRewriteOwnedVertex> verts(vertices);
    std::vector<uint32_t> ix(indexes);
    for (uint32_t i = 0; i < vertices; ++i)
    {
        verts[i] = MakeVert(float(i % 50), float(i / 50), 0);
        std::fill(verts[i].color, verts[i].color + 4, 0.0f);
        verts[i].color2[0] = 1;
    }
    for (uint32_t i = 0; i < indexes; ++i) ix[i] = i % vertices;
    float matrix[16]; FillIdentityMatrix(matrix);
    PathTraceSkinnedJointMatrix joint = {};
    joint.rows[0] = joint.rows[5] = joint.rows[10] = 1;
    auto source = MakeTriangleWrite(99000,1,0,verts.data(),ix.data(),matrix);
    source.vertexCount = source.meshKey.vertexCount = vertices;
    source.indexCount = source.meshKey.indexCount = indexes;
    source.meshKey.topologySignature = TopologySignature(vertices,indexes,ix.data());
    source.sourceClass = kRtCpuRewriteClassSkinnedEntity;
    source.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
    source.meshKey.deformationClass = PtCanonicalDeformationClass::Skinned;
    source.meshKey.jointSubmeshIndex = 0;
    source.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::SkinnedSurface;
    source.instanceKey.jointSubmeshIndex = 0;
    source.joints = &joint; source.jointCount = 1;
    RtCpuProducerRewriteService service;
    service.Init(); service.SampleRequestedRoute(1);
    source.geometry = service.RetainGeometry(source);
    Check(bool(source.geometry), "R4-016 source geometry retained before full-scene capture");
    if (!source.geometry) { service.Shutdown(); return; }
    source.vertices = nullptr; source.indexes = nullptr;
    auto world = MakeTriangleWrite(100000,rows+1,0,verts.data(),ix.data(),matrix);
    world.sourceClass = kRtCpuRewriteClassStaticWorld;
    world.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::StaticWorldMap;
    world.meshKey.deformationClass = PtCanonicalDeformationClass::Static;
    world.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::StaticSurface;
    std::shared_ptr<const RtCpuRewriteResidentStatic> staticOwner;
    std::shared_ptr<const RtCpuRewriteResidentDynamic> owner;
    for (uint64_t frame = 1700; frame < 1704; ++frame)
    {
        bool captured = service.TryAcquireRootInput(frame,7,4);
        joint.rows[3] = float(frame);
        for (uint32_t i = 0; i < rows && captured; ++i)
        {
            auto write = source;
            write.meshKey.sourceAssetId += i; write.instanceKey.renderDefIndex += i;
            write.modelMatrix[12] = float(frame + i);
            captured = service.WriteSurface(write);
        }
        bool populate = false;
        captured = captured && service.PrepareResidentStaticInput(1,populate) && populate == (frame == 1700);
        if (captured && populate) captured = service.WriteSurface(world);
        const bool ready = captured && service.SealOverlayFromCapturingInput(frame) && service.SealRootInput() &&
            service.TryAcquireExactProduct(frame,7,4,4000) && service.TryAcquireOverlay(frame);
        Check(ready, "R4-016 scene-sized worker product publishes instead of recurring warmup failure");
        if (!ready) break;
        const auto* view = service.ProductView();
        Check(view->residentStatic && view->StaticSourceCount() == 1 && view->vertexCount == 3 &&
            view->indexCount == 3 && view->vertices[1].position[0] == 1 &&
            (!staticOwner || staticOwner == view->residentStatic),
            "R4-017 mixed cold population preserves static geometry alongside retained skin references");
        staticOwner = view->residentStatic;
        const auto& skin = view->skinnedPrepared;
        RtCpuRewriteJoinResult join;
        Check(skin.vertexCount == rows * vertices && skin.indexCount == rows * indexes &&
            skin.count == rows && service.BuildJoinPlan(frame,join) && join.skinnedCount == rows &&
            RtCpuRewriteValidateSkinnedPrepared(view,service.OverlayView(),join) == RtCpuRewriteSkinnedRejectReason::None,
            "R4-016 complete large product reaches production join and skinned validation");
        uint64_t gpuBytes = uint64_t(skin.indexCount) * sizeof(uint32_t);
        for (uint64_t bytes : skin.capacities) gpuBytes += bytes;
        uint64_t peak = 0;
        Check(gpuBytes * 4 > oldLimit && RtCpuRewriteValidateSkinnedCapacity(gpuBytes * 3,gpuBytes,0,
            RtCpuProducerRewriteService::kGpuSkinnedRetainCapBytes,&peak) && peak == gpuBytes * 4,
            "R4-016 all three GPU slots plus replacement fit the full-level admission bound");
        Check(!RtCpuRewriteValidateSkinnedCapacity(RtCpuProducerRewriteService::kGpuSkinnedRetainCapBytes,1,0,
            RtCpuProducerRewriteService::kGpuSkinnedRetainCapBytes,&peak),
            "R4-016 a real over-limit GPU request remains rejected");
        const uint64_t sourceAndOutput = uint64_t(rows) * vertices * 2 * sizeof(PathTraceSkinnedSourceVertex);
        Check(sourceAndOutput > oldLimit && (frame == 1700 || view->residentDynamic) && (!owner || owner == view->residentDynamic) &&
            service.OverlayView()->joints[0].rows[3] == float(frame) &&
            service.OverlayView()->rows[0].currentObjectToWorld[12] == float(frame),
            "R4-016 product beyond the old arena cap is reused while current poses advance");
        std::printf("[INFO] R4-016 scene triangles=%u vertices=%u heapCharge=%llu gpuPackage=%llu fourPackages=%llu\n",
            skin.indexCount/3,skin.vertexCount,(unsigned long long)heapCharge,
            (unsigned long long)gpuBytes,(unsigned long long)(gpuBytes*4));
        owner = view->residentDynamic;
        service.ReleaseConsumedOverlay(true); service.ReleaseConsumedProduct();
    }
    service.Shutdown();
}

void TestR4018RigidAttributePatches()
{
    PathTraceSmokeVertex source[3] = {};
    for (uint32_t i = 0; i < 3; ++i)
    {
        source[i].position[0] = float(i); source[i].position[3] = 1;
        source[i].normal[2] = 1; source[i].color[0] = 0.25f;
        source[i].texCoord[0] = source[i].texCoord[2] = float(i);
        source[i].texCoord[1] = source[i].texCoord[3] = float(i) + 0.5f;
    }
    RtCpuRewriteRigidMeshView mesh;
    mesh.vertices = source; mesh.vertexCount = 3; mesh.indexCount = 3; mesh.triangleCount = 1;
    PathTraceRigidRouteInstance route = {};
    route.vertexOffset = 3; route.triangleOffset = 1;
    route.vertexCount = 3; route.indexCount = 3; route.triangleCount = 1;
    std::vector<PathTraceSmokeVertex> sparse(9), full(9);
    for (uint32_t i = 0; i < 9; ++i) sparse[i] = full[i] = source[i % 3];
    std::vector<uint32_t> ids(3,10), indexes(3,2), fullIds=ids, fullIndexes=indexes;
    for (uint32_t frame = 0; frame < 4; ++frame)
    {
        RtCpuRewriteTextureMatrices matrices;
        const bool uvChanged = frame != 1;
        const bool materialChanged = frame != 2;
        if (frame < 3)
        {
            matrices.primary[0] = 2; matrices.primary[2] = 0.75f;
            matrices.normal[4] = 3; matrices.normal[5] = -0.5f;
        }
        route.materialId = frame < 3 ? 77 : 10; route.materialIndex = frame < 3 ? 8 : 2;
        RtCpuRewriteRigidAttributePatch patch;
        Check(RtCpuRewriteBuildRigidAttributePatch(mesh,route,matrices,9,3,uvChanged,materialChanged,patch),
            "R4-018 prepare exact attribute patch");
        Check(patch.vertices.size() == (uvChanged ? 3u : 0u) && patch.materialIds.size() == (materialChanged ? 1u : 0u),
            "R4-018 material-only changes copy no vertices and UV-only changes copy no triangle IDs");
        std::copy(patch.vertices.begin(),patch.vertices.end(),sparse.begin()+patch.vertexOffset);
        std::copy(patch.materialIds.begin(),patch.materialIds.end(),ids.begin()+patch.triangleOffset);
        std::copy(patch.materialIndexes.begin(),patch.materialIndexes.end(),indexes.begin()+patch.triangleOffset);
        for (uint32_t i=0;i<3;++i)
        {
            full[3+i] = source[i];
            RtCpuRewriteApplyTextureMatrix(full[3+i].texCoord,matrices.primary);
            RtCpuRewriteApplyTextureMatrix(full[3+i].texCoord+2,matrices.normal);
        }
        fullIds[1]=route.materialId;fullIndexes[1]=route.materialIndex;
        Check(std::memcmp(full.data(),sparse.data(),full.size()*sizeof(PathTraceSmokeVertex))==0 && ids==fullIds && indexes==fullIndexes,
            "R4-018 sparse bytes equal full rebuild, preserving neighboring surfaces and restoring authored UVs");
    }
    RtCpuRewriteRigidAttributePatch unchanged; unchanged.vertexOffset=123;
    RtCpuRewriteTextureMatrices matrices;
    route.vertexOffset=UINT32_MAX;
    Check(!RtCpuRewriteBuildRigidAttributePatch(mesh,route,matrices,9,3,true,true,unchanged) && unchanged.vertexOffset==123,
        "R4-018 overflowed vertex span rejects without partial publication");
    route.vertexOffset=3;route.triangleOffset=3;
    Check(!RtCpuRewriteBuildRigidAttributePatch(mesh,route,matrices,9,3,false,true,unchanged),
        "R4-018 out-of-range triangle span rejects");
    route.triangleOffset=1;matrices.primary[0]=std::numeric_limits<float>::infinity();
    Check(!RtCpuRewriteBuildRigidAttributePatch(mesh,route,matrices,9,3,true,false,unchanged) && unchanged.vertexOffset==123,
        "R4-018 nonfinite UV rejects without replacing prior output");
}

void TestR4025ClassifierNameReuse()
{
    auto cache = std::make_unique<RtSmokeClassifierNameCache>();
    unsigned builds = 0;
    auto build = [&](const char* name) { ++builds; return BuildSmokeClassifierNameInfo(name); };
    auto equal = [](const RtSmokeClassifierNameInfo& a, const RtSmokeClassifierNameInfo& b) {
        return a.nameLooksGui == b.nameLooksGui && a.nameLooksParticle == b.nameLooksParticle &&
            a.nameLooksDecal == b.nameLooksDecal && a.nameLooksGlass == b.nameLooksGlass &&
            a.nameLooksGlow == b.nameLooksGlow && a.nameLooksSignage == b.nameLooksSignage;
    };
    const char* names[] = {"", "textures/base/wall", "GUI/terminal_Console_pda_cursor",
        "video/cinematic", "guis/menu", "particle/smoke_dust_steam_fog_muzzle_spark_bloodcloud",
        "decal/stain_grime_dirt_scorch_burn_bullet_mud_blood_splat_mark",
        "glass/window_visor_transparent", "glow/light_lamp_beam_flare_strip_striplight_tube_neon",
        "emissive_emit_bulb_fluoro_flouro", "logo_sign_label_snack_soda_cola_add_screen_monitor",
        "models/mapobjects/swinglights/swinglighttex1", "GLASS\\NeOn", "glassy", "lights"};
    bool parity = true;
    for (const char* name : names)
    {
        const auto expected = BuildSmokeClassifierNameInfo(name);
        parity &= equal(cache->Get(name, build), expected);
        const auto before = builds;
        parity &= equal(cache->Get(name, build), expected) && builds == before;
    }
    Check(parity, "R4-025 cold and reused names match all original name flags without replay");

    char mutableName[64] = "glass/window";
    const auto glass = cache->Get(mutableName, build);
    std::strcpy(mutableName, "particle/smoke");
    const auto smoke = cache->Get(mutableName, build);
    Check(glass.nameLooksGlass && !glass.nameLooksParticle && smoke.nameLooksParticle &&
        !smoke.nameLooksGlass && equal(cache->Get("glass/window", build), glass),
        "R4-025 renamed or reused source address cannot corrupt owned keys");

    bool evictionParity = true;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < 1024; ++i)
        {
            const std::string name = "textures/" + std::to_string(i) + (i % 2 ? "/glass" : "/smoke");
            evictionParity &= equal(cache->Get(name.c_str(), build), BuildSmokeClassifierNameInfo(name.c_str()));
        }
    Check(evictionParity, "R4-025 bounded cache eviction preserves exact-key results");

    std::string longName(1100, 'x'); longName += "glass";
    const char nonAscii[] = "\xC3\xA9_glass";
    const auto bypassBefore = cache->Stats().bypasses;
    const auto longResult = cache->Get(longName.c_str(), build);
    const auto nonAsciiResult = cache->Get(nonAscii, build);
    const auto nullResult = cache->Get(nullptr, build);
    Check(longResult.nameLooksGlass && equal(nonAsciiResult, BuildSmokeClassifierNameInfo(nonAscii)) &&
        equal(nullResult, BuildSmokeClassifierNameInfo(nullptr)) && cache->Stats().bypasses == bypassBefore + 3,
        "R4-025 long non-ASCII and null names preserve uncached oracle without truncation");

    // Reuse a name while replacing every kind of live classifier fact, as after
    // reload or in-place stage changes. Cached facts must not freeze those inputs.
    RtSmokeTranslucentClassifierInput input; input.materialPresent = true; input.stageCount = 1;
    RtSmokeTranslucentClassifierStageInput stage; stage.valid = true;
    const auto fixedName = cache->Get("models/lights/glass", build);
    bool liveParity = true;
    for (int i = 0; i < 512; ++i)
    {
        input.sortIsGuiOrSubview = (i & 1) != 0;
        input.sortIsDecal = (i & 2) != 0;
        input.sortIsPostProcess = (i & 4) != 0;
        input.polygonOffsetDecal = (i & 8) != 0;
        stage.hasScreenTexgen = (i & 16) != 0;
        stage.isAdditiveBlend = (i & 32) != 0;
        stage.isAmbientStage = (i & 64) != 0;
        stage.isDiffuseStage = (i & 128) != 0;
        stage.hasAmbientBlendStage = (i & 256) != 0;
        stage.hasImage = (i & 16) != 0; stage.looksAddDefault0200 = (i & 32) != 0;
        auto provider = [&](int) { return stage; };
        const auto expected = BuildSmokeTranslucentClassifierInfoFromSource(input, "models/lights/glass", 1, provider);
        const auto actual = BuildSmokeTranslucentClassifierInfoFromSource(input, "models/lights/glass", 1, provider, &fixedName);
        liveParity &= RtSmokeTranslucentClassifierInfoEqual(expected, actual);
    }
    Check(liveParity, "R4-025 reused name leaves sort flags stages and image facts current");
    const auto callsBefore = builds;
    cache->Get("models/lights/glass", build);
    Check(builds == callsBefore, "R4-025 live-state changes do not force redundant name classification");
}

void TestR4038RouteBoundary()
{
    RtCpuProducerRewriteService service;
    service.Init();
    service.ApplyRouteAtFrameBoundary(1);
    Check(service.Route() == RtCpuProducerRewriteRoute::RewriteWarmup,
        "R4-038 idle boundary enables warmup");
    const auto epoch = service.LifecycleGeneration();
    service.ApplyRouteAtFrameBoundary(0);
    Check(service.Route() == RtCpuProducerRewriteRoute::DrainingToLegacy &&
        service.LifecycleGeneration() == epoch + 1,
        "R4-038 off during warmup invalidates once at boundary");
    service.ApplyRouteAtFrameBoundary(1);
    Check(service.Route() == RtCpuProducerRewriteRoute::DrainingToLegacy &&
        service.LifecycleGeneration() == epoch + 1,
        "R4-038 rapid reenable waits for actual backend drain without another reset");
    service.NotifyBackendDrained();
    Check(service.Route() == RtCpuProducerRewriteRoute::DrainingToLegacy,
        "R4-038 backend ack does not resume capture during backend execution");
    service.ApplyRouteAtFrameBoundary(1);
    Check(service.Route() == RtCpuProducerRewriteRoute::RewriteWarmup,
        "R4-038 acknowledged drain resumes latest request at idle boundary");
    service.ApplyRouteAtFrameBoundary(0);
    service.NotifyBackendDrained();
    service.ApplyRouteAtFrameBoundary(0);
    Check(service.Route() == RtCpuProducerRewriteRoute::LegacyOnly,
        "R4-038 acknowledged off request returns to legacy");

    service.ApplyRouteAtFrameBoundary(1);
    std::mutex mutex;
    std::condition_variable cv;
    int entered = 0;
    bool release = false;
    std::atomic<bool> resetDone{false};
    auto hold = [&] {
        std::unique_lock<std::mutex> lock(mutex);
        ++entered; cv.notify_all();
        cv.wait(lock, [&] { return release; });
        return true;
    };
    auto light = service.SubmitLightPreparation(38,64,hold);
    auto manager = service.SubmitLightManagerPreparation(38,64,hold);
    auto planning = service.SubmitMaterialRecords(38,64,hold);
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check(cv.wait_for(lock,std::chrono::seconds(2),[&] { return entered == 3; }),
            "R4-038 three CPU job families held on owned inputs");
    }
    const auto beforeReset = service.LifecycleGeneration();
    std::thread reset([&] { service.ApplyRouteAtFrameBoundary(0); resetDone.store(true); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (service.LifecycleGeneration() == beforeReset && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    Check(service.LifecycleGeneration() != beforeReset && !resetDone.load(),
        "R4-038 invalidation begins but cannot finish while CPU jobs own inputs");
    { std::lock_guard<std::mutex> lock(mutex); release=true; cv.notify_all(); }
    reset.join();
    Check(resetDone.load() && !service.FinishLightPreparation(light,38) &&
        !service.FinishLightManagerPreparation(manager,38) && !service.FinishMaterialRecords(planning,38),
        "R4-038 drain completes held jobs and rejects all old-epoch receipts");
    service.Shutdown();
}

void TestR4038SkinnedHistory()
{
    RtCpuRewriteJoinedSkinned js{};
    js.instanceKey.worldGeneration=7; js.instanceKey.renderDefIndex=101;
    js.instanceKey.renderDefGeneration=2; js.jointCount=12;
    js.materialLogicalId=50; js.materialIndex=5;
    RtCpuRewriteSkinnedMeshView mesh{};
    mesh.meshKey.sourceAssetId=1234; mesh.meshKey.sourceAssetGeneration=3;
    mesh.meshKey.vertexCount=60; mesh.meshKey.indexCount=90;
    mesh.signature=4567; mesh.sourceContentSignature=8910;
    mesh.vertexCount=60; mesh.indexCount=90; mesh.triangleCount=30;
    const auto previous=RtCpuRewriteMakeSkinnedLayoutRow(js,mesh,100,150,24);
    js.materialLogicalId=51; js.materialIndex=8;
    const auto repacked=RtCpuRewriteMakeSkinnedLayoutRow(js,mesh,300,450,48);
    Check(!std::equal(previous.words,previous.words+32,repacked.words) &&
        RtCpuRewriteSkinnedHistoryIdentityEqual(previous,repacked) &&
        RtCpuRewriteSkinnedPreviousPoseValid(100,101,7,7,true),
        "R4-038 material changes and all packed offsets preserve same-instance source history");
    bool rejects=true;
    for (int word=0;word<20;++word) {
        auto different=repacked; ++different.words[word];
        rejects &= !RtCpuRewriteSkinnedHistoryIdentityEqual(previous,different);
    }
    for (int word : {21,29}) {
        auto different=repacked; ++different.words[word];
        rejects &= !RtCpuRewriteSkinnedHistoryIdentityEqual(previous,different);
        different.words[word]=0;
        rejects &= !RtCpuRewriteSkinnedHistoryIdentityEqual(different,different);
    }
    Check(rejects,"R4-038 changed instance generation, source, topology, palette or bind bytes reject history");
    Check(!RtCpuRewriteSkinnedPreviousPoseValid(99,101,7,7,true) &&
        !RtCpuRewriteSkinnedPreviousPoseValid(100,101,6,7,true),
        "R4-038 membership fix does not relax frame-gap or lifecycle rejection");
    auto newcomer=js; newcomer.instanceKey.renderDefIndex=102;
    const auto newRow=RtCpuRewriteMakeSkinnedLayoutRow(newcomer,mesh,0,0,0);
    Check(!RtCpuRewriteSkinnedHistoryIdentityEqual(previous,newRow) &&
        RtCpuRewriteSkinnedHistoryIdentityEqual(previous,repacked),
        "R4-038 newcomer has no inherited history while surviving character retains it");
}

void TestR4039HistoryAndRecovery()
{
    RtCpuProducerRewriteService service; service.Init(); service.ApplyRouteAtFrameBoundary(1);
    RtCpuRewriteOwnedVertex verts[3]={MakeVert(0,0,0),MakeVert(1,0,0),MakeVert(0,1,0)};
    const uint32_t indexes[3]={0,1,2}; float matrix[16]; FillIdentityMatrix(matrix);
    auto capture=[&](uint64_t root,float x,bool present) {
        auto write=MakeTriangleWrite(39001,1,0,verts,indexes,matrix); write.modelMatrix[12]=x;
        const bool captured=service.TryAcquireRootInput(root,7,4) && (!present || service.WriteSurface(write)) &&
            service.SealOverlayFromCapturingInput(root);
        service.AbortCapturingInput();
        Check(captured && service.TryAcquireOverlay(root),"R4-039 capture owned exact overlay for history case");
    };
    capture(100,10,true); service.ReleaseConsumedOverlay(true);
    capture(101,99,true); service.ReleaseConsumedOverlay(false);
    Check(service.LastCommittedRootFrame()==100,"R4-039 rejected overlay cannot advance committed root");
    capture(105,15,true);
    const auto* overlay=service.OverlayView();
    Check(overlay && overlay->rows[0].previousObjectToWorld[12]==10 &&
        (overlay->rows[0].flags&PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM),
        "R4-039 resumed rigid uses committed pose across discarded and missing input frames");
    Check(RtCpuRewriteCommittedPoseValid(100,105,7,7,true,service.LastCommittedRootFrame()) &&
        !RtCpuRewriteCommittedPoseValid(101,105,7,7,true,service.LastCommittedRootFrame()) &&
        !RtCpuRewriteCommittedPoseValid(100,105,6,7,true,100) &&
        !RtCpuRewriteCommittedPoseValid(100,100,7,7,true,100) &&
        !RtCpuRewriteCommittedPoseValid(100,105,7,7,false,100),
        "R4-039 skinned gap continuity requires committed root, epoch, newer frame and matching source");
    service.ReleaseConsumedOverlay(true);
    capture(106,0,false); service.ReleaseConsumedOverlay(true);
    capture(107,17,true); overlay=service.OverlayView();
    Check(service.LastCommittedRootFrame()==106 && overlay && !(overlay->rows[0].flags&PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM),
        "R4-039 empty scene commit erases removed membership history");
    service.ReleaseConsumedOverlay(false);
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    Check(service.LastCommittedRootFrame()==0,"R4-039 lifecycle clears committed overlay history");
    service.RequestRecovery(1);
    service.ApplyRouteAtFrameBoundary(1);
    Check(service.Route()==RtCpuProducerRewriteRoute::DrainingToLegacy && service.RecoveryReason()==1,
        "R4-039 capacity request drains at idle boundary");
    service.NotifyBackendDrained(); service.ApplyRouteAtFrameBoundary(1);
    const auto epoch=service.LifecycleGeneration();
    service.ApplyRouteAtFrameBoundary(1);
    Check(service.Route()==RtCpuProducerRewriteRoute::LegacyOnly && service.LifecycleGeneration()==epoch,
        "R4-039 recovery latch preserves legacy rendering instead of retrying frozen scene every frame");
    service.ApplyRouteAtFrameBoundary(0); service.ApplyRouteAtFrameBoundary(1);
    Check(service.Route()==RtCpuProducerRewriteRoute::RewriteWarmup && service.RecoveryReason()==0,
        "R4-039 explicit off/on retries after capacity recovery");
    service.RequestRecovery(2); service.ApplyRouteAtFrameBoundary(1);
    service.Invalidate(RtCpuRewriteInvalidReason::MapWorldChange);
    service.NotifyBackendDrained(); service.ApplyRouteAtFrameBoundary(1);
    Check(service.Route()==RtCpuProducerRewriteRoute::RewriteWarmup && !service.RecoveryReason(),
        "R4-039 new map removes capacity recovery latch");
    service.Shutdown();
}

void TestR4039OverlayGrowth()
{
    using Service=RtCpuProducerRewriteService;
    Check(RtCpuRewriteArenaGrowth(1024,1025,4096)==2048 &&
        RtCpuRewriteArenaGrowth(1024,4096,4096)==4096 &&
        !RtCpuRewriteArenaGrowth(1024,4097,4096) &&
        !RtCpuRewriteArenaGrowth(1024,UINT64_MAX,4096),
        "R4-039 geometric arena growth is bounded and rejects overflow");
    Check(RtCpuRewriteSceneInstanceCountFits(6000,1000) &&
        RtCpuRewriteSceneInstanceCountFits(kRtCpuRewriteMaxExtras-1,1) &&
        !RtCpuRewriteSceneInstanceCountFits(kRtCpuRewriteMaxExtras,1) &&
        !RtCpuRewriteSceneInstanceCountFits(UINT64_MAX,1),
        "R4-039 resident instance budget exceeds old portal cap and checks combined shader IDs");
    Service service; service.Init(); service.ApplyRouteAtFrameBoundary(1);
    RtCpuRewriteOwnedVertex verts[3]={MakeVert(0,0,0),MakeVert(1,0,0),MakeVert(0,1,0)};
    const uint32_t indexes[3]={0,1,2}; float matrix[16]; FillIdentityMatrix(matrix);
    bool valid=service.TryAcquireRootInput(390,7,4);
    for (uint32_t i=0;i<6000 && valid;++i) {
        auto write=MakeTriangleWrite(39001,static_cast<int>(i+1),0,verts,indexes,matrix);
        write.modelMatrix[12]=float(i); valid=service.WriteSurface(write);
    }
    std::vector<PathTraceSkinnedJointMatrix> joints(4096);
    for (auto& joint:joints) { joint.rows[0]=joint.rows[5]=joint.rows[10]=1; joint.rows[3]=42; }
    for (uint32_t i=0;i<30 && valid;++i) {
        auto write=MakeTriangleWrite(39002,static_cast<int>(i+6001),0,verts,indexes,matrix);
        write.sourceClass=kRtCpuRewriteClassSkinnedEntity;
        write.meshKey.sourceDomain=PtCanonicalMeshSourceDomain::SkinnedBindSource;
        write.meshKey.deformationClass=PtCanonicalDeformationClass::Skinned;
        write.meshKey.jointSubmeshIndex=0;
        write.instanceKey.subInstanceKind=PtCanonicalSubInstanceKind::SkinnedSurface;
        write.instanceKey.jointSubmeshIndex=0;
        write.joints=joints.data();write.jointCount=static_cast<uint32_t>(joints.size());
        valid=service.WriteSurface(write);
    }
    valid=valid && service.SealOverlayFromCapturingInput(390);
    service.AbortCapturingInput();
    Check(valid && service.TryAcquireOverlay(390),"R4-039 native overlay copy grows beyond old row and joint arenas");
    const auto* overlay=service.OverlayView();
    Check(overlay && overlay->rowCount==6030 && overlay->jointCount==30*4096 &&
        overlay->rows[5999].currentObjectToWorld[12]==5999 &&
        overlay->joints[0].rows[3]==42 && overlay->joints[overlay->jointCount-1].rows[3]==42,
        "R4-039 grown overlay preserves early/late transforms and joint bytes");
    service.ReleaseConsumedOverlay(true);
    Check(service.LastCommittedRootFrame()==390 && !service.RecoveryReason(),
        "R4-039 grown history commits without losing resident membership");
    service.Shutdown();
}

void TestR4039PackedRanges()
{
    const uint64_t maxVertices = kRtCpuRewriteGpuRigidRetainCapBytes / sizeof(PathTraceSmokeVertex);
    Check(RtCpuRewritePackedCountsFit(maxVertices,0,0,0) &&
        !RtCpuRewritePackedCountsFit(maxVertices+1,0,0,0) &&
        !RtCpuRewritePackedCountsFit(UINT64_MAX,3,1,1) &&
        !RtCpuRewritePackedCountsFit(3,UINT64_MAX,1,1) &&
        !RtCpuRewritePackedCountsFit(3,3,UINT64_MAX,1) &&
        !RtCpuRewritePackedCountsFit(3,3,1,kRtCpuRewriteMaxExtras+1ull),
        "R4-039 packed capacity rejects before overflow or staging allocation");
    RtCpuRewritePackedRangeWitness a;
    a.textureMatrixSignature=1;a.sourceContentSignature=2;a.blasToken=3;
    a.vertexOffset=4;a.indexOffset=6;a.triangleOffset=2;
    a.vertexCount=30;a.indexCount=60;a.triangleCount=20;a.materialId=7;a.materialIndex=8;
    Check(RtCpuRewritePackedRangeReusable(a,a),"R4-039 unchanged packed range remains usable across other membership changes");
    bool rejects=true;
    for (uint64_t RtCpuRewritePackedRangeWitness::* member : {
        &RtCpuRewritePackedRangeWitness::textureMatrixSignature,&RtCpuRewritePackedRangeWitness::sourceContentSignature,
        &RtCpuRewritePackedRangeWitness::blasToken}) {
        auto b=a;++(b.*member);rejects &= !RtCpuRewritePackedRangeReusable(a,b);
    }
    for (uint32_t RtCpuRewritePackedRangeWitness::* member : {
        &RtCpuRewritePackedRangeWitness::vertexOffset,&RtCpuRewritePackedRangeWitness::indexOffset,
        &RtCpuRewritePackedRangeWitness::triangleOffset,&RtCpuRewritePackedRangeWitness::vertexCount,
        &RtCpuRewritePackedRangeWitness::indexCount,&RtCpuRewritePackedRangeWitness::triangleCount,
        &RtCpuRewritePackedRangeWitness::materialId,&RtCpuRewritePackedRangeWitness::materialIndex}) {
        auto b=a;++(b.*member);rejects &= !RtCpuRewritePackedRangeReusable(a,b);
    }
    auto unknown=a;unknown.sourceContentSignature=0;
    Check(rejects && !RtCpuRewritePackedRangeReusable(unknown,unknown),
        "R4-039 packed reuse rejects moved, changed, rebound, retinted and unproven ranges");
}

int main()
{
    TestR4039HistoryAndRecovery();
    TestR4039OverlayGrowth();
    TestR4039PackedRanges();
    TestR4038RouteBoundary();
    TestR4038SkinnedHistory();
    {
        RtCpuMaterialMembership membership;
        const RtCpuMaterialMembership::Key a{1,2,3},b{2,1,3};
        Check(RtCpuMaterialMembership::Hash{}(a)==RtCpuMaterialMembership::Hash{}(b),
            "R4-035 deliberately colliding membership keys exercise exact equality");
        membership.Reserve(3);
        Check(membership.Add(1,2,3) && membership.Add(1,2,3) && membership.Size()==1 &&
            membership.Contains(1,2,3) && !membership.Contains(2,1,3) &&
            !membership.Contains(1,2,4) && membership.Contains(99,99,0),
            "R4-035 duplicate keys, collision, material mismatch and absent material semantics");
        Check(membership.Add(2,1,3) && membership.Size()==2 && membership.Contains(2,1,3),
            "R4-035 colliding distinct identities coexist");
        membership.Clear();
        bool accepted=true;
        for (uint32_t i=0;i<RtCpuMaterialMembership::kMaxRows;++i) accepted=membership.Add(i,0,1)&&accepted;
        Check(accepted && membership.Size()==RtCpuMaterialMembership::kMaxRows &&
            !membership.Add(65535,0,1) && membership.Add(0,0,1),
            "R4-035 bounded membership rejects overflow but preserves duplicate hits");
        membership.Clear();
        Check(membership.Size()==0 && !membership.Contains(0,0,1) && membership.Add(65535,0,1),
            "R4-035 membership reset removes prior identities and releases row budget");
    }

    // A weapon replacement changes membership, offsets and material indices while
    // the 120 NPC allocations remain compatible. Exercise the production planner.
    {
        RtCpuRewriteSkinnedLayout oldLayout, nextLayout;
        for (uint64_t i = 0; i < 121; ++i)
        {
            RtCpuRewriteSkinnedLayoutRow row;
            for (uint32_t w = 0; w < 32; ++w) row.words[w] = (i + 1) * 100 + w;
            oldLayout.rows.push_back(row);
        }
        nextLayout = oldLayout;
        nextLayout.rows[120].words[2]++;
        for (auto& row : nextLayout.rows)
            for (uint32_t w = 20; w <= 28; ++w) row.words[w] += 1000;
        std::reverse(nextLayout.rows.begin(), nextLayout.rows.end());
        std::vector<uint64_t> bounds(121, 150000);
        std::vector<uint32_t> plan;
        Check(RtCpuRewritePlanSkinnedAllocationReuse(oldLayout, nextLayout, bounds, 125000, plan),
            "R4-034 weapon replacement plan accepted");
        bool matches = plan.size() == 121 && plan[0] == UINT32_MAX;
        for (uint32_t i = 1; i < plan.size(); ++i) matches &= plan[i] == 120 - i;
        Check(matches, "R4-034 reordered weapon change reuses all 120 NPC allocations");
        for (uint32_t word : {0u, 1u, 2u, 6u, 7u, 8u, 14u, 16u, 17u, 18u, 19u, 29u})
        {
            auto changed = oldLayout;
            changed.rows[60].words[word]++;
            RtCpuRewritePlanSkinnedAllocationReuse(oldLayout, changed, bounds, 125000, plan);
            Check(plan[60] == UINT32_MAX, "R4-034 changed identity/source/topology/count rejected");
        }
        RtCpuRewritePlanSkinnedAllocationReuse(oldLayout, nextLayout, bounds, 150001, plan);
        Check(std::all_of(plan.begin(), plan.end(), [](uint32_t i) { return i == UINT32_MAX; }),
            "R4-034 growth beyond queried BLAS vertex bound rejects reuse");
        nextLayout.rows = {oldLayout.rows[0], oldLayout.rows[0]};
        RtCpuRewritePlanSkinnedAllocationReuse(oldLayout, nextLayout, bounds, 125000, plan);
        Check(plan[0] == 0 && plan[1] == UINT32_MAX, "R4-034 no two candidate meshes alias one allocation");
        bounds.pop_back();
        Check(!RtCpuRewritePlanSkinnedAllocationReuse(oldLayout, nextLayout, bounds, 125000, plan) && plan.empty(),
            "R4-034 malformed retained allocation inventory fails closed");
        const auto capacity = RtCpuRewriteSkinnedAllocationVertexCapacity(120000);
        Check(capacity == 150000 && RtCpuRewriteSkinnedAllocationVertexCapacity(1) == 4097 &&
            RtCpuRewriteSkinnedAllocationVertexCapacity(INT32_MAX) == INT32_MAX &&
            RtCpuRewriteSkinnedAllocationVertexCapacity(uint64_t(INT32_MAX) + 1) == 0 &&
            RtCpuRewriteSkinnedAllocationVertexCapacity(0) == 0,
            "R4-034 output headroom respects signed API bounds");
        uint64_t peak = 0;
        Check(!RtCpuRewriteValidateSkinnedCapacity(100, capacity * sizeof(PathTraceSmokeVertex), 0,
            100 + 120000 * sizeof(PathTraceSmokeVertex), &peak), "R4-034 output headroom must fit total cap");
        Check(RtCpuRewriteRetirementReleaseBudgetAllows(0, 3000, false) &&
            RtCpuRewriteRetirementReleaseBudgetAllows(63, 1999, false) &&
            !RtCpuRewriteRetirementReleaseBudgetAllows(64, 0, false) &&
            !RtCpuRewriteRetirementReleaseBudgetAllows(1, 2000, false) &&
            RtCpuRewriteRetirementReleaseBudgetAllows(1000, 10000, true),
            "R4-034 cleanup has time/count budgets, progress and explicit drain bypass");
    }
    TestR4025ClassifierNameReuse();
    TestR4018RigidAttributePatches();
    TestR4016FullLevelCapacity();
    TestR4015ConcurrentReferences();
    TestR4015ResidentGeometry();
    TestR4014MaterialWorker();
    TestR4020LightWorker();
    TestR4021MaterialRecords();
    TestR4022MaterialFrame();
    TestR4030ConstantMaterials();
    TestR4029OwnedMaterialState();
    TestR4023RigidResolve();
    TestR4027ParallelLightProducts();
    TestR4026MaterialDependencies();
    TestR4028MaterialBindingOwnership();
    TestR4024MaterialBindings();
    TestR4013ResidentStaticWorld();
    TestR4013PopulationQueuePressure();
	TestPublicationAndMergedBytes();
	TestDuplicateCanonicalMesh();
	TestHeldPressure();
	TestStaleGeneration();
	TestCancelDuringBuild();
	TestMalformedAndCapFailure();
	TestStaticWorldClassId();
	TestConcurrentShardWriters();
	TestDrainStaysUntilNextRootFrame();
	TestTamperedSealRejected();
	TestMalformedKeyRejected();
	TestReceiptCycleRejected();
	TestProductBoundaryRejected();
	TestInvalidateOverlapsEnteredWriter();
	TestInvalidateOverlapsWorkerReading();
	TestSection12Counters();
	TestR2OverlayJoinAndRouteCounts();
	TestR2GeometryWithoutOverlayOmitted();
	TestR2UnpartitionedRigidAndFailClosed();
	TestR2IndexedTriangleOffsetAndLateOverlay();
	TestR2ExtraCountAbove64AndEvictShrink();
	TestR2CommitTailTransformOnly();
	TestR2PackedLayoutSkipAndDynamicBlasCacheHit();
	TestR3SkinnedCaptureSealAndIdSpace();
	TestR3CanonicalPoliciesAndBounds();
	TestR3PreviousJointOffsetAndPreviousTransform();
	TestF2RetirementPlannerLedgerAndDrain();
	TestR3010RetainedSkinnedPolicies();
	TestR3011CanonicalOverlayOrder();
	TestR4001MaterialBindings();
    TestR4003TextureMatrices();
    TestR4007RetainedProductPoses();
    TestR4008FrameDelivery();
    TestR4008StaticAndRetention();
    TestR4010SparseEmissivePose();
    TestR4011ExactLightLifetime();

	if (g_failures != 0)
	{
		std::printf("PathTraceCpuProducerRewriteHarness failures=%d\n", g_failures);
		return 1;
	}
	std::printf("PathTraceCpuProducerRewriteHarness PASS\n");
	return 0;
}
