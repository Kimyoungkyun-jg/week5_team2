#pragma once
#include <atomic>
#include <cstdint>

// 일감 실행 함수 원형
using FJobFunction = void(*)(void* Data);

struct FFiberCounter;

// 파이버 일감 단위
struct FFiberJob
{
	FJobFunction Function = nullptr;
	void* Data = nullptr;
	FFiberCounter* Counter = nullptr;
};

// 파이버 동기화 카운터
struct FFiberCounter
{
	std::atomic<int32_t> Value{ 0 };
	std::atomic<int32_t> WaitingCount{ 0 };
	void* WaitingFibers[64]{ nullptr };
	std::atomic_flag Lock = ATOMIC_FLAG_INIT;

	// 대기 파이버 등록
	void AddWaitingFiber(void* InFiber)
	{
		while (Lock.test_and_set(std::memory_order_acquire)) {}
		int32_t Index = WaitingCount.load(std::memory_order_relaxed);
		if (Index < 64)
		{
			WaitingFibers[Index] = InFiber;
			WaitingCount.store(Index + 1, std::memory_order_release);
		}
		Lock.clear(std::memory_order_release);
	}
};
