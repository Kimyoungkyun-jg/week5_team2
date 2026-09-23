#include "EnginePCH.h"
#include "World.h"
#include "Level.h"

#include "ObjectSystem/ObjectFactory.h"
#include "Core/EngineStatics.h"
#include "GameFramework/Actor/StaticMeshActor.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Input/InputSystem.h"

#include "UObject/UObjectIterator.h"

#include "Collision/Ray.h"
#include "Component/BillboardComponent.h"

UWorld::~UWorld()
{

}

bool  UWorld::Init()
{
	// Spawn Actor로 카메라 생성하고 세팅하기
	PersistentLevel = FObjectFactory::ConstructObject<ULevel>();

	if (!PersistentLevel)
	{
		LOG(Error, "Failed to create PersistentLevel");
		return false;
	}

	//레벨 연결
	PersistentLevel->SetWorld(this);
	Levels.Add(PersistentLevel);
	CurrentLevel = PersistentLevel;

	//카메라 생성
	CreateMainCamera();

	return true;
}

AActor* UWorld::SpawnActor(UClass* Class, FName InName, const FTransform* Transform)
{
	if (!Class) return nullptr;
	if (!Class->IsChildOf(AActor::StaticClass())) return nullptr;

	// 1. ObjectFactory로 Actor 생성
	AActor* NewActor = Cast<AActor>(FObjectFactory::ConstructObject(Class, PersistentLevel, InName));

	if (!NewActor)
	{
		LOG(Error, "SpawnActor : Failed to create Actor");
		return nullptr;
	}

	// 2. Actor에 World/Level 연결
	NewActor->World = this;
	NewActor->Level = PersistentLevel;

	// 3. Transform 적용
	const FTransform SpawnTransform = Transform ? *Transform : FTransform::Identity;

	if (NewActor->GetRootComponent())
	{
		NewActor->GetRootComponent()->SetTransform(SpawnTransform);

		if (UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(NewActor->GetRootComponent()))
		{
			WorldPrimitiveComponents.Add(PrimComp);
		}

	}

	// 4. Level->Actors에 등록
	PersistentLevel->AddActor(NewActor);

	// 5. PlayList에 추가
	BeginPlayList.Enqueue(NewActor);

	return NewActor;
}

void UWorld::Tick(float DeltaTime)
{
	while (!BeginPlayList.IsEmpty())
	{
		BeginPlayList.Peek()->BeginPlay();
		BeginPlayList.Dequeue();
	}

	for (ULevel* Level : Levels)
	{
		for (AActor* Actor : Level->GetActors())
		{
			Actor->Tick(DeltaTime);
		}

	}

	if (MainCamera)
	{
		MainCamera->Tick(DeltaTime);
	}

}

void UWorld::ClearWorld()
{
	// BeginPlay 대기 중인 Actor 제거
	while (!BeginPlayList.IsEmpty())
	{
		BeginPlayList.Dequeue();
	}

	PathTracker.SetPlaybackEnabled(false);
	PathTracker.SetPathRenderingEnabled(false);
	PathTracker.ClearPath();

	for (ULevel* Level : Levels)
	{
		Level->ClearActors();
	}
	LOG(Info, "{} : ", PersistentLevel->GetActorNum());
}

void UWorld::GatherRenderPackets(TQueue<FRenderPacket>& RenderQueue)
{
	//for (UPrimitiveComponent* Primitive : PrimitiveComponents)
	for (TObjectIterator<UPrimitiveComponent> Itr; Itr; ++Itr)
	{
		if (*Itr && Itr->IsVisible())
			Itr->SubmitToRenderQueue(RenderQueue);
	}
}

// 메인 카메라 생성
void UWorld::CreateMainCamera()
{
	if (MainCamera)
		return;

	MainCamera = FObjectFactory::ConstructObject<ACameraActor>();

	if (!MainCamera)
	{
		LOG(Error, "Failed to create MainCamera");
		return;
	}

	MainCamera->World = this;
	MainCamera->Level = nullptr;
	MainCamera->GetCameraComponent()->SetRelativeLocation(FVector(-5.0f, -5.0f, 5.0f));
}

int32 UWorld::GetActorNum()
{
	return PersistentLevel->GetActorNum();
}

bool UWorld::DestroyActor(AActor* Actor)
{
	if (!Actor)
		return false;

	ULevel* Level = Actor->GetLevel();

	if (!Level)
		return false;

	// 1. BeginPlay 대기열에서 제거
	TQueue<AActor*> NewBeginPlayList;

	while (!BeginPlayList.IsEmpty())
	{
		AActor* PendingActor = BeginPlayList.Peek();
		BeginPlayList.Dequeue();

		if (PendingActor != Actor)
		{
			NewBeginPlayList.Enqueue(PendingActor);
		}
	}

	BeginPlayList = std::move(NewBeginPlayList);


	// 4. Level의 Actors에서 제거
	for (int32 i = Level->Actors.Num() - 1; i >= 0; --i)
	{
		if (Level->Actors[i] == Actor)
		{
			Level->Actors.RemoveAt(i, 1);
			break;
		}
	}

	FString ActorName = Actor->GetName();
	uint32 ActorUUID = Actor->GetUUID();

	// 6. Actor 삭제
	delete Actor;

	LOG(Info, "Destroy Actor : {} UUID {}", ActorName, ActorUUID);

	return true;
}

// 다른 World의 객체를 제외하고 Component 교차 중 최근접 결과를 선택한다.
bool UWorld::LineTraceSingle(const FRay& WorldRay, FHitResult& OutHit,
	FBillboardTraceTransform ResolveBillboard, const void* ViewContext)
{
	OutHit = FHitResult();
	for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
	{
		if (!It->IsVisible() || !It->GetOwner() || It->GetOwner()->GetWorld() != this) continue;
		FHitResult Hit;
		bool bHit = false;
		UBillboardComponent* Billboard = Cast<UBillboardComponent>(*It);
		if (Billboard && ResolveBillboard)
			bHit = Billboard->LineTraceComponentForView(WorldRay, Hit, ResolveBillboard(*Billboard, ViewContext));
		else
			bHit = It->LineTraceComponent(WorldRay, Hit);
		if (bHit && Hit.HitComponent && Hit.Distance >= 0.0f && Hit.Distance < OutHit.Distance)
			OutHit = Hit;
	}
	return OutHit.HitComponent != nullptr;
}

void UWorld::BeginPlay()
{
}

void UWorld::EndPlay()
{
}
