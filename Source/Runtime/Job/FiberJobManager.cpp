#include "EnginePCH.h"
#include "FiberJobManager.h"
#include <windows.h>

thread_local void* FFiberJobManager::ThreadFiber = nullptr;

// 윈도우 파이버 실행 콜백
static VOID CALLBACK FiberEntryPoint(PVOID lpParameter)
{
	FFiberJobManager* Manager = static_cast<FFiberJobManager*>(lpParameter);
	if (Manager)
	{
		Manager->FiberWorkerLoop();
	}
}

// 싱글톤 인스턴스 획득
FFiberJobManager& FFiberJobManager::Get()
{
	static FFiberJobManager Instance;
	return Instance;
}

FFiberJobManager::~FFiberJobManager()
{
	Shutdown();
}

// 잡 시스템 초기화
void FFiberJobManager::Initialize(uint32_t InNumWorkers, uint32_t InNumFibers, uint32_t InFiberStackSize)
{
	if (bIsRunning.load())
	{
		return;
	}

	bIsRunning.store(true);
	FiberStackSize = InFiberStackSize;

	// 사용 가능한 하드웨어 스레드 수 산출
	if (InNumWorkers == 0)
	{
		uint32_t HardwareThreads = std::thread::hardware_concurrency();
		if (HardwareThreads == 0)
		{
			HardwareThreads = 4;
		}
		// 메인 스레드 제외 최대 코어 활용
		NumWorkers = (HardwareThreads > 1) ? (HardwareThreads - 1) : 1;
	}
	else
	{
		NumWorkers = InNumWorkers;
	}

	NumFibers = (std::max)(InNumFibers, NumWorkers * 4);

	// 메인 스레드 파이버 변환
	if (!IsThreadAFiber())
	{
		ThreadFiber = ConvertThreadToFiber(nullptr);
	}
	else
	{
		ThreadFiber = GetCurrentFiber();
	}

	// 파이버 풀 생성
	{
		std::lock_guard<std::mutex> Lock(FiberPoolMutex);
		FiberPool.reserve(NumFibers);
		for (uint32_t Index = 0; Index < NumFibers; ++Index)
		{
			void* Fiber = CreateJobFiber();
			if (Fiber)
			{
				FiberPool.push_back(Fiber);
			}
		}
	}

	// 워커 스레드 생성
	Workers.reserve(NumWorkers);
	for (uint32_t Index = 0; Index < NumWorkers; ++Index)
	{
		Workers.emplace_back([this]()
		{
			// 워커 스레드 파이버 변환
			if (!IsThreadAFiber())
			{
				ThreadFiber = ConvertThreadToFiber(nullptr);
			}
			else
			{
				ThreadFiber = GetCurrentFiber();
			}

			// 워커 메인 루프
			while (bIsRunning.load(std::memory_order_relaxed))
			{
				// 일감이 생길 때까지 대기
				{
					std::unique_lock<std::mutex> Lock(JobQueueMutex);
					WakeCondition.wait(Lock, [this]()
					{
						return !JobQueue.empty() || !bIsRunning.load(std::memory_order_relaxed);
					});
				}

				if (!bIsRunning.load(std::memory_order_relaxed))
				{
					break;
				}

				// 놀고 있는 파이버로 전환하여 일감 처리
				void* Fiber = PopFreeFiber();
				if (Fiber)
				{
					SwitchToFiber(Fiber);
				}
			}
		});
	}
}

// 잡 시스템 종료
void FFiberJobManager::Shutdown()
{
	if (!bIsRunning.exchange(false))
	{
		return;
	}

	WakeCondition.notify_all();

	// 워커 스레드 합류
	for (std::thread& Worker : Workers)
	{
		if (Worker.joinable())
		{
			Worker.join();
		}
	}
	Workers.clear();

	// 파이버 풀 해제
	{
		std::lock_guard<std::mutex> Lock(FiberPoolMutex);
		for (void* Fiber : FiberPool)
		{
			DeleteFiber(Fiber);
		}
		FiberPool.clear();
	}
}

