#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCommittedCapture.h"
#include "PathTraceCommittedBaseline.h"
#include "PathTraceCaptureProduct.h"

#include "PathTraceCVars.h"
#include "PathTraceGeometry.h"
#include "PathTraceSkinning.h"
#include "../RenderCommon.h"
#include "../Model_local.h"

#include <limits>
#include <type_traits>

namespace {

constexpr std::uint32_t RT_PT_COMMITTED_CAPTURE_MAX_VIEWS = 32;
constexpr std::size_t RT_PT_COMMITTED_CAPTURE_MAX_BYTES =
    16u * 1024u * 1024u;

struct RtPathTraceCommittedGeometryInput
{
    std::uint32_t ordinal = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t vertexOffset = 0;
    std::uint32_t indexOffset = 0;
    const idDrawVert* vertices = nullptr;
    const triIndex_t* indexes = nullptr;
    const idJointMat* joints = nullptr;
    std::uint32_t jointCount = 0;
    float modelMatrix[16] = {};
    float bumpMatrix[6] = {};
    std::uint64_t ambientHandle = 0;
    std::uint64_t indexHandle = 0;
    std::uint64_t jointHandle = 0;
};

static_assert(std::is_trivially_copyable<
    RtPathTraceCommittedGeometryInput>::value,
    "committed capture worker input must remain scalar/owned-pointer POD");

struct RtPathTraceCommittedGeometryJob
{
    const RtPathTraceCommittedGeometryInput* inputs = nullptr;
    std::uint32_t inputCount = 0;
    RtPathTraceCommittedGeometryProduct* product = nullptr;
    std::uint64_t cpuUs = 0;
};

struct RtPathTraceCommittedCaptureAssociation
{
    viewDef_t* viewDef = nullptr;
    RtPathTraceCommittedGeometryProduct* product = nullptr;
    RtPathTraceCommittedGeometryJob* job = nullptr;
};

struct RtPathTraceCommittedCaptureBatch
{
    RtPathTraceCommittedCaptureBatchContract contract;
    RtPathTraceCommittedCaptureAssociation views[
        RT_PT_COMMITTED_CAPTURE_MAX_VIEWS];
    bool enabled = false;
    int frameNumber = -1;
    viewDef_t* telemetryHost = nullptr;
    std::uint32_t observedViews = 0;
    std::uint32_t fallbackInvalidView = 0;
    std::uint32_t fallbackListUnavailable = 0;
    std::uint32_t fallbackViewCapacity = 0;
    std::uint32_t fallbackPreflight = 0;
    std::uint32_t fallbackBudget = 0;
    std::uint32_t fallbackCopy = 0;
    std::uint32_t fallbackNotAccepting = 0;
    std::uint32_t fallbackIncomplete = 0;
    std::uint64_t semanticMarshalUs = 0;
    std::uint64_t semanticOwnedBytes = 0;
};

struct RtPathTraceCommittedCaptureLane
{
    idParallelJobList* list = nullptr;
    bool handedOff = false;
    std::uint64_t lastCaptureToken = 0;
    std::size_t ownedBytesHighWater = 0;
    std::uint32_t allocations = 0;
    std::uint32_t reuses = 0;
};

RtPathTraceCommittedCaptureBatch g_committedCaptureBatch;
RtPathTraceCommittedCaptureLane g_committedCaptureLane;

#if defined(_MSC_VER) && defined(_WIN32)
__declspec(noinline)
#endif
bool TryCopyCommittedCaptureMemory(
    void* destination,
    const void* source,
    std::size_t byteCount)
{
    if (byteCount == 0)
    {
        return true;
    }
    if (!destination || !source)
    {
        return false;
    }
#if defined(_MSC_VER) && defined(_WIN32)
    __try
    {
        memcpy(destination, source, byteCount);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
#else
    memcpy(destination, source, byteCount);
    return true;
#endif
}

template <typename T>
T* CopyCommittedSemanticArray(const std::vector<T>& source)
{
    if (source.empty())
    {
        return nullptr;
    }
    T* destination = static_cast<T*>(R_FrameAlloc(
        static_cast<int>(source.size() * sizeof(T)), FRAME_ALLOC_DRAW_SURFACE));
    return TryCopyCommittedCaptureMemory(destination, source.data(),
        source.size() * sizeof(T)) ? destination : nullptr;
}

#if defined(_MSC_VER) && defined(_WIN32)
__declspec(noinline)
#endif
bool TryReadCommittedJointSource(
    const idRenderModelStatic* model,
    const idJointMat* expectedJoints,
    int& jointCount)
{
#if defined(_MSC_VER) && defined(_WIN32)
    __try
    {
        if (!model || model->jointsInverted != expectedJoints)
        {
            return false;
        }
        jointCount = model->numInvertedJoints;
        return jointCount > 0 && jointCount <= 4096;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        jointCount = 0;
        return false;
    }
#else
    if (!model || model->jointsInverted != expectedJoints)
    {
        return false;
    }
    jointCount = model->numInvertedJoints;
    return jointCount > 0 && jointCount <= 4096;
#endif
}

bool AddCommittedCaptureBytes(
    std::size_t count,
    std::size_t elementSize,
    std::size_t& total)
{
    if (elementSize != 0 && count >
        (RT_PT_COMMITTED_CAPTURE_MAX_BYTES - total) / elementSize)
    {
        return false;
    }
    total += count * elementSize;
    return total <= RT_PT_COMMITTED_CAPTURE_MAX_BYTES &&
        total <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}

void SetIdentityMatrix(float matrix[16])
{
    memset(matrix, 0, sizeof(float) * 16);
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}

void CaptureCommittedBumpMatrix(
    const drawSurf_t* drawSurf,
    float matrix[6])
{
    matrix[0] = 1.0f;
    matrix[1] = 0.0f;
    matrix[2] = 0.0f;
    matrix[3] = 0.0f;
    matrix[4] = 1.0f;
    matrix[5] = 0.0f;
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    const float* registers = drawSurf && drawSurf->shaderRegisters
        ? drawSurf->shaderRegisters
        : (material ? material->ConstantRegisters() : nullptr);
    if (!material || !registers)
    {
        return;
    }
    const int registerCount = material->GetNumRegisters();
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || stage->lighting != SL_BUMP || !stage->texture.hasMatrix)
        {
            continue;
        }
        for (int row = 0; row < 2; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                const int registerIndex = stage->texture.matrix[row][column];
                if (registerIndex >= 0 && registerIndex < registerCount)
                {
                    matrix[row * 3 + column] = registers[registerIndex];
                }
            }
        }
        return;
    }
}

bool ResolveCommittedJointSource(
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    const idJointMat*& joints,
    int& jointCount)
{
    joints = nullptr;
    jointCount = 0;
    if (drawSurf && drawSurf->jointCacheCpuSnapshot &&
        drawSurf->jointCacheCpuSnapshotCount > 0 &&
        drawSurf->jointCacheCpuSnapshotCount <= 4096)
    {
        joints = drawSurf->jointCacheCpuSnapshot;
        jointCount = drawSurf->jointCacheCpuSnapshotCount;
        return true;
    }
    joints = GetSmokeRtCpuSkinningJoints(tri);
    if (!joints)
    {
        return true;
    }
    // GetSmokeRtCpuSkinningJoints already validates this exact model-owned
    // source and its 1..4096 count. The frontend is its owner at this seal.
    const idRenderModelStatic* model = tri ? tri->staticModelWithJoints : nullptr;
    if (!TryReadCommittedJointSource(model, joints, jointCount))
    {
        return false;
    }
    return true;
}

