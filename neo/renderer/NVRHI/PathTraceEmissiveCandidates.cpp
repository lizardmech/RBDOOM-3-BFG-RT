#include "precompiled.h"
#pragma hdrstop

#include "PathTraceEmissiveCandidates.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceGeometryUniverse.h"
#include "PathTraceMaterialUniverse.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceSurfaceClassification.h"
#include "PathTraceTextureRegistry.h"
#include "../RenderCommon.h"

#include <algorithm>
#include <unordered_map>

namespace {

uint64 HashSmokeEmissiveIdentityValue(uint64 hash, uint64 value)
{
    hash ^= value;
    hash *= 1099511628211ull;
    return hash;
}

uint64 BuildSmokeEmissiveTriangleIdentity(uint32_t materialId, uint32_t instanceId, uint32_t primitiveIndex, uint32_t materialIndex, uint32_t triangleClassAndFlags)
{
    uint64 hash = 1469598103934665603ull;
    hash = HashSmokeEmissiveIdentityValue(hash, materialId);
    hash = HashSmokeEmissiveIdentityValue(hash, instanceId);
    hash = HashSmokeEmissiveIdentityValue(hash, primitiveIndex);
    hash = HashSmokeEmissiveIdentityValue(hash, materialIndex);
    hash = HashSmokeEmissiveIdentityValue(hash, triangleClassAndFlags);
    return hash;
}

uint64 BuildSmokeEmissiveTriangleIdentity64(uint32_t materialId, uint64 instanceId, uint32_t primitiveIndex, uint32_t materialIndex, uint32_t triangleClassAndFlags)
{
    uint64 hash = 1469598103934665603ull;
    hash = HashSmokeEmissiveIdentityValue(hash, materialId);
    hash = HashSmokeEmissiveIdentityValue(hash, instanceId & 0xffffffffull);
    hash = HashSmokeEmissiveIdentityValue(hash, instanceId >> 32);
    hash = HashSmokeEmissiveIdentityValue(hash, primitiveIndex);
    hash = HashSmokeEmissiveIdentityValue(hash, materialIndex);
    hash = HashSmokeEmissiveIdentityValue(hash, triangleClassAndFlags);
    return hash;
}

uint32_t FindSmokeMaterialTableIndexForId(const std::vector<uint32_t>& materialIds, uint32_t materialId)
{
    for (int materialIndex = 0; materialIndex < static_cast<int>(materialIds.size()); ++materialIndex)
    {
        if (materialIds[materialIndex] == materialId)
        {
            return static_cast<uint32_t>(materialIndex);
        }
    }
    return UINT32_MAX;
}

idVec3 TransformSmokeRoutePoint(const float objectToWorld[16], const idVec3& point)
{
    return idVec3(
        objectToWorld[0] * point.x + objectToWorld[4] * point.y + objectToWorld[8] * point.z + objectToWorld[12],
        objectToWorld[1] * point.x + objectToWorld[5] * point.y + objectToWorld[9] * point.z + objectToWorld[13],
        objectToWorld[2] * point.x + objectToWorld[6] * point.y + objectToWorld[10] * point.z + objectToWorld[14]);
}

}

std::vector<PathTraceSmokeMaterial> BuildSmokeEmissiveMaterialViews(
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeMaterial>& frameMaterials,
    uint32_t emissiveMaterialFlag);

float SmokeMaterialEmissiveLuminance(const PathTraceSmokeMaterial& material)
{
    const float r = Max(0.0f, material.emissiveColor[0]);
    const float g = Max(0.0f, material.emissiveColor[1]);
    const float b = Max(0.0f, material.emissiveColor[2]);
    return r * 0.2126f + g * 0.7152f + b * 0.0722f;
}

