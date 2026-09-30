#include "EnginePCH.h"
#include "PrimitiveBVH.h"

#include "Component/StaticMeshComponent.h"

#include <algorithm>
#include <array>
#include <limits>

namespace
{
	constexpr uint32 PrimitiveBVHLeafSize = 16;
	constexpr int32 SAHBinCount = 16;
	constexpr float SplitEpsilon = 1.0e-6f;

	FBox UnionBounds(const FBox& A, const FBox& B)
	{
		return {
			FVector(
				(std::min)(A.Min.X, B.Min.X),
				(std::min)(A.Min.Y, B.Min.Y),
				(std::min)(A.Min.Z, B.Min.Z)),
			FVector(
				(std::max)(A.Max.X, B.Max.X),
				(std::max)(A.Max.Y, B.Max.Y),
				(std::max)(A.Max.Z, B.Max.Z))};
	}

	FVector BoundsCenter(const FBox& Bounds)
	{
		return (Bounds.Min + Bounds.Max) * 0.5f;
	}

	float SurfaceArea(const FBox& Bounds)
	{
		const FVector Size = Bounds.Max - Bounds.Min;
		return 2.0f * (Size.X * Size.Y + Size.Y * Size.Z + Size.Z * Size.X);
	}

	float AxisValue(const FVector& Value, const int32 Axis)
	{
		return Axis == 0 ? Value.X : Axis == 1 ? Value.Y : Value.Z;
	}

	bool CanUseBoundsForPicking(UPrimitiveComponent* Primitive)
	{
		return Cast<UStaticMeshComponent>(Primitive) != nullptr;
	}
}

void FPrimitiveBVH::Update(
	const TArray<TWeakObjectPtr<UPrimitiveComponent>>& Primitives, const uint64 TopologyRevision,
	const TArray<TWeakObjectPtr<UPrimitiveComponent>>& DirtyPrimitives)
{
	if (LastTopologyRevision != TopologyRevision)
	{
		LastTopologyRevision = TopologyRevision;
		UpdateBoundedPrimitives.Reset();
		UpdateBypassPrimitives.Reset();
		UpdateBoundedPrimitives.Reserve(Primitives.Num());
		UpdateBypassPrimitives.Reserve(Primitives.Num());

		for (const TWeakObjectPtr<UPrimitiveComponent>& WeakPrimitive : Primitives)
		{
			UPrimitiveComponent* Primitive = WeakPrimitive.Get();
			if (!Primitive)
				continue;

			if (CanUseBoundsForPicking(Primitive))
				UpdateBoundedPrimitives.Add(Primitive);
			else
				UpdateBypassPrimitives.Add(Primitive);
		}

		Entries.Reset();
		EntryBoundsRevisions.Reset();
		BypassPrimitives.Reset();
		PrimitiveIndices.Reset();
		EntryLeafNodes.Reset();
		Nodes.Reset();
		NodeParents.Reset();
		NodeRefitSerials.Reset();
		EntryIndexByObjectIndex.clear();

		Entries.Reserve(UpdateBoundedPrimitives.Num());
		EntryBoundsRevisions.Reserve(UpdateBoundedPrimitives.Num());
		PrimitiveIndices.Reserve(UpdateBoundedPrimitives.Num());
		Nodes.Reserve((std::max)(1, UpdateBoundedPrimitives.Num() * 2));
		NodeParents.Reserve((std::max)(1, UpdateBoundedPrimitives.Num() * 2));
		NodeRefitSerials.Reserve((std::max)(1, UpdateBoundedPrimitives.Num() * 2));
		for (UPrimitiveComponent* Primitive : UpdateBoundedPrimitives)
		{
			FEntry Entry{};
			Entry.Primitive = Primitive;
			Entry.Bounds = Primitive->GetWorldBounds();
			const uint32 EntryIndex = Entries.Add(std::move(Entry));
			EntryBoundsRevisions.Add(Primitive->GetBoundsRevision());
			PrimitiveIndices.Add(EntryIndex);
			const uint32 ObjectIndex = Primitive->GetInternalIndex();
			if (EntryIndexByObjectIndex.size() <= ObjectIndex)
				EntryIndexByObjectIndex.resize(static_cast<size_t>(ObjectIndex) + 1, -1);
			EntryIndexByObjectIndex[ObjectIndex] = static_cast<int32>(EntryIndex);
		}
		EntryLeafNodes.SetNum(Entries.Num(), false);

		BypassPrimitives.Reserve(UpdateBypassPrimitives.Num());
		for (UPrimitiveComponent* Primitive : UpdateBypassPrimitives)
			BypassPrimitives.Add(Primitive);

		if (!PrimitiveIndices.IsEmpty())
			BuildNode(0, static_cast<uint32>(PrimitiveIndices.Num()), InvalidNodeIndex);
		return;
	}

	UpdateDirtyEntries.Reset();
	for (const TWeakObjectPtr<UPrimitiveComponent>& WeakPrimitive : DirtyPrimitives)
	{
		UPrimitiveComponent* Primitive = WeakPrimitive.Get();
		if (!Primitive)
			continue;

		const uint32 ObjectIndex = Primitive->GetInternalIndex();
		if (ObjectIndex >= EntryIndexByObjectIndex.size())
			continue;

		const int32 EntryIndex = EntryIndexByObjectIndex[ObjectIndex];
		if (EntryIndex < 0 || Entries[EntryIndex].Primitive != Primitive)
			continue;

		FEntry& Entry = Entries[EntryIndex];
		if (EntryBoundsRevisions[EntryIndex] != Primitive->GetBoundsRevision())
		{
			Entry.Bounds = Primitive->GetWorldBounds();
			EntryBoundsRevisions[EntryIndex] = Primitive->GetBoundsRevision();
			UpdateDirtyEntries.Add(static_cast<uint32>(EntryIndex));
		}
	}

	if (UpdateDirtyEntries.IsEmpty() || Nodes.IsEmpty())
		return;

	const int32 FullRefitThreshold = (std::max)(64, Entries.Num() / 128);
	if (UpdateDirtyEntries.Num() >= FullRefitThreshold)
	{
		RefitNode(0);
		return;
	}

	++RefitSerial;
	UpdateDirtyLeaves.Reset();
	for (const uint32 EntryIndex : UpdateDirtyEntries)
	{
		const uint32 LeafIndex = EntryLeafNodes[EntryIndex];
		if (NodeRefitSerials[LeafIndex] != RefitSerial)
		{
			NodeRefitSerials[LeafIndex] = RefitSerial;
			UpdateDirtyLeaves.Add(LeafIndex);
		}
	}
	for (const uint32 LeafIndex : UpdateDirtyLeaves)
		RefitFromLeaf(LeafIndex);
}

