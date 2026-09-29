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

static bool CompareRenderPackets(const FRenderPacket& A, const FRenderPacket& B)
{
	const EPSOType APSO = A.material ? A.material->PSOType : EPSOType::Count;
	const EPSOType BPSO = B.material ? B.material->PSOType : EPSOType::Count;
	if (APSO != BPSO)
		return static_cast<uint8>(APSO) < static_cast<uint8>(BPSO);

	if (A.material != B.material)
		return std::less<>{}(A.material, B.material);

	if (A.mesh != B.mesh)
		return std::less<>{}(A.mesh, B.mesh);

	const bool bOpaque = APSO == EPSOType::StaticMesh_Opaque || APSO == EPSOType::StaticMesh_Wireframe;
	return bOpaque
		? A.CameraDistanceSquared < B.CameraDistanceSquared
		: A.CameraDistanceSquared > B.CameraDistanceSquared;
}

bool FRenderer::Init()
{
	Temp = RenderCommand::CreateConstantBuffer(PerObjectSlotSize * MaxObjects);
	return Temp != nullptr;
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

	const uint32 MaxPacketsPerWorker = (MaxObjects + WorkerCount - 1) / WorkerCount;
	const uint32 WorkerBufferSize = MaxPacketsPerWorker * PerObjectSlotSize;

	for (uint32 Index = 0; Index < WorkerCount; ++Index)
	{
		DeferredWorkers[Index].Context = RenderCommand::CreateDeferredContext();

		assert(DeferredWorkers[Index].Context);

		HRESULT Hr = DeferredWorkers[Index].Context.As(&DeferredWorkers[Index].Context1);

		assert(SUCCEEDED(Hr));
		assert(DeferredWorkers[Index].Context1);

		DeferredWorkers[Index].PerObjectCB = RenderCommand::CreateConstantBuffer(WorkerBufferSize);

		assert(DeferredWorkers[Index].PerObjectCB);
		assert(DeferredWorkers[Index].PerObjectCB->GetBuffer());
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

	sort(InPackets.begin(), InPackets.end(), CompareRenderPackets);

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
	assert(TotalPackets <= MaxObjects);

	if (TotalPackets <= 500 || NumWorkers <= 1)
	{
		const UStaticMesh* LastMesh = nullptr;
		const UMaterial* LastMaterial = nullptr;

		void* MappedData = RenderCommand::MapBufferWriteDiscard(Temp.get());
		assert(MappedData);

		uint8* Base = static_cast<uint8*>(MappedData);

		for (int32 i = 0; i < TotalPackets; ++i)
		{
			FPerObjectConstants Constants;
			Constants.MVP = InPackets[i].MVP;
			Constants.World = InPackets[i].model;

			uint8* Dest = Base + i * PerObjectSlotSize;
			std::memcpy(Dest, &Constants, sizeof(FPerObjectConstants));
		}

		RenderCommand::UnmapBuffer(Temp.get());

		for (int32 i = 0; i < TotalPackets; ++i)
		{
			const FRenderPacket& RenderPacket = InPackets[i];

			if (RenderPacket.mesh == nullptr || RenderPacket.material == nullptr)
			{
				continue;
			}

			if (LastMesh != RenderPacket.mesh)
			{
				RenderCommand::BindMesh(RenderPacket.mesh);
				LastMesh = RenderPacket.mesh;
			}

			if (LastMaterial != RenderPacket.material)
			{
				BindMaterial(RenderPacket.material);
				LastMaterial = RenderPacket.material;
			}

			const uint32 FirstConstant = i * (PerObjectSlotSize / 16);
			const uint32 NumConstants = PerObjectSlotSize / 16;

			RenderCommand::BindConstantBufferRange(0, Temp.get(), EShaderBindFlagBits::Vertex, FirstConstant, NumConstants);
			RenderCommand::DrawIndexed(RenderPacket.IndexCount ? RenderPacket.IndexCount : RenderPacket.mesh->IndexBuffer->GetIndexCount(), RenderPacket.StartIndex);
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
		ID3D11DeviceContext1* Context1 = DeferredWorkers[JobIndex].Context1.Get();

		Context->OMSetRenderTargets(NumRTVs, RTVs, DSV);
		Context->RSSetViewports(NumViewports, &Viewport);

		const UStaticMesh* LastMesh  = nullptr;
		const UMaterial* LastMaterial = nullptr;

		assert(static_cast<uint32>(End - Start) * PerObjectSlotSize <= WorkerCB->GetBufferSize());

		void* MappedData = RenderCommand::MapBufferWriteDiscard(WorkerCB, Context);

		assert(MappedData);

		uint8* Base = static_cast<uint8*>(MappedData);

		for (int32 i = Start; i < End; ++i)
		{
			FPerObjectConstants Constants;
			Constants.MVP = InPackets[i].MVP;
			Constants.World = InPackets[i].model;

			const uint32 LocalIndex = i - Start;

			uint8* Dest = Base + LocalIndex * PerObjectSlotSize;

			std::memcpy(Dest, &Constants, sizeof(FPerObjectConstants));
		}

		RenderCommand::UnmapBuffer(WorkerCB, Context);

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
					BindMaterial(RenderPacket.material, Context);
					LastMaterial = RenderPacket.material;
				}

				const uint32 LocalIndex = i - Start;
				const uint32 FirstConstant = LocalIndex * (PerObjectSlotSize / 16);
				const uint32 NumConstants = PerObjectSlotSize / 16;

				RenderCommand::BindConstantBufferRange(0, WorkerCB, EShaderBindFlagBits::Vertex, FirstConstant, NumConstants, Context1);

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
void FRenderer::BindMaterial(UMaterial* material, ID3D11DeviceContext* Context)
{
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
