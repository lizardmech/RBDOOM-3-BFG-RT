import upt04_initial_bindings;
import upt04_random_contract;
import upt04_random_white_hash;

public Upt04RandomKey Upt04InitialRandomKeyAtVertex(
    uint2 pixel,
    uint pathVertex,
    uint streamNamespace,
    uint dimension)
{
    Upt04RandomKey key = (Upt04RandomKey)0;
    key.pixelX = pixel.x;
    key.pixelY = pixel.y;
    key.frameSampleIndex = gUpt04Control.frameSampleIndex;
    key.passNamespace = kUpt04PassInitial;
    key.pathVertex = pathVertex;
    key.streamNamespace = streamNamespace;
    key.dimension = dimension;
    return key;
}

public Upt04RandomKey Upt04InitialRandomKeyAtVertexSample(
    uint2 pixel,
    uint pathVertex,
    uint sampleOrdinal,
    uint streamNamespace,
    uint dimension)
{
    Upt04RandomKey key = Upt04InitialRandomKeyAtVertex(
        pixel, pathVertex, streamNamespace, dimension);
    key.sampleOrdinal = sampleOrdinal;
    return key;
}

public Upt04RandomKey Upt04InitialRandomKey(
    uint2 pixel,
    uint streamNamespace,
    uint dimension)
{
    return Upt04InitialRandomKeyAtVertex(
        pixel,
        0u,
        streamNamespace,
        dimension);
}

public uint Upt04InitialReplayKey(uint2 pixel)
{
    const Upt04RandomKey key = Upt04InitialRandomKey(
        pixel,
        kUpt04StreamReplayPathEvent,
        kUpt04DimensionReplayPathEvent);
    uint value = Upt04HashWord(key.pixelX ^ (key.pixelY * 0x9e3779b9u));
    value = Upt04HashWord(value ^ key.frameSampleIndex);
    value = Upt04HashWord(value ^ key.streamNamespace ^ key.dimension);
    // High bit belongs to UPT-09's rescue stamp. Keep the base identity
    // nonzero so UPT-08 can use zero as the invalid/no-sample sentinel.
    return (value & 0x7fffffffu) | 1u;
}
