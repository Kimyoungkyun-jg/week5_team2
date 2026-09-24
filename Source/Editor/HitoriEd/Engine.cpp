#include "EnginePCH.h"

#include "Editor/HitoriEd/Engine.h"

#include "Core/EngineStatics.h"
#include "Core/EngineTimer.h"
#include "Core/StatOverlay.h"
#include "Input/InputSystem.h"

#include "ObjectSystem/ObjectFactory.h"

#include "Render/GeometryGenerator.h"

#include "Engine/World.h"
#include "Engine/Level.h"

#include "Render/Renderer.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Actor/LightActor.h"

#include "Asset/AssetManager.h"
#include "Render/RenderResourceManager.h"

#include "Render/RenderCommand.h"
#include "Editor/Outliner/OutlinerPanel.h"
#include "Editor/HitoriEd/EditorFileUtils.h"
#include "UObject/UObjectIterator.h"

#include "Core/EngineLog.h"
#include "Serialization/JsonArchive.h"
#include "Serialization/DefaultSceneLoader.h"

// 렌더 자원·월드·에디터와 MultipleViewports 연결을 초기화한다.
bool FEditorApplication::Init(HINSTANCE hInstance)
{
	EditorUI = MakeUnique<FEditorUI>();
	EditorUI->Init();

	EditorUI->SetNewSceneCallback([this]() { CreateNewScene(); });
	EditorUI->SetOpenSceneCallback([this]() { OpenScene(); });
	EditorUI->SetSaveSceneCallback([this]() { SaveCurrentScene(); });
	EditorUI->SetSaveSceneAsCallback([this]() { SaveSceneAs(); });

	//OutputLogPanel = EditorUI->AddEditorPanel<FOutputLogPanel>();
	FLog::AddSink(OutputLogPanel);
	LOG(Info, "Engine Initialize...");

	LOG(Info, "Initialize Renderer...");
	Renderer = MakeUnique<FRenderer>();
	RenderDevice = MakeUnique<FRenderDevice>();
	RenderCommand::Init(RenderDevice.get());
	Renderer->Init();

	// Create Main Window & Swapchain
	FWindowContext MainWindowCtx;
	LOG(Info, "Create Main Window...");
	MainWindowCtx.Window = MakeUnique<FWindow>();
	if (!MainWindowCtx.Window->Create(hInstance, 1920, 1080, L"Hitori Engine"))
	{
		LOG(Error, "Failed To Create Main Window!");
		return false;
	}
	LOG(Info, "Success!");
	MainWindowCtx.Swapchain = MakeUnique<FSwapchain>(RenderDevice.get(), MainWindowCtx.Window.get());
	MainWindow = MainWindowCtx.Window.get();
	MainWindowSC = MainWindowCtx.Swapchain.get();
	Windows.Add(std::move(MainWindowCtx));


	LOG(Info, "Initialize AssetManager...");
	FRenderResourceManager::Init();
	UAssetManager::Get().Init();
	LOG(Info, "Initialize AssetManager Success!");


	LOG(Info, "Initialize ImGui...");
	ImGuiRenderer = MakeUnique<FImGuiRenderer>();
	if (!ImGuiRenderer->Init(MainWindow->GetHandle(), RenderDevice->GetDevice(), RenderDevice->GetContext()))
	{
		LOG(Error, "Failed To Initialize ImGui!");
	}
	LOG(Info, "Initialize ImGui Success!");

	GridRenderer = MakeUnique<FGridRenderer>();
	GridRenderer->Init(Renderer.get());

	GizmoRenderer = MakeUnique<FGizmoRenderer>();
	GizmoRenderer->Init(Renderer.get());

	Gizmo = MakeUnique<FGizmo>();

	// 필요한 Panel들 추가후 raw pointer 반환(소유권 = EditorUI)
	DetailsPanel = EditorUI->AddEditorPanel<FDetailsPanel>();
	EditorControlsPanel = EditorUI->AddEditorPanel<FEditorControlsPanel>();
	ViewportsPanel = EditorUI->AddEditorPanel<FViewportsPanel>();


	// OutLine
	OutlineRenderer = MakeUnique<FOutlineRenderer>();
	OutlineRenderer->Init(Renderer.get());

	Outline = MakeUnique<FOutline>();

	SystemFont = UAssetManager::GetAssetByPath<UFont>("Assets/Fonts/Pretendard.json");

	TextRenderer = MakeUnique<FTextRenderer>();
	TextRenderer->Init();

	// Scene
	World = FObjectFactory::ConstructObject<UWorld>();
	World->Init();

	// 공식 씬 파일 고속 로드
	if (!FDefaultSceneLoader::LoadScene(World, "Scenes/Default.scene"))
	{
		LOG(Warning, "Failed to load Scenes/Default.scene");
	}

	// 투영 행렬 생성 
	MultipleViewportsAdapter.InitializeFromWorld(*World);
	MultipleViewportsAdapter.SetLayoutMode(ELayoutMode::Single);
	MultipleViewportsAdapter.SetSingleViewIndex(0);

	World->GetMainCamera()->GetCameraComponent()->SetExternalInputManaged(true);


	OutlinerPanel = EditorUI->AddEditorPanel<FOutlinerPanel>();
	OutlinerPanel->SetWorld(World);
	OutlinerPanel->SetSelectionCallback(
		[this](UPrimitiveComponent* Primitive)
		{
			Gizmo->SetTarget(Primitive);
			Outline->SetTarget(Primitive);
			DetailsPanel->SetTarget(Primitive);
		}
	);

	OutlinerPanel->SetDeleteActorCallback(
		[this](AActor* Actor)
		{
			DeleteActor(Actor);
		}
	);

	LineBatcher = MakeUnique<FLineBatcher>();
	LineBatcher->Init(Renderer.get(), World);

	DetailsPanel->SetWorld(World);

	EditorControlsPanel->SetWorld(World);
	EditorControlsPanel->SetGizmo(Gizmo.get());
	EditorControlsPanel->SetViewportAdapter(&MultipleViewportsAdapter);

	ViewportsPanel->SetViewportAdapter(&MultipleViewportsAdapter);
	bIsRunning = true;
	return true;



	bIsRunning = true;

	return true;
}

