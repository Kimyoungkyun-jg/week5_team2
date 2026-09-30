#include "EnginePCH.h"
#include "PrimitiveComponent.h"
#include "../Rendering/Renderer.h"
#include "Asset/AssetManager.h"
#include "Rendering/RenderCommand.h"
#include "GameFramework/Actor.h"
#include "World/World.h"

namespace
{
FString PrimitiveTypeToString(EPrimitiveType Type)
{
	switch (Type)
	{
	case EPrimitiveType::Sphere:
		return "Sphere";
		break;
	case EPrimitiveType::Cube:
		return "Cube";
		break;
	case EPrimitiveType::Cone:
		return "Cone";
		break;
	case EPrimitiveType::Plane:
		return "Plane";
		break;
	default:
		return "";
		break;
	}
}
} // namespace

UPrimitiveComponent::UPrimitiveComponent()
{
}

UPrimitiveComponent::~UPrimitiveComponent()
{
}

void UPrimitiveComponent::BeginPlay()
{
	Super::BeginPlay();
}

void UPrimitiveComponent::SubmitToRenderPackets(TArray<FRenderPacket>& OutPackets)
{
}

void UPrimitiveComponent::OnBoundsMarkedDirty()
{
	AActor* OwnerActor = GetOwner();
	if (OwnerActor && OwnerActor->GetWorld())
		OwnerActor->GetWorld()->MarkPrimitiveBoundsDirty(this);
}

bool UPrimitiveComponent::LineTraceComponent(const FRay& WorldRay, FHitResult& OutHit)
{

	// world Ray와 world AABB 비교
	const FBox Bounds = GetWorldBounds();
	if (!RayIntersectsAABB(WorldRay, Bounds.Min, Bounds.Max, OutHit.Distance))
		return false;

	const FStaticMeshData* Mesh = GetMeshData();
	return Mesh && TraceMesh(WorldRay, *Mesh, GetWorldMatrix(), OutHit);
}

// 피킹 전용 경로: 레이 역수는 컨텍스트에서 받고, 이미 찾은 최근접보다 먼 박스는 정밀 판정을 건너뛴다.
bool UPrimitiveComponent::LineTraceWithContext(const FTraceContext& Context, FHitResult& OutHit)
{
	const FBox Bounds = GetWorldBounds();
	float BoxEnter = 0.0f;

	//if (!RayIntersectsAABB(Context, Bounds.Min, Bounds.Max, BoxEnter))
	//	return false;

	// 박스 안의 물체는 박스 진입 거리보다 가까울 수 없다.
	// 이미 더 가까운 교차를 찾았다면 월드 행렬·역행렬·삼각형 판정을 생략한다.
	if (BoxEnter > Context.BestDistance)
		return false;

	const FStaticMeshData* Mesh = GetMeshData();
	return Mesh && TraceMesh(Context.Ray, *Mesh, GetWorldMatrix(), OutHit);
}

bool UPrimitiveComponent::TraceMesh(const FRay& WorldRay, const FStaticMeshData& Mesh, const FMatrix& WorldMatrix, FHitResult& OutResult)
{
	FRay LocalRay;
	if (!ToLocalRayAffine(WorldRay, WorldMatrix, LocalRay))
		return false;
	float T;

	if (!RayIntersectsMesh(LocalRay, Mesh, T))
		return false;

	OutResult.HitComponent = this;
	OutResult.Distance = T;
	OutResult.ImpactPoint = WorldRay.Origin + WorldRay.Direction * T;

	return true;
}
