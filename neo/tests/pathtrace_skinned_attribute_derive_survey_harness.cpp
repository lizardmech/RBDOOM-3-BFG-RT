#include "../renderer/NVRHI/PathTraceSkinnedAttributeDeriveSurvey.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

void Set3(float value[3], float x, float y, float z)
{
    value[0] = x;
    value[1] = y;
    value[2] = z;
}

void TestRigidFrameTransportIsExact()
{
    PtSkinnedAttributeDeriveSourceVertex source[3];
    PtSkinnedAttributeDeriveCurrentVertex current[3];
    const std::uint32_t indexes[3] = { 0, 1, 2 };
    Set3(source[0].position, 0.0f, 0.0f, 0.0f);
    Set3(source[1].position, 1.0f, 0.0f, 0.0f);
    Set3(source[2].position, 0.0f, 1.0f, 0.0f);
    for (int vertex = 0; vertex < 3; ++vertex)
    {
        Set3(source[vertex].normal, 0.0f, 0.6f, 0.8f);
        Set3(source[vertex].tangent, 1.0f, 0.0f, 0.0f);
    }

    // Rotate +90 degrees around X: y -> z, z -> -y.
    Set3(current[0].position, 0.0f, 0.0f, 0.0f);
    Set3(current[1].position, 1.0f, 0.0f, 0.0f);
    Set3(current[2].position, 0.0f, 0.0f, 1.0f);
    for (int vertex = 0; vertex < 3; ++vertex)
    {
        Set3(current[vertex].normal, 0.0f, -0.8f, 0.6f);
        Set3(current[vertex].tangent, 1.0f, 0.0f, 0.0f);
    }

    PtSkinnedAttributeDeriveSurveyStats stats;
    PtSurveySkinnedAttributeDerivation(
        source, 3, current, 3, indexes, 3, stats);
    Expect(
        stats.cornerSampleCount == 3 &&
            stats.transportedNormalMaxErrorDegrees < 0.03f &&
            stats.transportedTangentMaxErrorDegrees < 0.03f,
        "rigid triangle-frame transport should preserve smooth basis directions");
    Expect(
        stats.geometricNormalMaxErrorDegrees > 30.0f,
        "plain geometric normal should not reproduce a deliberately smooth normal");
    Expect(
        stats.currentBasisWriteBytes == 144 &&
            stats.candidateCornerDeltaBytes == 24,
        "survey should report exact current-write and candidate-delta bytes");
}

void TestDeformationMismatchIsCounted()
{
    PtSkinnedAttributeDeriveSourceVertex source[3];
    PtSkinnedAttributeDeriveCurrentVertex current[3];
    const std::uint32_t indexes[3] = { 0, 1, 2 };
    Set3(source[0].position, 0.0f, 0.0f, 0.0f);
    Set3(source[1].position, 1.0f, 0.0f, 0.0f);
    Set3(source[2].position, 0.0f, 1.0f, 0.0f);
    Set3(current[0].position, 0.0f, 0.0f, 0.0f);
    Set3(current[1].position, 1.0f, 0.0f, 0.0f);
    Set3(current[2].position, 0.0f, 1.0f, 0.5f);
    for (int vertex = 0; vertex < 3; ++vertex)
    {
        Set3(source[vertex].normal, 0.0f, 0.0f, 1.0f);
        Set3(source[vertex].tangent, 1.0f, 0.0f, 0.0f);
        Set3(current[vertex].normal, 0.0f, 0.0f, 1.0f);
        Set3(current[vertex].tangent, 0.0f, 1.0f, 0.0f);
    }

    PtSkinnedAttributeDeriveSurveyStats stats;
    PtSurveySkinnedAttributeDerivation(
        source, 3, current, 3, indexes, 3, stats);
    Expect(
        stats.transportedNormalOver10Degrees == 3 &&
            stats.transportedTangentOver10Degrees == 3,
        "non-rigid mismatch should enter the declared angular buckets");
}

void TestInvalidTopologyFailsClosed()
{
    PtSkinnedAttributeDeriveSourceVertex source[3];
    PtSkinnedAttributeDeriveCurrentVertex current[3];
    const std::uint32_t indexes[6] = { 0, 1, 7, 0, 0, 0 };
    PtSkinnedAttributeDeriveSurveyStats stats;
    PtSurveySkinnedAttributeDerivation(
        source, 3, current, 3, indexes, 6, stats);
    Expect(
        stats.invalidIndexTriangles == 1 &&
            stats.degenerateBindTriangles == 1 &&
            stats.cornerSampleCount == 0,
        "invalid and degenerate triangles should be named and skipped");
}

void TestAggregateRmsUsesAllCorners()
{
    PtSkinnedAttributeDeriveSurveyStats a;
    a.surfaceCount = 1;
    a.cornerSampleCount = 1;
    a.transportedNormalSquaredErrorDegrees = 4.0;
    a.transportedNormalMaxErrorDegrees = 2.0f;
    PtSkinnedAttributeDeriveSurveyStats b;
    b.surfaceCount = 1;
    b.cornerSampleCount = 3;
    b.transportedNormalSquaredErrorDegrees = 12.0;
    b.transportedNormalMaxErrorDegrees = 3.0f;
    PtAccumulateSkinnedAttributeDeriveSurvey(a, b);
    Expect(
        b.surfaceCount == 2 &&
            b.cornerSampleCount == 4 &&
            std::fabs(
                PtSkinnedAttributeDeriveTransportedNormalRmsDegrees(b) -
                2.0f) < 1.0e-6f &&
            b.transportedNormalMaxErrorDegrees == 3.0f,
        "aggregate RMS should be weighted by exact corner samples");
}

} // namespace

int main()
{
    TestRigidFrameTransportIsExact();
    TestDeformationMismatchIsCounted();
    TestInvalidTopologyFailsClosed();
    TestAggregateRmsUsesAllCorners();

    if (g_failures != 0)
    {
        std::printf(
            "PathTraceSkinnedAttributeDeriveSurveyHarness: %d failure(s)\n",
            g_failures);
        return 1;
    }
    std::printf(
        "PathTraceSkinnedAttributeDeriveSurveyHarness: PASS\n");
    return 0;
}