// 프레임 시작·View 상태·월드 갱신·렌더·종료를 순차 반복한다.
void FEditorApplication::Run()
{
	EngineTimer::Init();

	LOG(Info, "{}", "Hello, World!");
	LOG(Info, "{}", FName().ToString());

	while (bIsRunning)
	{
		float DeltaTime = 0.0f;
		if (!BeginFrame(DeltaTime))
			break;

		UpdateMultipleViewportState(DeltaTime);
		TickWorldAndEditor(DeltaTime);
		RenderMultipleViewports();
		EndFrame();
	}
}

// 창 이벤트·입력을 갱신하고 DeltaTime을 계산한다.
bool FEditorApplication::BeginFrame(float& OutDeltaTime)
{

	EngineTimer::Tick();
	OutDeltaTime = EngineTimer::GetDeltaTime();
	FStatOverlay::Tick(OutDeltaTime);
	EditorControlsPanel->FEditorControlsPanel::DeltaTime = OutDeltaTime;
	FInputSystem::UpdateInputStates();

	MainWindow->ProcessMessage(bIsRunning);
	if (!bIsRunning)
		return false;

	if (!ImGui::GetIO().WantTextInput && FInputSystem::IsKeyPressed(EKeyCode::Delete))
		DeleteActor(OutlinerPanel->GetSelectedActor());

	HandleMainWindow();
	return true;
}

