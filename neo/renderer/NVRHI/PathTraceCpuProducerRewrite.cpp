#if !defined(RT_PT_CPU_PRODUCER_REWRITE_HARNESS)
#include "precompiled.h"
#pragma hdrstop
#endif

#include "PathTraceCpuProducerRewrite.h"
#include "PathTraceMaterialIdKernel.h"

#include <algorithm>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>

#if defined(RT_PT_CPU_PRODUCER_REWRITE_HARNESS)
#define OPTICK_EVENT(...) ((void)0)
#define OPTICK_THREAD(...) ((void)0)
#define OPTICK_TAG(...) ((void)0)
#else
#include "../RenderCommon.h"
#include "../RenderWorld_local.h"
#include "../Model.h"
#include "../Model_local.h"
#include "PathTraceCVars.h"
#include "PathTraceDoomLights.h"
#include "PathTraceDebugModes.h"
#include "PathTraceGeometryLifecycle.h"
#include "PathTraceSkinning.h"
#if defined(USE_OPTICK) && USE_OPTICK
#include <optick.h>
#else
#define OPTICK_EVENT(...) ((void)0)
#define OPTICK_THREAD(...) ((void)0)
#define OPTICK_TAG(...) ((void)0)
#endif
#endif

// Immutable CPU geometry only. Renderer/material pointers and GPU handles never
// enter this package. Source descriptors are separate from the frame's movers.
struct RtCpuRewriteResidentStatic
{
    uint64_t revision = 0, lifecycle = 0, world = 0, map = 0, signature = 0, bytes = 0;
    std::vector<PathTraceSmokeVertex> vertices;
    std::vector<uint32_t> indexes, classes, materials, materialIndexes;
    std::vector<RtCpuRewriteProductSource> sources;

    void Apply(RtCpuRewriteFrozenProductView& view) const
    {
        view.staticRevision = revision;
        view.staticContentSignature = signature;
        view.vertexCount = static_cast<uint32_t>(vertices.size());
        view.indexCount = static_cast<uint32_t>(indexes.size());
        view.triangleCount = static_cast<uint32_t>(classes.size());
        view.vertices = vertices.data(); view.indexes = indexes.data();
        view.triangleClassAndFlags = classes.data();
        view.triangleMaterialIds = materials.data();
        view.triangleMaterialIndexes = materialIndexes.data();
        view.staticSourceCount = static_cast<uint32_t>(sources.size());
        view.staticSources = sources.data();
    }
};

struct RtCpuRewriteResidentDynamic
{
    struct Identity
    {
        PtCanonicalMeshKey mesh;
        PtCanonicalInstanceKey instance;
        uint32_t material = 0, joints = 0, sourceClass = 0;
        std::shared_ptr<const RtCpuRewriteRetainedGeometry> geometry;
    };
    std::vector<Identity> identities;
    std::unique_ptr<uint8_t[]> bytes;
    uint64_t size = 0;
    RtCpuRewriteFrozenProductView view; // residentDynamic stays empty to avoid self ownership

    template<class T> void Rebase(const T*& pointer, const uint8_t* previous)
    {
        const auto address = reinterpret_cast<uintptr_t>(pointer);
        const auto base = reinterpret_cast<uintptr_t>(previous);
        if (pointer && address >= base && address - base <= size)
            pointer = reinterpret_cast<const T*>(bytes.get() + (address - base));
    }
    void CopyView(const RtCpuRewriteFrozenProductView& source, const uint8_t* previous, uint64_t count)
    {
        size = count;
        bytes.reset(new uint8_t[size]);
        std::memcpy(bytes.get(), previous, size);
        view = source; view.residentDynamic.reset();
        Rebase(view.vertices, previous); Rebase(view.indexes, previous);
        Rebase(view.triangleClassAndFlags, previous); Rebase(view.triangleMaterialIds, previous);
        Rebase(view.triangleMaterialIndexes, previous); Rebase(view.sources, previous);
        Rebase(view.rigidMeshes, previous); Rebase(view.skinnedMeshes, previous);
        // Static resident views point outside the copied arena and retain their own owner.
        Rebase(view.staticSources, previous);
        for (uint32_t i = 0; i < view.rigidMeshCount; ++i)
        {
            auto& mesh = const_cast<RtCpuRewriteRigidMeshView*>(view.rigidMeshes)[i];
            Rebase(mesh.vertices, previous); Rebase(mesh.indexes, previous);
        }
        for (uint32_t i = 0; i < view.skinnedMeshCount; ++i)
        {
            auto& mesh = const_cast<RtCpuRewriteSkinnedMeshView*>(view.skinnedMeshes)[i];
            Rebase(mesh.vertices, previous); Rebase(mesh.indexes, previous);
        }
        auto& skin = view.skinnedPrepared;
        Rebase(skin.rows, previous); Rebase(skin.vertices, previous); Rebase(skin.indexes, previous);
        Rebase(skin.blasIndexes, previous); Rebase(skin.dispatches, previous);
        Rebase(skin.records, previous); Rebase(skin.triangles, previous);
    }
};

bool RtCpuRewriteEvaluateEmissiveVertex(const PathTraceSkinnedSourceVertex& source,
    const PathTraceSkinnedJointMatrix* joints, uint32_t jointCount,
    const float objectToWorld[16], const float textureMatrix[6], PathTraceSmokeVertex& out)
{
    if (!joints || !objectToWorld || !textureMatrix) return false;
    float local[3] = {};
    for (uint32_t influence = 0; influence < 4; ++influence)
    {
        if (source.jointIndices[influence] >= jointCount ||
            !std::isfinite(source.jointWeights[influence])) return false;
        const auto& joint = joints[source.jointIndices[influence]];
        for (uint32_t row = 0; row < 3; ++row)
        {
            float value = 0;
            for (uint32_t column = 0; column < 4; ++column)
                value += joint.rows[row * 4 + column] * source.localPosition[column];
            local[row] += value * source.jointWeights[influence];
        }
    }
    PathTraceSmokeVertex candidate = {};
    for (uint32_t row = 0; row < 3; ++row)
    {
        candidate.position[row] = objectToWorld[row] * local[0] +
            objectToWorld[4 + row] * local[1] + objectToWorld[8 + row] * local[2] + objectToWorld[12 + row];
        if (!std::isfinite(candidate.position[row])) return false;
    }
    candidate.position[3] = 1;
    std::copy(source.texCoord, source.texCoord + 4, candidate.texCoord);
    RtCpuRewriteApplyTextureMatrix(candidate.texCoord, textureMatrix);
    if (!std::isfinite(candidate.texCoord[0]) || !std::isfinite(candidate.texCoord[1])) return false;
    out = candidate;
    return true;
}

bool RtCpuRewriteBuildRigidAttributePatch(const RtCpuRewriteRigidMeshView& mesh,
    const PathTraceRigidRouteInstance& route, const RtCpuRewriteTextureMatrices& matrices,
    uint32_t packedVertices, uint32_t packedTriangles, bool uvChanged, bool materialChanged,
    RtCpuRewriteRigidAttributePatch& out)
{
    if (mesh.vertexCount != route.vertexCount || mesh.triangleCount != route.triangleCount ||
        mesh.indexCount != route.indexCount || uint64_t(mesh.triangleCount) * 3 != mesh.indexCount ||
        uint64_t(route.vertexOffset) + mesh.vertexCount > packedVertices ||
        uint64_t(route.triangleOffset) + mesh.triangleCount > packedTriangles ||
        (uvChanged && mesh.vertexCount && !mesh.vertices)) return false;
    RtCpuRewriteRigidAttributePatch candidate;
    candidate.vertexOffset = route.vertexOffset; candidate.triangleOffset = route.triangleOffset;
    try
    {
        if (uvChanged && mesh.vertexCount)
        {
            candidate.vertices.assign(mesh.vertices, mesh.vertices + mesh.vertexCount);
            for (auto& vertex : candidate.vertices)
            {
                RtCpuRewriteApplyTextureMatrix(vertex.texCoord, matrices.primary);
                RtCpuRewriteApplyTextureMatrix(vertex.texCoord + 2, matrices.normal);
                for (float uv : vertex.texCoord) if (!std::isfinite(uv)) return false;
            }
        }
        if (materialChanged)
        {
            candidate.materialIds.assign(mesh.triangleCount, route.materialId);
            candidate.materialIndexes.assign(mesh.triangleCount, route.materialIndex);
        }
    }
    catch (const std::bad_alloc&) { return false; }
    out = std::move(candidate);
    return true;
}

namespace
{

constexpr std::uint64_t kAlign = 16;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::uint32_t kReceiptSentinel = 0;

static RtCpuProducerRewriteService* g_service = nullptr;

bool CheckedAdd(std::uint64_t a, std::uint64_t b, std::uint64_t* out)
{
	if (a > UINT64_MAX - b)
	{
		return false;
	}
	*out = a + b;
	return true;
}

bool CheckedMul(std::uint64_t a, std::uint64_t b, std::uint64_t* out)
{
	if (b != 0 && a > UINT64_MAX / b)
	{
		return false;
	}
	*out = a * b;
	return true;
}

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment)
{
	const std::uint64_t mask = alignment - 1;
	return (value + mask) & ~mask;
}

bool CheckedAlignUp(std::uint64_t value, std::uint64_t alignment, std::uint64_t* out)
{
	if (alignment == 0 || (alignment & (alignment - 1)) != 0)
	{
		return false;
	}
	const std::uint64_t mask = alignment - 1;
	if (value > UINT64_MAX - mask)
	{
		return false;
	}
	*out = (value + mask) & ~mask;
	return true;
}

std::uint64_t HashBytes(std::uint64_t hash, const void* data, std::size_t size)
{
	const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
	for (std::size_t i = 0; i < size; ++i)
	{
		hash ^= bytes[i];
		hash *= kFnvPrime;
	}
	return hash;
}

void TransformPoint(const float m[16], const float in[3], float out[3])
{
	out[0] = in[0] * m[0] + in[1] * m[4] + in[2] * m[8] + m[12];
	out[1] = in[0] * m[1] + in[1] * m[5] + in[2] * m[9] + m[13];
	out[2] = in[0] * m[2] + in[1] * m[6] + in[2] * m[10] + m[14];
}

void TransformVector(const float m[16], const float in[3], float out[3])
{
	out[0] = in[0] * m[0] + in[1] * m[4] + in[2] * m[8];
	out[1] = in[0] * m[1] + in[1] * m[5] + in[2] * m[9];
	out[2] = in[0] * m[2] + in[1] * m[6] + in[2] * m[10];
}

float Normalize3(float v[3])
{
	const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (len > 0.0f)
	{
		const float inv = 1.0f / len;
		v[0] *= inv;
		v[1] *= inv;
		v[2] *= inv;
	}
	return len;
}

int CmpU64(std::uint64_t a, std::uint64_t b)
{
	return (a > b) - (a < b);
}

int CmpU32(std::uint32_t a, std::uint32_t b)
{
	return (a > b) - (a < b);
}

int CmpMeshKey(const PtCanonicalMeshKey& a, const PtCanonicalMeshKey& b)
{
	if (int c = CmpU64(a.sourceAssetId, b.sourceAssetId))
	{
		return c;
	}
	if (int c = CmpU64(a.sourceAssetGeneration, b.sourceAssetGeneration))
	{
		return c;
	}
	if (int c = CmpU64(a.topologySignature, b.topologySignature))
	{
		return c;
	}
	return CmpU32(a.modelSurfaceIndex, b.modelSurfaceIndex);
}

int CmpInstanceKey(const PtCanonicalInstanceKey& a, const PtCanonicalInstanceKey& b)
{
	if (int c = CmpU64(a.worldGeneration, b.worldGeneration))
	{
		return c;
	}
	if (int c = CmpU32(a.renderDefIndex, b.renderDefIndex))
	{
		return c;
	}
	return CmpU32(a.modelSurfaceIndex, b.modelSurfaceIndex);
}

PathTraceSmokeVertex BuildVertex(const RtCpuRewriteOwnedVertex& src, const float modelMatrix[16])
{
	PathTraceSmokeVertex v = {};
	float worldPos[3];
	float worldN[3];
	float worldT[3];
	float worldB[3];
	TransformPoint(modelMatrix, src.xyz, worldPos);
	TransformVector(modelMatrix, src.normal, worldN);
	TransformVector(modelMatrix, src.tangent, worldT);
	TransformVector(modelMatrix, src.bitangent, worldB);
	Normalize3(worldN);
	if (Normalize3(worldT) == 0.0f)
	{
		worldT[0] = 1.0f;
		worldT[1] = 0.0f;
		worldT[2] = 0.0f;
	}
	if (Normalize3(worldB) == 0.0f)
	{
		worldB[0] = worldN[1] * worldT[2] - worldN[2] * worldT[1];
		worldB[1] = worldN[2] * worldT[0] - worldN[0] * worldT[2];
		worldB[2] = worldN[0] * worldT[1] - worldN[1] * worldT[0];
		worldB[0] *= src.bitangentSign;
		worldB[1] *= src.bitangentSign;
		worldB[2] *= src.bitangentSign;
		Normalize3(worldB);
	}
	v.position[0] = worldPos[0];
	v.position[1] = worldPos[1];
	v.position[2] = worldPos[2];
	v.position[3] = 1.0f;
	v.normal[0] = worldN[0];
	v.normal[1] = worldN[1];
	v.normal[2] = worldN[2];
	v.texCoord[0] = src.st[0];
	v.texCoord[1] = src.st[1];
	v.texCoord[2] = src.st[0];
	v.texCoord[3] = src.st[1];
	for (int i = 0; i < 4; ++i)
	{
		v.color[i] = src.color[i];
		v.color2[i] = src.color2[i];
	}
	v.tangent[0] = worldT[0];
	v.tangent[1] = worldT[1];
	v.tangent[2] = worldT[2];
	v.tangent[3] = src.bitangentSign;
	v.bitangent[0] = worldB[0];
	v.bitangent[1] = worldB[1];
	v.bitangent[2] = worldB[2];
	return v;
}


void FillIdentityMatrix16(float matrix[16])
{
	std::memset(matrix, 0, sizeof(float) * 16);
	matrix[0] = 1.0f;
	matrix[5] = 1.0f;
	matrix[10] = 1.0f;
	matrix[15] = 1.0f;
}

} // namespace

bool RtCpuRewritePlanSkinnedSlot(std::uint64_t serial, std::uint64_t slotSerial,
	int currentSlot, int selectedSlot)
{
	return selectedSlot >= 0 && selectedSlot < 3 && selectedSlot != currentSlot &&
		serial != UINT64_MAX && slotSerial <= serial &&
		(slotSerial == 0 || serial + 1 - slotSerial >= 3);
}

std::uint64_t RtCpuRewriteSkinnedAllocationVertexCapacity(std::uint64_t vertexCount)
{
    if (vertexCount == 0 || vertexCount > INT32_MAX) return 0;
    const std::uint64_t headroom = std::max<std::uint64_t>(vertexCount / 4, 4096);
    return vertexCount + std::min<std::uint64_t>(headroom, INT32_MAX - vertexCount);
}

bool RtCpuRewritePlanSkinnedAllocationReuse(const RtCpuRewriteSkinnedLayout& previous,
    const RtCpuRewriteSkinnedLayout& current, const std::vector<std::uint64_t>& vertexBounds,
    std::uint64_t currentVertexCount, std::vector<std::uint32_t>& previousMeshes)
{
    previousMeshes.clear();
    if (previous.rows.size() != vertexBounds.size() || !currentVertexCount ||
        currentVertexCount > INT32_MAX || previous.rows.size() > kRtCpuRewriteTlasMaxInstances ||
        current.rows.size() > kRtCpuRewriteTlasMaxInstances) return false;
    previousMeshes.assign(current.rows.size(), UINT32_MAX);
    std::vector<bool> used(previous.rows.size(), false);
    for (std::size_t i = 0; i < current.rows.size(); ++i)
    {
        const auto& row = current.rows[i];
        for (std::size_t j = 0; j < previous.rows.size(); ++j)
        {
            const auto& old = previous.rows[j];
            if (!used[j] && vertexBounds[j] >= currentVertexCount &&
                std::equal(row.words, row.words + 20, old.words) && row.words[29] == old.words[29])
            {
                previousMeshes[i] = static_cast<std::uint32_t>(j);
                used[j] = true;
                break;
            }
        }
    }
    return true;
}

bool RtCpuRewriteRetirementReleaseBudgetAllows(std::uint64_t released,
    std::uint64_t elapsedUs, bool forceSchedule)
{
    // Always make progress; a single driver destruction may exceed the time budget.
    return forceSchedule || (released < 64 && (released == 0 || elapsedUs < 2000));
}

bool RtCpuRewriteValidateSkinnedCapacity(std::uint64_t resident, std::uint64_t candidate,
	std::uint64_t retired, std::uint64_t limit, std::uint64_t* peak)
{
	std::uint64_t sum = 0;
	if (!CheckedAdd(resident, candidate, &sum) || !CheckedAdd(sum, retired, &sum) || sum > limit)
		return false;
	if (peak) *peak = sum;
	return true;
}

bool RtCpuRewriteSkinnedLayoutEqual(const RtCpuRewriteSkinnedLayout& a,
	const RtCpuRewriteSkinnedLayout& b)
{
	return RtCpuRewriteCompareSkinnedLayout(a, b).reason == RtCpuRewriteSkinnedLayoutMismatch::Equal;
}

bool RtCpuRewriteSkinnedHistoryIdentityEqual(const RtCpuRewriteSkinnedLayoutRow& a,
    const RtCpuRewriteSkinnedLayoutRow& b)
{
    // Source/instance identity and bind geometry govern previous-pose validity.
    // Material IDs and packed offsets describe this frame's allocation instead.
    return a.words[21] != 0 && a.words[29] != 0 &&
        std::equal(a.words, a.words + 20, b.words) &&
        a.words[21] == b.words[21] && a.words[29] == b.words[29];
}

RtCpuRewriteSkinnedLayoutComparison RtCpuRewriteCompareSkinnedLayout(
	const RtCpuRewriteSkinnedLayout& a, const RtCpuRewriteSkinnedLayout& b)
{
	using Reason = RtCpuRewriteSkinnedLayoutMismatch;
	if (a.rows.size() != b.rows.size()) return { Reason::RowCount };
	if (a.capacities != b.capacities) return { Reason::Capacity };
	for (size_t i = 0; i < a.rows.size(); ++i)
		for (size_t w = 0; w < 32; ++w)
			if (a.rows[i].words[w] != b.rows[i].words[w])
				return { Reason::RowField, static_cast<uint32_t>(i), static_cast<uint32_t>(w) };
	// Seven contiguous four-word arrays; neither the native nor harness ABI has padding.
	static_assert(sizeof(PathTraceSkinnedSourceVertex) == 112, "skinned source byte equality ABI");
	if (a.vertices.size() != b.vertices.size()) return { Reason::SourceVertexCount };
	// Keep the stable bulk comparison; scan to identify the element only on a mismatch.
	if (!a.vertices.empty() && std::memcmp(a.vertices.data(), b.vertices.data(),
		a.vertices.size() * sizeof(PathTraceSkinnedSourceVertex)) != 0)
	{
		for (size_t i = 0; i < a.vertices.size(); ++i)
			if (std::memcmp(&a.vertices[i], &b.vertices[i], sizeof(PathTraceSkinnedSourceVertex)) != 0)
				return { Reason::SourceVertex, static_cast<uint32_t>(i) };
	}
	if (a.indexes.size() != b.indexes.size()) return { Reason::SourceIndexCount };
	for (size_t i = 0; i < a.indexes.size(); ++i)
		if (a.indexes[i] != b.indexes[i]) return { Reason::SourceIndex, static_cast<uint32_t>(i) };
	if (a.blasIndexes.size() != b.blasIndexes.size()) return { Reason::BlasIndexCount };
	for (size_t i = 0; i < a.blasIndexes.size(); ++i)
		if (a.blasIndexes[i] != b.blasIndexes[i]) return { Reason::BlasIndex, static_cast<uint32_t>(i) };
	return {};
}

bool RtCpuRewriteSkinnedPreviousPoseValid(std::uint64_t previousFrame, std::uint64_t frame,
	std::uint64_t previousEpoch, std::uint64_t epoch, bool identityMatches)
{
	return identityMatches && epoch != 0 && previousEpoch == epoch &&
		previousFrame != UINT64_MAX && previousFrame + 1 == frame;
}

bool RtCpuRewriteCommittedPoseValid(uint64_t previousFrame, uint64_t frame,
    uint64_t previousEpoch, uint64_t epoch, bool identityMatches, uint64_t committedRoot)
{
    return identityMatches && epoch != 0 && previousEpoch == epoch &&
        previousFrame != 0 && previousFrame == committedRoot && frame > previousFrame;
}

bool RtCpuRewritePackedRangeReusable(const RtCpuRewritePackedRangeWitness& a,
    const RtCpuRewritePackedRangeWitness& b)
{
    return a.sourceContentSignature != 0 && a.blasToken != 0 &&
        a.sourceContentSignature == b.sourceContentSignature && a.blasToken == b.blasToken &&
        a.textureMatrixSignature == b.textureMatrixSignature &&
        a.vertexOffset == b.vertexOffset && a.indexOffset == b.indexOffset && a.triangleOffset == b.triangleOffset &&
        a.vertexCount == b.vertexCount && a.indexCount == b.indexCount && a.triangleCount == b.triangleCount &&
        a.materialId == b.materialId && a.materialIndex == b.materialIndex;
}

bool RtCpuRewriteSceneInstanceCountFits(uint64_t rigid, uint64_t skinned)
{
    return rigid <= kRtCpuRewriteMaxExtras && skinned <= kRtCpuRewriteMaxExtras - rigid;
}

bool RtCpuRewritePackedCountsFit(uint64_t vertices, uint64_t indexes, uint64_t triangles, uint64_t instances)
{
    // Check public count bounds before multiplication. With uint32 counts these
    // byte products and their sum cannot overflow uint64.
    if (vertices > UINT32_MAX || indexes > UINT32_MAX || triangles > UINT32_MAX ||
        instances > kRtCpuRewriteMaxExtras) return false;
    return vertices * sizeof(PathTraceSmokeVertex) + indexes * sizeof(uint32_t) +
        triangles * sizeof(uint32_t) * 2 + instances * sizeof(PathTraceRigidRouteInstance) <=
        kRtCpuRewriteGpuRigidRetainCapBytes;
}

uint64_t RtCpuRewriteArenaGrowth(uint64_t current, uint64_t required, uint64_t limit)
{
    if (required > limit || current > limit) return 0;
    if (required <= current) return current;
    uint64_t capacity = std::max<uint64_t>(current, 1);
    while (capacity < required) capacity = capacity > limit / 2 ? limit : capacity * 2;
    return capacity;
}

void RtCpuRewriteBuildAffineFromObjectToWorld(const float objectToWorld[16], float affine3x4[12])
{
	affine3x4[0] = objectToWorld[0];
	affine3x4[1] = objectToWorld[4];
	affine3x4[2] = objectToWorld[8];
	affine3x4[3] = objectToWorld[12];
	affine3x4[4] = objectToWorld[1];
	affine3x4[5] = objectToWorld[5];
	affine3x4[6] = objectToWorld[9];
	affine3x4[7] = objectToWorld[13];
	affine3x4[8] = objectToWorld[2];
	affine3x4[9] = objectToWorld[6];
	affine3x4[10] = objectToWorld[10];
	affine3x4[11] = objectToWorld[14];
}

bool RtCpuRewriteMatrixIsIdentity(const float matrix[16])
{
	float identity[16];
	FillIdentityMatrix16(identity);
	for (int i = 0; i < 16; ++i)
	{
		if (std::fabs(matrix[i] - identity[i]) > 0.0001f)
		{
			return false;
		}
	}
	return true;
}

RtCpuRewriteCaptureAdmissionReason RtCpuRewritePlanCaptureAdmission(
	const RtCpuRewriteCaptureAdmissionFacts& facts)
{
	const bool rootViewWeaponDepthException =
		facts.weaponDepthHack &&
		facts.sourceDomain == PtCanonicalMeshSourceDomain::SkinnedBindSource &&
		facts.rootView &&
		facts.allowSurfaceInViewId > 0 &&
		facts.allowSurfaceInViewId == facts.activeViewId;
	if (facts.sourceDomain == PtCanonicalMeshSourceDomain::Invalid ||
		facts.sourceDomain == PtCanonicalMeshSourceDomain::UnsupportedTransient)
	{
		return RtCpuRewriteCaptureAdmissionReason::UnsupportedSource;
	}
	if (facts.callback && facts.sourceDomain != PtCanonicalMeshSourceDomain::SkinnedBindSource)
	{
		return RtCpuRewriteCaptureAdmissionReason::Callback;
	}
	if (facts.forceUpdate) return RtCpuRewriteCaptureAdmissionReason::ForceUpdate;
	if (facts.weaponDepthHack && !rootViewWeaponDepthException) return RtCpuRewriteCaptureAdmissionReason::WeaponDepthHack;
	if (facts.modelDepthHack) return RtCpuRewriteCaptureAdmissionReason::ModelDepthHack;
	if (facts.sourceDomain == PtCanonicalMeshSourceDomain::SkinnedBindSource)
	{
		if (!facts.resolvedSurfaceCurrent) return RtCpuRewriteCaptureAdmissionReason::UnresolvedCurrentSurface;
		if (!facts.gpuSkinningEnabled) return RtCpuRewriteCaptureAdmissionReason::GpuSkinningDisabled;
		if (!facts.bindPoseSurface) return RtCpuRewriteCaptureAdmissionReason::CpuPosedSurface;
		if (!facts.jointSnapshotValid) return RtCpuRewriteCaptureAdmissionReason::MissingJointSnapshot;
	}
	return RtCpuRewriteCaptureAdmissionReason::Admitted;
}

bool RtCpuRewriteBuildMaterialBindings(const std::vector<std::uint32_t>& ids,
	std::vector<RtCpuRewriteMaterialBinding>& bindings)
{
	// Build transactionally: a malformed candidate cannot replace the old receipt.
	if (ids.empty() || ids.size() > 65536) return false;
	std::vector<RtCpuRewriteMaterialBinding> candidate;
	candidate.reserve(ids.size());
	for (std::uint32_t i = 0; i < ids.size(); ++i) candidate.push_back({ids[i], i});
	std::sort(candidate.begin(), candidate.end(), [](const auto& a, const auto& b) {
		return a.materialId < b.materialId;
	});
	for (std::size_t i = 1; i < candidate.size(); ++i)
		if (candidate[i-1].materialId == candidate[i].materialId) return false;
	bindings.swap(candidate);
	return true;
}

bool RtCpuRewriteResolveMaterial(const std::vector<RtCpuRewriteMaterialBinding>& bindings,
	std::uint32_t id, std::uint32_t& index)
{
	const auto found = std::lower_bound(bindings.begin(), bindings.end(), id,
		[](const auto& row, std::uint32_t key) { return row.materialId < key; });
	const bool mapped = found != bindings.end() && found->materialId == id;
	index = mapped ? found->materialIndex : 0; // Preserve the old slot-zero fallback.
	return mapped;
}

RtCpuRewriteSkinnedLayoutRow RtCpuRewriteMakeSkinnedLayoutRow(
    const RtCpuRewriteJoinedSkinned& js, const RtCpuRewriteSkinnedMeshView& mesh,
    uint64_t vertexOffset, uint64_t indexOffset, uint64_t jointOffset)
{
    const auto& k = mesh.meshKey;
    const auto& i = js.instanceKey;
    RtCpuRewriteSkinnedLayoutRow row;
    const uint64_t words[32] = {
        i.worldGeneration, i.renderDefIndex, i.renderDefGeneration,
        static_cast<uint64_t>(i.subInstanceKind), i.modelSurfaceIndex,
        static_cast<uint64_t>(i.jointSubmeshIndex),
        k.sourceAssetId, k.sourceAssetGeneration, k.topologySignature,
        static_cast<uint64_t>(k.sourceDomain), k.modelSurfaceIndex, k.vertexFormat,
        static_cast<uint64_t>(k.deformationClass), k.vertexCount, k.indexCount,
        static_cast<uint64_t>(k.jointSubmeshIndex), mesh.signature,
        mesh.vertexCount, mesh.indexCount, mesh.triangleCount, js.materialLogicalId,
        js.jointCount, vertexOffset, indexOffset, jointOffset, indexOffset / 3,
        vertexOffset, vertexOffset, js.materialIndex, mesh.sourceContentSignature, 0, 0
    };
    std::copy(words, words + 32, row.words);
    return row;
}

bool RtCpuRewritePatchSkinnedRouteRecord(PathTraceSkinnedHitRouteGpuRecord& record,
    const PathTraceSkinnedSurfaceDispatchRecord& dispatch, uint64_t sourceGeneration,
    uint64_t outputGeneration, uint32_t instanceId, uint32_t previousPositionCount)
{
    if (!sourceGeneration || !outputGeneration || instanceId > PT_SKINNED_HIT_ROUTE_MAX_SHADER_INSTANCE_ID)
        return false;
    record.shaderInstanceId = instanceId;
    record.sourceGpuIndexGenerationLo = static_cast<uint32_t>(sourceGeneration);
    record.sourceGpuIndexGenerationHi = static_cast<uint32_t>(sourceGeneration >> 32);
    record.outputStorageGenerationLo = static_cast<uint32_t>(outputGeneration);
    record.outputStorageGenerationHi = static_cast<uint32_t>(outputGeneration >> 32);
    record.flags &= ~PT_SKINNED_HIT_ROUTE_HAS_PREVIOUS;
    record.previousPositionOffset = PT_SKINNED_HIT_ROUTE_INVALID_INDEX;
    if (dispatch.flags & PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS)
    {
        if (uint64_t(dispatch.previousPositionOffset) + record.vertexCount > previousPositionCount) return false;
        record.flags |= PT_SKINNED_HIT_ROUTE_HAS_PREVIOUS;
        record.previousPositionOffset = dispatch.previousPositionOffset;
    }
    record.padding0 = previousPositionCount;
    return true;
}

std::uint64_t RtCpuRewriteStaticGeometrySignature(const RtCpuRewriteFrozenProductView& view)
{
    std::uint64_t hash = HashBytes(kFnvOffset, &view.vertexCount, sizeof(view.vertexCount));
    hash = HashBytes(hash, &view.indexCount, sizeof(view.indexCount));
    if (view.vertexCount) hash = HashBytes(hash, view.vertices, sizeof(*view.vertices) * view.vertexCount);
    if (view.indexCount) hash = HashBytes(hash, view.indexes, sizeof(*view.indexes) * view.indexCount);
    if (view.triangleCount) hash = HashBytes(hash, view.triangleClassAndFlags, sizeof(*view.triangleClassAndFlags) * view.triangleCount);
    return hash ? hash : 1;
}

bool RtCpuRewriteFitsResidentRigidBudget(std::uint64_t retainedBytes, std::uint64_t packedBytes)
{
    return RtCpuRewriteFitsGpuRetainCap(retainedBytes, packedBytes, RtCpuProducerRewriteService::kGpuRigidRetainCapBytes);
}

