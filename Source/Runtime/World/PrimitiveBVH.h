#pragma once

#include "Collision/Ray.h"
#include "Component/PrimitiveComponent.h"
#include "ObjectSystem/TWeakObjectPtr.h"

#include <vector>

// World의 Primitive Bounds를 보관하는 피킹 전용 공간 인덱스다.
// View에 따라 형상이 달라지는 Primitive는 Bypass 목록에 두고 정밀 검사에서 처리한다.
class FPrimitiveBVH
{
public:
	void Update(const TArray<TWeakObjectPtr<UPrimitiveComponent>>& Primitives, uint64 TopologyRevision,
		const TArray<TWeakObjectPtr<UPrimitiveComponent>>& DirtyPrimitives);
	void Reset();
	void GatherRayCandidates(const FRay& Ray, TArray<FLineTraceCandidate>& OutCandidates) const;

	int32 GetPrimitiveCount() const { return Entries.Num() + BypassPrimitives.Num(); }

private:
	static constexpr uint32 InvalidNodeIndex = static_cast<uint32>(-1);

	struct FEntry
	{
		UPrimitiveComponent* Primitive = nullptr;
		FBox Bounds{};
	};

	struct FNode
	{
		FBox Bounds{};
		uint32 Left = 0;
		uint32 Right = 0;
		uint32 First = 0;
		uint32 Count = 0;
	};

	uint32 BuildNode(uint32 First, uint32 Count, uint32 Parent);
	FBox RefitNode(uint32 NodeIndex);
	void RefitFromLeaf(uint32 LeafIndex);
	void TraverseRay(const FTraceContext& Context, uint32 NodeIndex,
		TArray<FLineTraceCandidate>& OutCandidates) const;

	TArray<FEntry> Entries;
	TArray<uint64> EntryBoundsRevisions;
	TArray<UPrimitiveComponent*> BypassPrimitives;
	TArray<uint32> PrimitiveIndices;
	TArray<uint32> EntryLeafNodes;
	TArray<FNode> Nodes;
	// Query 시 건드리지 않는 refit 메타데이터는 Node와 분리해 traversal cache 밀도를 유지한다.
	TArray<uint32> NodeParents;
	TArray<uint64> NodeRefitSerials;
	std::vector<int32> EntryIndexByObjectIndex;
	uint64 LastTopologyRevision = 0;
	uint64 RefitSerial = 0;

	// Update에서 용량을 재사용해 매 프레임 임시 할당을 피한다.
	TArray<UPrimitiveComponent*> UpdateBoundedPrimitives;
	TArray<UPrimitiveComponent*> UpdateBypassPrimitives;
	TArray<uint32> UpdateDirtyEntries;
	TArray<uint32> UpdateDirtyLeaves;
};