void TransformCommittedPoint(
    const float matrix[16],
    const idVec3& input,
    idVec3& output)
{
    output[0] = input[0] * matrix[0] + input[1] * matrix[4] +
        input[2] * matrix[8] + matrix[12];
    output[1] = input[0] * matrix[1] + input[1] * matrix[5] +
        input[2] * matrix[9] + matrix[13];
    output[2] = input[0] * matrix[2] + input[1] * matrix[6] +
        input[2] * matrix[10] + matrix[14];
}

void TransformCommittedVector(
    const float matrix[16],
    const idVec3& input,
    idVec3& output)
{
    output[0] = input[0] * matrix[0] + input[1] * matrix[4] +
        input[2] * matrix[8];
    output[1] = input[0] * matrix[1] + input[1] * matrix[5] +
        input[2] * matrix[9];
    output[2] = input[0] * matrix[2] + input[1] * matrix[6] +
        input[2] * matrix[10];
}

PathTraceSmokeVertex BuildCommittedVertex(
    const RtPathTraceCommittedGeometryInput& input,
    std::uint32_t vertexIndex)
{
    RtPathTraceCommittedVertexInput vertexInput;
    vertexInput.vertices = input.vertices;
    vertexInput.joints = input.joints;
    memcpy(vertexInput.modelMatrix, input.modelMatrix, sizeof(vertexInput.modelMatrix));
    memcpy(vertexInput.bumpMatrix, input.bumpMatrix, sizeof(vertexInput.bumpMatrix));
    return BuildPathTraceCommittedVertexFromOwned(vertexInput, vertexIndex);
}

} // namespace

PathTraceSmokeVertex BuildPathTraceCommittedVertexFromOwned(
    const RtPathTraceCommittedVertexInput& input,
    std::uint32_t vertexIndex)
{
    const idDrawVert& drawVert = input.vertices[vertexIndex];
    idVec3 localPosition = drawVert.xyz;
    idVec3 localNormal = drawVert.GetNormal();
    idVec3 localTangent = drawVert.GetTangent();
    idVec3 localBitangent = drawVert.GetBiTangent();
    const float bitangentSign = drawVert.GetBiTangentSign();
    if (input.joints)
    {
        localPosition = TransformSmokeSkinnedVertexPosition(drawVert, input.joints);
        localNormal = TransformSmokeSkinnedVertexNormal(drawVert, input.joints);
        localTangent = TransformSmokeSkinnedVertexTangent(drawVert, input.joints);
        localBitangent = TransformSmokeSkinnedVertexBitangent(drawVert, input.joints);
    }

    idVec3 worldPosition;
    idVec3 worldNormal;
    idVec3 worldTangent;
    idVec3 worldBitangent;
    TransformCommittedPoint(input.modelMatrix, localPosition, worldPosition);
    TransformCommittedVector(input.modelMatrix, localNormal, worldNormal);
    TransformCommittedVector(input.modelMatrix, localTangent, worldTangent);
    TransformCommittedVector(input.modelMatrix, localBitangent, worldBitangent);
    worldNormal.Normalize();
    if (worldTangent.Normalize() == 0.0f)
    {
        worldTangent.Set(1.0f, 0.0f, 0.0f);
    }
    if (worldBitangent.Normalize() == 0.0f)
    {
        worldBitangent.Cross(worldNormal, worldTangent);
        worldBitangent *= bitangentSign;
        worldBitangent.Normalize();
    }

    const idVec2 texCoord = drawVert.GetTexCoord();
    const idVec2 normalTexCoord(
        input.bumpMatrix[0] * texCoord.x +
            input.bumpMatrix[1] * texCoord.y + input.bumpMatrix[2],
        input.bumpMatrix[3] * texCoord.x +
            input.bumpMatrix[4] * texCoord.y + input.bumpMatrix[5]);
    PathTraceSmokeVertex vertex = {};
    vertex.position[0] = worldPosition.x;
    vertex.position[1] = worldPosition.y;
    vertex.position[2] = worldPosition.z;
    vertex.position[3] = 1.0f;
    vertex.normal[0] = worldNormal.x;
    vertex.normal[1] = worldNormal.y;
    vertex.normal[2] = worldNormal.z;
    vertex.texCoord[0] = texCoord.x;
    vertex.texCoord[1] = texCoord.y;
    vertex.texCoord[2] = normalTexCoord.x;
    vertex.texCoord[3] = normalTexCoord.y;
    for (int component = 0; component < 4; ++component)
    {
        vertex.color[component] = drawVert.color[component] * (1.0f / 255.0f);
        vertex.color2[component] = drawVert.color2[component] * (1.0f / 255.0f);
    }
    vertex.tangent[0] = worldTangent.x;
    vertex.tangent[1] = worldTangent.y;
    vertex.tangent[2] = worldTangent.z;
    vertex.tangent[3] = bitangentSign;
    vertex.bitangent[0] = worldBitangent.x;
    vertex.bitangent[1] = worldBitangent.y;
    vertex.bitangent[2] = worldBitangent.z;
    return vertex;
}

