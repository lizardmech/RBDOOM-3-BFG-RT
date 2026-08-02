import upt04_material_live_contract;

struct Upt04CompactMaterialPackControl
{
    uint materialCount;
};

[[vk::binding(0, 0)]] StructuredBuffer<Upt04SmokeMaterialRecord> gMaterialSource;
[[vk::binding(1, 0)]] RWStructuredBuffer<Upt04CompactMaterialRecord> gCompactMaterials;
[[vk::push_constant]] Upt04CompactMaterialPackControl gPackControl;

[shader("compute")]
[numthreads(128, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint materialIndex = dispatchThreadId.x;
    if (materialIndex < gPackControl.materialCount)
        gCompactMaterials[materialIndex] =
            Upt04PackCompactMaterial(gMaterialSource[materialIndex]);
}