// 패널의 Layout·Preset 요청과 입력을 Adapter에 반영한다.
void FEditorApplication::UpdateMultipleViewportState(const float DeltaTime)
{
	const FVector2 ViewportSize = ViewportsPanel->GetContentSize();
	const FVector2 LocalMousePosition = ViewportsPanel->GetLocalMousePosition();

	ELayoutMode RequestedLayout{};
	int32 RequestedSingleViewIndex = MultipleViewportsAdapter.GetSingleViewIndex();
	if (ViewportsPanel->ConsumeLayoutRequest(RequestedLayout, RequestedSingleViewIndex))
	{
		if (RequestedLayout == ELayoutMode::Single)
			MultipleViewportsAdapter.SetSingleViewIndex(RequestedSingleViewIndex);
		MultipleViewportsAdapter.SetLayoutMode(RequestedLayout);

	}

	int32 PresetViewIndex = InvalidViewIndex;
	EMultipleViewportsCameraPreset RequestedPreset = EMultipleViewportsCameraPreset::Perspective;
	if (ViewportsPanel->ConsumeCameraPresetRequest(PresetViewIndex, RequestedPreset))
		MultipleViewportsAdapter.ApplyCameraPreset(PresetViewIndex, RequestedPreset);
	MultipleViewportsAdapter.UpdateLayout(ViewportSize, LocalMousePosition);

	const float HorizontalDrag = ViewportsPanel->ConsumeHorizontalDrag();
	const float VerticalDrag = ViewportsPanel->ConsumeVerticalDrag();
	if (HorizontalDrag != 0.0f)
		MultipleViewportsAdapter.ApplySplitterDrag(EDragAxis::Horizontal, HorizontalDrag, ViewportSize);
	if (VerticalDrag != 0.0f)
		MultipleViewportsAdapter.ApplySplitterDrag(EDragAxis::Vertical, VerticalDrag, ViewportSize);
	if (HorizontalDrag != 0.0f || VerticalDrag != 0.0f)
	{
		MultipleViewportsAdapter.UpdateLayout(ViewportSize, LocalMousePosition);
		const FSplitRatio Ratio = MultipleViewportsAdapter.GetSplitRatio();
	}


	float MoveSpeed = EditorControlsPanel ? EditorControlsPanel->CameraSpeed : 20.0f;

	MultipleViewportsAdapter.UpdateInput(
		DeltaTime,
		LocalMousePosition,
		MoveSpeed,
		0.1f);
	if (ViewportsPanel->IsHovered() || MultipleViewportsAdapter.GetCapturedViewIndex() != InvalidViewIndex)
		MultipleViewportsAdapter.SetEditorViewIndex(0); // 0번 뷰로 고정
}

// 월드를 한 번 Tick·Capture한 뒤 에디터와 피킹을 갱신한다.
void FEditorApplication::TickWorldAndEditor(const float DeltaTime)
{
	// 월드 상태는 프레임마다 정확히 한 번 갱신하고 캡처한다.
	World->Tick(DeltaTime);
	EditorUI->Tick(DeltaTime);
	MultipleViewportsAdapter.CaptureWorld(*World);
	UpdateGizmoAndPicking();
}

// 공유 월드 캡처로 활성 View별 렌더 큐를 만들고 렌더한다.
void FEditorApplication::RenderMultipleViewports()
{

	const bool bActive = MultipleViewportsAdapter.IsViewActive(0);
	ViewportsPanel->SetView(0, MultipleViewportsAdapter.GetViewRect(0), bActive);

	TQueue<FRenderPacket> RenderQueue;
	MultipleViewportsAdapter.BuildRenderQueue(0, RenderQueue);

	RenderFrame(
		0,
		ViewportsPanel->GetRenderingInfo(0),
		MultipleViewportsAdapter.GetEngineViewProjection(0),
		MultipleViewportsAdapter.GetEngineCameraLocation(0),
		MultipleViewportsAdapter.GetEngineCameraForward(0),
		RenderQueue);


	EMultipleViewportsCameraPreset CameraPresets[4]{};
	//for (int32 ViewIndex = 0; ViewIndex < 4; ++ViewIndex)
	//	CameraPresets[ViewIndex] = MultipleViewportsAdapter.GetCameraPreset(ViewIndex);
	
	ViewportsPanel->SetControlState(
		MultipleViewportsAdapter.GetLayoutMode(),
		MultipleViewportsAdapter.GetSingleViewIndex(),
		CameraPresets);
}

