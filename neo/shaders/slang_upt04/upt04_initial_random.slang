import upt04_initial_bindings;
import upt04_random_contract;
import upt04_random_white_hash;
import upt04_replay_random;

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
    return Upt04MakeInitialReplayKey(pixel, gUpt04Control.frameSampleIndex);
}
