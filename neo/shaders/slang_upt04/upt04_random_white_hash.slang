import upt04_random_contract;

public uint Upt04HashWord(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

public float Upt04WhiteHash(Upt04RandomKey key)
{
    uint value = 0x9e3779b9u;
    value = Upt04HashWord(value ^ key.pixelX);
    value = Upt04HashWord(value ^ key.pixelY);
    value = Upt04HashWord(value ^ key.frameSampleIndex);
    value = Upt04HashWord(value ^ key.passNamespace);
    value = Upt04HashWord(value ^ key.pathVertex);
    value = Upt04HashWord(value ^ key.streamNamespace);
    value = Upt04HashWord(value ^ key.dimension);
    value = Upt04HashWord(value ^ key.sampleOrdinal);
    value = Upt04HashWord(value ^ key.replayEpoch);
    return float(value >> 8u) * (1.0f / 16777216.0f);
}
