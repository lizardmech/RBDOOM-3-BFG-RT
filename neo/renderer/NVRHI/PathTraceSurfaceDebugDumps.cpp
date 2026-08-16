#include "precompiled.h"
#pragma hdrstop

#include "PathTraceSurfaceDebugDumps.h"
#include "PathTraceCVars.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceMaterialClassifier.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceTextureRegistry.h"

void ProcessSmokeCrosshairMaterialDump(const viewDef_t* viewDef)
{
    if (r_pathTracingCrosshairMaterialDump.GetInteger() == 0)
    {
        return;
    }
    r_pathTracingCrosshairMaterialDump.SetInteger(0);

    idVec3 hitPoint = vec3_origin;
    int surfaceIndex = -1;
    int triangleIndex = -1;
    if (!FindCenterCameraRayAnchor(viewDef, hitPoint, surfaceIndex, triangleIndex))
    {
        common->Printf("PathTracePrimaryPass: crosshair material dump found no center-ray hit\n");
        return;
    }

    if (!viewDef || surfaceIndex < 0 || surfaceIndex >= viewDef->numDrawSurfs)
    {
        common->Printf("PathTracePrimaryPass: crosshair material dump invalid hit surface=%d triangle=%d\n", surfaceIndex, triangleIndex);
        return;
    }

    const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
    const srfTriangles_t* tri = nullptr;
    if (!ValidateSmokeDrawSurface(viewDef, drawSurf, tri, nullptr) || !drawSurf || !drawSurf->material || !tri)
    {
        common->Printf("PathTracePrimaryPass: crosshair material dump failed validation surface=%d triangle=%d\n", surfaceIndex, triangleIndex);
        return;
    }

    const idMaterial* material = drawSurf->material;
    const uint32_t materialId = SmokeMaterialId(material);
    idBounds worldBounds;
    worldBounds.Clear();
    if (drawSurf->space)
    {
        for (int corner = 0; corner < 8; ++corner)
        {
            const idVec3 localPoint(
                tri->bounds[(corner >> 0) & 1].x,
                tri->bounds[(corner >> 1) & 1].y,
                tri->bounds[(corner >> 2) & 1].z);
            idVec3 worldPoint;
            R_LocalPointToGlobal(drawSurf->space->modelMatrix, localPoint, worldPoint);
            worldBounds.AddPoint(worldPoint);
        }
    }
    const idVec3 localSize = tri->bounds[1] - tri->bounds[0];
    const idVec3 worldSize = worldBounds.IsCleared()
        ? vec3_zero : worldBounds[1] - worldBounds[0];
    const int entityIndex = drawSurf->space && drawSurf->space->entityDef
        ? drawSurf->space->entityDef->index : -1;
    common->Printf("PathTracePrimaryPass: crosshair material dump surface=%d triangle=%d entity=%d point=(%.2f %.2f %.2f) material='%s' id=%u sort=%.2f coverage=%d surfaceFlags=0x%08x cull=%d deform=%d stages=%d registers=%d verts=%d indexes=%d boundsLocal=(%.2f %.2f %.2f) boundsWorld=(%.2f %.2f %.2f)\n",
        surfaceIndex,
        triangleIndex,
        entityIndex,
        hitPoint.x,
        hitPoint.y,
        hitPoint.z,
        material->GetName(),
        materialId,
        material->GetSort(),
        static_cast<int>(material->Coverage()),
        material->GetSurfaceFlags(),
        static_cast<int>(material->GetCullType()),
        static_cast<int>(material->Deform()),
        material->GetNumStages(),
        material->GetNumRegisters(),
        tri->numVerts,
        tri->numIndexes,
        localSize.x,
        localSize.y,
        localSize.z,
        worldSize.x,
        worldSize.y,
        worldSize.z);

    // The center ray identifies one draw surface, not necessarily the surface
    // that contributes emission for a multi-surface rigid model.  Enumerate
    // every unique material carried by the same entity so a sibling lens,
    // decal, or trim surface cannot hide behind the struck material ID.
    if (entityIndex >= 0)
    {
        uint32_t entityMaterialIds[32] = {};
        int entityMaterialCount = 0;
        int entitySurfaceCount = 0;
        for (int entitySurfaceIndex = 0;
             entitySurfaceIndex < viewDef->numDrawSurfs;
             ++entitySurfaceIndex)
        {
            const drawSurf_t* entitySurface =
                viewDef->drawSurfs[entitySurfaceIndex];
            const int candidateEntityIndex =
                entitySurface && entitySurface->space &&
                    entitySurface->space->entityDef
                    ? entitySurface->space->entityDef->index
                    : -1;
            if (!entitySurface || !entitySurface->material ||
                candidateEntityIndex != entityIndex)
            {
                continue;
            }

            ++entitySurfaceCount;
            const uint32_t candidateMaterialId =
                SmokeMaterialId(entitySurface->material);
            bool duplicateMaterial = false;
            for (int knownIndex = 0;
                 knownIndex < entityMaterialCount;
                 ++knownIndex)
            {
                if (entityMaterialIds[knownIndex] == candidateMaterialId)
                {
                    duplicateMaterial = true;
                    break;
                }
            }
            if (duplicateMaterial || entityMaterialCount >= 32)
            {
                continue;
            }
            entityMaterialIds[entityMaterialCount++] =
                candidateMaterialId;

            const RtMaterialRecord* candidateRecord =
                FindPathTraceMaterialRecord(candidateMaterialId);
            const RtSmokeMaterialTextureInfo* candidateTextureInfo =
                FindSmokeMaterialTextureInfo(candidateMaterialId);
            common->Printf(
                "PathTracePrimaryPass: crosshair entity material entity=%d surface=%d ordinal=%d id=%u name='%s' classifier(valid/emissiveIntent/emissiveImage)=%d/%d/%d emissiveImage='%s' rtMetadata(found/diffuse/emissive)=%d/'%s'/'%s' suppress(proposal/surface)=%d/%d\n",
                entityIndex,
                entitySurfaceIndex,
                entityMaterialCount - 1,
                candidateMaterialId,
                entitySurface->material->GetName(),
                candidateRecord && candidateRecord->valid ? 1 : 0,
                candidateRecord && candidateRecord->valid &&
                        candidateRecord->emissiveIntent
                    ? 1
                    : 0,
                candidateRecord && candidateRecord->valid &&
                        candidateRecord->hasEmissiveImage
                    ? 1
                    : 0,
                candidateRecord && candidateRecord->valid
                    ? candidateRecord->emissiveImageName.c_str()
                    : "",
                candidateTextureInfo ? 1 : 0,
                candidateTextureInfo
                    ? candidateTextureInfo->diffuseImageName.c_str()
                    : "",
                candidateTextureInfo
                    ? candidateTextureInfo->emissiveImageName.c_str()
                    : "",
                candidateMaterialId == static_cast<uint32_t>(Max(
                    0,
                    r_pathTracingEmissiveProposalSuppressMaterialId.GetInteger()))
                    ? 1
                    : 0,
                candidateMaterialId == static_cast<uint32_t>(Max(
                    0,
                    r_pathTracingEmissiveSurfaceSuppressMaterialId.GetInteger()))
                    ? 1
                    : 0);
        }
        common->Printf(
            "PathTracePrimaryPass: crosshair entity material summary entity=%d surfaces=%d unique=%d capped=%d\n",
            entityIndex,
            entitySurfaceCount,
            entityMaterialCount,
            entityMaterialCount >= 32 ? 1 : 0);
    }

    const RtSmokeMaterialTextureInfo* textureInfo =
        FindSmokeMaterialTextureInfo(materialId);
    if (textureInfo)
    {
        common->Printf("PathTracePrimaryPass: crosshair material RT metadata id=%u hardwareOpaque=%d coverage=%d alphaTest=%d alphaImage=%d/'%s' alphaHandle/safe=%d/%d alphaCutoff=%.3f alphaModes(luma/dark/magenta)=%d/%d/%d modifiers(add/filter/detail/liquid/glass)=0x%02x proposalSuppressed=%d surfaceSuppressed=%d diffuse='%s' normal='%s' alphaReason='%s'\n",
            materialId,
            textureInfo->hardwareOpaqueGeometry ? 1 : 0,
            static_cast<int>(textureInfo->coverage),
            textureInfo->hasAlphaTest ? 1 : 0,
            textureInfo->hasAlphaImage ? 1 : 0,
            textureInfo->alphaImageName.c_str(),
            textureInfo->hasAlphaTextureHandle ? 1 : 0,
            textureInfo->hasSafeAlphaTexture ? 1 : 0,
            textureInfo->alphaCutoff,
            textureInfo->alphaFromDiffuseLuma ? 1 : 0,
            textureInfo->alphaFromDiffuseDarkKey ? 1 : 0,
            textureInfo->alphaFromDiffuseMagentaKey ? 1 : 0,
            (textureInfo->additiveDecal ? 0x01 : 0) |
                (textureInfo->filterDecal ? 0x02 : 0) |
                (textureInfo->detailDecal ? 0x04 : 0) |
                (textureInfo->liquidFilmCandidate ? 0x08 : 0) |
                (textureInfo->portalWindowFallback ? 0x10 : 0) |
                (textureInfo->objectGlassFallback ? 0x20 : 0),
            materialId == static_cast<uint32_t>(Max(0,
                r_pathTracingEmissiveProposalSuppressMaterialId.GetInteger()))
                ? 1 : 0,
            materialId == static_cast<uint32_t>(Max(0,
                r_pathTracingEmissiveSurfaceSuppressMaterialId.GetInteger()))
                ? 1 : 0,
            textureInfo->diffuseImageName.c_str(),
            textureInfo->normalImageName.c_str(),
            textureInfo->alphaReason.c_str());
    }
    else
    {
        common->Printf("PathTracePrimaryPass: crosshair material RT metadata id=%u found=0\n", materialId);
    }

    const RtMaterialRecord* record = FindPathTraceMaterialRecord(materialId);
    if (!record || !record->valid)
    {
        common->Printf("PathTracePrimaryPass: crosshair material classifier id=%u found=0\n", materialId);
        return;
    }

    common->Printf("PathTracePrimaryPass: crosshair material classifier id=%u found=1 route=%s routeReason=%s class=%s confidence=%s alphaTest=%d emissiveIntent=%d emissiveImage=%d/'%s' stageCounts(total/ambient/effect/additive/filter/alphaBlend/opaque/alphaClip/unknown)=%d/%d/%d/%d/%d/%d/%d/%d/%d dynamic(materialRegs/condition/color/alpha/alphaTest/texMatrix/image/cinematic/program)=%d/%d/%d/%d/%d/%d/%d/%d/%d\n",
        materialId,
        RtMaterialBsdfRouteName(record->route),
        RtMaterialBsdfRouteReasonName(record->routeReason),
        RtMaterialSurfaceClassName(record->surfaceClass),
        RtMaterialClassConfidenceName(record->surfaceClassConfidence),
        record->alphaTested ? 1 : 0,
        record->emissiveIntent ? 1 : 0,
        record->hasEmissiveImage ? 1 : 0,
        record->emissiveImageName.c_str(),
        record->stageFacts.stageCount,
        record->stageFacts.ambientStages,
        record->stageFacts.effectStages,
        record->stageFacts.additiveBlendStages,
        record->stageFacts.filterBlendStages,
        record->stageFacts.alphaBlendStages,
        record->stageFacts.opaqueReplaceStages,
        record->stageFacts.authoredAlphaClipStages,
        record->stageFacts.unknownCompositingStages,
        record->dynamicFacts.materialUsesRuntimeRegisters ? 1 : 0,
        record->dynamicFacts.conditionRegisterStages,
        record->dynamicFacts.colorRegisterStages,
        record->dynamicFacts.alphaRegisterStages,
        record->dynamicFacts.alphaTestRegisterStages,
        record->dynamicFacts.textureMatrixRegisterStages,
        record->dynamicFacts.dynamicImageStages,
        record->dynamicFacts.cinematicStages,
        record->dynamicFacts.customProgramStages);

    const int registerCount = material->GetNumRegisters();
    const float* registers = drawSurf->shaderRegisters;
    const auto registerValue = [registers, registerCount](int registerIndex, float fallback)
    {
        return registers && registerIndex >= 0 && registerIndex < registerCount
            ? registers[registerIndex]
            : fallback;
    };
    for (const RtMaterialCompositingStageFact& stage : record->compositingStages)
    {
        common->Printf("PathTracePrimaryPass: crosshair material stage id=%u index=%d lighting=%d op=%s image='%s' src=0x%llx dst=0x%llx condition(reg/value/dynamic)=%d/%.3f/%d color=(%.3f %.3f %.3f %.3f) alphaTest(enabled/ignored/reg/value)=%d/%d/%d/%.3f texMatrix=%d texgen=%d dynamicImage=%d frames=%d vertexColor=%d programs(v/f/g)=%d/%d/%d\n",
            materialId,
            stage.stageIndex,
            static_cast<int>(stage.lighting),
            RtMaterialCompositingOpName(stage.operation),
            stage.imageName.c_str(),
            static_cast<unsigned long long>(stage.srcBlendBits),
            static_cast<unsigned long long>(stage.dstBlendBits),
            stage.conditionRegister,
            registerValue(stage.conditionRegister, 1.0f),
            stage.conditionIsDynamic ? 1 : 0,
            registerValue(stage.colorRegisters[0], 1.0f),
            registerValue(stage.colorRegisters[1], 1.0f),
            registerValue(stage.colorRegisters[2], 1.0f),
            registerValue(stage.colorRegisters[3], 1.0f),
            stage.hasAlphaTest ? 1 : 0,
            stage.ignoreAlphaTest ? 1 : 0,
            stage.alphaTestRegister,
            registerValue(stage.alphaTestRegister, -1.0f),
            stage.hasTextureMatrix ? 1 : 0,
            static_cast<int>(stage.texgen),
            static_cast<int>(stage.dynamicImage),
            stage.dynamicFrameCount,
            static_cast<int>(stage.vertexColor),
            stage.vertexProgram,
            stage.fragmentProgram,
            stage.glslProgram);
    }
}

