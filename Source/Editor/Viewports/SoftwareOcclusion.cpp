#include "EnginePCH.h"
#include "Editor/Viewports/SoftwareOcclusion.h"

#include "Component/PrimitiveComponent.h"
#include "Rendering/StaticMeshData.h"
#include "Job/FiberJobManager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>

namespace
{
    constexpr float ClipEpsilon = 1.0e-6f;

    double NowSeconds()
    {
        using FClock = std::chrono::steady_clock;
        return std::chrono::duration<double>(FClock::now().time_since_epoch()).count();
    }

    FVector BoundsMin(const FAABB& Bounds) { return Bounds.Center - Bounds.Extent; }
    FVector BoundsMax(const FAABB& Bounds) { return Bounds.Center + Bounds.Extent; }

    FAABB MakeBounds(const FVector& Min, const FVector& Max)
    {
        return {(Min + Max) * 0.5f, (Max - Min) * 0.5f};
    }

    FAABB UnionBounds(const FAABB& A, const FAABB& B)
    {
        const FVector AMin = BoundsMin(A);
        const FVector AMax = BoundsMax(A);
        const FVector BMin = BoundsMin(B);
        const FVector BMax = BoundsMax(B);
        return MakeBounds(
            {std::min(AMin.X, BMin.X), std::min(AMin.Y, BMin.Y), std::min(AMin.Z, BMin.Z)},
            {std::max(AMax.X, BMax.X), std::max(AMax.Y, BMax.Y), std::max(AMax.Z, BMax.Z)});
    }

    float SurfaceArea(const FAABB& Bounds)
    {
        const FVector Size = Bounds.Extent * 2.0f;
        return 2.0f * (Size.X * Size.Y + Size.Y * Size.Z + Size.Z * Size.X);
    }

    float AxisValue(const FVector& Value, const int32 Axis)
    {
        return Axis == 0 ? Value.X : Axis == 1 ? Value.Y : Value.Z;
    }

    bool IsFiniteBounds(const FAABB& Bounds)
    {
        return std::isfinite(Bounds.Center.X) && std::isfinite(Bounds.Center.Y) && std::isfinite(Bounds.Center.Z) &&
            std::isfinite(Bounds.Extent.X) && std::isfinite(Bounds.Extent.Y) && std::isfinite(Bounds.Extent.Z) &&
            Bounds.Extent.X >= 0.0f && Bounds.Extent.Y >= 0.0f && Bounds.Extent.Z >= 0.0f;
    }

    void GetBoundsCorners(const FAABB& Bounds, FVector OutCorners[8])
    {
        int32 Index = 0;
        for (int32 Z = -1; Z <= 1; Z += 2)
            for (int32 Y = -1; Y <= 1; Y += 2)
                for (int32 X = -1; X <= 1; X += 2)
                    OutCorners[Index++] = Bounds.Center + FVector(
                        Bounds.Extent.X * static_cast<float>(X),
                        Bounds.Extent.Y * static_cast<float>(Y),
                        Bounds.Extent.Z * static_cast<float>(Z));
    }

    enum class EFrustumResult : uint8 { Outside, Intersect, Inside };

    EFrustumResult ClassifyFrustum(const FAABB& Bounds, const FFrustumPlanes& Frustum)
    {
        bool bIntersects = false;
        for (const FPlane& Plane : Frustum.Planes)
        {
            const float Radius = std::fabs(Plane.Normal.X) * Bounds.Extent.X +
                std::fabs(Plane.Normal.Y) * Bounds.Extent.Y +
                std::fabs(Plane.Normal.Z) * Bounds.Extent.Z;
            const float Distance = FVector::Dot(Plane.Normal, Bounds.Center) + Plane.Distance;
            if (Distance + Radius < 0.0f)
                return EFrustumResult::Outside;
            if (Distance - Radius < 0.0f)
                bIntersects = true;
        }
        return bIntersects ? EFrustumResult::Intersect : EFrustumResult::Inside;
    }

    float ClipPlaneDistance(const FVector4& Point, const int32 Plane)
    {
        switch (Plane)
        {
        case 0: return Point.X + Point.W;
        case 1: return Point.W - Point.X;
        case 2: return Point.Y + Point.W;
        case 3: return Point.W - Point.Y;
        case 4: return Point.Z;
        default: return Point.W - Point.Z;
        }
    }

    FVector4 LerpClip(const FVector4& A, const FVector4& B, const float T)
    {
        return A + (B - A) * T;
    }

    float EdgeFunction(const float AX, const float AY, const float BX, const float BY, const float PX, const float PY)
    {
        return (BX - AX) * (PY - AY) - (BY - AY) * (PX - AX);
    }

    uint32 FloatToSortableUint(float Value)
    {
        const uint32 Bits = std::bit_cast<uint32>(Value);
        const uint32 Mask = -static_cast<int32>(Bits >> 31) | 0x80000000u;
        return Bits ^ Mask;
    }

    // 부동소수점 거리 기반 기수 정렬
    void RadixSortCandidateDistances(
        TArray<FSoftwareOcclusionCuller::FCandidateDistance>& Items,
        TArray<FSoftwareOcclusionCuller::FCandidateDistance>& Temp)
    {
        const int32 Count = Items.Num();
        if (Count <= 64)
        {
            std::sort(Items.begin(), Items.end(), [](const auto& A, const auto& B)
            {
                return A.DistSq < B.DistSq;
            });
            return;
        }

        Temp.SetNum(Count, false);
        auto* Source = Items.GetData();
        auto* Dest = Temp.GetData();

        for (int32 ByteIndex = 0; ByteIndex < 4; ++ByteIndex)
        {
            const int32 Shift = ByteIndex * 8;
            uint32 Hist[256] = { 0 };

            for (int32 i = 0; i < Count; ++i)
            {
                const uint32 Key = FloatToSortableUint(Source[i].DistSq);
                const uint8 Bucket = static_cast<uint8>((Key >> Shift) & 0xff);
                ++Hist[Bucket];
            }

            uint32 Offset[256];
            Offset[0] = 0;
            for (int32 i = 1; i < 256; ++i)
            {
                Offset[i] = Offset[i - 1] + Hist[i - 1];
            }

            for (int32 i = 0; i < Count; ++i)
            {
                const uint32 Key = FloatToSortableUint(Source[i].DistSq);
                const uint8 Bucket = static_cast<uint8>((Key >> Shift) & 0xff);
                Dest[Offset[Bucket]++] = Source[i];
            }

            std::swap(Source, Dest);
        }

        if (Source != Items.GetData())
        {
            memcpy(Items.GetData(), Source, Count * sizeof(FSoftwareOcclusionCuller::FCandidateDistance));
        }
    }
}

void FSoftwareOcclusionCuller::ResetScene()
{
    ObjectStates.Reset();
    SyncSerial = 0;
    bInitialized = false;
    bBVHDirty = true;
    StaticObjectIndices.Reset();
    DynamicObjectIndices.Reset();
    BypassObjectIndices.Reset();
    BuiltStaticObjectIndices.Reset();
    BVHObjectIndices.Reset();
    BVHNodes.Reset();
    std::fill(std::begin(SuspendedFrames), std::end(SuspendedFrames), 0);
    LastBVHBuildMs = 0.0f;
#if defined(ENGINE_DEBUG)
    bSelfTestsRan = false;
#endif
}

void FSoftwareOcclusionCuller::SetSettings(const FSoftwareOcclusionSettings& InSettings)
{
    const FSoftwareOcclusionSettings Previous = Settings;
    const int32 PreviousTileSize = Settings.TileSize;
    Settings = InSettings;
    if (Settings.TileSize != 4 && Settings.TileSize != 8 && Settings.TileSize != 16)
        Settings.TileSize = 8;
    Settings.MinimumOccluderTiles = std::max(1, Settings.MinimumOccluderTiles);
    Settings.CpuTimeBudgetMs = std::max(0.0f, Settings.CpuTimeBudgetMs);
    Settings.DepthBias = std::max(0.0f, Settings.DepthBias);
    if (PreviousTileSize != Settings.TileSize)
        BufferWidth = 0;
    // 설정 패널이 매 프레임 호출하므로 값이 실제로 바뀐 경우에만 다음 프레임에 바로 다시 측정한다.
    if (Previous.Mode != Settings.Mode || Previous.TileSize != Settings.TileSize ||
        Previous.MinimumOccluderTiles != Settings.MinimumOccluderTiles || Previous.TriangleBudget != Settings.TriangleBudget ||
        Previous.CpuTimeBudgetMs != Settings.CpuTimeBudgetMs || Previous.DepthBias != Settings.DepthBias)
        std::fill(std::begin(SuspendedFrames), std::end(SuspendedFrames), 0);
}

