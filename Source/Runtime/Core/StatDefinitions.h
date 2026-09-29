#pragma once

#include "Stats.h"

namespace StatIds
{
inline FStatId PickingTotal()
{
	static const FStatId Id = FStats::Register({"Picking", "Total", EStatUnit::Milliseconds, EStatMode::Event, true});
	return Id;
}

inline FStatId PickingBroad()
{
	static const FStatId Id = FStats::Register({"Picking", "Broad", EStatUnit::Milliseconds, EStatMode::Event, true});
	return Id;
}

inline FStatId PickingNarrow()
{
	static const FStatId Id = FStats::Register({"Picking", "Narrow", EStatUnit::Milliseconds, EStatMode::Event, true});
	return Id;
}

inline FStatId OcclusionCaptured()
{
	static const FStatId Id = FStats::Register({"Occlusion", "Captured", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId OcclusionStatic()
{
	static const FStatId Id = FStats::Register({"Occlusion", "Static", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId OcclusionDynamic()
{
	static const FStatId Id = FStats::Register({"Occlusion", "Dynamic", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId OcclusionFrustumRejected()
{
	static const FStatId Id = FStats::Register({"Occlusion", "Frustum Rejected", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId OcclusionRejected()
{
	static const FStatId Id = FStats::Register({"Occlusion", "Occlusion Rejected", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId OcclusionVisible()
{
	static const FStatId Id = FStats::Register({"Occlusion", "Visible", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId RenderPackets()
{
	static const FStatId Id = FStats::Register({"Render", "Packets (Active View)", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId OcclusionOccluders()
{
	static const FStatId Id = FStats::Register({"Occlusion", "Occluders", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId OcclusionSourceTriangles()
{
	static const FStatId Id = FStats::Register({"Occlusion", "Source Triangles", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}


inline FStatId OcclusionBVHTested()
{
	static const FStatId Id = FStats::Register({"Occlusion", "BVH Tested", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId OcclusionBVHPruned()
{
	static const FStatId Id = FStats::Register({"Occlusion", "BVH Pruned", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId OcclusionCullTime()
{
	static const FStatId Id = FStats::Register({"Occlusion", "Cull", EStatUnit::Milliseconds, EStatMode::Event, true});
	return Id;
}

inline FStatId OcclusionBVHBuildTime()
{
	static const FStatId Id = FStats::Register({"Occlusion", "Last BVH Build", EStatUnit::Milliseconds, EStatMode::Gauge, true});
	return Id;
}
inline FStatId FrameFPS()
{
	static const FStatId Id = FStats::Register({"Frame", "FPS", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId FrameTime()
{
	static const FStatId Id = FStats::Register({"Frame", "Frame Time", EStatUnit::Milliseconds, EStatMode::Gauge, true});
	return Id;
}

inline FStatId OcclusionTested()
{
	static const FStatId Id = FStats::Register({"Occlusion", "Occlusion Tested", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId OcclusionTriangleBudget()
{
	static const FStatId Id = FStats::Register({"Occlusion", "Triangle Budget Exceeded (0/1)", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId OcclusionCpuBudget()
{
	static const FStatId Id = FStats::Register({"Occlusion", "CPU Budget Exceeded (0/1)", EStatUnit::Count, EStatMode::Gauge, true});
	return Id;
}

inline FStatId MemoryObjects()
{
	static const FStatId Id = FStats::Register({"Memory", "Object Bytes", EStatUnit::Bytes, EStatMode::Gauge, false});
	return Id;
}

inline FStatId MemoryAllocations()
{
	static const FStatId Id = FStats::Register({"Memory", "Object Allocations", EStatUnit::Count, EStatMode::Gauge, false});
	return Id;
}

inline FStatId MemoryProcess()
{
	static const FStatId Id = FStats::Register({"Memory", "Process Working Set", EStatUnit::Bytes, EStatMode::Gauge, false});
	return Id;
}

// Register before UI iteration; defaults belong to each definition.
inline void RegisterAll()
{
	PickingTotal();
	PickingBroad();
	PickingNarrow();
	OcclusionCaptured();
	OcclusionStatic();
	OcclusionDynamic();
	OcclusionFrustumRejected();
	OcclusionRejected();
	OcclusionVisible();
	RenderPackets();
	OcclusionOccluders();
	OcclusionSourceTriangles();
	OcclusionBVHTested();
	OcclusionBVHPruned();
	OcclusionCullTime();
	OcclusionBVHBuildTime();
	FrameFPS();
	FrameTime();
	OcclusionTested();
	OcclusionTriangleBudget();
	OcclusionCpuBudget();
	MemoryObjects();
	MemoryAllocations();
	MemoryProcess();
}
} // namespace StatIds