void FPrimitiveBVH::Reset()
{
	Entries.Reset();
	EntryBoundsRevisions.Reset();
	BypassPrimitives.Reset();
	PrimitiveIndices.Reset();
	EntryLeafNodes.Reset();
	Nodes.Reset();
	NodeParents.Reset();
	NodeRefitSerials.Reset();
	EntryIndexByObjectIndex.clear();
	UpdateBoundedPrimitives.Reset();
	UpdateBypassPrimitives.Reset();
	UpdateDirtyEntries.Reset();
	UpdateDirtyLeaves.Reset();
	LastTopologyRevision = 0;
	RefitSerial = 0;
}

void FPrimitiveBVH::GatherRayCandidates(const FRay& Ray,
	TArray<FLineTraceCandidate>& OutCandidates) const
{
	OutCandidates.Reset();
	const FTraceContext Context = MakeTraceContext(Ray, nullptr, nullptr);
	if (!Nodes.IsEmpty())
	{
		float RootDistance = 0.0f;
		if (RayIntersectsAABB(
			Context, Nodes[0].Bounds.Min, Nodes[0].Bounds.Max, RootDistance))
		{
			TraverseRay(Context, 0, OutCandidates);
		}
	}

	for (UPrimitiveComponent* Primitive : BypassPrimitives)
	{
		if (Primitive)
			OutCandidates.Add({Primitive, 0.0f, false});
	}

	std::sort(OutCandidates.begin(), OutCandidates.end(),
		[](const FLineTraceCandidate& A, const FLineTraceCandidate& B)
		{
			if (A.bHasBoundsDistance != B.bHasBoundsDistance)
				return A.bHasBoundsDistance;
			return A.bHasBoundsDistance && A.BoundsDistance < B.BoundsDistance;
		});
}

