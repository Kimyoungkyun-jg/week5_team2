#pragma once

#include "Stats.h"

namespace StatIds
{
inline FStatId PickingTotal()
{
	static const FStatId Id = FStats::Register({"Picking", "Total", EStatUnit::Milliseconds, EStatMode::Event});
	return Id;
}

inline FStatId PickingBroad()
{
	static const FStatId Id = FStats::Register({"Picking", "Broad", EStatUnit::Milliseconds, EStatMode::Event});
	return Id;
}

inline FStatId PickingNarrow()
{
	static const FStatId Id = FStats::Register({"Picking", "Narrow", EStatUnit::Milliseconds, EStatMode::Event});
	return Id;
}
} // namespace StatIds