// 화면을 표시하고 UI 변경 후 View 설정을 보관한다.
void FEditorApplication::EndFrame()
{
	PresentFrame();
	// UI 변경 후 설정을 복사해 종료 시 카메라 수명에 의존하지 않는다.
	//SettingsPanel->CaptureViewportSettings();
}

// 입력 View의 Ray와 피킹으로 Gizmo·공유 선택을 갱신한다.
void FEditorApplication::UpdateGizmoAndPicking()
{
	// Delete는 BeginFrame에서 한 번만 처리하고 여기서는 View 입력만 다룬다.
	const int32 ViewIndex = MultipleViewportsAdapter.GetActiveViewIndex();
	if (ViewIndex == InvalidViewIndex || !ViewportsPanel->IsHovered())
		return;

	const FVector2 LocalMousePosition = ViewportsPanel->GetLocalMousePosition();
	FRay Ray{};
	if (!MultipleViewportsAdapter.TryGetActiveViewRay(LocalMousePosition, Ray))
		return;

	const FRect& Rect = MultipleViewportsAdapter.GetViewRect(ViewIndex);
	const FVector2 ViewLocalMouse(
		LocalMousePosition.X - Rect.X,
		LocalMousePosition.Y - Rect.Y);
	const FMatrix ViewProjection = MultipleViewportsAdapter.GetEngineViewProjection(ViewIndex);
	bool bMouseDown = FInputSystem::IsMouseDown(EMouseButton::Left);

	Gizmo->Update(
		Ray,
		ViewLocalMouse,
		ViewProjection,
		static_cast<int>(Rect.Width),
		static_cast<int>(Rect.Height),
		bMouseDown,
		MultipleViewportsAdapter.GetEngineCameraLocation(ViewIndex),
		MultipleViewportsAdapter.IsOrthographic(ViewIndex));

	if (FInputSystem::IsMousePressed(EMouseButton::Left) && !Gizmo->IsUsing() && Gizmo->GetHoveredAxis() < 0)
	{
		MultipleViewportsAdapter.PickActiveView(LocalMousePosition, *World);
		MultipleViewportsAdapter.ApplyLastPickToOutliner(*OutlinerPanel);
	}

}

// View 행렬로 Scene·Grid·Gizmo·텍스트·Outline을 렌더한다.
void FEditorApplication::RenderFrame(const int32 ViewIndex, const FRenderingInfo& ViewRenderingInfo, const FMatrix& ViewProjection, const FVector& ViewCameraLocation, const FVector& ViewCameraForward, TQueue<FRenderPacket>& RenderQueue)
{
	RenderCommand::BeginRenderPass(ViewRenderingInfo);

	FEditorSettings DefaultSettings; // 기본 그리드 간격 사용
	GridRenderer->OnRenderPSGrid(
		ViewProjection,
		ViewCameraLocation,
		DefaultSettings,
		ViewRenderingInfo.ViewportSetting
	);

	const bool bDrawPrimitives = true;

	// 삼각형 연결은 유지하고 View별 Fill Mode만 선택한다.
	const ERasterizerState SceneRasterizerState = MultipleViewportsAdapter.IsViewWireframe(ViewIndex)
		? ERasterizerState::Wireframe : ERasterizerState::SolidBack;

	// 렌더 루프 — 반드시 RenderAll보다 먼저
	//SkyboxRenderer->OnRender(ViewProjection, ViewCameraLocation);
	if (bDrawPrimitives)
	{

		Renderer->RenderOpaque(RenderQueue, ViewProjection);
	}

	// 스텐실 기반이라 선택 대상의 가시성이 꺼져 있어도 외곽선만 그린다.
	if (Outline->GetTarget())
	{
		OutlineRenderer->OnRender(*Outline, ViewProjection, ViewRenderingInfo.ViewportSetting);
	}

	if (Gizmo->GetTarget())
	{
		auto Target = Cast<UPrimitiveComponent>(Gizmo->GetTarget());

		FBox box = Target->CalcBounds();

		RenderCommand::ClearDepthStencil(ViewRenderingInfo.DepthSteincil.Texture);

		GizmoRenderer->OnRender(
			*Gizmo,
			ViewProjection,
			ViewCameraLocation,
			MultipleViewportsAdapter.IsOrthographic(ViewIndex));
	}

	RenderCommand::ClearDepthStencil(ViewRenderingInfo.DepthSteincil.Texture);

	// 피킹된 액터의 UUID 기본 표시
	if (Gizmo->GetTarget() && SystemFont)
	{
		if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Gizmo->GetTarget()))
		{
			if (AActor* SelectedActor = Primitive->GetOwner())
			{
				FBox Box = Primitive->CalcBounds();
				FVector UUIDLocation;
				UUIDLocation.X = (Box.Min.X + Box.Max.X) * 0.5f;
				UUIDLocation.Y = (Box.Min.Y + Box.Max.Y) * 0.5f;
				UUIDLocation.Z = Box.Max.Z + 0.5f;

				FString Text = "UUID : " + std::to_string(SelectedActor->GetUUID());

				TextRenderer->BuildTextMesh(Text, 0.5f, *SystemFont);

				const FMatrix BillboardWorld = MultipleViewportsAdapter.BuildEngineBillboardMatrix(ViewIndex, UUIDLocation, 1.0f, 1.0f);
				TextRenderer->OnRender(Text, BillboardWorld, 0.5f, *SystemFont, ViewProjection);
			}
		}
	}


	RenderCommand::EndRenderPass(ViewRenderingInfo);
}


