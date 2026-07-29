#include "PathTraceGeometryAttributeSurvey.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr float kHalfMax = 65504.0f;
constexpr float kVectorLengthEpsilonSquared = 1.0e-20f;
constexpr float kUnormExactEpsilon = 1.0e-7f;
constexpr float kJointIntegralEpsilon = 255.0f * kUnormExactEpsilon;

static_assert(sizeof(PtGeometrySourcePosition) == 12,
    "GEO-12 survey requires the separate float3 position stream");
static_assert(sizeof(PtGeometrySourceAttribute) == 80,
    "GEO-12 survey byte projections require the full-fidelity attribute ABI");

void InitializeRanges(PtGeometryAttributeSurveyStats& stats)
{
    const float infinity = std::numeric_limits<float>::infinity();
    for (int component = 0; component < 3; ++component)
    {
        stats.positionMin[component] = infinity;
        stats.positionMax[component] = -infinity;
    }
    for (int component = 0; component < 2; ++component)
    {
        stats.texCoordMin[component] = infinity;
        stats.texCoordMax[component] = -infinity;
    }
    stats.skinnedWeightMin = infinity;
    stats.skinnedWeightMax = -infinity;
    stats.skinnedWeightSumMin = infinity;
    stats.skinnedWeightSumMax = -infinity;
    stats.skinnedJointIndexMin = UINT32_MAX;
}

void FinalizeRanges(PtGeometryAttributeSurveyStats& stats)
{
    for (int component = 0; component < 3; ++component)
    {
        if (!std::isfinite(stats.positionMin[component]))
        {
            stats.positionMin[component] = 0.0f;
            stats.positionMax[component] = 0.0f;
        }
    }
    for (int component = 0; component < 2; ++component)
    {
        if (!std::isfinite(stats.texCoordMin[component]))
        {
            stats.texCoordMin[component] = 0.0f;
            stats.texCoordMax[component] = 0.0f;
        }
    }
    if (stats.skinnedJointIndexMin == UINT32_MAX)
    {
        stats.skinnedJointIndexMin = 0;
        stats.skinnedJointIndexMax = 0;
    }
    if (!std::isfinite(stats.skinnedWeightMin))
    {
        stats.skinnedWeightMin = 0.0f;
        stats.skinnedWeightMax = 0.0f;
    }
    if (!std::isfinite(stats.skinnedWeightSumMin))
    {
        stats.skinnedWeightSumMin = 0.0f;
        stats.skinnedWeightSumMax = 0.0f;
    }
}

void InitializeRenderedRanges(PtRenderedGeometrySurveyStats& stats)
{
    InitializeRanges(stats.values);
    const float infinity = std::numeric_limits<float>::infinity();
    for (int component = 0; component < 2; ++component)
    {
        stats.normalMapTexCoordMin[component] = infinity;
        stats.normalMapTexCoordMax[component] = -infinity;
    }
}

void FinalizeRenderedRanges(PtRenderedGeometrySurveyStats& stats)
{
    FinalizeRanges(stats.values);
    for (int component = 0; component < 2; ++component)
    {
        if (!std::isfinite(stats.normalMapTexCoordMin[component]))
        {
            stats.normalMapTexCoordMin[component] = 0.0f;
            stats.normalMapTexCoordMax[component] = 0.0f;
        }
    }
}