uint32 FPrimitiveBVH::BuildNode(const uint32 First, const uint32 Count, const uint32 Parent)
{
	FBox NodeBounds = Entries[PrimitiveIndices[First]].Bounds;
	FVector CentroidMin = BoundsCenter(NodeBounds);
	FVector CentroidMax = CentroidMin;
	for (uint32 Offset = 1; Offset < Count; ++Offset)
	{
		const FBox& Bounds = Entries[PrimitiveIndices[First + Offset]].Bounds;
		const FVector Center = BoundsCenter(Bounds);
		NodeBounds = UnionBounds(NodeBounds, Bounds);
		CentroidMin.X = (std::min)(CentroidMin.X, Center.X);
		CentroidMin.Y = (std::min)(CentroidMin.Y, Center.Y);
		CentroidMin.Z = (std::min)(CentroidMin.Z, Center.Z);
		CentroidMax.X = (std::max)(CentroidMax.X, Center.X);
		CentroidMax.Y = (std::max)(CentroidMax.Y, Center.Y);
		CentroidMax.Z = (std::max)(CentroidMax.Z, Center.Z);
	}

	FNode Node{};
	Node.Bounds = NodeBounds;
	const uint32 NodeIndex = Nodes.Add(Node);
	NodeParents.Add(Parent);
	NodeRefitSerials.Add(0);
	if (Count <= PrimitiveBVHLeafSize)
	{
		Nodes[NodeIndex].First = First;
		Nodes[NodeIndex].Count = Count;
		for (uint32 Offset = 0; Offset < Count; ++Offset)
			EntryLeafNodes[PrimitiveIndices[First + Offset]] = NodeIndex;
		return NodeIndex;
	}

	struct FBin
	{
		FBox Bounds{};
		uint32 Count = 0;
		bool bValid = false;
	};

	float BestCost = (std::numeric_limits<float>::max)();
	int32 BestAxis = -1;
	int32 BestSplit = -1;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const float Minimum = AxisValue(CentroidMin, Axis);
		const float Extent = AxisValue(CentroidMax, Axis) - Minimum;
		if (Extent <= SplitEpsilon)
			continue;

		std::array<FBin, SAHBinCount> Bins{};
		for (uint32 Offset = 0; Offset < Count; ++Offset)
		{
			const FBox& Bounds = Entries[PrimitiveIndices[First + Offset]].Bounds;
			const int32 BinIndex = std::clamp(
				static_cast<int32>((AxisValue(BoundsCenter(Bounds), Axis) - Minimum) / Extent * SAHBinCount),
				0, SAHBinCount - 1);
			FBin& Bin = Bins[BinIndex];
			Bin.Bounds = Bin.bValid ? UnionBounds(Bin.Bounds, Bounds) : Bounds;
			Bin.bValid = true;
			++Bin.Count;
		}

		std::array<FBox, SAHBinCount> LeftBounds{};
		std::array<FBox, SAHBinCount> RightBounds{};
		std::array<uint32, SAHBinCount> LeftCounts{};
		std::array<uint32, SAHBinCount> RightCounts{};
		bool bLeftValid = false;
		bool bRightValid = false;
		for (int32 BinIndex = 0; BinIndex < SAHBinCount; ++BinIndex)
		{
			if (Bins[BinIndex].bValid)
			{
				LeftBounds[BinIndex] = bLeftValid
					? UnionBounds(LeftBounds[BinIndex - 1], Bins[BinIndex].Bounds)
					: Bins[BinIndex].Bounds;
				bLeftValid = true;
			}
			else if (BinIndex > 0)
			{
				LeftBounds[BinIndex] = LeftBounds[BinIndex - 1];
			}
			LeftCounts[BinIndex] = Bins[BinIndex].Count +
				(BinIndex > 0 ? LeftCounts[BinIndex - 1] : 0);
		}
		for (int32 BinIndex = SAHBinCount - 1; BinIndex >= 0; --BinIndex)
		{
			if (Bins[BinIndex].bValid)
			{
				RightBounds[BinIndex] = bRightValid
					? UnionBounds(RightBounds[BinIndex + 1], Bins[BinIndex].Bounds)
					: Bins[BinIndex].Bounds;
				bRightValid = true;
			}
			else if (BinIndex + 1 < SAHBinCount)
			{
				RightBounds[BinIndex] = RightBounds[BinIndex + 1];
			}
			RightCounts[BinIndex] = Bins[BinIndex].Count +
				(BinIndex + 1 < SAHBinCount ? RightCounts[BinIndex + 1] : 0);
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

	auto Begin = PrimitiveIndices.begin() + First;
	auto End = Begin + Count;
	uint32 LeftCount = 0;
	if (BestAxis >= 0)
	{
		const float Minimum = AxisValue(CentroidMin, BestAxis);
		const float Extent = AxisValue(CentroidMax, BestAxis) - Minimum;
		const auto Middle = std::partition(Begin, End,
			[&](const uint32 EntryIndex)
			{
				const float Center = AxisValue(BoundsCenter(Entries[EntryIndex].Bounds), BestAxis);
				const int32 Bin = std::clamp(
					static_cast<int32>((Center - Minimum) / Extent * SAHBinCount), 0, SAHBinCount - 1);
				return Bin <= BestSplit;
			});
		LeftCount = static_cast<uint32>(Middle - Begin);
	}

	if (LeftCount == 0 || LeftCount == Count)
	{
		const FVector Range = CentroidMax - CentroidMin;
		const int32 Axis = Range.X >= Range.Y && Range.X >= Range.Z ? 0 : Range.Y >= Range.Z ? 1 : 2;
		LeftCount = Count / 2;
		std::nth_element(Begin, Begin + LeftCount, End,
			[&](const uint32 A, const uint32 B)
			{
				return AxisValue(BoundsCenter(Entries[A].Bounds), Axis) <
					AxisValue(BoundsCenter(Entries[B].Bounds), Axis);
			});
	}

	const uint32 Left = BuildNode(First, LeftCount, NodeIndex);
	const uint32 Right = BuildNode(First + LeftCount, Count - LeftCount, NodeIndex);
	Nodes[NodeIndex].Left = Left;
	Nodes[NodeIndex].Right = Right;
	return NodeIndex;
}

FBox FPrimitiveBVH::RefitNode(const uint32 NodeIndex)
{
	FNode& Node = Nodes[NodeIndex];
	if (Node.Count > 0)
	{
		FBox Bounds = Entries[PrimitiveIndices[Node.First]].Bounds;
		for (uint32 Offset = 1; Offset < Node.Count; ++Offset)
			Bounds = UnionBounds(Bounds, Entries[PrimitiveIndices[Node.First + Offset]].Bounds);
		Node.Bounds = Bounds;
		return Bounds;
	}

	Node.Bounds = UnionBounds(RefitNode(Node.Left), RefitNode(Node.Right));
	return Node.Bounds;
}

void FPrimitiveBVH::RefitFromLeaf(const uint32 LeafIndex)
{
	FNode& Leaf = Nodes[LeafIndex];
	FBox Bounds = Entries[PrimitiveIndices[Leaf.First]].Bounds;
	for (uint32 Offset = 1; Offset < Leaf.Count; ++Offset)
		Bounds = UnionBounds(Bounds, Entries[PrimitiveIndices[Leaf.First + Offset]].Bounds);
	Leaf.Bounds = Bounds;

	uint32 ParentIndex = NodeParents[LeafIndex];
	while (ParentIndex != InvalidNodeIndex)
	{
		FNode& Parent = Nodes[ParentIndex];
		Parent.Bounds = UnionBounds(Nodes[Parent.Left].Bounds, Nodes[Parent.Right].Bounds);
		ParentIndex = NodeParents[ParentIndex];
	}
}

void FPrimitiveBVH::TraverseRay(const FTraceContext& Context, const uint32 NodeIndex,
	TArray<FLineTraceCandidate>& OutCandidates) const
{
	const FNode& Node = Nodes[NodeIndex];
	if (Node.Count > 0)
	{
		for (uint32 Offset = 0; Offset < Node.Count; ++Offset)
		{
			const FEntry& Entry = Entries[PrimitiveIndices[Node.First + Offset]];
			UPrimitiveComponent* Primitive = Entry.Primitive;
			float BoundsDistance = 0.0f;
			if (Primitive && RayIntersectsAABB(
				Context, Entry.Bounds.Min, Entry.Bounds.Max, BoundsDistance))
			{
				OutCandidates.Add({Primitive, BoundsDistance, true});
			}
		}
		return;
	}

	float LeftDistance = 0.0f;
	float RightDistance = 0.0f;
	const FNode& LeftNode = Nodes[Node.Left];
	const FNode& RightNode = Nodes[Node.Right];
	const bool bHitLeft = RayIntersectsAABB(
		Context, LeftNode.Bounds.Min, LeftNode.Bounds.Max, LeftDistance);
	const bool bHitRight = RayIntersectsAABB(
		Context, RightNode.Bounds.Min, RightNode.Bounds.Max, RightDistance);

	if (bHitLeft && bHitRight)
	{
		const uint32 NearNode = LeftDistance <= RightDistance ? Node.Left : Node.Right;
		const uint32 FarNode = LeftDistance <= RightDistance ? Node.Right : Node.Left;
		TraverseRay(Context, NearNode, OutCandidates);
		TraverseRay(Context, FarNode, OutCandidates);
	}
	else if (bHitLeft)
	{
		TraverseRay(Context, Node.Left, OutCandidates);
	}
	else if (bHitRight)
	{
		TraverseRay(Context, Node.Right, OutCandidates);
	}
}