RtCpuRewriteSkinnedRejectReason RtCpuRewriteValidateSkinnedPrepared(
    const RtCpuRewriteFrozenProductView* view, const RtCpuRewriteOverlayView* overlay,
    const RtCpuRewriteJoinResult& join)
{
    using Reason = RtCpuRewriteSkinnedRejectReason;
    uint32_t currentCount = 0;
    if (!overlay || (overlay->rowCount && !overlay->rows)) return Reason::MissingOverlay;
    for (uint32_t i = 0; i < overlay->rowCount; ++i)
        currentCount += overlay->rows[i].sourceClass == kRtCpuRewriteClassSkinnedEntity;
    if (currentCount != join.skinnedCount || join.skinnedCount != join.skinned.size() ||
        (view && view->skinnedPrepared.count != currentCount)) return Reason::WorkerMembershipMismatch;
    if (!currentCount) return Reason::None;
    if (!view) return Reason::MissingProduct;
    if (view->rootFrame > overlay->rootFrame || view->lifecycleGeneration != overlay->lifecycleGeneration)
        return Reason::WorkerFrameMismatch;
    const auto& p = view->skinnedPrepared;
    if (!p.rows || !p.vertices || !p.indexes || !p.blasIndexes || !p.dispatches || !p.records || !p.triangles ||
        !view->skinnedMeshes || !overlay->joints) return Reason::MissingProduct;
    for (uint32_t si = 0; si < currentCount; ++si)
    {
        const auto& js = join.skinned[si];
        const auto& d = p.dispatches[si];
        if (js.meshIndex >= view->skinnedMeshCount || !js.jointCount || js.jointCount > 4096 ||
            js.jointOffset > overlay->jointCount || js.jointCount > overlay->jointCount - js.jointOffset ||
            js.materialLogicalId == UINT32_MAX || js.materialIndex == UINT32_MAX) return Reason::JoinRange;
        auto mesh = view->skinnedMeshes[js.meshIndex];
        if (mesh.meshKey != js.meshKey) return Reason::WorkerMembershipMismatch;
        if (!js.sourceContentSignature || js.sourceContentSignature != mesh.sourceContentSignature)
            return Reason::WorkerSourceMismatch;
        auto witness = RtCpuRewriteMakeSkinnedLayoutRow(js, mesh, d.sourceVertexOffset, d.dynamicIndexOffset, d.currentJointOffset);
        // Material identity/index are exact owner patches; all structural fields must agree.
        witness.words[20] = p.rows[si].words[20]; witness.words[28] = p.rows[si].words[28];
        if (!std::equal(witness.words, witness.words + 32, p.rows[si].words)) return Reason::WorkerMembershipMismatch;
        if (uint64_t(d.sourceVertexOffset) + d.vertexCount > p.vertexCount ||
            uint64_t(d.dynamicIndexOffset) + mesh.indexCount > p.indexCount ||
            uint64_t(d.currentJointOffset) + js.jointCount > p.jointCount)
            return Reason::JoinRange;
    }
    return Reason::None;
}

bool RtCpuRewriteBuildSkinnedRoutePackage(
	const std::vector<RtCpuRewriteSkinnedRouteInput>& inputs,
	std::uint32_t rigidExtraCount,
	RtCpuRewriteSkinnedRoutePackage& out, bool cpuTemplates)
{
	out = RtCpuRewriteSkinnedRoutePackage();
	std::vector<PtSkinnedHitRouteCandidate> candidates;
	candidates.reserve(inputs.size());
	std::uint64_t expectedTriangles = 0;
	for (const RtCpuRewriteSkinnedRouteInput& input : inputs)
	{
		PtSkinnedHitRouteCandidate candidate;
		candidate.instanceKey = input.instanceKey;
		candidate.meshKey = input.meshKey;
		candidate.sourceChecksum = input.meshKey.topologySignature;
		candidate.sourceGpuIndexGeneration = input.sourceGpuIndexGeneration;
		candidate.sourceIndexOffsetBytes = input.sourceIndexOffsetBytes;
		candidate.sourceIndexCapacityBytes = input.sourceIndexCapacityBytes;
		candidate.sourceIndexes = input.sourceIndexes;
		candidate.sourceIndexCount = input.sourceIndexCount;
		candidate.outputStorageGeneration = input.outputStorageGeneration;
		candidate.outputVertexOffsetBytes = input.outputVertexOffsetBytes;
		candidate.outputVertexCount = input.outputVertexCount;
		candidate.outputCapacityBytes = input.outputCapacityBytes;
		candidate.previousPositionOffset = input.previousPositionOffset;
		candidate.previousPositionCount = input.previousPositionCount;
		candidate.previousValid = input.previousValid;
		candidate.fallbackMaterialId = input.materialLogicalId;
		candidate.fallbackMaterialIndex = input.materialIndex;
		candidate.fallbackTriangleClassAndFlags = PT_REWRITE_SKINNED_TRIANGLE_CLASS_AND_FLAGS;
		candidate.legacyCapturePresent = false;
		candidate.dispatchReady = !cpuTemplates;
		candidate.requirePrimitiveIdentity = true;
		candidate.requireEmissiveIdentity = true;
		candidates.push_back(candidate);
		if (!CheckedAdd(expectedTriangles, input.sourceIndexCount / 3u, &expectedTriangles)) return false;
	}
	const PtSkinnedHitRouteLegacyView legacy;
	out.build = cpuTemplates
        ? PtBuildSkinnedHitRouteCpuTemplates(candidates, RtCpuRewriteSkinnedFirstInstanceId(rigidExtraCount))
        : PtBuildSkinnedHitRoutes(candidates, legacy, RtCpuRewriteSkinnedFirstInstanceId(rigidExtraCount));
	if (out.build.records.size() != inputs.size() || out.build.stats.rejected != 0 ||
		out.build.triangles.size() != expectedTriangles)
	{
		return false;
	}
	out.upload = PtBuildSkinnedHitRouteGpuUpload(out.build, RtCpuRewriteSkinnedFirstInstanceId(rigidExtraCount));
	return out.upload.records.size() == inputs.size() &&
		out.upload.triangles.size() == expectedTriangles &&
		(inputs.empty() || out.upload.records[0].shaderInstanceId == RtCpuRewriteSkinnedFirstInstanceId(rigidExtraCount));
}

RtCpuRewriteSkinnedReplacementPlan RtCpuRewritePlanSkinnedReplacement(
	RtCpuRewriteSkinnedReplacementEvent event)
{
	RtCpuRewriteSkinnedReplacementPlan plan;
	switch (event)
	{
	case RtCpuRewriteSkinnedReplacementEvent::LocalFailure:
		plan.keepMembers = true;
		break;
	case RtCpuRewriteSkinnedReplacementEvent::ValidatedReplacement:
		plan.retireMembers = true;
		plan.swapCandidate = true;
		break;
	case RtCpuRewriteSkinnedReplacementEvent::LaterSceneFailure:
		plan.retainRetiredAfterSceneFailure = true;
		break;
	case RtCpuRewriteSkinnedReplacementEvent::Drain:
		plan.retireMembers = true;
		plan.enqueueBeforeForceSchedule = true;
		plan.resetSerialAfterForceSchedule = true;
		break;
	}
	return plan;
}

RtCpuRewriteRetirementDecision RtCpuRewritePlanRetirementAdmission(
	const RtCpuRewriteRetirementAdmissionInput& input)
{
	if (input.proposedCount != 0 &&
		(!input.proposedBytesKnown || input.proposedLogicalBytes == 0))
	{
		return RtCpuRewriteRetirementDecision::UnknownBytes;
	}
	std::uint64_t nextCount = 0;
	std::uint64_t nextBytes = 0;
	if (!CheckedAdd(input.currentLiveCount, input.proposedCount, &nextCount) ||
		!CheckedAdd(input.currentLiveLogicalBytes, input.proposedLogicalBytes, &nextBytes))
	{
		return RtCpuRewriteRetirementDecision::ArithmeticOverflow;
	}
	if (nextCount > input.countBound) return RtCpuRewriteRetirementDecision::CountPressure;
	if (nextBytes > input.logicalByteBound) return RtCpuRewriteRetirementDecision::BytePressure;
	return RtCpuRewriteRetirementDecision::Admit;
}

RtCpuRewriteRetirementMemberApplyPlan RtCpuRewritePlanRetirementMemberApply(
	const RtCpuRewriteRetirementMemberApplyInput& input)
{
	RtCpuRewriteRetirementMemberApplyPlan out;
	out.applyCandidate = input.decision == RtCpuRewriteRetirementDecision::Admit &&
		input.enqueueSucceeded;
	out.preservePrevious = !out.applyCandidate;
	return out;
}

RtCpuRewriteRetirementSite4Plan RtCpuRewritePlanRetirementSite4(
	const RtCpuRewriteRetirementSite4Input& input)
{
	RtCpuRewriteRetirementSite4Plan out;
	if (!input.hasBatch) return out;
	out.enqueueAndCompact = input.localReserveSucceeded && input.preflightSucceeded;
	out.evictionDeferred = !out.enqueueAndCompact;
	return out;
}

void RtCpuRewriteRetirementLedgerEnqueue(RtCpuRewriteRetirementLedger& ledger,
	std::uint64_t count, std::uint64_t logicalBytes,
	std::uint64_t skinnedCount, std::uint64_t skinnedLogicalBytes)
{
	std::uint64_t liveCount = 0, liveBytes = 0, unscheduledCount = 0, unscheduledBytes = 0;
	std::uint64_t enqueued = 0, skinnedLiveCount = 0, skinnedLiveBytes = 0;
	if (!CheckedAdd(ledger.liveCount, count, &liveCount) ||
		!CheckedAdd(ledger.liveLogicalBytes, logicalBytes, &liveBytes) ||
		!CheckedAdd(ledger.unscheduledCount, count, &unscheduledCount) ||
		!CheckedAdd(ledger.unscheduledLogicalBytes, logicalBytes, &unscheduledBytes) ||
		!CheckedAdd(ledger.enqueuedTotal, count, &enqueued) ||
		!CheckedAdd(ledger.skinnedLiveCount, skinnedCount, &skinnedLiveCount) ||
		!CheckedAdd(ledger.skinnedLiveLogicalBytes, skinnedLogicalBytes, &skinnedLiveBytes))
	{
		++ledger.reconciliationAnomalies;
		return;
	}
	ledger.liveCount = liveCount;
	ledger.liveLogicalBytes = liveBytes;
	ledger.unscheduledCount = unscheduledCount;
	ledger.unscheduledLogicalBytes = unscheduledBytes;
	ledger.enqueuedTotal = enqueued;
	ledger.skinnedLiveCount = skinnedLiveCount;
	ledger.skinnedLiveLogicalBytes = skinnedLiveBytes;
	ledger.liveCountHighWater = std::max(ledger.liveCountHighWater, liveCount);
	ledger.liveLogicalBytesHighWater = std::max(ledger.liveLogicalBytesHighWater, liveBytes);
	ledger.unscheduledCountHighWater = std::max(ledger.unscheduledCountHighWater, unscheduledCount);
	ledger.unscheduledLogicalBytesHighWater = std::max(ledger.unscheduledLogicalBytesHighWater, unscheduledBytes);
}

void RtCpuRewriteRetirementLedgerSchedule(RtCpuRewriteRetirementLedger& ledger,
	std::uint64_t count, std::uint64_t logicalBytes)
{
	if (count > ledger.unscheduledCount || logicalBytes > ledger.unscheduledLogicalBytes)
	{
		++ledger.reconciliationAnomalies;
		return;
	}
	ledger.unscheduledCount -= count;
	ledger.unscheduledLogicalBytes -= logicalBytes;
	ledger.scheduledTotal += count;
}

void RtCpuRewriteRetirementLedgerRelease(RtCpuRewriteRetirementLedger& ledger,
	std::uint64_t count, std::uint64_t logicalBytes,
	std::uint64_t skinnedCount, std::uint64_t skinnedLogicalBytes)
{
	if (count > ledger.liveCount || logicalBytes > ledger.liveLogicalBytes ||
		skinnedCount > ledger.skinnedLiveCount || skinnedLogicalBytes > ledger.skinnedLiveLogicalBytes)
	{
		++ledger.reconciliationAnomalies;
		return;
	}
	ledger.liveCount -= count;
	ledger.liveLogicalBytes -= logicalBytes;
	ledger.skinnedLiveCount -= skinnedCount;
	ledger.skinnedLiveLogicalBytes -= skinnedLogicalBytes;
	ledger.releasedTotal += count;
}

void RtCpuRewriteRetirementLedgerReject(RtCpuRewriteRetirementLedger& ledger,
	RtCpuRewriteRetirementDecision decision)
{
	switch (decision)
	{
	case RtCpuRewriteRetirementDecision::CountPressure: ++ledger.countPressureRejects; break;
	case RtCpuRewriteRetirementDecision::BytePressure: ++ledger.bytePressureRejects; break;
	case RtCpuRewriteRetirementDecision::UnknownBytes: ++ledger.unknownByteRejects; break;
	case RtCpuRewriteRetirementDecision::ArithmeticOverflow: ++ledger.arithmeticOverflowRejects; break;
	case RtCpuRewriteRetirementDecision::Admit: break;
	}
}

void RtCpuRewriteRetirementLedgerEvictionDeferred(RtCpuRewriteRetirementLedger& ledger)
{
	++ledger.evictionDeferred;
}

void RtCpuRewriteRetirementLedgerForcedDrain(RtCpuRewriteRetirementLedger& ledger, bool overBound)
{
	if (overBound) ++ledger.forcedDrainOverBound;
}

bool RtCpuRewriteRetirementLedgerReconcile(RtCpuRewriteRetirementLedger& ledger,
	std::uint64_t actualLiveCount, std::uint64_t actualLiveLogicalBytes,
	std::uint64_t actualSkinnedCount, std::uint64_t actualSkinnedLogicalBytes)
{
	const bool ok = ledger.liveCount == actualLiveCount &&
		ledger.liveLogicalBytes == actualLiveLogicalBytes &&
		ledger.skinnedLiveCount == actualSkinnedCount &&
		ledger.skinnedLiveLogicalBytes == actualSkinnedLogicalBytes &&
		ledger.skinnedLiveCount <= ledger.liveCount &&
		ledger.skinnedLiveLogicalBytes <= ledger.liveLogicalBytes;
	if (!ok) ++ledger.reconciliationAnomalies;
	return ok;
}

RtCpuRewriteRetirementDrainPlan RtCpuRewritePlanRetirementDrain(
	const RtCpuRewriteRetirementDrainInventory& inventory)
{
	return { inventory.skinnedCount, inventory.staticCount, inventory.dedicatedCount };
}

RtCpuRewriteSkinnedHandleSelection RtCpuRewriteSelectSkinnedHandles(
	const RtCpuRewriteSkinnedHandleSelectionInput& input)
{
	RtCpuRewriteSkinnedHandleSelection out;
	if (!input.persistentSharedTriangleDispatch || !input.persistentSharedEmissive)
	{
		return out;
	}
	out.triangleDispatch = RtCpuRewriteSkinnedHandleSource::PersistentSharedTriangleDispatch;
	out.emissive = RtCpuRewriteSkinnedHandleSource::PersistentSharedEmissive;
	if (input.haveLiveRoute)
	{
		if (!input.liveRecord || !input.liveTriangle || !input.livePrevious ||
			!input.liveDispatch || !input.liveSourceIndex)
		{
			return RtCpuRewriteSkinnedHandleSelection();
		}
		out.record = RtCpuRewriteSkinnedHandleSource::LiveRewrite;
		out.triangle = RtCpuRewriteSkinnedHandleSource::LiveRewrite;
		out.previous = RtCpuRewriteSkinnedHandleSource::LiveRewrite;
		out.dispatch = RtCpuRewriteSkinnedHandleSource::LiveRewrite;
		out.sourceIndex = RtCpuRewriteSkinnedHandleSource::LiveRewrite;
		out.recordCount = input.recordCount;
		out.triangleCount = input.triangleCount;
		out.sourceIndexCount = input.sourceIndexCount;
		out.previousPositionCount = input.previousPositionCount;
		out.surfaceDispatchCount = input.recordCount;
		out.triangleDispatchIndexCount = input.triangleCount * 3u;
	}
	else
	{
		if (!input.persistentZeroRecord || !input.persistentZeroTriangle)
		{
			return RtCpuRewriteSkinnedHandleSelection();
		}
		out.record = RtCpuRewriteSkinnedHandleSource::PersistentZeroRecord;
		out.triangle = RtCpuRewriteSkinnedHandleSource::PersistentZeroTriangle;
		// The zero-count bindings intentionally reuse layout-compatible sentinels:
		// t20/t21 use the zero t18 record and t28 uses the zero t19 triangle.
		out.previous = RtCpuRewriteSkinnedHandleSource::PersistentZeroRecord;
		out.dispatch = RtCpuRewriteSkinnedHandleSource::PersistentZeroRecord;
		out.sourceIndex = RtCpuRewriteSkinnedHandleSource::PersistentZeroTriangle;
	}
	out.valid = true;
	return out;
}

RtCpuRewriteKeepLastTransition RtCpuRewritePlanKeepLastTransition(
	const RtCpuRewriteKeepLastState& current,
	RtCpuRewriteKeepLastEvent event,
	RtCpuRewriteKeepLastFamily failureFamily)
{
	RtCpuRewriteKeepLastTransition out;
	if (event == RtCpuRewriteKeepLastEvent::SceneCommit || event == RtCpuRewriteKeepLastEvent::Drain)
	{
		return out;
	}
	if (failureFamily == RtCpuRewriteKeepLastFamily::None)
	{
		return out;
	}
	out.next.family = failureFamily;
	if (current.family == failureFamily)
	{
		out.next.consecutive = current.consecutive == UINT32_MAX ? UINT32_MAX : current.consecutive + 1u;
		out.next.warned = current.warned;
	}
	else
	{
		out.next.consecutive = 1;
	}
	if (out.next.consecutive >= 120 && !out.next.warned)
	{
		out.warnNow = true;
		out.next.warned = true;
	}
	out.localSkinnedDegradeEligible =
		out.next.family == RtCpuRewriteKeepLastFamily::LocalSkinned && out.next.consecutive >= 120;
	return out;
}

bool RtCpuRewriteValidatePackedRouteCommit(
	std::uint32_t vertexCount,
	std::uint32_t indexCount,
	std::uint32_t triangleCount,
	std::uint32_t instanceCount,
	std::uint32_t extraCount,
	const std::uint32_t* extraMasks,
	std::uint32_t* failReason,
	std::uint32_t rigidExtraCount)
{
	if (failReason)
	{
		*failReason = kRtCpuRewriteJoinOk;
	}
	if (rigidExtraCount == 0xFFFFFFFFu)
	{
		rigidExtraCount = extraCount;
	}
	if (rigidExtraCount > 0 && instanceCount == 0)
	{
		if (failReason)
		{
			*failReason = kRtCpuRewriteJoinInstanceIdUnresolved;
		}
		return false;
	}
	if (rigidExtraCount > 0 && (vertexCount == 0 || indexCount == 0 || triangleCount == 0 || instanceCount == 0))
	{
		if (failReason)
		{
			*failReason = kRtCpuRewriteJoinRouteCountIncomplete;
		}
		return false;
	}
	if (extraMasks)
	{
		for (std::uint32_t i = 0; i < extraCount; ++i)
		{
			if (extraMasks[i] == 0)
			{
				if (failReason)
				{
					*failReason = kRtCpuRewriteJoinMaskZero;
				}
				return false;
			}
		}
	}
	return true;
}

std::uint32_t RtCpuRewriteSkinnedFirstInstanceId(std::uint32_t rigidExtraCount)
{
	return 2u + rigidExtraCount;
}

bool RtCpuRewriteSkinnedIdsOverlapRigid(std::uint32_t firstSkinned, std::uint32_t skinnedCount, std::uint32_t rigidExtraCount)
{
	const std::uint32_t rigidBegin = 2u;
	const std::uint32_t rigidEnd = 2u + rigidExtraCount;
	for (std::uint32_t i = 0; i < skinnedCount; ++i)
	{
		const std::uint32_t id = firstSkinned + i;
		if (id >= rigidBegin && id < rigidEnd)
		{
			return true;
		}
	}
	return false;
}

std::uint32_t RtCpuRewriteSkinnedCombinedPreviousJointOffset(std::uint32_t currentTotal, std::uint32_t previousLocal)
{
	return currentTotal + previousLocal;
}

std::uint64_t RtCpuRewriteEvictUnreferencedRetain(RtCpuRewriteGpuRetainEntry* entries, std::uint32_t count, std::uint32_t* kept)
{
	std::uint32_t out = 0;
	std::uint64_t bytes = 0;
	for (std::uint32_t i = 0; i < count; ++i)
	{
		if (entries[i].referenced)
		{
			if (out != i)
			{
				entries[out] = entries[i];
			}
			bytes += entries[out].bytes;
			++out;
		}
	}
	if (kept)
	{
		*kept = out;
	}
	return bytes;
}

bool RtCpuRewriteFitsGpuRetainCap(std::uint64_t currentBytes, std::uint64_t addBytes, std::uint64_t cap)
{
	std::uint64_t sum = 0;
	if (!CheckedAdd(currentBytes, addBytes, &sum))
	{
		return false;
	}
	return sum <= cap;
}

bool RtCpuRewriteIsTransformOnlyCommit(const RtCpuRewriteCommitTailPredicate& pred)
{
	return pred.rewriteOnly &&
		pred.allDedicatedHits &&
		pred.dedicatedSetUnchanged &&
		pred.staticSignatureUnchangedOrNoStatic &&
		pred.extraFitsIdleSlot &&
		pred.packedCountsUnchanged;
}

bool RtCpuRewriteShouldSkipPackedRouteGeometryFill(const RtCpuRewritePackedLayoutPredicate& pred)
{
	return pred.allDedicatedHits &&
		pred.dedicatedSetUnchanged &&
		pred.packedCountsUnchanged &&
		pred.packedOffsetIdentity;
}

bool RtCpuRewriteDynamicBlasCacheHit(bool haveStatic, bool signatureMatchesLastCommitted, bool hasDynamicBlas)
{
	return haveStatic && signatureMatchesLastCommitted && hasDynamicBlas;
}

RtCpuProducerRewriteService::RtCpuProducerRewriteService() = default;

RtCpuProducerRewriteService::~RtCpuProducerRewriteService()
{
	Shutdown();
}

void RtCpuProducerRewriteService::Init()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_started)
	{
		return;
	}
	for (std::uint32_t i = 0; i < kInputSlots; ++i)
	{
		m_inputs[i].arena = new (std::nothrow) std::uint8_t[kInputBytes];
		if (!m_inputs[i].arena)
		{
			return;
		}
		std::memset(m_inputs[i].arena, 0, kInputBytes);
	}
	for (std::uint32_t i = 0; i < kProductSlots; ++i)
	{
		m_products[i].arena = new (std::nothrow) std::uint8_t[kProductBytes];
		if (!m_products[i].arena)
		{
			return;
		}
		std::memset(m_products[i].arena, 0, kProductBytes);
	}
	m_scratch = new (std::nothrow) std::uint8_t[kScratchBytes];
	if (!m_scratch)
	{
		return;
	}
	std::memset(m_scratch, 0, kScratchBytes);
	for (std::uint32_t i = 0; i < kOverlaySlots; ++i)
	{
		m_overlays[i].arena = new (std::nothrow) std::uint8_t[kOverlayBytes];
		if (!m_overlays[i].arena)
		{
			return;
		}
		std::memset(m_overlays[i].arena, 0, kOverlayBytes);
    m_overlays[i].rowCapacity = kOverlayBytes;
		m_overlays[i].jointArena = new (std::nothrow) std::uint8_t[kOverlayJointBytes];
		if (!m_overlays[i].jointArena)
		{
			return;
		}
		std::memset(m_overlays[i].jointArena, 0, kOverlayJointBytes);
    m_overlays[i].jointCapacity = kOverlayJointBytes;
	}
	m_lastOverlay.arena = new (std::nothrow) std::uint8_t[kOverlayBytes];
	if (!m_lastOverlay.arena)
	{
		return;
	}
	std::memset(m_lastOverlay.arena, 0, kOverlayBytes);
    m_lastOverlay.rowCapacity = kOverlayBytes;
	m_lastOverlay.jointArena = new (std::nothrow) std::uint8_t[kOverlayJointBytes];
	if (!m_lastOverlay.jointArena)
	{
		return;
	}
	std::memset(m_lastOverlay.jointArena, 0, kOverlayJointBytes);
    m_lastOverlay.jointCapacity = kOverlayJointBytes;
	m_counters.gpuRigidRetainCap = kGpuRigidRetainCapBytes;
	m_stop = false;
	m_cancel = false;
	m_hasWork = false;
	m_acceptance = true;
	m_drainComplete = false;
	m_geometryThread = std::thread(&RtCpuProducerRewriteService::GeometryWorkerMain, this);
	m_shadingThread = std::thread(&RtCpuProducerRewriteService::ShadingWorkerMain, this);
    m_lightManagerThread = std::thread(&RtCpuProducerRewriteService::LightManagerWorkerMain, this);
	m_planningThread = std::thread(&RtCpuProducerRewriteService::MaterialRecordsWorkerMain, this);
	m_started = true;
}

void RtCpuProducerRewriteService::Shutdown()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stop = true;
		m_acceptance = false;
	}
	WakeWorkers();
	if (m_geometryThread.joinable())
	{
		m_geometryThread.join();
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.threadsJoined;
	}
	if (m_shadingThread.joinable())
	{
		m_shadingThread.join();
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.threadsJoined;
	}
	if (m_planningThread.joinable())
	{
		m_planningThread.join();
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.threadsJoined;
	}
    if (m_lightManagerThread.joinable())
    {
        m_lightManagerThread.join();
        std::lock_guard<std::mutex> lock(m_counterMutex);
        ++m_counters.threadsJoined;
    }
	for (std::uint32_t i = 0; i < kInputSlots; ++i)
	{
		delete[] m_inputs[i].arena;
		m_inputs[i].arena = nullptr;
        FreeInputSlot(m_inputs[i]);
	}
	for (std::uint32_t i = 0; i < kOverlaySlots; ++i)
	{
		delete[] m_overlays[i].arena;
		m_overlays[i].arena = nullptr;
		delete[] m_overlays[i].jointArena;
		m_overlays[i].jointArena = nullptr;
		m_overlays[i].jointCount = 0;
		m_overlays[i].state.store(RtCpuProducerRewriteService::OverlaySlotState::Free);
	}
	delete[] m_lastOverlay.arena;
	m_lastOverlay.arena = nullptr;
	delete[] m_lastOverlay.jointArena;
	m_lastOverlay.jointArena = nullptr;
	m_lastOverlay.jointCount = 0;
	for (std::uint32_t i = 0; i < kProductSlots; ++i)
	{
		delete[] m_products[i].arena;
		m_products[i].arena = nullptr;
        FreeProductSlot(m_products[i]);
	}
    m_residentDynamic.reset();
    m_residentStatic.reset();
    m_staticRequestedRevision.store(0);
    m_staticPopulationState.store(0);
	delete[] m_scratch;
	m_scratch = nullptr;
	m_started = false;
	m_currentInput = -1;
	m_currentProduct = -1;
}

void RtCpuProducerRewriteService::WakeWorkers()
{
	m_hasWork = true;
	m_cv.notify_all();
}

void RtCpuProducerRewriteService::NoteSlotBytes()
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	std::uint64_t inputSum = 0;
	std::uint64_t productSum = 0;
	for (std::uint32_t i = 0; i < kInputSlots; ++i)
	{
		const std::uint64_t bytes = m_inputs[i].cursor.load(std::memory_order_acquire);
		m_counters.inputSlotBytes[i] = bytes;
		if (bytes > m_counters.inputSlotHighWater[i])
		{
			m_counters.inputSlotHighWater[i] = bytes;
		}
		inputSum += m_counters.inputSlotHighWater[i];
	}
	for (std::uint32_t i = 0; i < kProductSlots; ++i)
	{
		const std::uint64_t bytes = m_products[i].usedBytes.load(std::memory_order_acquire);
		m_counters.productSlotBytes[i] = bytes;
		if (bytes > m_counters.productSlotHighWater[i])
		{
			m_counters.productSlotHighWater[i] = bytes;
		}
		productSum += m_counters.productSlotHighWater[i];
	}
	m_counters.inputHighWater = inputSum;
	m_counters.productHighWater = productSum;
	if (m_scratchUsed.load() > m_counters.scratchHighWater)
	{
		m_counters.scratchHighWater = m_scratchUsed.load();
	}
	m_counters.serviceHighWater = m_counters.inputHighWater + m_counters.productHighWater + m_counters.scratchHighWater;
	m_counters.lifecycleGeneration = m_lifecycleGeneration.load();
	m_counters.configGeneration = m_configGeneration.load();
	std::uint64_t oldestReady = m_rootFrame.load();
	bool haveReady = false;
	for (std::uint32_t i = 0; i < kProductSlots; ++i)
	{
		const auto st = m_products[i].state.load(std::memory_order_acquire);
		if (st == RtCpuRewriteProductSlotState::Ready ||
			st == RtCpuRewriteProductSlotState::Consuming)
		{
			const std::uint64_t born = m_products[i].rootFrame;
			if (!haveReady || born < oldestReady)
			{
				oldestReady = born;
				haveReady = true;
			}
		}
	}
	const std::uint64_t now = m_rootFrame.load();
	m_counters.ticketAge = haveReady && now >= oldestReady ? now - oldestReady : 0;
}

std::uint8_t* RtCpuProducerRewriteService::ScratchAlloc(std::uint64_t bytes, std::uint64_t alignment, std::uint64_t& used)
{
	const std::uint64_t aligned = AlignUp(used, alignment);
	std::uint64_t end = 0;
	if (!CheckedAdd(aligned, bytes, &end) || end > kScratchBytes || !m_scratch)
	{
		return nullptr;
	}
	used = end;
	std::uint64_t high = m_scratchUsed.load();
	while (end > high && !m_scratchUsed.compare_exchange_weak(high, end))
	{
	}
	return m_scratch + aligned;
}

void RtCpuProducerRewriteService::FreeInputSlot(InputSlot& slot)
{
    slot.geometryOwners.clear();
    slot.initialArena.reset();
    slot.populateStatic = false;
    slot.staticRevision = 0;
	slot.cursor.store(0);
	slot.writerCount.store(0);
	slot.invalid.store(0);
	slot.receipts.store(0);
	slot.receiptHead.store(kReceiptSentinel);
	slot.canonicalSeal = 0;
	slot.ticket = 0;
	slot.state.store(RtCpuRewriteInputSlotState::Free, std::memory_order_release);
}

void RtCpuProducerRewriteService::FreeProductSlot(ProductSlot& slot)
{
	slot.view = {};
    slot.initialArena.reset();
	slot.usedBytes.store(0, std::memory_order_release);
	slot.ticket = 0;
	slot.rootFrame = 0;
	slot.state.store(RtCpuRewriteProductSlotState::Free, std::memory_order_release);
}

bool RtCpuProducerRewriteService::ReserveBytes(InputSlot& slot, std::uint64_t bytes, std::uint64_t alignment, std::uint64_t& outOffset)
{
	std::uint64_t cursor = slot.cursor.load(std::memory_order_relaxed);
	for (;;)
	{
		const std::uint64_t aligned = AlignUp(cursor, alignment);
		std::uint64_t end = 0;
		if (!CheckedAdd(aligned, bytes, &end) || end > slot.Limit())
		{
			return false;
		}
		if (slot.cursor.compare_exchange_weak(cursor, end, std::memory_order_acq_rel, std::memory_order_relaxed))
		{
			outOffset = aligned;
			return true;
		}
	}
}