// View Texture가 포함된 UI를 Swapchain에 합성해 표시한다.
void FEditorApplication::PresentFrame()
{
	// Swapchain 렌더링
	RenderCommand::BeginRenderPass(MainWindowSC->GetRenderingInfo());

	ImGuiRenderer->Begin();

	EditorUI->OnRender();

	ImGuiRenderer->End();

	RenderCommand::EndRenderPass(MainWindowSC->GetRenderingInfo());

	MainWindowSC->SwapBuffers();

}

// 엔진 종료에 필요한 자원 정리를 수행한다.
void FEditorApplication::Shutdown()
{
	UAssetManager::Get().Shutdown();
	FRenderResourceManager::Shutdown();

	while (GUObjectArray.Num() > 0)
	{
		delete GUObjectArray.Last();
	}

	ImGuiRenderer->Shutdown();
	RenderDevice->Shutdown();
}

// 메인 창 크기에 맞춰 Swapchain을 갱신한다.
void FEditorApplication::HandleMainWindow()
{
	if (MainWindow->CheckResized())
	{
		MainWindowSC->Resize(MainWindow->GetWidth(), MainWindow->GetHeight());
	}
}

// 선택과 Gizmo 참조를 정리한 뒤 Actor를 삭제한다.
void FEditorApplication::DeleteActor(AActor* Actor)
{
	if (!Actor)
		return;

	OutlinerPanel->SelectActor(nullptr);

	Actor->Destroy();
}

// 씬 변경으로 무효화된 에디터의 선택 참조를 모두 해제한다.
void FEditorApplication::ResetSceneSelection()
{
	Gizmo->SetTarget(nullptr);
	Outline->SetTarget(nullptr);
	DetailsPanel->SetTarget(nullptr);
	OutlinerPanel->SelectActor(nullptr);
}

// 새 씬 생성이 성공하면 에디터 선택 상태를 초기화한다.
void FEditorApplication::CreateNewScene()
{
	if (!FEditorFileUtils::NewScene(World))
		return;

	ResetSceneSelection();
}

// 씬 불러오기가 성공하면 에디터 선택 상태를 초기화한다.
void FEditorApplication::OpenScene()
{
	if (!FEditorFileUtils::LoadScene(World))
		return;

	ResetSceneSelection();
}

// 공통 파일 유틸리티로 현재 씬을 저장한다.
void FEditorApplication::SaveCurrentScene()
{
	FEditorFileUtils::SaveScene(World);
}

// 공통 파일 유틸리티로 새 경로에 씬을 저장한다.
void FEditorApplication::SaveSceneAs()
{
	FEditorFileUtils::SaveSceneAs(World);
}
