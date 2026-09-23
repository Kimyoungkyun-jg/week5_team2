#pragma once

#include "../Math/Transform.h"
#include "ActorComponent.h"

class USceneComponent : public UActorComponent
{
	DECLARE_CLASS(USceneComponent, UActorComponent)

	REFLECT_START(ClassName)
		PROPERTY(Transform)
		REFLECT_END()

public:
	USceneComponent() = default;
	virtual ~USceneComponent() override;

	// Get & Set
	const FVector& GetRelativeLocation() const { return Transform.Location; }
	void SetRelativeLocation(const FVector& InLocation) { Transform.Location = InLocation; }

	const FRotator& GetRelativeRotation() const { return Transform.Rotation; }
	void SetRelativeRotation(const FRotator& InRotation) { Transform.Rotation = InRotation; }

	const FVector& GetRelativeScale3D() const { return Transform.Scale; }
	void SetRelativeScale3D(const FVector& InScale) { Transform.Scale = InScale; }

	// 쿼터니언 적용된 회전행렬
	FQuat GetRelativeRotationQuat() const { return Transform.GetOrientation(); }

	const FTransform& GetTransform() const { return Transform; }
	void SetTransform(const FTransform& InTransform) { 
		Transform = InTransform; 
		bBoundsDirty = true; //위치가 바뀔때만 더티 마킹
	}

	// Attatch-To
	USceneComponent* GetAttachParent() const { return AttachParent; }
	const TArray<USceneComponent*>& GetAttachChildren() const { return AttachChildren; }
	void SetupAttachment(USceneComponent* InParent);
	void DetachFromParent();

	virtual FBox CalcLocalBounds() const { return FBox{ FVector(), FVector() }; }
	FBox CalcBounds() const { return CalcLocalBounds().GetWorldAABB(GetWorldMatrix()); }

	FVector GetWorldLocation() const;
	FRotator GetWorldRotation() const;
	FVector GetWorldScale3D() const;
	FMatrix GetWorldMatrix() const;

	// 위치가 바뀌면 더티 마킹
	void MarkBoundsDirty() { bBoundsDirty = true; }

	//더티할 때만 새로 계산하고, 아니면 캐시된 박스 즉시 반환!
	const FBox GetWorldBounds()
	{
		if (bBoundsDirty)
		{
			CachedWorldBounds = CalcBounds();
			bBoundsDirty = false;
		}
		return CachedWorldBounds;
	}

protected:
	FTransform Transform;

	USceneComponent* AttachParent = nullptr; // Attach 부모 정보
	TArray<USceneComponent*> AttachChildren;


	bool bBoundsDirty = true; // 처음 생성 시에는 계산 필요
	FBox CachedWorldBounds;
};