bool RtCpuProducerRewriteService::PublishReceipt(InputSlot& slot, std::uint32_t receiptOffset)
{
	auto* receipt = reinterpret_cast<ShardReceipt*>(slot.Data() + receiptOffset);
	std::uint32_t head = slot.receiptHead.load(std::memory_order_relaxed);
	for (;;)
	{
		receipt->nextOffset = head;
		if (slot.receiptHead.compare_exchange_weak(head, receiptOffset, std::memory_order_release, std::memory_order_relaxed))
		{
			slot.receipts.fetch_add(1, std::memory_order_release);
			return true;
		}
	}
}

void RtCpuProducerRewriteService::SampleRequestedRoute(int cvarValue)
{
	m_requestedRoute.store(cvarValue != 0 ? 1 : 0);
	const RtCpuProducerRewriteRoute route = m_route.load();
	if (cvarValue == 0)
	{
		if (route == RtCpuProducerRewriteRoute::RewriteOnly ||
            route == RtCpuProducerRewriteRoute::RewriteWarmup)
		{
			EnterDrainingToLegacy();
		}
	}
	else if (route == RtCpuProducerRewriteRoute::LegacyOnly)
	{
		m_route.store(RtCpuProducerRewriteRoute::RewriteWarmup);
	}
}

void RtCpuProducerRewriteService::AdvanceRootFrameForHarness()
{
	m_rootFrame.fetch_add(1, std::memory_order_acq_rel);
	if (m_route.load() == RtCpuProducerRewriteRoute::DrainingToLegacy &&
        m_drainComplete.load() && m_backendDrainComplete.load())
	{
		m_route.store(m_requestedRoute.load() != 0
			? RtCpuProducerRewriteRoute::RewriteWarmup
			: RtCpuProducerRewriteRoute::LegacyOnly);
		m_drainComplete.store(false);
        m_backendDrainComplete.store(false);
		m_acceptance.store(true);
	}
}

void RtCpuProducerRewriteService::ApplyRouteAtFrameBoundary(int cvarValue)
{
    if (!cvarValue) {
        m_pendingRecovery.store(0);
        m_recoveryReason.store(0);
    } else if (const auto reason = m_pendingRecovery.exchange(0)) {
        m_recoveryReason.store(reason);
    }
    const int effective = m_recoveryReason.load() ? 0 : cvarValue;
    // Sample before promotion so rapid off/on changes use the latest request.
    m_requestedRoute.store(effective != 0 ? 1 : 0);
    AdvanceRootFrameForHarness();
    SampleRequestedRoute(effective);
}

void RtCpuProducerRewriteService::RequestRecovery(std::uint32_t reason)
{
    if (!reason) return;
    std::uint32_t empty = 0;
    m_pendingRecovery.compare_exchange_strong(empty, reason);
}

void RtCpuProducerRewriteService::NotifyBackendDrained()
{
    if (m_route.load() == RtCpuProducerRewriteRoute::DrainingToLegacy && m_drainComplete.load())
        m_backendDrainComplete.store(true);
}

void RtCpuProducerRewriteService::EnterDrainingToLegacy()
{
	if (m_route.load() == RtCpuProducerRewriteRoute::DrainingToLegacy)
	{
		return;
	}
    OPTICK_EVENT("PT CPU Route Transition");
	m_route.store(RtCpuProducerRewriteRoute::DrainingToLegacy);
	{
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.drain;
		++m_counters.rollback;
	}
	Invalidate(RtCpuRewriteInvalidReason::LifecycleReset);
}

void RtCpuProducerRewriteService::WaitForInFlightIdle()
{
	for (;;)
	{
		bool busy = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            busy = m_materialBusy || m_lightBusy || m_lightManagerBusy || m_materialRecordsBusy;
        }
		for (std::uint32_t i = 0; i < kInputSlots; ++i)
		{
			if (m_inputs[i].writerCount.load(std::memory_order_acquire) != 0)
			{
				busy = true;
			}
			if (m_inputs[i].state.load(std::memory_order_acquire) == RtCpuRewriteInputSlotState::WorkerReading)
			{
				busy = true;
			}
		}
		for (std::uint32_t i = 0; i < kProductSlots; ++i)
		{
			if (m_products[i].state.load(std::memory_order_acquire) == RtCpuRewriteProductSlotState::Building)
			{
				busy = true;
			}
		}
		if (m_inWorkerReading.load(std::memory_order_acquire))
		{
			busy = true;
		}
		if (!busy)
		{
			return;
		}
		std::unique_lock<std::mutex> lock(m_mutex);
		m_cv.wait_for(lock, std::chrono::milliseconds(1));
	}
}

void RtCpuProducerRewriteService::Invalidate(RtCpuRewriteInvalidReason reason)
{
	m_acceptance.store(false);
    if (reason == RtCpuRewriteInvalidReason::MapWorldChange ||
        reason == RtCpuRewriteInvalidReason::BeginLevelLoad || reason == RtCpuRewriteInvalidReason::VidRestart) {
        m_pendingRecovery.store(0); m_recoveryReason.store(0);
    }
    m_backendDrainComplete.store(false);
	if (m_route.load() == RtCpuProducerRewriteRoute::RewriteOnly)
	{
		m_route.store(RtCpuProducerRewriteRoute::DrainingToLegacy);
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.drain;
	}
	{
        // Pair lifecycle changes with the material dependency CV predicate.
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lifecycleGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_cancel.store(true);
    }
	m_currentInput.store(-1);
	for (std::uint32_t i = 0; i < kInputSlots; ++i)
	{
		if (m_inputs[i].state.load() == RtCpuRewriteInputSlotState::Capturing)
		{
			m_inputs[i].invalid.store(1, std::memory_order_release);
		}
	}
	WakeWorkers();
	for (;;)
	{
		WaitForInFlightIdle();
		bool retry = false;
		for (std::uint32_t i = 0; i < kInputSlots; ++i)
		{
			InputSlot& slot = m_inputs[i];
			RtCpuRewriteInputSlotState state = slot.state.load(std::memory_order_acquire);
			if (state == RtCpuRewriteInputSlotState::WorkerReading)
			{
				retry = true;
				continue;
			}
			if (state == RtCpuRewriteInputSlotState::Capturing ||
				state == RtCpuRewriteInputSlotState::Sealed)
			{
				if (slot.writerCount.load(std::memory_order_acquire) != 0)
				{
					retry = true;
					continue;
				}
				if (!slot.state.compare_exchange_strong(state, RtCpuRewriteInputSlotState::Free,
					std::memory_order_acq_rel, std::memory_order_acquire))
				{
					retry = true;
					continue;
				}
				while (slot.writerCount.load(std::memory_order_acquire) != 0)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				slot.cursor.store(0);
				slot.invalid.store(0);
				slot.receipts.store(0);
				slot.receiptHead.store(kReceiptSentinel);
				slot.canonicalSeal = 0;
				slot.ticket = 0;
                slot.geometryOwners.clear();
                slot.initialArena.reset();
                slot.populateStatic = false;
                slot.staticRevision = 0;
			}
		}
		if (!retry)
		{
			break;
		}
	}
	for (std::uint32_t i = 0; i < kProductSlots; ++i)
	{
		ProductSlot& slot = m_products[i];
		if (slot.state.load() == RtCpuRewriteProductSlotState::Ready)
		{
			FreeProductSlot(slot);
			std::lock_guard<std::mutex> lock(m_counterMutex);
			++m_counters.productRejectStale;
		}
	}
	for (std::uint32_t i = 0; i < kOverlaySlots; ++i)
	{
		FreeOverlaySlot(m_overlays[i]);
	}
	FreeOverlaySlot(m_lastOverlay);
	m_currentOverlay.store(-1);
    m_residentDynamic.reset();
    m_residentStatic.reset(); // worker is idle; product shared owners retain old bytes
    m_staticRequestedRevision.store(0);
    m_staticPopulationState.store(0);
	m_lastJoined.clear();
	m_lastSkinnedJoined.clear();
	m_lastPackedVertexCount = 0;
	m_lastPackedIndexCount = 0;
	m_lastPackedTriangleCount = 0;
	m_cancel.store(false);
	m_drainComplete.store(true);
	if (reason != RtCpuRewriteInvalidReason::BeginLevelLoad &&
		reason != RtCpuRewriteInvalidReason::Shutdown &&
		m_route.load() != RtCpuProducerRewriteRoute::DrainingToLegacy)
	{
		m_acceptance.store(true);
	}
	NoteSlotBytes();
}

void RtCpuProducerRewriteService::BeginLevelLoad()
{
	Invalidate(RtCpuRewriteInvalidReason::BeginLevelLoad);
}

void RtCpuProducerRewriteService::EndLevelLoad()
{
	m_cancel.store(false);
	if (m_route.load() != RtCpuProducerRewriteRoute::DrainingToLegacy)
	{
		m_acceptance.store(true);
	}
}

bool RtCpuProducerRewriteService::PrepareResidentStaticInput(uint64_t revision, bool& populate)
{
    populate = false;
    const int index = m_currentInput.load();
    if (index < 0 || !revision) return false;
    auto& slot = m_inputs[index];
    if (slot.writerCount.load() || slot.state.load() != RtCpuRewriteInputSlotState::Capturing) return false;
    std::lock_guard<std::mutex> queueLock(m_mutex);
    if (m_staticRequestedRevision.exchange(revision) != revision)
        m_staticPopulationState.store(0);
    slot.staticRevision = revision;
    slot.populateStatic = m_staticPopulationState.load(std::memory_order_acquire) == 0;
    if (slot.populateStatic)
    {
        slot.initialArena.reset(new (std::nothrow) uint8_t[128ull * 1024 * 1024]);
        if (!slot.initialArena) { MarkCapturingInvalid(RtCpuRewriteInvalidReason::Capacity); return false; }
        // Dynamic jobs have already joined; migrate their owned bytes only on this
        // cold population frame. The ordinary ring's allocation/cap stays unchanged.
        std::memcpy(slot.Data(), slot.arena, static_cast<size_t>(slot.cursor.load()));
    }
    populate = slot.populateStatic;
    return true;
}

