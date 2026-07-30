#include "PathTraceSkinnedAttributeDeriveSurvey.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kDirectionLengthSquaredMinimum = 1.0e-12f;
constexpr float kRadiansToDegrees = 57.295779513082320876f;

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Frame
{
    Vec3 x;
    Vec3 y;
    Vec3 z;
};

Vec3 Load3(const float value[3])
{
    return { value[0], value[1], value[2] };
}

Vec3 Subtract(const Vec3& a, const Vec3& b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

float Dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

bool Normalize(Vec3& value)
{
    const float lengthSquared = Dot(value, value);
    if (!std::isfinite(lengthSquared) ||
        lengthSquared <= kDirectionLengthSquaredMinimum)
    {
        return false;
    }
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    value.x *= inverseLength;
    value.y *= inverseLength;
    value.z *= inverseLength;
    return
        std::isfinite(value.x) &&
        std::isfinite(value.y) &&
        std::isfinite(value.z);
}

bool BuildTriangleFrame(
    const Vec3& p0,
    const Vec3& p1,
    const Vec3& p2,
    Frame& frame)
{
    frame.x = Subtract(p1, p0);
    Vec3 edge2 = Subtract(p2, p0);
    frame.z = Cross(frame.x, edge2);
    if (!Normalize(frame.x) || !Normalize(frame.z))
    {
        return false;
    }
    frame.y = Cross(frame.z, frame.x);
    return Normalize(frame.y);
}

bool TransportDirection(
    const Vec3& source,
    const Frame& bindFrame,
    const Frame& currentFrame,
    Vec3& transported)
{
    Vec3 direction = source;
    if (!Normalize(direction))
    {
        return false;
    }
    const float coefficientX = Dot(direction, bindFrame.x);
    const float coefficientY = Dot(direction, bindFrame.y);
    const float coefficientZ = Dot(direction, bindFrame.z);
    transported = {
        currentFrame.x.x * coefficientX +
            currentFrame.y.x * coefficientY +
            currentFrame.z.x * coefficientZ,
        currentFrame.x.y * coefficientX +
            currentFrame.y.y * coefficientY +
            currentFrame.z.y * coefficientZ,
        currentFrame.x.z * coefficientX +
            currentFrame.y.z * coefficientY +
            currentFrame.z.z * coefficientZ
    };
    return Normalize(transported);
}

bool AngularErrorDegrees(
    Vec3 a,
    Vec3 b,
    float& errorDegrees)
{
    if (!Normalize(a) || !Normalize(b))
    {
        return false;
    }
    const float cosine = std::max(-1.0f, std::min(1.0f, Dot(a, b)));
    errorDegrees = std::acos(cosine) * kRadiansToDegrees;
    return std::isfinite(errorDegrees);
}

void AccumulateError(
    float errorDegrees,
    double& squaredErrorDegrees,
    float& maxErrorDegrees,
    std::uint64_t& over1Degree,
    std::uint64_t& over5Degrees,
    std::uint64_t& over10Degrees)
{
    squaredErrorDegrees +=
        static_cast<double>(errorDegrees) *
        static_cast<double>(errorDegrees);
    maxErrorDegrees = std::max(maxErrorDegrees, errorDegrees);
    over1Degree += errorDegrees > 1.0f ? 1u : 0u;
    over5Degrees += errorDegrees > 5.0f ? 1u : 0u;
    over10Degrees += errorDegrees > 10.0f ? 1u : 0u;
}

float Rms(double squaredErrorDegrees, std::uint64_t sampleCount)
{
    return sampleCount > 0
        ? static_cast<float>(
            std::sqrt(
                squaredErrorDegrees /
                static_cast<double>(sampleCount)))
        : 0.0f;
}

} // namespace