void AppendSmokeEmissiveInventoryForGeometry(
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeMaterial>& materials,
    const std::vector<PathTraceSmokeVertex>& vertices,
    const std::vector<uint32_t>& indexes,
    const std::vector<uint32_t>& triangleClasses,
    const std::vector<uint32_t>& triangleMaterialIndexes,
    uint32_t instanceId,
    const std::vector<uint32_t>* triangleInstanceIds,
    const std::vector<uint32_t>* triangleIdentityIds,
    uint32_t emissiveMaterialFlag,
    uint32_t triangleClassMask,
    uint32_t skinnedSurfaceClassId,
    int maxRecords,
    std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles,
    RtSmokeEmissiveInventoryStats& stats)
{
    OPTICK_EVENT("PT Emissive Append Geometry");

    const int triangleCount = Min(static_cast<int>(triangleMaterialIndexes.size()), static_cast<int>(indexes.size() / 3));
    for (int primitiveIndex = 0; primitiveIndex < triangleCount; ++primitiveIndex)
    {
        const uint32_t materialIndex = triangleMaterialIndexes[primitiveIndex];
        if (materialIndex >= materials.size() || materialIndex >= materialIds.size())
        {
            ++stats.skippedInvalidMaterialTriangles;
            continue;
        }

        const uint32_t materialId = materialIds[materialIndex];
        const PathTraceSmokeMaterial& material = materials[materialIndex];
        if ((material.flags & emissiveMaterialFlag) == 0)
        {
            ++stats.skippedNonEmissiveMaterialTriangles;
            continue;
        }

        const uint32_t triangleClassAndFlags = primitiveIndex < static_cast<int>(triangleClasses.size()) ? triangleClasses[primitiveIndex] : 0u;
        const uint32_t surfaceClass = triangleClassAndFlags & triangleClassMask;
        if ((triangleClassAndFlags & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) != 0u)
        {
            ++stats.skippedRuntimeInactiveTriangles;
            continue;
        }
        if (surfaceClass == skinnedSurfaceClassId)
        {
            ++stats.skippedSkinnedTriangles;
            continue;
        }

        const uint32_t identityInstanceId =
            (triangleInstanceIds && primitiveIndex < static_cast<int>(triangleInstanceIds->size()))
                ? (*triangleInstanceIds)[primitiveIndex]
                : instanceId;
        const uint32_t identityPrimitiveIndex =
            (triangleIdentityIds && primitiveIndex < static_cast<int>(triangleIdentityIds->size()))
                ? (*triangleIdentityIds)[primitiveIndex]
                : static_cast<uint32_t>(primitiveIndex);
        ++stats.totalTriangles;
        if (instanceId == 0)
        {
            ++stats.staticTriangles;
        }
        else
        {
            ++stats.dynamicTriangles;
        }
        if (std::find(stats.materialIndexes.begin(), stats.materialIndexes.end(), materialIndex) == stats.materialIndexes.end())
        {
            stats.materialIndexes.push_back(materialIndex);
        }

        const int indexOffset = primitiveIndex * 3;
        const uint32_t i0 = indexes[indexOffset + 0];
        const uint32_t i1 = indexes[indexOffset + 1];
        const uint32_t i2 = indexes[indexOffset + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
        {
            ++stats.skippedInvalidMaterialTriangles;
            continue;
        }

        const idVec3 p0 = SmokeVertexPosition(vertices[i0]);
        const idVec3 p1 = SmokeVertexPosition(vertices[i1]);
        const idVec3 p2 = SmokeVertexPosition(vertices[i2]);
        const idVec2 uv0 = SmokeVertexTexCoord(vertices[i0]);
        const idVec2 uv1 = SmokeVertexTexCoord(vertices[i1]);
        const idVec2 uv2 = SmokeVertexTexCoord(vertices[i2]);
        const idVec3 edge01 = p1 - p0;
        const idVec3 edge02 = p2 - p0;
        idVec3 areaNormal = edge01.Cross(edge02);
        const float doubleArea = areaNormal.Length();
        if (doubleArea <= 1.0e-6f)
        {
            ++stats.zeroAreaTriangles;
            continue;
        }

        const float area = doubleArea * 0.5f;
        areaNormal *= 1.0f / doubleArea;
        const float luminance = SmokeMaterialEmissiveLuminance(material);
        const idVec3 estimatedRadiance(
            Max(0.0f, material.emissiveColor[0]),
            Max(0.0f, material.emissiveColor[1]),
            Max(0.0f, material.emissiveColor[2]));
        const float sampleWeight = area * luminance;
        stats.totalArea += area;
        stats.totalWeightedLuminance += sampleWeight;

        if (static_cast<int>(emissiveTriangles.size()) >= maxRecords)
        {
            ++stats.cappedTriangles;
            continue;
        }

        PathTraceSmokeEmissiveTriangle record = {};
        const idVec3 center = (p0 + p1 + p2) * (1.0f / 3.0f);
        record.centerAndArea[0] = center.x;
        record.centerAndArea[1] = center.y;
        record.centerAndArea[2] = center.z;
        record.centerAndArea[3] = area;
        record.normalAndLuminance[0] = areaNormal.x;
        record.normalAndLuminance[1] = areaNormal.y;
        record.normalAndLuminance[2] = areaNormal.z;
        record.normalAndLuminance[3] = luminance;
        record.uvBounds[0] = Min(uv0.x, Min(uv1.x, uv2.x));
        record.uvBounds[1] = Min(uv0.y, Min(uv1.y, uv2.y));
        record.uvBounds[2] = Max(uv0.x, Max(uv1.x, uv2.x));
        record.uvBounds[3] = Max(uv0.y, Max(uv1.y, uv2.y));
        record.centroidUvAndWeight[0] = (uv0.x + uv1.x + uv2.x) * (1.0f / 3.0f);
        record.centroidUvAndWeight[1] = (uv0.y + uv1.y + uv2.y) * (1.0f / 3.0f);
        record.centroidUvAndWeight[2] = sampleWeight;
        record.centroidUvAndWeight[3] = 0.0f;
        record.estimatedRadianceAndLuminance[0] = estimatedRadiance.x;
        record.estimatedRadianceAndLuminance[1] = estimatedRadiance.y;
        record.estimatedRadianceAndLuminance[2] = estimatedRadiance.z;
        record.estimatedRadianceAndLuminance[3] = luminance;
        record.sampleWeightAndPdf[0] = sampleWeight;
        record.sampleWeightAndPdf[1] = 0.0f;
        record.sampleWeightAndPdf[2] = area;
        record.sampleWeightAndPdf[3] = 0.0f;
        record.materialIndex = materialIndex;
        record.instanceId = instanceId;
        record.primitiveIndex = static_cast<uint32_t>(primitiveIndex);
        record.flags = material.flags;
        record.emissiveTextureIndex = material.emissiveTextureIndex;
        record.emissiveTextureWidth = material.emissiveTextureWidth;
        record.emissiveTextureHeight = material.emissiveTextureHeight;
        record.materialId = materialId;
        const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(materialId, materialIndex);
        record.universeMaterialIndex = GetSmokeMaterialUniverseFacts(materialId, info).universeIndex;
        const uint64 identityHash = BuildSmokeEmissiveTriangleIdentity(materialId, identityInstanceId, identityPrimitiveIndex, materialIndex, triangleClassAndFlags);
        record.identityHashLo = static_cast<uint32_t>(identityHash & 0xffffffffu);
        record.identityHashHi = static_cast<uint32_t>(identityHash >> 32);
        record.padding0 = triangleClassAndFlags;
        emissiveTriangles.push_back(record);
    }

    stats.capturedTriangles = static_cast<int>(emissiveTriangles.size());
    stats.uniqueMaterials = static_cast<int>(stats.materialIndexes.size());
}

void FinalizeSmokeEmissiveTriangleSamplingFields(std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles, const RtSmokeEmissiveInventoryStats& stats)
{
    OPTICK_EVENT("PT Emissive Sampling Fields");

    const float inverseTotalWeightedLuminance = stats.totalWeightedLuminance > 1.0e-8f ? 1.0f / stats.totalWeightedLuminance : 0.0f;
    const float inverseTotalArea = stats.totalArea > 1.0e-8f ? 1.0f / stats.totalArea : 0.0f;
    for (PathTraceSmokeEmissiveTriangle& record : emissiveTriangles)
    {
        record.sampleWeightAndPdf[1] = record.sampleWeightAndPdf[0] * inverseTotalWeightedLuminance;
        record.sampleWeightAndPdf[3] = record.centerAndArea[3] * inverseTotalArea;
        record.centroidUvAndWeight[3] = record.sampleWeightAndPdf[1];
    }
}

RtSmokeEmissiveDistributionBuild BuildSmokeEmissiveDistribution(const std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles)
{
    OPTICK_EVENT("PT Emissive Distribution");

    RtSmokeEmissiveDistributionBuild build;
    build.entries.reserve(emissiveTriangles.size());

    float cumulativePdf = 0.0f;
    float fallbackWeight = -1.0f;
    uint32_t fallbackIndex = emissiveTriangles.empty() ? UINT32_MAX : 0u;
    for (size_t triangleIndex = 0; triangleIndex < emissiveTriangles.size(); ++triangleIndex)
    {
        const PathTraceSmokeEmissiveTriangle& triangle = emissiveTriangles[triangleIndex];
        const float weight = Max(triangle.sampleWeightAndPdf[0], 0.0f);
        if (weight > fallbackWeight)
        {
            fallbackWeight = weight;
            fallbackIndex = static_cast<uint32_t>(triangleIndex);
        }

        const float pdf = Max(triangle.sampleWeightAndPdf[1], 0.0f);
        if (pdf <= 0.0f)
        {
            build.zeroPdfSkipped++;
            continue;
        }

        cumulativePdf += pdf;
        PathTraceEmissiveDistributionEntry entry;
        entry.emissiveTriangleIndex = static_cast<uint32_t>(triangleIndex);
        entry.cumulativePdf = cumulativePdf;
        entry.weight = weight;
        build.entries.push_back(entry);
    }

    build.fallbackIndex = fallbackIndex;
    build.fallbackWeight = Max(fallbackWeight, 0.0f);
    build.totalPdf = cumulativePdf;
    build.valid = cumulativePdf > 1.0e-8f && !build.entries.empty();
    if (build.valid)
    {
        const float inverseTotalPdf = 1.0f / cumulativePdf;
        for (PathTraceEmissiveDistributionEntry& entry : build.entries)
        {
            entry.cumulativePdf *= inverseTotalPdf;
        }
        build.entries.back().cumulativePdf = 1.0f;
    }
    else
    {
        build.entries.clear();
    }

    return build;
}

PtSkinnedEmissiveAuditInventory BuildSmokeCanonicalSkinnedEmissiveAuditInventory(
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeMaterial>& materials,
    const std::vector<PathTraceSmokeVertex>& currentVertices,
    const std::vector<PathTraceSkinnedPreviousPosition>& previousPositions,
    const std::vector<PtSkinnedEmissiveAuditTriangle>& triangles,
    uint32_t emissiveMaterialFlag,
    int maxRecords)
{
    PtSkinnedEmissiveAuditInventory inventory;
    inventory.inputTriangles = triangles.size();
    maxRecords = Max(1, maxRecords);

    auto appendRecord =
        [&](const PtSkinnedEmissiveAuditTriangle& source,
            uint32_t sourceTriangleIndex,
            const PathTraceSmokeMaterial& material,
            bool previous,
            std::vector<PathTraceSmokeEmissiveTriangle>& destination)
        {
            idVec3 positions[3];
            idVec2 texCoords[3];
            for (int corner = 0; corner < 3; ++corner)
            {
                const uint32_t currentIndex =
                    source.currentVertexIndexes[corner];
                if (currentIndex >= currentVertices.size())
                {
                    ++inventory.invalidTriangles;
                    return;
                }
                texCoords[corner] =
                    SmokeVertexTexCoord(currentVertices[currentIndex]);
                if (previous)
                {
                    const uint32_t previousIndex =
                        source.previousPositionIndexes[corner];
                    if (previousIndex >= previousPositions.size())
                    {
                        ++inventory.invalidTriangles;
                        return;
                    }
                    const float* previousPosition =
                        previousPositions[previousIndex].
                            previousPosition;
                    positions[corner] = idVec3(
                        previousPosition[0],
                        previousPosition[1],
                        previousPosition[2]);
                }
                else
                {
                    positions[corner] =
                        SmokeVertexPosition(
                            currentVertices[currentIndex]);
                }
            }

            const idVec3 edge01 = positions[1] - positions[0];
            const idVec3 edge02 = positions[2] - positions[0];
            idVec3 areaNormal = edge01.Cross(edge02);
            const float doubleArea = areaNormal.Length();
            if (doubleArea <= 1.0e-6f)
            {
                if (previous)
                {
                    ++inventory.zeroAreaPreviousTriangles;
                }
                else
                {
                    ++inventory.zeroAreaCurrentTriangles;
                }
                return;
            }
            if (static_cast<int>(destination.size()) >= maxRecords)
            {
                return;
            }

            const float area = doubleArea * 0.5f;
            areaNormal *= 1.0f / doubleArea;
            const float luminance =
                SmokeMaterialEmissiveLuminance(material);
            const float sampleWeight = area * luminance;
            const idVec3 center =
                (positions[0] + positions[1] + positions[2]) *
                (1.0f / 3.0f);

            PathTraceSmokeEmissiveTriangle record = {};
            record.centerAndArea[0] = center.x;
            record.centerAndArea[1] = center.y;
            record.centerAndArea[2] = center.z;
            record.centerAndArea[3] = area;
            record.normalAndLuminance[0] = areaNormal.x;
            record.normalAndLuminance[1] = areaNormal.y;
            record.normalAndLuminance[2] = areaNormal.z;
            record.normalAndLuminance[3] = luminance;
            record.uvBounds[0] =
                Min(texCoords[0].x,
                    Min(texCoords[1].x, texCoords[2].x));
            record.uvBounds[1] =
                Min(texCoords[0].y,
                    Min(texCoords[1].y, texCoords[2].y));
            record.uvBounds[2] =
                Max(texCoords[0].x,
                    Max(texCoords[1].x, texCoords[2].x));
            record.uvBounds[3] =
                Max(texCoords[0].y,
                    Max(texCoords[1].y, texCoords[2].y));
            record.centroidUvAndWeight[0] =
                (texCoords[0].x + texCoords[1].x +
                    texCoords[2].x) *
                (1.0f / 3.0f);
            record.centroidUvAndWeight[1] =
                (texCoords[0].y + texCoords[1].y +
                    texCoords[2].y) *
                (1.0f / 3.0f);
            record.centroidUvAndWeight[2] = sampleWeight;
            record.estimatedRadianceAndLuminance[0] =
                Max(0.0f, material.emissiveColor[0]);
            record.estimatedRadianceAndLuminance[1] =
                Max(0.0f, material.emissiveColor[1]);
            record.estimatedRadianceAndLuminance[2] =
                Max(0.0f, material.emissiveColor[2]);
            record.estimatedRadianceAndLuminance[3] =
                luminance;
            record.sampleWeightAndPdf[0] = sampleWeight;
            record.sampleWeightAndPdf[2] = area;
            record.materialIndex = source.materialIndex;
            record.instanceId = source.instanceId;
            record.primitiveIndex = source.primitiveIndex;
            record.flags = material.flags;
            record.emissiveTextureIndex =
                material.emissiveTextureIndex;
            record.emissiveTextureWidth =
                material.emissiveTextureWidth;
            record.emissiveTextureHeight =
                material.emissiveTextureHeight;
            record.materialId = source.materialId;
            const RtSmokeMaterialTextureInfo info =
                ResolveSmokeMaterialTextureInfo(
                    source.materialId,
                    source.materialIndex);
            record.universeMaterialIndex =
                GetSmokeMaterialUniverseFacts(
                    source.materialId,
                    info).universeIndex;
            record.identityHashLo =
                static_cast<uint32_t>(
                    source.identityHash & 0xffffffffu);
            record.identityHashHi =
                static_cast<uint32_t>(
                    source.identityHash >> 32);
            record.padding0 = source.triangleClassAndFlags;
            destination.push_back(record);
            if (previous)
            {
                inventory.previousSourceTriangleIndexes.
                    push_back(sourceTriangleIndex);
            }
            else
            {
                inventory.currentSourceTriangleIndexes.
                    push_back(sourceTriangleIndex);
            }
        };

    for (uint32_t sourceTriangleIndex = 0;
         sourceTriangleIndex < triangles.size();
         ++sourceTriangleIndex)
    {
        const PtSkinnedEmissiveAuditTriangle& source =
            triangles[sourceTriangleIndex];
        if (source.materialIndex >= materials.size() ||
            source.materialIndex >= materialIds.size() ||
            materialIds[source.materialIndex] != source.materialId)
        {
            ++inventory.invalidTriangles;
            continue;
        }
        const PathTraceSmokeMaterial& material =
            materials[source.materialIndex];
        if ((material.flags & emissiveMaterialFlag) == 0u)
        {
            ++inventory.nonEmissiveTriangles;
            continue;
        }
        if ((source.triangleClassAndFlags &
                RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) != 0u)
        {
            ++inventory.runtimeInactiveTriangles;
            continue;
        }
        if (source.identityHash == 0)
        {
            ++inventory.zeroIdentityTriangles;
            continue;
        }

        appendRecord(
            source,
            sourceTriangleIndex,
            material,
            false,
            inventory.current);
        if (source.hasPrevious)
        {
            appendRecord(
                source,
                sourceTriangleIndex,
                material,
                true,
                inventory.previous);
        }
        else
        {
            ++inventory.missingPreviousTriangles;
        }
    }

    RtSmokeEmissiveInventoryStats currentStats =
        BuildSmokeEmissiveInventoryStatsForRecords(
            materialIds,
            inventory.current);
    RtSmokeEmissiveInventoryStats previousStats =
        BuildSmokeEmissiveInventoryStatsForRecords(
            materialIds,
            inventory.previous);
    FinalizeSmokeEmissiveTriangleSamplingFields(
        inventory.current,
        currentStats);
    FinalizeSmokeEmissiveTriangleSamplingFields(
        inventory.previous,
        previousStats);
    return inventory;
}

std::vector<PathTraceEmissiveLightRemap>
BuildSmokeCanonicalEmissiveLightRemap(
    const std::vector<PathTraceSmokeEmissiveTriangle>&
        currentTriangles,
    const std::vector<PathTraceSmokeEmissiveTriangle>&
        previousTriangles)
{
    auto identityKey =
        [](const PathTraceSmokeEmissiveTriangle& triangle)
        {
            return
                (static_cast<uint64>(
                    triangle.identityHashHi) << 32ull) |
                static_cast<uint64>(
                    triangle.identityHashLo);
        };
    auto compatible =
        [](const PathTraceSmokeEmissiveTriangle& current,
            const PathTraceSmokeEmissiveTriangle& previous)
        {
            return
                current.identityHashLo ==
                    previous.identityHashLo &&
                current.identityHashHi ==
                    previous.identityHashHi &&
                current.materialId == previous.materialId &&
                current.universeMaterialIndex ==
                    previous.universeMaterialIndex &&
                current.emissiveTextureIndex ==
                    previous.emissiveTextureIndex;
        };
    auto buildIdentityMap =
        [&](const std::vector<
                PathTraceSmokeEmissiveTriangle>& triangles,
            std::unordered_map<uint64, int>& map)
        {
            map.clear();
            map.reserve(triangles.size());
            for (int triangleIndex = 0;
                triangleIndex <
                    static_cast<int>(triangles.size());
                ++triangleIndex)
            {
                const uint64 identity =
                    identityKey(triangles[triangleIndex]);
                if (identity == 0)
                {
                    continue;
                }
                const auto result =
                    map.emplace(identity, triangleIndex);
                if (!result.second)
                {
                    result.first->second = -1;
                }
            }
        };

    std::vector<PathTraceEmissiveLightRemap> remap(
        std::max(
            currentTriangles.size(),
            previousTriangles.size()));
    std::unordered_map<uint64, int> currentByIdentity;
    std::unordered_map<uint64, int> previousByIdentity;
    buildIdentityMap(
        currentTriangles,
        currentByIdentity);
    buildIdentityMap(
        previousTriangles,
        previousByIdentity);

    for (int currentIndex = 0;
        currentIndex <
            static_cast<int>(currentTriangles.size());
        ++currentIndex)
    {
        const uint64 identity =
            identityKey(currentTriangles[currentIndex]);
        if (identity == 0)
        {
            remap[currentIndex].flags |=
                RT_SMOKE_EMISSIVE_REMAP_CURRENT_ZERO_IDENTITY;
            continue;
        }
        const auto currentIt =
            currentByIdentity.find(identity);
        if (currentIt == currentByIdentity.end() ||
            currentIt->second != currentIndex)
        {
            remap[currentIndex].flags |=
                RT_SMOKE_EMISSIVE_REMAP_CURRENT_DUPLICATE;
            continue;
        }
        const auto previousIt =
            previousByIdentity.find(identity);
        if (previousIt == previousByIdentity.end())
        {
            remap[currentIndex].flags |=
                RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_MISSING;
            continue;
        }
        if (previousIt->second < 0)
        {
            remap[currentIndex].flags |=
                RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_DUPLICATE;
            continue;
        }

        const int previousIndex = previousIt->second;
        if (previousIndex >=
                static_cast<int>(
                    previousTriangles.size()) ||
            !compatible(
                currentTriangles[currentIndex],
                previousTriangles[previousIndex]))
        {
            remap[currentIndex].flags |=
                RT_SMOKE_EMISSIVE_REMAP_INCOMPATIBLE;
            if (previousIndex >= 0 &&
                previousIndex <
                    static_cast<int>(remap.size()))
            {
                remap[previousIndex].flags |=
                    RT_SMOKE_EMISSIVE_REMAP_INCOMPATIBLE;
            }
            continue;
        }

        remap[currentIndex].currentToPreviousIndex =
            previousIndex;
        remap[currentIndex].flags |=
            RT_SMOKE_EMISSIVE_REMAP_VALID;
        remap[previousIndex].previousToCurrentIndex =
            currentIndex;
        remap[previousIndex].flags |=
            RT_SMOKE_EMISSIVE_REMAP_VALID;
    }

    for (int previousIndex = 0;
        previousIndex <
            static_cast<int>(previousTriangles.size());
        ++previousIndex)
    {
        const uint64 identity =
            identityKey(previousTriangles[previousIndex]);
        if (identity == 0)
        {
            remap[previousIndex].flags |=
                RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_ZERO_IDENTITY;
            continue;
        }
        const auto previousIt =
            previousByIdentity.find(identity);
        if (previousIt == previousByIdentity.end() ||
            previousIt->second != previousIndex)
        {
            remap[previousIndex].flags |=
                RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_DUPLICATE;
            continue;
        }
        const auto currentIt =
            currentByIdentity.find(identity);
        if (currentIt == currentByIdentity.end())
        {
            remap[previousIndex].flags |=
                RT_SMOKE_EMISSIVE_REMAP_CURRENT_MISSING;
            continue;
        }
        if (currentIt->second < 0)
        {
            remap[previousIndex].flags |=
                RT_SMOKE_EMISSIVE_REMAP_CURRENT_DUPLICATE;
        }
    }
    return remap;
}

void AppendSmokeRigidRouteEmissiveTriangleInventory(
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeMaterial>& materials,
    const RtPathTraceRigidRouteBuild& rigidRouteBuild,
    uint32_t emissiveMaterialFlag,
    int maxRecords,
    std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles,
    RtSmokeEmissiveInventoryStats& stats)
{
    OPTICK_EVENT("PT Emissive Append Rigid Route");

    if (rigidRouteBuild.instances.empty() || rigidRouteBuild.instanceObjectToWorld.empty())
    {
        return;
    }

    maxRecords = Max(1, maxRecords);
    const std::vector<PathTraceSmokeMaterial> materialViews = BuildSmokeEmissiveMaterialViews(materialIds, materials, emissiveMaterialFlag);
    const uint32_t rigidClassAndFlags = SmokeSurfaceClassId(RtSmokeSurfaceClass::RigidEntity);
    const int instanceCount = Min(static_cast<int>(rigidRouteBuild.instances.size()), static_cast<int>(rigidRouteBuild.instanceObjectToWorld.size()));
    for (int instanceIndex = 0; instanceIndex < instanceCount; ++instanceIndex)
    {
        const PathTraceRigidRouteInstance& routeInstance = rigidRouteBuild.instances[instanceIndex];
        const float* objectToWorld = rigidRouteBuild.instanceObjectToWorld[instanceIndex].data();
        const uint32_t routeTlasInstanceId = static_cast<uint32_t>(2 + instanceIndex);
        const bool seenThisFrame =
            instanceIndex >= static_cast<int>(rigidRouteBuild.instanceSeenThisFrame.size()) ||
            rigidRouteBuild.instanceSeenThisFrame[instanceIndex] != 0u;
        bool countedEmissiveInstance = false;
        ++stats.routedRigidInstances;
        if (seenThisFrame)
        {
            ++stats.routedRigidSeenInstances;
        }
        else
        {
            ++stats.routedRigidCacheInstances;
        }
        for (uint32_t localTriangleIndex = 0; localTriangleIndex < routeInstance.triangleCount; ++localTriangleIndex)
        {
            const uint32_t globalTriangleIndex = routeInstance.triangleOffset + localTriangleIndex;
            if (globalTriangleIndex >= rigidRouteBuild.triangleMaterialIndexes.size())
            {
                ++stats.skippedInvalidMaterialTriangles;
                ++stats.routedRigidInvalidTriangles;
                continue;
            }

            const uint32_t materialIndex = rigidRouteBuild.triangleMaterialIndexes[globalTriangleIndex];
            if (materialIndex >= materialViews.size() || materialIndex >= materialIds.size())
            {
                ++stats.skippedInvalidMaterialTriangles;
                ++stats.routedRigidInvalidTriangles;
                continue;
            }

            const uint32_t sourceTriangleClassAndFlags =
                globalTriangleIndex < rigidRouteBuild.triangleClassAndFlags.size()
                    ? rigidRouteBuild.triangleClassAndFlags[globalTriangleIndex]
                    : rigidClassAndFlags;
            if ((sourceTriangleClassAndFlags & RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) != 0u)
            {
                ++stats.skippedRuntimeInactiveTriangles;
                continue;
            }

            const PathTraceSmokeMaterial& material = materialViews[materialIndex];
            if ((material.flags & emissiveMaterialFlag) == 0)
            {
                ++stats.routedRigidNonEmissiveTriangles;
                continue;
            }
            if (!countedEmissiveInstance)
            {
                countedEmissiveInstance = true;
                ++stats.routedRigidEmissiveInstances;
                if (seenThisFrame)
                {
                    ++stats.routedRigidEmissiveSeenInstances;
                }
                else
                {
                    ++stats.routedRigidEmissiveCacheInstances;
                }
            }

            const uint32_t indexOffset = routeInstance.indexOffset + localTriangleIndex * 3u;
            if (indexOffset + 2u >= rigidRouteBuild.indexes.size())
            {
                ++stats.skippedInvalidMaterialTriangles;
                ++stats.routedRigidInvalidTriangles;
                continue;
            }

            const uint32_t i0 = routeInstance.vertexOffset + rigidRouteBuild.indexes[indexOffset + 0u];
            const uint32_t i1 = routeInstance.vertexOffset + rigidRouteBuild.indexes[indexOffset + 1u];
            const uint32_t i2 = routeInstance.vertexOffset + rigidRouteBuild.indexes[indexOffset + 2u];
            if (i0 >= rigidRouteBuild.vertices.size() || i1 >= rigidRouteBuild.vertices.size() || i2 >= rigidRouteBuild.vertices.size())
            {
                ++stats.skippedInvalidMaterialTriangles;
                ++stats.routedRigidInvalidTriangles;
                continue;
            }

            const idVec3 p0 = TransformSmokeRoutePoint(objectToWorld, SmokeVertexPosition(rigidRouteBuild.vertices[i0]));
            const idVec3 p1 = TransformSmokeRoutePoint(objectToWorld, SmokeVertexPosition(rigidRouteBuild.vertices[i1]));
            const idVec3 p2 = TransformSmokeRoutePoint(objectToWorld, SmokeVertexPosition(rigidRouteBuild.vertices[i2]));
            const idVec2 uv0 = SmokeVertexTexCoord(rigidRouteBuild.vertices[i0]);
            const idVec2 uv1 = SmokeVertexTexCoord(rigidRouteBuild.vertices[i1]);
            const idVec2 uv2 = SmokeVertexTexCoord(rigidRouteBuild.vertices[i2]);
            const idVec3 edge01 = p1 - p0;
            const idVec3 edge02 = p2 - p0;
            idVec3 areaNormal = edge01.Cross(edge02);
            const float doubleArea = areaNormal.Length();
            if (doubleArea <= 1.0e-6f)
            {
                continue;
            }

            ++stats.totalTriangles;
            ++stats.dynamicTriangles;
            ++stats.routedRigidTriangles;
            if (std::find(stats.materialIndexes.begin(), stats.materialIndexes.end(), materialIndex) == stats.materialIndexes.end())
            {
                stats.materialIndexes.push_back(materialIndex);
            }

            const float area = doubleArea * 0.5f;
            areaNormal *= 1.0f / doubleArea;
            const float luminance = SmokeMaterialEmissiveLuminance(material);
            const idVec3 estimatedRadiance(
                Max(0.0f, material.emissiveColor[0]),
                Max(0.0f, material.emissiveColor[1]),
                Max(0.0f, material.emissiveColor[2]));
            const float sampleWeight = area * luminance;
            stats.totalArea += area;
            stats.totalWeightedLuminance += sampleWeight;
            stats.routedRigidArea += area;
            stats.routedRigidWeightedLuminance += sampleWeight;

            if (static_cast<int>(emissiveTriangles.size()) >= maxRecords)
            {
                ++stats.cappedTriangles;
                ++stats.routedRigidCappedTriangles;
                continue;
            }

            PathTraceSmokeEmissiveTriangle record = {};
            const idVec3 center = (p0 + p1 + p2) * (1.0f / 3.0f);
            record.centerAndArea[0] = center.x;
            record.centerAndArea[1] = center.y;
            record.centerAndArea[2] = center.z;
            record.centerAndArea[3] = area;
            record.normalAndLuminance[0] = areaNormal.x;
            record.normalAndLuminance[1] = areaNormal.y;
            record.normalAndLuminance[2] = areaNormal.z;
            record.normalAndLuminance[3] = luminance;
            record.uvBounds[0] = Min(uv0.x, Min(uv1.x, uv2.x));
            record.uvBounds[1] = Min(uv0.y, Min(uv1.y, uv2.y));
            record.uvBounds[2] = Max(uv0.x, Max(uv1.x, uv2.x));
            record.uvBounds[3] = Max(uv0.y, Max(uv1.y, uv2.y));
            record.centroidUvAndWeight[0] = (uv0.x + uv1.x + uv2.x) * (1.0f / 3.0f);
            record.centroidUvAndWeight[1] = (uv0.y + uv1.y + uv2.y) * (1.0f / 3.0f);
            record.centroidUvAndWeight[2] = sampleWeight;
            record.centroidUvAndWeight[3] = 0.0f;
            record.estimatedRadianceAndLuminance[0] = estimatedRadiance.x;
            record.estimatedRadianceAndLuminance[1] = estimatedRadiance.y;
            record.estimatedRadianceAndLuminance[2] = estimatedRadiance.z;
            record.estimatedRadianceAndLuminance[3] = luminance;
            record.sampleWeightAndPdf[0] = sampleWeight;
            record.sampleWeightAndPdf[1] = 0.0f;
            record.sampleWeightAndPdf[2] = area;
            record.sampleWeightAndPdf[3] = 0.0f;
            record.materialIndex = materialIndex;
            record.instanceId = routeTlasInstanceId;
            record.primitiveIndex = localTriangleIndex;
            record.flags = material.flags;
            record.emissiveTextureIndex = material.emissiveTextureIndex;
            record.emissiveTextureWidth = material.emissiveTextureWidth;
            record.emissiveTextureHeight = material.emissiveTextureHeight;
            record.materialId = materialIds[materialIndex];
            const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(record.materialId, materialIndex);
            record.universeMaterialIndex = GetSmokeMaterialUniverseFacts(record.materialId, info).universeIndex;
            const uint64 sourceInstanceId =
                static_cast<uint64>(routeInstance.instanceIdLo) |
                (static_cast<uint64>(routeInstance.instanceIdHi) << 32);
            const uint64 identityHash = BuildSmokeEmissiveTriangleIdentity64(record.materialId, sourceInstanceId, localTriangleIndex, materialIndex, sourceTriangleClassAndFlags);
            record.identityHashLo = static_cast<uint32_t>(identityHash & 0xffffffffu);
            record.identityHashHi = static_cast<uint32_t>(identityHash >> 32);
            record.padding0 = sourceTriangleClassAndFlags;
            emissiveTriangles.push_back(record);
            ++stats.routedRigidCapturedTriangles;
        }
    }

    stats.capturedTriangles = static_cast<int>(emissiveTriangles.size());
    stats.uniqueMaterials = static_cast<int>(stats.materialIndexes.size());
}

std::vector<uint32_t> BuildSmokeWorldStaticEmissiveMaterialIds(const viewDef_t* viewDef)
{
    OPTICK_EVENT("PT World Static Emissive Material Ids");

    std::vector<uint32_t> materialIds;
    if (!viewDef || !viewDef->renderWorld)
    {
        return materialIds;
    }

    idRenderWorldLocal* renderWorld = viewDef->renderWorld;
    for (int entityIndex = 0; entityIndex < renderWorld->entityDefs.Num(); ++entityIndex)
    {
        const idRenderEntityLocal* entity = renderWorld->entityDefs[entityIndex];
        const idRenderModel* model = entity ? entity->parms.hModel : nullptr;
        if (!model || !model->IsStaticWorldModel())
        {
            continue;
        }

        for (int surfaceIndex = 0; surfaceIndex < model->NumSurfaces(); ++surfaceIndex)
        {
            const modelSurface_t* surface = model->Surface(surfaceIndex);
            const idMaterial* material = surface ? surface->shader : nullptr;
            if (!material)
            {
                continue;
            }

            const uint32_t materialId = SmokeMaterialId(material);
            const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(materialId, -1);
            const RtSmokeMaterialUniverseFacts& facts = GetSmokeMaterialUniverseFacts(materialId, info);
            if (!facts.emissive)
            {
                continue;
            }

            if (std::find(materialIds.begin(), materialIds.end(), materialId) == materialIds.end())
            {
                materialIds.push_back(materialId);
            }
        }
    }

    return materialIds;
}

void AppendSmokeWorldStaticEmissiveTriangleInventory(
    const viewDef_t* viewDef,
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeMaterial>& materials,
    uint32_t emissiveMaterialFlag,
    uint32_t staticSurfaceClassId,
    int maxRecords,
    std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles,
    RtSmokeEmissiveInventoryStats& stats)
{
    OPTICK_EVENT("PT World Static Emissive Inventory");

    if (!viewDef || !viewDef->renderWorld)
    {
        return;
    }

    idRenderWorldLocal* renderWorld = viewDef->renderWorld;
    uint32_t worldPrimitiveId = 0;
    const int appendedBefore = static_cast<int>(emissiveTriangles.size());
    for (int entityIndex = 0; entityIndex < renderWorld->entityDefs.Num(); ++entityIndex)
    {
        const idRenderEntityLocal* entity = renderWorld->entityDefs[entityIndex];
        const idRenderModel* model = entity ? entity->parms.hModel : nullptr;
        if (!model || !model->IsStaticWorldModel())
        {
            continue;
        }
        ++stats.worldStaticScannedEntities;

        for (int surfaceIndex = 0; surfaceIndex < model->NumSurfaces(); ++surfaceIndex)
        {
            const modelSurface_t* surface = model->Surface(surfaceIndex);
            const idMaterial* material = surface ? surface->shader : nullptr;
            const srfTriangles_t* tri = surface ? surface->geometry : nullptr;
            if (!material || !tri || !tri->verts || !tri->indexes)
            {
                continue;
            }

            ++stats.worldStaticScannedSurfaces;
            stats.worldStaticScannedTriangles += tri->numIndexes / 3;
            const uint32_t materialId = SmokeMaterialId(material);
            const uint32_t materialIndex = FindSmokeMaterialTableIndexForId(materialIds, materialId);
            if (materialIndex == UINT32_MAX || materialIndex >= materials.size())
            {
                const int triangles = tri->numIndexes / 3;
                stats.skippedInvalidMaterialTriangles += triangles;
                stats.worldStaticSkippedInvalidMaterialTriangles += triangles;
                continue;
            }

            const PathTraceSmokeMaterial& smokeMaterial = materials[materialIndex];
            if ((smokeMaterial.flags & emissiveMaterialFlag) == 0)
            {
                const int triangles = tri->numIndexes / 3;
                stats.skippedNonEmissiveMaterialTriangles += triangles;
                stats.worldStaticSkippedNonEmissiveMaterialTriangles += triangles;
                continue;
            }

            const float luminance = SmokeMaterialEmissiveLuminance(smokeMaterial);
            const idVec3 estimatedRadiance(
                Max(0.0f, smokeMaterial.emissiveColor[0]),
                Max(0.0f, smokeMaterial.emissiveColor[1]),
                Max(0.0f, smokeMaterial.emissiveColor[2]));
            int acceptedSurfaceTriangles = 0;

            for (int indexOffset = 0; indexOffset + 2 < tri->numIndexes; indexOffset += 3)
            {
                ++worldPrimitiveId;
                const int i0 = tri->indexes[indexOffset + 0];
                const int i1 = tri->indexes[indexOffset + 1];
                const int i2 = tri->indexes[indexOffset + 2];
                if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= tri->numVerts || i1 >= tri->numVerts || i2 >= tri->numVerts)
                {
                    ++stats.skippedInvalidMaterialTriangles;
                    ++stats.worldStaticSkippedInvalidMaterialTriangles;
                    continue;
                }

                const idDrawVert& v0 = tri->verts[i0];
                const idDrawVert& v1 = tri->verts[i1];
                const idDrawVert& v2 = tri->verts[i2];
                const idVec3 p0 = v0.xyz;
                const idVec3 p1 = v1.xyz;
                const idVec3 p2 = v2.xyz;
                const idVec2 uv0 = v0.GetTexCoord();
                const idVec2 uv1 = v1.GetTexCoord();
                const idVec2 uv2 = v2.GetTexCoord();
                const idVec3 edge01 = p1 - p0;
                const idVec3 edge02 = p2 - p0;
                idVec3 areaNormal = edge01.Cross(edge02);
                const float doubleArea = areaNormal.Length();
                if (doubleArea <= 1.0e-6f)
                {
                    ++stats.zeroAreaTriangles;
                    ++stats.worldStaticZeroAreaTriangles;
                    continue;
                }

                const float area = doubleArea * 0.5f;
                areaNormal *= 1.0f / doubleArea;
                const float sampleWeight = area * luminance;
                ++stats.totalTriangles;
                ++stats.staticTriangles;
                ++stats.fullLevelStaticTriangles;
                stats.totalArea += area;
                stats.totalWeightedLuminance += sampleWeight;

                if (std::find(stats.materialIndexes.begin(), stats.materialIndexes.end(), materialIndex) == stats.materialIndexes.end())
                {
                    stats.materialIndexes.push_back(materialIndex);
                }

                if (static_cast<int>(emissiveTriangles.size()) >= maxRecords)
                {
                    ++stats.cappedTriangles;
                    ++stats.worldStaticCappedTriangles;
                    continue;
                }

                PathTraceSmokeEmissiveTriangle record = {};
                const idVec3 center = (p0 + p1 + p2) * (1.0f / 3.0f);
                record.centerAndArea[0] = center.x;
                record.centerAndArea[1] = center.y;
                record.centerAndArea[2] = center.z;
                record.centerAndArea[3] = area;
                record.normalAndLuminance[0] = areaNormal.x;
                record.normalAndLuminance[1] = areaNormal.y;
                record.normalAndLuminance[2] = areaNormal.z;
                record.normalAndLuminance[3] = luminance;
                record.uvBounds[0] = Min(uv0.x, Min(uv1.x, uv2.x));
                record.uvBounds[1] = Min(uv0.y, Min(uv1.y, uv2.y));
                record.uvBounds[2] = Max(uv0.x, Max(uv1.x, uv2.x));
                record.uvBounds[3] = Max(uv0.y, Max(uv1.y, uv2.y));
                record.centroidUvAndWeight[0] = (uv0.x + uv1.x + uv2.x) * (1.0f / 3.0f);
                record.centroidUvAndWeight[1] = (uv0.y + uv1.y + uv2.y) * (1.0f / 3.0f);
                record.centroidUvAndWeight[2] = sampleWeight;
                record.estimatedRadianceAndLuminance[0] = estimatedRadiance.x;
                record.estimatedRadianceAndLuminance[1] = estimatedRadiance.y;
                record.estimatedRadianceAndLuminance[2] = estimatedRadiance.z;
                record.estimatedRadianceAndLuminance[3] = luminance;
                record.sampleWeightAndPdf[0] = sampleWeight;
                record.sampleWeightAndPdf[2] = area;
                record.materialIndex = materialIndex;
                record.instanceId = 0;
                record.primitiveIndex = worldPrimitiveId;
                record.flags = smokeMaterial.flags;
                record.emissiveTextureIndex = smokeMaterial.emissiveTextureIndex;
                record.emissiveTextureWidth = smokeMaterial.emissiveTextureWidth;
                record.emissiveTextureHeight = smokeMaterial.emissiveTextureHeight;
                record.materialId = materialId;
                const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(materialId, static_cast<int>(materialIndex));
                record.universeMaterialIndex = GetSmokeMaterialUniverseFacts(materialId, info).universeIndex;
                const uint32_t classAndFlags = staticSurfaceClassId;
                const uint64 identityHash = BuildSmokeEmissiveTriangleIdentity(materialId, 0, worldPrimitiveId, materialIndex, classAndFlags);
                record.identityHashLo = static_cast<uint32_t>(identityHash & 0xffffffffu);
                record.identityHashHi = static_cast<uint32_t>(identityHash >> 32);
                record.padding0 = classAndFlags;
                emissiveTriangles.push_back(record);
                ++acceptedSurfaceTriangles;
            }
            if (acceptedSurfaceTriangles > 0)
            {
                ++stats.worldStaticAcceptedSurfaces;
                stats.worldStaticAcceptedTriangles += acceptedSurfaceTriangles;
            }
        }
    }

    stats.capturedTriangles = static_cast<int>(emissiveTriangles.size());
    stats.worldStaticFinalAppended += stats.capturedTriangles - appendedBefore;
    stats.uniqueMaterials = static_cast<int>(stats.materialIndexes.size());
}