void FSoftwareOcclusionCuller::SynchronizeObjects(const TArray<FRenderableObject>& Objects)
{
    ++SyncSerial;
    StaticObjectIndices.Reset();
    DynamicObjectIndices.Reset();
    BypassObjectIndices.Reset();
    StaticObjectIndices.Reserve(Objects.Num());
    DynamicObjectIndices.Reserve(Objects.Num());
    BypassObjectIndices.Reserve(Objects.Num());

    for (int32 Index = 0; Index < Objects.Num(); ++Index)
    {
        const FRenderableObject& Object = Objects[Index];
        if (!Object.Primitive || !Object.bCanBeOccluded)
        {
            BypassObjectIndices.Add(static_cast<uint32>(Index));
            continue;
        }

        const uint32 InternalIndex = Object.Primitive->GetInternalIndex();
        if (InternalIndex >= static_cast<uint32>(ObjectStates.Num()))
        {
            ObjectStates.SetNum(InternalIndex + 1);
        }

        FObjectState& State = ObjectStates[InternalIndex];
        const uint32 SerialNumber = Object.Primitive->GetSerialNumber();
        if (State.SerialNumber != SerialNumber)
        {
            State.SerialNumber = SerialNumber;
            State.BoundsRevision = Object.BoundsRevision;
            State.SeenSerial = SyncSerial;
            State.bDynamic = bInitialized;
            if (State.bDynamic)
            {
                bBVHDirty = true;
            }
        }
        else
        {
            if (!State.bDynamic && State.BoundsRevision != Object.BoundsRevision)
            {
                State.bDynamic = true;
                bBVHDirty = true;
            }
            State.BoundsRevision = Object.BoundsRevision;
            State.SeenSerial = SyncSerial;
        }

        if (State.bDynamic)
        {
            DynamicObjectIndices.Add(static_cast<uint32>(Index));
        }
        else
        {
            StaticObjectIndices.Add(static_cast<uint32>(Index));
        }
    }

    bool bSameStaticLayout = StaticObjectIndices.Num() == BuiltStaticObjectIndices.Num();
    for (int32 Index = 0; bSameStaticLayout && Index < StaticObjectIndices.Num(); ++Index)
    {
        bSameStaticLayout = StaticObjectIndices[Index] == BuiltStaticObjectIndices[Index];
    }
    bBVHDirty = bBVHDirty || !bSameStaticLayout;
    bInitialized = true;
}

void FSoftwareOcclusionCuller::PrepareBuffers(const int32 ViewWidth, const int32 ViewHeight)
{
    // 뷰포트 해상도와 무관하게 긴 변을 MaxBufferExtent 이하로 줄인 저해상도 버퍼에 래스터화한다.
    const float Scale = std::min(1.0f,
        static_cast<float>(MaxBufferExtent) / static_cast<float>(std::max(ViewWidth, ViewHeight)));
    const int32 NewWidth = std::max(1, static_cast<int32>(std::lround(static_cast<float>(ViewWidth) * Scale)));
    const int32 NewHeight = std::max(1, static_cast<int32>(std::lround(static_cast<float>(ViewHeight) * Scale)));
    const int32 NewTilesX = (NewWidth + Settings.TileSize - 1) / Settings.TileSize;
    const int32 NewTilesY = (NewHeight + Settings.TileSize - 1) / Settings.TileSize;
    if (NewWidth == BufferWidth && NewHeight == BufferHeight && NewTilesX == TilesX && NewTilesY == TilesY)
        return;

    BufferWidth = NewWidth;
    BufferHeight = NewHeight;
    TilesX = NewTilesX;
    TilesY = NewTilesY;
    Tiles.SetNum(TilesX * TilesY, false);
    DirtyTiles.Reserve(TilesX * TilesY);

    HZBLevels.Reset();
    int32 Width = TilesX;
    int32 Height = TilesY;
    while (Width > 0 && Height > 0)
    {
        FHZBLevel Level{};
        Level.Width = Width;
        Level.Height = Height;
        Level.Cells.SetNum(Width * Height, false);
        HZBLevels.Add(std::move(Level));
        if (Width == 1 && Height == 1)
            break;
        Width = (Width + 1) / 2;
        Height = (Height + 1) / 2;
    }
}

void FSoftwareOcclusionCuller::ClearBuffers()
{
    for (FOcclusionTile& Tile : Tiles)
    {
        Tile.CoverageMask = 0;
        Tile.bDirty = false;
        for (float& Depth : Tile.SubcellDepth)
            Depth = 1.0f;
    }
    for (FHZBLevel& Level : HZBLevels)
    {
        for (FHZBCell& Cell : Level.Cells)
        {
            Cell.Depth = 1.0f;
            Cell.bCovered = false;
        }
    }
    DirtyTiles.Reset();
}

void FSoftwareOcclusionCuller::EnsureBVH(const TArray<FRenderableObject>& Objects)
{
    if (!bBVHDirty)
        return;

    const double Start = NowSeconds();
    BuiltStaticObjectIndices = StaticObjectIndices;
    BVHObjectIndices = StaticObjectIndices;
    BVHNodes.Reset();
    BVHNodes.Reserve(std::max(1, BVHObjectIndices.Num() * 2));
    if (!BVHObjectIndices.IsEmpty())
        BuildBVHNode(Objects, 0, static_cast<uint32>(BVHObjectIndices.Num()));
    LastBVHBuildMs = static_cast<float>((NowSeconds() - Start) * 1000.0);
    bBVHDirty = false;
}

uint32 FSoftwareOcclusionCuller::BuildBVHNode(const TArray<FRenderableObject>& Objects, const uint32 First, const uint32 Count)
{
    FAABB NodeBounds = Objects[BVHObjectIndices[First]].WorldBounds;
    FVector CentroidMin = NodeBounds.Center;
    FVector CentroidMax = NodeBounds.Center;
    for (uint32 Offset = 1; Offset < Count; ++Offset)
    {
        const FAABB& Bounds = Objects[BVHObjectIndices[First + Offset]].WorldBounds;
        NodeBounds = UnionBounds(NodeBounds, Bounds);
        CentroidMin.X = std::min(CentroidMin.X, Bounds.Center.X);
        CentroidMin.Y = std::min(CentroidMin.Y, Bounds.Center.Y);
        CentroidMin.Z = std::min(CentroidMin.Z, Bounds.Center.Z);
        CentroidMax.X = std::max(CentroidMax.X, Bounds.Center.X);
        CentroidMax.Y = std::max(CentroidMax.Y, Bounds.Center.Y);
        CentroidMax.Z = std::max(CentroidMax.Z, Bounds.Center.Z);
    }

    FBVHNode Node{};
    Node.Bounds = NodeBounds;
    Node.First = First;
    Node.Count = Count;
    const uint32 NodeIndex = BVHNodes.Add(Node);
    if (Count <= BVHLeafSize)
    {
        BVHNodes[NodeIndex].bLeaf = true;
        return NodeIndex;
    }

    struct FBin
    {
        FAABB Bounds{};
        uint32 Count = 0;
        bool bValid = false;
    };

    float BestCost = std::numeric_limits<float>::max();
    int32 BestAxis = -1;
    int32 BestSplit = -1;
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        const float Minimum = AxisValue(CentroidMin, Axis);
        const float Extent = AxisValue(CentroidMax, Axis) - Minimum;
        if (Extent <= ClipEpsilon)
            continue;

        std::array<FBin, SAHBinCount> Bins{};
        for (uint32 Offset = 0; Offset < Count; ++Offset)
        {
            const FAABB& Bounds = Objects[BVHObjectIndices[First + Offset]].WorldBounds;
            const int32 BinIndex = std::clamp(
                static_cast<int32>((AxisValue(Bounds.Center, Axis) - Minimum) / Extent * SAHBinCount),
                0, SAHBinCount - 1);
            FBin& Bin = Bins[BinIndex];
            Bin.Bounds = Bin.bValid ? UnionBounds(Bin.Bounds, Bounds) : Bounds;
            Bin.bValid = true;
            ++Bin.Count;
        }

        std::array<FAABB, SAHBinCount> LeftBounds{};
        std::array<FAABB, SAHBinCount> RightBounds{};
        std::array<uint32, SAHBinCount> LeftCounts{};
        std::array<uint32, SAHBinCount> RightCounts{};
        bool bLeftValid = false;
        bool bRightValid = false;
        for (int32 BinIndex = 0; BinIndex < SAHBinCount; ++BinIndex)
        {
            if (Bins[BinIndex].bValid)
            {
                LeftBounds[BinIndex] = bLeftValid ? UnionBounds(LeftBounds[BinIndex - 1], Bins[BinIndex].Bounds) : Bins[BinIndex].Bounds;
                bLeftValid = true;
            }
            else if (BinIndex > 0)
            {
                LeftBounds[BinIndex] = LeftBounds[BinIndex - 1];
            }
            LeftCounts[BinIndex] = Bins[BinIndex].Count + (BinIndex > 0 ? LeftCounts[BinIndex - 1] : 0);
        }
        for (int32 BinIndex = SAHBinCount - 1; BinIndex >= 0; --BinIndex)
        {
            if (Bins[BinIndex].bValid)
            {
                RightBounds[BinIndex] = bRightValid ? UnionBounds(RightBounds[BinIndex + 1], Bins[BinIndex].Bounds) : Bins[BinIndex].Bounds;
                bRightValid = true;
            }
            else if (BinIndex + 1 < SAHBinCount)
            {
                RightBounds[BinIndex] = RightBounds[BinIndex + 1];
            }
            RightCounts[BinIndex] = Bins[BinIndex].Count + (BinIndex + 1 < SAHBinCount ? RightCounts[BinIndex + 1] : 0);
        }

        for (int32 Split = 0; Split < SAHBinCount - 1; ++Split)
        {
            if (LeftCounts[Split] == 0 || RightCounts[Split + 1] == 0)
                continue;
            const float Cost = SurfaceArea(LeftBounds[Split]) * static_cast<float>(LeftCounts[Split]) +
                SurfaceArea(RightBounds[Split + 1]) * static_cast<float>(RightCounts[Split + 1]);
            if (Cost < BestCost)
            {
                BestCost = Cost;
                BestAxis = Axis;
                BestSplit = Split;
            }
        }
    }

    auto Begin = BVHObjectIndices.begin() + First;
    auto End = Begin + Count;
    uint32 LeftCount = 0;
    if (BestAxis >= 0)
    {
        const float Minimum = AxisValue(CentroidMin, BestAxis);
        const float Extent = AxisValue(CentroidMax, BestAxis) - Minimum;
        const auto Middle = std::partition(Begin, End, [&](const uint32 ObjectIndex)
        {
            const float Center = AxisValue(Objects[ObjectIndex].WorldBounds.Center, BestAxis);
            const int32 Bin = std::clamp(static_cast<int32>((Center - Minimum) / Extent * SAHBinCount), 0, SAHBinCount - 1);
            return Bin <= BestSplit;
        });
        LeftCount = static_cast<uint32>(Middle - Begin);
    }

    if (LeftCount == 0 || LeftCount == Count)
    {
        const FVector Range = CentroidMax - CentroidMin;
        const int32 Axis = Range.X >= Range.Y && Range.X >= Range.Z ? 0 : Range.Y >= Range.Z ? 1 : 2;
        LeftCount = Count / 2;
        std::nth_element(Begin, Begin + LeftCount, End, [&](const uint32 A, const uint32 B)
        {
            return AxisValue(Objects[A].WorldBounds.Center, Axis) < AxisValue(Objects[B].WorldBounds.Center, Axis);
        });
    }

    const uint32 Left = BuildBVHNode(Objects, First, LeftCount);
    const uint32 Right = BuildBVHNode(Objects, First + LeftCount, Count - LeftCount);
    BVHNodes[NodeIndex].Left = Left;
    BVHNodes[NodeIndex].Right = Right;
    return NodeIndex;
}

