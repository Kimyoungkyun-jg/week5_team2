#include "EnginePCH.h"
#include "Swapchain.h"

#include "Core/Window.h"

FSwapchain::FSwapchain(FRenderDevice* InRenderDevice, FWindow* InWindow)
{
	RenderDevice = InRenderDevice;

	DXGI_SAMPLE_DESC SampleDesc{};
	SampleDesc.Count = 1;
	SampleDesc.Quality = 0;

	DXGI_MODE_DESC BufferDesc{};
	BufferDesc.Width = InWindow->GetWidth();
	BufferDesc.Height = InWindow->GetHeight();
	BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

	Desc.BufferDesc = BufferDesc;
	Desc.SampleDesc = SampleDesc;
	Desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	Desc.BufferCount = 2;
	Desc.OutputWindow = InWindow->GetHandle();
	Desc.Windowed = true;
	Desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	Desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	HRESULT hr = RenderDevice->GetFactory()->CreateSwapChain(RenderDevice->GetDevice(), &Desc, Swapchain.GetAddressOf());
	if (FAILED(hr))
		LOG(Error, "Failed To Create Swapchain!");

	CreateBackbuffer();


	ValidateRenderingInfo();
}

void FSwapchain::CreateBackbuffer()
{
	ComPtr<ID3D11Texture2D> Backbuffer;
	Swapchain->GetBuffer(
		0,
		IID_PPV_ARGS(&Backbuffer)
	);

	D3D11_TEXTURE2D_DESC TextureDesc;
	Backbuffer->GetDesc(&TextureDesc);

	BackbufferTexture = MakeUnique<FTexture2D>(RenderDevice->GetDevice(), Backbuffer, TextureDesc);
}

FSwapchain::~FSwapchain()
{
}

void FSwapchain::Resize(int32 InWidth, int32 InHeight)
{
	BackbufferTexture = nullptr;

	// Swapchain 크기 변경
	Swapchain->ResizeBuffers(
		0,
		InWidth,
		InHeight,
		DXGI_FORMAT_UNKNOWN,
		0
	);

	//Update Desc
	Swapchain->GetDesc(&Desc);

	CreateBackbuffer();

	ValidateRenderingInfo();
}

void FSwapchain::SwapBuffers(uint32 SyncInterval, uint32 Flags)
{
	Swapchain->Present(1, 0);
}

void FSwapchain::ValidateRenderingInfo()
{
	RenderingInfo.ColorRenderTargets.Reset();
	RenderingInfo.ViewportSetting.Width = Desc.BufferDesc.Width;
	RenderingInfo.ViewportSetting.Height = Desc.BufferDesc.Height;

	FRenderingDesc Desc{};
	Desc.Texture = BackbufferTexture.get();

	RenderingInfo.ColorRenderTargets.Add(Desc);
}
