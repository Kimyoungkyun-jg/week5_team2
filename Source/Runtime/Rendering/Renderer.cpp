#include "EnginePCH.h"
#include "Renderer.h"
#include "Shader.h"
#include "Mesh.h"
#include "Material.h"

#include "RenderCommand.h"
#include "Core/EngineTimer.h"
#include "Camera/CameraComponent.h"
#include "Job/FiberJobManager.h"

#include <algorithm>
#include <functional>


namespace
{
	EPSOType GetPacketPSO(const FRenderPacket& Packet)
	{
		return Packet.material ? Packet.material->PSOType : EPSOType::Count;
	}

	bool IsOpaquePSO(const EPSOType PSO)
	{
		return PSO == EPSOType::StaticMesh_Opaque || PSO == EPSOType::StaticMesh_Wireframe;
	}

	bool CompareStateThenDepth(const FRenderPacket& A, const FRenderPacket& B)
	{
		const EPSOType APSO = GetPacketPSO(A);
		const EPSOType BPSO = GetPacketPSO(B);
		if (APSO != BPSO)
			return static_cast<uint8>(APSO) < static_cast<uint8>(BPSO);

		if (A.material != B.material)
			return std::less<>{}(A.material, B.material);

		if (A.mesh != B.mesh)
			return std::less<>{}(A.mesh, B.mesh);

		return IsOpaquePSO(APSO)
			? A.CameraDistanceSquared < B.CameraDistanceSquared
			: A.CameraDistanceSquared > B.CameraDistanceSquared;
	}

}

bool FRenderer::Init()
{
	Temp = RenderCommand::CreateConstantBuffer(sizeof(FPerObjectConstants));

	return true;
}

// 지연 워커 초기화
void FRenderer::EnsureDeferredWorkers()
{
	if (DeferredWorkers.Num() > 0)
	{
		return;
	}

	uint32 WorkerCount = FFiberJobManager::Get().GetNumWorkers();
	if (WorkerCount == 0)
	{
		WorkerCount = (std::max)(1u, std::thread::hardware_concurrency());
	}

	DeferredWorkers.SetNum(WorkerCount);
	for (uint32 Index = 0; Index < WorkerCount; ++Index)
	{
		DeferredWorkers[Index].Context = RenderCommand::CreateDeferredContext();
		DeferredWorkers[Index].PerObjectCB = RenderCommand::CreateConstantBuffer(sizeof(FPerObjectConstants));
	}
}

// 기존 단일 카메라의 ViewProjection으로 렌더 패킷 배열 전체를 그린다.
void FRenderer::RenderAll(TArray<FRenderPacket>& InPackets, UCameraComponent* CameraComponent)
{
	RenderAll(InPackets, CameraComponent->GetViewProjectionMatrix());
}

// 불투명 우선·반투명 거리순으로 배열을 정렬해 View 행렬과 Section 범위로 그린다.
void FRenderer::RenderAll(TArray<FRenderPacket>& InPackets, const FMatrix& ViewProjection)
{
	RenderOpaque(InPackets, ViewProjection);
	RenderTranslucent(ViewProjection);
}

