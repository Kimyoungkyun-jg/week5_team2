#pragma once

#include "Core/Window.h"
#include "Render/RenderDevice.h"
#include "Render/RenderPacket.h"
#include "Render/Renderer.h"
#include "Render/Swapchain.h"
#include "Render/Texture2D.h"

#include "Core/Application.h"

class FObjViewerApp : public FApplication
{
public:
	bool Init(HINSTANCE hInstance) override;
	void Run() override;
	void Shutdown() override;

private:
	// Frame
	void HandleResize();
	void HandleShortcuts();
	void UpdateCamera();
	void RenderFrame();

	// Mesh
	bool LoadMesh(const FString& Path);
	void UpdateWindowTitle();

	// Render
	void BuildRenderQueue(TQueue<FRenderPacket>& OutQueue) const;
	void CreateDepthBuffer(uint32 Width, uint32 Height);

	// Camera
	void FitCameraToMesh();
	FVector GetCameraEye() const;

	// 선언 역순으로 해제되므로 의존 대상(Device)을 가장 위에 둔다.
	TUniquePtr<FRenderDevice> RenderDevice;
	TUniquePtr<FRenderer> Renderer;
	TUniquePtr<FWindow> MainWindow;
	TUniquePtr<FSwapchain> Swapchain;
	TUniquePtr<FTexture2D> DepthBuffer;

	UStaticMesh* Mesh = nullptr;
	FString CurrentMeshPath;

	// 오빗 카메라: Target을 중심으로 Yaw·Pitch(도) 방향, Distance만큼 떨어진 위치
	FVector CameraTarget = FVector(0.0f, 0.0f, 0.0f);
	float CameraYaw = 0.0f;
	float CameraPitch = 0.0f;
	float CameraDistance = 5.0f;

	bool bIsRunning = false;
};