float FSoftwareOcclusionCuller::DistanceSquaredToBounds(const FAABB& Bounds) const
{
    const FVector Minimum = BoundsMin(Bounds);
    const FVector Maximum = BoundsMax(Bounds);
    const float X = CurrentCameraLocation.X < Minimum.X ? Minimum.X - CurrentCameraLocation.X :
        CurrentCameraLocation.X > Maximum.X ? CurrentCameraLocation.X - Maximum.X : 0.0f;
    const float Y = CurrentCameraLocation.Y < Minimum.Y ? Minimum.Y - CurrentCameraLocation.Y :
        CurrentCameraLocation.Y > Maximum.Y ? CurrentCameraLocation.Y - Maximum.Y : 0.0f;
    const float Z = CurrentCameraLocation.Z < Minimum.Z ? Minimum.Z - CurrentCameraLocation.Z :
        CurrentCameraLocation.Z > Maximum.Z ? CurrentCameraLocation.Z - Maximum.Z : 0.0f;
    return X * X + Y * Y + Z * Z;
}

void FSoftwareOcclusionCuller::AddDebugBounds(const FAABB& Bounds, const ESoftwareOcclusionDebugState State)
{
    if (Settings.bDebugBounds && DebugBounds.Num() < 256)
    {
        static std::mutex BoundsMutex;
        std::lock_guard<std::mutex> Lock(BoundsMutex);
        if (DebugBounds.Num() < 256)
        {
            DebugBounds.Add({Bounds, State});
        }
    }
}

#if defined(ENGINE_DEBUG)
void FSoftwareOcclusionCuller::RunDebugSelfTests()
{
    if (bSelfTestsRan || Tiles.IsEmpty())
        return;
    bSelfTestsRan = true;

    FOcclusionTile& Tile = Tiles[0];
    Tile.CoverageMask = FullCoverageMask;
    for (float& Depth : Tile.SubcellDepth)
        Depth = 0.25f;
    Tile.bDirty = true;
    DirtyTiles.Add(0);
    UpdateDirtyHZB();

    FProjectedBounds Behind{};
    Behind.MinX = 0.0f;
    Behind.MinY = 0.0f;
    Behind.MaxX = static_cast<float>(std::min(Settings.TileSize, BufferWidth));
    Behind.MaxY = static_cast<float>(std::min(Settings.TileSize, BufferHeight));
    Behind.NearestDepth = 0.75f;
    Behind.bValid = true;
    Behind.bUncertain = false;
    assert(IsOccluded(Behind, false));
    assert(IsOccluded(Behind, true));

    FProjectedBounds InFront = Behind;
    InFront.NearestDepth = 0.1f;
    assert(!IsOccluded(InFront, false));

    Tile.CoverageMask &= static_cast<uint16>(~1u);
    Tile.bDirty = true;
    DirtyTiles.Add(0);
    UpdateDirtyHZB();
    assert(!IsOccluded(Behind, false));
    assert(!IsOccluded(Behind, true));
    ClearBuffers();
}
#endif

FSoftwareOcclusionCuller::FProjectedBounds FSoftwareOcclusionCuller::ProjectBounds(const FAABB& Bounds) const
{
    FProjectedBounds Result{};
    if (!IsFiniteBounds(Bounds) || BufferWidth <= 0 || BufferHeight <= 0)
        return Result;

    const FVector Minimum = BoundsMin(Bounds);
    const FVector Maximum = BoundsMax(Bounds);
    if (CurrentCameraLocation.X >= Minimum.X && CurrentCameraLocation.X <= Maximum.X &&
        CurrentCameraLocation.Y >= Minimum.Y && CurrentCameraLocation.Y <= Maximum.Y &&
        CurrentCameraLocation.Z >= Minimum.Z && CurrentCameraLocation.Z <= Maximum.Z)
        return Result;

    FVector Corners[8];
    GetBoundsCorners(Bounds, Corners);
    float MinX = std::numeric_limits<float>::max();
    float MinY = std::numeric_limits<float>::max();
    float MaxX = -std::numeric_limits<float>::max();
    float MaxY = -std::numeric_limits<float>::max();
    float NearestDepth = 1.0f;

    for (const FVector& Corner : Corners)
    {
        const FVector4 Clip = FVector4(Corner, 1.0f) * CurrentViewProjection;
        if (!std::isfinite(Clip.X) || !std::isfinite(Clip.Y) || !std::isfinite(Clip.Z) || !std::isfinite(Clip.W) ||
            Clip.W <= ClipEpsilon || Clip.Z < 0.0f)
            return Result;

        const float InvW = 1.0f / Clip.W;
        const float NdcX = Clip.X * InvW;
        const float NdcY = Clip.Y * InvW;
        const float Depth = Clip.Z * InvW;
        const float ScreenX = (NdcX * 0.5f + 0.5f) * static_cast<float>(BufferWidth);
        const float ScreenY = (-NdcY * 0.5f + 0.5f) * static_cast<float>(BufferHeight);
        MinX = std::min(MinX, ScreenX);
        MinY = std::min(MinY, ScreenY);
        MaxX = std::max(MaxX, ScreenX);
        MaxY = std::max(MaxY, ScreenY);
        NearestDepth = std::min(NearestDepth, Depth);
    }

    MinX = std::clamp(MinX, 0.0f, static_cast<float>(BufferWidth));
    MinY = std::clamp(MinY, 0.0f, static_cast<float>(BufferHeight));
    MaxX = std::clamp(MaxX, 0.0f, static_cast<float>(BufferWidth));
    MaxY = std::clamp(MaxY, 0.0f, static_cast<float>(BufferHeight));
    if (!(MaxX > MinX && MaxY > MinY) || !std::isfinite(NearestDepth))
        return Result;

    Result.MinX = MinX;
    Result.MinY = MinY;
    Result.MaxX = MaxX;
    Result.MaxY = MaxY;
    Result.NearestDepth = NearestDepth;
    Result.bValid = true;
    Result.bUncertain = false;
    return Result;
}

