#pragma once

// Pure CPU measurement for geometry-modernization Q2.
//
// The candidate stores a normal/tangent direction in a bind-triangle
// orthonormal frame, then reconstructs it in the corresponding posed-triangle
// frame. This models the least invasive "derive from geometric normal plus
// stored deltas" scheme without changing the live GPU vertex ABI.

#include <cstddef>
#include <cstdint>

struct PtSkinnedAttributeDeriveSourceVertex
{
    float position[3] = {};
    float normal[3] = {};
    float tangent[3] = {};
};

struct PtSkinnedAttributeDeriveCurrentVertex
{
    float position[3] = {};
    float normal[3] = {};
    float tangent[3] = {};
};

struct PtSkinnedAttributeDeriveSurveyStats
{
    std::uint64_t surfaceCount = 0;
    std::uint64_t sourceVertexCount = 0;
    std::uint64_t currentVertexCount = 0;
    std::uint64_t indexCount = 0;
    std::uint64_t requestedTriangleCount = 0;
    std::uint64_t sampledTriangleCount = 0;
    std::uint64_t cornerSampleCount = 0;
    std::uint64_t invalidIndexTriangles = 0;
    std::uint64_t degenerateBindTriangles = 0;
    std::uint64_t degenerateCurrentTriangles = 0;
    std::uint64_t invalidDirectionCorners = 0;

    double geometricNormalSquaredErrorDegrees = 0.0;
    double transportedNormalSquaredErrorDegrees = 0.0;
    double transportedTangentSquaredErrorDegrees = 0.0;
    float geometricNormalMaxErrorDegrees = 0.0f;
    float transportedNormalMaxErrorDegrees = 0.0f;
    float transportedTangentMaxErrorDegrees = 0.0f;

    std::uint64_t transportedNormalOver1Degree = 0;
    std::uint64_t transportedNormalOver5Degrees = 0;
    std::uint64_t transportedNormalOver10Degrees = 0;
    std::uint64_t transportedTangentOver1Degree = 0;
    std::uint64_t transportedTangentOver5Degrees = 0;
    std::uint64_t transportedTangentOver10Degrees = 0;

    // Current PathTraceSmokeVertex writes normal, tangent, and bitangent as
    // three float4 values. The candidate delta figure assumes two signed
    // oct16 direction pairs per triangle corner; it is comparison evidence,
    // not an accepted storage ABI.
    std::uint64_t currentBasisWriteBytes = 0;
    std::uint64_t candidateCornerDeltaBytes = 0;
};

void PtSurveySkinnedAttributeDerivation(
    const PtSkinnedAttributeDeriveSourceVertex* sourceVertices,
    std::size_t sourceVertexCount,
    const PtSkinnedAttributeDeriveCurrentVertex* currentVertices,
    std::size_t currentVertexCount,
    const std::uint32_t* indexes,
    std::size_t indexCount,
    PtSkinnedAttributeDeriveSurveyStats& stats);

void PtAccumulateSkinnedAttributeDeriveSurvey(
    const PtSkinnedAttributeDeriveSurveyStats& source,
    PtSkinnedAttributeDeriveSurveyStats& destination);

float PtSkinnedAttributeDeriveGeometricNormalRmsDegrees(
    const PtSkinnedAttributeDeriveSurveyStats& stats);
float PtSkinnedAttributeDeriveTransportedNormalRmsDegrees(
    const PtSkinnedAttributeDeriveSurveyStats& stats);
float PtSkinnedAttributeDeriveTransportedTangentRmsDegrees(
    const PtSkinnedAttributeDeriveSurveyStats& stats);
