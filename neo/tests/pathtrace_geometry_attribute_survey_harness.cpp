#include "../renderer/NVRHI/PathTraceGeometryAttributeSurvey.h"

#include <cmath>
#include <cstdio>
#include <limits>

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

struct TestPayload
{
    PtGeometrySourcePosition positions[2];
    PtGeometrySourceAttribute attributes[2];
    std::uint32_t indexes[3] = { 0, 1, 0 };
    PtGeometrySourceTriangle triangles[1];

    TestPayload()
    {
        positions[0].xyz[0] = -8.0f;
        positions[0].xyz[1] = 2.0f;
        positions[0].xyz[2] = 4.0f;
        positions[1].xyz[0] = 12.0f;
        positions[1].xyz[1] = 3.0f;
        positions[1].xyz[2] = 9.0f;
        for (int vertex = 0; vertex < 2; ++vertex)
        {
            attributes[vertex].normal[2] = 1.0f;
            attributes[vertex].tangent[0] = 1.0f;
            attributes[vertex].bitangent[1] = 1.0f;
            attributes[vertex].bitangentSign = 1.0f;
            attributes[vertex].texCoord[0] =
                vertex == 0 ? -2.25f : 70000.0f;
            attributes[vertex].texCoord[1] =
                vertex == 0 ? 0.125f : 0.33333334f;
            attributes[vertex].color[0] = 1.0f;
            attributes[vertex].color[1] = 128.0f / 255.0f;
            attributes[vertex].color2[0] = 0.5f;
            attributes[vertex].color2[1] = 0.25f;
            attributes[vertex].color2[2] = 0.25f;
        }
    }

    PtGeometrySourcePayloadView View() const
    {
        PtGeometrySourcePayloadView view;
        view.positions = positions;
        view.positionCount = 2;
        view.attributes = attributes;
        view.attributeCount = 2;
        view.indexes = indexes;
        view.indexCount = 3;
        view.triangles = triangles;
        view.triangleCount = 1;
        return view;
    }
};

PtCanonicalMeshKey MakeKey(bool skinned, std::uint64_t assetId)
{
    PtCanonicalMeshKey key;
    key.sourceAssetId = assetId;
    key.sourceAssetGeneration = 1;
    key.topologySignature = assetId * 17;
    key.sourceDomain = skinned
        ? PtCanonicalMeshSourceDomain::SkinnedBindSource
        : PtCanonicalMeshSourceDomain::RegisteredRenderModel;
    key.modelSurfaceIndex = 0;
    key.vertexFormat = 1;
    key.deformationClass = skinned
        ? PtCanonicalDeformationClass::Skinned
        : PtCanonicalDeformationClass::Rigid;
    key.vertexCount = 2;
    key.indexCount = 3;
    return key;
}

