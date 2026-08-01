public static const uint kUpt04EventAnalytic = 1u;
public static const uint kUpt04LobeDiffuse = 1u;

public struct Upt04PathEvent
{
    public float3 direction;
    public float pdf;
    public float3 throughput;
    public uint lobeMask;
    public uint valid;
};
