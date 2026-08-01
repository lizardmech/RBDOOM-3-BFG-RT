import upt03_reservoir_contract;
import upt04_surface_contract;
import upt04_path_event_contract;
import upt04_material_contract;
import upt04_material_set_deterministic_probe;
import upt04_random_contract;
import upt04_random_white_hash;

[[vk::binding(0, 0)]] StructuredBuffer<Upt04Surface> gSurfaces;
[[vk::binding(1, 0)]] RWStructuredBuffer<Upt03PackedReservoir> gReservoirs;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.y * 8u + dispatchThreadId.x;
    const Upt04Surface surface = gSurfaces[index];
    Upt03LogicalReservoir reservoir = (Upt03LogicalReservoir)0;
    const Upt04MaterialEvaluation material = Upt04EvaluateDeterministicProbe(surface);
    if (material.valid != 0u)
    {
        Upt04RandomKey key = (Upt04RandomKey)0;
        key.pixelX = dispatchThreadId.x;
        key.pixelY = dispatchThreadId.y;
        key.passNamespace = kUpt04PassInitial;
        key.streamNamespace = kUpt04StreamInitialReservoirSelection;
        key.dimension = kUpt04DimensionInitialReservoirSelection;
        Upt03Candidate candidate = (Upt03Candidate)0;
        candidate.status = 2u;
        candidate.eventKind = kUpt04EventAnalytic;
        candidate.lobeMask = material.lobeMask;
        candidate.flags = 1u;
        candidate.identity0 = surface.materialIdentity | 1u;
        candidate.generationFingerprint = kUpt03ExpectedGeneration;
        candidate.contributionR = material.weightedContribution.x;
        candidate.contributionG = material.weightedContribution.y;
        candidate.contributionB = material.weightedContribution.z;
        candidate.target = material.target;
        candidate.proposalPdf = 1.0f;
        candidate.pathPdf = 1.0f;
        candidate.partialJacobian = 1.0f;
        candidate.accumulatedRouletteProbability = 1.0f;
        Upt03StreamCandidate(reservoir, candidate, Upt04WhiteHash(key));
    }
    gReservoirs[index] = Upt03PackReservoir(reservoir);
}