namespace {

void RunPathTraceCommittedGeometryJob(void* data)
{
    OPTICK_EVENT("PT Backend Job Committed Dynamic Geometry");
    RtPathTraceCommittedGeometryJob* job =
        static_cast<RtPathTraceCommittedGeometryJob*>(data);
    if (!job || !job->product ||
        (job->inputCount > 0 && !job->inputs))
    {
        return;
    }
    const std::uint64_t startUs = Sys_Microseconds();
    RtPathTraceCommittedGeometryProduct& product = *job->product;
    for (std::uint32_t surfaceIndex = 0;
         surfaceIndex < job->inputCount; ++surfaceIndex)
    {
        const RtPathTraceCommittedGeometryInput& input = job->inputs[surfaceIndex];
        RtPathTraceCommittedGeometrySurface& output = product.surfaces[surfaceIndex];
        output.ordinal = input.ordinal;
        output.source = RtPathTraceCommittedGeometrySource::CpuTriArrays;
        output.ambientHandle = input.ambientHandle;
        output.indexHandle = input.indexHandle;
        output.jointHandle = input.jointHandle;
        output.vertexOffset = input.vertexOffset;
        output.indexOffset = input.indexOffset;

        if (input.vertexCount == 0 || input.indexCount == 0)
        {
            output.complete = true;
            continue;
        }

        PathTraceSmokeVertex* vertices = product.vertices + input.vertexOffset;
        std::uint32_t* indexes = product.indexes + input.indexOffset;
        for (std::uint32_t vertexIndex = 0;
             vertexIndex < input.vertexCount; ++vertexIndex)
        {
            vertices[vertexIndex] = BuildCommittedVertex(input, vertexIndex);
            const idVec3 normal = SmokeVertexNormal(vertices[vertexIndex]);
            const idVec2 texCoord = SmokeVertexTexCoord(vertices[vertexIndex]);
            if (!SmokeNormalIsUsable(normal))
            {
                ++output.counters.invalidNormalVerts;
                vertices[vertexIndex].normal[0] = 0.0f;
                vertices[vertexIndex].normal[1] = 0.0f;
                vertices[vertexIndex].normal[2] = 0.0f;
            }
            if (!SmokeTexCoordIsUsable(texCoord))
            {
                ++output.counters.invalidUvVerts;
                vertices[vertexIndex].texCoord[0] = 0.0f;
                vertices[vertexIndex].texCoord[1] = 0.0f;
            }
        }

        std::uint32_t emitted = 0;
        for (std::uint32_t sourceIndex = 0;
             sourceIndex + 2 < input.indexCount; sourceIndex += 3)
        {
            const int i0 = input.indexes[sourceIndex + 0];
            const int i1 = input.indexes[sourceIndex + 1];
            const int i2 = input.indexes[sourceIndex + 2];
            if (i0 < 0 || i1 < 0 || i2 < 0 ||
                i0 >= static_cast<int>(input.vertexCount) ||
                i1 >= static_cast<int>(input.vertexCount) ||
                i2 >= static_cast<int>(input.vertexCount))
            {
                ++output.counters.invalidIndexCount;
                continue;
            }
            const PathTraceSmokeVertex& v0 = vertices[i0];
            const PathTraceSmokeVertex& v1 = vertices[i1];
            const PathTraceSmokeVertex& v2 = vertices[i2];
            if (IsZeroAreaSmokeTriangle(
                    SmokeVertexPosition(v0), SmokeVertexPosition(v1),
                    SmokeVertexPosition(v2)))
            {
                continue;
            }
            indexes[emitted++] = static_cast<std::uint32_t>(i0);
            indexes[emitted++] = static_cast<std::uint32_t>(i1);
            indexes[emitted++] = static_cast<std::uint32_t>(i2);
            const bool invalidNormal =
                !SmokeNormalIsUsable(SmokeVertexNormal(v0)) ||
                !SmokeNormalIsUsable(SmokeVertexNormal(v1)) ||
                !SmokeNormalIsUsable(SmokeVertexNormal(v2));
            const bool invalidUv =
                !SmokeTexCoordIsUsable(SmokeVertexTexCoord(v0)) ||
                !SmokeTexCoordIsUsable(SmokeVertexTexCoord(v1)) ||
                !SmokeTexCoordIsUsable(SmokeVertexTexCoord(v2));
            output.counters.invalidNormalTriangles += invalidNormal ? 1 : 0;
            output.counters.invalidUvTriangles += invalidUv ? 1 : 0;
            output.counters.forcedGeometricNormalTriangles += invalidNormal ? 1 : 0;
        }
        if (emitted == 0)
        {
            output.counters.zeroAreaOnly = 1;
            output.vertexCount = 0;
            output.indexCount = 0;
        }
        else
        {
            output.vertexCount = input.vertexCount;
            output.indexCount = emitted;
        }
        output.complete = true;
    }
    product.complete = true;
    job->cpuUs = Sys_Microseconds() - startUs;
}

REGISTER_PARALLEL_JOB(
    RunPathTraceCommittedGeometryJob,
    "Path trace committed dynamic geometry shadow conversion");

bool CountersEqual(
    const RtPathTraceCommittedGeometryCounters& a,
    const RtPathTraceCommittedGeometryCounters& b)
{
    return a.invalidNormalVerts == b.invalidNormalVerts &&
        a.invalidUvVerts == b.invalidUvVerts &&
        a.invalidNormalTriangles == b.invalidNormalTriangles &&
        a.invalidUvTriangles == b.invalidUvTriangles &&
        a.forcedGeometricNormalTriangles == b.forcedGeometricNormalTriangles &&
        a.invalidIndexCount == b.invalidIndexCount &&
        a.zeroAreaOnly == b.zeroAreaOnly;
}

} // namespace

bool RtPathTracePrimarySemanticDtoShapeValid(
    const RtPathTracePrimarySemanticDto& dto,
    std::uint64_t expectedSealedPrimaryViewToken)
{
    return RtPathTracePrimarySemanticDtoShapeComplete(
        dto, expectedSealedPrimaryViewToken);
}

bool RtPathTraceCommittedGeometryProductShapeValid(
    const RtPathTraceCommittedGeometryProduct* product,
    const viewDef_t* viewDef,
    std::int32_t expectedSurfaceCount,
    std::uint64_t expectedCaptureToken)
{
    if (!product || !viewDef || !product->complete ||
        product->sealedPrimaryViewToken !=
            viewDef->pathTraceSealedPrimaryViewToken ||
        (!viewDef->isSubview && product->configFingerprint == 0) ||
        (!viewDef->isSubview &&
            !RtPathTracePrimarySemanticDtoShapeValid(product->semanticDto,
                viewDef->pathTraceSealedPrimaryViewToken)) ||
        !RtPathTraceCommittedCaptureKeyMatches(
            product->viewIdentity,
            product->surfaceCount,
            product->captureToken,
            static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(viewDef)),
            expectedSurfaceCount,
            expectedCaptureToken) ||
        expectedSurfaceCount < 0 ||
        (expectedSurfaceCount > 0 && !product->surfaces) ||
        (product->vertexCapacity > 0 && !product->vertices) ||
        (product->indexCapacity > 0 && !product->indexes))
    {
        return false;
    }
    for (std::int32_t index = 0; index < expectedSurfaceCount; ++index)
    {
        const RtPathTraceCommittedGeometrySurface& surface = product->surfaces[index];
        if (!RtPathTraceCommittedCaptureSurfaceShapeValid(
                surface,
                static_cast<std::uint32_t>(index),
                product->vertexCapacity,
                product->indexCapacity))
        {
            return false;
        }
    }
    return true;
}

void BeginPathTraceCommittedCaptureFrame()
{
    if (g_committedCaptureBatch.contract.active)
    {
        const bool began = RtPathTraceCommittedCaptureBegin(
            g_committedCaptureBatch.contract,
            g_committedCaptureBatch.contract.capacity,
            g_committedCaptureBatch.contract.captureToken);
        assert(!began);
        assert(false && "stale committed-capture batch Begin");
        return;
    }
    g_committedCaptureBatch = RtPathTraceCommittedCaptureBatch();
    g_committedCaptureBatch.enabled =
        r_pathTracing.GetInteger() != 0 &&
        r_pathTracingCommittedDynamicGeometry.GetInteger() == 1;
    if (!g_committedCaptureBatch.enabled)
    {
        return;
    }
    const std::uint64_t captureToken =
        RtPathTraceCommittedCaptureNextToken(
            g_committedCaptureLane.lastCaptureToken);
    const bool began = RtPathTraceCommittedCaptureBegin(
        g_committedCaptureBatch.contract,
        RT_PT_COMMITTED_CAPTURE_MAX_VIEWS,
        captureToken);
    assert(began);
    if (!began)
    {
        return;
    }
    g_committedCaptureBatch.frameNumber = tr.frameCount;
    if (!g_committedCaptureLane.list && parallelJobManager &&
        !g_committedCaptureLane.handedOff)
    {
        g_committedCaptureLane.list = parallelJobManager->AllocJobList(
            JOBLIST_RENDERER_FRONTEND,
            JOBLIST_PRIORITY_MEDIUM,
            RT_PT_COMMITTED_CAPTURE_MAX_VIEWS,
            0,
            nullptr);
        if (g_committedCaptureLane.list)
        {
            ++g_committedCaptureLane.allocations;
        }
    }
    else if (g_committedCaptureLane.list)
    {
        ++g_committedCaptureLane.reuses;
    }
}