void RtCpuProducerRewriteService::CancelInFlight()
{
	{
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancel.store(true);
        m_lifecycleGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
	{
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.workerCancels;
	}
	WakeWorkers();
}

bool RtCpuProducerRewriteService::WaitForDrainForHarness(std::uint32_t timeoutMs)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (m_drainComplete.load())
		{
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return m_drainComplete.load();
}

bool RtCpuProducerRewriteService::TryAcquireRootInput(std::uint64_t rootFrame, std::uint64_t worldGeneration, std::uint64_t mapGeneration)
{
	if (!m_started || !m_acceptance.load())
	{
		return false;
	}
	if (m_route.load() == RtCpuProducerRewriteRoute::DrainingToLegacy ||
		m_route.load() == RtCpuProducerRewriteRoute::LegacyOnly)
	{
		return false;
	}
	if (m_currentInput.load() >= 0)
	{
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.inputPressureDrops;
		++m_counters.inputDrop;
		return false;
	}
	std::lock_guard<std::mutex> queueLock(m_mutex);
	int freeIndex = -1;
	for (std::uint32_t i = 0; i < kInputSlots; ++i)
	{
		RtCpuRewriteInputSlotState expected = RtCpuRewriteInputSlotState::Free;
		if (m_inputs[i].state.compare_exchange_strong(expected, RtCpuRewriteInputSlotState::Capturing))
		{
			freeIndex = static_cast<int>(i);
			break;
		}
	}
	if (freeIndex < 0)
	{
		// Queued geometry is replaceable; a worker-owned input is never overwritten.
		for (std::uint32_t i = 0; i < kInputSlots; ++i)
		{
			auto expected = RtCpuRewriteInputSlotState::Sealed;
            if (m_inputs[i].state.load(std::memory_order_acquire) != expected) continue;
            if (m_inputs[i].populateStatic) continue; // required cold population is not disposable
			if (m_inputs[i].state.compare_exchange_strong(expected, RtCpuRewriteInputSlotState::Capturing))
			{
				freeIndex = static_cast<int>(i);
				std::lock_guard<std::mutex> counters(m_counterMutex);
				++m_counters.inputReclaimed;
				break;
			}
		}
	}
	if (freeIndex < 0)
	{
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.inputPressureDrops;
		++m_counters.inputDrop;
		return false;
	}
	InputSlot& slot = m_inputs[freeIndex];
    slot.geometryOwners.clear(); // includes reclaimed queued inputs
    slot.staticRevision = 0;
    slot.populateStatic = false;
	slot.ticket = m_nextTicket.fetch_add(1);
	slot.lifecycleGeneration = m_lifecycleGeneration.load();
	slot.worldGeneration = worldGeneration;
	slot.mapGeneration = mapGeneration;
	slot.configGeneration = m_configGeneration.load();
	slot.rootFrame = rootFrame;
	m_rootFrame.store(rootFrame, std::memory_order_release);
	slot.cursor.store(sizeof(std::uint64_t), std::memory_order_relaxed);
	slot.writerCount.store(0);
	slot.invalid.store(0);
	slot.receipts.store(0);
	slot.receiptHead.store(kReceiptSentinel);
	slot.canonicalSeal = 0;
	m_currentInput.store(freeIndex);
	std::lock_guard<std::mutex> lock(m_counterMutex);
	++m_counters.inputAcquire;
	return true;
}

bool RtCpuProducerRewriteService::HasCapturingSlot() const
{
	const int index = m_currentInput.load();
	if (index < 0)
	{
		return false;
	}
	return m_inputs[index].state.load() == RtCpuRewriteInputSlotState::Capturing;
}

void RtCpuProducerRewriteService::MarkCapturingInvalid(RtCpuRewriteInvalidReason reason)
{
	const int index = m_currentInput.load();
	if (index < 0)
	{
		return;
	}
	m_inputs[index].invalid.store(1);
	std::lock_guard<std::mutex> lock(m_counterMutex);
	if (reason == RtCpuRewriteInvalidReason::Capacity)
	{
		++m_counters.invalidCapacity;
	}
	else if (reason == RtCpuRewriteInvalidReason::ParallelAddModelsDisabled)
	{
		++m_counters.parallelAddModelsDisabled;
	}
	else
	{
		++m_counters.invalidMalformed;
	}
}

std::shared_ptr<const RtCpuRewriteRetainedGeometry> RtCpuProducerRewriteService::RetainGeometry(const RtCpuRewriteSurfaceWrite& write)
{
    OPTICK_EVENT("PT CPU Resident Geometry Capture");
    if (!write.vertexCount || write.indexCount < 3 || write.indexCount % 3) return {};
    const bool native = write.nativeDrawVerts && write.nativeIndexes;
    if (native && write.nativeIndexStride && write.nativeIndexStride != 2 && write.nativeIndexStride != 4) return {};
    if (!native && (!write.vertices || !write.indexes)) return {};
    const uint64_t vertBytes = uint64_t(write.vertexCount) * sizeof(RtCpuRewriteOwnedVertex);
    const uint64_t indexBytes = uint64_t(write.indexCount) * sizeof(uint32_t);
    const uint64_t bytes = sizeof(RtCpuRewriteRetainedGeometry) + vertBytes + indexBytes;
    const uint64_t cap = 512ull * 1024 * 1024;
    auto geometry = std::make_shared<RtCpuRewriteRetainedGeometry>();
    uint64_t prior = m_geometryBudget->load();
    do { if (prior > cap || bytes > cap - prior) return {}; }
    while (!m_geometryBudget->compare_exchange_weak(prior, prior + bytes));
    geometry->budget = m_geometryBudget; geometry->charged = bytes;
    geometry->vertices.resize(write.vertexCount); geometry->indexes.resize(write.indexCount);
    auto* dstVerts = geometry->vertices.data(); auto* dstIdx = geometry->indexes.data();
	if (native)
	{
#if !defined(RT_PT_CPU_PRODUCER_REWRITE_HARNESS)
		const idDrawVert* srcVerts = static_cast<const idDrawVert*>(write.nativeDrawVerts);
		const std::uint8_t* srcIdxBytes = static_cast<const std::uint8_t*>(write.nativeIndexes);
		const std::uint32_t stride = write.nativeIndexStride ? write.nativeIndexStride : static_cast<std::uint32_t>(sizeof(triIndex_t));
		for (std::uint32_t i = 0; i < write.vertexCount; ++i)
		{
			const idDrawVert& src = srcVerts[i];
			RtCpuRewriteOwnedVertex& dst = dstVerts[i];
			dst.xyz[0] = src.xyz.x;
			dst.xyz[1] = src.xyz.y;
			dst.xyz[2] = src.xyz.z;
			const idVec3 n = src.GetNormal();
			const idVec3 t = src.GetTangent();
			const idVec3 b = src.GetBiTangent();
			dst.normal[0] = n.x;
			dst.normal[1] = n.y;
			dst.normal[2] = n.z;
			dst.tangent[0] = t.x;
			dst.tangent[1] = t.y;
			dst.tangent[2] = t.z;
			dst.bitangent[0] = b.x;
			dst.bitangent[1] = b.y;
			dst.bitangent[2] = b.z;
			dst.bitangentSign = src.GetBiTangentSign();
			const idVec2 st = src.GetTexCoord();
			dst.st[0] = st.x;
			dst.st[1] = st.y;
			for (int c = 0; c < 4; ++c)
			{
				dst.color[c] = src.color[c] * (1.0f / 255.0f);
				dst.color2[c] = src.color2[c] * (1.0f / 255.0f);
			}
		}
		for (std::uint32_t i = 0; i < write.indexCount; ++i)
		{
			std::uint32_t idx = 0;
			if (stride == 4)
			{
				std::memcpy(&idx, srcIdxBytes + static_cast<std::size_t>(i) * 4, 4);
			}
			else
			{
				std::uint16_t s = 0;
				std::memcpy(&s, srcIdxBytes + static_cast<std::size_t>(i) * stride, sizeof(s));
				idx = s;
			}
			if (idx >= write.vertexCount)
			{
				return {};
			}
			dstIdx[i] = idx;
		}
#else
		return {};
#endif
	}
	else
	{
		std::memcpy(dstVerts, write.vertices, static_cast<std::size_t>(vertBytes));
		std::memcpy(dstIdx, write.indexes, static_cast<std::size_t>(indexBytes));
	}
    uint64_t topo = HashBytes(kFnvOffset, &write.vertexCount, sizeof(write.vertexCount));
    topo = HashBytes(topo, &write.indexCount, sizeof(write.indexCount));
    for (uint32_t index : geometry->indexes)
    {
        if (index >= write.vertexCount) return {};
        topo = HashBytes(topo, &index, sizeof(index));
    }
    if (topo != write.meshKey.topologySignature) return {};
    geometry->topology = topo;
    uint64_t content = HashBytes(kFnvOffset, dstVerts, vertBytes);
    content = HashBytes(content, dstIdx, indexBytes);
    content = HashBytes(content, &write.vertexCount, sizeof(write.vertexCount));
    geometry->content = HashBytes(content, &write.indexCount, sizeof(write.indexCount));
    OPTICK_TAG("residentGeometryCopiedBytes", bytes);
    OPTICK_TAG("residentGeometryRetainedBytes", m_geometryBudget->load());
    return geometry;
}

const RtCpuRewriteOwnedVertex* RtCpuProducerRewriteService::InputVertices(const InputSlot& input, const SurfaceManifest& man) const
{
    return man.geometryRef == UINT32_MAX
        ? reinterpret_cast<const RtCpuRewriteOwnedVertex*>(input.Data() + man.vertexOffset)
        : input.geometryOwners[man.geometryRef]->vertices.data();
}
const uint32_t* RtCpuProducerRewriteService::InputIndexes(const InputSlot& input, const SurfaceManifest& man) const
{
    return man.geometryRef == UINT32_MAX
        ? reinterpret_cast<const uint32_t*>(input.Data() + man.indexOffset)
        : input.geometryOwners[man.geometryRef]->indexes.data();
}

bool RtCpuProducerRewriteService::WriteSurfaceReference(const RtCpuRewriteSurfaceWrite& write)
{
    OPTICK_EVENT("PT CPU Resident Geometry Reference");
    const int index = m_currentInput.load();
    if (index < 0) return false;
    InputSlot& slot = m_inputs[index];
    slot.writerCount.fetch_add(1);
    struct WriterGuard { InputSlot& slot; ~WriterGuard() { slot.writerCount.fetch_sub(1); } } guard{slot};
    if (slot.state.load() != RtCpuRewriteInputSlotState::Capturing || slot.invalid.load() ||
        slot.lifecycleGeneration != m_lifecycleGeneration.load()) return false;
    const auto& geometry = write.geometry;
    if (!geometry || write.vertexCount != geometry->vertices.size() || write.indexCount != geometry->indexes.size() ||
        write.meshKey.topologySignature != geometry->topology || !write.vertexCount ||
        write.indexCount < 3 || write.indexCount % 3 ||
        (write.jointCount && !write.joints) ||
        (write.sourceClass != kRtCpuRewriteClassSkinnedEntity && write.jointCount) ||
        (write.sourceClass == kRtCpuRewriteClassSkinnedEntity && (!write.joints || !write.jointCount || write.jointCount > 4096)))
    { slot.invalid.store(1); return false; }
    uint64_t manOff = 0, recOff = 0, jointOff = 0;
    const uint64_t jointBytes = uint64_t(write.jointCount) * sizeof(PathTraceSkinnedJointMatrix);
    if (!ReserveBytes(slot, sizeof(SurfaceManifest), alignof(SurfaceManifest), manOff) ||
        !ReserveBytes(slot, sizeof(ShardReceipt), alignof(ShardReceipt), recOff) ||
        (jointBytes && !ReserveBytes(slot, jointBytes, alignof(PathTraceSkinnedJointMatrix), jointOff)))
    { slot.invalid.store(1); return false; }
    SurfaceManifest man = {};
    try
    {
        std::lock_guard<std::mutex> lock(slot.geometryMutex);
        if (slot.geometryOwners.size() >= 65535) { slot.invalid.store(1); return false; }
        man.geometryRef = static_cast<uint32_t>(slot.geometryOwners.size());
        slot.geometryOwners.push_back(geometry);
    }
    catch (...) { slot.invalid.store(1); return false; }
    man.meshKey = write.meshKey; man.instanceKey = write.instanceKey;
    man.sourceClass = write.sourceClass; man.materialLogicalId = write.materialLogicalId;
    man.activeEmissiveStage = write.activeEmissiveStage; man.surfaceOrdinal = write.surfaceOrdinal;
    man.vertexCount = write.vertexCount; man.indexCount = write.indexCount;
    man.jointCount = write.jointCount; man.jointOffset = static_cast<uint32_t>(jointOff);
    std::memcpy(man.modelMatrix, write.modelMatrix, sizeof(man.modelMatrix));
    std::memcpy(man.bounds, write.bounds, sizeof(man.bounds));
    if (jointBytes) std::memcpy(slot.Data() + jointOff, write.joints, jointBytes);
    man.sourceContentSignature = HashBytes(geometry->content, &write.jointCount, sizeof(write.jointCount));
    if (!man.sourceContentSignature) man.sourceContentSignature = 1;
    std::memcpy(slot.Data() + manOff, &man, sizeof(man));
    auto* receipt = reinterpret_cast<ShardReceipt*>(slot.Data() + recOff);
    receipt->manifestOffset = static_cast<uint32_t>(manOff);
    PublishReceipt(slot, static_cast<uint32_t>(recOff));
    OPTICK_TAG("residentGeometryReferencedBytes", uint64_t(write.vertexCount) * sizeof(RtCpuRewriteOwnedVertex) + uint64_t(write.indexCount) * sizeof(uint32_t));
    OPTICK_TAG("residentGeometryInputBytes", sizeof(SurfaceManifest) + sizeof(ShardReceipt) + jointBytes);
    return true;
}

bool RtCpuProducerRewriteService::WriteSurface(const RtCpuRewriteSurfaceWrite& write)
{
    if (write.geometry) return WriteSurfaceReference(write);
	OPTICK_EVENT("PT CPU Input Capture Shard");
	const int index = m_currentInput.load();
	if (index < 0)
	{
		return false;
	}
	InputSlot& slot = m_inputs[index];
	slot.writerCount.fetch_add(1, std::memory_order_acq_rel);
	while (m_stallWrite.load(std::memory_order_acquire) && !m_stop.load(std::memory_order_acquire))
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (slot.state.load(std::memory_order_acquire) != RtCpuRewriteInputSlotState::Capturing ||
		slot.invalid.load(std::memory_order_acquire) != 0 ||
		slot.lifecycleGeneration != m_lifecycleGeneration.load(std::memory_order_acquire))
	{
		slot.writerCount.fetch_sub(1, std::memory_order_acq_rel);
		return false;
	}
	std::uint64_t charged = 256;
	std::uint64_t term = 0;
	if (!CheckedMul(32, write.vertexCount, &term) || !CheckedAdd(charged, term, &charged) ||
		!CheckedMul(4, write.indexCount, &term) || !CheckedAdd(charged, term, &charged))
	{
		slot.invalid.store(1);
		slot.writerCount.fetch_sub(1);
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.invalidCapacity;
		return false;
	}
	if (slot.cursor.load() + charged > slot.Limit())
	{
		slot.invalid.store(1);
		slot.writerCount.fetch_sub(1);
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.invalidCapacity;
		return false;
	}
	const bool native = write.nativeDrawVerts != nullptr && write.nativeIndexes != nullptr;
	if (write.vertexCount == 0 || write.indexCount < 3 || (write.indexCount % 3) != 0)
	{
		slot.invalid.store(1);
		slot.writerCount.fetch_sub(1);
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.invalidMalformed;
		return false;
	}
	if (!native && (!write.vertices || !write.indexes))
	{
		slot.invalid.store(1);
		slot.writerCount.fetch_sub(1);
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.invalidMalformed;
		return false;
	}
	if (!native)
	{
		for (std::uint32_t i = 0; i < write.indexCount; ++i)
		{
			if (write.indexes[i] >= write.vertexCount)
			{
				slot.invalid.store(1);
				slot.writerCount.fetch_sub(1);
				std::lock_guard<std::mutex> lock(m_counterMutex);
				++m_counters.invalidMalformed;
				return false;
			}
		}
	}
	std::uint64_t vertBytes = 0;
	std::uint64_t indexBytes = 0;
	if (!CheckedMul(write.vertexCount, sizeof(RtCpuRewriteOwnedVertex), &vertBytes) ||
		!CheckedMul(write.indexCount, sizeof(std::uint32_t), &indexBytes))
	{
		slot.invalid.store(1);
		slot.writerCount.fetch_sub(1);
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.invalidCapacity;
		return false;
	}
	std::uint64_t vertOff = 0;
	std::uint64_t indexOff = 0;
	std::uint64_t manOff = 0;
	std::uint64_t recOff = 0;
	std::uint64_t jointOff = 0;
	std::uint64_t jointBytes = 0;
	if (write.sourceClass == kRtCpuRewriteClassSkinnedEntity)
	{
		if (!write.joints || write.jointCount == 0 || write.jointCount > 4096)
		{
			slot.invalid.store(1);
			slot.writerCount.fetch_sub(1);
			std::lock_guard<std::mutex> lock(m_counterMutex);
			++m_counters.invalidMalformed;
			return false;
		}
		if (!CheckedMul(write.jointCount, sizeof(PathTraceSkinnedJointMatrix), &jointBytes))
		{
			slot.invalid.store(1);
			slot.writerCount.fetch_sub(1);
			std::lock_guard<std::mutex> lock(m_counterMutex);
			++m_counters.invalidCapacity;
			return false;
		}
	}
	if (!ReserveBytes(slot, vertBytes, kAlign, vertOff) ||
		!ReserveBytes(slot, indexBytes, alignof(std::uint32_t), indexOff) ||
		(jointBytes > 0 && !ReserveBytes(slot, jointBytes, alignof(PathTraceSkinnedJointMatrix), jointOff)) ||
		!ReserveBytes(slot, sizeof(SurfaceManifest), alignof(SurfaceManifest), manOff) ||
		!ReserveBytes(slot, sizeof(ShardReceipt), alignof(ShardReceipt), recOff))
	{
		slot.invalid.store(1);
		slot.writerCount.fetch_sub(1);
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.invalidCapacity;
		return false;
	}
	auto* dstVerts = reinterpret_cast<RtCpuRewriteOwnedVertex*>(slot.Data() + vertOff);
	auto* dstIdx = reinterpret_cast<std::uint32_t*>(slot.Data() + indexOff);
	if (native)
	{
#if !defined(RT_PT_CPU_PRODUCER_REWRITE_HARNESS)
		const idDrawVert* srcVerts = static_cast<const idDrawVert*>(write.nativeDrawVerts);
		const std::uint8_t* srcIdxBytes = static_cast<const std::uint8_t*>(write.nativeIndexes);
		const std::uint32_t stride = write.nativeIndexStride ? write.nativeIndexStride : static_cast<std::uint32_t>(sizeof(triIndex_t));
		for (std::uint32_t i = 0; i < write.vertexCount; ++i)
		{
			const idDrawVert& src = srcVerts[i];
			RtCpuRewriteOwnedVertex& dst = dstVerts[i];
			dst.xyz[0] = src.xyz.x;
			dst.xyz[1] = src.xyz.y;
			dst.xyz[2] = src.xyz.z;
			const idVec3 n = src.GetNormal();
			const idVec3 t = src.GetTangent();
			const idVec3 b = src.GetBiTangent();
			dst.normal[0] = n.x;
			dst.normal[1] = n.y;
			dst.normal[2] = n.z;
			dst.tangent[0] = t.x;
			dst.tangent[1] = t.y;
			dst.tangent[2] = t.z;
			dst.bitangent[0] = b.x;
			dst.bitangent[1] = b.y;
			dst.bitangent[2] = b.z;
			dst.bitangentSign = src.GetBiTangentSign();
			const idVec2 st = src.GetTexCoord();
			dst.st[0] = st.x;
			dst.st[1] = st.y;
			for (int c = 0; c < 4; ++c)
			{
				dst.color[c] = src.color[c] * (1.0f / 255.0f);
				dst.color2[c] = src.color2[c] * (1.0f / 255.0f);
			}
		}
		for (std::uint32_t i = 0; i < write.indexCount; ++i)
		{
			std::uint32_t idx = 0;
			if (stride == 4)
			{
				std::memcpy(&idx, srcIdxBytes + static_cast<std::size_t>(i) * 4, 4);
			}
			else
			{
				std::uint16_t s = 0;
				std::memcpy(&s, srcIdxBytes + static_cast<std::size_t>(i) * stride, sizeof(s));
				idx = s;
			}
			if (idx >= write.vertexCount)
			{
				slot.invalid.store(1);
				slot.writerCount.fetch_sub(1);
				std::lock_guard<std::mutex> lock(m_counterMutex);
				++m_counters.invalidMalformed;
				return false;
			}
			dstIdx[i] = idx;
		}
#else
		slot.invalid.store(1);
		slot.writerCount.fetch_sub(1);
		return false;
#endif
	}
	else
	{
		std::memcpy(dstVerts, write.vertices, static_cast<std::size_t>(vertBytes));
		std::memcpy(dstIdx, write.indexes, static_cast<std::size_t>(indexBytes));
	}
	if (jointBytes > 0)
	{
		std::memcpy(slot.Data() + jointOff, write.joints, static_cast<std::size_t>(jointBytes));
	}
	SurfaceManifest manifest = {};
	manifest.meshKey = write.meshKey;
	manifest.instanceKey = write.instanceKey;
	manifest.sourceClass = write.sourceClass;
	std::memcpy(manifest.modelMatrix, write.modelMatrix, sizeof(manifest.modelMatrix));
	std::memcpy(manifest.bounds, write.bounds, sizeof(manifest.bounds));
	manifest.materialLogicalId = write.materialLogicalId;
	manifest.activeEmissiveStage = write.activeEmissiveStage;
	manifest.surfaceOrdinal = write.surfaceOrdinal;
	manifest.vertexOffset = static_cast<std::uint32_t>(vertOff);
	manifest.vertexCount = write.vertexCount;
	manifest.indexOffset = static_cast<std::uint32_t>(indexOff);
	manifest.indexCount = write.indexCount;
	manifest.jointOffset = static_cast<std::uint32_t>(jointOff);
	manifest.jointCount = write.jointCount;
	if (write.sourceClass == kRtCpuRewriteClassSkinnedEntity)
	{
		// Hash owned bind bytes at the capture owner; poses and material state are exact overlays.
		std::uint64_t source = HashBytes(kFnvOffset, dstVerts, static_cast<std::size_t>(vertBytes));
		source = HashBytes(source, dstIdx, static_cast<std::size_t>(indexBytes));
		source = HashBytes(source, &write.vertexCount, sizeof(write.vertexCount));
		source = HashBytes(source, &write.indexCount, sizeof(write.indexCount));
		source = HashBytes(source, &write.jointCount, sizeof(write.jointCount));
		manifest.sourceContentSignature = source ? source : 1;
	}
	std::memcpy(slot.Data() + manOff, &manifest, sizeof(manifest));
	auto* receipt = reinterpret_cast<ShardReceipt*>(slot.Data() + recOff);
	receipt->manifestOffset = static_cast<std::uint32_t>(manOff);
	PublishReceipt(slot, static_cast<std::uint32_t>(recOff));
	slot.writerCount.fetch_sub(1, std::memory_order_acq_rel);
	NoteSlotBytes();
	return true;
}

bool RtCpuProducerRewriteService::ValidateAndSealInput(InputSlot& slot)
{
	OPTICK_EVENT("PT CPU Input Seal");
	std::uint64_t seal = kFnvOffset;
	std::uint32_t count = 0;
	std::uint64_t totalV = 0;
	std::uint64_t totalI = 0;
	std::uint32_t cursor = slot.receiptHead.load(std::memory_order_acquire);
	const std::uint64_t used = slot.cursor.load(std::memory_order_acquire);
	const std::uint32_t expected = slot.receipts.load(std::memory_order_acquire);
	const std::uint32_t maxWalk = expected > 0 ? expected + 1u : 1u;
	std::uint32_t tortoise = cursor;
	bool moveTortoise = false;
	while (cursor != kReceiptSentinel)
	{
		if (count >= maxWalk)
		{
			return false;
		}
		if (cursor < sizeof(std::uint64_t) ||
			static_cast<std::uint64_t>(cursor) + sizeof(ShardReceipt) > used ||
			(cursor % alignof(ShardReceipt)) != 0)
		{
			return false;
		}
		const auto* receipt = reinterpret_cast<const ShardReceipt*>(slot.Data() + cursor);
		const std::uint32_t manOff = receipt->manifestOffset;
		if (manOff < sizeof(std::uint64_t) ||
			static_cast<std::uint64_t>(manOff) + sizeof(SurfaceManifest) > used ||
			(manOff % alignof(SurfaceManifest)) != 0)
		{
			return false;
		}
		const auto* man = reinterpret_cast<const SurfaceManifest*>(slot.Data() + manOff);
		if (man->vertexCount == 0 || man->indexCount < 3 || (man->indexCount % 3) != 0)
		{
			return false;
		}
        const bool referenced = man->geometryRef != UINT32_MAX;
        if (referenced)
        {
            if (man->geometryRef >= slot.geometryOwners.size()) return false;
            const auto& geometry = slot.geometryOwners[man->geometryRef];
            if (!geometry || geometry->vertices.size() != man->vertexCount || geometry->indexes.size() != man->indexCount ||
                geometry->topology != man->meshKey.topologySignature) return false;
        }
        else
        {
		std::uint64_t vEnd = 0;
		std::uint64_t iEnd = 0;
		if (!CheckedMul(man->vertexCount, sizeof(RtCpuRewriteOwnedVertex), &vEnd) ||
			!CheckedAdd(man->vertexOffset, vEnd, &vEnd) || vEnd > used)
		{
			return false;
		}
		if (!CheckedMul(man->indexCount, sizeof(std::uint32_t), &iEnd) ||
			!CheckedAdd(man->indexOffset, iEnd, &iEnd) || iEnd > used)
		{
			return false;
		}
		if ((man->vertexOffset % kAlign) != 0 ||
			(man->indexOffset % alignof(std::uint32_t)) != 0)
		{
			return false;
		}
        }
		std::uint64_t jointEnd = 0;
		if (man->sourceClass == kRtCpuRewriteClassSkinnedEntity)
		{
			if (man->jointCount == 0 || man->jointCount > 4096 ||
				(man->jointOffset % alignof(PathTraceSkinnedJointMatrix)) != 0 ||
				!CheckedMul(man->jointCount, sizeof(PathTraceSkinnedJointMatrix), &jointEnd) ||
				!CheckedAdd(man->jointOffset, jointEnd, &jointEnd) || jointEnd > used)
			{
				return false;
			}
		}
		else if (man->jointCount != 0)
		{
			return false;
		}
		if (man->meshKey.sourceAssetId == 0 ||
			man->meshKey.sourceAssetGeneration == 0 ||
			man->meshKey.vertexFormat != 1 ||
			man->meshKey.vertexCount != man->vertexCount ||
			man->meshKey.indexCount != man->indexCount ||
			(man->sourceClass != kRtCpuRewriteClassSkinnedEntity && man->meshKey.jointSubmeshIndex != -1) ||
			man->meshKey.modelSurfaceIndex != man->instanceKey.modelSurfaceIndex ||
			man->instanceKey.worldGeneration == 0 ||
			man->instanceKey.renderDefGeneration == 0 ||
			man->instanceKey.renderDefIndex == UINT32_MAX ||
			(man->sourceClass != kRtCpuRewriteClassSkinnedEntity && man->instanceKey.jointSubmeshIndex != -1) ||
			man->instanceKey.modelSurfaceIndex != man->surfaceOrdinal ||
			(man->sourceClass != kRtCpuRewriteClassStaticWorld &&
				man->sourceClass != kRtCpuRewriteClassRigidEntity &&
				man->sourceClass != kRtCpuRewriteClassSkinnedEntity))
		{
			return false;
		}
		if (man->meshKey.sourceDomain == PtCanonicalMeshSourceDomain::StaticWorldMap)
		{
			if (man->meshKey.deformationClass != PtCanonicalDeformationClass::Static ||
				man->instanceKey.subInstanceKind != PtCanonicalSubInstanceKind::StaticSurface ||
				man->sourceClass != kRtCpuRewriteClassStaticWorld)
			{
				return false;
			}
		}
		else if (man->meshKey.sourceDomain == PtCanonicalMeshSourceDomain::RegisteredRenderModel)
		{
			if (man->meshKey.deformationClass != PtCanonicalDeformationClass::Rigid ||
				man->instanceKey.subInstanceKind != PtCanonicalSubInstanceKind::RigidSurface ||
				man->sourceClass != kRtCpuRewriteClassRigidEntity)
			{
				return false;
			}
		}
		else if (man->meshKey.sourceDomain == PtCanonicalMeshSourceDomain::SkinnedBindSource)
		{
			if (man->meshKey.deformationClass != PtCanonicalDeformationClass::Skinned ||
				man->instanceKey.subInstanceKind != PtCanonicalSubInstanceKind::SkinnedSurface ||
				man->sourceClass != kRtCpuRewriteClassSkinnedEntity)
			{
				return false;
			}
		}
		else
		{
			return false;
		}
        const auto* indexes = InputIndexes(slot, *man);
        if (!referenced)
        {
		for (std::uint32_t i = 0; i < man->indexCount; ++i)
		{
			if (indexes[i] >= man->vertexCount)
			{
				return false;
			}
		}
        }
		if (!CheckedAdd(totalV, referenced ? 0 : man->vertexCount, &totalV) ||
			!CheckedAdd(totalI, referenced ? 0 : man->indexCount, &totalI))
		{
			return false;
		}
        if (!referenced)
        {
		std::uint64_t topo = kFnvOffset;
		topo = HashBytes(topo, &man->vertexCount, sizeof(man->vertexCount));
		topo = HashBytes(topo, &man->indexCount, sizeof(man->indexCount));
		for (std::uint32_t i = 0; i < man->indexCount; ++i)
		{
			topo = HashBytes(topo, &indexes[i], sizeof(indexes[i]));
		}
		if (topo != man->meshKey.topologySignature)
		{
			return false;
		}
        }
		seal = HashBytes(seal, man, sizeof(*man));
		seal = HashBytes(seal, &cursor, sizeof(cursor));
		++count;
		const std::uint32_t next = receipt->nextOffset;
		if (next == cursor)
		{
			return false;
		}
		cursor = next;
		if (moveTortoise)
		{
			if (tortoise == kReceiptSentinel)
			{
				return false;
			}
			const auto* tRec = reinterpret_cast<const ShardReceipt*>(slot.Data() + tortoise);
			tortoise = tRec->nextOffset;
			if (tortoise == cursor && cursor != kReceiptSentinel)
			{
				return false;
			}
		}
		moveTortoise = !moveTortoise;
	}
	if (count != slot.receipts.load(std::memory_order_acquire))
	{
		return false;
	}
	std::uint64_t cap = 256;
	std::uint64_t term = 0;
	if (!CheckedMul(32, totalV, &term) || !CheckedAdd(cap, term, &cap) ||
		!CheckedMul(4, totalI, &term) || !CheckedAdd(cap, term, &cap) ||
		!CheckedMul(256, count, &term) || !CheckedAdd(cap, term, &cap) ||
		cap > slot.Limit())
	{
		return false;
	}
	slot.canonicalSeal = seal;
	return true;
}

bool RtCpuProducerRewriteService::SealRootInput()
{
	const int index = m_currentInput.exchange(-1);
	if (index < 0)
	{
		return false;
	}
	InputSlot& slot = m_inputs[index];
	if (slot.state.load() != RtCpuRewriteInputSlotState::Capturing)
	{
		return false;
	}
	if (slot.writerCount.load() != 0 || slot.invalid.load() != 0 || !ValidateAndSealInput(slot))
	{
		FreeInputSlot(slot);
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.inputAbort;
		++m_counters.invalidMalformed;
		return false;
	}
	// Pair publication/wake with the worker wait mutex; also keep ticket telemetry owned.
	std::lock_guard<std::mutex> queueLock(m_mutex);
    if (slot.populateStatic) m_staticPopulationState.store(1, std::memory_order_release);
	slot.state.store(RtCpuRewriteInputSlotState::Sealed, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.inputSeal;
		m_counters.lastPublishedTicket = slot.ticket;
	}
	WakeWorkers();
	return true;
}

void RtCpuProducerRewriteService::AbortCapturingInput()
{
	const int index = m_currentInput.exchange(-1);
	if (index < 0)
	{
		return;
	}
	FreeInputSlot(m_inputs[index]);
	std::lock_guard<std::mutex> lock(m_counterMutex);
	++m_counters.inputAbort;
}

void RtCpuProducerRewriteService::AbandonAcquiredInputForHarness()
{
	m_currentInput.store(-1);
}

void RtCpuProducerRewriteService::ReleaseHeldInputsForHarness()
{
	m_currentInput.store(-1);
	for (std::uint32_t i = 0; i < kInputSlots; ++i)
	{
		if (m_inputs[i].state.load() == RtCpuRewriteInputSlotState::Capturing)
		{
			FreeInputSlot(m_inputs[i]);
		}
	}
}

bool RtCpuProducerRewriteService::TamperSealedManifestForHarness()
{
	const int index = m_currentInput.load();
	if (index < 0)
	{
		return false;
	}
	InputSlot& slot = m_inputs[index];
	const std::uint32_t head = slot.receiptHead.load();
	if (head == kReceiptSentinel)
	{
		return false;
	}
	auto* receipt = reinterpret_cast<ShardReceipt*>(slot.Data() + head);
	auto* man = reinterpret_cast<SurfaceManifest*>(slot.Data() + receipt->manifestOffset);
	man->indexCount += 1;
	return true;
}

bool RtCpuProducerRewriteService::TamperManifestKeyForHarness()
{
	const int index = m_currentInput.load();
	if (index < 0)
	{
		return false;
	}
	InputSlot& slot = m_inputs[index];
	const std::uint32_t head = slot.receiptHead.load();
	if (head == kReceiptSentinel)
	{
		return false;
	}
	auto* receipt = reinterpret_cast<ShardReceipt*>(slot.Data() + head);
	auto* man = reinterpret_cast<SurfaceManifest*>(slot.Data() + receipt->manifestOffset);
	man->meshKey.vertexFormat = 0;
	man->meshKey.sourceAssetGeneration = 0;
	return true;
}

bool RtCpuProducerRewriteService::TamperJointSpanForHarness()
{
	const int index = m_currentInput.load();
	if (index < 0) return false;
	InputSlot& slot = m_inputs[index];
	const std::uint32_t head = slot.receiptHead.load();
	if (head == kReceiptSentinel) return false;
	auto* receipt = reinterpret_cast<ShardReceipt*>(slot.Data() + head);
	if (static_cast<std::uint64_t>(receipt->manifestOffset) + sizeof(SurfaceManifest) > slot.cursor.load()) return false;
	auto* man = reinterpret_cast<SurfaceManifest*>(slot.Data() + receipt->manifestOffset);
	man->jointOffset = static_cast<std::uint32_t>(slot.cursor.load() + 1u);
	return true;
}

bool RtCpuProducerRewriteService::TamperManifestOffsetForHarness()
{
	const int index = m_currentInput.load();
	if (index < 0) return false;
	InputSlot& slot = m_inputs[index];
	const std::uint32_t head = slot.receiptHead.load();
	if (head == kReceiptSentinel) return false;
	auto* receipt = reinterpret_cast<ShardReceipt*>(slot.Data() + head);
	receipt->manifestOffset = static_cast<std::uint32_t>(slot.cursor.load() + 1u);
	return true;
}

bool RtCpuProducerRewriteService::TamperReceiptCycleForHarness()
{
	const int index = m_currentInput.load();
	if (index < 0)
	{
		return false;
	}
	InputSlot& slot = m_inputs[index];
	const std::uint32_t head = slot.receiptHead.load();
	if (head == kReceiptSentinel)
	{
		return false;
	}
	auto* receipt = reinterpret_cast<ShardReceipt*>(slot.Data() + head);
	receipt->nextOffset = head;
	return true;
}

bool RtCpuProducerRewriteService::TamperProductBoundaryForHarness()
{
	for (std::uint32_t i = 0; i < kInputSlots; ++i)
	{
		InputSlot& slot = m_inputs[i];
        // The test holds the worker before it reads any manifest bytes. Releasing
        // that hold publishes the tamper, avoiding a race against normal sealing.
		if (slot.state.load(std::memory_order_acquire) != RtCpuRewriteInputSlotState::WorkerReading ||
            !m_stallWorkerReading.load(std::memory_order_acquire) || !m_inWorkerReading.load(std::memory_order_acquire))
		{
			continue;
		}
		const std::uint32_t head = slot.receiptHead.load();
		if (head == kReceiptSentinel)
		{
			return false;
		}
		auto* receipt = reinterpret_cast<ShardReceipt*>(slot.Data() + head);
		auto* man = reinterpret_cast<SurfaceManifest*>(slot.Data() + receipt->manifestOffset);
		man->vertexCount = 8u * 1024u * 1024u;
		man->meshKey.vertexCount = man->vertexCount;
		return true;
	}
	return false;
}

void RtCpuProducerRewriteService::SetWriteStallForHarness(bool stall)
{
	m_stallWrite.store(stall, std::memory_order_release);
}

void RtCpuProducerRewriteService::SetWorkerReadingStallForHarness(bool stall)
{
	m_stallWorkerReading.store(stall, std::memory_order_release);
	WakeWorkers();
}

bool RtCpuProducerRewriteService::WaitUntilWriterEnteredForHarness(std::uint32_t timeoutMs)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < deadline)
	{
		for (std::uint32_t i = 0; i < kInputSlots; ++i)
		{
			if (m_inputs[i].writerCount.load(std::memory_order_acquire) != 0)
			{
				return true;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	for (std::uint32_t i = 0; i < kInputSlots; ++i)
	{
		if (m_inputs[i].writerCount.load(std::memory_order_acquire) != 0)
		{
			return true;
		}
	}
	return false;
}

bool RtCpuProducerRewriteService::WaitUntilWorkerReadingForHarness(std::uint32_t timeoutMs)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (m_inWorkerReading.load(std::memory_order_acquire))
		{
			return true;
		}
		for (std::uint32_t i = 0; i < kInputSlots; ++i)
		{
			if (m_inputs[i].state.load(std::memory_order_acquire) == RtCpuRewriteInputSlotState::WorkerReading)
			{
				return true;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return m_inWorkerReading.load(std::memory_order_acquire);
}

void RtCpuProducerRewriteService::SetGeometryBuildStallForHarness(bool stall)
{
	m_stallBuild.store(stall);
	WakeWorkers();
}

bool RtCpuProducerRewriteService::WaitUntilBuildingForHarness(std::uint32_t timeoutMs)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (m_inBuilding.load())
		{
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return m_inBuilding.load();
}

bool RtCpuProducerRewriteService::BuildFrozenProduct(InputSlot& input, ProductSlot& product)
{
	OPTICK_EVENT("PT CPU Frozen Geometry Build");
    if (input.populateStatic)
    {
        product.initialArena.reset(new (std::nothrow) uint8_t[256ull * 1024 * 1024]);
        if (!product.initialArena) return false;
    }
    const bool residentMatches = m_residentStatic &&
        m_residentStatic->revision == input.staticRevision &&
        m_residentStatic->lifecycle == input.lifecycleGeneration &&
        m_residentStatic->world == input.worldGeneration && m_residentStatic->map == input.mapGeneration;
    if (input.staticRevision && !input.populateStatic && !residentMatches) return false;
	std::uint64_t scratchUsed = 0;
	const std::uint32_t surfaceCount = input.receipts.load(std::memory_order_acquire);
	auto** order = reinterpret_cast<const SurfaceManifest**>(
		ScratchAlloc(static_cast<std::uint64_t>(surfaceCount) * sizeof(const SurfaceManifest*), alignof(void*), scratchUsed));
	if (!order && surfaceCount > 0)
	{
		return false;
	}
	std::uint32_t n = 0;
	std::uint32_t cursor = input.receiptHead.load(std::memory_order_acquire);
	const std::uint64_t used = input.cursor.load(std::memory_order_acquire);
	while (cursor != kReceiptSentinel && n < surfaceCount)
	{
		if (static_cast<std::uint64_t>(cursor) + sizeof(ShardReceipt) > used)
		{
			return false;
		}
		const auto* receipt = reinterpret_cast<const ShardReceipt*>(input.Data() + cursor);
		order[n++] = reinterpret_cast<const SurfaceManifest*>(input.Data() + receipt->manifestOffset);
		cursor = receipt->nextOffset;
	}
	if (n != surfaceCount)
	{
		return false;
	}
	std::sort(order, order + n, [](const SurfaceManifest* a, const SurfaceManifest* b)
	{
		if (int c = CmpInstanceKey(a->instanceKey, b->instanceKey))
		{
			return c < 0;
		}
		if (int c = CmpMeshKey(a->meshKey, b->meshKey))
		{
			return c < 0;
		}
		return a->surfaceOrdinal < b->surfaceOrdinal;
	});

    // The complete geometry package is pose-independent. Exact poses stay in the
    // current overlay; immutable source identity and membership govern this cache.
    bool cacheable = !input.populateStatic;
    std::vector<RtCpuRewriteResidentDynamic::Identity> identities;
    identities.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        const auto& man = *order[i];
        if (man.geometryRef == UINT32_MAX || man.sourceClass == kRtCpuRewriteClassStaticWorld)
        { cacheable = false; break; }
        RtCpuRewriteResidentDynamic::Identity identity;
        identity.mesh = man.meshKey; identity.instance = man.instanceKey;
        identity.material = man.materialLogicalId; identity.joints = man.jointCount;
        identity.sourceClass = man.sourceClass;
        identity.geometry = input.geometryOwners[man.geometryRef];
        identities.push_back(std::move(identity));
    }
    const bool priorCompatible = cacheable && m_residentDynamic &&
        m_residentDynamic->view.lifecycleGeneration == input.lifecycleGeneration &&
        m_residentDynamic->view.worldGeneration == input.worldGeneration &&
        m_residentDynamic->view.mapGeneration == input.mapGeneration &&
        m_residentDynamic->view.configGeneration == input.configGeneration &&
        m_residentDynamic->view.staticRevision == input.staticRevision;
    bool reusable = priorCompatible && m_residentDynamic->identities.size() == identities.size();
    for (size_t i = 0; reusable && i < identities.size(); ++i)
    {
        const auto& a = identities[i]; const auto& b = m_residentDynamic->identities[i];
        reusable = CmpMeshKey(a.mesh, b.mesh) == 0 && CmpInstanceKey(a.instance, b.instance) == 0 &&
            a.material == b.material && a.joints == b.joints && a.sourceClass == b.sourceClass && a.geometry == b.geometry;
    }
    if (reusable)
    {
        OPTICK_EVENT("PT CPU Resident Geometry Product Reuse");
        product.view = m_residentDynamic->view;
        product.view.residentDynamic = m_residentDynamic;
        product.ticket = product.view.ticket = input.ticket;
        product.inputSeal = product.view.inputSeal = input.canonicalSeal;
        product.lifecycleGeneration = input.lifecycleGeneration;
        product.worldGeneration = input.worldGeneration; product.mapGeneration = input.mapGeneration;
        product.configGeneration = input.configGeneration;
        product.contentSignature = product.view.contentSignature;
        product.rootFrame = product.view.rootFrame = input.rootFrame;
        product.usedBytes.store(m_residentDynamic->size);
        OPTICK_TAG("residentGeometryProductReused", 1u);
        OPTICK_TAG("residentGeometryProductBytes", m_residentDynamic->size);
        OPTICK_TAG("residentGeometrySources", n);
        OPTICK_TAG("residentStaticBuilt", 0u);
        OPTICK_TAG("residentStaticReused", input.staticRevision ? 1u : 0u);
        OPTICK_TAG("residentStaticBytes", input.staticRevision ? m_residentStatic->bytes : 0);
        OPTICK_TAG("residentStaticSources", product.view.StaticSourceCount());
        return true;
    }
    OPTICK_TAG("residentGeometryProductReused", 0u);
    // The previous immutable product is the cache; no retained product chain or
    // mutable shared geometry is introduced. Index exact owned source pointers.
    const auto previousDynamic = priorCompatible ? m_residentDynamic : nullptr;
    std::unordered_multimap<const RtCpuRewriteRetainedGeometry*, uint32_t> previousOwners;
    if (previousDynamic && previousDynamic->view.sourceCount == previousDynamic->identities.size()) {
        previousOwners.reserve(previousDynamic->identities.size());
        for (uint32_t i=0;i<previousDynamic->identities.size();++i)
            previousOwners.emplace(previousDynamic->identities[i].geometry.get(), i);
    }
    auto previousSource = [&](const SurfaceManifest& man) -> const RtCpuRewriteProductSource* {
        if (!previousDynamic || man.geometryRef >= input.geometryOwners.size()) return nullptr;
        const auto range = previousOwners.equal_range(input.geometryOwners[man.geometryRef].get());
        for (auto it=range.first;it!=range.second;++it) {
            const auto& identity=previousDynamic->identities[it->second];
            if (CmpMeshKey(identity.mesh,man.meshKey)==0 && identity.joints==man.jointCount &&
                identity.sourceClass==man.sourceClass)
                return &previousDynamic->view.sources[it->second];
        }
        return nullptr;
    };
    uint64_t convertedHits=0, convertedMisses=0, convertedReuseBytes=0;


	std::uint32_t staticVerts = 0;
	std::uint32_t staticIndexes = 0;
	std::uint32_t rigidVerts = 0;
	std::uint32_t rigidIndexes = 0;
	std::uint32_t uniqueRigid = 0;
	std::uint32_t uniqueSkinned = 0;
	auto* uniqueOf = reinterpret_cast<std::uint32_t*>(
		ScratchAlloc(static_cast<std::uint64_t>(n) * sizeof(std::uint32_t), alignof(std::uint32_t), scratchUsed));
	auto* sourceUnique = reinterpret_cast<std::uint32_t*>(
		ScratchAlloc(static_cast<std::uint64_t>(n) * sizeof(std::uint32_t), alignof(std::uint32_t), scratchUsed));
	auto* uniqueOfSkinned = reinterpret_cast<std::uint32_t*>(
		ScratchAlloc(static_cast<std::uint64_t>(n) * sizeof(std::uint32_t), alignof(std::uint32_t), scratchUsed));
	auto* sourceUniqueSkinned = reinterpret_cast<std::uint32_t*>(
		ScratchAlloc(static_cast<std::uint64_t>(n) * sizeof(std::uint32_t), alignof(std::uint32_t), scratchUsed));
	if (n > 0 && (!uniqueOf || !sourceUnique || !uniqueOfSkinned || !sourceUniqueSkinned))
	{
		return false;
	}
	for (std::uint32_t s = 0; s < n; ++s)
	{
		sourceUnique[s] = UINT32_MAX;
		const bool isStatic = order[s]->sourceClass == kRtCpuRewriteClassStaticWorld;
		const bool isRigid = order[s]->sourceClass == kRtCpuRewriteClassRigidEntity;
		const bool isSkinned = order[s]->sourceClass == kRtCpuRewriteClassSkinnedEntity;
		if (isStatic && isRigid)
		{
			return false;
		}
		std::uint64_t nextV = 0;
		std::uint64_t nextI = 0;
		if (isStatic)
		{
			if (!CheckedAdd(staticVerts, order[s]->vertexCount, &nextV) ||
				!CheckedAdd(staticIndexes, order[s]->indexCount, &nextI) ||
				nextV > UINT32_MAX || nextI > UINT32_MAX)
			{
				return false;
			}
			staticVerts = static_cast<std::uint32_t>(nextV);
			staticIndexes = static_cast<std::uint32_t>(nextI);
			continue;
		}
		if (isSkinned)
		{
			sourceUniqueSkinned[s] = UINT32_MAX;
			std::uint32_t foundS = UINT32_MAX;
			for (std::uint32_t u = 0; u < uniqueSkinned; ++u)
			{
				if (CmpMeshKey(order[uniqueOfSkinned[u]]->meshKey, order[s]->meshKey) == 0)
				{
                    const auto* prior = order[uniqueOfSkinned[u]];
                    const auto* priorA = previousSource(*prior);
                    const auto* priorB = previousSource(*order[s]);
                    const bool sameConverted = priorA && priorB && priorA->skinnedMeshIndex != UINT32_MAX &&
                        priorA->skinnedMeshIndex == priorB->skinnedMeshIndex;
                    if (!sameConverted && (prior->jointCount != order[s]->jointCount ||
                        prior->vertexCount != order[s]->vertexCount || prior->indexCount != order[s]->indexCount ||
                        std::memcmp(InputVertices(input, *prior), InputVertices(input, *order[s]),
                            uint64_t(prior->vertexCount) * sizeof(RtCpuRewriteOwnedVertex)) != 0 ||
                        std::memcmp(InputIndexes(input, *prior), InputIndexes(input, *order[s]),
                            uint64_t(prior->indexCount) * sizeof(uint32_t)) != 0)) return false;
                    foundS = u;
                    break;
				}
			}
			if (foundS == UINT32_MAX)
			{
				uniqueOfSkinned[uniqueSkinned] = s;
				foundS = uniqueSkinned;
				++uniqueSkinned;
			}
			sourceUniqueSkinned[s] = foundS;
			continue;
		}
		if (!isRigid)
		{
			return false;
		}
		std::uint32_t found = UINT32_MAX;
		for (std::uint32_t u = 0; u < uniqueRigid; ++u)
		{
			if (CmpMeshKey(order[uniqueOf[u]]->meshKey, order[s]->meshKey) == 0)
			{
				found = u;
				break;
			}
		}
		if (found == UINT32_MAX)
		{
			for (std::uint32_t t = 0; t < n; ++t)
			{
				if (order[t]->sourceClass == kRtCpuRewriteClassStaticWorld &&
					CmpMeshKey(order[t]->meshKey, order[s]->meshKey) == 0)
				{
					return false;
				}
			}
			if (!CheckedAdd(rigidVerts, order[s]->vertexCount, &nextV) ||
				!CheckedAdd(rigidIndexes, order[s]->indexCount, &nextI) ||
				nextV > UINT32_MAX || nextI > UINT32_MAX)
			{
				return false;
			}
			rigidVerts = static_cast<std::uint32_t>(nextV);
			rigidIndexes = static_cast<std::uint32_t>(nextI);
			uniqueOf[uniqueRigid] = s;
			found = uniqueRigid;
			++uniqueRigid;
		}
		sourceUnique[s] = found;
	}
	const std::uint32_t totalVerts = staticVerts;
	const std::uint32_t totalIndexes = staticIndexes;
	const std::uint32_t totalTris = totalIndexes / 3;
	std::uint64_t vBytes = 0;
	std::uint64_t iBytes = 0;
	std::uint64_t srcBytes = 0;
	if (!CheckedMul(totalVerts, sizeof(PathTraceSmokeVertex), &vBytes) ||
		!CheckedMul(totalIndexes, sizeof(std::uint32_t), &iBytes) ||
		!CheckedMul(n, sizeof(RtCpuRewriteProductSource), &srcBytes))
	{
		return false;
	}
	std::uint8_t* dst = product.Data();
	std::uint64_t bumpAt = 0;
	auto bump = [&](std::uint64_t bytes, std::uint64_t align) -> std::uint8_t*
	{
		std::uint64_t aligned = 0;
		std::uint64_t end = 0;
		if (!CheckedAlignUp(bumpAt, align, &aligned) || !CheckedAdd(aligned, bytes, &end) || end > product.Limit())
		{
			return nullptr;
		}
		std::uint8_t* p = dst + aligned;
		bumpAt = end;
		return p;
	};
	std::uint64_t triBytes = 0;
	std::uint64_t threeTri = 0;
	std::uint64_t cap = 0;
	std::uint64_t alignedCap = 0;
	if (!CheckedMul(totalTris, sizeof(std::uint32_t), &triBytes) ||
		!CheckedMul(3, triBytes, &threeTri) ||
		!CheckedAdd(vBytes, iBytes, &cap) ||
		!CheckedAdd(cap, threeTri, &cap) ||
		!CheckedAdd(cap, srcBytes, &cap) ||
		!CheckedAlignUp(cap, kAlign, &alignedCap) ||
		alignedCap > product.Limit())
	{
		return false;
	}
	PathTraceSmokeVertex* verts = totalVerts ? reinterpret_cast<PathTraceSmokeVertex*>(bump(vBytes, alignof(PathTraceSmokeVertex))) : nullptr;
	std::uint32_t* indexes = totalIndexes ? reinterpret_cast<std::uint32_t*>(bump(iBytes, alignof(std::uint32_t))) : nullptr;
	std::uint32_t* classes = totalTris ? reinterpret_cast<std::uint32_t*>(bump(totalTris * sizeof(std::uint32_t), alignof(std::uint32_t))) : nullptr;
	std::uint32_t* matIds = totalTris ? reinterpret_cast<std::uint32_t*>(bump(totalTris * sizeof(std::uint32_t), alignof(std::uint32_t))) : nullptr;
	std::uint32_t* matIdx = totalTris ? reinterpret_cast<std::uint32_t*>(bump(totalTris * sizeof(std::uint32_t), alignof(std::uint32_t))) : nullptr;
	RtCpuRewriteProductSource* sources = reinterpret_cast<RtCpuRewriteProductSource*>(bump(srcBytes, alignof(RtCpuRewriteProductSource)));
	std::uint64_t rigidViewBytes = 0;
	if (!CheckedMul(uniqueRigid, sizeof(RtCpuRewriteRigidMeshView), &rigidViewBytes))
	{
		return false;
	}
	auto* rigidMeshes = uniqueRigid ? reinterpret_cast<RtCpuRewriteRigidMeshView*>(bump(rigidViewBytes, alignof(RtCpuRewriteRigidMeshView))) : nullptr;
	std::uint64_t skinnedViewBytes = 0;
	if (!CheckedMul(uniqueSkinned, sizeof(RtCpuRewriteSkinnedMeshView), &skinnedViewBytes))
	{
		return false;
	}
	auto* skinnedMeshes = uniqueSkinned ? reinterpret_cast<RtCpuRewriteSkinnedMeshView*>(bump(skinnedViewBytes, alignof(RtCpuRewriteSkinnedMeshView))) : nullptr;
	if ((totalVerts && !verts) || (totalIndexes && !indexes) || (totalTris && (!classes || !matIds || !matIdx)) || !sources || (uniqueRigid && !rigidMeshes) || (uniqueSkinned && !skinnedMeshes))
	{
		return false;
	}

	std::uint32_t vCursor = 0;
	std::uint32_t iCursor = 0;
	std::uint32_t tCursor = 0;
	std::uint64_t signature = kFnvOffset;
	float identity[16];
	FillIdentityMatrix16(identity);
	for (std::uint32_t u = 0; u < uniqueRigid; ++u)
	{
		const SurfaceManifest* man = order[uniqueOf[u]];
		std::uint64_t rvb = 0;
		std::uint64_t rib = 0;
		if (!CheckedMul(man->vertexCount, sizeof(PathTraceSmokeVertex), &rvb) ||
			!CheckedMul(man->indexCount, sizeof(std::uint32_t), &rib))
		{
			return false;
		}
		auto* rVerts = reinterpret_cast<PathTraceSmokeVertex*>(bump(rvb, alignof(PathTraceSmokeVertex)));
		auto* rIdx = reinterpret_cast<std::uint32_t*>(bump(rib, alignof(std::uint32_t)));
		if (!rVerts || !rIdx)
		{
			return false;
		}
		const auto* srcVerts = InputVertices(input, *man);
		const auto* srcIdx = InputIndexes(input, *man);
		std::uint64_t meshSig = kFnvOffset;
        const auto* oldSource = previousSource(*man);
        const auto* oldMesh = oldSource && oldSource->rigidMeshIndex < previousDynamic->view.rigidMeshCount
            ? &previousDynamic->view.rigidMeshes[oldSource->rigidMeshIndex] : nullptr;
        if (oldMesh && oldMesh->vertexCount == man->vertexCount && oldMesh->indexCount == man->indexCount) {
            std::memcpy(rVerts,oldMesh->vertices,static_cast<size_t>(rvb));
            std::memcpy(rIdx,oldMesh->indexes,static_cast<size_t>(rib));
            meshSig=oldMesh->signature; ++convertedHits; convertedReuseBytes+=rvb+rib;
        } else {
            ++convertedMisses;
		for (std::uint32_t vi = 0; vi < man->vertexCount; ++vi)
		{
			rVerts[vi] = BuildVertex(srcVerts[vi], identity);
			meshSig = HashBytes(meshSig, &rVerts[vi].position, sizeof(float) * 3);
		}
		for (std::uint32_t ii = 0; ii < man->indexCount; ++ii)
		{
			rIdx[ii] = srcIdx[ii];
			meshSig = HashBytes(meshSig, &rIdx[ii], sizeof(std::uint32_t));
		}
		meshSig = HashBytes(meshSig, &man->sourceClass, sizeof(man->sourceClass));
        }
        signature = HashBytes(signature, &meshSig, sizeof(meshSig));
        rigidMeshes[u].sourceContentSignature = man->sourceContentSignature;
		rigidMeshes[u].meshKey = man->meshKey;
		rigidMeshes[u].signature = meshSig;
		rigidMeshes[u].vertexCount = man->vertexCount;
		rigidMeshes[u].indexCount = man->indexCount;
		rigidMeshes[u].triangleCount = man->indexCount / 3;
		rigidMeshes[u].sourceClass = man->sourceClass;
		rigidMeshes[u].materialLogicalId = man->materialLogicalId;
		rigidMeshes[u].vertices = rVerts;
		rigidMeshes[u].indexes = rIdx;
	}
	for (std::uint32_t u = 0; u < uniqueSkinned; ++u)
	{
		const SurfaceManifest* man = order[uniqueOfSkinned[u]];
		std::uint64_t svb = 0;
		std::uint64_t sib = 0;
		if (!CheckedMul(man->vertexCount, sizeof(PathTraceSkinnedSourceVertex), &svb) ||
			!CheckedMul(man->indexCount, sizeof(std::uint32_t), &sib))
		{
			return false;
		}
		auto* sVerts = reinterpret_cast<PathTraceSkinnedSourceVertex*>(bump(svb, alignof(PathTraceSkinnedSourceVertex)));
		auto* sIdx = reinterpret_cast<std::uint32_t*>(bump(sib, alignof(std::uint32_t)));
		if (!sVerts || !sIdx)
		{
			return false;
		}
		const auto* srcVerts = InputVertices(input, *man);
		const auto* srcIdx = InputIndexes(input, *man);
		std::uint64_t meshSig = kFnvOffset;
        const auto* oldSource = previousSource(*man);
        const auto* oldMesh = oldSource && oldSource->skinnedMeshIndex < previousDynamic->view.skinnedMeshCount
            ? &previousDynamic->view.skinnedMeshes[oldSource->skinnedMeshIndex] : nullptr;
        if (oldMesh && oldMesh->vertexCount == man->vertexCount && oldMesh->indexCount == man->indexCount &&
            oldMesh->sourceContentSignature == man->sourceContentSignature) {
            std::memcpy(sVerts,oldMesh->vertices,static_cast<size_t>(svb));
            std::memcpy(sIdx,oldMesh->indexes,static_cast<size_t>(sib));
            meshSig=oldMesh->signature; ++convertedHits; convertedReuseBytes+=svb+sib;
        } else {
            ++convertedMisses;
		for (std::uint32_t vi = 0; vi < man->vertexCount; ++vi)
		{
			PathTraceSkinnedSourceVertex& dst = sVerts[vi];
			dst = {};
			dst.localPosition[0] = srcVerts[vi].xyz[0];
			dst.localPosition[1] = srcVerts[vi].xyz[1];
			dst.localPosition[2] = srcVerts[vi].xyz[2];
			dst.localPosition[3] = 1.0f;
			dst.localNormal[0] = srcVerts[vi].normal[0];
			dst.localNormal[1] = srcVerts[vi].normal[1];
			dst.localNormal[2] = srcVerts[vi].normal[2];
			dst.localTangent[0] = srcVerts[vi].tangent[0];
			dst.localTangent[1] = srcVerts[vi].tangent[1];
			dst.localTangent[2] = srcVerts[vi].tangent[2];
			dst.localTangent[3] = srcVerts[vi].bitangentSign;
			dst.texCoord[0] = srcVerts[vi].st[0];
			dst.texCoord[1] = srcVerts[vi].st[1];
			dst.texCoord[2] = srcVerts[vi].st[0];
			dst.texCoord[3] = srcVerts[vi].st[1];
			for (int k = 0; k < 4; ++k)
			{
				dst.color[k] = srcVerts[vi].color[k];
				dst.jointIndices[k] = static_cast<std::uint32_t>(srcVerts[vi].color[k] * 255.0f + 0.5f);
				dst.jointWeights[k] = srcVerts[vi].color2[k];
				if (man->jointCount == 0 || dst.jointIndices[k] >= man->jointCount)
				{
					dst.jointIndices[k] = 0;
					dst.jointWeights[k] = 0.0f;
				}
			}
			meshSig = HashBytes(meshSig, &dst, sizeof(dst));
		}
		for (std::uint32_t ii = 0; ii < man->indexCount; ++ii)
		{
			sIdx[ii] = srcIdx[ii];
			meshSig = HashBytes(meshSig, &sIdx[ii], sizeof(std::uint32_t));
		}
        }
        signature = HashBytes(signature, &meshSig, sizeof(meshSig));
		skinnedMeshes[u].meshKey = man->meshKey;
		skinnedMeshes[u].signature = meshSig;
		skinnedMeshes[u].sourceContentSignature = man->sourceContentSignature;
		skinnedMeshes[u].vertexCount = man->vertexCount;
		skinnedMeshes[u].indexCount = man->indexCount;
		skinnedMeshes[u].triangleCount = man->indexCount / 3;
		skinnedMeshes[u].sourceClass = man->sourceClass;
		skinnedMeshes[u].materialLogicalId = man->materialLogicalId;
		skinnedMeshes[u].vertices = sVerts;
		skinnedMeshes[u].indexes = sIdx;
	}
	for (std::uint32_t s = 0; s < n; ++s)
	{
		const SurfaceManifest* man = order[s];
		const auto* srcVerts = InputVertices(input, *man);
		const auto* srcIdx = InputIndexes(input, *man);
		sources[s].meshKey = man->meshKey;
		sources[s].instanceKey = man->instanceKey;
		sources[s].surfaceOrdinal = man->surfaceOrdinal;
		sources[s].sourceClass = man->sourceClass;
		sources[s].vertexCount = man->vertexCount;
		sources[s].indexCount = man->indexCount;
		sources[s].triangleCount = man->indexCount / 3;
		if (man->sourceClass == kRtCpuRewriteClassRigidEntity)
		{
			sources[s].rigidMeshIndex = sourceUnique[s];
			sources[s].skinnedMeshIndex = UINT32_MAX;
			sources[s].vertexBegin = 0;
			sources[s].indexBegin = 0;
			sources[s].triangleBegin = 0;
			continue;
		}
		if (man->sourceClass == kRtCpuRewriteClassSkinnedEntity)
		{
			sources[s].rigidMeshIndex = UINT32_MAX;
			sources[s].skinnedMeshIndex = sourceUniqueSkinned[s];
			sources[s].vertexBegin = 0;
			sources[s].indexBegin = 0;
			sources[s].triangleBegin = 0;
			continue;
		}
		sources[s].rigidMeshIndex = UINT32_MAX;
		sources[s].skinnedMeshIndex = UINT32_MAX;
		sources[s].vertexBegin = vCursor;
		sources[s].indexBegin = iCursor;
		sources[s].triangleBegin = tCursor;
		for (std::uint32_t vi = 0; vi < man->vertexCount; ++vi)
		{
			verts[vCursor + vi] = BuildVertex(srcVerts[vi], man->modelMatrix);
			signature = HashBytes(signature, &verts[vCursor + vi].position, sizeof(float) * 3);
		}
		for (std::uint32_t ii = 0; ii < man->indexCount; ++ii)
		{
			indexes[iCursor + ii] = srcIdx[ii] + vCursor;
			signature = HashBytes(signature, &indexes[iCursor + ii], sizeof(std::uint32_t));
		}
		signature = HashBytes(signature, &man->materialLogicalId, sizeof(man->materialLogicalId));
		const std::uint32_t tris = man->indexCount / 3;
		for (std::uint32_t ti = 0; ti < tris; ++ti)
		{
			classes[tCursor + ti] = man->sourceClass;
			matIds[tCursor + ti] = man->materialLogicalId;
			matIdx[tCursor + ti] = 0;
		}
		vCursor += man->vertexCount;
		iCursor += man->indexCount;
		tCursor += tris;
	}

    RtCpuRewriteSkinnedPreparedView skin;
    {
        OPTICK_EVENT("PT CPU Worker Skinned Prepare");
        uint64_t vc = 0, ic = 0, jc = 0;
        for (uint32_t si = 0; si < n; ++si)
        {
            const auto& man = *order[si];
            if (man.sourceClass != kRtCpuRewriteClassSkinnedEntity) continue;
            if (!man.jointCount || man.jointCount > 4096 || !man.vertexCount || !man.indexCount || man.indexCount % 3)
                return false;
            ++skin.count;
            vc += man.vertexCount; ic += man.indexCount; jc += man.jointCount;
        }
        if (vc > INT_MAX || ic > INT_MAX || jc > INT_MAX / 2 || skin.count > kRtCpuRewriteTlasMaxInstances) return false;
        skin.vertexCount = static_cast<uint32_t>(vc); skin.indexCount = static_cast<uint32_t>(ic);
        skin.jointCount = static_cast<uint32_t>(jc);
        const uint64_t capacities[8] = {
            vc * sizeof(PathTraceSkinnedSourceVertex), ic * sizeof(uint32_t), vc * sizeof(PathTraceSmokeVertex),
            skin.count * sizeof(PathTraceSkinnedSurfaceDispatchRecord), jc * 2 * sizeof(PathTraceSkinnedJointMatrix),
            vc * sizeof(PathTraceSkinnedPreviousPosition), skin.count * sizeof(PathTraceSkinnedHitRouteGpuRecord),
            ic / 3 * sizeof(PathTraceSkinnedHitRouteGpuTriangle)
        };
        std::copy(capacities, capacities + 8, skin.capacities);
        // This heap scratch is temporary, separately charged from the allocated scratch arena.
        const uint64_t heapScratch = ic / 3 * 1024ull + skin.count * 4096ull;
        OPTICK_TAG("skinWorkerRows", skin.count);
        OPTICK_TAG("skinWorkerVertices", skin.vertexCount);
        OPTICK_TAG("skinWorkerIndexes", skin.indexCount);
        OPTICK_TAG("skinWorkerJoints", skin.jointCount);
        OPTICK_TAG("skinWorkerHeapScratch", heapScratch);
        OPTICK_TAG("skinWorkerHeapLimit", kSkinnedHeapScratchBytes);
        OPTICK_TAG("skinWorkerHeapRejected", heapScratch > kSkinnedHeapScratchBytes ? 1u : 0u);
        if (heapScratch > kSkinnedHeapScratchBytes) return false;
        if (skin.count)
        {
            auto* rows = reinterpret_cast<RtCpuRewriteSkinnedLayoutRow*>(bump(skin.count * sizeof(RtCpuRewriteSkinnedLayoutRow), alignof(RtCpuRewriteSkinnedLayoutRow)));
            auto* sv = reinterpret_cast<PathTraceSkinnedSourceVertex*>(bump(capacities[0], alignof(PathTraceSkinnedSourceVertex)));
            auto* ix = reinterpret_cast<uint32_t*>(bump(capacities[1], alignof(uint32_t)));
            auto* bi = reinterpret_cast<uint32_t*>(bump(capacities[1], alignof(uint32_t)));
            auto* dispatch = reinterpret_cast<PathTraceSkinnedSurfaceDispatchRecord*>(bump(capacities[3], alignof(PathTraceSkinnedSurfaceDispatchRecord)));
            auto* records = reinterpret_cast<PathTraceSkinnedHitRouteGpuRecord*>(bump(capacities[6], alignof(PathTraceSkinnedHitRouteGpuRecord)));
            auto* triangles = reinterpret_cast<PathTraceSkinnedHitRouteGpuTriangle*>(bump(capacities[7], alignof(PathTraceSkinnedHitRouteGpuTriangle)));
            if (!rows || !sv || !ix || !bi || !dispatch || !records || !triangles) return false;
            std::vector<RtCpuRewriteSkinnedRouteInput> inputs;
            inputs.reserve(skin.count);
            uint32_t vo = 0, io = 0, jo = 0, slot = 0;
            for (uint32_t si = 0; si < n; ++si)
            {
                const auto& man = *order[si];
                if (man.sourceClass != kRtCpuRewriteClassSkinnedEntity) continue;
                const auto& mesh = skinnedMeshes[sourceUniqueSkinned[si]];
                RtCpuRewriteJoinedSkinned js;
                js.meshKey = man.meshKey; js.instanceKey = man.instanceKey;
                js.materialLogicalId = man.materialLogicalId; js.jointCount = man.jointCount;
                rows[slot] = RtCpuRewriteMakeSkinnedLayoutRow(js, mesh, vo, io, jo);
                auto& d = dispatch[slot]; d = {};
                d.sourceVertexOffset = d.outputVertexOffset = d.previousPositionOffset = vo;
                d.vertexCount = mesh.vertexCount; d.currentJointOffset = jo;
                d.previousJointOffset = skin.jointCount + jo; d.surfaceRecordIndex = slot;
                d.dynamicIndexOffset = io; d.triangleCount = mesh.triangleCount;
                d.flags = PT_SKINNED_DISPATCH_RT_CPU_SKINNED | PT_SKINNED_DISPATCH_SOURCE_READY |
                    PT_SKINNED_DISPATCH_HAS_CURRENT_JOINTS;
                std::copy(mesh.vertices, mesh.vertices + mesh.vertexCount, sv + vo);
                for (uint32_t ii = 0; ii < mesh.indexCount; ++ii)
                {
                    if (mesh.indexes[ii] >= mesh.vertexCount) return false;
                    ix[io + ii] = mesh.indexes[ii]; bi[io + ii] = vo + mesh.indexes[ii];
                }
                RtCpuRewriteSkinnedRouteInput in;
                in.meshKey = man.meshKey; in.instanceKey = man.instanceKey;
                in.sourceIndexes = ix + io; in.sourceIndexCount = mesh.indexCount;
                in.sourceIndexOffsetBytes = uint64_t(io) * sizeof(uint32_t); in.sourceIndexCapacityBytes = capacities[1];
                in.outputVertexOffsetBytes = uint64_t(vo) * sizeof(PathTraceSmokeVertex);
                in.outputVertexCount = mesh.vertexCount; in.outputCapacityBytes = capacities[2];
                in.materialLogicalId = man.materialLogicalId;
                inputs.push_back(in);
                vo += mesh.vertexCount; io += mesh.indexCount; jo += man.jointCount; ++slot;
            }
            RtCpuRewriteSkinnedRoutePackage routes;
            if (!RtCpuRewriteBuildSkinnedRoutePackage(inputs, 0, routes, true)) return false;
            std::copy(routes.upload.records.begin(), routes.upload.records.end(), records);
            std::copy(routes.upload.triangles.begin(), routes.upload.triangles.end(), triangles);
            skin.rows = rows; skin.vertices = sv; skin.indexes = ix; skin.blasIndexes = bi;
            skin.dispatches = dispatch; skin.records = records; skin.triangles = triangles;
        }
        OPTICK_TAG("skinWorkerTriangles", skin.indexCount / 3);
    }
    product.view.skinnedPrepared = skin;

	OPTICK_EVENT("PT CPU Geometry Product Seal");
	product.ticket = input.ticket;
	product.inputSeal = input.canonicalSeal;
	product.lifecycleGeneration = input.lifecycleGeneration;
	product.worldGeneration = input.worldGeneration;
	product.mapGeneration = input.mapGeneration;
	product.configGeneration = input.configGeneration;
	product.contentSignature = signature;
	product.rootFrame = input.rootFrame;
	std::uint64_t finalBytes = 0;
	if (!CheckedAlignUp(bumpAt, kAlign, &finalBytes) || finalBytes > product.Limit())
	{
		return false;
	}
	product.usedBytes.store(finalBytes, std::memory_order_release);
	product.view.ticket = product.ticket;
	product.view.inputSeal = product.inputSeal;
	product.view.lifecycleGeneration = product.lifecycleGeneration;
	product.view.worldGeneration = product.worldGeneration;
	product.view.mapGeneration = product.mapGeneration;
	product.view.configGeneration = product.configGeneration;
	product.view.contentSignature = signature;
	product.view.vertexCount = totalVerts;
	product.view.indexCount = totalIndexes;
	product.view.triangleCount = totalTris;
	product.view.sourceCount = n;
	product.view.vertices = verts;
	product.view.indexes = indexes;
	product.view.triangleClassAndFlags = classes;
	product.view.triangleMaterialIds = matIds;
	product.view.triangleMaterialIndexes = matIdx;
	product.view.sources = sources;
	product.view.rootFrame = input.rootFrame;
	product.view.rigidMeshCount = uniqueRigid;
	product.view.rigidMeshes = rigidMeshes;
	product.view.staticContentSignature = RtCpuRewriteStaticGeometrySignature(product.view);
	product.view.skinnedMeshCount = uniqueSkinned;
	product.view.skinnedMeshes = skinnedMeshes;
    if (input.populateStatic)
    {
        OPTICK_EVENT("PT CPU Resident Static Build");
        auto resident = std::make_shared<RtCpuRewriteResidentStatic>();
        resident->revision = input.staticRevision;
        resident->lifecycle = input.lifecycleGeneration;
        resident->world = input.worldGeneration; resident->map = input.mapGeneration;
        resident->signature = product.view.staticContentSignature;
        // All counts have already passed the product arena bounds. Check the
        // separate retained allocation before copying any geometry into it.
        resident->bytes = uint64_t(totalVerts) * sizeof(*verts) + uint64_t(totalIndexes) * sizeof(*indexes) +
            uint64_t(totalTris) * 3 * sizeof(uint32_t) + uint64_t(n) * sizeof(*sources);
        if (resident->bytes > 256ull * 1024 * 1024) return false;
        if (totalVerts) resident->vertices.assign(verts, verts + totalVerts);
        if (totalIndexes) resident->indexes.assign(indexes, indexes + totalIndexes);
        if (totalTris)
        {
            resident->classes.assign(classes, classes + totalTris);
            resident->materials.assign(matIds, matIds + totalTris);
            resident->materialIndexes.assign(matIdx, matIdx + totalTris);
        }
        resident->sources.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
            if (sources[i].sourceClass == kRtCpuRewriteClassStaticWorld) resident->sources.push_back(sources[i]);
        m_residentStatic = std::move(resident);
    }
    if (input.staticRevision)
    {
        product.view.residentStatic = m_residentStatic;
        m_residentStatic->Apply(product.view);
    }
    if (cacheable)
    {
        OPTICK_EVENT("PT CPU Resident Geometry Product Build");
        auto resident = std::make_shared<RtCpuRewriteResidentDynamic>();
        resident->identities = std::move(identities);
        resident->CopyView(product.view, product.Data(), finalBytes);
        m_residentDynamic = resident;
        product.view = resident->view;
        product.view.residentDynamic = std::move(resident);
    }
    {
        std::lock_guard<std::mutex> lock(m_counterMutex);
        m_counters.convertedMeshHits += convertedHits;
        m_counters.convertedMeshMisses += convertedMisses;
    }
    OPTICK_TAG("convertedMeshReuseCount", convertedHits);
    OPTICK_TAG("convertedMeshBuildCount", convertedMisses);
    OPTICK_TAG("convertedMeshReusedBytes", convertedReuseBytes);
    OPTICK_TAG("residentGeometryProductBytes", cacheable ? finalBytes : 0);
    OPTICK_TAG("residentGeometrySources", n);
    OPTICK_TAG("residentStaticBuilt", input.populateStatic ? 1u : 0u);
    OPTICK_TAG("residentStaticReused", input.staticRevision && !input.populateStatic ? 1u : 0u);
    OPTICK_TAG("residentStaticBytes", input.staticRevision ? m_residentStatic->bytes : 0);
    OPTICK_TAG("residentStaticSources", product.view.StaticSourceCount());
	NoteSlotBytes();
	return true;
}


void RtCpuProducerRewriteService::FreeOverlaySlot(OverlaySlot& slot)
{
	slot.rootFrame = 0;
	slot.lifecycleGeneration = 0;
	slot.rowCount = 0;
	slot.jointCount = 0;
	slot.view = {};
	slot.state.store(OverlaySlotState::Free, std::memory_order_release);
}

namespace {
bool GrowRewriteOverlayArena(uint8_t*& arena, uint64_t& capacity, uint64_t required, uint64_t limit, uint64_t used)
{
    if (!arena || used > capacity) return false;
    if (required <= capacity) return true;
    const auto next = RtCpuRewriteArenaGrowth(capacity, required, limit);
    if (!next) return false;
    auto replacement = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[next]);
    if (!replacement) return false;
    if (used) std::memcpy(replacement.get(), arena, static_cast<size_t>(used));
    delete[] arena;
    arena = replacement.release(); capacity = next;
    return true;
}
}

bool RtCpuProducerRewriteService::CopyOverlayFromInput(InputSlot& input, OverlaySlot& overlay, std::uint64_t rootFrame)
{
	if (!overlay.arena)
	{
		return false;
	}
	overlay.jointCount = 0;
	std::uint32_t cursor = input.receiptHead.load(std::memory_order_acquire);
	const std::uint64_t used = input.cursor.load(std::memory_order_acquire);
	RtCpuRewriteOverlayRow* rows = reinterpret_cast<RtCpuRewriteOverlayRow*>(overlay.arena);
    std::uint32_t visited = 0;
	std::uint32_t count = 0;
	while (cursor != kReceiptSentinel)
	{
		if (++visited > input.receipts.load(std::memory_order_acquire))
		{
			return false;
		}
		if (cursor < sizeof(std::uint64_t) ||
			static_cast<std::uint64_t>(cursor) + sizeof(ShardReceipt) > used ||
			(cursor % alignof(ShardReceipt)) != 0)
		{
			return false;
		}
		const auto* receipt = reinterpret_cast<const ShardReceipt*>(input.Data() + cursor);
		if (receipt->manifestOffset < sizeof(std::uint64_t) ||
			static_cast<std::uint64_t>(receipt->manifestOffset) + sizeof(SurfaceManifest) > used ||
			(receipt->manifestOffset % alignof(SurfaceManifest)) != 0)
		{
			return false;
		}
		const auto* man = reinterpret_cast<const SurfaceManifest*>(input.Data() + receipt->manifestOffset);
		if (man->sourceClass == kRtCpuRewriteClassRigidEntity ||
			man->sourceClass == kRtCpuRewriteClassSkinnedEntity)
		{
            if (count >= kRtCpuRewriteMaxExtras || !GrowRewriteOverlayArena(overlay.arena,
                overlay.rowCapacity, (uint64_t(count) + 1) * sizeof(RtCpuRewriteOverlayRow),
                kOverlayMaxBytes, uint64_t(count) * sizeof(RtCpuRewriteOverlayRow))) {
                RequestRecovery(1); return false;
            }
            rows = reinterpret_cast<RtCpuRewriteOverlayRow*>(overlay.arena);
			RtCpuRewriteOverlayRow& row = rows[count];
			row = {};
			row.instanceKey = man->instanceKey;
			row.meshKey = man->meshKey;
			std::memcpy(row.currentObjectToWorld, man->modelMatrix, sizeof(row.currentObjectToWorld));
			std::memcpy(row.previousObjectToWorld, man->modelMatrix, sizeof(row.previousObjectToWorld));
			row.materialLogicalId = man->materialLogicalId;
			row.surfaceOrdinal = man->surfaceOrdinal;
			row.sourceClass = man->sourceClass;
			row.sourceContentSignature = man->sourceContentSignature;
			if (man->sourceClass == kRtCpuRewriteClassSkinnedEntity)
			{
				std::uint64_t jointBytes = 0;
				std::uint64_t jointEnd = 0;
				if (!overlay.jointArena || man->jointCount == 0 || man->jointCount > 4096 ||
					(man->jointOffset % alignof(PathTraceSkinnedJointMatrix)) != 0 ||
					!CheckedMul(man->jointCount, sizeof(PathTraceSkinnedJointMatrix), &jointBytes) ||
					!CheckedAdd(man->jointOffset, jointBytes, &jointEnd) || jointEnd > used)
				{
					return false;
				}
				const std::uint64_t usedJoints = static_cast<std::uint64_t>(overlay.jointCount) * sizeof(PathTraceSkinnedJointMatrix);
				std::uint64_t overlayEnd = 0;
				if (!CheckedAdd(usedJoints, jointBytes, &overlayEnd) ||
                    !GrowRewriteOverlayArena(overlay.jointArena, overlay.jointCapacity,
                        overlayEnd, kOverlayJointMaxBytes, usedJoints))
                {
                    RequestRecovery(2);
					return false;
				}
				std::memcpy(overlay.jointArena + usedJoints, input.Data() + man->jointOffset, static_cast<std::size_t>(jointBytes));
				row.jointOffset = overlay.jointCount;
				row.jointCount = man->jointCount;
				overlay.jointCount += man->jointCount;
			}
			++count;
		}
		cursor = receipt->nextOffset;
	}
	{
		OPTICK_EVENT("PT CPU Overlay Canonical Order");
		// Receipt arrival is a concurrent job-order detail, never a packed-layout key.
		// Rows own offsets into the separate palette arena, so sorting keeps joints exact.
		std::sort(rows, rows + count, [](const RtCpuRewriteOverlayRow& a, const RtCpuRewriteOverlayRow& b)
		{
			if (int c = CmpInstanceKey(a.instanceKey, b.instanceKey)) return c < 0;
			if (int c = CmpMeshKey(a.meshKey, b.meshKey)) return c < 0;
			return a.surfaceOrdinal < b.surfaceOrdinal;
		});
	}
	overlay.rootFrame = rootFrame;
	overlay.lifecycleGeneration = input.lifecycleGeneration;
	overlay.rowCount = count;
	overlay.view.rootFrame = rootFrame;
    overlay.view.staticRevision = input.staticRevision;
	overlay.view.lifecycleGeneration = overlay.lifecycleGeneration;
	overlay.view.rowCount = count;
	overlay.view.rows = rows;
	overlay.view.jointCount = overlay.jointCount;
	overlay.view.joints = overlay.jointCount && overlay.jointArena
		? reinterpret_cast<const PathTraceSkinnedJointMatrix*>(overlay.jointArena)
		: nullptr;
    OPTICK_TAG("overlayRowCapacityBytes", overlay.rowCapacity);
    OPTICK_TAG("overlayJointCapacityBytes", overlay.jointCapacity);
	return true;
}

bool RtCpuProducerRewriteService::SealOverlayFromCapturingInput(std::uint64_t rootFrame,
    std::shared_ptr<const PathTraceDoomAnalyticLightSnapshotData> analyticLights)
{
	OPTICK_EVENT("PT CPU Overlay Seal");
	if (rootFrame == 0)
	{
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.overlayDrop;
		return false;
	}
	const int inputIndex = m_currentInput.load();
	if (inputIndex < 0)
	{
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.overlayDrop;
		return false;
	}
	std::unique_lock<std::mutex> overlayLock(m_overlayMutex);
	int freeIndex = -1;
	for (std::uint32_t i = 0; i < kOverlaySlots; ++i)
	{
		OverlaySlotState expected = OverlaySlotState::Free;
		if (m_overlays[i].state.compare_exchange_strong(expected, OverlaySlotState::Writing))
		{
			freeIndex = static_cast<int>(i);
			break;
		}
	}
	if (freeIndex < 0)
	{
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.overlayDrop;
		return false;
	}
	overlayLock.unlock();
	if (!CopyOverlayFromInput(m_inputs[inputIndex], m_overlays[freeIndex], rootFrame))
	{
		overlayLock.lock();
		FreeOverlaySlot(m_overlays[freeIndex]);
		std::lock_guard<std::mutex> lock(m_counterMutex);
		++m_counters.overlayDrop;
		return false;
	}
	std::lock_guard<std::mutex> lock(m_counterMutex);
	m_overlays[freeIndex].view.analyticLights = std::move(analyticLights);
	m_overlays[freeIndex].state.store(OverlaySlotState::Sealed, std::memory_order_release);
	++m_counters.overlaySeal;
	++m_counters.overlayAcquire;
	return true;
}

void RtCpuProducerRewriteService::DiscardOverlaysThrough(std::uint64_t rootFrame)
{
	std::lock_guard<std::mutex> lock(m_overlayMutex);
	std::uint64_t reclaimed = 0;
	for (auto& slot : m_overlays)
	{
		if (slot.state.load(std::memory_order_acquire) == OverlaySlotState::Sealed && slot.rootFrame <= rootFrame)
		{
			FreeOverlaySlot(slot);
			++reclaimed;
		}
	}
	std::lock_guard<std::mutex> counters(m_counterMutex);
	m_counters.overlayReclaimed += reclaimed;
}

bool RtCpuProducerRewriteService::TryAcquireOverlay(std::uint64_t viewRootFrame)
{
	OPTICK_EVENT("PT CPU Overlay Apply");
	if (viewRootFrame) DiscardOverlaysThrough(viewRootFrame - 1);
	std::unique_lock<std::mutex> lock(m_overlayMutex);
	int chosen = -1;
	for (std::uint32_t i = 0; viewRootFrame && i < kOverlaySlots; ++i)
	{
		if (m_overlays[i].state.load(std::memory_order_acquire) == OverlaySlotState::Sealed &&
			m_overlays[i].rootFrame == viewRootFrame) { chosen = static_cast<int>(i); break; }
	}
	if (chosen < 0)
	{
		std::lock_guard<std::mutex> counters(m_counterMutex);
		++m_counters.overlayFrameMismatch;
		return false;
	}
	auto& slot = m_overlays[chosen];
	slot.state.store(OverlaySlotState::Consuming, std::memory_order_release);
	m_currentOverlay.store(chosen);
	lock.unlock();
	// Previous history belongs to the backend. The frontend never reads this cache.
	if (m_lastOverlay.rootFrame != 0 && m_lastOverlay.rootFrame < slot.rootFrame &&
		m_lastOverlay.lifecycleGeneration == slot.lifecycleGeneration)
	{
		auto* rows = reinterpret_cast<RtCpuRewriteOverlayRow*>(slot.arena);
		std::uint32_t previousIndex = 0;
		for (std::uint32_t i = 0; i < slot.rowCount; ++i)
		{
			while (previousIndex < m_lastOverlay.rowCount &&
				CmpInstanceKey(m_lastOverlay.view.rows[previousIndex].instanceKey, rows[i].instanceKey) < 0) ++previousIndex;
			if (previousIndex < m_lastOverlay.rowCount &&
				CmpInstanceKey(m_lastOverlay.view.rows[previousIndex].instanceKey, rows[i].instanceKey) == 0)
			{
				std::memcpy(rows[i].previousObjectToWorld, m_lastOverlay.view.rows[previousIndex].currentObjectToWorld,
					sizeof(rows[i].previousObjectToWorld));
				rows[i].flags = PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM | PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS;
			}
		}
	}
	OPTICK_TAG("overlayRootFrame", viewRootFrame);
    OPTICK_TAG("overlayPreviousCommittedRoot", m_lastOverlay.rootFrame);
	return true;
}

const RtCpuRewriteOverlayView* RtCpuProducerRewriteService::OverlayView() const
{
	const int index = m_currentOverlay.load();
	if (index < 0)
	{
		return nullptr;
	}
	if (m_overlays[index].state.load() != OverlaySlotState::Consuming)
	{
		return nullptr;
	}
	return &m_overlays[index].view;
}

void RtCpuProducerRewriteService::ReleaseConsumedOverlay(bool committed)
{
	const int index = m_currentOverlay.exchange(-1);
	if (index < 0)
	{
		return;
	}
	OverlaySlot& slot = m_overlays[index];
    if (committed && slot.view.rows)
	{
		const std::size_t bytes = sizeof(RtCpuRewriteOverlayRow) * slot.rowCount;
        if (GrowRewriteOverlayArena(m_lastOverlay.arena, m_lastOverlay.rowCapacity, bytes, kOverlayMaxBytes, 0))
		{
			std::memcpy(m_lastOverlay.arena, slot.view.rows, bytes);
			m_lastOverlay.rowCount = slot.rowCount;
			m_lastOverlay.rootFrame = slot.rootFrame;
			m_lastOverlay.lifecycleGeneration = slot.lifecycleGeneration;
			m_lastOverlay.view.rootFrame = slot.rootFrame;
			m_lastOverlay.view.lifecycleGeneration = slot.lifecycleGeneration;
			m_lastOverlay.view.rowCount = slot.rowCount;
			m_lastOverlay.view.rows = reinterpret_cast<RtCpuRewriteOverlayRow*>(m_lastOverlay.arena);
            // Skinned joints have their own transaction-committed history. This
            // cache supplies rigid transforms and the last committed root only.
            m_lastOverlay.jointCount = m_lastOverlay.view.jointCount = 0;
            m_lastOverlay.view.joints = nullptr;
		}
        else FreeOverlaySlot(m_lastOverlay); // Allocation failure must not expose stale history.
	}
	std::lock_guard<std::mutex> lock(m_overlayMutex);
	FreeOverlaySlot(slot);
}

bool RtCpuProducerRewriteService::BuildJoinPlan(std::uint64_t viewRootFrame, RtCpuRewriteJoinResult& out)
{
	out = {};
	const RtCpuRewriteFrozenProductView* product = ProductView();
	if (!product)
	{
		out.failReason = kRtCpuRewriteJoinOk;
		return true;
	}
	const RtCpuRewriteOverlayView* overlay = OverlayView();
	const bool overlayPresent = overlay && viewRootFrame != 0 && overlay->rows;
	std::uint32_t packedV = 0;
	std::uint32_t packedI = 0;
	std::uint32_t packedT = 0;
	std::vector<std::uint8_t> meshUsed(product->rigidMeshCount, 0);
    std::vector<uint32_t> sourceOrder(product->sourceCount);
    for (uint32_t i = 0; i < product->sourceCount; ++i) sourceOrder[i] = i;
    std::sort(sourceOrder.begin(), sourceOrder.end(), [&](uint32_t a, uint32_t b) {
        const int c = CmpInstanceKey(product->sources[a].instanceKey, product->sources[b].instanceKey);
        return c != 0 ? c < 0 : a < b;
    });
    auto findSource = [&](const PtCanonicalInstanceKey& key) -> const RtCpuRewriteProductSource* {
        const auto it = std::lower_bound(sourceOrder.begin(), sourceOrder.end(), key,
            [&](uint32_t i, const PtCanonicalInstanceKey& k) { return CmpInstanceKey(product->sources[i].instanceKey, k) < 0; });
        return it != sourceOrder.end() && CmpInstanceKey(product->sources[*it].instanceKey, key) == 0 ? &product->sources[*it] : nullptr;
    };

	if (overlayPresent)
	{
		for (std::uint32_t r = 0; r < overlay->rowCount; ++r)
		{
			const RtCpuRewriteOverlayRow& row = overlay->rows[r];
			if (row.sourceClass == kRtCpuRewriteClassSkinnedEntity)
			{
				continue;
			}
			std::uint32_t meshIndex = UINT32_MAX;
			if (const auto* source = findSource(row.instanceKey)) meshIndex = source->rigidMeshIndex;
			if (meshIndex == UINT32_MAX || meshIndex >= product->rigidMeshCount)
			{
				++out.instanceJoinMiss;
				continue;
			}
			if (out.joinedCount >= kRtCpuRewriteMaxExtras)
			{
				RequestRecovery(3);
				out.failReason = kRtCpuRewriteJoinInstanceIdUnresolved;
				return false;
			}
			const RtCpuRewriteRigidMeshView& mesh = product->rigidMeshes[meshIndex];
            if (!RtCpuRewritePackedCountsFit(uint64_t(packedV) + mesh.vertexCount,
                uint64_t(packedI) + mesh.indexCount, uint64_t(packedT) + mesh.triangleCount,
                uint64_t(out.joinedCount) + 1)) {
                RequestRecovery(5);
                out.failReason = kRtCpuRewriteJoinRetainCap;
                return false;
            }
			RtCpuRewriteJoinedInstance joined = {};
			joined.routeRecordIndex = out.joinedCount;
			joined.meshIndex = meshIndex;
			joined.instanceId = 2u + out.joinedCount;
			joined.instanceMask = 0x02;
			joined.meshKey = mesh.meshKey;
			joined.instanceKey = row.instanceKey;
			std::memcpy(joined.currentObjectToWorld, row.currentObjectToWorld, sizeof(joined.currentObjectToWorld));
			joined.route.vertexOffset = packedV;
			joined.route.indexOffset = packedI;
			joined.route.triangleOffset = packedT;
			joined.route.materialId = row.materialLogicalId;
			joined.route.materialIndex = 0;
			joined.route.vertexCount = mesh.vertexCount;
			joined.route.indexCount = mesh.indexCount;
			joined.route.triangleCount = mesh.triangleCount;
			joined.route.flags = row.flags;
			RtCpuRewriteBuildAffineFromObjectToWorld(row.currentObjectToWorld, joined.route.currentObjectToWorld);
			RtCpuRewriteBuildAffineFromObjectToWorld(row.previousObjectToWorld, joined.route.previousObjectToWorld);
			packedV += mesh.vertexCount;
			packedI += mesh.indexCount;
			packedT += mesh.triangleCount;
			meshUsed[meshIndex] = 1;
			out.joined.push_back(joined);
			++out.joinedCount;
			++out.instanceJoinHit;
		}
	}
	for (std::uint32_t m = 0; m < product->rigidMeshCount; ++m)
	{
		if (!meshUsed[m])
		{
			++out.omittedGeometryWithoutOverlay;
		}
	}
	if (overlayPresent)
	{
		for (std::uint32_t r = 0; r < overlay->rowCount; ++r)
		{
			const RtCpuRewriteOverlayRow& row = overlay->rows[r];
			if (row.sourceClass != kRtCpuRewriteClassSkinnedEntity)
			{
				continue;
			}
			std::uint32_t meshIndex = UINT32_MAX;
			if (const auto* source = findSource(row.instanceKey)) meshIndex = source->skinnedMeshIndex;
			if (meshIndex == UINT32_MAX || meshIndex >= product->skinnedMeshCount)
			{
				++out.instanceJoinMiss;
				continue;
			}
			RtCpuRewriteJoinedSkinned js = {};
			js.meshIndex = meshIndex;
			js.instanceId = 0;
			js.instanceMask = 0x02;
			js.jointOffset = row.jointOffset;
			js.jointCount = row.jointCount;
			js.sourceContentSignature = row.sourceContentSignature;
			std::memcpy(js.currentObjectToWorld, row.currentObjectToWorld, sizeof(js.currentObjectToWorld));
			std::memcpy(js.previousObjectToWorld, row.previousObjectToWorld, sizeof(js.previousObjectToWorld));
			js.meshKey = row.meshKey;
			js.instanceKey = row.instanceKey;
			js.materialLogicalId = row.materialLogicalId;
			out.skinned.push_back(js);
			++out.skinnedCount;
			++out.instanceJoinHit;
		}
	}
	out.rigidRouteVertexCount = packedV;
    if (!RtCpuRewriteSceneInstanceCountFits(out.joinedCount, out.skinnedCount)) {
        RequestRecovery(3);
        out.failReason = kRtCpuRewriteJoinInstanceIdUnresolved;
        return false;
    }
	out.rigidRouteIndexCount = packedI;
	out.rigidRouteTriangleCount = packedT;
	out.rigidRouteInstanceCount = out.joinedCount;
	out.packedBytes = static_cast<std::uint64_t>(packedV) * sizeof(PathTraceSmokeVertex) +
		static_cast<std::uint64_t>(packedI) * sizeof(std::uint32_t) +
		static_cast<std::uint64_t>(packedT) * sizeof(std::uint32_t) * 2 +
		static_cast<std::uint64_t>(out.joinedCount) * sizeof(PathTraceRigidRouteInstance);
	std::vector<std::uint32_t> masks(out.joinedCount);
	for (std::uint32_t i = 0; i < out.joinedCount; ++i)
	{
		masks[i] = out.joined[i].instanceMask;
	}
	if (!RtCpuRewriteValidatePackedRouteCommit(
			out.rigidRouteVertexCount,
			out.rigidRouteIndexCount,
			out.rigidRouteTriangleCount,
			out.rigidRouteInstanceCount,
			out.joinedCount,
			out.joinedCount ? masks.data() : nullptr,
			&out.failReason))
	{
		return false;
	}
	m_lastSkinnedJoined = out.skinned;
	m_lastJoined.clear();
	m_lastJoined.reserve(out.joinedCount);
	for (std::uint32_t i = 0; i < out.joinedCount; ++i)
	{
		RtCpuRewriteLastJoined last = {};
		last.instanceKey = overlayPresent ? overlay->rows[0].instanceKey : PtCanonicalInstanceKey{};
		last.meshKey = product->rigidMeshes[out.joined[i].meshIndex].meshKey;
		last.route = out.joined[i].route;
		m_lastJoined.push_back(last);
	}
	if (overlayPresent)
	{
		std::uint32_t overlayCursor = 0;
		m_lastJoined.clear();
		for (std::uint32_t r = 0; r < overlay->rowCount && overlayCursor < out.joinedCount; ++r)
		{
			const RtCpuRewriteOverlayRow& row = overlay->rows[r];
            const auto* source = findSource(row.instanceKey);
            const bool matched = source && source->rigidMeshIndex != UINT32_MAX;
			if (!matched)
			{
				continue;
			}
			RtCpuRewriteLastJoined last = {};
			last.instanceKey = row.instanceKey;
			last.meshKey = row.meshKey;
			last.route = out.joined[overlayCursor].route;
			m_lastJoined.push_back(last);
			++overlayCursor;
		}
	}
	m_lastPackedVertexCount = packedV;
	m_lastPackedIndexCount = packedI;
	m_lastPackedTriangleCount = packedT;
	{
		std::lock_guard<std::mutex> lock(m_counterMutex);
		m_counters.instanceJoinHit += out.instanceJoinHit;
		m_counters.instanceJoinMiss += out.instanceJoinMiss;
	}
	return true;
}

bool RtCpuProducerRewriteService::BuildLateJoinPlan(std::uint64_t viewRootFrame, RtCpuRewriteJoinResult& out)
{
	OPTICK_EVENT("PT CPU Overlay Apply");
	out = {};
	if (viewRootFrame == 0 || m_lastJoined.empty())
	{
		out.failReason = kRtCpuRewriteJoinOk;
		return true;
	}
	const RtCpuRewriteOverlayView* overlay = OverlayView();
	if (!overlay || !overlay->rows)
	{
		out.omittedGeometryWithoutOverlay = static_cast<std::uint32_t>(m_lastJoined.size());
		return true;
	}
	for (std::uint32_t r = 0; r < overlay->rowCount; ++r)
	{
		const RtCpuRewriteOverlayRow& row = overlay->rows[r];
		if (row.sourceClass == kRtCpuRewriteClassSkinnedEntity)
		{
			continue;
		}
		const RtCpuRewriteLastJoined* last = nullptr;
		for (std::size_t i = 0; i < m_lastJoined.size(); ++i)
		{
			if (CmpInstanceKey(m_lastJoined[i].instanceKey, row.instanceKey) == 0)
			{
				last = &m_lastJoined[i];
				break;
			}
		}
		if (!last)
		{
			++out.instanceJoinMiss;
			continue;
		}
		if (out.joinedCount >= kRtCpuRewriteMaxExtras)
		{
			out.failReason = kRtCpuRewriteJoinInstanceIdUnresolved;
			return false;
		}
		RtCpuRewriteJoinedInstance joined = {};
		joined.routeRecordIndex = out.joinedCount;
		joined.instanceId = 2u + out.joinedCount;
		joined.instanceMask = 0x02;
		joined.meshKey = last->meshKey;
		joined.instanceKey = row.instanceKey;
		std::memcpy(joined.currentObjectToWorld, row.currentObjectToWorld, sizeof(joined.currentObjectToWorld));
		joined.route = last->route;
		joined.route.materialId = row.materialLogicalId;
		joined.route.flags = row.flags;
		RtCpuRewriteBuildAffineFromObjectToWorld(row.currentObjectToWorld, joined.route.currentObjectToWorld);
		RtCpuRewriteBuildAffineFromObjectToWorld(row.previousObjectToWorld, joined.route.previousObjectToWorld);
		out.joined.push_back(joined);
		++out.joinedCount;
		++out.instanceJoinHit;
	}
	if (overlay)
	{
		for (std::uint32_t r = 0; r < overlay->rowCount; ++r)
		{
			const RtCpuRewriteOverlayRow& row = overlay->rows[r];
			if (row.sourceClass != kRtCpuRewriteClassSkinnedEntity)
			{
				continue;
			}
			RtCpuRewriteJoinedSkinned js = {};
			js.instanceMask = 0x02;
			js.jointOffset = row.jointOffset;
			js.jointCount = row.jointCount;
			js.sourceContentSignature = row.sourceContentSignature;
			std::memcpy(js.currentObjectToWorld, row.currentObjectToWorld, sizeof(js.currentObjectToWorld));
			std::memcpy(js.previousObjectToWorld, row.previousObjectToWorld, sizeof(js.previousObjectToWorld));
			js.meshKey = row.meshKey;
			js.instanceKey = row.instanceKey;
			js.materialLogicalId = row.materialLogicalId;
			for (std::size_t i = 0; i < m_lastSkinnedJoined.size(); ++i)
			{
				if (CmpInstanceKey(m_lastSkinnedJoined[i].instanceKey, row.instanceKey) == 0)
				{
					js.meshIndex = m_lastSkinnedJoined[i].meshIndex;
					break;
				}
			}
			out.skinned.push_back(js);
			++out.skinnedCount;
			++out.instanceJoinHit;
		}
	}
	out.rigidRouteVertexCount = m_lastPackedVertexCount;
	out.rigidRouteIndexCount = m_lastPackedIndexCount;
	out.rigidRouteTriangleCount = m_lastPackedTriangleCount;
	out.rigidRouteInstanceCount = out.joinedCount;
	out.packedBytes = static_cast<std::uint64_t>(out.joinedCount) * sizeof(PathTraceRigidRouteInstance);
	std::vector<std::uint32_t> masks(out.joinedCount);
	for (std::uint32_t i = 0; i < out.joinedCount; ++i)
	{
		masks[i] = out.joined[i].instanceMask;
	}
	if (!RtCpuRewriteValidatePackedRouteCommit(
			out.rigidRouteVertexCount,
			out.rigidRouteIndexCount,
			out.rigidRouteTriangleCount,
			out.rigidRouteInstanceCount,
			out.joinedCount,
			out.joinedCount ? masks.data() : nullptr,
			&out.failReason))
	{
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(m_counterMutex);
		m_counters.instanceJoinHit += out.instanceJoinHit;
		m_counters.instanceJoinMiss += out.instanceJoinMiss;
	}
	return true;
}

void RtCpuProducerRewriteService::NoteMeshGpuHit()
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	++m_counters.meshGpuHit;
}

