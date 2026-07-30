#include "precompiled.h"
#pragma hdrstop

#include "PathTraceSurfaceDebugDumps.h"
#include "PathTraceCVars.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceMaterialClassifier.h"
#include "PathTraceSceneCapture.h"

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
    common->Printf("PathTracePrimaryPass: crosshair material dump surface=%d triangle=%d point=(%.2f %.2f %.2f) material='%s' id=%u sort=%.2f coverage=%d surfaceFlags=0x%08x cull=%d deform=%d stages=%d registers=%d verts=%d indexes=%d\n",
        surfaceIndex,
        triangleIndex,
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
        tri->numIndexes);

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