void CapturePathTraceCommittedGeometryFrontendInput(viewDef_t* viewDef)
{
    if (viewDef)
    {
        viewDef->pathTraceCommittedGeometryProduct = nullptr;
        viewDef->pathTraceCommittedGeometryCaptureToken = 0;
        viewDef->pathTraceCommittedCaptureTelemetry = nullptr;
    }
    if (!g_committedCaptureBatch.contract.active)
    {
        return;
    }
    OPTICK_EVENT("PT Capture Committed Dynamic Geometry Input");
    ++g_committedCaptureBatch.observedViews;
    if (!viewDef)
    {
        ++g_committedCaptureBatch.fallbackInvalidView;
        return;
    }
    // Keep diagnostics independent of worker-product success. The last root
    // view wins; absent a root, the first observed subview is the host.
    if (!g_committedCaptureBatch.telemetryHost || !viewDef->isSubview)
    {
        g_committedCaptureBatch.telemetryHost = viewDef;
    }
    if (!g_committedCaptureBatch.contract.accepting ||
        g_committedCaptureBatch.contract.submitted ||
        g_committedCaptureBatch.contract.joined)
    {
        ++g_committedCaptureBatch.fallbackNotAccepting;
        return;
    }
    if (!g_committedCaptureLane.list)
    {
        ++g_committedCaptureBatch.fallbackListUnavailable;
        return;
    }
    const int surfaceCountInt = viewDef->numDrawSurfs;
    if (surfaceCountInt < 0 ||
        (surfaceCountInt > 0 && !viewDef->drawSurfs))
    {
        ++g_committedCaptureBatch.fallbackInvalidView;
        return;
    }
    if (g_committedCaptureBatch.contract.queuedViews >=
        RT_PT_COMMITTED_CAPTURE_MAX_VIEWS)
    {
        ++g_committedCaptureBatch.fallbackViewCapacity;
        return;
    }

    std::size_t vertexCount = 0;
    std::size_t indexCount = 0;
    std::size_t jointCount = 0;
    for (int surfaceIndex = 0; surfaceIndex < surfaceCountInt; ++surfaceIndex)
    {
        const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
        const srfTriangles_t* tri = drawSurf ? drawSurf->frontEndGeo : nullptr;
        if (!drawSurf || !tri || tri->numVerts <= 0 ||
            drawSurf->numIndexes <= 0)
        {
            continue;
        }
        if (!tri->verts || !tri->indexes ||
            drawSurf->numIndexes > tri->numIndexes)
        {
            ++g_committedCaptureBatch.fallbackPreflight;
            return;
        }
        const idJointMat* joints = nullptr;
        int jointsForSurface = 0;
        if (!ResolveCommittedJointSource(drawSurf, tri, joints, jointsForSurface))
        {
            ++g_committedCaptureBatch.fallbackPreflight;
            return;
        }
        if (vertexCount > UINT32_MAX - static_cast<std::size_t>(tri->numVerts) ||
            indexCount > UINT32_MAX - static_cast<std::size_t>(drawSurf->numIndexes) ||
            jointCount > UINT32_MAX - static_cast<std::size_t>(jointsForSurface))
        {
            ++g_committedCaptureBatch.fallbackPreflight;
            return;
        }
        vertexCount += static_cast<std::size_t>(tri->numVerts);
        indexCount += static_cast<std::size_t>(drawSurf->numIndexes);
        jointCount += static_cast<std::size_t>(jointsForSurface);
    }

    RtPathTraceCaptureOwnerSnapshot semanticSource;
    std::size_t semanticSourceBytes = 0;
    std::uint64_t semanticMarshalUs = 0;
    if (!viewDef->isSubview)
    {
        RtPathTracePrimaryViewDtoLineage sourceLineage;
        sourceLineage.sealedViewToken =
            viewDef->pathTraceSealedPrimaryViewToken;
        sourceLineage.predecessorCommittedViewToken =
            viewDef->pathTraceSealedPredecessorViewToken;
        sourceLineage.frameIndex =
            viewDef->pathTraceSealedPrimaryViewFrameIndex;
        sourceLineage.worldLifecycleGeneration =
            viewDef->pathTraceWorldLifecycleGeneration;
        sourceLineage.mapLoadSerial = viewDef->pathTraceSealedMapLoadSerial;
        sourceLineage.mapTimeStamp = static_cast<std::uint64_t>(
            viewDef->pathTraceSealedMapTimeStamp);
        RtPathTracePlanningCopyName(sourceLineage.mapName,
            sizeof(sourceLineage.mapName), viewDef->pathTraceSealedMapName);
        sourceLineage.barrierGeneration =
            viewDef->pathTraceSealedBarrierGeneration;
        sourceLineage.primaryView = true;
        RtPathTraceCommittedSemanticConfig semanticConfig;
        const std::uint64_t semanticMarshalStartUs = Sys_Microseconds();
        if (!CopyPathTraceCommittedSemanticConfigForSourcePhase1(
                sourceLineage, semanticConfig) ||
            !CapturePathTraceOwnerSourceSnapshot(
                viewDef, semanticConfig, semanticSource,
                semanticSourceBytes) ||
            !semanticSource.complete ||
            semanticSource.surfaces.size() !=
                static_cast<std::size_t>(surfaceCountInt))
        {
            ++g_committedCaptureBatch.fallbackCopy;
            return;
        }
        semanticMarshalUs = Sys_Microseconds() - semanticMarshalStartUs;
    }
    g_committedCaptureBatch.semanticMarshalUs += semanticMarshalUs;
    g_committedCaptureBatch.semanticOwnedBytes +=
        static_cast<std::uint64_t>(semanticSourceBytes);

    const std::size_t surfaceCount = static_cast<std::size_t>(surfaceCountInt);
    std::size_t ownedBytes = 0;
    if (!AddCommittedCaptureBytes(1, sizeof(RtPathTraceCommittedGeometryProduct), ownedBytes) ||
        !AddCommittedCaptureBytes(surfaceCount, sizeof(RtPathTraceCommittedGeometrySurface), ownedBytes) ||
        !AddCommittedCaptureBytes(surfaceCount, sizeof(RtPathTraceCommittedGeometryInput), ownedBytes) ||
        !AddCommittedCaptureBytes(vertexCount, sizeof(idDrawVert), ownedBytes) ||
        !AddCommittedCaptureBytes(indexCount, sizeof(triIndex_t), ownedBytes) ||
        !AddCommittedCaptureBytes(jointCount, sizeof(idJointMat), ownedBytes) ||
        !AddCommittedCaptureBytes(vertexCount, sizeof(PathTraceSmokeVertex), ownedBytes) ||
        !AddCommittedCaptureBytes(indexCount, sizeof(std::uint32_t), ownedBytes) ||
        !AddCommittedCaptureBytes(1, sizeof(RtPathTraceCommittedGeometryJob), ownedBytes) ||
        !AddCommittedCaptureBytes(semanticSource.surfaces.size(),
            sizeof(RtPathTraceCaptureRawSurface), ownedBytes) ||
        !AddCommittedCaptureBytes(semanticSource.classifierStages.size(),
            sizeof(RtSmokeTranslucentClassifierStageInput), ownedBytes) ||
        !AddCommittedCaptureBytes(semanticSource.runtimeStages.size(),
            sizeof(RtPathTraceRuntimeMaterialStagePod), ownedBytes) ||
        !AddCommittedCaptureBytes(semanticSource.registers.size(), sizeof(float), ownedBytes) ||
        !AddCommittedCaptureBytes(semanticSource.vertices.size(), sizeof(idDrawVert), ownedBytes) ||
        !AddCommittedCaptureBytes(semanticSource.indexes.size(), sizeof(triIndex_t), ownedBytes) ||
        !AddCommittedCaptureBytes(semanticSource.joints.size(), sizeof(idJointMat), ownedBytes) ||
        !AddCommittedCaptureBytes(semanticSource.modelTables.size(),
            sizeof(RtPathTraceCaptureModelTokenTablePod), ownedBytes) ||
        !AddCommittedCaptureBytes(semanticSource.modelSurfaceTokens.size(),
            sizeof(std::uint64_t), ownedBytes) ||
        !AddCommittedCaptureBytes(semanticSource.variantBases.size(),
            sizeof(RtPathTraceMaterialTextureVariantBasePod), ownedBytes) ||
        !AddCommittedCaptureBytes(semanticSource.registryMaterials.size(),
            sizeof(RtPathTraceCaptureRegistryMaterialPod), ownedBytes) ||
        !RtPathTraceCommittedCaptureReserve(
            g_committedCaptureBatch.contract,
            ownedBytes,
            RT_PT_COMMITTED_CAPTURE_MAX_BYTES))
    {
        ++g_committedCaptureBatch.fallbackBudget;
        return;
    }

    RtPathTraceCommittedGeometryProduct* product =
        new (R_ClearedFrameAlloc(sizeof(*product), FRAME_ALLOC_DRAW_SURFACE))
            RtPathTraceCommittedGeometryProduct{};
    product->viewIdentity = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(viewDef));
    product->captureToken = g_committedCaptureBatch.contract.captureToken;
    product->configFingerprint =
        semanticSource.lateConsumeToken.configFingerprint;
    product->sealedPrimaryViewToken =
        viewDef->pathTraceSealedPrimaryViewToken;
    product->surfaceCount = surfaceCountInt;
    product->vertexCapacity = static_cast<std::uint32_t>(vertexCount);
    product->indexCapacity = static_cast<std::uint32_t>(indexCount);
    RtPathTracePrimarySemanticDto& semanticDto = product->semanticDto;
    semanticDto.sealedPrimaryViewToken = viewDef->pathTraceSealedPrimaryViewToken;
    semanticDto.materialRegistryGeneration = semanticSource.registryGeneration;
    semanticDto.residentMaterialFactsGeneration =
        semanticSource.residentMaterialFactsGeneration;
    semanticDto.sourceDrawSurfCount = semanticSource.sourceDrawSurfCount;
    semanticDto.surfaceCount = static_cast<std::uint32_t>(semanticSource.surfaces.size());
    for (const RtPathTraceCaptureRawSurface& raw : semanticSource.surfaces)
    {
        semanticDto.factsDerivedSurfaceCount +=
            raw.semanticFactsDerived || raw.semanticFactsPresent ? 1u : 0u;
    }
    semanticDto.classifierStageCount = static_cast<std::uint32_t>(semanticSource.classifierStages.size());
    semanticDto.runtimeStageCount = static_cast<std::uint32_t>(semanticSource.runtimeStages.size());
    semanticDto.registerCount = static_cast<std::uint32_t>(semanticSource.registers.size());
    semanticDto.vertexCount = static_cast<std::uint32_t>(semanticSource.vertices.size());
    semanticDto.indexCount = static_cast<std::uint32_t>(semanticSource.indexes.size());
    semanticDto.jointCount = static_cast<std::uint32_t>(semanticSource.joints.size());
    semanticDto.modelTableCount = static_cast<std::uint32_t>(semanticSource.modelTables.size());
    semanticDto.modelSurfaceTokenCount = static_cast<std::uint32_t>(semanticSource.modelSurfaceTokens.size());
    semanticDto.variantBaseCount = static_cast<std::uint32_t>(semanticSource.variantBases.size());
    semanticDto.registryMaterialCount = static_cast<std::uint32_t>(semanticSource.registryMaterials.size());
    semanticDto.surfaces = CopyCommittedSemanticArray(semanticSource.surfaces);
    semanticDto.classifierStages = CopyCommittedSemanticArray(semanticSource.classifierStages);
    semanticDto.runtimeStages = CopyCommittedSemanticArray(semanticSource.runtimeStages);
    semanticDto.registers = CopyCommittedSemanticArray(semanticSource.registers);
    semanticDto.vertices = CopyCommittedSemanticArray(semanticSource.vertices);
    semanticDto.indexes = CopyCommittedSemanticArray(semanticSource.indexes);
    semanticDto.joints = CopyCommittedSemanticArray(semanticSource.joints);
    semanticDto.modelTables = CopyCommittedSemanticArray(semanticSource.modelTables);
    semanticDto.modelSurfaceTokens = CopyCommittedSemanticArray(semanticSource.modelSurfaceTokens);
    semanticDto.variantBases = CopyCommittedSemanticArray(semanticSource.variantBases);
    semanticDto.registryMaterials = CopyCommittedSemanticArray(semanticSource.registryMaterials);
    semanticDto.complete = !viewDef->isSubview;
    semanticDto.complete = semanticDto.complete &&
        RtPathTracePrimarySemanticDtoShapeValid(
            semanticDto, viewDef->pathTraceSealedPrimaryViewToken);
    if (!viewDef->isSubview && !semanticDto.complete)
    {
        ++g_committedCaptureBatch.fallbackCopy;
        return;
    }
    product->surfaces = surfaceCount > 0
        ? static_cast<RtPathTraceCommittedGeometrySurface*>(R_ClearedFrameAlloc(
            static_cast<int>(surfaceCount * sizeof(*product->surfaces)),
            FRAME_ALLOC_DRAW_SURFACE))
        : nullptr;
    product->vertices = vertexCount > 0
        ? static_cast<PathTraceSmokeVertex*>(R_FrameAlloc(
            static_cast<int>(vertexCount * sizeof(PathTraceSmokeVertex)),
            FRAME_ALLOC_DRAW_SURFACE))
        : nullptr;
    product->indexes = indexCount > 0
        ? static_cast<std::uint32_t*>(R_FrameAlloc(
            static_cast<int>(indexCount * sizeof(std::uint32_t)),
            FRAME_ALLOC_DRAW_SURFACE))
        : nullptr;
    RtPathTraceCommittedGeometryInput* inputs = surfaceCount > 0
        ? static_cast<RtPathTraceCommittedGeometryInput*>(R_ClearedFrameAlloc(
            static_cast<int>(surfaceCount * sizeof(*inputs)),
            FRAME_ALLOC_DRAW_SURFACE))
        : nullptr;

    std::uint32_t vertexOffset = 0;
    std::uint32_t indexOffset = 0;
    for (int surfaceIndex = 0; surfaceIndex < surfaceCountInt; ++surfaceIndex)
    {
        RtPathTraceCommittedGeometryInput& input = inputs[surfaceIndex];
        input.ordinal = static_cast<std::uint32_t>(surfaceIndex);
        const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
        const srfTriangles_t* tri = drawSurf ? drawSurf->frontEndGeo : nullptr;
        if (!drawSurf || !tri || tri->numVerts <= 0 ||
            drawSurf->numIndexes <= 0)
        {
            continue;
        }
        input.vertexCount = static_cast<std::uint32_t>(tri->numVerts);
        input.indexCount = static_cast<std::uint32_t>(drawSurf->numIndexes);
        input.vertexOffset = vertexOffset;
        input.indexOffset = indexOffset;
        idDrawVert* ownedVertices = static_cast<idDrawVert*>(R_FrameAlloc(
            tri->numVerts * static_cast<int>(sizeof(idDrawVert)),
            FRAME_ALLOC_DRAW_SURFACE));
        triIndex_t* ownedIndexes = static_cast<triIndex_t*>(R_FrameAlloc(
            drawSurf->numIndexes * static_cast<int>(sizeof(triIndex_t)),
            FRAME_ALLOC_DRAW_SURFACE));
        if (!TryCopyCommittedCaptureMemory(
                ownedVertices,
                tri->verts,
                static_cast<std::size_t>(tri->numVerts) * sizeof(idDrawVert)) ||
            !TryCopyCommittedCaptureMemory(
                ownedIndexes,
                tri->indexes,
                static_cast<std::size_t>(drawSurf->numIndexes) * sizeof(triIndex_t)))
        {
            ++g_committedCaptureBatch.fallbackCopy;
            return;
        }
        input.vertices = ownedVertices;
        input.indexes = ownedIndexes;

        const idJointMat* sourceJoints = nullptr;
        int sourceJointCount = 0;
        if (!ResolveCommittedJointSource(
                drawSurf, tri, sourceJoints, sourceJointCount))
        {
            ++g_committedCaptureBatch.fallbackPreflight;
            return;
        }
        if (sourceJoints && sourceJointCount > 0)
        {
            idJointMat* ownedJoints = static_cast<idJointMat*>(R_FrameAlloc(
                sourceJointCount * static_cast<int>(sizeof(idJointMat)),
                FRAME_ALLOC_DRAW_SURFACE));
            if (!TryCopyCommittedCaptureMemory(
                    ownedJoints,
                    sourceJoints,
                    static_cast<std::size_t>(sourceJointCount) *
                        sizeof(idJointMat)))
            {
                ++g_committedCaptureBatch.fallbackCopy;
                return;
            }
            for (int vertexIndex = 0; vertexIndex < tri->numVerts; ++vertexIndex)
            {
                const idDrawVert& vertex = ownedVertices[vertexIndex];
                if (vertex.color[0] >= sourceJointCount ||
                    vertex.color[1] >= sourceJointCount ||
                    vertex.color[2] >= sourceJointCount ||
                    vertex.color[3] >= sourceJointCount)
                {
                    // Do not let a compact owned array convert a malformed
                    // source joint index into a different in-bounds read.
                    ++g_committedCaptureBatch.fallbackPreflight;
                    return;
                }
            }
            input.joints = ownedJoints;
            input.jointCount = static_cast<std::uint32_t>(sourceJointCount);
        }
        if (drawSurf->space)
        {
            memcpy(input.modelMatrix, drawSurf->space->modelMatrix,
                sizeof(input.modelMatrix));
        }
        else
        {
            SetIdentityMatrix(input.modelMatrix);
        }
        CaptureCommittedBumpMatrix(drawSurf, input.bumpMatrix);
        input.ambientHandle = static_cast<std::uint64_t>(
            drawSurf->ambientCache != 0 ? drawSurf->ambientCache : tri->ambientCache);
        input.indexHandle = static_cast<std::uint64_t>(
            drawSurf->indexCache != 0 ? drawSurf->indexCache : tri->indexCache);
        input.jointHandle = static_cast<std::uint64_t>(drawSurf->jointCache);
        vertexOffset += input.vertexCount;
        indexOffset += input.indexCount;
    }

    RtPathTraceCommittedGeometryJob* job =
        new (R_ClearedFrameAlloc(sizeof(*job), FRAME_ALLOC_DRAW_SURFACE))
            RtPathTraceCommittedGeometryJob{};
    job->inputs = inputs;
    job->inputCount = static_cast<std::uint32_t>(surfaceCount);
    job->product = product;
    const std::uint32_t queueIndex =
        g_committedCaptureBatch.contract.queuedViews;
    if (!RtPathTraceCommittedCaptureQueue(g_committedCaptureBatch.contract))
    {
        if (!g_committedCaptureBatch.contract.accepting ||
            g_committedCaptureBatch.contract.submitted ||
            g_committedCaptureBatch.contract.joined)
        {
            ++g_committedCaptureBatch.fallbackNotAccepting;
        }
        else
        {
            ++g_committedCaptureBatch.fallbackViewCapacity;
        }
        return;
    }
    g_committedCaptureBatch.views[queueIndex].viewDef = viewDef;
    g_committedCaptureBatch.views[queueIndex].product = product;
    g_committedCaptureBatch.views[queueIndex].job = job;
    g_committedCaptureLane.list->AddJob(
        RunPathTraceCommittedGeometryJob, job);
}

