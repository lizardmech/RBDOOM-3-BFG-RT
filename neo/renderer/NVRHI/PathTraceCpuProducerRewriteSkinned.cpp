#include "precompiled.h"
#pragma hdrstop

#include "PathTracePrimaryPass.h"
#include "PathTraceCpuProducerRewrite.h"
#include "PathTraceSmokeResources.h"
#include "PathTraceAcceleration.h"
#include "PathTraceCVars.h"
#include <nvrhi/utils.h>
#include <algorithm>

#if defined(USE_OPTICK) && USE_OPTICK
#include <optick.h>
#else
#define OPTICK_EVENT(...) ((void)0)
#define OPTICK_TAG(...) ((void)0)
#endif

namespace
{

enum class RewriteStructuredBufferUsage
{
	Default,
	AccelStructBuildInput
};

nvrhi::BufferHandle RewriteCreateStructuredBuffer(
	nvrhi::IDevice* device,
	const char* debugName,
	size_t bytes,
	uint32_t stride,
	bool uav,
	RewriteStructuredBufferUsage usage = RewriteStructuredBufferUsage::Default)
{
	if (!device || bytes == 0 || stride == 0)
	{
		return nullptr;
	}
	nvrhi::BufferDesc desc;
	desc.byteSize = bytes;
	desc.structStride = stride;
	desc.debugName = debugName;
	desc.canHaveUAVs = uav;
	desc.isAccelStructBuildInput =
		usage == RewriteStructuredBufferUsage::AccelStructBuildInput;
	desc.initialState = uav ? nvrhi::ResourceStates::UnorderedAccess : nvrhi::ResourceStates::ShaderResource;
	desc.keepInitialState = true;
	return device->createBuffer(desc);
}

} // namespace

namespace
{
constexpr uint32_t kRewriteSkinnedBlasRefreshCadenceFrames = 60;
constexpr uint64_t kRewriteSkinnedCpuCap = 256ull * 1024ull * 1024ull;

}