std::vector<PathTraceSmokeMaterial> BuildSmokeEmissiveMaterialViews(const std::vector<uint32_t>& materialIds, const std::vector<PathTraceSmokeMaterial>& frameMaterials, uint32_t emissiveMaterialFlag)
{
    OPTICK_EVENT("PT Emissive Material Views");

    std::vector<PathTraceSmokeMaterial> materialViews = frameMaterials;
    const int materialCount = Min(static_cast<int>(materialIds.size()), static_cast<int>(frameMaterials.size()));
    for (int materialIndex = 0; materialIndex < materialCount; ++materialIndex)
    {
        PathTraceSmokeMaterial material = frameMaterials[materialIndex];

        if ((material.flags & emissiveMaterialFlag) == 0)
        {
            material.flags &= ~emissiveMaterialFlag;
            material.emissiveColor[0] = 0.0f;
            material.emissiveColor[1] = 0.0f;
            material.emissiveColor[2] = 0.0f;
            material.emissiveColor[3] = 1.0f;
        }

        materialViews[materialIndex] = material;
    }

    return materialViews;
}

void BuildSmokeEmissiveLightCandidateSummaries(
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles,
    RtSmokeEmissiveInventoryStats& stats)
{
    OPTICK_EVENT("PT Emissive Candidate Summaries");

    stats.lightCandidates.clear();
    stats.lightCandidates.reserve(stats.uniqueMaterials);
    stats.candidateMaterials = 0;
    stats.texturedCandidateMaterials = 0;
    stats.untexturedCandidateMaterials = 0;

    for (int triangleIndex = 0; triangleIndex < stats.capturedTriangles; ++triangleIndex)
    {
        const PathTraceSmokeEmissiveTriangle& record = emissiveTriangles[triangleIndex];
        const int materialIndex = static_cast<int>(record.materialIndex);
        if (materialIndex < 0 || materialIndex >= static_cast<int>(materialIds.size()))
        {
            continue;
        }

        const uint32_t materialId = materialIds[materialIndex];
        RtSmokeEmissiveLightCandidateSummary* candidate = nullptr;
        for (RtSmokeEmissiveLightCandidateSummary& existing : stats.lightCandidates)
        {
            if (existing.materialId == materialId)
            {
                candidate = &existing;
                break;
            }
        }

        if (!candidate)
        {
            const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(materialId, materialIndex);
            const RtSmokeMaterialUniverseFacts& facts = GetSmokeMaterialUniverseFacts(materialId, info);
            RtSmokeEmissiveLightCandidateSummary newCandidate = {};
            newCandidate.materialId = materialId;
            newCandidate.universeMaterialIndex = facts.universeIndex;
            newCandidate.materialIndex = record.materialIndex;
            newCandidate.hasEmissiveTexture = facts.hasEmissiveImage;
            newCandidate.hasSafeEmissiveTexture = facts.hasSafeEmissiveTexture;
            newCandidate.emissiveTextureIndex = record.emissiveTextureIndex;
            newCandidate.emissiveTextureWidth = record.emissiveTextureWidth;
            newCandidate.emissiveTextureHeight = record.emissiveTextureHeight;
            stats.lightCandidates.push_back(newCandidate);
            candidate = &stats.lightCandidates.back();
        }

        const idVec4 recordEmissiveColor(
            Max(0.0f, record.estimatedRadianceAndLuminance[0]),
            Max(0.0f, record.estimatedRadianceAndLuminance[1]),
            Max(0.0f, record.estimatedRadianceAndLuminance[2]),
            1.0f);
        const float recordLuminance = Max(0.0f, record.estimatedRadianceAndLuminance[3]);
        const float recordWeight = Max(0.0f, record.centroidUvAndWeight[2]);
        const float previousWeight = candidate->weightedLuminance;
        const float nextWeight = previousWeight + recordWeight;
        if (nextWeight > 1.0e-8f)
        {
            const float previousScale = previousWeight / nextWeight;
            const float recordScale = recordWeight / nextWeight;
            candidate->emissiveColor = candidate->emissiveColor * previousScale + recordEmissiveColor * recordScale;
            candidate->emissiveLuminance = candidate->emissiveLuminance * previousScale + recordLuminance * recordScale;
        }
        else if (candidate->triangles == 0)
        {
            candidate->emissiveColor = recordEmissiveColor;
            candidate->emissiveLuminance = recordLuminance;
        }

        ++candidate->triangles;
        if (record.instanceId == 0)
        {
            ++candidate->staticTriangles;
        }
        else
        {
            ++candidate->dynamicTriangles;
        }
        candidate->area += record.centerAndArea[3];
        candidate->weightedLuminance = nextWeight;
        if (candidate->emissiveTextureIndex == UINT32_MAX && record.emissiveTextureIndex != UINT32_MAX)
        {
            candidate->emissiveTextureIndex = record.emissiveTextureIndex;
            candidate->emissiveTextureWidth = record.emissiveTextureWidth;
            candidate->emissiveTextureHeight = record.emissiveTextureHeight;
        }
    }

    std::sort(stats.lightCandidates.begin(), stats.lightCandidates.end(),
        [](const RtSmokeEmissiveLightCandidateSummary& lhs, const RtSmokeEmissiveLightCandidateSummary& rhs)
        {
            return lhs.weightedLuminance > rhs.weightedLuminance;
        });

    stats.candidateMaterials = static_cast<int>(stats.lightCandidates.size());
    for (const RtSmokeEmissiveLightCandidateSummary& candidate : stats.lightCandidates)
    {
        if (candidate.emissiveTextureIndex != UINT32_MAX)
        {
            ++stats.texturedCandidateMaterials;
        }
        else
        {
            ++stats.untexturedCandidateMaterials;
        }
    }
}