bool FSoftwareOcclusionCuller::QueryEdgeSubcells(
    const FProjectedBounds& Bounds,
    const int32 FullMinX,
    const int32 FullMinY,
    const int32 FullMaxX,
    const int32 FullMaxY) const
{
    const float SubcellSize = static_cast<float>(Settings.TileSize) / static_cast<float>(SubcellsPerAxis);
    const int32 TotalSubcellsX = TilesX * SubcellsPerAxis;
    const int32 TotalSubcellsY = TilesY * SubcellsPerAxis;
    const int32 MinCellX = std::clamp(static_cast<int32>(std::floor(Bounds.MinX / SubcellSize)), 0, TotalSubcellsX - 1);
    const int32 MinCellY = std::clamp(static_cast<int32>(std::floor(Bounds.MinY / SubcellSize)), 0, TotalSubcellsY - 1);
    const int32 MaxCellX = std::clamp(static_cast<int32>(std::ceil(Bounds.MaxX / SubcellSize)), 1, TotalSubcellsX);
    const int32 MaxCellY = std::clamp(static_cast<int32>(std::ceil(Bounds.MaxY / SubcellSize)), 1, TotalSubcellsY);

    for (int32 CellY = MinCellY; CellY < MaxCellY; ++CellY)
    {
        for (int32 CellX = MinCellX; CellX < MaxCellX; ++CellX)
        {
            const int32 TileX = CellX / SubcellsPerAxis;
            const int32 TileY = CellY / SubcellsPerAxis;
            if (TileX >= FullMinX && TileX < FullMaxX && TileY >= FullMinY && TileY < FullMaxY)
                continue;

            const int32 LocalX = CellX % SubcellsPerAxis;
            const int32 LocalY = CellY % SubcellsPerAxis;
            const int32 BitIndex = LocalY * SubcellsPerAxis + LocalX;
            const FOcclusionTile& Tile = Tiles[TileY * TilesX + TileX];
            const uint16 Bit = static_cast<uint16>(1u << BitIndex);
            if ((Tile.CoverageMask & Bit) == 0 || Bounds.NearestDepth <= Tile.SubcellDepth[BitIndex] + Settings.DepthBias)
                return false;
        }
    }
    return true;
}

bool FSoftwareOcclusionCuller::QueryHZBCell(
    const int32 Level,
    const int32 X,
    const int32 Y,
    const int32 MinTileX,
    const int32 MinTileY,
    const int32 MaxTileX,
    const int32 MaxTileY,
    const float NearestDepth) const
{
    const int32 Scale = 1 << Level;
    const int32 CellMinX = X * Scale;
    const int32 CellMinY = Y * Scale;
    const int32 CellMaxX = std::min((X + 1) * Scale, TilesX);
    const int32 CellMaxY = std::min((Y + 1) * Scale, TilesY);
    if (CellMaxX <= MinTileX || CellMaxY <= MinTileY || CellMinX >= MaxTileX || CellMinY >= MaxTileY)
        return true;

    const bool bFullyInside = CellMinX >= MinTileX && CellMinY >= MinTileY && CellMaxX <= MaxTileX && CellMaxY <= MaxTileY;
    if (bFullyInside || Level == 0)
    {
        const FHZBLevel& HZBLevel = HZBLevels[Level];
        const FHZBCell& Cell = HZBLevel.Cells[Y * HZBLevel.Width + X];
        return Cell.bCovered && NearestDepth > Cell.Depth + Settings.DepthBias;
    }

    const FHZBLevel& ChildLevel = HZBLevels[Level - 1];
    for (int32 ChildY = Y * 2; ChildY < Y * 2 + 2; ++ChildY)
    {
        for (int32 ChildX = X * 2; ChildX < X * 2 + 2; ++ChildX)
        {
            if (ChildX < ChildLevel.Width && ChildY < ChildLevel.Height &&
                !QueryHZBCell(Level - 1, ChildX, ChildY, MinTileX, MinTileY, MaxTileX, MaxTileY, NearestDepth))
                return false;
        }
    }
    return true;
}

bool FSoftwareOcclusionCuller::QueryHZBRegion(
    const int32 MinTileX,
    const int32 MinTileY,
    const int32 MaxTileX,
    const int32 MaxTileY,
    const float NearestDepth) const
{
    if (MinTileX >= MaxTileX || MinTileY >= MaxTileY || HZBLevels.IsEmpty())
        return true;
    const int32 TopLevelIndex = HZBLevels.Num() - 1;
    const FHZBLevel& Top = HZBLevels[TopLevelIndex];
    for (int32 Y = 0; Y < Top.Height; ++Y)
        for (int32 X = 0; X < Top.Width; ++X)
            if (!QueryHZBCell(TopLevelIndex, X, Y, MinTileX, MinTileY, MaxTileX, MaxTileY, NearestDepth))
                return false;
    return true;
}

bool FSoftwareOcclusionCuller::IsOccluded(const FProjectedBounds& Bounds, const bool bUseHierarchy) const
{
    if (!Bounds.bValid || Bounds.bUncertain || Tiles.IsEmpty())
        return false;

    const float TileSize = static_cast<float>(Settings.TileSize);
    const int32 FullMinX = std::clamp(static_cast<int32>(std::ceil(Bounds.MinX / TileSize)), 0, TilesX);
    const int32 FullMinY = std::clamp(static_cast<int32>(std::ceil(Bounds.MinY / TileSize)), 0, TilesY);
    const int32 FullMaxX = std::clamp(static_cast<int32>(std::floor(Bounds.MaxX / TileSize)), 0, TilesX);
    const int32 FullMaxY = std::clamp(static_cast<int32>(std::floor(Bounds.MaxY / TileSize)), 0, TilesY);

    if (!QueryEdgeSubcells(Bounds, FullMinX, FullMinY, FullMaxX, FullMaxY))
        return false;

    if (FullMinX >= FullMaxX || FullMinY >= FullMaxY)
        return true;

    if (bUseHierarchy)
        return QueryHZBRegion(FullMinX, FullMinY, FullMaxX, FullMaxY, Bounds.NearestDepth);

    for (int32 Y = FullMinY; Y < FullMaxY; ++Y)
    {
        for (int32 X = FullMinX; X < FullMaxX; ++X)
        {
            const FOcclusionTile& Tile = Tiles[Y * TilesX + X];
            if (Tile.CoverageMask != FullCoverageMask)
                return false;
            float Depth = 0.0f;
            for (const float SubcellDepth : Tile.SubcellDepth)
                Depth = std::max(Depth, SubcellDepth);
            if (Bounds.NearestDepth <= Depth + Settings.DepthBias)
                return false;
        }
    }
    return true;
}

void FSoftwareOcclusionCuller::UpdateHZBParent(const int32 Level, const int32 X, const int32 Y)
{
    FHZBLevel& ParentLevel = HZBLevels[Level];
    FHZBLevel& ChildLevel = HZBLevels[Level - 1];
    FHZBCell& Parent = ParentLevel.Cells[Y * ParentLevel.Width + X];
    Parent.bCovered = true;
    Parent.Depth = 0.0f;
    for (int32 ChildY = Y * 2; ChildY < Y * 2 + 2; ++ChildY)
    {
        for (int32 ChildX = X * 2; ChildX < X * 2 + 2; ++ChildX)
        {
            if (ChildX >= ChildLevel.Width || ChildY >= ChildLevel.Height)
            {
                Parent.bCovered = false;
                Parent.Depth = 1.0f;
                return;
            }
            const FHZBCell& Child = ChildLevel.Cells[ChildY * ChildLevel.Width + ChildX];
            if (!Child.bCovered)
            {
                Parent.bCovered = false;
                Parent.Depth = 1.0f;
                return;
            }
            Parent.Depth = std::max(Parent.Depth, Child.Depth);
        }
    }
}

void FSoftwareOcclusionCuller::UpdateDirtyHZB()
{
    if (HZBLevels.IsEmpty())
        return;

    for (const uint32 TileIndex : DirtyTiles)
    {
        FOcclusionTile& Tile = Tiles[TileIndex];
        int32 X = static_cast<int32>(TileIndex) % TilesX;
        int32 Y = static_cast<int32>(TileIndex) / TilesX;
        FHZBCell& Base = HZBLevels[0].Cells[TileIndex];
        Base.bCovered = Tile.CoverageMask == FullCoverageMask;
        Base.Depth = 1.0f;
        if (Base.bCovered)
        {
            Base.Depth = 0.0f;
            for (const float Depth : Tile.SubcellDepth)
                Base.Depth = std::max(Base.Depth, Depth);
        }

        for (int32 Level = 1; Level < HZBLevels.Num(); ++Level)
        {
            X /= 2;
            Y /= 2;
            UpdateHZBParent(Level, X, Y);
        }
        Tile.bDirty = false;
    }
    DirtyTiles.Reset();
}