void JoinPathTraceCommittedCaptureFrame()
{
    if (!g_committedCaptureBatch.contract.active)
    {
        return;
    }
    const std::uint32_t queued =
        g_committedCaptureBatch.contract.queuedViews;
    g_committedCaptureBatch.contract.accepting = false;
    std::uint64_t submitUs = 0;
    std::uint64_t joinUs = 0;
    std::uint64_t jobCpuSumUs = 0;
    std::uint64_t jobCpuMaxUs = 0;
    std::uint32_t completeViews = 0;
    if (queued > 0)
    {
        OPTICK_EVENT("PT Capture Committed Dynamic Geometry Join");
        const std::uint64_t submitStartUs = Sys_Microseconds();
        // A zero worker count executes synchronously on the submitter in this
        // job manager. One explicit JLProc worker owns this major task.
        g_committedCaptureLane.list->Submit(nullptr, 1);
        g_committedCaptureBatch.contract.submitted = true;
        submitUs = Sys_Microseconds() - submitStartUs;
        const std::uint64_t joinStartUs = Sys_Microseconds();
        g_committedCaptureLane.list->Wait();
        joinUs = Sys_Microseconds() - joinStartUs;
        g_committedCaptureBatch.contract.joined = true;
    }

    const std::uint64_t finalizeStartUs = Sys_Microseconds();
    for (std::uint32_t index = 0; index < queued; ++index)
    {
        RtPathTraceCommittedCaptureAssociation& association =
            g_committedCaptureBatch.views[index];
        if (association.job)
        {
            jobCpuSumUs += association.job->cpuUs;
            jobCpuMaxUs = Max(jobCpuMaxUs, association.job->cpuUs);
        }
        if (RtPathTraceCommittedGeometryProductShapeValid(
                association.product,
                association.viewDef,
                association.viewDef->numDrawSurfs,
                g_committedCaptureBatch.contract.captureToken))
        {
            ++completeViews;
        }
        else
        {
            ++g_committedCaptureBatch.fallbackIncomplete;
        }
    }

    if (g_committedCaptureBatch.contract.ownedBytes >
        g_committedCaptureLane.ownedBytesHighWater)
    {
        g_committedCaptureLane.ownedBytesHighWater =
            g_committedCaptureBatch.contract.ownedBytes;
    }
    const std::uint32_t fallbackViews =
        g_committedCaptureBatch.fallbackInvalidView +
        g_committedCaptureBatch.fallbackListUnavailable +
        g_committedCaptureBatch.fallbackViewCapacity +
        g_committedCaptureBatch.fallbackPreflight +
        g_committedCaptureBatch.fallbackBudget +
        g_committedCaptureBatch.fallbackCopy +
        g_committedCaptureBatch.fallbackNotAccepting +
        g_committedCaptureBatch.fallbackIncomplete;
    RtPathTraceCommittedCaptureTelemetry telemetry;
    telemetry.captureToken = g_committedCaptureBatch.contract.captureToken;
    telemetry.frameNumber = g_committedCaptureBatch.frameNumber;
    telemetry.observedViews = g_committedCaptureBatch.observedViews;
    telemetry.queuedViews = queued;
    telemetry.completeViews = completeViews;
    telemetry.fallbackViews = fallbackViews;
    telemetry.fallbackInvalidView = g_committedCaptureBatch.fallbackInvalidView;
    telemetry.fallbackListUnavailable =
        g_committedCaptureBatch.fallbackListUnavailable;
    telemetry.fallbackViewCapacity =
        g_committedCaptureBatch.fallbackViewCapacity;
    telemetry.fallbackPreflight = g_committedCaptureBatch.fallbackPreflight;
    telemetry.fallbackBudget = g_committedCaptureBatch.fallbackBudget;
    telemetry.fallbackCopy = g_committedCaptureBatch.fallbackCopy;
    telemetry.fallbackNotAccepting =
        g_committedCaptureBatch.fallbackNotAccepting;
    telemetry.fallbackIncomplete = g_committedCaptureBatch.fallbackIncomplete;
    telemetry.listAllocations = g_committedCaptureLane.allocations;
    telemetry.listReuses = g_committedCaptureLane.reuses;
    telemetry.ownedBytes = static_cast<std::uint64_t>(
        g_committedCaptureBatch.contract.ownedBytes);
    telemetry.ownedBytesHighWater = static_cast<std::uint64_t>(
        g_committedCaptureLane.ownedBytesHighWater);
    telemetry.jobCpuSumUs = jobCpuSumUs;
    telemetry.jobCpuMaxUs = jobCpuMaxUs;
    telemetry.submitUs = submitUs;
    telemetry.joinUs = joinUs;
    telemetry.semanticMarshalUs = g_committedCaptureBatch.semanticMarshalUs;
    telemetry.semanticOwnedBytes = g_committedCaptureBatch.semanticOwnedBytes;
    telemetry.viewReconciled =
        RtPathTraceCommittedCaptureViewTelemetryReconciles(telemetry);
    assert(telemetry.viewReconciled);

    RtPathTraceCommittedCaptureTelemetry* telemetryCarrier =
        g_committedCaptureBatch.telemetryHost
            ? new (R_ClearedFrameAlloc(
                  sizeof(RtPathTraceCommittedCaptureTelemetry),
                  FRAME_ALLOC_DRAW_SURFACE))
                  RtPathTraceCommittedCaptureTelemetry(telemetry)
            : nullptr;

    for (std::uint32_t index = 0; index < queued; ++index)
    {
        RtPathTraceCommittedCaptureAssociation& association =
            g_committedCaptureBatch.views[index];
        if (RtPathTraceCommittedGeometryProductShapeValid(
                association.product,
                association.viewDef,
                association.viewDef->numDrawSurfs,
                g_committedCaptureBatch.contract.captureToken))
        {
            association.viewDef->pathTraceCommittedGeometryCaptureToken =
                g_committedCaptureBatch.contract.captureToken;
            association.viewDef->pathTraceCommittedGeometryProduct =
                association.product;
            if (RtPathTraceCommittedPrimaryDtoMayPublish(
                    !association.viewDef->isSubview,
                    true,
                    g_committedCaptureBatch.contract.joined,
                    association.product->complete,
                    association.product->sealedPrimaryViewToken,
                    association.viewDef->pathTraceSealedPrimaryViewToken))
            {
                RtPathTracePrimaryViewDtoLineage lineage;
                lineage.sealedViewToken =
                    association.viewDef->pathTraceSealedPrimaryViewToken;
                lineage.predecessorCommittedViewToken =
                    association.viewDef->pathTraceSealedPredecessorViewToken;
                lineage.frameIndex =
                    association.viewDef->pathTraceSealedPrimaryViewFrameIndex;
                lineage.worldLifecycleGeneration =
                    association.viewDef->pathTraceWorldLifecycleGeneration;
                lineage.mapLoadSerial =
                    association.viewDef->pathTraceSealedMapLoadSerial;
                lineage.mapTimeStamp = static_cast<std::uint64_t>(
                    association.viewDef->pathTraceSealedMapTimeStamp);
                RtPathTracePlanningCopyName(lineage.mapName,
                    sizeof(lineage.mapName),
                    association.viewDef->pathTraceSealedMapName);
                lineage.barrierGeneration =
                    association.viewDef->pathTraceSealedBarrierGeneration;
                lineage.configFingerprint =
                    association.product->configFingerprint;
                lineage.primaryView = true;
                lineage.complete = true;
                PublishPathTracePrimaryViewDtoLineagePhase1(lineage);
            }
        }
    }
    const std::uint64_t finalizeUs = Sys_Microseconds() - finalizeStartUs;
    if (telemetryCarrier && g_committedCaptureBatch.telemetryHost)
    {
        telemetryCarrier->finalizeUs = finalizeUs;
        g_committedCaptureBatch.telemetryHost->
            pathTraceCommittedCaptureTelemetry = telemetryCarrier;
    }
    assert(RtPathTraceCommittedCaptureMayRelease(
        g_committedCaptureBatch.contract));
    assert(!g_committedCaptureLane.list ||
        !g_committedCaptureLane.list->IsSubmitted());
    g_committedCaptureBatch = RtPathTraceCommittedCaptureBatch();
}

