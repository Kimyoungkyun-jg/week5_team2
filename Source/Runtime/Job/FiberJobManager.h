#pragma once
#include "FiberTypes.h"
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <algorithm>

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

	// 워커 스레드 수 반환
	uint32_t GetNumWorkers() const { return NumWorkers; }

	// 일감 등록
	void RunJob(const FFiberJob& InJob);
	void RunJobs(const FFiberJob* InJobs, uint32_t InNumJobs, FFiberCounter* InCounter = nullptr);

	// 카운터 완료 대기
	void WaitForCounter(FFiberCounter* InCounter, int32_t InTargetValue = 0);

	// 병렬 분할 실행 템플릿
	template<typename FuncType>
	void ParallelFor(int32_t TotalCount, int32_t ChunkSize, const FuncType& Function)
	{
		if (TotalCount <= 0) return;
		if (ChunkSize <= 0) ChunkSize = 1;

		int32_t NumJobs = (TotalCount + ChunkSize - 1) / ChunkSize;
		if (NumJobs <= 1)
		{
			Function(0, TotalCount);
			return;
		}

		struct FParallelTask
		{
			const FuncType* Func;
			int32_t Start;
			int32_t End;

			static void Execute(void* InData)
			{
				FParallelTask* Task = static_cast<FParallelTask*>(InData);
				const FuncType& Lambda = *Task->Func;
				Lambda(Task->Start, Task->End);
			}
		};

		std::vector<FParallelTask> Tasks(NumJobs);
		std::vector<FFiberJob> Jobs(NumJobs);
		FFiberCounter Counter;

		for (int32_t Index = 0; Index < NumJobs; ++Index)
		{
			int32_t Start = Index * ChunkSize;
			int32_t End = (std::min)(Start + ChunkSize, TotalCount);

			Tasks[Index] = { &Function, Start, End };
			Jobs[Index] = { &FParallelTask::Execute, &Tasks[Index], &Counter };
		}

		RunJobs(Jobs.data(), NumJobs, &Counter);
		WaitForCounter(&Counter, 0);
	}

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

	// 파이버 풀에 반환
	void ReturnFiber(void* InFiber);

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
	std::condition_variable WakeCondition;

	thread_local static void* ThreadFiber;
};