std::uint16_t FloatToHalf(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::uint32_t exponent = (bits >> 23) & 0xffu;
    std::uint32_t mantissa = bits & 0x7fffffu;

    if (exponent == 0xffu)
    {
        if (mantissa == 0)
        {
            return static_cast<std::uint16_t>(sign | 0x7c00u);
        }
        return static_cast<std::uint16_t>(
            sign | 0x7c00u | std::max(1u, mantissa >> 13));
    }

    const int halfExponent = static_cast<int>(exponent) - 127 + 15;
    if (halfExponent >= 31)
    {
        return static_cast<std::uint16_t>(sign | 0x7c00u);
    }
    if (halfExponent <= 0)
    {
        if (halfExponent < -10)
        {
            return static_cast<std::uint16_t>(sign);
        }
        mantissa |= 0x800000u;
        const int shift = 14 - halfExponent;
        std::uint32_t halfMantissa = mantissa >> shift;
        const std::uint32_t remainderMask = (1u << shift) - 1u;
        const std::uint32_t remainder = mantissa & remainderMask;
        const std::uint32_t halfway = 1u << (shift - 1);
        if (remainder > halfway ||
            (remainder == halfway && (halfMantissa & 1u) != 0))
        {
            ++halfMantissa;
        }
        return static_cast<std::uint16_t>(sign | halfMantissa);
    }

    std::uint32_t halfMantissa = mantissa >> 13;
    const std::uint32_t remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u ||
        (remainder == 0x1000u && (halfMantissa & 1u) != 0))
    {
        ++halfMantissa;
        if (halfMantissa == 0x400u)
        {
            halfMantissa = 0;
            if (halfExponent + 1 >= 31)
            {
                return static_cast<std::uint16_t>(sign | 0x7c00u);
            }
            return static_cast<std::uint16_t>(
                sign | (static_cast<std::uint32_t>(halfExponent + 1) << 10));
        }
    }
    return static_cast<std::uint16_t>(
        sign |
        (static_cast<std::uint32_t>(halfExponent) << 10) |
        halfMantissa);
}

float HalfToFloat(std::uint16_t value)
{
    const std::uint32_t sign =
        static_cast<std::uint32_t>(value & 0x8000u) << 16;
    std::uint32_t exponent = (value >> 10) & 0x1fu;
    std::uint32_t mantissa = value & 0x3ffu;
    std::uint32_t bits = 0;
    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            bits = sign;
        }
        else
        {
            int unbiasedExponent = -14;
            while ((mantissa & 0x400u) == 0)
            {
                mantissa <<= 1;
                --unbiasedExponent;
            }
            mantissa &= 0x3ffu;
            bits = sign |
                (static_cast<std::uint32_t>(unbiasedExponent + 127) << 23) |
                (mantissa << 13);
        }
    }
    else if (exponent == 0x1fu)
    {
        bits = sign | 0x7f800000u | (mantissa << 13);
    }
    else
    {
        bits = sign | ((exponent - 15u + 127u) << 23) | (mantissa << 13);
    }
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

bool Normalize3(const float value[3], float normalized[3])
{
    const float lengthSquared =
        value[0] * value[0] +
        value[1] * value[1] +
        value[2] * value[2];
    if (!std::isfinite(lengthSquared) ||
        lengthSquared <= kVectorLengthEpsilonSquared)
    {
        return false;
    }
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    for (int component = 0; component < 3; ++component)
    {
        normalized[component] = value[component] * inverseLength;
    }
    return true;
}

float SignNotZero(float value)
{
    return value >= 0.0f ? 1.0f : -1.0f;
}

