#include "EnginePCH.h"
#include "Editor/Stats/StatsPanel.h"
#include <algorithm>

bool FStatsPanel::Init()
{
	return true;
}

void FStatsPanel::Tick(float DeltaTime)
{
}

void FStatsPanel::OnRender()
{
	ImGui::SetNextWindowSize(ImVec2(500, 500), ImGuiCond_FirstUseEver);
	ImGui::Begin("Stats");

	const TArray<FStatRecord>& Records = FStats::GetRecords();

	FString CurrentGroup;
	TArray<FString> DrawnGroups;
	ImGui::TextDisabled("Occlusion / Packets: active view. Frame counters: all rendered views.");
	ImGui::TextDisabled("CPU/GPU pass timings: per call. GPU samples arrive asynchronously.");
	ImGui::TextDisabled("Enable GPU timing rows to collect timestamps and capture markers.");
	ImGui::TextDisabled("GPU mode: Frustum Rejected includes occlusion; CPU-only counters are zero.");

	for (const FStatRecord& Record : Records)
	{
		if (std::find(DrawnGroups.begin(), DrawnGroups.end(), Record.Desc.Group) != DrawnGroups.end())
			continue;

		CurrentGroup = Record.Desc.Group;
		DrawnGroups.Add(CurrentGroup);

		if (!ImGui::CollapsingHeader(CurrentGroup.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			continue;
		}

		const FString TableId = "##" + CurrentGroup;

		if (ImGui::BeginTable(TableId.c_str(), 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("Name");
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
				if (ImGui::Checkbox("##enabled", &bEnabled))
					FStats::SetEnabled(static_cast<FStatId>(Index), bEnabled);
				ImGui::SameLine();
				ImGui::TextUnformatted(GroupRecord.Desc.Name.c_str());
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
	}
	ImGui::End();
}
