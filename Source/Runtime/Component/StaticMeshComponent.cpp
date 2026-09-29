#include "EnginePCH.h"
#include "StaticMeshComponent.h"
#include "Asset/AssetManager.h"
#include "Rendering/RenderCommand.h"


// StaticMesh 컴포넌트를 초기화한다.
UStaticMeshComponent::UStaticMeshComponent()
{
	StaticMesh = UAssetManager::GetAssetByKey<UStaticMesh>("Cube");
}

// StaticMesh 컴포넌트의 소멸을 처리한다.
UStaticMeshComponent::~UStaticMeshComponent()
{
}

// 부모 컴포넌트의 시작 처리를 호출한다.
void UStaticMeshComponent::BeginPlay()
{
	Super::BeginPlay();
}

// 부모 컴포넌트의 프레임 갱신을 호출한다.
void UStaticMeshComponent::TickComponent(float DeltaTime)
{
	Super::TickComponent(DeltaTime);
}

void UStaticMeshComponent::SetStaticMesh(UStaticMesh* InStaticMesh)
{
    if (StaticMesh == InStaticMesh)
        return;

    StaticMesh = InStaticMesh;
    ClearOverrideMaterials();
    MarkBoundsDirtyRecursive();
}

int32 UStaticMeshComponent::GetNumMaterials() const
{
    return StaticMesh ? static_cast<int32>(StaticMesh->GetMeshData().MaterialSlots.Num()) : 0;
}

FString UStaticMeshComponent::GetMaterialSlotName(int32 SlotIndex) const
{
    if (!StaticMesh || SlotIndex < 0 || SlotIndex >= GetNumMaterials())
        return FString();
    return StaticMesh->GetMeshData().MaterialSlots[SlotIndex].Name;
}

UMaterial* UStaticMeshComponent::GetDefaultMaterial(int32 SlotIndex) const
{
    return StaticMesh ? StaticMesh->GetMaterial(static_cast<uint32>(SlotIndex)) : nullptr;
}

// TArray 기반 고속 패킷 제출
void UStaticMeshComponent::SubmitToRenderPackets(TArray<FRenderPacket>& OutPackets)
{
    if (!StaticMesh)
        return;

    const FStaticMeshData& MeshData = StaticMesh->GetMeshData();
    const FMatrix WorldMatrix = GetWorldMatrix();

    for (const FStaticMeshSection& Section : MeshData.Sections)
    {
        UMaterial* SectionMaterial = GetMaterial(static_cast<int32>(Section.MaterialSlotIndex));
        if (!SectionMaterial)
            continue;

        FRenderPacket rp;
        rp.mesh = StaticMesh;
        rp.model = WorldMatrix;
        rp.StartIndex = Section.StartIndex;
        rp.IndexCount = Section.IndexCount;
        rp.material = SectionMaterial;

        OutPackets.Add(rp);
    }
}