float Oct16AngularErrorDegrees(const float source[3])
{
    float normal[3];
    if (!Normalize3(source, normal))
    {
        return 0.0f;
    }
    const float inverseL1 = 1.0f /
        (std::fabs(normal[0]) + std::fabs(normal[1]) +
            std::fabs(normal[2]));
    float encoded[2] = {
        normal[0] * inverseL1,
        normal[1] * inverseL1
    };
    if (normal[2] < 0.0f)
    {
        const float oldX = encoded[0];
        encoded[0] = (1.0f - std::fabs(encoded[1])) *
            SignNotZero(oldX);
        encoded[1] = (1.0f - std::fabs(oldX)) *
            SignNotZero(encoded[1]);
    }
    for (int component = 0; component < 2; ++component)
    {
        const float unorm = encoded[component] * 0.5f + 0.5f;
        const std::uint32_t quantized = static_cast<std::uint32_t>(
            std::floor(unorm * 65535.0f + 0.5f));
        encoded[component] =
            (static_cast<float>(quantized) / 65535.0f) * 2.0f - 1.0f;
    }

    float decoded[3] = {
        encoded[0],
        encoded[1],
        1.0f - std::fabs(encoded[0]) - std::fabs(encoded[1])
    };
    if (decoded[2] < 0.0f)
    {
        const float oldX = decoded[0];
        decoded[0] = (1.0f - std::fabs(decoded[1])) *
            SignNotZero(oldX);
        decoded[1] = (1.0f - std::fabs(oldX)) *
            SignNotZero(decoded[1]);
    }
    if (!Normalize3(decoded, decoded))
    {
        return 180.0f;
    }
    const float dot = std::max(
        -1.0f,
        std::min(
            1.0f,
            normal[0] * decoded[0] +
                normal[1] * decoded[1] +
                normal[2] * decoded[2]));
    return std::acos(dot) * (180.0f / 3.14159265358979323846f);
}

float AngularErrorDegrees(const float lhs[3], const float rhs[3])
{
    float normalizedLhs[3];
    float normalizedRhs[3];
    if (!Normalize3(lhs, normalizedLhs) ||
        !Normalize3(rhs, normalizedRhs))
    {
        return 180.0f;
    }
    const float dot = std::max(
        -1.0f,
        std::min(
            1.0f,
            normalizedLhs[0] * normalizedRhs[0] +
                normalizedLhs[1] * normalizedRhs[1] +
                normalizedLhs[2] * normalizedRhs[2]));
    return std::acos(dot) * (180.0f / 3.14159265358979323846f);
}

void ObserveUnorm8(
    float value,
    std::uint64_t& componentCount,
    std::uint64_t& outOfRangeCount,
    std::uint64_t& exactCount,
    float& maxAbsError)
{
    ++componentCount;
    if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
    {
        ++outOfRangeCount;
        return;
    }
    const std::uint32_t quantized = static_cast<std::uint32_t>(
        std::floor(value * 255.0f + 0.5f));
    const float decoded = static_cast<float>(quantized) / 255.0f;
    const float error = std::fabs(decoded - value);
    maxAbsError = std::max(maxAbsError, error);
    if (error <= kUnormExactEpsilon)
    {
        ++exactCount;
    }
}

