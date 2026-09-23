#include "EnginePCH.h"
#include "DefaultSceneLoader.h"

#include <fstream>
#include <string>
#include <sstream>

#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor/StaticMeshActor.h"
#include "Component/StaticMeshComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Asset/AssetManager.h"
#include "Render/Mesh.h"
#include "Core/EngineLog.h"

bool FDefaultSceneLoader::LoadScene(UWorld* World, const FString& Path)
{
	if (!World) return false;

	std::ifstream File(Path);
	if (!File.is_open())
	{
		LOG(Warning, "Failed to open scene file: {}", Path);
		return false;
	}

	LOG(Info, "Loading Default.scene (Single-Pass)...");

	// 사과 메시 사전 캐싱
	UStaticMesh* AppleMesh = UAssetManager::GetAssetByPath<UStaticMesh>("Models/Apple/apple_mid.obj");
	UStaticMesh* BittenAppleMesh = UAssetManager::GetAssetByPath<UStaticMesh>("Models/Apple/bitten_apple_mid.obj");
	if (!AppleMesh) AppleMesh = UAssetManager::GetAssetByPath<UStaticMesh>("Cube");
	if (!BittenAppleMesh) BittenAppleMesh = AppleMesh;

	std::string Line;
	bool bInPrimitives = false;
	bool bInCamera = false;

	uint32 CurrentUUID = 0;
	FVector Loc(0.0f, 0.0f, 0.0f);
	FRotator Rot(0.0f, 0.0f, 0.0f);
	FVector Scale(1.0f, 1.0f, 1.0f);
	bool bIsBitten = false;
	bool bHasObject = false;

	int32 SpawnedCount = 0;

	while (std::getline(File, Line))
	{
		// PerspectiveCamera 섹션 시작
		if (!bInPrimitives && Line.find("\"PerspectiveCamera\"") != std::string::npos)
		{
			bInCamera = true;
			continue;
		}

		// Primitives 섹션 시작
		if (Line.find("\"Primitives\"") != std::string::npos)
		{
			bInCamera = false;
			bInPrimitives = true;
			continue;
		}

		// 카메라 데이터 파싱
		if (bInCamera && World->GetMainCamera())
		{
			if (Line.find("\"Location\"") != std::string::npos)
			{
				float X = 0.0f, Y = 0.0f, Z = 0.0f;
				if (const char* Bracket = strchr(Line.c_str(), '['))
				{
					if (sscanf(Bracket + 1, "%f , %f , %f", &X, &Y, &Z) >= 3 ||
						sscanf(Bracket + 1, "%f, %f, %f", &X, &Y, &Z) >= 3)
					{
						FTransform CamTransform = World->GetMainCamera()->GetActorTransform();
						CamTransform.Location = FVector(X, Y, Z);
						if (World->GetMainCamera()->GetRootComponent())
							World->GetMainCamera()->GetRootComponent()->SetTransform(CamTransform);
					}
				}
			}
			else if (Line.find("\"Rotation\"") != std::string::npos)
			{
				float R0 = 0.0f, R1 = 0.0f, R2 = 0.0f;
				if (const char* Bracket = strchr(Line.c_str(), '['))
				{
					if (sscanf(Bracket + 1, "%f , %f , %f", &R0, &R1, &R2) >= 3 ||
						sscanf(Bracket + 1, "%f, %f, %f", &R0, &R1, &R2) >= 3)
					{
						float Deg0 = R0 * 180.0f / 3.14159265f;
						float Deg1 = R1 * 180.0f / 3.14159265f;
						float Deg2 = R2 * 180.0f / 3.14159265f;
						FTransform CamTransform = World->GetMainCamera()->GetActorTransform();
						CamTransform.Rotation = FRotator(Deg2, Deg1, Deg0);
						if (World->GetMainCamera()->GetRootComponent())
							World->GetMainCamera()->GetRootComponent()->SetTransform(CamTransform);
					}
				}
			}
			else if (Line.find("\"FOV\"") != std::string::npos)
			{
				float Fov = 60.0f;
				if (const char* Bracket = strchr(Line.c_str(), '['))
					if (sscanf(Bracket + 1, "%f", &Fov) >= 1 && World->GetMainCamera()->GetCameraComponent())
						World->GetMainCamera()->GetCameraComponent()->SetFieldOfView(Fov);
			}
			else if (Line.find("\"NearClip\"") != std::string::npos)
			{
				float NearZ = 0.1f;
				if (const char* Bracket = strchr(Line.c_str(), '['))
					if (sscanf(Bracket + 1, "%f", &NearZ) >= 1 && World->GetMainCamera()->GetCameraComponent())
						World->GetMainCamera()->GetCameraComponent()->SetNearZ(NearZ);
			}
			else if (Line.find("\"FarClip\"") != std::string::npos)
			{
				float FarZ = 100.0f;
				if (const char* Bracket = strchr(Line.c_str(), '['))
					if (sscanf(Bracket + 1, "%f", &FarZ) >= 1 && World->GetMainCamera()->GetCameraComponent())
						World->GetMainCamera()->GetCameraComponent()->SetFarZ(FarZ);
			}
			continue;
		}

		// Primitives 섹션 파싱
		if (bInPrimitives)
		{
			// 새로운 UUID 항목 시작 확인
			if (Line.find('{') != std::string::npos)
			{
				// 숫자 UUID 추출
				size_t Q1 = Line.find('\"');
				if (Q1 != std::string::npos)
				{
					size_t Q2 = Line.find('\"', Q1 + 1);
					if (Q2 != std::string::npos)
					{
						std::string Key = Line.substr(Q1 + 1, Q2 - Q1 - 1);
						char* EndPtr = nullptr;
						uint32 Uid = static_cast<uint32>(strtoul(Key.c_str(), &EndPtr, 10));
						if (Uid > 0 && EndPtr == Key.c_str() + Key.length())
						{
							CurrentUUID = Uid;
							Loc = FVector(0.0f, 0.0f, 0.0f);
							Rot = FRotator(0.0f, 0.0f, 0.0f);
							Scale = FVector(1.0f, 1.0f, 1.0f);
							bIsBitten = false;
							bHasObject = true;
						}
					}
				}
				continue;
			}

			if (bHasObject)
			{
				if (Line.find("\"Location\"") != std::string::npos)
				{
					if (const char* Bracket = strchr(Line.c_str(), '['))
					{
						float X = 0.0f, Y = 0.0f, Z = 0.0f;
						if (sscanf(Bracket + 1, "%f , %f , %f", &X, &Y, &Z) >= 3 ||
							sscanf(Bracket + 1, "%f, %f, %f", &X, &Y, &Z) >= 3)
						{
							Loc = FVector(X, Y, Z);
						}
					}
				}
				else if (Line.find("\"Rotation\"") != std::string::npos)
				{
					if (const char* Bracket = strchr(Line.c_str(), '['))
					{
						float R0 = 0.0f, R1 = 0.0f, R2 = 0.0f;
						if (sscanf(Bracket + 1, "%f , %f , %f", &R0, &R1, &R2) >= 3 ||
							sscanf(Bracket + 1, "%f, %f, %f", &R0, &R1, &R2) >= 3)
						{
							Rot = FRotator(R0, R1, R2);
						}
					}
				}
				else if (Line.find("\"Scale\"") != std::string::npos)
				{
					if (const char* Bracket = strchr(Line.c_str(), '['))
					{
						float SX = 1.0f, SY = 1.0f, SZ = 1.0f;
						if (sscanf(Bracket + 1, "%f , %f , %f", &SX, &SY, &SZ) >= 3 ||
							sscanf(Bracket + 1, "%f, %f, %f", &SX, &SY, &SZ) >= 3)
						{
							Scale = FVector(SX, SY, SZ);
						}
					}
				}
				else if (Line.find("\"ObjStaticMeshAsset\"") != std::string::npos)
				{
					if (Line.find("bitten") != std::string::npos)
					{
						bIsBitten = true;
					}
				}
				else if (Line.find('}') != std::string::npos)
				{
					// 오브젝트 파싱 완료 -> 즉시 스폰
					FTransform SpawnTransform;
					SpawnTransform.Location = Loc;
					SpawnTransform.Rotation = Rot;
					SpawnTransform.Scale = Scale;

					AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(NAME_None, &SpawnTransform);
					if (Actor)
					{
						Actor->SetUUID(CurrentUUID);
						if (UStaticMeshComponent* Comp = Actor->GetStaticMeshComponent())
						{
							Comp->SetUUID(CurrentUUID);
							Comp->SetStaticMesh(bIsBitten ? BittenAppleMesh : AppleMesh);
							Comp->MarkBoundsDirty();
						}
						++SpawnedCount;
					}

					bHasObject = false;
				}
			}
		}
	}

	LOG(Info, "Default.scene Single-Pass Loading Completed: {} actors spawned", SpawnedCount);
	return true;
}