void PtSurveySkinnedAttributeDerivation(
    const PtSkinnedAttributeDeriveSourceVertex* sourceVertices,
    std::size_t sourceVertexCount,
    const PtSkinnedAttributeDeriveCurrentVertex* currentVertices,
    std::size_t currentVertexCount,
    const std::uint32_t* indexes,
    std::size_t indexCount,
    PtSkinnedAttributeDeriveSurveyStats& stats)
{
    stats = PtSkinnedAttributeDeriveSurveyStats();
    stats.surfaceCount = 1;
    stats.sourceVertexCount = sourceVertexCount;
    stats.currentVertexCount = currentVertexCount;
    stats.indexCount = indexCount;
    stats.requestedTriangleCount = indexCount / 3;
    stats.currentBasisWriteBytes =
        static_cast<std::uint64_t>(currentVertexCount) * 48u;

    if (sourceVertices == nullptr ||
        currentVertices == nullptr ||
        indexes == nullptr ||
        sourceVertexCount == 0 ||
        currentVertexCount == 0)
    {
        stats.invalidIndexTriangles = stats.requestedTriangleCount;
        return;
    }

    for (std::size_t triangle = 0;
        triangle < stats.requestedTriangleCount;
        ++triangle)
    {
        const std::uint32_t i0 = indexes[triangle * 3 + 0];
        const std::uint32_t i1 = indexes[triangle * 3 + 1];
        const std::uint32_t i2 = indexes[triangle * 3 + 2];
        if (i0 >= sourceVertexCount ||
            i1 >= sourceVertexCount ||
            i2 >= sourceVertexCount ||
            i0 >= currentVertexCount ||
            i1 >= currentVertexCount ||
            i2 >= currentVertexCount)
        {
            ++stats.invalidIndexTriangles;
            continue;
        }

        Frame bindFrame;
        if (!BuildTriangleFrame(
                Load3(sourceVertices[i0].position),
                Load3(sourceVertices[i1].position),
                Load3(sourceVertices[i2].position),
                bindFrame))
        {
            ++stats.degenerateBindTriangles;
            continue;
        }
        Frame currentFrame;
        if (!BuildTriangleFrame(
                Load3(currentVertices[i0].position),
                Load3(currentVertices[i1].position),
                Load3(currentVertices[i2].position),
                currentFrame))
        {
            ++stats.degenerateCurrentTriangles;
            continue;
        }

        ++stats.sampledTriangleCount;
        const std::uint32_t corners[3] = { i0, i1, i2 };
        for (std::uint32_t corner : corners)
        {
            Vec3 transportedNormal;
            Vec3 transportedTangent;
            float geometricNormalError = 0.0f;
            float transportedNormalError = 0.0f;
            float transportedTangentError = 0.0f;
            if (!TransportDirection(
                    Load3(sourceVertices[corner].normal),
                    bindFrame,
                    currentFrame,
                    transportedNormal) ||
                !TransportDirection(
                    Load3(sourceVertices[corner].tangent),
                    bindFrame,
                    currentFrame,
                    transportedTangent) ||
                !AngularErrorDegrees(
                    currentFrame.z,
                    Load3(currentVertices[corner].normal),
                    geometricNormalError) ||
                !AngularErrorDegrees(
                    transportedNormal,
                    Load3(currentVertices[corner].normal),
                    transportedNormalError) ||
                !AngularErrorDegrees(
                    transportedTangent,
                    Load3(currentVertices[corner].tangent),
                    transportedTangentError))
            {
                ++stats.invalidDirectionCorners;
                continue;
            }

            ++stats.cornerSampleCount;
            stats.geometricNormalSquaredErrorDegrees +=
                static_cast<double>(geometricNormalError) *
                static_cast<double>(geometricNormalError);
            stats.geometricNormalMaxErrorDegrees =
                std::max(
                    stats.geometricNormalMaxErrorDegrees,
                    geometricNormalError);
            AccumulateError(
                transportedNormalError,
                stats.transportedNormalSquaredErrorDegrees,
                stats.transportedNormalMaxErrorDegrees,
                stats.transportedNormalOver1Degree,
                stats.transportedNormalOver5Degrees,
                stats.transportedNormalOver10Degrees);
            AccumulateError(
                transportedTangentError,
                stats.transportedTangentSquaredErrorDegrees,
                stats.transportedTangentMaxErrorDegrees,
                stats.transportedTangentOver1Degree,
                stats.transportedTangentOver5Degrees,
                stats.transportedTangentOver10Degrees);
        }
    }

    stats.candidateCornerDeltaBytes =
        stats.cornerSampleCount * 8u;
}

