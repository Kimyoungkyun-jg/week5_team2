#pragma once

#include "Vertex.h"

struct FBox
{
	FVector Min;
	FVector Max;

	FBox GetWorldAABB(const FMatrix& M) const
	{
		FVector Center = (Min + Max) * 0.5f;
		FVector Extent = (Max - Min) * 0.5f;

		// 중심은 그냥 변환
		FVector4 C = FVector4(Center, 1.0f) * M;

		// 범위는 회전 부분의 절댓값으로 변환
		FVector E;
		E.X = Extent.X * fabsf(M.M[0][0]) + Extent.Y * fabsf(M.M[1][0]) + Extent.Z * fabsf(M.M[2][0]);
		E.Y = Extent.X * fabsf(M.M[0][1]) + Extent.Y * fabsf(M.M[1][1]) + Extent.Z * fabsf(M.M[2][1]);
		E.Z = Extent.X * fabsf(M.M[0][2]) + Extent.Y * fabsf(M.M[1][2]) + Extent.Z * fabsf(M.M[2][2]);

		return FBox{ FVector(C.X, C.Y, C.Z) - E, FVector(C.X, C.Y, C.Z) + E };
	}
};

// Cooked obj Data의 Section 구조체
struct FStaticMeshSection
{
	uint32 StartIndex = 0;
	uint32 IndexCount = 0;
	uint32 MaterialSlotIndex = 0;
};

struct FStaticMaterialSlot
{
	FString Name;
	FVector4 BaseColor = FVector4(1, 1, 1, 1);
	FString DiffuseTexturePath;
};

struct FStaticMeshData
{
	TArray<FVertexPNCT> Vertices;
	TArray<uint32> Indices;
	TArray<FStaticMeshSection> Sections;
	TArray<FStaticMaterialSlot> MaterialSlots;
	FBox AABB;

	// TODO: 나중에 Sections, MaterialSlots 도 Append 해줘야 함.
	void Append(const FStaticMeshData& Other)
	{
		uint32 Base = (uint32)Vertices.Num();

		Vertices.Append(Other.Vertices);

		for (uint32 i : Other.Indices)
			Indices.Add(Base + i);
	}

	void Translate(const FVector& Offset)
	{
		for (auto& V : Vertices)
			V.Position += Offset;
	}

	// Vertices, Indices, Sections, MaterialSlots가 서로 맞는지 검사한다.
	// 실패하면 이유를 OutError에 담는다. 로그는 호출한 쪽이 경로와 함께 남긴다.
	bool Validate(FString& OutError) const;
};