void ProcessSmokeCrosshairZeroRoughnessToggle(const viewDef_t* viewDef)
{
    if (!ConsumeSmokeCrosshairZeroRoughnessToggleRequest())
    {
        return;
    }

    idVec3 hitPoint = vec3_origin;
    int surfaceIndex = -1;
    int triangleIndex = -1;
    if (!FindCenterCameraRayAnchor(viewDef, hitPoint, surfaceIndex, triangleIndex))
    {
        common->Printf("PathTracePrimaryPass: crosshair zero-roughness toggle found no center-ray hit\n");
        return;
    }

    if (!viewDef || surfaceIndex < 0 || surfaceIndex >= viewDef->numDrawSurfs)
    {
        common->Printf("PathTracePrimaryPass: crosshair zero-roughness toggle invalid hit surface=%d triangle=%d\n", surfaceIndex, triangleIndex);
        return;
    }

    const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
    const srfTriangles_t* tri = nullptr;
    if (!ValidateSmokeDrawSurface(viewDef, drawSurf, tri, nullptr) || !drawSurf || !drawSurf->material || !tri)
    {
        common->Printf("PathTracePrimaryPass: crosshair zero-roughness toggle failed validation surface=%d triangle=%d\n", surfaceIndex, triangleIndex);
        return;
    }

    const idMaterial* material = drawSurf->material;
    const uint32_t materialId = SmokeMaterialId(material);
    const bool enabled = ToggleSmokeMaterialZeroRoughnessOverride(materialId, material->GetName());
    common->Printf("PathTracePrimaryPass: crosshair zero-roughness toggle %s surface=%d triangle=%d point=(%.2f %.2f %.2f) material='%s' id=%u\n",
        enabled ? "enabled" : "disabled",
        surfaceIndex,
        triangleIndex,
        hitPoint.x,
        hitPoint.y,
        hitPoint.z,
        material->GetName(),
        materialId);
}

