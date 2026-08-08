import upt04_random_contract;
import upt04_random_white_hash;

// Pure replay-seed construction shared by D0 and reuse passes. Keeping this
// module free of pass bindings is what makes persisted replay independently
// verifiable and usable after receiver reprojection.
public uint Upt04MakeInitialReplayKey(uint2 pixel, uint frameSampleIndex)
{
    uint value = Upt04HashWord(pixel.x ^ (pixel.y * 0x9e3779b9u));
    value = Upt04HashWord(value ^ frameSampleIndex);
    value = Upt04HashWord(value ^ kUpt04StreamReplayPathEvent
        ^ kUpt04DimensionReplayPathEvent);
    // High bit belongs to UPT-09's rescue stamp. Keep the base identity
    // nonzero so UPT-08 can use zero as the invalid/no-sample sentinel.
    return (value & 0x7fffffffu) | 1u;
}

// Global candidates must be reconstructible after they move to another
// receiver. Use only the two words persisted in the reservoir as the PSS
// seed; the original screen pixel is deliberately absent. UPT-09 owns the
// replayKey high bit as a direct-rescue stamp, so it cannot perturb replay.
public Upt04RandomKey Upt04PersistedReplayRandomKey(
    uint replayKey,
    uint replayIndex,
    uint pathVertex,
    uint sampleOrdinal,
    uint streamNamespace,
    uint dimension)
{
    Upt04RandomKey key = (Upt04RandomKey)0;
    key.pixelX = replayKey & 0x7fffffffu;
    key.pixelY = replayIndex;
    key.passNamespace = kUpt04PassInitial;
    key.pathVertex = pathVertex;
    key.streamNamespace = streamNamespace;
    key.dimension = dimension;
    key.sampleOrdinal = sampleOrdinal;
    return key;
}
