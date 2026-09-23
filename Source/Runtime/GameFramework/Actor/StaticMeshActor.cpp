#include "EnginePCH.h"
#include "StaticMeshActor.h"

#include "Component/PrimitiveComponent.h"
#include "ObjectSystem/ObjectFactory.h"
#include "Asset/AssetManager.h"

namespace
{
	FString PrimitiveTypeToString(EPrimitiveType Type)
	{
		switch (Type)
		{
		case EPrimitiveType::Cube:
			return "Cube";

		case EPrimitiveType::Sphere:
			return "Sphere";

		case EPrimitiveType::Plane:
			return "Plane";

		case EPrimitiveType::Cone:
			return "Cone";

		default:
			return "";
		}
	}
}

AStaticMeshActor::AStaticMeshActor()
{
	StaticMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>("UPrimitiveComponent");
	SetRootComponent(StaticMeshComponent);

}

void AStaticMeshActor::SetPrimitiveType(EPrimitiveType Type)
{
	//StaticMeshComponent->SetType(Type);
	//StaticMeshComponent->SetMesh(UAssetManager::GetAssetByPath<UStaticMesh>(PrimitiveTypeToString(Type)));
}

void AStaticMeshActor::BeginPlay()
{
	Super::BeginPlay();
}
