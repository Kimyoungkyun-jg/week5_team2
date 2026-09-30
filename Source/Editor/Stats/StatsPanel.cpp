#include "EnginePCH.h"
#include "Editor/Stats/StatsPanel.h"
#include <algorithm>
#include <vector>
#include <format>
#include "Core/StatDefinitions.h"
#include "Rendering/GPUProfiler.h"

namespace
{
	void DrawNote(const char* Text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.78f, 0.80f, 1.0f));
		ImGui::TextWrapped("%s", Text);
		ImGui::PopStyleColor();
	}

	struct FCostRow
	{
		FStatId Id;
		const char* Hint;
	};

	void DrawCostBreakdown(const char* Title, std::vector<FCostRow> Rows)
	{
		ImGui::Dummy(ImVec2(0.0f, 10.0f));
		ImGui::SeparatorText(Title);
		ImGui::Dummy(ImVec2(0.0f, 6.0f));
		const auto HasRecentSample = [](FStatId Id)
		{
			const FStatRecord& R = FStats::GetRecord(Id);
			return R.bEnabled && R.SampleCount > 0 && FStats::GetFrameNumber() - R.LastSampleFrame <= 2;
		};
		std::sort(Rows.begin(), Rows.end(), [&](const FCostRow& A, const FCostRow& B)
		{
			const double Left = HasRecentSample(A.Id) ? FStats::GetRecord(A.Id).CurrentValue : -1.0;
			const double Right = HasRecentSample(B.Id) ? FStats::GetRecord(B.Id).CurrentValue : -1.0;
			return Left > Right;
		});
		const double Largest = HasRecentSample(Rows.front().Id) ? FStats::GetRecord(Rows.front().Id).CurrentValue : 0.0;
		const auto RecentCount = std::count_if(Rows.begin(), Rows.end(), [&](const FCostRow& Row)
		{
			return HasRecentSample(Row.Id);
		});
		if (Largest > 0.0)
		{
			ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered), "BOTTLENECK CANDIDATE (Last)");
			ImGui::TextWrapped("%s  |  %.3f ms", FStats::GetRecord(Rows.front().Id).Desc.Name.c_str(), Largest);
			DrawNote(Rows.front().Hint);
		}
		else
			DrawNote(RecentCount > 0 ? "Measured cost: 0 ms" : "No recent samples");
		if (static_cast<size_t>(RecentCount) < Rows.size())
			ImGui::TextDisabled("Measured stages: %d / %d", static_cast<int>(RecentCount), static_cast<int>(Rows.size()));
		ImGui::Dummy(ImVec2(0.0f, 4.0f));
		ImGui::PushID(Title);
		if (!ImGui::TreeNode("Stage details"))
		{
			ImGui::PopID();
			return;
		}
		ImGui::Dummy(ImVec2(0.0f, 8.0f));
		for (const FCostRow& Row : Rows)
		{
			const FStatRecord& R = FStats::GetRecord(Row.Id);
			ImGui::TextUnformatted(R.Desc.Name.c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", Row.Hint);
			if (!HasRecentSample(Row.Id))
			{
				ImGui::TextDisabled(R.bEnabled ? "No recent sample" : "Off");
				ImGui::Dummy(ImVec2(0.0f, 5.0f));
				continue;
			}
			const FString Label = std::format("{:.3f} ms", R.CurrentValue);
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::GetStyleColorVec4(
				Row.Id == Rows.front().Id ? ImGuiCol_ButtonHovered : ImGuiCol_Button));
			ImGui::ProgressBar(Largest > 0.0 ? static_cast<float>(R.CurrentValue / Largest) : 0.0f,
				ImVec2(-1.0f, 0.0f), Label.c_str());
			ImGui::PopStyleColor();
			ImGui::Dummy(ImVec2(0.0f, 7.0f));
		}
		ImGui::TreePop();
		ImGui::PopID();
	}
}

bool FStatsPanel::Init()
{
	return true;
}

void FStatsPanel::Tick(float DeltaTime)
{
}