// TArray 기반 불투명 메시 지연 컨텍스트 병렬 렌더링
void FRenderer::RenderOpaque(TArray<FRenderPacket>& InPackets, const FMatrix& ViewProjection)
{
	const int32 TotalPackets = InPackets.Num();
	if (TotalPackets == 0)
	{
		return;
	}

	EnsureDeferredWorkers();

	std::sort(InPackets.begin(), InPackets.end(), CompareStateThenDepth);

	// 머티리얼 파라미터 사전 일괄 갱신
	TArray<UMaterial*> UniqueMaterials;
	UniqueMaterials.Reserve(8);
	for (const FRenderPacket& Packet : InPackets)
	{
		if (Packet.material && std::find(UniqueMaterials.begin(), UniqueMaterials.end(), Packet.material) == UniqueMaterials.end())
		{
			UniqueMaterials.Add(Packet.material);
		}
	}
	for (UMaterial* Mat : UniqueMaterials)
	{
		UpdateMaterialParams(Mat);
	}

	const int32 NumWorkers = DeferredWorkers.Num();
	if (TotalPackets <= 500 || NumWorkers <= 1)
	{
		// 단일 스레드 직접 렌더 경로
		RenderCommand::BindConstantBuffer(0, Temp.get(), EShaderBindFlagBits::Vertex);

		const UStaticMesh* LastMesh = nullptr;
		const UMaterial* LastMaterial = nullptr;
		EPSOType LastPSO = EPSOType::Count;

		for (const FRenderPacket& RenderPacket : InPackets)
		{
			if (RenderPacket.mesh != nullptr && RenderPacket.material != nullptr)
			{
				if (LastMesh != RenderPacket.mesh)
				{
					RenderCommand::BindMesh(RenderPacket.mesh);
					LastMesh = RenderPacket.mesh;
				}

				if (LastMaterial != RenderPacket.material)
				{
					const bool bPSOChanged = LastPSO != RenderPacket.material->PSOType;
					BindMaterial(RenderPacket.material, nullptr, bPSOChanged);
					LastMaterial = RenderPacket.material;
					LastPSO = RenderPacket.material->PSOType;
				}

				FPerObjectConstants Constants;
				Constants.MVP = RenderPacket.MVP;
				Constants.World = RenderPacket.model;
				RenderCommand::UpdateBufferData(Temp.get(), &Constants);

				RenderCommand::DrawIndexed(
					RenderPacket.IndexCount ? RenderPacket.IndexCount : RenderPacket.mesh->IndexBuffer->GetIndexCount(),
					RenderPacket.StartIndex
				);
			}
		}
		return;
	}

	// 현재 바인딩된 렌더 타깃과 뷰포트 정보 획득
	ID3D11RenderTargetView* RTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = { nullptr };
	ID3D11DepthStencilView* DSV = nullptr;
	RenderCommand::GetRenderDevice()->GetContext()->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, RTVs, &DSV);

	UINT NumRTVs = 0;
	for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
	{
		if (RTVs[i])
		{
			NumRTVs = i + 1;
		}
	}

	UINT NumViewports = 1;
	D3D11_VIEWPORT Viewport{};
	RenderCommand::GetRenderDevice()->GetContext()->RSGetViewports(&NumViewports, &Viewport);

	const int32 ChunkSize = (TotalPackets + NumWorkers - 1) / NumWorkers;
	const int32 NumJobs = (TotalPackets + ChunkSize - 1) / ChunkSize;

	std::vector<ComPtr<ID3D11CommandList>> CommandLists(NumJobs);

	FFiberJobManager::Get().ParallelFor(TotalPackets, ChunkSize, [&](int32 Start, int32 End)
	{
		const int32 JobIndex = Start / ChunkSize;
		if (JobIndex >= DeferredWorkers.Num())
		{
			return;
		}

		ID3D11DeviceContext* Context = DeferredWorkers[JobIndex].Context.Get();
		FConstantBuffer* WorkerCB = DeferredWorkers[JobIndex].PerObjectCB.get();

		Context->OMSetRenderTargets(NumRTVs, RTVs, DSV);
		Context->RSSetViewports(NumViewports, &Viewport);

		RenderCommand::BindConstantBuffer(0, WorkerCB, EShaderBindFlagBits::Vertex, Context);

		const UStaticMesh* LastMesh  = nullptr;
		const UMaterial* LastMaterial = nullptr;
		EPSOType LastPSO = EPSOType::Count;

		for (int32 i = Start; i < End; ++i)
		{
			const FRenderPacket& RenderPacket = InPackets[i];
			if (RenderPacket.mesh != nullptr && RenderPacket.material != nullptr)
			{
				if (LastMesh != RenderPacket.mesh)
				{
					RenderCommand::BindMesh(RenderPacket.mesh, Context);
					LastMesh = RenderPacket.mesh;
				}

				if (LastMaterial != RenderPacket.material)
				{
					const bool bPSOChanged = LastPSO != RenderPacket.material->PSOType;
					BindMaterial(RenderPacket.material, Context, bPSOChanged);
					LastMaterial = RenderPacket.material;
					LastPSO = RenderPacket.material->PSOType;
				}

				FPerObjectConstants Constants;
				Constants.MVP = RenderPacket.MVP;
				Constants.World = RenderPacket.model;
				RenderCommand::UpdateBufferData(WorkerCB, &Constants, sizeof(FPerObjectConstants), Context);

				RenderCommand::DrawIndexed(
					RenderPacket.IndexCount ? RenderPacket.IndexCount : RenderPacket.mesh->IndexBuffer->GetIndexCount(),
					RenderPacket.StartIndex,
					0,
					Context
				);
			}
		}

		Context->FinishCommandList(FALSE, CommandLists[JobIndex].GetAddressOf());
	});

	// 메인 스레드에서 커맨드 리스트 순차 실행
	for (int32 i = 0; i < NumJobs; ++i)
	{
		if (CommandLists[i])
		{
			RenderCommand::ExecuteCommandList(CommandLists[i].Get(), false);
		}
	}

	// 실행 후 메인 즉시 컨텍스트의 렌더 타깃과 뷰포트 상태 복구
	RenderCommand::GetRenderDevice()->GetContext()->OMSetRenderTargets(NumRTVs, RTVs, DSV);
	RenderCommand::GetRenderDevice()->GetContext()->RSSetViewports(NumViewports, &Viewport);

	// 획득한 렌더 타깃 참조 해제
	for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
	{
		if (RTVs[i])
		{
			RTVs[i]->Release();
		}
	}
	if (DSV)
	{
		DSV->Release();
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

// 머티리얼 바인딩
void FRenderer::BindMaterial(UMaterial* material, ID3D11DeviceContext* Context, const bool bBindPipelineState)
{
	if (bBindPipelineState)
		RenderCommand::BindPipelineState(FRenderResourceManager::GetPSO(material->PSOType), Context);
	for (int i = 0; i < material->Textures.size(); i++)
	{
		RenderCommand::BindShaderResource(i, material->Textures[i], EShaderBindFlagBits::Pixel, Context);
	}
	RenderCommand::BindSamplerState(0, material->SamplerState, EShaderBindFlagBits::Pixel, Context);
	if (material->ParamBuffer)
	{
		RenderCommand::BindConstantBuffer(1, material->ParamBuffer.get(), EShaderBindFlagBits::Pixel, Context);
	}
}

// 머티리얼 파라미터 버퍼 갱신
void FRenderer::UpdateMaterialParams(UMaterial* material)
{
	if (!material || !material->ParamBuffer)
	{
		return;
	}

	switch (material->PSOType)
	{
	case EPSOType::StaticMesh_Opaque:
	case EPSOType::StaticMesh_Wireframe:
	{
		const float TotalTime = EngineTimer::GetTotalTime();
		FStaticMeshMaterialParams Params{};
		Params.BaseColor = material->BaseColor;
		Params.UVOffset = material->UVScrollSpeed * TotalTime;
		Params.bOpaque = 1.0f;
		RenderCommand::UpdateBufferData(material->ParamBuffer.get(), &Params, sizeof(FStaticMeshMaterialParams));
		break;
	}
	case EPSOType::StaticMesh_Translucent:
	{
		const float TotalTime = EngineTimer::GetTotalTime();
		FStaticMeshMaterialParams Params{};
		Params.BaseColor = material->BaseColor;
		Params.UVOffset = material->UVScrollSpeed * TotalTime;
		Params.bOpaque = 0.0f;
		RenderCommand::UpdateBufferData(material->ParamBuffer.get(), &Params, sizeof(FStaticMeshMaterialParams));
		break;
	}
	default:
		break;
	}
}

// b0 MVP 채우고 꽂기
void FRenderer::UpdatePerObjectConstants(const FRenderPacket& RenderPacket, const FMatrixRegister& ViewProjection)
{
	FPerObjectConstants Constants;

	const FMatrixRegister Model = FMatrixRegister::Load(RenderPacket.model);
	(Model * ViewProjection).Store(Constants.MVP);   // MVP: 레지스터에서 목적지로 바로
	Model.Store(Constants.World);                   // World: 이미 올린 model 재사용

	RenderCommand::UpdateBufferData(Temp.get(), &Constants);
}