void ObserveVertex(
    const PtGeometrySourcePosition& position,
    const PtGeometrySourceAttribute& attribute,
    bool skinned,
    PtGeometryAttributeSurveyStats& stats)
{
    ++stats.vertexCount;
    stats.currentPositionBytes += sizeof(PtGeometrySourcePosition);
    stats.currentAttributeBytes += sizeof(PtGeometrySourceAttribute);

    for (int component = 0; component < 3; ++component)
    {
        const float value = position.xyz[component];
        if (!std::isfinite(value))
        {
            ++stats.nonFinitePositionComponents;
            continue;
        }
        stats.positionMin[component] =
            std::min(stats.positionMin[component], value);
        stats.positionMax[component] =
            std::max(stats.positionMax[component], value);
    }

    for (int component = 0; component < 2; ++component)
    {
        const float value = attribute.texCoord[component];
        if (!std::isfinite(value))
        {
            ++stats.nonFiniteTexCoordComponents;
            continue;
        }
        stats.texCoordMin[component] =
            std::min(stats.texCoordMin[component], value);
        stats.texCoordMax[component] =
            std::max(stats.texCoordMax[component], value);
        ++stats.halfTexCoordComponents;
        if (std::fabs(value) > kHalfMax)
        {
            ++stats.halfTexCoordOverflowComponents;
            continue;
        }
        const float decoded = HalfToFloat(FloatToHalf(value));
        if (value != 0.0f && decoded == 0.0f)
        {
            ++stats.halfTexCoordUnderflowToZeroComponents;
        }
        const float absError = std::fabs(decoded - value);
        stats.halfTexCoordMaxAbsError =
            std::max(stats.halfTexCoordMaxAbsError, absError);
        if (value != 0.0f)
        {
            stats.halfTexCoordMaxRelativeError = std::max(
                stats.halfTexCoordMaxRelativeError,
                absError / std::fabs(value));
        }
    }

    const float* basisVectors[] = {
        attribute.normal,
        attribute.tangent,
        attribute.bitangent
    };
    bool basisFinite[3] = { true, true, true };
    for (int vectorIndex = 0; vectorIndex < 3; ++vectorIndex)
    {
        for (int component = 0; component < 3; ++component)
        {
            if (!std::isfinite(basisVectors[vectorIndex][component]))
            {
                ++stats.nonFiniteBasisComponents;
                basisFinite[vectorIndex] = false;
            }
        }
    }

    float normalized[3];
    if (!basisFinite[0] ||
        !Normalize3(attribute.normal, normalized))
    {
        ++stats.normalDegenerateVertices;
    }
    else
    {
        ++stats.normalOct16Samples;
        stats.normalOct16MaxAngularErrorDegrees = std::max(
            stats.normalOct16MaxAngularErrorDegrees,
            Oct16AngularErrorDegrees(attribute.normal));
    }
    if (!basisFinite[1] ||
        !Normalize3(attribute.tangent, normalized))
    {
        ++stats.tangentDegenerateVertices;
    }
    else
    {
        ++stats.tangentOct16Samples;
        stats.tangentOct16MaxAngularErrorDegrees = std::max(
            stats.tangentOct16MaxAngularErrorDegrees,
            Oct16AngularErrorDegrees(attribute.tangent));
    }
    if (!basisFinite[2] ||
        !Normalize3(attribute.bitangent, normalized))
    {
        ++stats.bitangentDegenerateVertices;
    }

    if (basisFinite[0] && basisFinite[1] && basisFinite[2] &&
        std::isfinite(attribute.bitangentSign) &&
        std::fabs(std::fabs(attribute.bitangentSign) - 1.0f) <=
            kUnormExactEpsilon)
    {
        float normal[3];
        float tangent[3];
        float bitangent[3];
        if (Normalize3(attribute.normal, normal) &&
            Normalize3(attribute.tangent, tangent) &&
            Normalize3(attribute.bitangent, bitangent))
        {
            const float reconstructed[3] = {
                (normal[1] * tangent[2] - normal[2] * tangent[1]) *
                    attribute.bitangentSign,
                (normal[2] * tangent[0] - normal[0] * tangent[2]) *
                    attribute.bitangentSign,
                (normal[0] * tangent[1] - normal[1] * tangent[0]) *
                    attribute.bitangentSign
            };
            ++stats.bitangentReconstructionSamples;
            stats.bitangentReconstructionMaxAngularErrorDegrees = std::max(
                stats.bitangentReconstructionMaxAngularErrorDegrees,
                AngularErrorDegrees(reconstructed, bitangent));
        }
        else
        {
            ++stats.bitangentReconstructionInvalid;
        }
    }
    else
    {
        ++stats.bitangentReconstructionInvalid;
    }

    for (int component = 0; component < 4; ++component)
    {
        const float color = attribute.color[component];
        const float color2 = attribute.color2[component];
        if (!std::isfinite(color))
        {
            ++stats.nonFiniteColorComponents;
        }
        if (!std::isfinite(color2))
        {
            ++stats.nonFiniteColorComponents;
        }
        ObserveUnorm8(
            color,
            stats.colorComponents,
            stats.colorOutOfUnormRangeComponents,
            stats.colorUnorm8ExactComponents,
            stats.colorUnorm8MaxAbsError);
        ObserveUnorm8(
            color2,
            stats.color2Components,
            stats.color2OutOfUnormRangeComponents,
            stats.color2Unorm8ExactComponents,
            stats.color2Unorm8MaxAbsError);
    }

    if (!skinned)
    {
        return;
    }
    ++stats.skinnedVertexCount;
    float weightSum = 0.0f;
    bool finiteWeightSum = true;
    for (int component = 0; component < 4; ++component)
    {
        ++stats.skinnedJointComponents;
        const float scaledJoint = attribute.color[component] * 255.0f;
        if (!std::isfinite(scaledJoint) ||
            scaledJoint < 0.0f ||
            scaledJoint > 255.0f)
        {
            ++stats.skinnedJointOutOfByteRangeComponents;
        }
        else
        {
            const float rounded = std::floor(scaledJoint + 0.5f);
            if (std::fabs(scaledJoint - rounded) > kJointIntegralEpsilon)
            {
                ++stats.skinnedJointNonIntegralComponents;
            }
            const std::uint32_t jointIndex =
                static_cast<std::uint32_t>(rounded);
            stats.skinnedJointIndexMin =
                std::min(stats.skinnedJointIndexMin, jointIndex);
            stats.skinnedJointIndexMax =
                std::max(stats.skinnedJointIndexMax, jointIndex);
        }

        const float weight = attribute.color2[component];
        if (!std::isfinite(weight))
        {
            ++stats.skinnedWeightNonFiniteComponents;
            finiteWeightSum = false;
            continue;
        }
        stats.skinnedWeightMin =
            std::min(stats.skinnedWeightMin, weight);
        stats.skinnedWeightMax =
            std::max(stats.skinnedWeightMax, weight);
        weightSum += weight;
    }
    if (finiteWeightSum)
    {
        stats.skinnedWeightSumMin =
            std::min(stats.skinnedWeightSumMin, weightSum);
        stats.skinnedWeightSumMax =
            std::max(stats.skinnedWeightSumMax, weightSum);
    }
}