std::vector<PathTraceSmokeLightCandidate> BuildSmokeLightCandidateBufferRecords(const RtSmokeEmissiveInventoryStats& stats)
{
    OPTICK_EVENT("PT Emissive Candidate Buffer Records");

    std::vector<PathTraceSmokeLightCandidate> candidates;
    candidates.reserve(Max(1, stats.candidateMaterials));
    for (const RtSmokeEmissiveLightCandidateSummary& summary : stats.lightCandidates)
    {
        PathTraceSmokeLightCandidate candidate = {};
        candidate.emissiveColorAndLuminance[0] = summary.emissiveColor.x;
        candidate.emissiveColorAndLuminance[1] = summary.emissiveColor.y;
        candidate.emissiveColorAndLuminance[2] = summary.emissiveColor.z;
        candidate.emissiveColorAndLuminance[3] = summary.emissiveLuminance;
        candidate.areaAndWeightedLuminance[0] = summary.area;
        candidate.areaAndWeightedLuminance[1] = summary.weightedLuminance;
        candidate.areaAndWeightedLuminance[2] = 0.0f;
        candidate.areaAndWeightedLuminance[3] = 0.0f;
        candidate.materialId = summary.materialId;
        candidate.universeMaterialIndex = summary.universeMaterialIndex;
        candidate.materialIndex = summary.materialIndex;
        candidate.triangleCount = static_cast<uint32_t>(Max(0, summary.triangles));
        candidate.staticTriangleCount = static_cast<uint32_t>(Max(0, summary.staticTriangles));
        candidate.dynamicTriangleCount = static_cast<uint32_t>(Max(0, summary.dynamicTriangles));
        candidate.emissiveTextureIndex = summary.emissiveTextureIndex;
        candidate.emissiveTextureWidth = summary.emissiveTextureWidth;
        candidate.emissiveTextureHeight = summary.emissiveTextureHeight;
        if (summary.hasEmissiveTexture)
        {
            candidate.flags |= RT_SMOKE_LIGHT_CANDIDATE_TEXTURED;
        }
        if (summary.hasSafeEmissiveTexture)
        {
            candidate.flags |= RT_SMOKE_LIGHT_CANDIDATE_SAFE_TEXTURE;
        }
        if (summary.staticTriangles > 0)
        {
            candidate.flags |= RT_SMOKE_LIGHT_CANDIDATE_HAS_STATIC_TRIANGLES;
        }
        if (summary.dynamicTriangles > 0)
        {
            candidate.flags |= RT_SMOKE_LIGHT_CANDIDATE_HAS_DYNAMIC_TRIANGLES;
        }
        candidates.push_back(candidate);
    }

    if (candidates.empty())
    {
        candidates.resize(1);
    }
    return candidates;
}

