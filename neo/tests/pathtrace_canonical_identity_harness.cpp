#include "PathTraceCanonicalGeometryIdentity.h"

#include <cstdint>
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

PtCanonicalMeshKey MakeMeshKey()
{
    PtCanonicalMeshKey key;
    key.sourceAssetId = 101;
    key.sourceAssetGeneration = 3;
    key.topologySignature = 0x123456789abcdef0ull;
    key.sourceDomain = PtCanonicalMeshSourceDomain::RegisteredRenderModel;
    key.modelSurfaceIndex = 2;
    key.vertexFormat = 7;
    key.deformationClass = PtCanonicalDeformationClass::Rigid;
    key.vertexCount = 120;
    key.indexCount = 360;
    return key;
}

PtCanonicalInstanceKey MakeInstanceKey()
{
    PtCanonicalInstanceKey key;
    key.worldGeneration = 9;
    key.renderDefIndex = 44;
    key.renderDefGeneration = 5;
    key.subInstanceKind = PtCanonicalSubInstanceKind::RigidSurface;
    key.modelSurfaceIndex = 2;
    return key;
}

PtCanonicalIdentityObservation MakeObservation(PtCanonicalIdentityProducer producer)
{
    PtCanonicalIdentityObservation observation;
    observation.producer = producer;
    observation.observationFrame = 100;
    observation.present = true;
    observation.registryEligible = true;
    observation.hasDurableWorld = true;
    observation.hasDurableAsset = true;
    observation.hasDerivedHashes = true;
    observation.world.worldGeneration = 9;
    observation.mesh = MakeMeshKey();
    observation.instance = MakeInstanceKey();
    observation.meshHash = PtHashCanonicalMeshKey(observation.mesh);
    observation.instanceHash = PtHashCanonicalInstanceKey(observation.instance);
    observation.materialBindingRevision = 11;
    observation.storageGeneration = 17;
    return observation;
}

void TestIdentitySeparation()
{
    const PtCanonicalMeshKey mesh = MakeMeshKey();
    const PtCanonicalInstanceKey instance = MakeInstanceKey();
    Expect(PtCanonicalMeshKeyIsValid(mesh), "mesh key should be valid");
    Expect(PtCanonicalInstanceKeyIsValid(instance), "instance key should be valid");
    Expect(PtHashCanonicalMeshKey(mesh) == PtHashCanonicalMeshKey(mesh), "mesh hash should be deterministic");
    Expect(PtHashCanonicalInstanceKey(instance) == PtHashCanonicalInstanceKey(instance), "instance hash should be deterministic");

    PtCanonicalIdentityObservation visible = MakeObservation(PtCanonicalIdentityProducer::VisibleDrawSurf);
    PtCanonicalIdentityObservation lifecycle = MakeObservation(PtCanonicalIdentityProducer::LifecycleFeed);
    Expect(PtCompareCanonicalIdentityObservations(visible, lifecycle) == PT_CANONICAL_IDENTITY_EXACT,
        "same canonical tuples from different producers should agree");

    lifecycle.materialBindingRevision++;
    std::uint32_t flags = PtCompareCanonicalIdentityObservations(visible, lifecycle);
    Expect(flags == PT_CANONICAL_IDENTITY_MATERIAL_ONLY_DIFFERENCE,
        "material revision should not change geometry identity");

    lifecycle = MakeObservation(PtCanonicalIdentityProducer::LifecycleFeed);
    lifecycle.storageGeneration++;
    flags = PtCompareCanonicalIdentityObservations(visible, lifecycle);
    Expect(flags == PT_CANONICAL_IDENTITY_TRANSIENT_STORAGE_ONLY_DIFFERENCE,
        "storage generation should not change geometry identity");

    lifecycle = MakeObservation(PtCanonicalIdentityProducer::LifecycleFeed);
    lifecycle.mesh.sourceAssetId++;
    lifecycle.meshHash = PtHashCanonicalMeshKey(lifecycle.mesh);
    flags = PtCompareCanonicalIdentityObservations(visible, lifecycle);
    Expect((flags & PT_CANONICAL_IDENTITY_SOURCE_ASSET_MISMATCH) != 0,
        "asset change should report source mismatch");
    Expect(visible.instance == lifecycle.instance,
        "mesh swap must not redefine the live instance key");

    lifecycle = MakeObservation(PtCanonicalIdentityProducer::LifecycleFeed);
    lifecycle.instance.renderDefGeneration++;
    lifecycle.instanceHash = PtHashCanonicalInstanceKey(lifecycle.instance);
    flags = PtCompareCanonicalIdentityObservations(visible, lifecycle);
    Expect((flags & PT_CANONICAL_IDENTITY_INSTANCE_SLOT_MISMATCH) != 0,
        "slot generation reuse should report instance mismatch");

    lifecycle = MakeObservation(PtCanonicalIdentityProducer::LifecycleFeed);
    lifecycle.mesh.topologySignature++;
    lifecycle.meshHash = visible.meshHash;
    flags = PtCompareCanonicalIdentityObservations(visible, lifecycle);
    Expect((flags & PT_CANONICAL_IDENTITY_TOPOLOGY_MISMATCH) != 0,
        "topology change should report topology mismatch");
    Expect((flags & PT_CANONICAL_IDENTITY_MESH_HASH_COLLISION) != 0,
        "equal hashes with unequal tuples should report collision");

    lifecycle = MakeObservation(PtCanonicalIdentityProducer::LifecycleFeed);
    lifecycle.hasDurableWorld = false;
    lifecycle.world.worldGeneration = 0;
    flags = PtCompareCanonicalIdentityObservations(visible, lifecycle);
    Expect((flags & PT_CANONICAL_IDENTITY_MISSING_DURABLE_WORLD) != 0,
        "missing world generation should stay explicit");
}