void ShutdownPathTraceCommittedCaptureLane()
{
    const bool idle = !g_committedCaptureBatch.contract.active &&
        (!g_committedCaptureLane.list ||
            !g_committedCaptureLane.list->IsSubmitted());
    assert(idle);
    if (!idle || g_committedCaptureLane.handedOff)
    {
        return;
    }
    idParallelJobList* list = g_committedCaptureLane.list;
    g_committedCaptureLane.list = nullptr;
    g_committedCaptureLane.handedOff = true;
    if (list && parallelJobManager)
    {
        parallelJobManager->FreeJobList(list);
    }
}

void ComparePathTraceCommittedDynamicGeometry(
    const viewDef_t* viewDef,
    std::int32_t surfaceIndex,
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    const PathTraceSmokeVertex* serialVertices,
    std::uint32_t serialVertexCount,
    const std::uint32_t* serialIndexes,
    std::uint32_t serialIndexCount,
    std::uint32_t serialVertexBase,
    const RtPathTraceCommittedGeometryCounters& serialCounters)
{
    OPTICK_EVENT("PT Committed Dynamic Geometry Compare");
    RtPathTraceCommittedCaptureTelemetry* telemetry = viewDef
        ? viewDef->pathTraceCommittedCaptureTelemetry
        : nullptr;
    if (!telemetry)
    {
        // Join attaches the independent carrier only to the selected root/host.
        // Subview products remain valid, but their comparisons are intentionally
        // outside this host-scoped reconciliation row.
        return;
    }
    const RtPathTraceCommittedGeometryProduct* product = viewDef
        ? viewDef->pathTraceCommittedGeometryProduct
        : nullptr;
    if (!RtPathTraceCommittedGeometryProductShapeValid(
            product,
            viewDef,
            viewDef ? viewDef->numDrawSurfs : -1,
            viewDef ? viewDef->pathTraceCommittedGeometryCaptureToken : 0) ||
        surfaceIndex < 0 || surfaceIndex >= product->surfaceCount)
    {
        RtPathTraceCommittedCaptureRecordCompare(
            *telemetry,
            RtPathTraceCommittedCaptureCompareTerminal::Mismatch,
            true);
        return;
    }
    const RtPathTraceCommittedGeometrySurface& surface =
        product->surfaces[surfaceIndex];

    const vertCacheHandle_t ambientHandle = drawSurf
        ? (drawSurf->ambientCache != 0
            ? drawSurf->ambientCache
            : (tri ? tri->ambientCache : 0))
        : 0;
    const vertCacheHandle_t indexHandle = drawSurf
        ? (drawSurf->indexCache != 0
            ? drawSurf->indexCache
            : (tri ? tri->indexCache : 0))
        : 0;
    if ((ambientHandle != 0 && !idVertexCache::CacheIsStatic(ambientHandle) &&
            vertexCache.CacheIsCurrent(ambientHandle)) ||
        (indexHandle != 0 && !idVertexCache::CacheIsStatic(indexHandle) &&
            vertexCache.CacheIsCurrent(indexHandle)))
    {
        RtPathTraceCommittedCaptureRecordCompare(
            *telemetry,
            RtPathTraceCommittedCaptureCompareTerminal::SourceExcluded);
        return;
    }

    bool vertexMatch = surface.vertexCount == serialVertexCount;
    if (vertexMatch && serialVertexCount > 0)
    {
        vertexMatch = serialVertices &&
            memcmp(product->vertices + surface.vertexOffset,
                serialVertices,
                serialVertexCount * sizeof(PathTraceSmokeVertex)) == 0;
    }
    bool indexMatch = surface.indexCount == serialIndexCount;
    if (indexMatch)
    {
        for (std::uint32_t index = 0; index < serialIndexCount; ++index)
        {
            if (!serialIndexes || serialIndexes[index] < serialVertexBase ||
                product->indexes[surface.indexOffset + index] !=
                    serialIndexes[index] - serialVertexBase)
            {
                indexMatch = false;
                break;
            }
        }
    }
    const bool counterMatch = CountersEqual(surface.counters, serialCounters);
    if (vertexMatch && indexMatch && counterMatch)
    {
        RtPathTraceCommittedCaptureRecordCompare(
            *telemetry,
            RtPathTraceCommittedCaptureCompareTerminal::Exact);
    }
    else
    {
        RtPathTraceCommittedCaptureRecordCompare(
            *telemetry,
            RtPathTraceCommittedCaptureCompareTerminal::Mismatch,
            false,
            !vertexMatch,
            !indexMatch,
            !counterMatch);
    }
}