bool PathTracePrimaryPass::CommitRewriteSkinnedGpuSkinAndHitRoute(
    nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
    const RtCpuRewriteFrozenProductView* view, const RtCpuRewriteJoinResult& join,
    uint32_t rigidExtraCount, int selectedSlot, RtCpuRewriteSkinnedTransaction& tx,
    std::vector<nvrhi::rt::InstanceDesc>& extraTlas)
{
    auto reject = [&](RtCpuRewriteSkinnedRejectReason reason) {
        m_rewriteSkinnedLastRejectReason = static_cast<uint32_t>(reason);
        tx.outcome = 0;
        return false;
    };
    if (!device || !commandList) return reject(RtCpuRewriteSkinnedRejectReason::InvalidDevice);
    if (join.skinnedCount != join.skinned.size() || join.skinnedCount > kRtCpuRewriteTlasMaxInstances)
        return reject(RtCpuRewriteSkinnedRejectReason::JoinRange);
    if (m_rewriteTlasCommitSerial == UINT64_MAX)
        return reject(RtCpuRewriteSkinnedRejectReason::SerialOverflow);
    if (selectedSlot < 0 || selectedSlot >= 3)
        return reject(RtCpuRewriteSkinnedRejectReason::NoIdleSlot);
    tx.selectedSlot = selectedSlot;
    tx.zero = join.skinnedCount == 0;
    RtCpuRewriteSkinnedPackage& retained = m_rewriteSkinnedPackages[selectedSlot];
    if (!tx.zero && !RtCpuRewritePlanSkinnedSlot(m_rewriteTlasCommitSerial,
        retained.lastCommittedSerial, m_rewriteCurrentSkinnedSlot, selectedSlot))
        return reject(RtCpuRewriteSkinnedRejectReason::NoIdleSlot);
    auto* service = RtCpuProducerRewrite_GetService();
    const RtCpuRewriteOverlayView* overlay = service ? service->OverlayView() : nullptr;
    if (!tx.zero && (!overlay || !overlay->joints)) return reject(RtCpuRewriteSkinnedRejectReason::MissingOverlay);
    if (!tx.zero && (!view || !view->skinnedMeshes || !view->skinnedMeshCount))
        return reject(RtCpuRewriteSkinnedRejectReason::MissingProduct);
    if (!tx.zero && (!m_smokeSkinnedGpuSkinningPipeline || !m_smokeSkinnedGpuSkinningBindingLayout))
        return reject(RtCpuRewriteSkinnedRejectReason::MissingPipeline);
    const uint32_t firstSkinned = RtCpuRewriteSkinnedFirstInstanceId(rigidExtraCount);
    if (RtCpuRewriteSkinnedIdsOverlapRigid(firstSkinned, join.skinnedCount, rigidExtraCount))
        return reject(RtCpuRewriteSkinnedRejectReason::InstanceIdOverlap);

    OPTICK_EVENT("PT CPU Commit Skinned");
    const auto compatibility = RtCpuRewriteValidateSkinnedPrepared(view, overlay, join);
    if (compatibility != RtCpuRewriteSkinnedRejectReason::None) return reject(compatibility);
    RtCpuRewriteSkinnedLayout layout;
    std::vector<PathTraceSkinnedSourceVertex> normalSourceVertices;
    std::vector<PathTraceSkinnedSurfaceDispatchRecord> dispatches;
    std::vector<PathTraceSkinnedJointMatrix> joints;
    std::vector<uint32_t> previousMeshes;
    RtCpuRewriteSkinnedRoutePackage routes;
    uint64_t vertexCount = 0, indexCount = 0, jointCount = 0;
    uint64_t gpuBytes = 0, layoutCpuBytes = 0;
    uint64_t residentGpu = 0, residentCpu = m_rewriteSkinnedHistoryCpuBytes;
    for (const auto& slot : m_rewriteSkinnedPackages)
    {
        residentGpu += slot.logicalBytes;
        residentCpu += slot.cpuBytes;
    }
    try
    {
        {
            OPTICK_EVENT("PT CPU Commit Skinned Prepare");
            if (!tx.zero)
            {
                OPTICK_EVENT("PT CPU Skinned Worker Consume");
                const auto& prepared = view->skinnedPrepared;
                vertexCount = prepared.vertexCount; indexCount = prepared.indexCount; jointCount = prepared.jointCount;
                const uint64_t count = prepared.count;
                layout.capacities.assign(prepared.capacities, prepared.capacities + 8);
                OPTICK_TAG("skinWorkerConsumedVertices", vertexCount);
                OPTICK_TAG("skinWorkerConsumedIndexes", indexCount);
                OPTICK_TAG("skinWorkerConsumedTriangles", indexCount / 3);
                OPTICK_TAG("skinWorkerRootAge", overlay->rootFrame - view->rootFrame);
                OPTICK_TAG("skinMainReconstruction", uint32_t(0));
                for (uint64_t bytes : layout.capacities) gpuBytes += bytes;
                gpuBytes += indexCount * sizeof(uint32_t); // Private BLAS index arrays.
                tx.allocationVertexCapacity = RtCpuRewriteSkinnedAllocationVertexCapacity(vertexCount);
                if (!tx.allocationVertexCapacity) return reject(RtCpuRewriteSkinnedRejectReason::Capacity);
                gpuBytes += (tx.allocationVertexCapacity - vertexCount) * sizeof(PathTraceSmokeVertex);
                layoutCpuBytes = count * (sizeof(RtCpuRewriteSkinnedLayoutRow) + sizeof(RtCpuRewriteSkinnedGpuSlot) +
                        sizeof(nvrhi::rt::GeometryDesc)) + 8 * sizeof(uint64_t);
                tx.historyCpuBytes = jointCount * sizeof(PathTraceSkinnedJointMatrix) +
                    count * sizeof(RtCpuRewriteRetainedSkinnedJoints);
                // Only exact pose/record patches and cold material/normal-UV copies remain.
                const uint64_t staging = layoutCpuBytes + layout.capacities[0] + tx.historyCpuBytes +
                    layout.capacities[3] + layout.capacities[4] + layout.capacities[6] + layout.capacities[7] +
                    (count + retained.meshes.size()) * (sizeof(uint64_t) + sizeof(uint32_t) + sizeof(bool));
                if (!RtCpuRewriteValidateSkinnedCapacity(residentCpu, staging, 0, kRewriteSkinnedCpuCap, &tx.cpuPeak))
                    return reject(RtCpuRewriteSkinnedRejectReason::Capacity);
                layout.rows.assign(prepared.rows, prepared.rows + count);
                dispatches.assign(prepared.dispatches, prepared.dispatches + count);
                joints.resize(jointCount * 2);
                tx.history.reserve(count);
                uint32_t previousValidCount = 0;
                for (uint32_t si = 0; si < join.skinnedCount; ++si)
                {
                    const auto& js = join.skinned[si];
                    auto& row = layout.rows[si];
                    row.words[20] = js.materialLogicalId; row.words[28] = js.materialIndex;
                    auto& d = dispatches[si];
                    const uint32_t jo = d.currentJointOffset;
                    if (js.hasTextureMatrix)
                    {
                        d.flags |= PT_SKINNED_DISPATCH_HAS_TEX_MATRIX;
                        std::copy(js.textureMatrix, js.textureMatrix + 3, d.texMatrix0);
                        std::copy(js.textureMatrix + 3, js.textureMatrix + 6, d.texMatrix1);
                    }
                    RtCpuRewriteBuildAffineFromObjectToWorld(js.currentObjectToWorld, d.currentObjectToWorld);
                    RtCpuRewriteBuildAffineFromObjectToWorld(js.currentObjectToWorld, d.previousObjectToWorld);
                    const PathTraceSkinnedJointMatrix* previous = overlay->joints + js.jointOffset;
                    for (const auto& history : m_rewritePreviousSkinnedJoints)
                    {
                        if (history.joints.size() == js.jointCount &&
                            RtCpuRewriteCommittedPoseValid(history.rootFrame, overlay->rootFrame,
                                history.epoch, overlay->lifecycleGeneration,
                                RtCpuRewriteSkinnedHistoryIdentityEqual(history.layout, row), service->LastCommittedRootFrame()))
                        {
                            previous = history.joints.data();
                            RtCpuRewriteBuildAffineFromObjectToWorld(history.objectToWorld, d.previousObjectToWorld);
                            d.flags |= PT_SKINNED_DISPATCH_HAS_PREVIOUS_JOINTS | PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS;
                            ++previousValidCount;
                            break;
                        }
                    }
                    std::copy(overlay->joints + js.jointOffset, overlay->joints + js.jointOffset + js.jointCount, joints.begin() + jo);
                    std::copy(previous, previous + js.jointCount, joints.begin() + jointCount + jo);
                    RtCpuRewriteRetainedSkinnedJoints history;
                    history.instanceKey = js.instanceKey;
                    history.layout = row;
                    history.rootFrame = overlay->rootFrame;
                    history.epoch = overlay->lifecycleGeneration;
                    std::copy(js.currentObjectToWorld, js.currentObjectToWorld + 16, history.objectToWorld);
                    history.joints.assign(overlay->joints + js.jointOffset, overlay->joints + js.jointOffset + js.jointCount);
                    tx.history.push_back(std::move(history));
                }
                // Each accepted history row has the same source vertices and
                // joints, copied into this frame's offsets. The skinning shader
                // writes previous positions into this frame's output allocation.
                OPTICK_TAG("skinnedPreviousValid", previousValidCount);
                OPTICK_TAG("skinnedPreviousReset", static_cast<uint32_t>(count) - previousValidCount);
                tx.layoutComparison = RtCpuRewriteCompareSkinnedLayout(layout, retained.layout);
                tx.replace = !retained.output || !retained.contentsValid ||
                    tx.layoutComparison.reason != RtCpuRewriteSkinnedLayoutMismatch::Equal;
                tx.layoutMiss = retained.output && tx.replace;
                if (tx.replace)
                {
                    OPTICK_EVENT("PT CPU Skinned Allocation Plan");
                    std::vector<uint64_t> vertexBounds;
                    vertexBounds.reserve(retained.meshes.size());
                    for (const auto& mesh : retained.meshes) vertexBounds.push_back(mesh.allocationVertexCapacity);
                    if (!RtCpuRewritePlanSkinnedAllocationReuse(retained.layout, layout, vertexBounds,
                        vertexCount, previousMeshes)) return reject(RtCpuRewriteSkinnedRejectReason::Replacement);
                }
                tx.normalTextureSignature = 1469598103934665603ull;
                for (const auto& skin : join.skinned)
                    tx.normalTextureSignature = (tx.normalTextureSignature ^
                        RtCpuRewriteTextureMatrixSignature(skin.normalTextureMatrix, 6)) * 1099511628211ull;
                if (tx.replace || retained.normalTextureSignature != tx.normalTextureSignature)
                {
                    normalSourceVertices.assign(prepared.vertices, prepared.vertices + vertexCount);
                    OPTICK_TAG("skinMainNormalUvVertices", vertexCount);
                    for (uint32_t si = 0; si < join.skinnedCount; ++si)
                    {
                        const auto& d = dispatches[si];
                        for (uint32_t v = d.sourceVertexOffset; v < d.sourceVertexOffset + d.vertexCount; ++v)
                            RtCpuRewriteApplyTextureMatrix(normalSourceVertices[v].texCoord + 2, join.skinned[si].normalTextureMatrix);
                    }
                }

                if (!RtCpuRewriteValidateSkinnedCapacity(residentGpu, tx.replace ? gpuBytes : 0,
                    m_rewriteRetirementLedger.skinnedLiveLogicalBytes,
                    RtCpuProducerRewriteService::kGpuSkinnedRetainCapBytes, &tx.gpuPeak))
                    return reject(RtCpuRewriteSkinnedRejectReason::Capacity);
                if (tx.replace)
                {
                    if (m_rewriteNextSkinnedGeneration == 0 || m_rewriteNextSkinnedGeneration > UINT64_MAX - 2)
                        return reject(RtCpuRewriteSkinnedRejectReason::GenerationExhaustion);
                    tx.replacement.sourceGeneration = m_rewriteNextSkinnedGeneration++;
                    tx.replacement.outputGeneration = m_rewriteNextSkinnedGeneration++;
                    tx.package = &tx.replacement;
                }
                else tx.package = &retained;
                routes.upload.records.assign(prepared.records, prepared.records + count);
                uint32_t previousPositionCount = 0;
                for (const auto& d : dispatches)
                    if (d.flags & PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS)
                        previousPositionCount = std::max(previousPositionCount, d.previousPositionOffset + d.vertexCount);
                for (uint32_t si = 0; si < count; ++si)
                    if (!RtCpuRewritePatchSkinnedRouteRecord(routes.upload.records[si], dispatches[si],
                        tx.package->sourceGeneration, tx.package->outputGeneration, firstSkinned + si, previousPositionCount))
                        return reject(RtCpuRewriteSkinnedRejectReason::RoutePackage);
                if (tx.replace)
                {
                    OPTICK_EVENT("PT CPU Skinned Cold Material Patch");
                    routes.upload.triangles.assign(prepared.triangles, prepared.triangles + indexCount / 3);
                    for (uint32_t si = 0; si < count; ++si)
                    {
                        const auto& record = routes.upload.records[si];
                        const uint64_t instanceHash = uint64_t(record.instanceHashLo) | (uint64_t(record.instanceHashHi) << 32);
                        for (uint32_t ti = record.triangleMetadataOffset; ti < record.triangleMetadataOffset + record.triangleCount; ++ti)
                            PtPatchSkinnedHitRouteMaterial(routes.upload.triangles[ti], instanceHash,
                                join.skinned[si].materialLogicalId, join.skinned[si].materialIndex);
                    }
                    OPTICK_TAG("skinMainMaterialTriangles", indexCount / 3);
                }
                extraTlas.reserve(extraTlas.size() + join.skinnedCount);
            }
            // Preflight ownership transfer before allocation/recording. No live member is changed.
            size_t retireCount = 0;
            for (int slot = 0; slot < 3; ++slot)
                if (tx.zero || (tx.replace && slot == selectedSlot)) retireCount += m_rewriteSkinnedPackages[slot].meshes.size();
            if (!RtCpuRewriteValidateSkinnedCapacity(tx.cpuPeak ? tx.cpuPeak : residentCpu,
                retireCount * sizeof(RtCpuRewriteRetirementCandidate), 0, kRewriteSkinnedCpuCap, &tx.cpuPeak))
                return reject(RtCpuRewriteSkinnedRejectReason::Capacity);
            tx.retirement.reserve(retireCount);
            for (int slot = 0; slot < 3; ++slot)
            {
                if (!tx.zero && (!tx.replace || slot != selectedSlot)) continue;
                const auto& old = m_rewriteSkinnedPackages[slot];
                for (const auto& mesh : old.meshes)
                    tx.retirement.push_back({m_rewriteTlasCommitSerial, mesh.blas, old.output, mesh.indexBuffer,
                        mesh.bytes, RtCpuRewriteRetirementKind::Skinned});
            }
            if (!tx.retirement.empty())
            {
                RtCpuRewriteRetirementDecision decision;
                if (!PreflightRewriteGpuGeometryBatch(tx.retirement, &decision))
                {
                    m_rewriteRetirementRejectedThisAttempt = true;
                    RtCpuRewriteRetirementLedgerReject(m_rewriteRetirementLedger, decision);
                    PublishRewriteRetirementTelemetry();
                    if (!m_rewriteRetirementWarningLatched)
                    {
                        common->Warning("R3-010 skinned retirement preflight rejected (%u); keep-last until route/lifecycle drain, retireFrames=%d",
                            static_cast<uint32_t>(decision), r_pathTracingSceneRetireFrames.GetInteger());
                        m_rewriteRetirementWarningLatched = true;
                    }
                    switch (decision)
                    {
                    case RtCpuRewriteRetirementDecision::BytePressure:
                        return reject(RtCpuRewriteSkinnedRejectReason::RetirementBytePressure);
                    case RtCpuRewriteRetirementDecision::UnknownBytes:
                        return reject(RtCpuRewriteSkinnedRejectReason::RetirementUnknownBytes);
                    case RtCpuRewriteRetirementDecision::ArithmeticOverflow:
                        return reject(RtCpuRewriteSkinnedRejectReason::RetirementArithmeticOverflow);
                    default:
                        return reject(RtCpuRewriteSkinnedRejectReason::RetirementCountPressure);
                    }
                }
            }
        }
        if (tx.zero) { tx.outcome = 4; return true; }
        {
            OPTICK_EVENT("PT CPU Commit Skinned Resolve");
            auto& p = *tx.package;
            if (tx.replace)
            {
                OPTICK_EVENT("PT CPU Skinned Allocate");
                auto create = [&](const char* name, size_t bytes, uint32_t stride, bool uav) {
                    auto buffer = RewriteCreateStructuredBuffer(device, name, bytes, stride, uav);
                    tx.bufferCreates += buffer != nullptr;
                    return buffer;
                };
                p.source = create("PathTraceRewriteSkinnedSource", layout.capacities[0], sizeof(PathTraceSkinnedSourceVertex), false);
                p.indexes = create("PathTraceRewriteSkinnedIndexes", layout.capacities[1], sizeof(uint32_t), false);
                p.output = RewriteCreateStructuredBuffer(device, "PathTraceRewriteSkinnedOutput",
                    tx.allocationVertexCapacity * sizeof(PathTraceSmokeVertex),
                    sizeof(PathTraceSmokeVertex), true, RewriteStructuredBufferUsage::AccelStructBuildInput);
                tx.bufferCreates += p.output != nullptr;
                p.dispatch = create("PathTraceRewriteSkinnedDispatch", layout.capacities[3], sizeof(PathTraceSkinnedSurfaceDispatchRecord), false);
                p.joints = create("PathTraceRewriteSkinnedJoints", layout.capacities[4], sizeof(PathTraceSkinnedJointMatrix), false);
                p.previous = create("PathTraceRewriteSkinnedPrevPos", layout.capacities[5], sizeof(PathTraceSkinnedPreviousPosition), true);
                p.records = create("PathTraceRewriteSkinnedHitRoute", layout.capacities[6], sizeof(PathTraceSkinnedHitRouteGpuRecord), false);
                p.triangles = create("PathTraceRewriteSkinnedHitRouteTriangles", layout.capacities[7], sizeof(PathTraceSkinnedHitRouteGpuTriangle), false);
                if (!p.source || !p.indexes || !p.output || !p.dispatch || !p.joints || !p.previous || !p.records || !p.triangles)
                    return reject(RtCpuRewriteSkinnedRejectReason::Allocation);
                nvrhi::BindingSetDesc skinningBindingSetDesc;
                skinningBindingSetDesc.bindings = {
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(0, p.source),
                    nvrhi::BindingSetItem::StructuredBuffer_UAV(0, p.output),
                    nvrhi::BindingSetItem::StructuredBuffer_UAV(1, p.previous),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(1, p.dispatch),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(2, p.joints),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(3, p.joints)
                };
                p.computeBinding = device->createBindingSet(skinningBindingSetDesc, m_smokeSkinnedGpuSkinningBindingLayout);
                tx.bindingCreates += p.computeBinding != nullptr;
                if (!p.computeBinding) return reject(RtCpuRewriteSkinnedRejectReason::Allocation);
                p.meshes.reserve(join.skinnedCount);
                for (uint32_t si = 0; si < join.skinnedCount; ++si)
                {
                    const auto& mesh = view->skinnedMeshes[join.skinned[si].meshIndex];
                    RtCpuRewriteSkinnedGpuSlot meshSlot;
                    if (previousMeshes[si] != UINT32_MAX)
                    {
                        meshSlot = retained.meshes[previousMeshes[si]];
                        if (!meshSlot.blas || !meshSlot.indexBuffer ||
                            meshSlot.desc.bottomLevelGeometries.size() != 1 ||
                            meshSlot.desc.bottomLevelGeometries[0].geometryType != nvrhi::rt::GeometryType::Triangles ||
                            meshSlot.desc.bottomLevelGeometries[0].geometryData.triangles.indexCount != mesh.indexCount ||
                            meshSlot.indexBuffer->getDesc().byteSize != uint64_t(mesh.indexCount) * sizeof(uint32_t))
                            return reject(RtCpuRewriteSkinnedRejectReason::Blas);
                        ++tx.blasReuses;
                    }
                    else
                    {
                        auto blasIndexBuffer = RewriteCreateStructuredBuffer(device, "PathTraceRewriteSkinnedBLASIndexes",
                            uint64_t(mesh.indexCount) * sizeof(uint32_t), sizeof(uint32_t), false,
                            RewriteStructuredBufferUsage::AccelStructBuildInput);
                        tx.bufferCreates += blasIndexBuffer != nullptr;
                        if (!blasIndexBuffer) return reject(RtCpuRewriteSkinnedRejectReason::Allocation);
                        RtSmokeBlasCreateDesc meshBlasDesc = {};
                        meshBlasDesc.device = device;
                        meshBlasDesc.vertexBuffer = p.output;
                        meshBlasDesc.indexBuffer = blasIndexBuffer;
                        meshBlasDesc.vertexCount = static_cast<int>(tx.allocationVertexCapacity);
                        meshBlasDesc.indexCount = static_cast<int>(mesh.indexCount);
                        meshBlasDesc.debugName = "PathTraceRewriteSkinnedBLAS";
                        meshBlasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::AllowUpdate | nvrhi::rt::AccelStructBuildFlags::PreferFastBuild;
                        auto result = CreateSmokeBlas(meshBlasDesc);
                        tx.blasCreates += result.accelStruct != nullptr;
                        if (!result.Succeeded()) return reject(RtCpuRewriteSkinnedRejectReason::Blas);
                        meshSlot.blas = result.accelStruct; meshSlot.indexBuffer = blasIndexBuffer;
                        meshSlot.desc = std::move(result.accelStructDesc);
                        meshSlot.allocationVertexCapacity = tx.allocationVertexCapacity;
                    }
                    // Creation queries the capacity bound. This BUILD uses the current
                    // range and addresses; future UPDATEs keep this build's exact layout.
                    auto& geometry = meshSlot.desc.bottomLevelGeometries[0].geometryData.triangles;
                    geometry.vertexBuffer = p.output;
                    geometry.vertexCount = static_cast<uint32_t>(vertexCount);
                    meshSlot.bytes = uint64_t(mesh.vertexCount) * sizeof(PathTraceSmokeVertex) + uint64_t(mesh.indexCount) * sizeof(uint32_t);
                    // Charge shared retained storage exactly once across the retiring batch.
                    // Other entries retain their output-range + private-index charge.
                    if (si == 0) meshSlot.bytes += gpuBytes - layout.capacities[2] - indexCount * sizeof(uint32_t);
                    p.meshes.push_back(std::move(meshSlot));
                }
                p.logicalBytes = gpuBytes; p.cpuBytes = layoutCpuBytes;
                p.recordCount = join.skinnedCount; p.triangleCount = static_cast<uint32_t>(indexCount / 3);
                p.sourceIndexCount = static_cast<uint32_t>(indexCount); p.outputCount = static_cast<uint32_t>(vertexCount);
            }
            if (p.meshes.size() != join.skinnedCount || !p.computeBinding || p.computeBinding->getLayout() != m_smokeSkinnedGpuSkinningBindingLayout)
                return reject(RtCpuRewriteSkinnedRejectReason::Replacement);
            uint64_t retirementLogicalBytes = 0;
            for (const auto& mesh : p.meshes)
            {
                retirementLogicalBytes += mesh.bytes;
                if (!mesh.blas || !mesh.indexBuffer || mesh.desc.bottomLevelGeometries.size() != 1)
                    return reject(RtCpuRewriteSkinnedRejectReason::Blas);
                for (const auto& geometry : mesh.desc.bottomLevelGeometries)
                    if (geometry.geometryType != nvrhi::rt::GeometryType::Triangles ||
                        geometry.geometryData.triangles.vertexOffset != 0 ||
                        !ValidateSmokeTriangleGeometryBufferRanges(geometry.geometryData.triangles))
                        return reject(RtCpuRewriteSkinnedRejectReason::Blas);
            }
            if (retirementLogicalBytes != p.logicalBytes)
                return reject(RtCpuRewriteSkinnedRejectReason::Capacity);
            tx.refresh = !tx.replace && p.usesSinceFullBuild >= kRewriteSkinnedBlasRefreshCadenceFrames;
            for (uint32_t si = 0; si < join.skinnedCount; ++si)
            {
                const auto& mesh = p.meshes[si];
                nvrhi::rt::InstanceDesc extra;
                extra.setInstanceID(firstSkinned + si).setInstanceMask(0x02)
                    .setInstanceContributionToHitGroupIndex(PT_PATH_TRACE_SBT_SKINNED_INSTANCE_CONTRIBUTION)
                    .setFlags(nvrhi::rt::InstanceFlags::TriangleCullDisable)
                    .setTransform(nvrhi::rt::c_IdentityTransform).setBLAS(mesh.blas);
                extraTlas.push_back(extra); // capacity was reserved in Prepare
            }
        }
        {
            OPTICK_EVENT("PT CPU Commit Skinned Record");
            auto& p = *tx.package;
            // No data-dependent rejection or allocation follows this boundary.
            // A later scene rejection must not take a warm hit on this slot after
            // its shared BLAS/index allocations have received candidate bytes.
            if (tx.replace && tx.blasReuses) retained.contentsValid = false;
            tx.recorded = true;
            auto upload = [&](nvrhi::IBuffer* buffer, const void* data, size_t bytes) {
                commandList->writeBuffer(buffer, data, bytes);
            };
            if (tx.replace)
            {
                tx.stableUploadBytes = layout.capacities[0] + layout.capacities[1] +
                    layout.capacities[7] + indexCount * sizeof(uint32_t);
                upload(p.source, normalSourceVertices.data(), layout.capacities[0]);
                upload(p.indexes, view->skinnedPrepared.indexes, layout.capacities[1]);
                upload(p.triangles, routes.upload.triangles.data(), layout.capacities[7]);
                for (uint32_t si = 0; si < join.skinnedCount; ++si)
                {
                    const auto& d = dispatches[si];
                    upload(p.meshes[si].indexBuffer, view->skinnedPrepared.blasIndexes + d.dynamicIndexOffset,
                        uint64_t(view->skinnedMeshes[join.skinned[si].meshIndex].indexCount) * sizeof(uint32_t));
                }
            }
            tx.exactUploadBytes = layout.capacities[3] + layout.capacities[4] + layout.capacities[6];
            if (!tx.replace && !normalSourceVertices.empty())
            {
                upload(p.source, normalSourceVertices.data(), layout.capacities[0]);
                tx.exactUploadBytes += layout.capacities[0];
            }
            upload(p.dispatch, dispatches.data(), layout.capacities[3]);
            upload(p.joints, joints.data(), layout.capacities[4]);
            upload(p.records, routes.upload.records.data(), layout.capacities[6]);
            nvrhi::ComputeState state;
            state.pipeline = m_smokeSkinnedGpuSkinningPipeline;
            state.bindings = {p.computeBinding};
            commandList->setComputeState(state);
            uint32_t maxVerts = 1;
            for (const auto& d : dispatches) maxVerts = std::max(maxVerts, d.vertexCount);
            commandList->dispatch((maxVerts + 63u) / 64u, join.skinnedCount, 1);
            commandList->setBufferState(p.output, nvrhi::ResourceStates::AccelStructBuildInput);
            commandList->commitBarriers();
            for (uint32_t si = 0; si < join.skinnedCount; ++si)
            {
                const auto& mesh = p.meshes[si];
                auto flags = mesh.desc.buildFlags;
                if (!tx.replace && !tx.refresh) flags = flags | nvrhi::rt::AccelStructBuildFlags::PerformUpdate;
                commandList->buildBottomLevelAccelStruct(mesh.blas, mesh.desc.bottomLevelGeometries.data(),
                    mesh.desc.bottomLevelGeometries.size(), flags);
                if (tx.replace || tx.refresh) ++tx.fullBuilds; else ++tx.updates;
            }
            if (tx.replace) p.layout = std::move(layout);
            tx.outcome = tx.replace ? 1u : (tx.refresh ? 3u : 2u);
        }
    }
    catch (const std::bad_alloc&)
    {
        // API allocation/device errors after recording are fatal under the existing backend policy.
        if (tx.recorded) throw;
        return reject(RtCpuRewriteSkinnedRejectReason::Capacity);
    }
    m_rewriteSkinnedLastRejectReason = 0;
    return true;
}