void ObserveHalfComponent(
    float value,
    std::uint64_t& components,
    std::uint64_t& overflowComponents,
    std::uint64_t& underflowComponents,
    float& maxAbsError,
    float& maxRelativeError)
{
    ++components;
    if (std::fabs(value) > kHalfMax)
    {
        ++overflowComponents;
        return;
    }
    const float decoded = HalfToFloat(FloatToHalf(value));
    if (value != 0.0f && decoded == 0.0f)
    {
        ++underflowComponents;
    }
    const float absError = std::fabs(decoded - value);
    maxAbsError = std::max(maxAbsError, absError);
    if (value != 0.0f)
    {
        maxRelativeError = std::max(
            maxRelativeError,
            absError / std::fabs(value));
    }
}

void MergeStats(
    const PtGeometryAttributeSurveyStats& source,
    PtGeometryAttributeSurveyStats& destination)
{
    destination.recordCount += source.recordCount;
    destination.rigidRecordCount += source.rigidRecordCount;
    destination.skinnedRecordCount += source.skinnedRecordCount;
    destination.vertexCount += source.vertexCount;
    destination.currentPositionBytes += source.currentPositionBytes;
    destination.currentAttributeBytes += source.currentAttributeBytes;
    for (int component = 0; component < 3; ++component)
    {
        destination.positionMin[component] = std::min(
            destination.positionMin[component],
            source.positionMin[component]);
        destination.positionMax[component] = std::max(
            destination.positionMax[component],
            source.positionMax[component]);
    }
    for (int component = 0; component < 2; ++component)
    {
        destination.texCoordMin[component] = std::min(
            destination.texCoordMin[component],
            source.texCoordMin[component]);
        destination.texCoordMax[component] = std::max(
            destination.texCoordMax[component],
            source.texCoordMax[component]);
    }
#define PT_MERGE_COUNT(field) destination.field += source.field
    PT_MERGE_COUNT(nonFinitePositionComponents);
    PT_MERGE_COUNT(nonFiniteTexCoordComponents);
    PT_MERGE_COUNT(nonFiniteBasisComponents);
    PT_MERGE_COUNT(nonFiniteColorComponents);
    PT_MERGE_COUNT(halfTexCoordComponents);
    PT_MERGE_COUNT(halfTexCoordOverflowComponents);
    PT_MERGE_COUNT(halfTexCoordUnderflowToZeroComponents);
    PT_MERGE_COUNT(normalDegenerateVertices);
    PT_MERGE_COUNT(tangentDegenerateVertices);
    PT_MERGE_COUNT(bitangentDegenerateVertices);
    PT_MERGE_COUNT(normalOct16Samples);
    PT_MERGE_COUNT(tangentOct16Samples);
    PT_MERGE_COUNT(bitangentReconstructionSamples);
    PT_MERGE_COUNT(bitangentReconstructionInvalid);
    PT_MERGE_COUNT(colorComponents);
    PT_MERGE_COUNT(colorOutOfUnormRangeComponents);
    PT_MERGE_COUNT(colorUnorm8ExactComponents);
    PT_MERGE_COUNT(color2Components);
    PT_MERGE_COUNT(color2OutOfUnormRangeComponents);
    PT_MERGE_COUNT(color2Unorm8ExactComponents);
    PT_MERGE_COUNT(skinnedVertexCount);
    PT_MERGE_COUNT(skinnedJointComponents);
    PT_MERGE_COUNT(skinnedJointNonIntegralComponents);
    PT_MERGE_COUNT(skinnedJointOutOfByteRangeComponents);
    PT_MERGE_COUNT(skinnedWeightNonFiniteComponents);
#undef PT_MERGE_COUNT
    destination.halfTexCoordMaxAbsError = std::max(
        destination.halfTexCoordMaxAbsError,
        source.halfTexCoordMaxAbsError);
    destination.halfTexCoordMaxRelativeError = std::max(
        destination.halfTexCoordMaxRelativeError,
        source.halfTexCoordMaxRelativeError);
    destination.normalOct16MaxAngularErrorDegrees = std::max(
        destination.normalOct16MaxAngularErrorDegrees,
        source.normalOct16MaxAngularErrorDegrees);
    destination.tangentOct16MaxAngularErrorDegrees = std::max(
        destination.tangentOct16MaxAngularErrorDegrees,
        source.tangentOct16MaxAngularErrorDegrees);
    destination.bitangentReconstructionMaxAngularErrorDegrees = std::max(
        destination.bitangentReconstructionMaxAngularErrorDegrees,
        source.bitangentReconstructionMaxAngularErrorDegrees);
    destination.colorUnorm8MaxAbsError = std::max(
        destination.colorUnorm8MaxAbsError,
        source.colorUnorm8MaxAbsError);
    destination.color2Unorm8MaxAbsError = std::max(
        destination.color2Unorm8MaxAbsError,
        source.color2Unorm8MaxAbsError);
    if (source.skinnedVertexCount != 0)
    {
        destination.skinnedJointIndexMin = std::min(
            destination.skinnedJointIndexMin,
            source.skinnedJointIndexMin);
        destination.skinnedJointIndexMax = std::max(
            destination.skinnedJointIndexMax,
            source.skinnedJointIndexMax);
        destination.skinnedWeightMin = std::min(
            destination.skinnedWeightMin,
            source.skinnedWeightMin);
        destination.skinnedWeightMax = std::max(
            destination.skinnedWeightMax,
            source.skinnedWeightMax);
        destination.skinnedWeightSumMin = std::min(
            destination.skinnedWeightSumMin,
            source.skinnedWeightSumMin);
        destination.skinnedWeightSumMax = std::max(
            destination.skinnedWeightSumMax,
            source.skinnedWeightSumMax);
    }
}

