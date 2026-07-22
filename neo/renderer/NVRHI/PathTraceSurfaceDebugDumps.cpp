#include "precompiled.h"
#pragma hdrstop

#include "PathTraceSurfaceDebugDumps.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceSceneCapture.h"

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