void PtAccumulateSkinnedAttributeDeriveSurvey(
    const PtSkinnedAttributeDeriveSurveyStats& source,
    PtSkinnedAttributeDeriveSurveyStats& destination)
{
#define PT_ACCUMULATE_FIELD(field) destination.field += source.field
    PT_ACCUMULATE_FIELD(surfaceCount);
    PT_ACCUMULATE_FIELD(sourceVertexCount);
    PT_ACCUMULATE_FIELD(currentVertexCount);
    PT_ACCUMULATE_FIELD(indexCount);
    PT_ACCUMULATE_FIELD(requestedTriangleCount);
    PT_ACCUMULATE_FIELD(sampledTriangleCount);
    PT_ACCUMULATE_FIELD(cornerSampleCount);
    PT_ACCUMULATE_FIELD(invalidIndexTriangles);
    PT_ACCUMULATE_FIELD(degenerateBindTriangles);
    PT_ACCUMULATE_FIELD(degenerateCurrentTriangles);
    PT_ACCUMULATE_FIELD(invalidDirectionCorners);
    PT_ACCUMULATE_FIELD(geometricNormalSquaredErrorDegrees);
    PT_ACCUMULATE_FIELD(transportedNormalSquaredErrorDegrees);
    PT_ACCUMULATE_FIELD(transportedTangentSquaredErrorDegrees);
    PT_ACCUMULATE_FIELD(transportedNormalOver1Degree);
    PT_ACCUMULATE_FIELD(transportedNormalOver5Degrees);
    PT_ACCUMULATE_FIELD(transportedNormalOver10Degrees);
    PT_ACCUMULATE_FIELD(transportedTangentOver1Degree);
    PT_ACCUMULATE_FIELD(transportedTangentOver5Degrees);
    PT_ACCUMULATE_FIELD(transportedTangentOver10Degrees);
    PT_ACCUMULATE_FIELD(currentBasisWriteBytes);
    PT_ACCUMULATE_FIELD(candidateCornerDeltaBytes);
#undef PT_ACCUMULATE_FIELD
    destination.geometricNormalMaxErrorDegrees =
        std::max(
            destination.geometricNormalMaxErrorDegrees,
            source.geometricNormalMaxErrorDegrees);
    destination.transportedNormalMaxErrorDegrees =
        std::max(
            destination.transportedNormalMaxErrorDegrees,
            source.transportedNormalMaxErrorDegrees);
    destination.transportedTangentMaxErrorDegrees =
        std::max(
            destination.transportedTangentMaxErrorDegrees,
            source.transportedTangentMaxErrorDegrees);
}

float PtSkinnedAttributeDeriveGeometricNormalRmsDegrees(
    const PtSkinnedAttributeDeriveSurveyStats& stats)
{
    return Rms(
        stats.geometricNormalSquaredErrorDegrees,
        stats.cornerSampleCount);
}

float PtSkinnedAttributeDeriveTransportedNormalRmsDegrees(
    const PtSkinnedAttributeDeriveSurveyStats& stats)
{
    return Rms(
        stats.transportedNormalSquaredErrorDegrees,
        stats.cornerSampleCount);
}

float PtSkinnedAttributeDeriveTransportedTangentRmsDegrees(
    const PtSkinnedAttributeDeriveSurveyStats& stats)
{
    return Rms(
        stats.transportedTangentSquaredErrorDegrees,
        stats.cornerSampleCount);
}