void ProcessSmokeCrosshairFullMetalToggle(const viewDef_t* viewDef)
{
    if (!ConsumeSmokeCrosshairFullMetalToggleRequest())
    {
        return;
    }

    idVec3 hitPoint = vec3_origin;
    int surfaceIndex = -1;
    int triangleIndex = -1;
    if (!FindCenterCameraRayAnchor(viewDef, hitPoint, surfaceIndex, triangleIndex))
    {
        common->Printf("PathTracePrimaryPass: crosshair full-metal toggle found no center-ray hit\n");
        return;
    }

    if (!viewDef || surfaceIndex < 0 || surfaceIndex >= viewDef->numDrawSurfs)
    {
        common->Printf("PathTracePrimaryPass: crosshair full-metal toggle invalid hit surface=%d triangle=%d\n", surfaceIndex, triangleIndex);
        return;
    }

    const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
    const srfTriangles_t* tri = nullptr;
    if (!ValidateSmokeDrawSurface(viewDef, drawSurf, tri, nullptr) || !drawSurf || !drawSurf->material || !tri)
    {
        common->Printf("PathTracePrimaryPass: crosshair full-metal toggle failed validation surface=%d triangle=%d\n", surfaceIndex, triangleIndex);
        return;
    }

    const idMaterial* material = drawSurf->material;
    const uint32_t materialId = SmokeMaterialId(material);
    const bool enabled = ToggleSmokeMaterialFullMetalOverride(materialId, material->GetName());
    common->Printf("PathTracePrimaryPass: crosshair full-metal toggle %s surface=%d triangle=%d point=(%.2f %.2f %.2f) material='%s' id=%u\n",
        enabled ? "enabled" : "disabled",
        surfaceIndex,
        triangleIndex,
        hitPoint.x,
        hitPoint.y,
        hitPoint.z,
        material->GetName(),
        materialId);
}
