struct FInstanceBound
{
    float3 Center;
    float Radius;
    float3 Extent;
    uint Pad;
};

StructuredBuffer<FInstanceBound> Instances : register(t0);
Texture2D<float> HZBTexture : register(t1);
SamplerState PointClampSampler : register(s0);

RWStructuredBuffer<uint> VisibilityBits : register(u0);

cbuffer CullConstants : register(b0)
{
    float4 FrustumPlanes[6];
    row_major float4x4 ViewProjection;
    float3 CameraPosition;
    float Pad0;
    float2 HZBSize;
    uint NumInstances;
    uint NumWords;
    uint bUseHZB;
    uint NumHZBMips;
    float DepthBias;
    float Pad;
};

groupshared uint s_Bits[4];

[numthreads(64, 1, 1)]
void mainCS(uint3 GroupThreadID : SV_GroupThreadID, uint3 GroupID : SV_GroupID, uint3 DispatchThreadID : SV_DispatchThreadID)
{
    if (GroupThreadID.x < 4)
    {
        s_Bits[GroupThreadID.x] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    uint Index = DispatchThreadID.x;
    bool bVisible = false;
    uint LodCode = 0;

    if (Index < NumInstances)
    {
        FInstanceBound Bound = Instances[Index];
        float3 Center = Bound.Center;
        float3 Extent = Bound.Extent;

        bVisible = true;
        [unroll]
        for (int i = 0; i < 6; ++i)
        {
            float4 Plane = FrustumPlanes[i];
            float Dist = dot(Plane.xyz, Center) + Plane.w;
            float Radius = dot(abs(Plane.xyz), Extent);
            if (Dist < -Radius)
            {
                bVisible = false;
                break;
            }
        }

        if (bVisible && bUseHZB != 0)
        {
            float3 BoxMin = Center - Extent;
            float3 BoxMax = Center + Extent;

            float3 Corners[8];
            Corners[0] = float3(BoxMin.x, BoxMin.y, BoxMin.z);
            Corners[1] = float3(BoxMax.x, BoxMin.y, BoxMin.z);
            Corners[2] = float3(BoxMin.x, BoxMax.y, BoxMin.z);
            Corners[3] = float3(BoxMax.x, BoxMax.y, BoxMin.z);
            Corners[4] = float3(BoxMin.x, BoxMin.y, BoxMax.z);
            Corners[5] = float3(BoxMax.x, BoxMin.y, BoxMax.z);
            Corners[6] = float3(BoxMin.x, BoxMax.y, BoxMax.z);
            Corners[7] = float3(BoxMax.x, BoxMax.y, BoxMax.z);

            float3 MinNDC = float3(1e9f, 1e9f, 1e9f);
            float3 MaxNDC = float3(-1e9f, -1e9f, -1e9f);
            bool bNearClipped = false;

            [unroll]
            for (int c = 0; c < 8; ++c)
            {
                float4 Clip = mul(float4(Corners[c], 1.0f), ViewProjection);
                if (Clip.w <= 0.001f)
                {
                    bNearClipped = true;
                    break;
                }
                float3 NDC = Clip.xyz / Clip.w;
                MinNDC = min(MinNDC, NDC);
                MaxNDC = max(MaxNDC, NDC);
            }

            if (!bNearClipped)
            {
                float2 MinUV = float2(MinNDC.x * 0.5f + 0.5f, -MaxNDC.y * 0.5f + 0.5f);
                float2 MaxUV = float2(MaxNDC.x * 0.5f + 0.5f, -MinNDC.y * 0.5f + 0.5f);
                MinUV = clamp(MinUV, 0.0f, 1.0f);
                MaxUV = clamp(MaxUV, 0.0f, 1.0f);

                float2 PixelSize = (MaxUV - MinUV) * HZBSize;
                float MaxDim = max(PixelSize.x, PixelSize.y);
                float Mip = ceil(log2(max(MaxDim, 1.0f)));
                Mip = clamp(Mip, 0.0f, float(NumHZBMips - 1));

                float4 Depths;
                Depths.x = HZBTexture.SampleLevel(PointClampSampler, float2(MinUV.x, MinUV.y), Mip);
                Depths.y = HZBTexture.SampleLevel(PointClampSampler, float2(MaxUV.x, MinUV.y), Mip);
                Depths.z = HZBTexture.SampleLevel(PointClampSampler, float2(MinUV.x, MaxUV.y), Mip);
                Depths.w = HZBTexture.SampleLevel(PointClampSampler, float2(MaxUV.x, MaxUV.y), Mip);

                float MaxHZBDepth = max(max(Depths.x, Depths.y), max(Depths.z, Depths.w));
                if (MinNDC.z > MaxHZBDepth + DepthBias)
                {
                    bVisible = false;
                }
            }
        }

        // 거리 비율 기반 단계 판정
        if (bVisible)
        {
            float Dist = length(Center - CameraPosition);
            float ScreenDiameter = (Bound.Radius * 2.0f) / max(Dist, 0.001f);
            
            if (ScreenDiameter < 0.05f)
            {
                LodCode = 3u;
            }
            else if (ScreenDiameter < 0.15f)
            {
                LodCode = 2u;
            }
            else
            {
                LodCode = 1u;
            }
        }
    }

    // 두 비트씩 묶어 공유 메모리에 저장
    if (LodCode > 0u)
    {
        uint LocalWord = GroupThreadID.x / 16;
        uint LocalShift = (GroupThreadID.x % 16) * 2;
        InterlockedOr(s_Bits[LocalWord], LodCode << LocalShift);
    }
    GroupMemoryBarrierWithGroupSync();

    if (GroupThreadID.x < 4)
    {
        uint OutIndex = GroupID.x * 4 + GroupThreadID.x;
        if (OutIndex < NumWords)
        {
            VisibilityBits[OutIndex] = s_Bits[GroupThreadID.x];
        }
    }
}