void RtCpuProducerRewriteService::NoteMeshGpuMiss(std::uint64_t uploadBytes, bool rebuiltBlas)
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	++m_counters.meshGpuMiss;
	m_counters.vertexUploadBytes += uploadBytes;
	if (rebuiltBlas)
	{
		++m_counters.meshGpuRebuild;
	}
}

void RtCpuProducerRewriteService::NoteSkippedUploadFrame()
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	++m_counters.skippedUploadFrames;
}

void RtCpuProducerRewriteService::NoteLateGeometryReuse()
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	++m_counters.lateGeometryReuse;
}

void RtCpuProducerRewriteService::NoteGpuRetainBytes(std::uint64_t bytes)
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	m_counters.gpuRigidRetainBytes = bytes;
}

void RtCpuProducerRewriteService::NoteJoinCommitFail(std::uint32_t failReason)
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	if (failReason == kRtCpuRewriteJoinRouteCountIncomplete)
	{
		++m_counters.routeCountIncomplete;
	}
	else if (failReason == kRtCpuRewriteJoinInstanceIdUnresolved)
	{
		++m_counters.instanceIdUnresolved;
	}
}

void RtCpuProducerRewriteService::NotePackedGeometryFill(bool skipped)
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	if (skipped)
	{
		++m_counters.packedGeometryFillSkipped;
	}
	else
	{
		++m_counters.packedGeometryFill;
	}
}

