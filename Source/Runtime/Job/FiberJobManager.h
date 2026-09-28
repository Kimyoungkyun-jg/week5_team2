#pragma once
#include "FiberTypes.h"
#include <vector>
#include <thread>
#include <mutex>
#include <queue>

// 파이버 기반 잡 시스템 관리자
class FFiberJobManager
{
public:
	// 싱글톤 인스턴스 반환
	static FFiberJobManager& Get();

	// 잡 매니저 초기화
	void Initialize(uint32_t InNumWorkers = 0, uint32_t InNumFibers = 128, uint32_t InFiberStackSize = 64 * 1024);

	// 잡 매니저 종료
	void Shutdown();

	// 일감 등록
	void RunJob(const FFiberJob& InJob);
	void RunJobs(const FFiberJob* InJobs, uint32_t InNumJobs, FFiberCounter* InCounter = nullptr);

	// 카운터 완료 대기
	void WaitForCounter(FFiberCounter* InCounter, int32_t InTargetValue = 0);

	// 유효 파이버 반환
	void ReturnFiber(void* InFiber);

	// 내부 파이버 실행 진입점
	void FiberWorkerLoop();

private:
	FFiberJobManager() = default;
	~FFiberJobManager();

	// 파이버 생성
	void* CreateJobFiber();

	// 다음 일감 획득
	bool PopJob(FFiberJob& OutJob);

	// 놀고 있는 파이버 획득
	void* PopFreeFiber();

private:
	std::atomic<bool> bIsRunning{ false };
	uint32_t NumWorkers = 0;
	uint32_t NumFibers = 0;
	uint32_t FiberStackSize = 0;

	std::vector<std::thread> Workers;
	std::vector<void*> FiberPool;
	std::mutex FiberPoolMutex;

	std::queue<FFiberJob> JobQueue;
	std::mutex JobQueueMutex;

	thread_local static void* ThreadFiber;
};
