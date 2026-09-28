#pragma once
#include "Math/Transform.h"
#include "Mesh.h"
#include "Shader.h"
#include "Material.h"

// Todo: subuv
class UTexture2D;

struct FRenderPacket {
	FMatrix model;
	UStaticMesh* mesh = nullptr;
	UMaterial* material = nullptr;

	// 카메라와의 거리 제곱. 반투명 정렬에 사용
	float CameraToParticleDistance = 0.0f;

	const void* MaterialParamData = nullptr;
	uint32 MaterialParamDataSize = 0;

	uint32 StartIndex = 0;
	uint32 IndexCount = 0; // 0이면 전체 IndexBuffer 사용
};