void RtCpuProducerRewriteService::NoteCaptureEntityDecision(
	PtCanonicalMeshSourceDomain domain,
	RtCpuRewriteCaptureAdmissionReason reason)
{
	const std::uint32_t domainIndex = static_cast<std::uint32_t>(domain);
	const std::uint32_t reasonIndex = static_cast<std::uint32_t>(reason);
	if (domainIndex >= kRtCpuRewriteCaptureSourceDomainCount ||
		reasonIndex >= kRtCpuRewriteCaptureAdmissionReasonCount)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(m_counterMutex);
	++m_counters.captureEntityCandidates[domainIndex];
	if (reason == RtCpuRewriteCaptureAdmissionReason::Admitted ||
		reason == RtCpuRewriteCaptureAdmissionReason::CpuPosedSurface ||
		reason == RtCpuRewriteCaptureAdmissionReason::MissingJointSnapshot)
	{
		++m_counters.captureEntityProvisional[domainIndex];
	}
	else
	{
		++m_counters.captureEntityRejected[domainIndex][reasonIndex];
	}
}

void RtCpuProducerRewriteService::NoteCaptureSkinnedSurfaceDecision(
	RtCpuRewriteCaptureAdmissionReason reason,
	std::uint32_t jointRows)
{
	const std::uint32_t reasonIndex = static_cast<std::uint32_t>(reason);
	if (reasonIndex >= kRtCpuRewriteCaptureAdmissionReasonCount)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(m_counterMutex);
	++m_counters.skinnedSurfaceCandidates;
	if (reason == RtCpuRewriteCaptureAdmissionReason::Admitted)
	{
		++m_counters.skinnedSurfaceAdmitted;
		m_counters.skinnedJointRows += jointRows;
		m_counters.skinnedJointRowsHighWater = std::max(
			m_counters.skinnedJointRowsHighWater, static_cast<std::uint64_t>(jointRows));
	}
	else
	{
		++m_counters.skinnedSurfaceRejected[reasonIndex];
	}
}

void RtCpuProducerRewriteService::NoteRetirementLedger(const RtCpuRewriteRetirementLedger& ledger)
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	m_counters.retirement = ledger;
}

void RtCpuProducerRewriteService::NoteCommitTail(bool transformOnly, std::uint64_t packedVertBytes)
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	if (transformOnly)
	{
		++m_counters.commitTransformOnly;
		m_counters.packedRouteVertexBytes = 0;
	}
	else
	{
		++m_counters.commitFull;
		m_counters.packedRouteVertexBytes = packedVertBytes;
	}
}

void RtCpuProducerRewriteService::GeometryWorkerMain()
{
	OPTICK_THREAD("PT CPU Geometry");
	for (;;)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_cv.wait(lock, [&]
		{
			return m_stop.load() || m_hasWork.load() || m_cancel.load();
		});
		if (m_stop.load())
		{
			break;
		}
		m_hasWork.store(false);

		int inputIndex = -1;
		std::uint64_t bestTicket = 0;
		for (std::uint32_t i = 0; i < kInputSlots; ++i)
		{
			if (m_inputs[i].state.load(std::memory_order_acquire) == RtCpuRewriteInputSlotState::Sealed)
			{
				if (inputIndex < 0 || m_inputs[i].ticket < bestTicket)
				{
					inputIndex = static_cast<int>(i);
					bestTicket = m_inputs[i].ticket;
				}
			}
		}
		if (inputIndex < 0)
		{
			continue;
		}
		InputSlot& input = m_inputs[inputIndex];
		RtCpuRewriteInputSlotState expectedIn = RtCpuRewriteInputSlotState::Sealed;
		if (!input.state.compare_exchange_strong(expectedIn, RtCpuRewriteInputSlotState::WorkerReading))
		{
			continue;
		}
		lock.unlock();
		m_inWorkerReading.store(true, std::memory_order_release);
		while (m_stallWorkerReading.load(std::memory_order_acquire) && !m_stop.load(std::memory_order_acquire))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		m_inWorkerReading.store(false, std::memory_order_release);

		int productIndex = -1;
		for (std::uint32_t i = 0; i < kProductSlots; ++i)
		{
			RtCpuRewriteProductSlotState expected = RtCpuRewriteProductSlotState::Free;
			if (m_products[i].state.compare_exchange_strong(expected, RtCpuRewriteProductSlotState::Building))
			{
				productIndex = static_cast<int>(i);
				break;
			}
		}
		if (productIndex < 0)
		{
            if (input.populateStatic)
            {
                std::lock_guard<std::mutex> queueLock(m_mutex);
                if (m_staticRequestedRevision.load() == input.staticRevision) m_staticPopulationState.store(0);
            }
			FreeInputSlot(input);
			std::lock_guard<std::mutex> counters(m_counterMutex);
			++m_counters.productPressureDrops;
			++m_counters.workerFail;
			WakeWorkers();
			continue;
		}

		{
			std::lock_guard<std::mutex> counters(m_counterMutex);
			++m_counters.workerDispatch;
		}
		m_inBuilding.store(true);
		WakeWorkers();
		while (m_stallBuild.load() && !m_stop.load() && !m_cancel.load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		ProductSlot& product = m_products[productIndex];
		const bool cancelled = m_cancel.load() || m_stop.load() ||
			input.lifecycleGeneration != m_lifecycleGeneration.load();
		bool built = false;
		if (!cancelled)
		{
			try { built = BuildFrozenProduct(input, product); }
            catch (const std::bad_alloc&) { built = false; }
		}
		m_inBuilding.store(false);
        const auto populationRevision = input.populateStatic ? input.staticRevision : 0;
        if (populationRevision)
        {
            std::lock_guard<std::mutex> queueLock(m_mutex);
            if (m_staticRequestedRevision.load() == populationRevision)
                m_staticPopulationState.store(built && !cancelled && !m_cancel.load() ? 2u : 0u, std::memory_order_release);
        }
		FreeInputSlot(input);
		if (!built || cancelled || m_cancel.load() || m_stop.load() ||
			product.lifecycleGeneration != m_lifecycleGeneration.load())
		{
			FreeProductSlot(product);
			std::lock_guard<std::mutex> counters(m_counterMutex);
			if (cancelled || m_cancel.load())
			{
				++m_counters.workerCancels;
			}
			else
			{
				++m_counters.workerFail;
			}
			WakeWorkers();
			continue;
		}
		if (m_cancel.load(std::memory_order_acquire) || m_stop.load(std::memory_order_acquire) ||
			product.lifecycleGeneration != m_lifecycleGeneration.load(std::memory_order_acquire))
		{
			FreeProductSlot(product);
			std::lock_guard<std::mutex> counters(m_counterMutex);
			++m_counters.workerCancels;
			WakeWorkers();
			continue;
		}
		{
			std::lock_guard<std::mutex> counters(m_counterMutex);
			++m_counters.workerComplete;
			++m_counters.productsReady;
			m_counters.lastPublishedTicket = product.ticket;
		}
		product.state.store(RtCpuRewriteProductSlotState::Ready, std::memory_order_release);
		WakeWorkers(); // Drain any Sealed inputs already queued before this job started.
	}
}

uint64_t RtCpuRewriteMaterialInput::ChargedBytes() const
{
    uint64_t bytes = registers.size() * sizeof(float) + sources.size() * sizeof(RtCpuRewriteMaterialSource) +
        surfaces.size() * (sizeof(RtCpuRewriteMaterialSurface) + sizeof(RtPathTraceRuntimeMaterialEvalPod) +
            sizeof(RtPathTraceRuntimeMaterialDecisionPod) + sizeof(RtCpuRewriteMaterialFrameSurface) +
            sizeof(RtCpuMaterialRecordSample) + sizeof(uint32_t) + 160) +
        registry.size() * (sizeof(RtCpuRewriteMaterialIdentity) + 64) +
        constantSurfaces.size() * (2 * sizeof(RtCpuRewriteMaterialFrameSurface) + 32);
    for (const auto& source : sources) bytes += uint64_t(source.stages.size()) * sizeof(RtPathTraceRuntimeMaterialStagePod);
    return bytes + bindings.ChargedBytes();
}

bool RtCpuRewriteMaterialInput::WithinCapacity() const
{
    if (surfaces.size() > kMaxSurfaces || constantSurfaces.size() > kMaxSurfaces - surfaces.size() ||
        sources.size() > kMaxSurfaces ||
        registers.size() > kMaxBytes / sizeof(float) ||
        registry.size() > kMaxBytes / (sizeof(RtCpuRewriteMaterialIdentity) + 64)) return false;
    if (ChargedBytes() > kMaxBytes) return false;
    for (const auto& surface : surfaces)
        if (surface.source >= sources.size() || surface.registerBegin > registers.size() ||
            surface.registerCount > registers.size() - surface.registerBegin) return false;
    return true;
}

std::shared_ptr<RtCpuRewriteMaterialJob> RtCpuProducerRewriteService::SubmitMaterials(RtCpuRewriteMaterialInput&& input, bool deferBindings)
{
    if (!input.WithinCapacity() || (deferBindings && (!input.prepareFrame || input.bindings.ChargedBytes() != 0))) return {};
    auto job = std::make_shared<RtCpuRewriteMaterialJob>();
    job->inputCharge = input.ChargedBytes();
    job->deferBindings = deferBindings;
    job->bindingState = deferBindings ? RtCpuRewriteMaterialJob::BindingState::Pending : RtCpuRewriteMaterialJob::BindingState::Ready;
    job->input = std::move(input);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started || m_stop.load() || m_materialBusy || m_lightBusy) return {};
        job->lifecycle = m_lifecycleGeneration.load();
        m_materialBusy = true;
        m_materialJob = job;
    }
    OPTICK_TAG("materialBindingsDeferred", deferBindings ? 1u : 0u);
    OPTICK_TAG("materialWorkerSubmitRoot", job->input.rootFrame);
    m_cv.notify_all();
    return job;
}

