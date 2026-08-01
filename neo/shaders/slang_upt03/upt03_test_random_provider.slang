import upt03_reservoir_contract;

// UPT-03 composes this deterministic provider at the entry point.  Production
// white/hash and optional STBN providers can replace it without changing the
// reservoir module or packed output ABI.
public float Upt03TestWinnerRandom(Upt03Candidate candidate)
{
    return candidate.selectionRandom;
}
