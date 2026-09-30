#pragma once

#include "Editor/EditorUI/EditorPanel.h"
#include "Core/Stats.h"

class FStatsPanel : public IEditorPanel
{
  public:
	bool Init() override;
	void Tick(float DeltaTime) override;
	void OnRender() override;
	void SetOpen(bool bOpen) override;

	const char* GetPanelName() const override
	{
		return "Stats";
	}
};
