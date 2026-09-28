#pragma once

#include "RenderPacket.h"
#include "Texture2D.h"
#include "Text/Font.h"


#include "RenderingInfo.h"

struct FPerObjectConstants
{
	FMatrix MVP;
	FMatrix World;
};

class UCameraComponent;

class FRenderer
{
	friend class UTexture2D;
public:

	bool Init();

	// 기존 단일 카메라의 ViewProjection으로 렌더 큐 전체를 그린다.
	void RenderAll(TQueue<FRenderPacket>& InQueue, UCameraComponent* CameraComponent);

	// Adapter가 계산한 ViewProjection을 직접 받아 View별 렌더 큐를 그린다.
	void RenderAll(TQueue<FRenderPacket>& InQueue, const FMatrix& ViewProjection);

	// 큐를 정렬해 불투명 패킷만 그린다. 반투명은 RenderTranslucent 호출 전까지 보관한다.
	void RenderOpaque(TQueue<FRenderPacket>& InQueue, const FMatrix& ViewProjection);

	// RenderOpaque가 보관한 반투명 패킷을 먼 것부터 그린다.
	void RenderTranslucent(const FMatrix& ViewProjection);

private:
	// FIFO 소비용 배열의 용량만 재사용하며 매 View의 패킷 값은 새로 채운다.
	TArray<FRenderPacket> RenderPackets;
	// 정렬된 RenderPackets에서 반투명 패킷이 시작되는 위치
	uint32 FirstTranslucentIndex = 0;
	TUniquePtr<FConstantBuffer> CB;
	TUniquePtr<FConstantBuffer> Temp;

	D3D11_VIEWPORT ViewportInfo;

	uint32 Width;
	uint32 Height;

	FLOAT ClearColor[4] = { 0.3f, 0.3f, 0.3f, 1.0f };

	void DrawPackets(uint32 Begin, uint32 End, const FMatrix& ViewProjection);
	void BindMaterial(UMaterial* material);
	void UpdateMaterialParams(const FRenderPacket& RenderPacket);
	void UpdatePerObjectConstants(const FRenderPacket& RenderPacket, const FMatrix& ViewProjection);
};