void PathTracePrimaryPass::FinalizeRewriteSkinnedTransaction(RtCpuRewriteSkinnedTransaction& tx)
{
    OPTICK_EVENT("PT CPU Commit Skinned Finalize");
    if (!tx.retirement.empty()) EnqueuePreflightedRewriteGpuGeometryBatch(tx.retirement);
    if (tx.zero)
    {
        for (auto& p : m_rewriteSkinnedPackages) p = RtCpuRewriteSkinnedPackage();
        for (int slot = 0; slot < 3; ++slot)
            if (slot != tx.selectedSlot) m_rewriteTlasSlots[slot].sceneBindingReceipt = {};
        m_rewriteCurrentSkinnedSlot = -1;
    }
    else
    {
        auto& p = m_rewriteSkinnedPackages[tx.selectedSlot];
        if (tx.replace) std::swap(p, tx.replacement);
        p.contentsValid = true;
        p.normalTextureSignature = tx.normalTextureSignature;
        p.lastCommittedSerial = m_rewriteTlasCommitSerial;
        p.usesSinceFullBuild = (tx.replace || tx.refresh) ? 0 : p.usesSinceFullBuild + 1;
        m_rewriteCurrentSkinnedSlot = tx.selectedSlot;
        tx.package = &p;
    }
    const auto* p = tx.zero ? nullptr : tx.package;
    m_rewriteSkinnedSourceVertexBuffer = p ? p->source : nullptr;
    m_rewriteSkinnedIndexBuffer = p ? p->indexes : nullptr;
    m_rewriteSkinnedOutputVertexBuffer = p ? p->output : nullptr;
    m_rewriteSkinnedDispatchBuffer = p ? p->dispatch : nullptr;
    m_rewriteSkinnedJointBuffer = p ? p->joints : nullptr;
    m_rewriteSkinnedPreviousPositionBuffer = p ? p->previous : nullptr;
    m_rewriteSkinnedHitRouteRecordBuffer = p ? p->records : nullptr;
    m_rewriteSkinnedHitRouteTriangleBuffer = p ? p->triangles : nullptr;
    m_rewriteSkinnedGpuSkinningBindingSet = p ? p->computeBinding : nullptr;
    m_rewriteSkinnedHitRouteRecordCount = p ? p->recordCount : 0;
    m_rewriteSkinnedHitRouteTriangleCount = p ? p->triangleCount : 0;
    m_rewriteSkinnedSourceIndexCount = p ? p->sourceIndexCount : 0;
    m_rewriteSkinnedOutputVertexCount = p ? p->outputCount : 0;
    m_rewriteSkinnedSourceIndexGeneration = p ? p->sourceGeneration : 0;
    m_rewriteSkinnedOutputStorageGeneration = p ? p->outputGeneration : 0;
    m_rewriteSkinnedGpuComputeDispatched = p != nullptr;
    m_rewriteSkinnedGpuBytes = p ? p->logicalBytes : 0;
    m_rewritePreviousSkinnedJoints.swap(tx.history);
    m_rewriteSkinnedHistoryCpuBytes = tx.historyCpuBytes;
}