void FinalizePathTraceCommittedCaptureTelemetry(const viewDef_t* viewDef)
{
    RtPathTraceCommittedCaptureTelemetry* telemetry = viewDef
        ? viewDef->pathTraceCommittedCaptureTelemetry
        : nullptr;
    if (!telemetry)
    {
        return;
    }
    assert(!telemetry->finalized);
    if (telemetry->finalized)
    {
        return;
    }
    telemetry->viewReconciled =
        RtPathTraceCommittedCaptureViewTelemetryReconciles(*telemetry);
    telemetry->compareReconciled =
        RtPathTraceCommittedCaptureCompareTelemetryReconciles(*telemetry);
    telemetry->finalized = true;
    assert(telemetry->viewReconciled);
    assert(telemetry->compareReconciled);
    if (!RtPathTraceCommittedCaptureTelemetryDue(telemetry->captureToken))
    {
        return;
    }
    common->Printf(
        "PathTracePrimaryPass: committedDynamicGeometryShadow token=%llu frame=%d hostScope=%s parallelism=%u views(observed/queued/complete/fallback)=%u/%u/%u/%u fallback(invalid/list/cap/preflight/budget/copy/notAccepting/incomplete)=%u/%u/%u/%u/%u/%u/%u/%u list(alloc/reuse)=%u/%u ownedBytes=%llu highWater=%llu jobCpu(sum/max)=%llu/%llu timing(submit/join/finalize)=%llu/%llu/%llu compare(calls/sourceExcluded/exact/mismatch)=%u/%u/%u/%u mismatch(shape/vertex/index/counter)=%u/%u/%u/%u reconcile(view/compare)=%d/%d\n",
        static_cast<unsigned long long>(telemetry->captureToken),
        telemetry->frameNumber,
        viewDef->isSubview ? "firstSubview" : "root",
        telemetry->configuredParallelism,
        telemetry->observedViews,
        telemetry->queuedViews,
        telemetry->completeViews,
        telemetry->fallbackViews,
        telemetry->fallbackInvalidView,
        telemetry->fallbackListUnavailable,
        telemetry->fallbackViewCapacity,
        telemetry->fallbackPreflight,
        telemetry->fallbackBudget,
        telemetry->fallbackCopy,
        telemetry->fallbackNotAccepting,
        telemetry->fallbackIncomplete,
        telemetry->listAllocations,
        telemetry->listReuses,
        static_cast<unsigned long long>(telemetry->ownedBytes),
        static_cast<unsigned long long>(telemetry->ownedBytesHighWater),
        static_cast<unsigned long long>(telemetry->jobCpuSumUs),
        static_cast<unsigned long long>(telemetry->jobCpuMaxUs),
        static_cast<unsigned long long>(telemetry->submitUs),
        static_cast<unsigned long long>(telemetry->joinUs),
        static_cast<unsigned long long>(telemetry->finalizeUs),
        telemetry->compareCalls,
        telemetry->compareSourceExcluded,
        telemetry->compareExact,
        telemetry->compareMismatch,
        telemetry->compareShapeMismatch,
        telemetry->compareVertexMismatch,
        telemetry->compareIndexMismatch,
        telemetry->compareCounterMismatch,
        telemetry->viewReconciled ? 1 : 0,
        telemetry->compareReconciled ? 1 : 0);
}