bool RtCpuProducerRewriteService::PublishMaterialBindings(
    const std::shared_ptr<RtCpuRewriteMaterialJob>& job, RtCpuMaterialBindingInput&& bindings)
{
    OPTICK_EVENT("PT CPU Material Bindings Publish");
    if (!job) return false;
    const uint64_t charge = bindings.ChargedBytes();
    uint32_t rejected = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!job->deferBindings || job->bindingState != RtCpuRewriteMaterialJob::BindingState::Pending) rejected = 1;
        else if (m_stop.load() || job->lifecycle != m_lifecycleGeneration.load()) rejected = 2;
        else if (charge > RtCpuRewriteMaterialInput::kMaxBytes - job->inputCharge) rejected = 3;
        if (!rejected) {
            job->deferredBindings = std::move(bindings);
            job->bindingState = RtCpuRewriteMaterialJob::BindingState::Ready;
        } else if (job->bindingState == RtCpuRewriteMaterialJob::BindingState::Pending) {
            job->bindingState = RtCpuRewriteMaterialJob::BindingState::Cancelled;
        }
    }
    OPTICK_TAG("materialBindingsPublishRejected", rejected);
    OPTICK_TAG("materialNumericPreparedAtBindings", job->numericPrepared.load() ? 1u : 0u);
    OPTICK_TAG("materialBindingPublishRoot", job->input.rootFrame);
    m_cv.notify_all();
    return rejected == 0;
}

void RtCpuProducerRewriteService::CancelMaterialBindings(const std::shared_ptr<RtCpuRewriteMaterialJob>& job)
{
    if (!job) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (job->bindingState == RtCpuRewriteMaterialJob::BindingState::Pending)
            job->bindingState = RtCpuRewriteMaterialJob::BindingState::Cancelled;
    }
    m_cv.notify_all();
}

bool RtCpuProducerRewriteService::FinishMaterials(const std::shared_ptr<RtCpuRewriteMaterialJob>& job)
{
    OPTICK_EVENT("PT CPU Material Join");
    if (!job) return false;
    std::unique_lock<std::mutex> lock(job->mutex);
    job->cv.wait(lock, [&] { return job->done; });
    const bool valid = job->success && !m_stop.load() && job->lifecycle == m_lifecycleGeneration.load();
    OPTICK_TAG("materialWorkerConsumed", valid ? 1u : 0u);
    OPTICK_TAG("materialWorkerRejected", valid ? 0u : 1u);
    OPTICK_TAG("materialWorkerLifecycleRejected", job->lifecycle != m_lifecycleGeneration.load() ? 1u : 0u);
    return valid;
}

std::shared_ptr<RtCpuRewriteLightJob> RtCpuProducerRewriteService::SubmitLightPreparation(
    uint64_t rootFrame, uint64_t chargedBytes, std::function<bool()> prepare)
{
    if (!rootFrame || chargedBytes > 512ull * 1024 * 1024 || !prepare) return {};
    auto job = std::make_shared<RtCpuRewriteLightJob>();
    job->prepare = std::move(prepare); job->rootFrame = rootFrame; job->chargedBytes = chargedBytes;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started || m_stop.load() || m_materialBusy || m_lightBusy) return {};
        job->lifecycle = m_lifecycleGeneration.load();
        m_lightBusy = true; m_lightJob = job;
    }
    m_cv.notify_all();
    return job;
}

bool RtCpuProducerRewriteService::FinishLightPreparation(const std::shared_ptr<RtCpuRewriteLightJob>& job, uint64_t rootFrame)
{
    OPTICK_EVENT("PT CPU Lights Join", Optick::Category::Wait);
    if (!job) return false;
    std::unique_lock<std::mutex> lock(job->mutex);
    job->cv.wait(lock, [&] { return job->done; });
    const bool valid = job->success && !m_stop.load() && job->rootFrame == rootFrame &&
        job->lifecycle == m_lifecycleGeneration.load();
    OPTICK_TAG("lightWorkerConsumed", valid ? 1u : 0u);
    OPTICK_TAG("lightWorkerRejected", valid ? 0u : 1u);
    OPTICK_TAG("lightWorkerLifecycleRejected", job->lifecycle != m_lifecycleGeneration.load() ? 1u : 0u);
    OPTICK_TAG("lightWorkerRootRejected", job->rootFrame != rootFrame ? 1u : 0u);
    OPTICK_TAG("lightWorkerRoot", job->rootFrame);
    OPTICK_TAG("lightWorkerChargedBytes", job->chargedBytes);
    return valid;
}

std::shared_ptr<RtCpuRewriteLightJob> RtCpuProducerRewriteService::SubmitLightManagerPreparation(
    uint64_t rootFrame, uint64_t chargedBytes, std::function<bool()> prepare)
{
    return SubmitManagerSlot(rootFrame,chargedBytes,std::move(prepare),false);
}
std::shared_ptr<RtCpuRewriteLightJob> RtCpuProducerRewriteService::SubmitMaterialBindingPreparation(
    uint64_t rootFrame, uint64_t chargedBytes, std::function<bool()> prepare)
{
    return SubmitManagerSlot(rootFrame,chargedBytes,std::move(prepare),true);
}
bool RtCpuProducerRewriteService::FinishLightManagerPreparation(
    const std::shared_ptr<RtCpuRewriteLightJob>& job, uint64_t rootFrame)
{
    OPTICK_EVENT("PT CPU Light Manager Join", Optick::Category::Wait);
    return FinishManagerSlot(job,rootFrame,false);
}
bool RtCpuProducerRewriteService::FinishMaterialBindingPreparation(
    const std::shared_ptr<RtCpuRewriteLightJob>& job, uint64_t rootFrame)
{
    OPTICK_EVENT("PT CPU Material Bindings Join", Optick::Category::Wait);
    return FinishManagerSlot(job,rootFrame,true);
}

std::shared_ptr<RtCpuRewriteLightJob> RtCpuProducerRewriteService::SubmitManagerSlot(
    uint64_t rootFrame, uint64_t chargedBytes, std::function<bool()> prepare, bool bindings)
{
    if (!rootFrame || chargedBytes > (bindings ? RtCpuMaterialBindingPlanInput::kMaxBytes : 512ull * 1024 * 1024) || !prepare) return {};
    auto job = std::make_shared<RtCpuRewriteLightJob>();
    job->rootFrame = rootFrame; job->chargedBytes = chargedBytes;
    job->lightManagerPreparation = !bindings; job->materialBindingPreparation = bindings; job->prepare = std::move(prepare);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started || m_stop.load() || m_lightManagerBusy) return {};
        job->lifecycle = m_lifecycleGeneration.load();
        m_lightManagerBusy = true; m_lightManagerJob = job;
    }
    m_cv.notify_all();
    return job;
}

bool RtCpuProducerRewriteService::FinishManagerSlot(
    const std::shared_ptr<RtCpuRewriteLightJob>& job, uint64_t rootFrame, bool bindings)
{
    if (!job) return false;
    std::unique_lock<std::mutex> lock(job->mutex);
    job->cv.wait(lock, [&] { return job->done; });
    const bool valid = job->success && (bindings ? job->materialBindingPreparation : job->lightManagerPreparation) && !m_stop.load() &&
        job->rootFrame == rootFrame && job->lifecycle == m_lifecycleGeneration.load();
    if (bindings) {
        OPTICK_TAG("materialBindingWorkerConsumed",valid ? 1u : 0u);
        OPTICK_TAG("materialBindingWorkerRejected",valid ? 0u : 1u);
        OPTICK_TAG("materialBindingLifecycleRejected",job->lifecycle != m_lifecycleGeneration.load() ? 1u : 0u);
        OPTICK_TAG("materialBindingRootRejected",job->rootFrame != rootFrame ? 1u : 0u);
        OPTICK_TAG("materialBindingWorkerRoot",job->rootFrame);
    } else {
        OPTICK_TAG("lightManagerWorkerConsumed",valid ? 1u : 0u);
        OPTICK_TAG("lightManagerWorkerRejected",valid ? 0u : 1u);
        OPTICK_TAG("lightManagerLifecycleRejected",job->lifecycle != m_lifecycleGeneration.load() ? 1u : 0u);
        OPTICK_TAG("lightManagerRootRejected",job->rootFrame != rootFrame ? 1u : 0u);
        OPTICK_TAG("lightManagerWorkerRoot",job->rootFrame);
    }
    return valid;
}

void RtCpuProducerRewriteService::LightManagerWorkerMain()
{
    OPTICK_THREAD("PT CPU Light Manager");
    for (;;)
    {
        std::shared_ptr<RtCpuRewriteLightJob> job;
        bool stopping = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&] { return m_stop.load() || m_lightManagerJob != nullptr; });
            stopping = m_stop.load(); job.swap(m_lightManagerJob);
            if (!job) return;
        }
        bool success = false;
        try {
            if (job->materialBindingPreparation) {
                OPTICK_EVENT("PT CPU Material Bindings Worker");
                OPTICK_TAG("materialBindingWorkerRoot",job->rootFrame);
                OPTICK_TAG("materialBindingWorkerChargedBytes",job->chargedBytes);
                if (!stopping && job->lifecycle == m_lifecycleGeneration.load()) success = job->prepare();
            } else {
                OPTICK_EVENT("PT CPU Light Manager Worker");
                OPTICK_TAG("lightManagerWorkerRoot",job->rootFrame);
                OPTICK_TAG("lightManagerWorkerChargedBytes",job->chargedBytes);
                if (!stopping && job->lifecycle == m_lifecycleGeneration.load()) success = job->prepare();
            }
        } catch (...) { success = false; }
        job->prepare = {};
        { std::lock_guard<std::mutex> lock(m_mutex); m_lightManagerBusy = false; }
        { std::lock_guard<std::mutex> lock(job->mutex); job->success = success; job->done = true; }
        job->cv.notify_all();
        if (stopping) return;
    }
}

std::shared_ptr<RtCpuRewriteLightJob> RtCpuProducerRewriteService::SubmitMaterialRecords(
    uint64_t rootFrame, uint64_t chargedBytes, std::function<bool()> prepare)
{
    return SubmitPlanningPreparation(rootFrame, chargedBytes, std::move(prepare), false);
}

std::shared_ptr<RtCpuRewriteLightJob> RtCpuProducerRewriteService::SubmitRigidPreparation(
    uint64_t rootFrame, uint64_t chargedBytes, std::function<bool()> prepare)
{
    return SubmitPlanningPreparation(rootFrame, chargedBytes, std::move(prepare), true);
}

std::shared_ptr<RtCpuRewriteLightJob> RtCpuProducerRewriteService::SubmitPlanningPreparation(
    uint64_t rootFrame, uint64_t chargedBytes, std::function<bool()> prepare, bool rigid)
{
    if (!rootFrame || chargedBytes > 256ull * 1024 * 1024 || !prepare) return {};
    auto job = std::make_shared<RtCpuRewriteLightJob>();
    job->prepare = std::move(prepare); job->rootFrame = rootFrame; job->chargedBytes = chargedBytes;
    job->rigidPreparation = rigid;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started || m_stop.load() || m_materialRecordsBusy) return {};
        job->lifecycle = m_lifecycleGeneration.load();
        m_materialRecordsBusy = true; m_materialRecordsJob = job;
    }
    m_cv.notify_all();
    return job;
}

bool RtCpuProducerRewriteService::FinishMaterialRecords(const std::shared_ptr<RtCpuRewriteLightJob>& job, uint64_t rootFrame)
{
    OPTICK_EVENT("PT CPU Material Records Join", Optick::Category::Wait);
    if (!job) return false;
    std::unique_lock<std::mutex> lock(job->mutex);
    job->cv.wait(lock, [&] { return job->done; });
    const bool valid = job->success && !job->rigidPreparation && !m_stop.load() && job->rootFrame == rootFrame &&
        job->lifecycle == m_lifecycleGeneration.load();
    OPTICK_TAG("materialRecordsWorkerConsumed", valid ? 1u : 0u);
    OPTICK_TAG("materialRecordsWorkerRejected", valid ? 0u : 1u);
    OPTICK_TAG("materialRecordsWorkerLifecycleRejected", job->lifecycle != m_lifecycleGeneration.load() ? 1u : 0u);
    OPTICK_TAG("materialRecordsWorkerRootRejected", job->rootFrame != rootFrame ? 1u : 0u);
    OPTICK_TAG("materialRecordsWorkerRoot", job->rootFrame);
    OPTICK_TAG("materialRecordsWorkerChargedBytes", job->chargedBytes);
    return valid;
}

bool RtCpuProducerRewriteService::FinishRigidPreparation(const std::shared_ptr<RtCpuRewriteLightJob>& job, uint64_t rootFrame)
{
    OPTICK_EVENT("PT CPU Rigid Resolve Join", Optick::Category::Wait);
    if (!job) return false;
    std::unique_lock<std::mutex> lock(job->mutex);
    job->cv.wait(lock, [&] { return job->done; });
    const bool valid = job->success && job->rigidPreparation && !m_stop.load() && job->rootFrame == rootFrame &&
        job->lifecycle == m_lifecycleGeneration.load();
    OPTICK_TAG("rigidResolveWorkerConsumed", valid ? 1u : 0u);
    OPTICK_TAG("rigidResolveWorkerRejected", valid ? 0u : 1u);
    OPTICK_TAG("rigidResolveWorkerLifecycleRejected", job->lifecycle != m_lifecycleGeneration.load() ? 1u : 0u);
    OPTICK_TAG("rigidResolveWorkerRootRejected", job->rootFrame != rootFrame ? 1u : 0u);
    OPTICK_TAG("rigidResolveWorkerRoot", job->rootFrame);
    OPTICK_TAG("rigidResolveWorkerChargedBytes", job->chargedBytes);
    return valid;
}

void RtCpuProducerRewriteService::ShadingWorkerMain()
{
    OPTICK_THREAD("PT CPU Shading");
    for (;;)
    {
        std::shared_ptr<RtCpuRewriteMaterialJob> job;
        std::shared_ptr<RtCpuRewriteLightJob> light;
        bool stopping = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&] { return m_stop.load() || m_materialJob != nullptr || m_lightJob != nullptr; });
            stopping = m_stop.load();
            job.swap(m_materialJob);
            light.swap(m_lightJob);
            if (!job && !light) return;
        }
        if (light)
        {
            bool success = false;
            try
            {
                OPTICK_EVENT("PT CPU Lights Worker Prepare");
                OPTICK_TAG("lightWorkerRoot", light->rootFrame);
                OPTICK_TAG("lightWorkerLifecycle", light->lifecycle);
                if (!stopping) success = light->prepare();
            }
            catch (...) { success = false; }
            light->prepare = {}; // release owned inputs, including abandoned jobs
            {
                std::lock_guard<std::mutex> lock(m_mutex); m_lightBusy = false;
            }
            {
                std::lock_guard<std::mutex> lock(light->mutex);
                light->success = success; light->done = true;
            }
            light->cv.notify_all();
            if (stopping) return;
            continue;
        }
        bool success = false;
        try
        {
            OPTICK_EVENT("PT CPU Material Worker");
            if (!stopping)
            {
              {
                OPTICK_EVENT("PT CPU Shading Evaluate");
                job->output.resize(job->input.surfaces.size());
                for (size_t i = 0; i < job->input.surfaces.size(); ++i)
                {
                    const auto& surface = job->input.surfaces[i];
                    const auto& source = job->input.sources[surface.source];
                    const float emptyRegisters = 0.0f;
                    const float* regs = !surface.hasRegisters ? nullptr : surface.registerCount
                        ? job->input.registers.data() + surface.registerBegin : &emptyRegisters;
                    job->output[i] = BuildPathTraceRuntimeMaterialEvalFromPod(true, surface.materialId,
                        source.stages.data(), source.stages.size(), regs, surface.registerCount,
                        source.opaqueCompatibility, surface.origin, true);
                }
                if (job->input.prepareFrame)
                {
                    OPTICK_EVENT("PT CPU Material Frame Prepare");
                    success = BuildRtCpuMaterialFrame(job->input, job->output, job->frame);
                    OPTICK_TAG("materialConstantWorkerRows", static_cast<uint32_t>(job->input.constantSurfaces.size()));
                    OPTICK_TAG("materialFrameSurfaceRows", static_cast<uint32_t>(job->frame.surfaces.size()));
                    OPTICK_TAG("materialFrameActiveIds", static_cast<uint32_t>(job->frame.activeIds.size()));
                    OPTICK_TAG("materialFrameRoot", job->input.rootFrame);
                    OPTICK_TAG("materialStateWorkerSamples",static_cast<uint32_t>(job->frame.recordSamples.size()));
                    OPTICK_TAG("materialStateWorkerEmissiveSamples",static_cast<uint32_t>(job->frame.emissiveSampleOrdinals.size()));
                }
                else success = true;
              }
              if (success && job->input.prepareFrame)
              {
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->numericPrepared.store(true);
                }
                job->cv.notify_all();
                if (job->deferBindings) {
                    OPTICK_EVENT("PT CPU Material Bindings Wait", Optick::Category::Wait);
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [&] {
                        return job->bindingState != RtCpuRewriteMaterialJob::BindingState::Pending ||
                            m_stop.load() || job->lifecycle != m_lifecycleGeneration.load();
                    });
                    success = job->bindingState == RtCpuRewriteMaterialJob::BindingState::Ready &&
                        !m_stop.load() && job->lifecycle == m_lifecycleGeneration.load();
                }
                if (success) {
                    OPTICK_EVENT("PT CPU Material Bindings Worker");
                    job->frame.bindingSafety = BuildRtCpuMaterialBindingSafety(
                        job->deferBindings ? job->deferredBindings : job->input.bindings);
                    OPTICK_TAG("materialBindingWorkerRules", static_cast<uint32_t>(job->frame.bindingSafety.size()));
                }
              }
            }
            OPTICK_TAG("materialWorkerSurfaces", static_cast<uint32_t>(job->output.size()));
        }
        catch (...) { success = false; } // Complete a failed receipt; never escape the thread.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            job->bindingState = RtCpuRewriteMaterialJob::BindingState::Closed;
            m_materialBusy = false;
        }
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->success = success;
            job->done = true;
        }
        job->cv.notify_all();
        if (stopping) return;
    }
}

void RtCpuProducerRewriteService::MaterialRecordsWorkerMain()
{
    OPTICK_THREAD("PT CPU Planning");
    for (;;) {
        std::shared_ptr<RtCpuRewriteLightJob> job;
        bool stopping=false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock,[&] { return m_stop.load() || m_materialRecordsJob != nullptr; });
            stopping=m_stop.load();job.swap(m_materialRecordsJob);
            if (!job) return;
        }
        bool success=false;
        try {
            if (job->rigidPreparation) {
                OPTICK_EVENT("PT CPU Rigid Resolve Worker");
                OPTICK_TAG("rigidResolveWorkerRoot",job->rootFrame);
                if (!stopping) success=job->prepare();
            } else {
                OPTICK_EVENT("PT CPU Material Records Worker");
                OPTICK_TAG("materialRecordsWorkerRoot",job->rootFrame);
                if (!stopping) success=job->prepare();
            }
        } catch (...) { success=false; }
        job->prepare={};
        { std::lock_guard<std::mutex> lock(m_mutex);m_materialRecordsBusy=false; }
        { std::lock_guard<std::mutex> lock(job->mutex);job->success=success;job->done=true; }
        job->cv.notify_all();
        if (stopping) return;
    }
}

bool RtCpuProducerRewriteService::WaitForReadyProduct(std::uint32_t timeoutMs, std::uint64_t minimumTicket)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < deadline)
	{
		for (std::uint32_t i = 0; i < kProductSlots; ++i)
		{
			if (m_products[i].state.load(std::memory_order_acquire) == RtCpuRewriteProductSlotState::Ready &&
            m_products[i].ticket >= minimumTicket)
			{
				return true;
			}
		}
		std::unique_lock<std::mutex> lock(m_mutex);
		m_cv.wait_for(lock, std::chrono::milliseconds(1));
	}
	for (std::uint32_t i = 0; i < kProductSlots; ++i)
	{
		if (m_products[i].state.load(std::memory_order_acquire) == RtCpuRewriteProductSlotState::Ready &&
            m_products[i].ticket >= minimumTicket)
		{
			return true;
		}
	}
	return false;
}

bool RtCpuProducerRewriteService::TryAcquireNewestCompatibleProduct(
	std::uint64_t lifecycleGeneration,
	std::uint64_t worldGeneration,
	std::uint64_t mapGeneration,
	std::uint64_t configGeneration,
	std::uint64_t maxRootFrame, std::uint64_t minRootFrame)
{
	OPTICK_EVENT("PT CPU Geometry Acquire");
	int best = -1;
	std::uint64_t bestTicket = 0;
	for (std::uint32_t i = 0; i < kProductSlots; ++i)
	{
		if (m_products[i].state.load(std::memory_order_acquire) != RtCpuRewriteProductSlotState::Ready)
		{
			continue;
		}
		const ProductSlot& slot = m_products[i];
		if (slot.lifecycleGeneration != lifecycleGeneration)
		{
			std::lock_guard<std::mutex> lock(m_counterMutex);
			++m_counters.productRejectStale;
			continue;
		}
		if (slot.worldGeneration != worldGeneration)
		{
			std::lock_guard<std::mutex> lock(m_counterMutex);
			++m_counters.productRejectWorld;
			continue;
		}
		if (slot.mapGeneration != mapGeneration)
		{
			std::lock_guard<std::mutex> lock(m_counterMutex);
			++m_counters.productRejectMap;
			continue;
		}
		if (slot.configGeneration != configGeneration)
		{
			std::lock_guard<std::mutex> lock(m_counterMutex);
			++m_counters.productRejectConfig;
			continue;
		}
		if (slot.rootFrame > maxRootFrame || slot.rootFrame < minRootFrame) continue;
		if (best < 0 || slot.ticket > bestTicket)
		{
			best = static_cast<int>(i);
			bestTicket = slot.ticket;
		}
	}
	if (best < 0)
	{
		return false;
	}
	RtCpuRewriteProductSlotState expected = RtCpuRewriteProductSlotState::Ready;
	if (!m_products[best].state.compare_exchange_strong(expected, RtCpuRewriteProductSlotState::Consuming))
	{
		return false;
	}
	m_currentProduct.store(best);
	// Only the consumer reclaims Ready products. Workers own Building/Free slots.
	std::uint32_t superseded = 0;
	for (std::uint32_t i = 0; i < kProductSlots; ++i)
	{
		auto& slot = m_products[i];
		if (slot.state.load(std::memory_order_acquire) != RtCpuRewriteProductSlotState::Ready ||
			slot.ticket >= bestTicket || slot.lifecycleGeneration != lifecycleGeneration ||
			slot.worldGeneration != worldGeneration || slot.mapGeneration != mapGeneration ||
			slot.configGeneration != configGeneration) continue;
		expected = RtCpuRewriteProductSlotState::Ready;
		if (slot.state.compare_exchange_strong(expected, RtCpuRewriteProductSlotState::Consuming))
		{
			FreeProductSlot(slot);
			++superseded;
		}
	}
	OPTICK_TAG("supersededReadyProducts", superseded);
	std::lock_guard<std::mutex> lock(m_counterMutex);
	OPTICK_TAG("reacquiredProduct", static_cast<std::uint32_t>(m_counters.lastConsumedTicket == bestTicket));
	++m_counters.productAcquire;
	m_counters.lastConsumedTicket = m_products[best].ticket;
	return true;
}

bool RtCpuProducerRewriteService::TryAcquireExactProduct(std::uint64_t rootFrame,
	std::uint64_t worldGeneration, std::uint64_t mapGeneration, std::uint32_t timeoutMs)
{
	OPTICK_EVENT("PT CPU Geometry Dependency Wait");
	const auto life = LifecycleGeneration();
	const auto config = ConfigGeneration();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	do
	{
		if (!rootFrame || !m_acceptance.load() || m_stop.load() || LifecycleGeneration() != life) break;
		if (TryAcquireNewestCompatibleProduct(life, worldGeneration, mapGeneration, config, rootFrame, rootFrame)) return true;
		std::unique_lock<std::mutex> lock(m_mutex);
		m_cv.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(1)));
	} while (std::chrono::steady_clock::now() < deadline);
	return false;
}

const RtCpuRewriteFrozenProductView* RtCpuProducerRewriteService::ProductView() const
{
	const int index = m_currentProduct.load();
	if (index < 0)
	{
		return nullptr;
	}
	if (m_products[index].state.load() != RtCpuRewriteProductSlotState::Consuming)
	{
		return nullptr;
	}
	return &m_products[index].view;
}

void RtCpuProducerRewriteService::ReleaseConsumedProduct(bool retain)
{
	const int index = m_currentProduct.exchange(-1);
	if (index < 0)
	{
		return;
	}
	auto& slot = m_products[index];
	if (retain && m_acceptance.load(std::memory_order_acquire) &&
		slot.lifecycleGeneration == m_lifecycleGeneration.load(std::memory_order_acquire))
		slot.state.store(RtCpuRewriteProductSlotState::Ready, std::memory_order_release);
	else
		FreeProductSlot(slot);
	NoteSlotBytes();
}

void RtCpuProducerRewriteService::NotifyGpuCommit(std::uint64_t)
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	++m_counters.productCommit;
	if (m_route.load() == RtCpuProducerRewriteRoute::RewriteWarmup)
	{
		m_route.store(RtCpuProducerRewriteRoute::RewriteOnly);
		++m_counters.latch;
	}
}

void RtCpuProducerRewriteService::NotifyGpuReuse()
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	++m_counters.productReuse;
}

RtCpuProducerRewriteRoute RtCpuProducerRewriteService::Route() const
{
	return m_route.load();
}

RtCpuRewriteCounters RtCpuProducerRewriteService::Counters() const
{
	const_cast<RtCpuProducerRewriteService*>(this)->NoteSlotBytes();
	std::lock_guard<std::mutex> lock(m_counterMutex);
	RtCpuRewriteCounters copy = m_counters;
	copy.lifecycleGeneration = m_lifecycleGeneration.load();
	copy.configGeneration = m_configGeneration.load();
	return copy;
}

std::uint64_t RtCpuProducerRewriteService::LifecycleGeneration() const
{
	return m_lifecycleGeneration.load();
}

std::uint64_t RtCpuProducerRewriteService::ConfigGeneration() const
{
	return m_configGeneration.load();
}

#if !defined(RT_PT_CPU_PRODUCER_REWRITE_HARNESS)
namespace { void ClearFrontendResidents(); }
#endif

void RtCpuProducerRewrite_InitService()
{
	if (!g_service)
	{
		g_service = new RtCpuProducerRewriteService();
	}
	g_service->Init();
#if !defined(RT_PT_CPU_PRODUCER_REWRITE_HARNESS)
	tr.cpuProducerRewriteService = g_service;
#endif
}

void RtCpuProducerRewrite_ShutdownService()
{
	if (g_service)
	{
		g_service->Shutdown();
		delete g_service;
		g_service = nullptr;
	}
#if !defined(RT_PT_CPU_PRODUCER_REWRITE_HARNESS)
	ClearFrontendResidents();
	tr.cpuProducerRewriteService = nullptr;
#endif
}

void RtCpuProducerRewrite_Invalidate(RtCpuRewriteInvalidReason reason)
{
	if (g_service)
	{
		g_service->Invalidate(reason);
    #if !defined(RT_PT_CPU_PRODUCER_REWRITE_HARNESS)
        ClearFrontendResidents();
    #endif
	}
}

void RtCpuProducerRewrite_BeginLevelLoad()
{
	if (g_service)
	{
		g_service->BeginLevelLoad();
    #if !defined(RT_PT_CPU_PRODUCER_REWRITE_HARNESS)
        ClearFrontendResidents();
    #endif
	}
}

void RtCpuProducerRewrite_EndLevelLoad()
{
	if (g_service)
	{
		g_service->EndLevelLoad();
	}
}

RtCpuProducerRewriteService* RtCpuProducerRewrite_GetService()
{
	return g_service;
}

#if !defined(RT_PT_CPU_PRODUCER_REWRITE_HARNESS)

extern idCVar r_useParallelAddModels;
extern idCVar r_pathTracingCpuProducerRewrite;

namespace
{
// Written only after Game/Draw and backend work have joined. Capture workers
// read these immutable values until the next idle boundary.
int rewriteSuppressedEntityIndex = -1;
uint32_t rewriteSuppressedMaterialId = 0;
bool RewriteMaterialSuppressed(const idMaterial* material)
{
    return rewriteSuppressedMaterialId != 0 && material &&
        HashPathTraceMaterialName(material->GetName()) == rewriteSuppressedMaterialId;
}
}

void RtCpuProducerRewrite_OnFrameBoundary()
{
    if (!g_service) return;
    OPTICK_EVENT("PT CPU Route Boundary");
    const auto before = g_service->Route();
    const auto epoch = g_service->LifecycleGeneration();
    const auto recoveryBefore = g_service->RecoveryReason();
    const int suppressedEntity = r_pathTracingGeometrySuppressEntityIndex.GetInteger();
    const uint32_t suppressedMaterial = static_cast<uint32_t>(Max(0,
        r_pathTracingGeometrySuppressMaterialId.GetInteger()));
    if (suppressedEntity != rewriteSuppressedEntityIndex ||
        suppressedMaterial != rewriteSuppressedMaterialId)
    {
        rewriteSuppressedEntityIndex = suppressedEntity;
        rewriteSuppressedMaterialId = suppressedMaterial;
        // Rebuild retained membership through the existing drain/ack protocol.
        RtCpuProducerRewrite_Invalidate(RtCpuRewriteInvalidReason::LifecycleReset);
    }
    g_service->ApplyRouteAtFrameBoundary(r_pathTracingCpuProducerRewrite.GetInteger());
    if (g_service->LifecycleGeneration() != epoch) ClearFrontendResidents();
    OPTICK_TAG("routeBoundaryBefore", static_cast<uint32_t>(before));
    OPTICK_TAG("routeBoundaryAfter", static_cast<uint32_t>(g_service->Route()));
    OPTICK_TAG("routeBoundaryInvalidated", g_service->LifecycleGeneration() != epoch ? 1u : 0u);
    OPTICK_TAG("rewriteRecoveryReason", g_service->RecoveryReason());
    if (g_service->RecoveryReason() && g_service->RecoveryReason() != recoveryBefore)
        common->Warning("CPU producer rewrite suspended after capacity/persistent rejection (reason %u); draining to legacy. Reduce load and set r_pathTracingCpuProducerRewrite 0 then 1, or reload the map, to retry.",
            g_service->RecoveryReason());
}

