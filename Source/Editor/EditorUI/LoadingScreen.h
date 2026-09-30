#pragma once

#include "Core/Types.h"
#include "Core/EngineString.h"
#include <d3d11.h>

class UTexture2D;

class FLoadingScreen
{
public:
	FLoadingScreen();
	~FLoadingScreen();

	bool Init();
	void Tick(float DeltaTime);
	void Draw();

	bool IsFinished() const { return bIsFinished; }
	float GetProgress() const { return CurrentProgress; }

private:
	UTexture2D* SpriteTexture = nullptr;
	float CurrentProgress = 0.0f;
	float ElapsedTime = 0.0f;
	float TotalDuration = 2.5f;
	int32 CurrentFrameIndex = 0;
	bool bIsFinished = false;
	FString CurrentStatusText;
};