void FSoftwareOcclusionCuller::RasterizeClippedTriangle(const FVector4& A, const FVector4& B, const FVector4& C)
{
    if (A.W <= ClipEpsilon || B.W <= ClipEpsilon || C.W <= ClipEpsilon)
        return;

    struct FScreenVertex { float X, Y, Z; };
    const auto ToScreen = [&](const FVector4& Point)
    {
        const float InvW = 1.0f / Point.W;
        return FScreenVertex{
            (Point.X * InvW * 0.5f + 0.5f) * static_cast<float>(BufferWidth),
            (-Point.Y * InvW * 0.5f + 0.5f) * static_cast<float>(BufferHeight),
            Point.Z * InvW};
    };
    const FScreenVertex V0 = ToScreen(A);
    const FScreenVertex V1 = ToScreen(B);
    const FScreenVertex V2 = ToScreen(C);
    if (!std::isfinite(V0.X) || !std::isfinite(V0.Y) || !std::isfinite(V0.Z) ||
        !std::isfinite(V1.X) || !std::isfinite(V1.Y) || !std::isfinite(V1.Z) ||
        !std::isfinite(V2.X) || !std::isfinite(V2.Y) || !std::isfinite(V2.Z))
        return;

    const float Area = EdgeFunction(V0.X, V0.Y, V1.X, V1.Y, V2.X, V2.Y);
    // D3D11 Rasterizer의 FrontCounterClockwise=false와 같은 screen-space clockwise 앞면만 쓴다.
    if (Area <= ClipEpsilon)
        return;

    // 깊이 평면 Z(x, y)의 화면 공간 기울기. 서브셀 안의 최대 깊이를 모서리 평가 없이 구한다.
    const float DepthDX = (-(V2.Y - V1.Y) * V0.Z - (V0.Y - V2.Y) * V1.Z - (V1.Y - V0.Y) * V2.Z) / Area;
    const float DepthDY = ((V2.X - V1.X) * V0.Z + (V0.X - V2.X) * V1.Z + (V1.X - V0.X) * V2.Z) / Area;
    const float MaxVertexDepth = std::max({V0.Z, V1.Z, V2.Z});

    const float SubcellSize = static_cast<float>(Settings.TileSize) / static_cast<float>(SubcellsPerAxis);
    const float HalfSubcell = SubcellSize * 0.5f;
    const float DepthSlack = (std::fabs(DepthDX) + std::fabs(DepthDY)) * HalfSubcell;
    const int32 TotalSubcellsX = TilesX * SubcellsPerAxis;
    const int32 TotalSubcellsY = TilesY * SubcellsPerAxis;
    const float MinX = std::min({V0.X, V1.X, V2.X});
    const float MinY = std::min({V0.Y, V1.Y, V2.Y});
    const float MaxX = std::max({V0.X, V1.X, V2.X});
    const float MaxY = std::max({V0.Y, V1.Y, V2.Y});
    const int32 MinCellX = std::clamp(static_cast<int32>(std::floor(MinX / SubcellSize)), 0, TotalSubcellsX - 1);
    const int32 MinCellY = std::clamp(static_cast<int32>(std::floor(MinY / SubcellSize)), 0, TotalSubcellsY - 1);
    const int32 MaxCellX = std::clamp(static_cast<int32>(std::ceil(MaxX / SubcellSize)), 0, TotalSubcellsX);
    const int32 MaxCellY = std::clamp(static_cast<int32>(std::ceil(MaxY / SubcellSize)), 0, TotalSubcellsY);

    for (int32 CellY = MinCellY; CellY < MaxCellY; ++CellY)
    {
        for (int32 CellX = MinCellX; CellX < MaxCellX; ++CellX)
        {
            // 서브셀 중심 한 점으로 포함을 판정해 인접 삼각형의 공유 엣지에 구멍이 생기지 않게 한다.
            const float CenterX = static_cast<float>(CellX) * SubcellSize + HalfSubcell;
            const float CenterY = static_cast<float>(CellY) * SubcellSize + HalfSubcell;
            const float E0 = EdgeFunction(V0.X, V0.Y, V1.X, V1.Y, CenterX, CenterY);
            const float E1 = EdgeFunction(V1.X, V1.Y, V2.X, V2.Y, CenterX, CenterY);
            const float E2 = EdgeFunction(V2.X, V2.Y, V0.X, V0.Y, CenterX, CenterY);
            if (E0 < 0.0f || E1 < 0.0f || E2 < 0.0f)
                continue;

            // 중심 깊이에 서브셀 모서리까지의 평면 증가분을 더해 보수적인 최대 깊이를 쓴다.
            const float CenterDepth = (E1 * V0.Z + E2 * V1.Z + E0 * V2.Z) / Area;
            const float FarthestDepth = std::min(CenterDepth + DepthSlack, MaxVertexDepth);
            if (FarthestDepth < 0.0f || FarthestDepth > 1.0f)
                continue;

            const int32 TileX = CellX / SubcellsPerAxis;
            const int32 TileY = CellY / SubcellsPerAxis;
            const int32 BitIndex = (CellY % SubcellsPerAxis) * SubcellsPerAxis + (CellX % SubcellsPerAxis);
            const uint16 Bit = static_cast<uint16>(1u << BitIndex);
            const uint32 TileIndex = static_cast<uint32>(TileY * TilesX + TileX);
            FOcclusionTile& Tile = Tiles[TileIndex];
            const bool bWasCovered = (Tile.CoverageMask & Bit) != 0;
            if (!bWasCovered || FarthestDepth < Tile.SubcellDepth[BitIndex])
            {
                Tile.CoverageMask |= Bit;
                Tile.SubcellDepth[BitIndex] = FarthestDepth;
                if (!Tile.bDirty)
                {
                    Tile.bDirty = true;
                    DirtyTiles.Add(TileIndex);
                }
            }
        }
    }
}

void FSoftwareOcclusionCuller::RasterizeOccluder(const FRenderableObject& Object, const FProjectedBounds& Projected)
{
    if (!Object.StaticMeshData || !Projected.bValid || Projected.bUncertain || !bAllowRasterization)
        return;

    const int32 TileWidth = static_cast<int32>(std::ceil((Projected.MaxX - Projected.MinX) / Settings.TileSize));
    const int32 TileHeight = static_cast<int32>(std::ceil((Projected.MaxY - Projected.MinY) / Settings.TileSize));
    if (TileWidth * TileHeight < Settings.MinimumOccluderTiles)
        return;

    if (Settings.CpuTimeBudgetMs > 0.0f && (NowSeconds() - CullStartSeconds) * 1000.0 > Settings.CpuTimeBudgetMs)
    {
        bAllowRasterization = false;
        ActiveStats->bCpuBudgetExceeded = true;
        return;
    }

    constexpr uint32 TriangleCount = 12;
    if (UsedTriangles + TriangleCount > Settings.TriangleBudget)
    {
        ActiveStats->bTriangleBudgetExceeded = true;
        return;
    }

    UsedTriangles += TriangleCount;
    ActiveStats->SourceTriangles += TriangleCount;
    ++ActiveStats->OccludersRasterized;

    const FMatrix ModelViewProjection = Object.WorldMatrix * CurrentViewProjection;
    const FBox& Box = Object.StaticMeshData->AABB;
    const bool bValidBox = Box.Min.X < Box.Max.X && Box.Min.Y < Box.Max.Y && Box.Min.Z < Box.Max.Z;

    // 바운딩 박스 정점 변환
    TransformedVertices.SetNum(8, false);
    if (bValidBox)
    {
        const FVector LocalCorners[8] = {
            { Box.Min.X, Box.Min.Y, Box.Min.Z },
            { Box.Max.X, Box.Min.Y, Box.Min.Z },
            { Box.Min.X, Box.Max.Y, Box.Min.Z },
            { Box.Max.X, Box.Max.Y, Box.Min.Z },
            { Box.Min.X, Box.Min.Y, Box.Max.Z },
            { Box.Max.X, Box.Min.Y, Box.Max.Z },
            { Box.Min.X, Box.Max.Y, Box.Max.Z },
            { Box.Max.X, Box.Max.Y, Box.Max.Z }
        };
        for (int32 i = 0; i < 8; ++i)
        {
            TransformedVertices[i] = FVector4(LocalCorners[i], 1.0f) * ModelViewProjection;
        }
    }
    else
    {
        FVector WorldCorners[8];
        GetBoundsCorners(Object.WorldBounds, WorldCorners);
        for (int32 i = 0; i < 8; ++i)
        {
            TransformedVertices[i] = FVector4(WorldCorners[i], 1.0f) * CurrentViewProjection;
        }
    }

    static constexpr uint32 BoxIndices[36] = {
        7, 5, 1,  7, 1, 3,
        4, 6, 2,  4, 2, 0,
        6, 7, 3,  6, 3, 2,
        5, 4, 0,  5, 0, 1,
        4, 5, 7,  4, 7, 6,
        0, 2, 3,  0, 3, 1
    };

    // 바운딩 박스 인덱스 순회
    for (int32 Index = 0; Index + 2 < 36; Index += 3)
    {
        const uint32 I0 = BoxIndices[Index];
        const uint32 I1 = BoxIndices[Index + 1];
        const uint32 I2 = BoxIndices[Index + 2];
        if (I0 >= static_cast<uint32>(TransformedVertices.Num()) ||
            I1 >= static_cast<uint32>(TransformedVertices.Num()) ||
            I2 >= static_cast<uint32>(TransformedVertices.Num()))
            continue;

        FVector4 PolygonA[16]{};
        FVector4 PolygonB[16]{};
        PolygonA[0] = TransformedVertices[I0];
        PolygonA[1] = TransformedVertices[I1];
        PolygonA[2] = TransformedVertices[I2];
        int32 VertexCount = 3;
        FVector4* Input = PolygonA;
        FVector4* Output = PolygonB;
        for (int32 Plane = 0; Plane < 6 && VertexCount > 0; ++Plane)
        {
            int32 OutputCount = 0;
            FVector4 Previous = Input[VertexCount - 1];
            float PreviousDistance = ClipPlaneDistance(Previous, Plane);
            bool bPreviousInside = PreviousDistance >= 0.0f;
            for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex)
            {
                const FVector4 Current = Input[Vertex];
                const float CurrentDistance = ClipPlaneDistance(Current, Plane);
                const bool bCurrentInside = CurrentDistance >= 0.0f;
                if (bCurrentInside != bPreviousInside)
                {
                    const float T = PreviousDistance / (PreviousDistance - CurrentDistance);
                    Output[OutputCount++] = LerpClip(Previous, Current, T);
                }
                if (bCurrentInside)
                    Output[OutputCount++] = Current;
                Previous = Current;
                PreviousDistance = CurrentDistance;
                bPreviousInside = bCurrentInside;
            }
            VertexCount = OutputCount;
            std::swap(Input, Output);
        }

        for (int32 Triangle = 1; Triangle + 1 < VertexCount; ++Triangle)
        {
            RasterizeClippedTriangle(Input[0], Input[Triangle], Input[Triangle + 1]);
            ++ActiveStats->ClippedTriangles;
        }
    }
    UpdateDirtyHZB();
}