namespace
{
struct ResidentEntitySurface
{
    RtCpuRewriteSurfaceWrite write; // only geometry owner, keys and scalar metadata retained
    std::vector<PathTraceSkinnedJointMatrix> joints;
    const idMaterial* material = nullptr; // frontend-only, override/lifecycle validated
    idVec3 localOrigin;
    uintptr_t vertexIdentity = 0, indexIdentity = 0; // never dereferenced
};
struct ResidentEntity
{
    uint32_t generation = 0, modelEpoch = 0, assetGeneration = 0;
    uint64_t asset = 0;
    idRenderModel* model = nullptr;
    const idMaterial* customShader = nullptr;
    const idDeclSkin* customSkin = nullptr;
    bool initialized = false, live = false, refresh = false;
    uint64_t capturedFrame = 0;
    std::vector<ResidentEntitySurface> surfaces;
};
std::vector<ResidentEntity> residentEntities;
uint64_t residentEntityLifecycle = 0, residentEntityWorld = 0, residentEntityMap = 0;
const idMaterial* residentEntityGlobalMaterial = nullptr;
bool residentEntityGpuSkinning = false;

PtCanonicalMeshSourceDomain ResidentSourceDomain(const renderEntity_t& entity)
{
    const auto* model = entity.hModel;
    PtSourceDomainFacts facts;
    facts.sourcePresent = model != nullptr; facts.staticWorld = model && model->IsStaticWorldModel();
    facts.continuous = model && model->IsDynamicModel() == DM_CONTINUOUS;
    facts.cached = model && model->IsDynamicModel() == DM_CACHED;
    facts.entityJointed = entity.joints || entity.numJoints;
    return PtClassifySourceDomain(facts);
}

bool PrepareResidentEntities(viewDef_t* parms)
{
    OPTICK_EVENT("PT CPU Resident Entity Admission");
    auto* world = parms->renderWorld;
    if (!world) return false;
    const auto lifecycle = g_service->LifecycleGeneration();
    if (residentEntityLifecycle != lifecycle || residentEntityWorld != world->pathTraceWorldLifecycleGeneration ||
        residentEntityMap != world->mapLoadSerial || residentEntityGlobalMaterial != tr.primaryRenderView.globalMaterial ||
        residentEntityGpuSkinning != r_useGPUSkinning.GetBool())
    {
        residentEntities.clear();
        residentEntityLifecycle = lifecycle; residentEntityWorld = world->pathTraceWorldLifecycleGeneration;
        residentEntityMap = world->mapLoadSerial; residentEntityGlobalMaterial = tr.primaryRenderView.globalMaterial;
        residentEntityGpuSkinning = r_useGPUSkinning.GetBool();
    }
    if (world->entityDefs.Num() > LUDICROUS_INDEX) return false;
    residentEntities.resize(world->entityDefs.Num());
    for (auto* ve = parms->viewEntitys; ve; ve = ve->next) ve->pathTraceResident = false;
    std::vector<int> depths(world->numPortalAreas, -1), queue;
    const int startArea = world->PointInArea(parms->renderView.vieworg);
    const int depth = idMath::ClampInt(0,32,r_pathTracingCpuResidentPortalDepth.GetInteger());
    if (startArea >= 0 && startArea < world->numPortalAreas)
    {
        depths[startArea] = 0; queue.push_back(startArea);
        for (size_t i = 0; i < queue.size(); ++i)
        {
            const int area = queue[i];
            if (depths[area] >= depth) continue;
            for (auto* portal = world->portalAreas[area].portals; portal; portal = portal->next)
            {
                const int next = portal->intoArea;
                if (next < 0 || next >= world->numPortalAreas || depths[next] >= 0 ||
                    (portal->doublePortal && (portal->doublePortal->blockingBits & PS_BLOCK_VIEW))) continue;
                depths[next] = depths[area] + 1; queue.push_back(next);
            }
        }
    }
    uint32_t live = 0, refreshed = 0, reused = 0, removed = 0, cold = 0;
    for (int i = 0; i < world->entityDefs.Num(); ++i)
    {
        auto& cached = residentEntities[i];
        auto* def = world->entityDefs[i];
        bool eligible = def && def->parms.hModel && i != rewriteSuppressedEntityIndex;
        PtCanonicalMeshSourceDomain domain = eligible ? ResidentSourceDomain(def->parms) : PtCanonicalMeshSourceDomain::Invalid;
        eligible = eligible && (domain == PtCanonicalMeshSourceDomain::RegisteredRenderModel ||
            (domain == PtCanonicalMeshSourceDomain::SkinnedBindSource && r_useGPUSkinning.GetBool()));
        if (eligible && !r_skipSuppress.GetBool())
            eligible = !(def->parms.suppressSurfaceInViewID && def->parms.suppressSurfaceInViewID == parms->renderView.viewID) &&
                !(def->parms.allowSurfaceInViewID && def->parms.allowSurfaceInViewID != parms->renderView.viewID);
        if (!eligible)
        {
            removed += !cached.surfaces.empty(); cached = {}; continue;
        }
        PtGeometryLifecycle::PtFrontendCanonicalAuthority authority;
        if (!PtGeometryLifecycle::ResolveFrontendCanonicalAuthority(world, i, def->parms.hModel, domain, authority)) return false;
        const uint32_t epoch = PtGeometryLifecycle::EntityModelEpoch(world, i);
        if (cached.model != def->parms.hModel || cached.generation != authority.renderDefGeneration ||
            cached.modelEpoch != epoch || cached.asset != authority.sourceAssetId || cached.assetGeneration != authority.sourceAssetGeneration ||
            cached.customShader != def->parms.customShader || cached.customSkin != def->parms.customSkin)
        {
            cached = {};
            cached.model = def->parms.hModel; cached.generation = authority.renderDefGeneration;
            cached.modelEpoch = epoch; cached.asset = authority.sourceAssetId; cached.assetGeneration = authority.sourceAssetGeneration;
            cached.customShader = def->parms.customShader; cached.customSkin = def->parms.customSkin;
        }
        cached.live = true; ++live;
        const bool visible = def->viewCount == tr.viewCount && def->viewEntity && !def->viewEntity->scissorRect.IsEmpty();
        bool inUpdateRegion = startArea < 0 || !def->entityRefs;
        for (auto* ref = def->entityRefs; ref && !inUpdateRegion; ref = ref->ownerNext)
            inUpdateRegion = ref->area && ref->area->areaNum >= 0 && ref->area->areaNum < world->numPortalAreas && depths[ref->area->areaNum] >= 0;
        const bool exact = visible || def->parms.weaponDepthHack || def->parms.modelDepthHack != 0 || def->parms.allowSurfaceInViewID;
        // Static registered geometry needs no model job for a transform change.
        // Its current matrix and current shader registers are applied at seal.
        cached.refresh = !cached.initialized || ((inUpdateRegion || exact) &&
            (domain == PtCanonicalMeshSourceDomain::SkinnedBindSource || def->parms.callback));
        cold += !cached.initialized;
        if (cached.refresh)
        {
            R_SetEntityDefViewEntity(def)->pathTraceResident = true;
            ++refreshed;
        }
        else ++reused;
    }
    OPTICK_TAG("residentEntityMembers", live);
    OPTICK_TAG("residentEntityRefreshed", refreshed);
    OPTICK_TAG("residentEntityReused", reused);
    OPTICK_TAG("residentEntityRemoved", removed);
    OPTICK_TAG("residentEntityCold", cold);
    OPTICK_TAG("residentUpdateAreas", static_cast<uint32_t>(queue.size()));
    return true;
}

bool CaptureRetainedEntities(viewDef_t* parms)
{
    OPTICK_EVENT("PT CPU Resident Entity Seal");
    uint32_t surfaces = 0;
    auto* world = parms->renderWorld;
    for (size_t i = 0; i < residentEntities.size(); ++i)
    {
        auto& cached = residentEntities[i];
        if (!cached.live) continue;
        auto* def = world->entityDefs[static_cast<int>(i)];
        if (!def || PtGeometryLifecycle::EntityGeneration(world, static_cast<int>(i)) != cached.generation) return false;
        if (cached.refresh)
        {
            // A refreshed model that produced no admitted surfaces must retire its
            // old rows; never resurrect an invisible/deleted model from the cache.
            if (cached.capturedFrame != parms->pathTraceRewriteRootFrame) cached.surfaces.clear();
            cached.initialized = true;
            surfaces += static_cast<uint32_t>(cached.surfaces.size());
            continue; // model jobs already wrote current manifests and carriers
        }
        auto* space = (viewEntity_t*)R_ClearedFrameAlloc(sizeof(viewEntity_t), FRAME_ALLOC_VIEW_ENTITY);
        space->pathTraceMaterialSnapshot = true;
        space->pathTraceRenderDefIndex = static_cast<int>(i); space->pathTraceEntityNum = def->parms.entityNum;
        std::memcpy(space->modelMatrix, def->modelMatrix, sizeof(space->modelMatrix));
        for (const auto& row : cached.surfaces)
        {
            RtCpuRewriteSurfaceWrite write = row.write;
            std::memcpy(write.modelMatrix, def->modelMatrix, sizeof(write.modelMatrix));
            std::copy(def->globalReferenceBounds.ToFloatPtr(), def->globalReferenceBounds.ToFloatPtr() + 6, write.bounds);
            write.joints = row.joints.empty() ? nullptr : row.joints.data();
            write.jointCount = static_cast<uint32_t>(row.joints.size());
            if (!g_service->WriteSurface(write)) return false;
            auto* ds = (drawSurf_t*)R_ClearedFrameAlloc(sizeof(drawSurf_t), FRAME_ALLOC_DRAW_SURFACE);
            ds->space = space; ds->modelSurfaceIndex = static_cast<int>(write.surfaceOrdinal);
            ds->numIndexes = static_cast<int>(write.indexCount);
            idVec3 origin; R_LocalPointToGlobal(def->modelMatrix, row.localOrigin, origin);
            for (int c = 0; c < 3; ++c) ds->pathTraceSurfaceOrigin[c] = origin[c];
            R_SetupDrawSurfShader(ds, row.material, &def->parms);
            ds->nextOnLight = parms->pathTraceRewriteSurfaces; parms->pathTraceRewriteSurfaces = ds;
            ++parms->pathTraceRewriteSurfaceCount; ++surfaces;
        }
    }
    OPTICK_TAG("residentEntitySurfaces", surfaces);
    return true;
}

struct ResidentWorldSurface
{
    int ordinal = 0, indexes = 0;
    const idMaterial* material = nullptr;
    idVec3 origin;
};
struct ResidentWorldEntity
{
    int index = 0, modified = 0;
    uint32_t generation = 0;
    idRenderModel* model = nullptr; // frontend-only, checked before use
    std::vector<ResidentWorldSurface> surfaces;
};
// Only the frontend reads these renderer pointers. Lifecycle checks precede every
// access; frame carriers copy identity/origin/registers and clear geometry pointers.
std::vector<ResidentWorldEntity> residentWorldEntities;
uint64_t residentFrontendLifecycle = 0, residentFrontendWorld = 0, residentFrontendMap = 0;
uint64_t residentFrontendRevision = 0;
const idMaterial* residentGlobalMaterial = nullptr;

void ClearFrontendResidents()
{
    // Called at the existing renderer lifecycle drain boundaries, after writers
    // and workers have joined and before native entities/materials are freed.
    residentEntities.clear(); residentWorldEntities.clear();
    residentEntityLifecycle = residentFrontendLifecycle = 0;
}

bool CaptureResidentWorld(viewDef_t* parms)
{
    OPTICK_EVENT("PT CPU Resident Static Materials");
    auto* world = parms->renderWorld;
    if (!world) return false;
    const auto lifecycle = g_service->LifecycleGeneration();
    bool discover = residentFrontendLifecycle != lifecycle ||
        residentFrontendWorld != world->pathTraceWorldLifecycleGeneration || residentFrontendMap != world->mapLoadSerial ||
        residentGlobalMaterial != tr.primaryRenderView.globalMaterial;
    if (!discover) for (const auto& cached : residentWorldEntities)
    {
        auto* def = cached.index < world->entityDefs.Num() ? world->entityDefs[cached.index] : nullptr;
        if (!def || def->parms.hModel != cached.model || def->lastModifiedFrameNum != cached.modified ||
            PtGeometryLifecycle::EntityGeneration(world, cached.index) != cached.generation)
        { discover = true; break; }
    }
    if (discover)
    {
        OPTICK_EVENT("PT CPU Resident Static Discover");
        std::vector<ResidentWorldEntity> entities;
        uint32_t count = 0;
        for (int i = 0; i < world->entityDefs.Num(); ++i)
        {
            auto* def = world->entityDefs[i];
            auto* model = def ? def->parms.hModel : nullptr;
            if (!model || !model->IsStaticWorldModel()) continue;
            // Area models are immutable map data. Unsupported dynamic callbacks
            // remain outside this family and are never invoked by discovery.
            if (def->parms.callback || def->parms.forceUpdate || def->parms.weaponDepthHack || def->parms.modelDepthHack != 0) continue;
            ResidentWorldEntity entity;
            entity.index = i; entity.model = model; entity.modified = def->lastModifiedFrameNum;
            entity.generation = PtGeometryLifecycle::EntityGeneration(world, i);
            for (int ordinal = 0; ordinal < model->NumSurfaces(); ++ordinal)
            {
                const auto* surface = model->Surface(ordinal);
                auto* tri = surface ? surface->geometry : nullptr;
                const auto* shader = surface ? surface->shader : nullptr;
                if (!tri || !tri->numIndexes || !shader || shader->Deform() != DFRM_NONE) continue;
                if (!shader->IsDrawn() && !shader->SurfaceCastsShadow()) continue;
                if (def->parms.customShader) shader = def->parms.customShader;
                else if (def->parms.customSkin) shader = def->parms.customSkin->RemapShaderBySkin(shader);
                if (tr.primaryRenderView.globalMaterial) shader = tr.primaryRenderView.globalMaterial;
                if (!shader || !shader->IsDrawn() || shader->Deform() != DFRM_NONE || RewriteMaterialSuppressed(shader)) continue;
                if (++count > 65535u) return false;
                ResidentWorldSurface row;
                row.ordinal = ordinal; row.indexes = tri->numIndexes; row.material = shader;
                R_LocalPointToGlobal(def->modelMatrix, tri->bounds.GetCenter(), row.origin);
                entity.surfaces.push_back(row);
            }
            entities.push_back(std::move(entity));
        }
        residentWorldEntities = std::move(entities);
        residentFrontendLifecycle = lifecycle;
        residentFrontendWorld = world->pathTraceWorldLifecycleGeneration;
        residentFrontendMap = world->mapLoadSerial;
        residentGlobalMaterial = tr.primaryRenderView.globalMaterial;
        ++residentFrontendRevision;
    }
    bool populate = false;
    if (!g_service->PrepareResidentStaticInput(residentFrontendRevision, populate)) return false;
    uint32_t surfaces = 0;
    for (const auto& cached : residentWorldEntities)
    {
        auto* def = world->entityDefs[cached.index];
        auto* space = (viewEntity_t*)R_ClearedFrameAlloc(sizeof(viewEntity_t), FRAME_ALLOC_VIEW_ENTITY);
        space->pathTraceMaterialSnapshot = true;
        space->pathTraceRenderDefIndex = cached.index;
        space->pathTraceEntityNum = def->parms.entityNum;
        std::memcpy(space->modelMatrix, def->modelMatrix, sizeof(space->modelMatrix));
        viewEntity_t capture = {};
        capture.pathTraceResident = true; // explicit cold population uses the admitted capture hook
        capture.entityDef = def;
        std::memcpy(capture.modelMatrix, def->modelMatrix, sizeof(capture.modelMatrix));
        for (const auto& row : cached.surfaces)
        {
            auto* ds = (drawSurf_t*)R_ClearedFrameAlloc(sizeof(drawSurf_t), FRAME_ALLOC_DRAW_SURFACE);
            ds->space = space; ds->modelSurfaceIndex = row.ordinal; ds->numIndexes = row.indexes;
            for (int c = 0; c < 3; ++c) ds->pathTraceSurfaceOrigin[c] = row.origin[c];
            R_SetupDrawSurfShader(ds, row.material, &def->parms);
            if (populate)
            {
                auto* tri = cached.model->Surface(row.ordinal)->geometry;
                if (row.material->ReceivesLighting() && !tri->tangentsCalculated) R_DeriveTangents(tri);
                ds->frontEndGeo = tri;
            }
            ds->nextOnLight = capture.pathTraceDrawSurfs;
            capture.pathTraceDrawSurfs = ds;
            ++surfaces;
        }
        if (populate)
        {
            OPTICK_EVENT("PT CPU Resident Static Capture");
            RtCpuProducerRewrite_CaptureAdmittedModel(&capture, cached.model);
        }
        for (auto* ds = capture.pathTraceDrawSurfs; ds; )
        {
            auto* next = ds->nextOnLight;
            ds->frontEndGeo = nullptr;
            ds->nextOnLight = parms->pathTraceRewriteSurfaces;
            parms->pathTraceRewriteSurfaces = ds;
            ++parms->pathTraceRewriteSurfaceCount;
            ds = next;
        }
    }
    OPTICK_TAG("residentStaticCapture", populate ? 1u : 0u);
    OPTICK_TAG("residentStaticMaterialSurfaces", surfaces);
    OPTICK_TAG("residentStaticRevision", residentFrontendRevision);
    return true;
}
}

void RtCpuProducerRewrite_OnRootViewBegin(viewDef_t* parms)
{
	if (!parms || parms->isSubview || !g_service)
	{
		return;
	}
	parms->pathTraceRewriteRootFrame = static_cast<std::uint64_t>(tr.frameCount);
	if (g_service->Route() == RtCpuProducerRewriteRoute::LegacyOnly ||
		g_service->Route() == RtCpuProducerRewriteRoute::DrainingToLegacy)
	{
		return;
	}
	if (!r_useParallelAddModels.GetBool())
	{
		return;
	}
	const std::uint64_t worldGen = parms->renderWorld ? parms->renderWorld->pathTraceWorldLifecycleGeneration : 0;
	const std::uint64_t mapGen = parms->renderWorld ? parms->renderWorld->mapLoadSerial : 0;

    if (!g_service->TryAcquireRootInput(tr.frameCount, worldGen, mapGen)) return;
    try
    {
        if (!PrepareResidentEntities(parms)) g_service->MarkCapturingInvalid(RtCpuRewriteInvalidReason::Malformed);
    }
    catch (const std::bad_alloc&) { g_service->MarkCapturingInvalid(RtCpuRewriteInvalidReason::Capacity); }

}

void RtCpuProducerRewrite_OnRootViewSealOrAbort(viewDef_t* parms)
{
	if (!g_service)
	{
		return;
	}
	if (parms && parms->isSubview)
	{
		return;
	}
	if (!parms)
	{
		residentEntities.clear();
                    g_service->AbortCapturingInput();
		return;
	}
	if (g_service->HasCapturingSlot())
	{
		if (parms)
		{
			parms->pathTraceRewriteRootFrame = static_cast<std::uint64_t>(tr.frameCount);
            PathTraceDoomAnalyticLightSnapshot lights;
            bool captured = false;
            try
            {
                if (!CaptureRetainedEntities(parms) || !CaptureResidentWorld(parms))
                {
                    g_service->MarkCapturingInvalid(RtCpuRewriteInvalidReason::Capacity);
                    residentEntities.clear();
                    g_service->AbortCapturingInput();
                    return;
                }
                OPTICK_EVENT("PT CPU Lights Snapshot");
                const bool unified = NormalizePathTraceDebugMode(idMath::ClampInt(0, 58,
                    r_pathTracingDebugMode.GetInteger())) == 0 && r_pathTracingUnifiedPtEnable.GetInteger() != 0;
                captured = CapturePathTraceDoomAnalyticLightSnapshot(parms,
                    BuildCurrentDoomAnalyticLightOptions(unified), lights, true);
                OPTICK_TAG("lightSnapshotCaptured", captured ? 1u : 0u);
                OPTICK_TAG("viewCaptureSurfaces", parms->pathTraceRewriteSurfaceCount);
            }
            catch (const std::bad_alloc&) { captured = false; }
            if (!captured) { residentEntities.clear();
                    g_service->AbortCapturingInput(); return; }
            if (!g_service->SealOverlayFromCapturingInput(parms->pathTraceRewriteRootFrame, std::move(lights.data)))
            {
                residentEntities.clear();
                g_service->AbortCapturingInput();
                return;
            }
		}
		if (!g_service->SealRootInput()) residentEntities.clear();
	}

}

void RtCpuProducerRewrite_CaptureAdmittedModel(viewEntity_t* vEntity, idRenderModel* resolvedSurfaceModel)
{
	if (!g_service || !g_service->HasCapturingSlot() || !vEntity || !vEntity->entityDef || !vEntity->pathTraceResident)
	{
		return;
	}
	if (tr.viewDef && tr.viewDef->isSubview)
	{
		return;
	}
	idRenderEntityLocal* entityDef = vEntity->entityDef;
	const renderEntity_t* renderEntity = &entityDef->parms;
	idRenderModel* sourceModel = renderEntity->hModel;
	idRenderModel* model = resolvedSurfaceModel;
	PtSourceDomainFacts sourceFacts;
	sourceFacts.sourcePresent = sourceModel != nullptr;
	sourceFacts.staticWorld = sourceModel && sourceModel->IsStaticWorldModel();
	sourceFacts.continuous = sourceModel && sourceModel->IsDynamicModel() == DM_CONTINUOUS;
	sourceFacts.cached = sourceModel && sourceModel->IsDynamicModel() == DM_CACHED;
	sourceFacts.entityJointed = renderEntity->joints != nullptr || renderEntity->numJoints > 0;
	const PtCanonicalMeshSourceDomain domain = PtClassifySourceDomain(sourceFacts);
	RtCpuRewriteCaptureAdmissionFacts entityAdmission;
	entityAdmission.sourceDomain = domain;
	entityAdmission.callback = renderEntity->callback != nullptr;
	entityAdmission.forceUpdate = renderEntity->forceUpdate;
	entityAdmission.weaponDepthHack = renderEntity->weaponDepthHack;
	entityAdmission.modelDepthHack = renderEntity->modelDepthHack != 0.0f;
	entityAdmission.rootView = tr.viewDef != nullptr && !tr.viewDef->isSubview;
	entityAdmission.allowSurfaceInViewId = renderEntity->allowSurfaceInViewID;
	entityAdmission.activeViewId = tr.viewDef != nullptr ? tr.viewDef->renderView.viewID : 0;
	entityAdmission.resolvedSurfaceCurrent =
		domain == PtCanonicalMeshSourceDomain::SkinnedBindSource &&
		model != nullptr && entityDef->dynamicModel != nullptr && model == entityDef->dynamicModel;
	entityAdmission.gpuSkinningEnabled = r_useGPUSkinning.GetBool();
	const RtCpuRewriteCaptureAdmissionReason entityReason = RtCpuRewritePlanCaptureAdmission(entityAdmission);
	g_service->NoteCaptureEntityDecision(domain, entityReason);
	if (entityReason != RtCpuRewriteCaptureAdmissionReason::Admitted &&
		entityReason != RtCpuRewriteCaptureAdmissionReason::CpuPosedSurface &&
		entityReason != RtCpuRewriteCaptureAdmissionReason::MissingJointSnapshot)
	{
		return;
	}
	if (!sourceModel || !model)
	{
		return;
	}
	const bool worldModel = domain == PtCanonicalMeshSourceDomain::StaticWorldMap;
    ResidentEntity* resident = !worldModel && entityDef->index >= 0 &&
        static_cast<size_t>(entityDef->index) < residentEntities.size() && residentEntities[entityDef->index].live
        ? &residentEntities[entityDef->index] : nullptr;
    std::vector<ResidentEntitySurface> previous;
    if (resident)
    {
        previous.swap(resident->surfaces);
        resident->capturedFrame = tr.viewDef->pathTraceRewriteRootFrame;
    }

	PtGeometryLifecycle::PtFrontendCanonicalAuthority authority = {};
	if (!PtGeometryLifecycle::ResolveFrontendCanonicalAuthority(
			entityDef->world, entityDef->index, sourceModel, domain, authority) ||
		authority.sourceAssetId == 0 || authority.sourceAssetGeneration == 0 ||
		authority.worldGeneration == 0 || authority.renderDefGeneration == 0)
	{
		g_service->MarkCapturingInvalid(RtCpuRewriteInvalidReason::Malformed);
		return;
	}

	for (drawSurf_t* ds = vEntity->pathTraceDrawSurfs; ds != nullptr; ds = ds->nextOnLight)
	{
        if ((!worldModel && entityDef->index == rewriteSuppressedEntityIndex) ||
            RewriteMaterialSuppressed(ds->material)) continue;
		if (ds->linkChain != nullptr)
		{
			continue;
		}
		if (ds->modelSurfaceIndex < 0 || ds->modelSurfaceIndex >= model->NumSurfaces())
		{
			continue;
		}
		const modelSurface_t* surf = model->Surface(ds->modelSurfaceIndex);
		if (!surf || surf->geometry != ds->frontEndGeo)
		{
			continue;
		}
		if (surf->shader && surf->shader->Deform() != DFRM_NONE)
		{
			continue;
		}
		srfTriangles_t* tri = surf->geometry;
		if (!tri)
		{
			continue;
		}
		if (!tri->verts || !tri->indexes || tri->numVerts <= 0 || tri->numIndexes < 3 || (tri->numIndexes % 3) != 0)
		{
			g_service->MarkCapturingInvalid(RtCpuRewriteInvalidReason::Malformed);
			return;
		}
		const bool skinnedModel = domain == PtCanonicalMeshSourceDomain::SkinnedBindSource;
		const idJointMat* skinJoints = nullptr;
		int skinJointCount = 0;
		if (skinnedModel)
		{
			SmokeRtSkinningJointSnapshot snapshot;
			RtCpuRewriteCaptureAdmissionFacts surfaceAdmission = entityAdmission;
			surfaceAdmission.bindPoseSurface = tri->staticModelWithJoints != nullptr;
			surfaceAdmission.jointSnapshotValid = GetSmokeRtCpuSkinningJointSnapshot(tri, snapshot);
			const RtCpuRewriteCaptureAdmissionReason reason = RtCpuRewritePlanCaptureAdmission(surfaceAdmission);
			if (reason != RtCpuRewriteCaptureAdmissionReason::Admitted)
			{
				g_service->NoteCaptureSkinnedSurfaceDecision(reason);
				continue;
			}
			skinJoints = snapshot.jointsInverted;
			skinJointCount = snapshot.numInvertedJoints;
			g_service->NoteCaptureSkinnedSurfaceDecision(RtCpuRewriteCaptureAdmissionReason::Admitted,
				static_cast<std::uint32_t>(skinJointCount));
		}
        const ResidentEntitySurface* retained = nullptr;
        if (resident) for (const auto& row : previous)
        {
            if (row.write.surfaceOrdinal == static_cast<uint32_t>(ds->modelSurfaceIndex) &&
                row.write.meshKey.sourceAssetId == authority.sourceAssetId &&
                row.write.meshKey.sourceAssetGeneration == authority.sourceAssetGeneration &&
                row.write.vertexCount == static_cast<uint32_t>(tri->numVerts) && row.write.indexCount == static_cast<uint32_t>(tri->numIndexes) &&
                row.vertexIdentity == reinterpret_cast<uintptr_t>(tri->verts) && row.indexIdentity == reinterpret_cast<uintptr_t>(tri->indexes))
            { retained = &row; break; }
        }
        uint64_t topo = retained ? retained->write.meshKey.topologySignature : kFnvOffset;
        if (!retained)
        {
		topo = HashBytes(topo, &tri->numVerts, sizeof(tri->numVerts));
		topo = HashBytes(topo, &tri->numIndexes, sizeof(tri->numIndexes));
		for (int i = 0; i < tri->numIndexes; ++i)
		{
			const std::uint32_t idx = static_cast<std::uint32_t>(tri->indexes[i]);
			topo = HashBytes(topo, &idx, sizeof(idx));
		}
        }

		RtCpuRewriteSurfaceWrite write = {};
		write.meshKey.sourceAssetId = authority.sourceAssetId;
		write.meshKey.sourceAssetGeneration = authority.sourceAssetGeneration;
		write.meshKey.topologySignature = topo;
		write.meshKey.sourceDomain = domain;
		write.meshKey.modelSurfaceIndex = static_cast<std::uint32_t>(ds->modelSurfaceIndex);
		write.meshKey.vertexFormat = 1;
		write.meshKey.deformationClass = worldModel ? PtCanonicalDeformationClass::Static : (skinnedModel ? PtCanonicalDeformationClass::Skinned : PtCanonicalDeformationClass::Rigid);
		write.meshKey.vertexCount = static_cast<std::uint32_t>(tri->numVerts);
		write.meshKey.indexCount = static_cast<std::uint32_t>(tri->numIndexes);
		write.meshKey.jointSubmeshIndex = skinnedModel ? static_cast<std::int32_t>(ds->modelSurfaceIndex) : -1;
		write.instanceKey.worldGeneration = authority.worldGeneration;
		write.instanceKey.renderDefIndex = static_cast<std::uint32_t>(entityDef->index);
		write.instanceKey.renderDefGeneration = authority.renderDefGeneration;
		write.instanceKey.subInstanceKind = worldModel ? PtCanonicalSubInstanceKind::StaticSurface : (skinnedModel ? PtCanonicalSubInstanceKind::SkinnedSurface : PtCanonicalSubInstanceKind::RigidSurface);
		write.instanceKey.modelSurfaceIndex = static_cast<std::uint32_t>(ds->modelSurfaceIndex);
		write.instanceKey.jointSubmeshIndex = skinnedModel ? static_cast<std::int32_t>(ds->modelSurfaceIndex) : -1;
		write.sourceClass = worldModel ? kRtCpuRewriteClassStaticWorld : (skinnedModel ? kRtCpuRewriteClassSkinnedEntity : kRtCpuRewriteClassRigidEntity);
		std::memcpy(write.modelMatrix, vEntity->modelMatrix, sizeof(write.modelMatrix));
		const idBounds& bounds = entityDef->globalReferenceBounds;
		write.bounds[0] = bounds[0].x;
		write.bounds[1] = bounds[0].y;
		write.bounds[2] = bounds[0].z;
		write.bounds[3] = bounds[1].x;
		write.bounds[4] = bounds[1].y;
		write.bounds[5] = bounds[1].z;
		write.materialLogicalId = ds->material ? HashPathTraceMaterialName(ds->material->GetName()) : 0;
		write.activeEmissiveStage = 0;
		write.surfaceOrdinal = static_cast<std::uint32_t>(ds->modelSurfaceIndex);
		write.vertexCount = static_cast<std::uint32_t>(tri->numVerts);
		write.indexCount = static_cast<std::uint32_t>(tri->numIndexes);
		write.nativeDrawVerts = tri->verts;
		write.nativeIndexes = tri->indexes;
		write.nativeIndexStride = static_cast<std::uint32_t>(sizeof(triIndex_t));
		std::vector<PathTraceSkinnedJointMatrix> jointRows;
		if (skinnedModel)
		{
			jointRows.resize(static_cast<std::size_t>(skinJointCount));
			for (int j = 0; j < skinJointCount; ++j)
			{
				const float* rows = skinJoints[j].ToFloatPtr();
				for (int k = 0; k < 12; ++k)
				{
					jointRows[static_cast<std::size_t>(j)].rows[k] = rows[k];
				}
			}
			write.joints = jointRows.data();
			write.jointCount = static_cast<std::uint32_t>(skinJointCount);
		}
        try
        {
            if (resident)
            {
                write.geometry = retained ? retained->write.geometry : g_service->RetainGeometry(write);
                if (!write.geometry) { g_service->MarkCapturingInvalid(RtCpuRewriteInvalidReason::Capacity); return; }
            }
            if (!g_service->WriteSurface(write)) return;
            if (resident)
            {
                ResidentEntitySurface row;
                row.write = write;
                row.write.nativeDrawVerts = row.write.nativeIndexes = nullptr;
                row.write.vertices = nullptr; row.write.indexes = nullptr; row.write.joints = nullptr;
                row.joints = std::move(jointRows); row.material = ds->material;
                row.localOrigin = tri->bounds.GetCenter();
                row.vertexIdentity = reinterpret_cast<uintptr_t>(tri->verts);
                row.indexIdentity = reinterpret_cast<uintptr_t>(tri->indexes);
                resident->surfaces.push_back(std::move(row));
            }
        }
        catch (const std::bad_alloc&) { g_service->MarkCapturingInvalid(RtCpuRewriteInvalidReason::Capacity); return; }

	}
}

#endif