RtSmokeEmissiveInventoryStats BuildSmokeEmissiveInventoryStatsForRecords(
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles)
{
    OPTICK_EVENT("PT Emissive Stats From Records");

    RtSmokeEmissiveInventoryStats stats;
    stats.capturedTriangles = static_cast<int>(emissiveTriangles.size());
    for (const PathTraceSmokeEmissiveTriangle& record : emissiveTriangles)
    {
        if (record.centerAndArea[3] <= 0.0f)
        {
            continue;
        }

        ++stats.totalTriangles;
        if (record.instanceId == 0)
        {
            ++stats.staticTriangles;
        }
        else
        {
            ++stats.dynamicTriangles;
        }
        const uint32_t surfaceClass = record.padding0 & RT_SMOKE_TRIANGLE_CLASS_MASK;
        if (record.instanceId >= 2u && surfaceClass == SmokeSurfaceClassId(RtSmokeSurfaceClass::RigidEntity))
        {
            ++stats.routedRigidTriangles;
            ++stats.routedRigidCapturedTriangles;
        }
        stats.totalArea += record.centerAndArea[3];
        stats.totalWeightedLuminance += record.sampleWeightAndPdf[0];

        if (record.materialIndex < materialIds.size() &&
            std::find(stats.materialIndexes.begin(), stats.materialIndexes.end(), record.materialIndex) == stats.materialIndexes.end())
        {
            stats.materialIndexes.push_back(record.materialIndex);
        }
    }

    stats.uniqueMaterials = static_cast<int>(stats.materialIndexes.size());
    BuildSmokeEmissiveLightCandidateSummaries(materialIds, emissiveTriangles, stats);
    return stats;
}

