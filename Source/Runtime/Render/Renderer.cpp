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
	while (InQueue.IsEmpty() == false)
	{
		const FRenderPacket& RenderPacket = InQueue.Peek();
		if (RenderPacket.mesh != nullptr && RenderPacket.material != nullptr)
		{
			RenderCommand::BindMesh(RenderPacket.mesh);
			BindMaterial(RenderPacket.material);

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
	RenderCommand::BindShaderProgram(material->Shader);
	RenderCommand::SetBlendState(material->BlendState);
	// 반투명은 뒤에 그려지는 Grid·다른 반투명을 가리지 않도록 깊이를 쓰지 않는다.
	const bool bTranslucent = material->BlendState != EBlendState::Opaque;
	RenderCommand::SetDepthStencilState(bTranslucent && material->DepthStencilState == EDepthStencilState::Default
		? EDepthStencilState::ReadOnly : material->DepthStencilState);

	for (int i = 0; i < material->Textures.size(); i++)
	{
		RenderCommand::BindShaderResource(i, material->Textures[i], EShaderBindFlagBits::Pixel);
	}
	RenderCommand::BindSamplerState(0, material->SamplerState, EShaderBindFlagBits::Pixel);
}

// b1 내용 채우고 꽂기
void FRenderer::UpdateMaterialParams(const FRenderPacket& RenderPacket)
{
	switch (RenderPacket.material->ParamLayout)
	{
		case EMaterialParamLayout::StaticMesh:
		{
			const float TotalTime = EngineTimer::GetTotalTime();

			FStaticMeshMaterialParams Params{};
			Params.BaseColor = RenderPacket.material->BaseColor;
			Params.UVOffset = RenderPacket.material->UVScrollSpeed * TotalTime;
			Params.bOpaque = RenderPacket.material->BlendState == EBlendState::Opaque ? 1.0f : 0.0f;

			RenderCommand::UpdateBufferData(RenderPacket.material->ParamBuffer.get(), &Params, sizeof(FStaticMeshMaterialParams));
			RenderCommand::BindConstantBuffer(1, RenderPacket.material->ParamBuffer.get(), EShaderBindFlagBits::Pixel);
			break;
		}
		case EMaterialParamLayout::ParticleSubUV:
		{
			if (RenderPacket.material->ParamBuffer && RenderPacket.MaterialParamData != nullptr)
			{
				RenderCommand::UpdateBufferData(RenderPacket.material->ParamBuffer.get(), RenderPacket.MaterialParamData, RenderPacket.MaterialParamDataSize);
				RenderCommand::BindConstantBuffer(1, RenderPacket.material->ParamBuffer.get(), EShaderBindFlagBits::Pixel);
			}
			break;
		}
		case EMaterialParamLayout::None:
		{
			break;
		}
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