void TestFullFidelitySurveyIsObservational()
{
    PtGeometrySourceRegistry registry;
    TestPayload rigid;
    const PtGeometrySourcePayloadView rigidView = rigid.View();
    Expect(
        registry.Observe(MakeKey(false, 11), 1, &rigidView) ==
            PtGeometrySourceObserveResult::Added,
        "rigid survey setup should add");

    TestPayload skinned;
    skinned.attributes[0].color[0] = 17.0f / 255.0f;
    skinned.attributes[0].color[1] = 42.0f / 255.0f;
    skinned.attributes[1].color[0] = 255.0f / 255.0f;
    skinned.attributes[1].color[1] = 3.0f / 255.0f;
    const PtGeometrySourcePayloadView skinnedView = skinned.View();
    Expect(
        registry.Observe(MakeKey(true, 12), 1, &skinnedView) ==
            PtGeometrySourceObserveResult::Added,
        "skinned survey setup should add");

    const std::uint64_t checksumBefore =
        registry.Find(MakeKey(false, 11))->sourceChecksum;
    PtGeometryAttributeSurvey survey;
    PtSurveyGeometrySourceRegistry(registry, survey);

    Expect(
        survey.records.size() == 2 &&
            survey.totals.recordCount == 2 &&
            survey.totals.rigidRecordCount == 1 &&
            survey.totals.skinnedRecordCount == 1,
        "survey should classify every canonical source record");
    Expect(
        survey.totals.vertexCount == 4 &&
            survey.totals.currentPositionBytes ==
                4 * sizeof(PtGeometrySourcePosition) &&
            survey.totals.currentAttributeBytes ==
                4 * sizeof(PtGeometrySourceAttribute),
        "survey should measure exact full-fidelity source bytes");
    Expect(
        survey.totals.positionMin[0] == -8.0f &&
            survey.totals.positionMax[0] == 12.0f &&
            survey.totals.texCoordMin[0] == -2.25f &&
            survey.totals.texCoordMax[0] == 70000.0f,
        "survey should retain position and wide-UV ranges without clamping");
    Expect(
        survey.totals.halfTexCoordOverflowComponents == 2 &&
            survey.totals.halfTexCoordMaxAbsError > 0.0f,
        "half survey should reject wide components and report finite error");
    Expect(
        survey.totals.normalDegenerateVertices == 0 &&
            survey.totals.tangentDegenerateVertices == 0 &&
            survey.totals.bitangentDegenerateVertices == 0 &&
            survey.totals.bitangentReconstructionInvalid == 0 &&
            survey.totals.bitangentReconstructionMaxAngularErrorDegrees <
                0.01f,
        "orthonormal source basis should reconstruct exactly");
    Expect(
        survey.totals.colorOutOfUnormRangeComponents == 0 &&
            survey.totals.colorUnorm8ExactComponents ==
                survey.totals.colorComponents &&
            survey.totals.colorUnorm8MaxAbsError < 1.0e-7f,
        "byte-derived color should prove exact UNORM8 round trip");
    Expect(
        survey.totals.skinnedVertexCount == 2 &&
            survey.totals.skinnedJointNonIntegralComponents == 0 &&
            survey.totals.skinnedJointOutOfByteRangeComponents == 0 &&
            survey.totals.skinnedJointIndexMin == 0 &&
            survey.totals.skinnedJointIndexMax == 255 &&
            survey.totals.skinnedWeightSumMin == 1.0f &&
            survey.totals.skinnedWeightSumMax == 1.0f,
        "skinned survey should recover exact byte joints and normalized weights");
    Expect(
        registry.Find(MakeKey(false, 11))->sourceChecksum == checksumBefore &&
            registry.Stats().payloadCopies == 2,
        "survey must not mutate or republish source records");
}

void TestFailuresRemainVisible()
{
    PtGeometrySourceRegistry registry;
    TestPayload payload;
    payload.positions[0].xyz[1] =
        std::numeric_limits<float>::infinity();
    payload.attributes[0].normal[0] =
        std::numeric_limits<float>::quiet_NaN();
    payload.attributes[0].color[0] = 1.5f;
    payload.attributes[0].color2[0] =
        std::numeric_limits<float>::quiet_NaN();
    const PtGeometrySourcePayloadView view = payload.View();
    Expect(
        registry.Observe(MakeKey(false, 21), 1, &view) ==
            PtGeometrySourceObserveResult::Added,
        "invalid numeric survey setup should remain value-owned");

    PtGeometryAttributeSurvey survey;
    PtSurveyGeometrySourceRegistry(registry, survey);
    Expect(
        survey.totals.nonFinitePositionComponents == 1 &&
            survey.totals.nonFiniteBasisComponents == 1 &&
            survey.totals.nonFiniteColorComponents == 1,
        "survey should count non-finite source components");
    Expect(
        survey.totals.colorOutOfUnormRangeComponents == 1 &&
            survey.totals.color2OutOfUnormRangeComponents == 1 &&
            survey.totals.normalDegenerateVertices == 1,
        "candidate failures should remain explicit instead of clamping");
}

}

int main()
{
    TestFullFidelitySurveyIsObservational();
    TestFailuresRemainVisible();
    if (g_failures != 0)
    {
        std::printf(
            "PathTraceGeometryAttributeSurveyHarness: %d failure(s)\n",
            g_failures);
        return 1;
    }
    std::printf("PathTraceGeometryAttributeSurveyHarness: PASS\n");
    return 0;
}