void MergeRenderedStats(
    const PtRenderedGeometrySurveyStats& source,
    PtRenderedGeometrySurveyStats& destination)
{
    MergeStats(source.values, destination.values);
    for (int component = 0; component < 2; ++component)
    {
        destination.normalMapTexCoordMin[component] = std::min(
            destination.normalMapTexCoordMin[component],
            source.normalMapTexCoordMin[component]);
        destination.normalMapTexCoordMax[component] = std::max(
            destination.normalMapTexCoordMax[component],
            source.normalMapTexCoordMax[component]);
    }
    destination.nonFiniteNormalMapTexCoordComponents +=
        source.nonFiniteNormalMapTexCoordComponents;
    destination.halfNormalMapTexCoordComponents +=
        source.halfNormalMapTexCoordComponents;
    destination.halfNormalMapTexCoordOverflowComponents +=
        source.halfNormalMapTexCoordOverflowComponents;
    destination.halfNormalMapTexCoordUnderflowToZeroComponents +=
        source.halfNormalMapTexCoordUnderflowToZeroComponents;
    destination.halfNormalMapTexCoordMaxAbsError = std::max(
        destination.halfNormalMapTexCoordMaxAbsError,
        source.halfNormalMapTexCoordMaxAbsError);
    destination.halfNormalMapTexCoordMaxRelativeError = std::max(
        destination.halfNormalMapTexCoordMaxRelativeError,
        source.halfNormalMapTexCoordMaxRelativeError);
}

}

