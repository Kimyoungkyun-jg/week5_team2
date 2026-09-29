Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> DestHZB : register(u0);

cbuffer HZBBuildConstants : register(b0)
{
    uint2 DestSize;
    uint2 SourceSize;
    uint bIsFirstPass;
    uint Pad;
};

[numthreads(16, 16, 1)]
void mainCS(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    if (DispatchThreadID.x >= DestSize.x || DispatchThreadID.y >= DestSize.y)
        return;

    uint2 MaxCoord = SourceSize - 1;
    float MaxDepth;

    if (bIsFirstPass != 0)
    {
        float2 Scale = float2(SourceSize) / float2(DestSize);
        uint2 SrcCoord = uint2(float2(DispatchThreadID.xy) * Scale);
        uint2 Step = max(uint2(1, 1), uint2(Scale * 0.5f));

        float d0 = SourceDepth.Load(int3(min(SrcCoord, MaxCoord), 0)).r;
        float d1 = SourceDepth.Load(int3(min(SrcCoord + uint2(Step.x, 0), MaxCoord), 0)).r;
        float d2 = SourceDepth.Load(int3(min(SrcCoord + uint2(0, Step.y), MaxCoord), 0)).r;
        float d3 = SourceDepth.Load(int3(min(SrcCoord + Step, MaxCoord), 0)).r;

        MaxDepth = max(max(d0, d1), max(d2, d3));
    }
    else
    {
        uint2 SrcCoord = DispatchThreadID.xy * 2;
        float d0 = SourceDepth.Load(int3(min(SrcCoord, MaxCoord), 0)).r;
        float d1 = SourceDepth.Load(int3(min(SrcCoord + uint2(1, 0), MaxCoord), 0)).r;
        float d2 = SourceDepth.Load(int3(min(SrcCoord + uint2(0, 1), MaxCoord), 0)).r;
        float d3 = SourceDepth.Load(int3(min(SrcCoord + uint2(1, 1), MaxCoord), 0)).r;

        MaxDepth = max(max(d0, d1), max(d2, d3));
    }

    DestHZB[DispatchThreadID.xy] = MaxDepth;
}