// 단일 일감 등록
void FFiberJobManager::RunJob(const FFiberJob& InJob)
{
	RunJobs(&InJob, 1, InJob.Counter);
}

// 다중 일감 등록
void FFiberJobManager::RunJobs(const FFiberJob* InJobs, uint32_t InNumJobs, FFiberCounter* InCounter)
{
	if (!InJobs || InNumJobs == 0)
	{
		return;
	}

	if (InCounter)
	{
		InCounter->Value.fetch_add(InNumJobs, std::memory_order_relaxed);
	}

	{
		std::lock_guard<std::mutex> Lock(JobQueueMutex);
		for (uint32_t Index = 0; Index < InNumJobs; ++Index)
		{
			FFiberJob Job = InJobs[Index];
			if (InCounter)
			{
				Job.Counter = InCounter;
			}
			JobQueue.push(Job);
		}
	}

	WakeCondition.notify_all();
}

// 카운터 완료 대기
void FFiberJobManager::WaitForCounter(FFiberCounter* InCounter, int32_t InTargetValue)
{
	if (!InCounter)
	{
		return;
	}

	while (InCounter->Value.load(std::memory_order_acquire) > InTargetValue)
	{
		FFiberJob NextJob;
		if (PopJob(NextJob))
		{
			// 대기 중인 일감 직접 처리
			if (NextJob.Function)
			{
				NextJob.Function(NextJob.Data);
			}
			if (NextJob.Counter)
			{
				NextJob.Counter->Value.fetch_sub(1, std::memory_order_release);
			}
		}
		else
		{
			// 일감이 남아있지 않으면 양보
			std::this_thread::yield();
		}
	}
}

// 파이버 풀에 반환
void FFiberJobManager::ReturnFiber(void* InFiber)
{
	if (!InFiber)
	{
		return;
	}

	std::lock_guard<std::mutex> Lock(FiberPoolMutex);
	FiberPool.push_back(InFiber);
}

// 파이버 작업 루프
void FFiberJobManager::FiberWorkerLoop()
{
	while (bIsRunning.load(std::memory_order_relaxed))
	{
		FFiberJob Job;
		if (PopJob(Job))
		{
			if (Job.Function)
			{
				Job.Function(Job.Data);
			}

			if (Job.Counter)
			{
				Job.Counter->Value.fetch_sub(1, std::memory_order_release);
			}
		}
		else
		{
			// 일감이 없으면 파이버를 풀에 반환하고 원래 스레드로 복귀
			void* CurrentFiberHandle = GetCurrentFiber();
			ReturnFiber(CurrentFiberHandle);

			if (ThreadFiber && ThreadFiber != CurrentFiberHandle)
			{
				SwitchToFiber(ThreadFiber);
			}
			else
			{
				break;
			}
		}
	}

	if (ThreadFiber)
	{
		SwitchToFiber(ThreadFiber);
	}
}

// 신규 파이버 생성
void* FFiberJobManager::CreateJobFiber()
{
	return CreateFiberEx(FiberStackSize, FiberStackSize, 0, FiberEntryPoint, this);
}

// 큐에서 일감 인출
bool FFiberJobManager::PopJob(FFiberJob& OutJob)
{
	std::lock_guard<std::mutex> Lock(JobQueueMutex);
	if (JobQueue.empty())
	{
		return false;
	}

	OutJob = JobQueue.front();
	JobQueue.pop();
	return true;
}

// 풀에서 놀고 있는 파이버 인출
void* FFiberJobManager::PopFreeFiber()
{
	std::lock_guard<std::mutex> Lock(FiberPoolMutex);
	if (FiberPool.empty())
	{
		return nullptr;
	}

	void* Fiber = FiberPool.back();
	FiberPool.pop_back();
	return Fiber;
}