void PtSurveyGeometrySourceRegistry(
    const PtGeometrySourceRegistry& registry,
    PtGeometryAttributeSurvey& survey)
{
    survey = PtGeometryAttributeSurvey();
    InitializeRanges(survey.totals);
    survey.records.reserve(registry.RecordCount());

    for (std::size_t recordIndex = 0;
        recordIndex < registry.RecordCount();
        ++recordIndex)
    {
        const PtGeometrySourceRecord* source =
            registry.RecordAt(recordIndex);
        if (source == nullptr)
        {
            continue;
        }

        PtGeometryAttributeSurveyRecord record;
        record.meshHash = source->meshHash;
        record.sourceDomain = source->key.sourceDomain;
        record.deformationClass = source->key.deformationClass;
        record.modelSurfaceIndex = source->key.modelSurfaceIndex;
        InitializeRanges(record.stats);
        record.stats.recordCount = 1;
        const bool skinned =
            source->key.sourceDomain ==
                PtCanonicalMeshSourceDomain::SkinnedBindSource &&
            source->key.deformationClass ==
                PtCanonicalDeformationClass::Skinned;
        record.stats.skinnedRecordCount = skinned ? 1 : 0;
        record.stats.rigidRecordCount = skinned ? 0 : 1;

        const std::size_t vertexCount = std::min(
            source->payload.positions.size(),
            source->payload.AttributeCount());
        for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
        {
            PtGeometrySourceAttribute attribute;
            if (!source->payload.DecodeAttribute(vertex, attribute))
            {
                continue;
            }
            ObserveVertex(
                source->payload.positions[vertex],
                attribute,
                skinned,
                record.stats);
        }
        FinalizeRanges(record.stats);
        MergeStats(record.stats, survey.totals);
        survey.records.push_back(record);
    }
    FinalizeRanges(survey.totals);
}

void PtBeginRenderedGeometrySurvey(PtRenderedGeometrySurvey& survey)
{
    survey = PtRenderedGeometrySurvey();
    InitializeRenderedRanges(survey.totals);
}