std::vector<PathTraceSmokeEmissiveTriangle> BuildSmokeEmissiveTriangleInventory(
    const std::vector<uint32_t>& materialIds,
    const std::vector<PathTraceSmokeMaterial>& materials,
    const std::vector<PathTraceSmokeVertex>& staticVertices,
    const std::vector<uint32_t>& staticIndexes,
    const std::vector<uint32_t>& staticTriangleClasses,
    const std::vector<uint32_t>& staticTriangleMaterialIndexes,
    const std::vector<PathTraceSmokeVertex>& dynamicVertices,
    const std::vector<uint32_t>& dynamicIndexes,
    const std::vector<uint32_t>& dynamicTriangleClasses,
    const std::vector<uint32_t>& dynamicTriangleMaterialIndexes,
    const std::vector<uint32_t>& dynamicTriangleInstanceIds,
    const std::vector<uint32_t>& dynamicTriangleIdentityIds,
    uint32_t emissiveMaterialFlag,
    uint32_t triangleClassMask,
    uint32_t skinnedSurfaceClassId,
    int maxRecords,
    RtSmokeEmissiveInventoryStats& stats)
{
    OPTICK_EVENT("PT Emissive Triangle Inventory Detail");

    stats = RtSmokeEmissiveInventoryStats();
    std::vector<PathTraceSmokeEmissiveTriangle> emissiveTriangles;
    maxRecords = Max(1, maxRecords);
    emissiveTriangles.reserve(Min(maxRecords, 1024));
    const std::vector<PathTraceSmokeMaterial> materialViews = BuildSmokeEmissiveMaterialViews(materialIds, materials, emissiveMaterialFlag);
    AppendSmokeEmissiveInventoryForGeometry(materialIds, materialViews, staticVertices, staticIndexes, staticTriangleClasses, staticTriangleMaterialIndexes, 0, nullptr, nullptr, emissiveMaterialFlag, triangleClassMask, skinnedSurfaceClassId, maxRecords, emissiveTriangles, stats);
    AppendSmokeEmissiveInventoryForGeometry(materialIds, materialViews, dynamicVertices, dynamicIndexes, dynamicTriangleClasses, dynamicTriangleMaterialIndexes, 1, &dynamicTriangleInstanceIds, &dynamicTriangleIdentityIds, emissiveMaterialFlag, triangleClassMask, skinnedSurfaceClassId, maxRecords, emissiveTriangles, stats);
    stats.capturedTriangles = static_cast<int>(emissiveTriangles.size());
    stats.uniqueMaterials = static_cast<int>(stats.materialIndexes.size());
    FinalizeSmokeEmissiveTriangleSamplingFields(emissiveTriangles, stats);
    BuildSmokeEmissiveLightCandidateSummaries(materialIds, emissiveTriangles, stats);
    if (emissiveTriangles.empty())
    {
        emissiveTriangles.resize(1);
    }
    return emissiveTriangles;
}
