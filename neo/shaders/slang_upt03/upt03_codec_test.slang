import upt03_reservoir_contract;
import upt03_test_random_provider;

[[vk::binding(0, 0)]] StructuredBuffer<Upt03Candidate> gCandidates;
[[vk::binding(1, 0)]] RWStructuredBuffer<Upt03PackedReservoir> gOutput;

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint outputIndex = dispatchThreadId.x;
    if (outputIndex >= 8u)
        return;

    Upt03LogicalReservoir reservoir = (Upt03LogicalReservoir)0;
    [unroll]
    for (uint candidateIndex = 0u; candidateIndex < 4u; ++candidateIndex)
    {
        const Upt03Candidate candidate = gCandidates[outputIndex * 4u + candidateIndex];
        Upt03StreamCandidate(reservoir, candidate, Upt03TestWinnerRandom(candidate));
    }
    gOutput[outputIndex] = Upt03PackReservoir(reservoir);
}