void FStatsPanel::OnRender()
{
	ImGui::SetNextWindowSize(ImVec2(700, 650), ImGuiCond_FirstUseEver);
	ImGui::Begin("Stats");
	ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered), "PERFORMANCE OVERVIEW");
	ImGui::Dummy(ImVec2(0.0f, 8.0f));
	if (ImGui::Button("Enable GPU breakdown"))
	{
		for (const FStatId Id : {StatIds::GpuFrame(), StatIds::GpuOpaque(), StatIds::GpuHZB(),
			StatIds::GpuCull(), StatIds::GpuGrid(), StatIds::GpuEditor()})
			FStats::SetEnabled(Id, true);
	}
	ImGui::SameLine();
	if (ImGui::Button("Reset samples"))
	{
		// UI runs after EndFrame: discard outstanding GPU samples from the old capture.
		FGPUProfiler::Get().Shutdown();
		FStats::ResetSamples();
	}
	DrawCostBreakdown("CPU scene", {
		{StatIds::CaptureWorld(), "Next: object count / repeated scene collection"},
		{StatIds::OcclusionCullTime(), "Next: culling mode / Result Map Wait"},
		{StatIds::PacketBuild(), "Next: visible packets / matrix construction"},
		{StatIds::RenderSubmit(), "Next: Opaque CPU breakdown below"}
	});
	DrawCostBreakdown("Opaque CPU", {
		{StatIds::RenderSort(), "Next: packet count / repeated sorting"},
		{StatIds::RenderMaterials(), "Next: material count / repeated updates"},
		{StatIds::RenderUpload(), "Next: CB written bytes / Map calls"},
		{StatIds::RenderDrawLoop(), "Next: draw calls / repeated binding"},
		{StatIds::RenderWorkers(), "Next: worker load balance / recording cost"},
		{StatIds::RenderExecute(), "Next: command list count / submission cost"}
	});
	DrawCostBreakdown("GPU passes", {
		{StatIds::GpuOpaque(), "Next: resolution / LOD comparison"},
		{StatIds::GpuHZB(), "Next: GPU culling on/off comparison"},
		{StatIds::GpuCull(), "Next: tested objects / GPU culling on/off"},
		{StatIds::GpuGrid(), "Next: grid on/off comparison"},
		{StatIds::GpuEditor(), "Next: bounds / outline / gizmo on/off"}
	});

	const TArray<FStatRecord>& Records = FStats::GetRecords();

	FString CurrentGroup;
	TArray<FString> DrawnGroups;
	ImGui::Dummy(ImVec2(0.0f, 12.0f));
	ImGui::SeparatorText("Detailed statistics");
	ImGui::Dummy(ImVec2(0.0f, 8.0f));

	for (const FStatRecord& Record : Records)
	{
		if (std::find(DrawnGroups.begin(), DrawnGroups.end(), Record.Desc.Group) != DrawnGroups.end())
			continue;

		CurrentGroup = Record.Desc.Group;
		DrawnGroups.Add(CurrentGroup);
		ImGui::Dummy(ImVec2(0.0f, 6.0f));

		if (!ImGui::CollapsingHeader(CurrentGroup.c_str()))
		{
			continue;
		}

		const FString TableId = "##" + CurrentGroup;

		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 6.0f));
		if (ImGui::BeginTable(TableId.c_str(), 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.8f);
			ImGui::TableSetupColumn("Current / Last");
			ImGui::TableSetupColumn("Avg");
			ImGui::TableSetupColumn("Max");
			ImGui::TableSetupColumn("Count");
			ImGui::TableHeadersRow();

			for (int32 Index = 0; Index < Records.Num(); ++Index)
			{
				const FStatRecord& GroupRecord = Records[Index];
				if (GroupRecord.Desc.Group != CurrentGroup)
					continue;

				const double Average = GroupRecord.SampleCount > 0 ? GroupRecord.TotalValue / static_cast<double>(GroupRecord.SampleCount) : 0.0;

				ImGui::TableNextRow();

				ImGui::TableSetColumnIndex(0);
				ImGui::PushID(Index);
				bool bEnabled = GroupRecord.bEnabled;
				// Match the existing pink button theme without changing other panels.
				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetStyleColorVec4(
					bEnabled ? ImGuiCol_Button : ImGuiCol_FrameBg));
				ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
				ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
				if (ImGui::Checkbox("##enabled", &bEnabled))
					FStats::SetEnabled(static_cast<FStatId>(Index), bEnabled);
				ImGui::PopStyleColor(4);
				ImGui::SameLine();
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextUnformatted(GroupRecord.Desc.Name.c_str());
				ImGui::PopTextWrapPos();
				ImGui::PopID();
				if (!bEnabled)
				{
					ImGui::TableSetColumnIndex(1);
					ImGui::TextDisabled("Off");
					continue;
				}

				ImGui::TableSetColumnIndex(1);

				switch (GroupRecord.Desc.Unit)
				{
				case EStatUnit::Milliseconds:
					ImGui::Text("%.3f ms", GroupRecord.CurrentValue);
					break;

				case EStatUnit::Count:
					ImGui::Text("%.0f", GroupRecord.CurrentValue);
					break;

				case EStatUnit::Bytes:
					ImGui::Text("%.0f B", GroupRecord.CurrentValue);
					break;
				}

				if (GroupRecord.Desc.Mode == EStatMode::Event)
				{
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%.3f", Average);

					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%.3f", GroupRecord.MaxValue);

					ImGui::TableSetColumnIndex(4);
					ImGui::Text("%llu", static_cast<unsigned long long>(GroupRecord.SampleCount));
				}
				else
				{
					for (int Column = 2; Column <= 4; ++Column)
					{
						ImGui::TableSetColumnIndex(Column);
						ImGui::TextDisabled("-");
					}
				}
			}

			ImGui::EndTable();
		}
		ImGui::PopStyleVar();
	}
	ImGui::End();
}
