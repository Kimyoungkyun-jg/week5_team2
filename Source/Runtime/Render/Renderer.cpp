#include "EnginePCH.h"
#include "Renderer.h"
#include "Shader.h"
#include "Mesh.h"

#include "Core/EngineTimer.h"

#include "RenderCommand.h"

#include "Camera/CameraComponent.h"

#include <algorithm>

bool FRenderer::Init()
{
	Temp = RenderCommand::CreateConstantBuffer(sizeof(FPerObjectConstants));

	return true;
}

// 카메라의 ViewProjection을 공통 렌더 경로로 전달한다.
void FRenderer::RenderAll(TQueue<FRenderPacket>& InQueue, UCameraComponent* CameraComponent)
{
	RenderAll(InQueue, CameraComponent->GetViewProjectionMatrix());
}

// 불투명 우선·반투명 거리순으로 정렬해 View 행렬과 Section 범위로 그린다.
void FRenderer::RenderAll(TQueue<FRenderPacket>& InQueue, const FMatrix& ViewProjection)
{
	RenderOpaque(InQueue, ViewProjection);
	RenderTranslucent(ViewProjection);
}

// 불투명 메시를 큐에서 직접 꺼내 즉시 렌더링
void FRenderer::RenderOpaque(TQueue<FRenderPacket>& InQueue, const FMatrix& ViewProjection)
{

	EPSOType LastPSO = static_cast<EPSOType>(255);
	while (InQueue.IsEmpty() == false)
	{
		const FRenderPacket& RenderPacket = InQueue.Peek();
		if (RenderPacket.mesh != nullptr && RenderPacket.material != nullptr)
		{
			RenderCommand::BindMesh(RenderPacket.mesh);
			// 1. 매 프레임 첫 번째 사과: (255 != 0) 이므로 무조건 D3D11에 1회 바인딩!
			// 2. 2번째 ~ 50,000번째 사과: (0 == 0) 이므로 49,999번은 완벽 스킵!
			if (LastPSO != RenderPacket.material->PSOType)
			{
				LastPSO = RenderPacket.material->PSOType;
				RenderCommand::BindPipelineState(FRenderResourceManager::GetPSO(LastPSO));
			}
			// 텍스처와 CBuffer 바인딩
			for (int i = 0; i < RenderPacket.material->Textures.size(); i++)
			{
				RenderCommand::BindShaderResource(i, RenderPacket.material->Textures[i], EShaderBindFlagBits::Pixel);
			}
			RenderCommand::BindSamplerState(0, RenderPacket.material->SamplerState, EShaderBindFlagBits::Pixel);
			UpdateMaterialParams(RenderPacket);
			UpdatePerObjectConstants(RenderPacket, ViewProjection);
			RenderCommand::DrawIndexed(
				RenderPacket.IndexCount ? RenderPacket.IndexCount : RenderPacket.mesh->IndexBuffer->GetIndexCount(),
				RenderPacket.StartIndex
			);
		}
		InQueue.Dequeue();
	}
}

// 반투명 객체가 없으므로 아무 작업도 수행하지 않음
void FRenderer::RenderTranslucent(const FMatrix& ViewProjection)
{
}

// 기존 인터페이스 호환용 함수
void FRenderer::DrawPackets(uint32 Begin, uint32 End, const FMatrix& ViewProjection)
{
}

// Material마다 Shader/Texture/Sampler/State 꽂기
void FRenderer::BindMaterial(UMaterial* material)
{

	RenderCommand::BindPipelineState(FRenderResourceManager::GetPSO(material->PSOType));
	// 머티리얼이 가진 텍스처 및 샘플러 바인딩
	for (int i = 0; i < material->Textures.size(); i++)
	{
		RenderCommand::BindShaderResource(i, material->Textures[i], EShaderBindFlagBits::Pixel);
	}
	RenderCommand::BindSamplerState(0, material->SamplerState, EShaderBindFlagBits::Pixel);
}

// b1 내용 채우고 꽂기
void FRenderer::UpdateMaterialParams(const FRenderPacket& RenderPacket)
{
	switch (RenderPacket.material->PSOType)
	{
	case EPSOType::StaticMesh_Opaque:
	case EPSOType::StaticMesh_Wireframe:
	{
		const float TotalTime = EngineTimer::GetTotalTime();
		FStaticMeshMaterialParams Params{};
		Params.BaseColor = RenderPacket.material->BaseColor;
		Params.UVOffset = RenderPacket.material->UVScrollSpeed * TotalTime;
		Params.bOpaque = 1.0f; // 오팩이므로 무조건 1.0f
		RenderCommand::UpdateBufferData(RenderPacket.material->ParamBuffer.get(), &Params, sizeof(FStaticMeshMaterialParams));
		RenderCommand::BindConstantBuffer(1, RenderPacket.material->ParamBuffer.get(), EShaderBindFlagBits::Pixel);
		break;
	}
	case EPSOType::StaticMesh_Translucent:
	{
		const float TotalTime = EngineTimer::GetTotalTime();
		FStaticMeshMaterialParams Params{};
		Params.BaseColor = RenderPacket.material->BaseColor;
		Params.UVOffset = RenderPacket.material->UVScrollSpeed * TotalTime;
		Params.bOpaque = 0.0f; // 반투명이므로 0.0f
		RenderCommand::UpdateBufferData(RenderPacket.material->ParamBuffer.get(), &Params, sizeof(FStaticMeshMaterialParams));
		RenderCommand::BindConstantBuffer(1, RenderPacket.material->ParamBuffer.get(), EShaderBindFlagBits::Pixel);
		break;
	}
	case EPSOType::Particle_AlphaBlend:
	case EPSOType::Particle_Additive:
	{
		if (RenderPacket.material->ParamBuffer && RenderPacket.MaterialParamData != nullptr)
		{
			RenderCommand::UpdateBufferData(RenderPacket.material->ParamBuffer.get(), RenderPacket.MaterialParamData, RenderPacket.MaterialParamDataSize);
			RenderCommand::BindConstantBuffer(1, RenderPacket.material->ParamBuffer.get(), EShaderBindFlagBits::Pixel);
		}
		break;
	}
	default:
		break;
	}
}

// b0 MVP 채우고 꽂기
void FRenderer::UpdatePerObjectConstants(const FRenderPacket& RenderPacket, const FMatrix& ViewProjection)
{
	// rp.Transform 과 Camera VP 행렬 곱
	// 행렬곱의 결과 (MVP Matrix) Constant Buffer 업데이트 필요

	FPerObjectConstants Constants;

	Constants.MVP = (RenderPacket.model * ViewProjection).GetTransposed();
	Constants.World = RenderPacket.model.GetTransposed();

	RenderCommand::UpdateBufferData(Temp.get(), &Constants);
	RenderCommand::BindConstantBuffer(0, Temp.get(), EShaderBindFlagBits::Vertex);
}