void FSoftwareOcclusionCuller::ProcessObject(
    const FRenderableObject& Object,
    const bool bStatic,
    const bool bFrustumAccepted,
    const bool bUseOcclusion,
    TArray<UPrimitiveComponent*>& OutVisible)
{
    (void)OutVisible;
    if (!Object.Primitive)
        return;
    if (!bFrustumAccepted && !IsAABBInFrustum(Object.WorldBounds, CurrentFrustum))
    {
        ++ActiveStats->FrustumRejected;
        return;
    }

    if (!bUseOcclusion)
    {
        if (VisibilityFlags.IsValidIndex(static_cast<int32>(Object.StableIndex)))
            VisibilityFlags[Object.StableIndex] = 1;
        AddDebugBounds(Object.WorldBounds,
            bStatic ? ESoftwareOcclusionDebugState::StaticVisible : ESoftwareOcclusionDebugState::DynamicVisible);
        return;
    }

    if (!Object.bCanBeOccluded)
    {
        if (VisibilityFlags.IsValidIndex(static_cast<int32>(Object.StableIndex)))
            VisibilityFlags[Object.StableIndex] = 1;
        AddDebugBounds(Object.WorldBounds, ESoftwareOcclusionDebugState::Visible);
        return;
    }

    const FProjectedBounds Projected = ProjectBounds(Object.WorldBounds);
    ++ActiveStats->OcclusionTested;
    const bool bUseHierarchy = Settings.Mode != ESoftwareOcclusionMode::LinearSubcells;
    if (Projected.bValid && !Projected.bUncertain && IsOccluded(Projected, bUseHierarchy))
    {
        ++ActiveStats->OcclusionRejected;
        AddDebugBounds(Object.WorldBounds, ESoftwareOcclusionDebugState::Occluded);
        return;
    }

    if (VisibilityFlags.IsValidIndex(static_cast<int32>(Object.StableIndex)))
        VisibilityFlags[Object.StableIndex] = 1;
    AddDebugBounds(Object.WorldBounds,
        !Projected.bValid || Projected.bUncertain
            ? ESoftwareOcclusionDebugState::Fallback
            : bStatic ? ESoftwareOcclusionDebugState::StaticVisible : ESoftwareOcclusionDebugState::DynamicVisible);
    if (bStatic && Object.bCanOcclude && Projected.bValid && !Projected.bUncertain)
        RasterizeOccluder(Object, Projected);
}

void FSoftwareOcclusionCuller::TraverseBVH(
    const TArray<FRenderableObject>& Objects,
    const uint32 NodeIndex,
    const bool bFrustumAccepted,
    const bool bUseOcclusion,
    TArray<UPrimitiveComponent*>& OutVisible)
{
    const FBVHNode& Node = BVHNodes[NodeIndex];
    ++ActiveStats->BVHNodesTested;
    bool bNodeInside = bFrustumAccepted;
    if (!bFrustumAccepted)
    {
        const EFrustumResult FrustumResult = ClassifyFrustum(Node.Bounds, CurrentFrustum);
        if (FrustumResult == EFrustumResult::Outside)
        {
            ActiveStats->FrustumRejected += Node.Count;
            return;
        }
        bNodeInside = FrustumResult == EFrustumResult::Inside;
    }

    if (bUseOcclusion)
    {
        const FProjectedBounds Projected = ProjectBounds(Node.Bounds);
        if (Projected.bValid && !Projected.bUncertain && IsOccluded(Projected, true))
        {
            ++ActiveStats->BVHNodesPruned;
            ActiveStats->OcclusionRejected += Node.Count;
            AddDebugBounds(Node.Bounds, ESoftwareOcclusionDebugState::Occluded);
            return;
        }
    }

    if (Node.bLeaf)
    {
        auto Begin = BVHObjectIndices.begin() + Node.First;
        auto End = Begin + Node.Count;
        if (bUseOcclusion)
        {
            std::sort(Begin, End, [&](const uint32 A, const uint32 B)
            {
                return DistanceSquaredToBounds(Objects[A].WorldBounds) < DistanceSquaredToBounds(Objects[B].WorldBounds);
            });
        }
        for (auto It = Begin; It != End; ++It)
            ProcessObject(Objects[*It], true, bNodeInside, bUseOcclusion, OutVisible);
        return;
    }

    uint32 NearChild = Node.Left;
    uint32 FarChild = Node.Right;
    if (bUseOcclusion && DistanceSquaredToBounds(BVHNodes[FarChild].Bounds) < DistanceSquaredToBounds(BVHNodes[NearChild].Bounds))
        std::swap(NearChild, FarChild);
    TraverseBVH(Objects, NearChild, bNodeInside, bUseOcclusion, OutVisible);
    TraverseBVH(Objects, FarChild, bNodeInside, bUseOcclusion, OutVisible);
}

