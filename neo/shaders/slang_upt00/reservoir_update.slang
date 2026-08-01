import upt00_contracts;

[[vk::binding(0, 0)]] StructuredBuffer<Upt00Candidate> gCandidates;
[[vk::binding(1, 0)]] RWStructuredBuffer<Upt00Reservoir> gOutput;

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint outputIndex = dispatchThreadId.x;
    const uint firstCandidate = outputIndex * 4u;

    Upt00Reservoir reservoir = (Upt00Reservoir)0;
    [unroll]
    for (uint candidateIndex = 0u; candidateIndex < 4u; ++candidateIndex)
    {
        const Upt00Candidate candidate = gCandidates[firstCandidate + candidateIndex];
        const bool validPositive = candidate.status == 2u
            && isfinite(candidate.weight)
            && isfinite(candidate.target)
            && candidate.weight > 0.0f
            && candidate.target > 0.0f;
        if (!validPositive)
        {
            continue;
        }

        const float newWeightSum = reservoir.weightSum + candidate.weight;
        const float selection = Upt00UnitFloat(
            Upt00Hash(candidate.sampleId ^ (outputIndex * 0x9e3779b9u) ^ candidateIndex));
        if (selection * newWeightSum < candidate.weight)
        {
            reservoir.selectedTarget = candidate.target;
            reservoir.selectedId = candidate.sampleId;
            reservoir.selectedRadiance = candidate.radiance;
            reservoir.flags = 1u;
        }
        reservoir.weightSum = newWeightSum;
        reservoir.effectiveM += 1u;
    }

    gOutput[outputIndex] = reservoir;
}
