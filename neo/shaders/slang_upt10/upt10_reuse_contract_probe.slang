import upt10_reuse_contract;

[[vk::binding(0, 0)]] StructuredBuffer<Upt10MappedProposalInput> gProposals;
[[vk::binding(1, 0)]] StructuredBuffer<Upt10MergeControl> gControls;
[[vk::binding(2, 0)]] RWStructuredBuffer<Upt10MergeResult> gResults;

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint pairIndex = dispatchThreadId.x;
    if (pairIndex >= 8u)
        return;

    gResults[pairIndex] = Upt10MergeMappedPair(
        gProposals[pairIndex * 2u],
        gProposals[pairIndex * 2u + 1u],
        gControls[pairIndex]);
}