void FSoftwareOcclusionCuller::Cull(
    const int32 ViewIndex,
    const TArray<FRenderableObject>& Objects,
    const FFrustumPlanes& Frustum,
    const FMatrix& ViewProjection,
    const FVector& CameraLocation,
    const int32 ViewWidth,
    const int32 ViewHeight,
    const bool bWireframe,
    TArray<UPrimitiveComponent*>& OutVisible,
    FSoftwareOcclusionStats& OutStats)
{
    OutVisible.Reset();
    DebugBounds.Reset();
    VisibilityFlags.SetNum(Objects.Num(), false);
    std::fill(VisibilityFlags.begin(), VisibilityFlags.end(), static_cast<uint8>(0));
    OutStats = {};
    OutStats.CapturedPrimitives = static_cast<uint32>(Objects.Num());
    OutStats.StaticObjects = static_cast<uint32>(StaticObjectIndices.Num());
    OutStats.DynamicObjects = static_cast<uint32>(DynamicObjectIndices.Num());
    ActiveStats = &OutStats;
    CullStartSeconds = NowSeconds();
    CurrentViewProjection = ViewProjection;
    CurrentCameraLocation = CameraLocation;
    CurrentFrustum = Frustum;

    if (Settings.Mode == ESoftwareOcclusionMode::Disabled || bWireframe || ViewWidth <= 0 || ViewHeight <= 0)
    {
        const int32 TotalObjects = Objects.Num();
        if (TotalObjects > 0)
        {
            const uint32 NumWorkers = (std::max)(1u, FFiberJobManager::Get().GetNumWorkers());
            const int32 ChunkSize = (TotalObjects + NumWorkers - 1) / NumWorkers;
            const int32 NumJobs = (TotalObjects + ChunkSize - 1) / ChunkSize;

            if (WorkerVisibleBuffers.Num() < NumJobs)
            {
                WorkerVisibleBuffers.SetNum(NumJobs);
            }
            if (WorkerRejectedBuffers.Num() < NumJobs)
            {
                WorkerRejectedBuffers.SetNum(NumJobs);
            }
            for (int32 i = 0; i < NumJobs; ++i)
            {
                WorkerVisibleBuffers[i].Reset();
                WorkerRejectedBuffers[i] = 0;
            }

            // 절두체 검사 병렬 수행
            FFiberJobManager::Get().ParallelFor(TotalObjects, ChunkSize, [&](int32 Start, int32 End)
            {
                const int32 JobIndex = Start / ChunkSize;
                TArray<UPrimitiveComponent*>& LocalVisible = WorkerVisibleBuffers[JobIndex];
                LocalVisible.Reserve(End - Start);
                for (int32 Index = Start; Index < End; ++Index)
                {
                    const FRenderableObject& Object = Objects[Index];
                    if (Object.Primitive && IsAABBInFrustum(Object.WorldBounds, Frustum))
                    {
                        LocalVisible.Add(Object.Primitive);
                    }
                    else if (Object.Primitive)
                    {
                        ++WorkerRejectedBuffers[JobIndex];
                    }
                }
            });

            for (int32 JobIndex = 0; JobIndex < NumJobs; ++JobIndex)
            {
                OutVisible.Append(WorkerVisibleBuffers[JobIndex]);
                OutStats.FrustumRejected += WorkerRejectedBuffers[JobIndex];
            }
        }
        OutStats.FinalVisible = static_cast<uint32>(OutVisible.Num());
        OutStats.CullMs = static_cast<float>((NowSeconds() - CullStartSeconds) * 1000.0);
        ActiveStats = nullptr;
        return;
    }

    // 효과가 낮아 쉬는 중이면 BVH 프러스텀 컬링만 하고, 주기가 끝난 프레임에 오클루전을 다시 측정한다.
    int32& Suspended = SuspendedFrames[std::clamp(ViewIndex, 0, MaxViews - 1)];
    bool bSuspended = Settings.Mode != ESoftwareOcclusionMode::StaticBVHFrustumOnly && Suspended > 0;

    bSuspended = false; //항상 오쿨루전 작동


    if (bSuspended)
        --Suspended;
    OutStats.bOcclusionSuspended = bSuspended;
    const bool bBVHFrustumOnly = Settings.Mode == ESoftwareOcclusionMode::StaticBVHFrustumOnly || bSuspended;
    if (!bBVHFrustumOnly)
    {
        PrepareBuffers(ViewWidth, ViewHeight);
        ClearBuffers();
#if defined(ENGINE_DEBUG)
        RunDebugSelfTests();
#endif
    }
    UsedTriangles = 0;
    bAllowRasterization = true;

    if (Settings.Mode == ESoftwareOcclusionMode::StaticBVHHierarchical || bBVHFrustumOnly)
    {
        EnsureBVH(Objects);
        OutStats.BVHBuildMs = LastBVHBuildMs;
        if (!BVHNodes.IsEmpty())
            TraverseBVH(Objects, 0, false, !bBVHFrustumOnly, OutVisible);
        for (const uint32 Index : DynamicObjectIndices)
            ProcessObject(Objects[Index], false, false, !bBVHFrustumOnly, OutVisible);
        for (const uint32 Index : BypassObjectIndices)
            ProcessObject(Objects[Index], false, false, !bBVHFrustumOnly, OutVisible);
    }
    else
    {
        const int32 TotalObjects = Objects.Num();
        const uint32 NumWorkers = (std::max)(1u, FFiberJobManager::Get().GetNumWorkers());
        const int32 ChunkSize = (TotalObjects + NumWorkers - 1) / NumWorkers;
        const int32 NumJobs = (TotalObjects + ChunkSize - 1) / ChunkSize;

        if (WorkerCandidateBuffers.Num() < NumJobs)
        {
            WorkerCandidateBuffers.SetNum(NumJobs);
        }
        if (WorkerRejectedBuffers.Num() < NumJobs)
        {
            WorkerRejectedBuffers.SetNum(NumJobs);
        }
        for (int32 i = 0; i < NumJobs; ++i)
        {
            WorkerCandidateBuffers[i].Reset();
            WorkerRejectedBuffers[i] = 0;
        }

        // 절두체 검사 병렬 수행
        FFiberJobManager::Get().ParallelFor(TotalObjects, ChunkSize, [&](int32 Start, int32 End)
        {
            const int32 JobIndex = Start / ChunkSize;
            TArray<uint32>& LocalCandidates = WorkerCandidateBuffers[JobIndex];
            LocalCandidates.Reserve(End - Start);
            for (int32 Index = Start; Index < End; ++Index)
            {
                if (Objects[Index].Primitive && IsAABBInFrustum(Objects[Index].WorldBounds, Frustum))
                {
                    LocalCandidates.Add(static_cast<uint32>(Index));
                }
                else if (Objects[Index].Primitive)
                {
                    ++WorkerRejectedBuffers[JobIndex];
                }
            }
        });

        CandidateIndices.Reset();
        CandidateIndices.Reserve(TotalObjects);
        for (int32 JobIndex = 0; JobIndex < NumJobs; ++JobIndex)
        {
            CandidateIndices.Append(WorkerCandidateBuffers[JobIndex]);
            OutStats.FrustumRejected += WorkerRejectedBuffers[JobIndex];
        }

        const int32 TotalCandidates = CandidateIndices.Num();
        CandidateDistances.SetNum(TotalCandidates, false);

        if (TotalCandidates > 0)
        {
            const int32 DistChunkSize = (TotalCandidates + NumWorkers - 1) / NumWorkers;
            // 거리 계산 병렬 수행
            FFiberJobManager::Get().ParallelFor(TotalCandidates, DistChunkSize, [&](int32 Start, int32 End)
            {
                for (int32 i = Start; i < End; ++i)
                {
                    const uint32 ObjIdx = CandidateIndices[i];
                    CandidateDistances[i] = { ObjIdx, DistanceSquaredToBounds(Objects[ObjIdx].WorldBounds) };
                }
            });

            // 기수 정렬 수행
            RadixSortCandidateDistances(CandidateDistances, CandidateDistancesTemp);

            // 오클루더 선별
            OccluderIndices.Reset();
            const uint32 MaxOccluders = 1000;

            for (const FCandidateDistance& Item : CandidateDistances)
            {
                if (OccluderIndices.Num() >= static_cast<int32>(MaxOccluders))
                {
                    break;
                }

                const uint32 Index = Item.Index;
                const FRenderableObject& Object = Objects[Index];

                if (!Object.bCanOcclude || !Object.StaticMeshData || !Object.Primitive)
                {
                    continue;
                }

                const uint32 InternalIndex = Object.Primitive->GetInternalIndex();
                bool bStatic = false;
                if (InternalIndex < static_cast<uint32>(ObjectStates.Num()))
                {
                    const FObjectState& State = ObjectStates[InternalIndex];
                    bStatic = (State.SerialNumber == Object.Primitive->GetSerialNumber()) && !State.bDynamic;
                }

                if (!bStatic)
                {
                    continue;
                }

                const FProjectedBounds Projected = ProjectBounds(Object.WorldBounds);
                if (!Projected.bValid || Projected.bUncertain)
                {
                    continue;
                }

                const int32 TileWidth = static_cast<int32>(std::ceil((Projected.MaxX - Projected.MinX) / Settings.TileSize));
                const int32 TileHeight = static_cast<int32>(std::ceil((Projected.MaxY - Projected.MinY) / Settings.TileSize));
                if (TileWidth * TileHeight < Settings.MinimumOccluderTiles)
                {
                    continue;
                }

                OccluderIndices.Add(Index);
            }

            const int32 NumOccluders = OccluderIndices.Num();
            if (NumOccluders > 0)
            {
                const int32 OccluderChunkSize = (NumOccluders + NumWorkers - 1) / NumWorkers;
                const int32 NumJobs = (NumOccluders + OccluderChunkSize - 1) / OccluderChunkSize;

                if (WorkerClippedTriangleBuffers.Num() < NumJobs)
                {
                    WorkerClippedTriangleBuffers.SetNum(NumJobs);
                }
                for (int32 i = 0; i < NumJobs; ++i)
                {
                    WorkerClippedTriangleBuffers[i].Reset();
                }

                // 기하 변환 및 클리핑 병렬 수행
                FFiberJobManager::Get().ParallelFor(NumOccluders, OccluderChunkSize, [&](int32 Start, int32 End)
                {
                    const int32 JobIndex = Start / OccluderChunkSize;
                    TArray<FClippedTriangle>& LocalTriangles = WorkerClippedTriangleBuffers[JobIndex];
                    LocalTriangles.Reserve((End - Start) * 12);

                    static constexpr uint32 BoxIndices[36] = {
                        7, 5, 1,  7, 1, 3,
                        4, 6, 2,  4, 2, 0,
                        6, 7, 3,  6, 3, 2,
                        5, 4, 0,  5, 0, 1,
                        4, 5, 7,  4, 7, 6,
                        0, 2, 3,  0, 3, 1
                    };

                    for (int32 i = Start; i < End; ++i)
                    {
                        const uint32 Index = OccluderIndices[i];
                        const FRenderableObject& Object = Objects[Index];

                        FVector4 Verts[8];
                        const FMatrix MVP = Object.WorldMatrix * CurrentViewProjection;
                        const FBox& Box = Object.StaticMeshData->AABB;
                        const bool bValidBox = Box.Min.X < Box.Max.X && Box.Min.Y < Box.Max.Y && Box.Min.Z < Box.Max.Z;
                        if (bValidBox)
                        {
                            const FVector LocalCorners[8] = {
                                { Box.Min.X, Box.Min.Y, Box.Min.Z },
                                { Box.Max.X, Box.Min.Y, Box.Min.Z },
                                { Box.Min.X, Box.Max.Y, Box.Min.Z },
                                { Box.Max.X, Box.Max.Y, Box.Min.Z },
                                { Box.Min.X, Box.Min.Y, Box.Max.Z },
                                { Box.Max.X, Box.Min.Y, Box.Max.Z },
                                { Box.Min.X, Box.Max.Y, Box.Max.Z },
                                { Box.Max.X, Box.Max.Y, Box.Max.Z }
                            };
                            for (int32 c = 0; c < 8; ++c)
                            {
                                Verts[c] = FVector4(LocalCorners[c], 1.0f) * MVP;
                            }
                        }
                        else
                        {
                            FVector WorldCorners[8];
                            GetBoundsCorners(Object.WorldBounds, WorldCorners);
                            for (int32 c = 0; c < 8; ++c)
                            {
                                Verts[c] = FVector4(WorldCorners[c], 1.0f) * CurrentViewProjection;
                            }
                        }

                        for (int32 tri = 0; tri < 36; tri += 3)
                        {
                            const uint32 I0 = BoxIndices[tri];
                            const uint32 I1 = BoxIndices[tri + 1];
                            const uint32 I2 = BoxIndices[tri + 2];

                            FVector4 PolygonA[16]{};
                            FVector4 PolygonB[16]{};
                            PolygonA[0] = Verts[I0];
                            PolygonA[1] = Verts[I1];
                            PolygonA[2] = Verts[I2];
                            int32 VertexCount = 3;
                            FVector4* Input = PolygonA;
                            FVector4* Output = PolygonB;

                            for (int32 Plane = 0; Plane < 6 && VertexCount > 0; ++Plane)
                            {
                                int32 OutputCount = 0;
                                FVector4 Previous = Input[VertexCount - 1];
                                float PreviousDistance = ClipPlaneDistance(Previous, Plane);
                                bool bPreviousInside = PreviousDistance >= 0.0f;
                                for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex)
                                {
                                    const FVector4 Current = Input[Vertex];
                                    const float CurrentDistance = ClipPlaneDistance(Current, Plane);
                                    const bool bCurrentInside = CurrentDistance >= 0.0f;
                                    if (bCurrentInside != bPreviousInside)
                                    {
                                        const float T = PreviousDistance / (PreviousDistance - CurrentDistance);
                                        Output[OutputCount++] = LerpClip(Previous, Current, T);
                                    }
                                    if (bCurrentInside)
                                    {
                                        Output[OutputCount++] = Current;
                                    }
                                    Previous = Current;
                                    PreviousDistance = CurrentDistance;
                                    bPreviousInside = bCurrentInside;
                                }
                                VertexCount = OutputCount;
                                std::swap(Input, Output);
                            }

                            for (int32 t = 1; t + 1 < VertexCount; ++t)
                            {
                                LocalTriangles.Add({ Input[0], Input[t], Input[t + 1] });
                            }
                        }
                    }
                });

                // 클리핑된 삼각형 래스터화
                ActiveStats->OccludersRasterized = NumOccluders;
                ActiveStats->SourceTriangles = NumOccluders * 12;

                for (int32 JobIndex = 0; JobIndex < NumJobs; ++JobIndex)
                {
                    const TArray<FClippedTriangle>& Triangles = WorkerClippedTriangleBuffers[JobIndex];
                    ActiveStats->ClippedTriangles += Triangles.Num();
                    for (const FClippedTriangle& Tri : Triangles)
                    {
                        if (UsedTriangles >= Settings.TriangleBudget)
                        {
                            ActiveStats->bTriangleBudgetExceeded = true;
                            break;
                        }
                        if (Settings.CpuTimeBudgetMs > 0.0f && (NowSeconds() - CullStartSeconds) * 1000.0 > Settings.CpuTimeBudgetMs)
                        {
                            bAllowRasterization = false;
                            ActiveStats->bCpuBudgetExceeded = true;
                            break;
                        }

                        RasterizeClippedTriangle(Tri.V0, Tri.V1, Tri.V2);
                        ++UsedTriangles;
                    }
                    if (!bAllowRasterization || ActiveStats->bTriangleBudgetExceeded)
                    {
                        break;
                    }
                }

                UpdateDirtyHZB();
            }

            // 가시성 병렬 판정
            const int32 QueryChunkSize = (TotalCandidates + NumWorkers - 1) / NumWorkers;
            const bool bUseHierarchy = Settings.Mode != ESoftwareOcclusionMode::LinearSubcells;

            std::atomic<uint32> TotalOcclusionTested{ 0 };
            std::atomic<uint32> TotalOcclusionRejected{ 0 };

            FFiberJobManager::Get().ParallelFor(TotalCandidates, QueryChunkSize, [&](int32 Start, int32 End)
            {
                uint32 LocalTested = 0;
                uint32 LocalRejected = 0;

                for (int32 i = Start; i < End; ++i)
                {
                    const uint32 Index = CandidateDistances[i].Index;
                    const FRenderableObject& Object = Objects[Index];
                    if (!Object.Primitive)
                    {
                        continue;
                    }

                    if (!Object.bCanBeOccluded)
                    {
                        if (VisibilityFlags.IsValidIndex(static_cast<int32>(Object.StableIndex)))
                        {
                            VisibilityFlags[Object.StableIndex] = 1;
                        }
                        AddDebugBounds(Object.WorldBounds, ESoftwareOcclusionDebugState::Visible);
                        continue;
                    }

                    const FProjectedBounds Projected = ProjectBounds(Object.WorldBounds);
                    ++LocalTested;

                    if (Projected.bValid && !Projected.bUncertain && IsOccluded(Projected, bUseHierarchy))
                    {
                        ++LocalRejected;
                        AddDebugBounds(Object.WorldBounds, ESoftwareOcclusionDebugState::Occluded);
                        continue;
                    }

                    if (VisibilityFlags.IsValidIndex(static_cast<int32>(Object.StableIndex)))
                    {
                        VisibilityFlags[Object.StableIndex] = 1;
                    }

                    AddDebugBounds(Object.WorldBounds,
                        !Projected.bValid || Projected.bUncertain
                            ? ESoftwareOcclusionDebugState::Fallback
                            : ESoftwareOcclusionDebugState::Visible);
                }

                TotalOcclusionTested.fetch_add(LocalTested, std::memory_order_relaxed);
                TotalOcclusionRejected.fetch_add(LocalRejected, std::memory_order_relaxed);
            });

            ActiveStats->OcclusionTested += TotalOcclusionTested.load(std::memory_order_relaxed);
            ActiveStats->OcclusionRejected += TotalOcclusionRejected.load(std::memory_order_relaxed);
        }
    }

    uint64 CoveredSubcells = 0;
    uint32 CoveredTiles = 0;
    if (!bBVHFrustumOnly)
    {
        for (const FOcclusionTile& Tile : Tiles)
        {
            CoveredSubcells += std::popcount(static_cast<uint32>(Tile.CoverageMask));
            CoveredTiles += Tile.CoverageMask == FullCoverageMask ? 1u : 0u;
        }
        if (!Tiles.IsEmpty())
        {
            OutStats.SubcellCoveragePercent = 100.0f * static_cast<float>(CoveredSubcells) /
                static_cast<float>(Tiles.Num() * SubcellCount);
            OutStats.FullTileCoveragePercent = 100.0f * static_cast<float>(CoveredTiles) /
                static_cast<float>(Tiles.Num());
        }
    }

    const int32 TotalObjects = Objects.Num();
    if (TotalObjects > 0)
    {
        const uint32 NumWorkers = (std::max)(1u, FFiberJobManager::Get().GetNumWorkers());
        const int32 ChunkSize = (TotalObjects + NumWorkers - 1) / NumWorkers;
        const int32 NumJobs = (TotalObjects + ChunkSize - 1) / ChunkSize;

        if (WorkerVisibleBuffers.Num() < NumJobs)
        {
            WorkerVisibleBuffers.SetNum(NumJobs);
        }
        for (int32 i = 0; i < NumJobs; ++i)
        {
            WorkerVisibleBuffers[i].Reset();
        }

        // 가시성 결과 병렬 수집
        FFiberJobManager::Get().ParallelFor(TotalObjects, ChunkSize, [&](int32 Start, int32 End)
        {
            const int32 JobIndex = Start / ChunkSize;
            TArray<UPrimitiveComponent*>& Local = WorkerVisibleBuffers[JobIndex];
            Local.Reserve(End - Start);
            for (int32 Index = Start; Index < End; ++Index)
            {
                const FRenderableObject& Object = Objects[Index];
                if (Object.Primitive && VisibilityFlags.IsValidIndex(static_cast<int32>(Object.StableIndex)) &&
                    VisibilityFlags[Object.StableIndex] != 0)
                {
                    Local.Add(Object.Primitive);
                }
            }
        });

        OutVisible.Reserve(TotalObjects);
        for (int32 JobIndex = 0; JobIndex < NumJobs; ++JobIndex)
        {
            OutVisible.Append(WorkerVisibleBuffers[JobIndex]);
        }
    }
    OutStats.FinalVisible = static_cast<uint32>(OutVisible.Num());
    OutStats.CullMs = static_cast<float>((NowSeconds() - CullStartSeconds) * 1000.0);
    ActiveStats = nullptr;

    if (!bBVHFrustumOnly)
    {
        const uint32 Candidates = OutStats.OcclusionRejected + OutStats.FinalVisible;
        if (Candidates > 0 &&
            static_cast<float>(OutStats.OcclusionRejected) < MinOcclusionRejectRatio * static_cast<float>(Candidates))
        { }
            
            //Suspended = OcclusionProbeInterval;
    }
}