void PtAppendRenderedGeometrySurveyRecord(
    const PtRenderedGeometrySurveyRecord& metadata,
    const PtRenderedGeometrySurveyVertex* vertices,
    std::size_t vertexCount,
    PtRenderedGeometrySurvey& survey)
{
    if (vertices == nullptr || vertexCount == 0)
    {
        ++survey.invalidRangeRecordCount;
        return;
    }

    PtRenderedGeometrySurveyRecord record = metadata;
    record.stats = PtRenderedGeometrySurveyStats();
    InitializeRenderedRanges(record.stats);
    record.stats.values.recordCount = 1;
    const bool skinned =
        record.surfaceClassId == 2u;
    record.stats.values.skinnedRecordCount = skinned ? 1 : 0;
    record.stats.values.rigidRecordCount = skinned ? 0 : 1;

    for (std::size_t vertexIndex = 0;
        vertexIndex < vertexCount;
        ++vertexIndex)
    {
        const PtRenderedGeometrySurveyVertex& vertex =
            vertices[vertexIndex];
        PtGeometrySourcePosition position;
        PtGeometrySourceAttribute attribute;
        std::memcpy(position.xyz, vertex.position, sizeof(position.xyz));
        std::memcpy(attribute.normal, vertex.normal, sizeof(attribute.normal));
        std::memcpy(
            attribute.texCoord,
            vertex.texCoord,
            sizeof(attribute.texCoord));
        std::memcpy(attribute.color, vertex.color, sizeof(attribute.color));
        std::memcpy(attribute.color2, vertex.color2, sizeof(attribute.color2));
        std::memcpy(
            attribute.tangent,
            vertex.tangent,
            sizeof(attribute.tangent));
        std::memcpy(
            attribute.bitangent,
            vertex.bitangent,
            sizeof(attribute.bitangent));
        float normal[3];
        float tangent[3];
        attribute.bitangentSign =
            Normalize3(attribute.normal, normal) &&
                Normalize3(attribute.tangent, tangent)
                ? SignNotZero(
                    (normal[1] * tangent[2] -
                        normal[2] * tangent[1]) *
                            attribute.bitangent[0] +
                    (normal[2] * tangent[0] -
                        normal[0] * tangent[2]) *
                            attribute.bitangent[1] +
                    (normal[0] * tangent[1] -
                        normal[1] * tangent[0]) *
                            attribute.bitangent[2])
                : 0.0f;
        ObserveVertex(
            position,
            attribute,
            skinned,
            record.stats.values);
        // The rendered ABI is 112 bytes. ObserveVertex accounts for the
        // canonical 12+80 layout, so retain the exact 20-byte delta here.
        record.stats.values.currentAttributeBytes += 20;

        for (int component = 0; component < 2; ++component)
        {
            const float value = vertex.texCoord[component + 2];
            if (!std::isfinite(value))
            {
                ++record.stats.
                    nonFiniteNormalMapTexCoordComponents;
                continue;
            }
            record.stats.normalMapTexCoordMin[component] =
                std::min(
                    record.stats.normalMapTexCoordMin[component],
                    value);
            record.stats.normalMapTexCoordMax[component] =
                std::max(
                    record.stats.normalMapTexCoordMax[component],
                    value);
            ObserveHalfComponent(
                value,
                record.stats.halfNormalMapTexCoordComponents,
                record.stats.
                    halfNormalMapTexCoordOverflowComponents,
                record.stats.
                    halfNormalMapTexCoordUnderflowToZeroComponents,
                record.stats.halfNormalMapTexCoordMaxAbsError,
                record.stats.halfNormalMapTexCoordMaxRelativeError);
        }
    }

    FinalizeRenderedRanges(record.stats);
    MergeRenderedStats(record.stats, survey.totals);
    if (record.domain ==
        PtRenderedGeometrySurveyDomain::StaticResident)
    {
        ++survey.staticRecordCount;
    }
    else if (record.domain ==
        PtRenderedGeometrySurveyDomain::DynamicFallback)
    {
        ++survey.dynamicRecordCount;
    }
    survey.records.push_back(record);
}

void PtRecordRenderedGeometrySurveyInvalidRange(
    PtRenderedGeometrySurvey& survey)
{
    ++survey.invalidRangeRecordCount;
}
