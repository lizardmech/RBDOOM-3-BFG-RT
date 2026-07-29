#pragma once

// Pure CPU measurement contract for GEO-12 geometry attribute experiments.
//
// This module observes the full-fidelity canonical source registry. It does
// not select an encoding, mutate source records, or change any GPU ABI.

#include "PathTraceGeometrySourceRegistry.h"

#include <cstdint>
#include <string>
#include <vector>

struct PtGeometryAttributeSurveyStats
{
    std::uint64_t recordCount = 0;
    std::uint64_t rigidRecordCount = 0;
    std::uint64_t skinnedRecordCount = 0;
    std::uint64_t vertexCount = 0;
    std::uint64_t currentPositionBytes = 0;
    std::uint64_t currentAttributeBytes = 0;

    float positionMin[3] = {};
    float positionMax[3] = {};
    float texCoordMin[2] = {};
    float texCoordMax[2] = {};
    std::uint64_t nonFinitePositionComponents = 0;
    std::uint64_t nonFiniteTexCoordComponents = 0;
    std::uint64_t nonFiniteBasisComponents = 0;
    std::uint64_t nonFiniteColorComponents = 0;

    std::uint64_t halfTexCoordComponents = 0;
    std::uint64_t halfTexCoordOverflowComponents = 0;
    std::uint64_t halfTexCoordUnderflowToZeroComponents = 0;
    float halfTexCoordMaxAbsError = 0.0f;
    float halfTexCoordMaxRelativeError = 0.0f;

    std::uint64_t normalDegenerateVertices = 0;
    std::uint64_t tangentDegenerateVertices = 0;
    std::uint64_t bitangentDegenerateVertices = 0;
    std::uint64_t normalOct16Samples = 0;
    std::uint64_t tangentOct16Samples = 0;
    float normalOct16MaxAngularErrorDegrees = 0.0f;
    float tangentOct16MaxAngularErrorDegrees = 0.0f;

    std::uint64_t bitangentReconstructionSamples = 0;
    std::uint64_t bitangentReconstructionInvalid = 0;
    float bitangentReconstructionMaxAngularErrorDegrees = 0.0f;

    std::uint64_t colorComponents = 0;
    std::uint64_t colorOutOfUnormRangeComponents = 0;
    std::uint64_t colorUnorm8ExactComponents = 0;
    float colorUnorm8MaxAbsError = 0.0f;
    std::uint64_t color2Components = 0;
    std::uint64_t color2OutOfUnormRangeComponents = 0;
    std::uint64_t color2Unorm8ExactComponents = 0;
    float color2Unorm8MaxAbsError = 0.0f;

    std::uint64_t skinnedVertexCount = 0;
    std::uint64_t skinnedJointComponents = 0;
    std::uint64_t skinnedJointNonIntegralComponents = 0;
    std::uint64_t skinnedJointOutOfByteRangeComponents = 0;
    std::uint32_t skinnedJointIndexMin = 0;
    std::uint32_t skinnedJointIndexMax = 0;
    float skinnedWeightMin = 0.0f;
    float skinnedWeightMax = 0.0f;
    float skinnedWeightSumMin = 0.0f;
    float skinnedWeightSumMax = 0.0f;
    std::uint64_t skinnedWeightNonFiniteComponents = 0;
};

struct PtGeometryAttributeSurveyRecord
{
    std::uint64_t meshHash = 0;
    PtCanonicalMeshSourceDomain sourceDomain =
        PtCanonicalMeshSourceDomain::Invalid;
    PtCanonicalDeformationClass deformationClass =
        PtCanonicalDeformationClass::Invalid;
    std::uint32_t modelSurfaceIndex = 0;
    PtGeometryAttributeSurveyStats stats;
};

struct PtGeometryAttributeSurvey
{
    PtGeometryAttributeSurveyStats totals;
    std::vector<PtGeometryAttributeSurveyRecord> records;
};

enum class PtRenderedGeometrySurveyDomain : std::uint32_t
{
    Invalid = 0,
    StaticResident = 1,
    DynamicFallback = 2
};

// POD copy of the values visible to the rendered PathTraceSmokeVertex ABI.
// Keeping this type renderer-independent lets the measurement remain unit
// testable without pulling engine/PCH types into the pure survey module.
struct PtRenderedGeometrySurveyVertex
{
    float position[3] = {};
    float normal[3] = {};
    float texCoord[4] = {};
    float color[4] = {};
    float color2[4] = {};
    float tangent[3] = {};
    float bitangent[3] = {};
};

struct PtRenderedGeometrySurveyStats
{
    PtGeometryAttributeSurveyStats values;
    float normalMapTexCoordMin[2] = {};
    float normalMapTexCoordMax[2] = {};
    std::uint64_t nonFiniteNormalMapTexCoordComponents = 0;
    std::uint64_t halfNormalMapTexCoordComponents = 0;
    std::uint64_t halfNormalMapTexCoordOverflowComponents = 0;
    std::uint64_t halfNormalMapTexCoordUnderflowToZeroComponents = 0;
    float halfNormalMapTexCoordMaxAbsError = 0.0f;
    float halfNormalMapTexCoordMaxRelativeError = 0.0f;
};

struct PtRenderedGeometrySurveyRecord
{
    PtRenderedGeometrySurveyDomain domain =
        PtRenderedGeometrySurveyDomain::Invalid;
    std::uint64_t identity = 0;
    std::uint32_t surfaceClassId = 0;
    std::uint32_t materialId = 0;
    int entityIndex = -1;
    int modelSurfaceIndex = -1;
    int portalArea = -1;
    std::string modelName;
    std::string materialName;
    PtRenderedGeometrySurveyStats stats;
};

struct PtRenderedGeometrySurvey
{
    PtRenderedGeometrySurveyStats totals;
    std::uint64_t staticRecordCount = 0;
    std::uint64_t dynamicRecordCount = 0;
    std::uint64_t invalidRangeRecordCount = 0;
    std::vector<PtRenderedGeometrySurveyRecord> records;
};

void PtSurveyGeometrySourceRegistry(
    const PtGeometrySourceRegistry& registry,
    PtGeometryAttributeSurvey& survey);

void PtBeginRenderedGeometrySurvey(PtRenderedGeometrySurvey& survey);
void PtAppendRenderedGeometrySurveyRecord(
    const PtRenderedGeometrySurveyRecord& metadata,
    const PtRenderedGeometrySurveyVertex* vertices,
    std::size_t vertexCount,
    PtRenderedGeometrySurvey& survey);
void PtRecordRenderedGeometrySurveyInvalidRange(
    PtRenderedGeometrySurvey& survey);