void TestPrimitiveAndHistoryKeys()
{
    PtCanonicalPrimitiveKey primitive;
    primitive.mesh = MakeMeshKey();
    primitive.localPrimitiveIndex = 119;
    Expect(PtCanonicalPrimitiveKeyIsValid(primitive), "last local primitive should be valid");
    primitive.localPrimitiveIndex = 120;
    Expect(!PtCanonicalPrimitiveKeyIsValid(primitive), "primitive beyond topology should be invalid");

    PtCanonicalHistoryOwnerKey primary;
    primary.worldGeneration = 9;
    primary.ownerGeneration = 7;
    primary.ownerKind = PtCanonicalHistoryOwnerKind::RenderTarget;
    primary.ownerRole = PtCanonicalHistoryOwnerRole::PrimaryGameplay;
    Expect(PtCanonicalHistoryOwnerKeyIsValid(primary), "primary history owner should be valid");

    PtCanonicalHistoryOwnerKey subview = primary;
    subview.ownerGeneration = 8;
    subview.ownerRole = PtCanonicalHistoryOwnerRole::Subview;
    Expect(primary != subview, "subview history owner must differ from primary");
    Expect(PtHashCanonicalHistoryOwnerKey(primary) != PtHashCanonicalHistoryOwnerKey(subview),
        "primary and subview history hashes should differ");
}

void TestCheckedStorageArithmetic()
{
    std::uint64_t result = 0;
    Expect(PtCheckedAddU64(10, 20, result) && result == 30, "checked add should succeed");
    Expect(!PtCheckedAddU64(std::numeric_limits<std::uint64_t>::max(), 1, result),
        "checked add should reject overflow");
    Expect(PtCheckedMulU64(12, 16, result) && result == 192, "checked multiply should succeed");
    Expect(!PtCheckedMulU64(std::numeric_limits<std::uint64_t>::max(), 2, result),
        "checked multiply should reject overflow");

    PtCanonicalShaderPageRange input;
    input.pageBaseBytes = 4096;
    input.pageSizeBytes = 4096;
    input.absoluteOffsetBytes = 4352;
    input.byteSize = 512;
    input.elementStrideBytes = 16;
    PtCanonicalShaderRange32 output;
    Expect(PtNarrowCanonicalRangeToShaderPage(input, output) == PtCanonicalShaderNarrowResult::Success,
        "bounded page range should narrow");
    Expect(output.elementOffset == 16 && output.elementCount == 32,
        "narrowed element range should use page-relative units");

    input.absoluteOffsetBytes = 4080;
    Expect(PtNarrowCanonicalRangeToShaderPage(input, output) == PtCanonicalShaderNarrowResult::BeforePage,
        "range before page should be rejected");

    input.absoluteOffsetBytes = 4353;
    Expect(PtNarrowCanonicalRangeToShaderPage(input, output) == PtCanonicalShaderNarrowResult::Misaligned,
        "misaligned range should be rejected");

    input.absoluteOffsetBytes = 8000;
    input.byteSize = 512;
    Expect(PtNarrowCanonicalRangeToShaderPage(input, output) == PtCanonicalShaderNarrowResult::OutsidePage,
        "range beyond page should be rejected");

    input.pageBaseBytes = 0;
    input.pageSizeBytes = (static_cast<std::uint64_t>(UINT32_MAX) + 2ull) * 16ull;
    input.absoluteOffsetBytes = (static_cast<std::uint64_t>(UINT32_MAX) + 1ull) * 16ull;
    input.byteSize = 16;
    Expect(PtNarrowCanonicalRangeToShaderPage(input, output) == PtCanonicalShaderNarrowResult::ShaderWidthExceeded,
        "page-local offset wider than shader ABI should be rejected");
}

}

int main()
{
    TestIdentitySeparation();
    TestPrimitiveAndHistoryKeys();
    TestCheckedStorageArithmetic();
    if (g_failures != 0)
    {
        std::printf("PathTraceCanonicalIdentityHarness: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PathTraceCanonicalIdentityHarness: PASS\n");
    return 0;
}
