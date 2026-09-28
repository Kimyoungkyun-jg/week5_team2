#include "EnginePCH.h"
#include "FiberJobManager.h"
#include <windows.h>

// 파이버 작업 진입점
static VOID CALLBACK FiberEntryPoint(PVOID lpParameter)
{
	FFiberJobManager::FFiberTaskContext* Context = static_cast<FFiberJobManager::FFiberTaskContext*>(lpParameter);
	if (Context && Context->Manager)
	{
		Context->Manager->FiberWorkerLoop(Context);
	}
}

// 싱글톤 반환
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

	// 워커 스레드 수 산출
	if (InNumWorkers == 0)
	{
		uint32_t HardwareThreads = std::thread::hardware_concurrency();
		if (HardwareThreads == 0)
		{
			HardwareThreads = 4;
		}
		// 코어 활용
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
		ConvertThreadToFiber(nullptr);
	}

	// 파이버 풀 생성
	{
		std::lock_guard<std::mutex> Lock(FiberPoolMutex);
		FiberPool.reserve(NumFibers);
		AllocatedFibers.reserve(NumFibers);
		for (uint32_t Index = 0; Index < NumFibers; ++Index)
		{
			FFiberTaskContext* Context = CreateJobFiber();
			if (Context)
			{
				FiberPool.push_back(Context);
				AllocatedFibers.push_back(Context);
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
				ConvertThreadToFiber(nullptr);
			}

			// 워커 루프
			while (bIsRunning.load(std::memory_order_relaxed))
			{
				FFiberJob Job;
				{
					// 대기
					std::unique_lock<std::mutex> Lock(JobQueueMutex);
					WakeCondition.wait(Lock, [this]()
					{
						return !JobQueue.empty() || !bIsRunning.load(std::memory_order_relaxed);
					});

					if (!bIsRunning.load(std::memory_order_relaxed) && JobQueue.empty())
					{
						break;
					}

					if (!JobQueue.empty())
					{
						Job = JobQueue.front();
						JobQueue.pop();
					}
				}

				if (!Job.Function)
				{
					continue;
				}

				// 일감 처리
				FFiberTaskContext* Context = PopFreeFiber();
				if (Context)
				{
					Context->CallerFiber = GetCurrentFiber();
					Context->CurrentJob = Job;
					SwitchToFiber(Context->FiberHandle);
					// 파이버 반환
					ReturnFiber(Context);
				}
				else
				{
					// 직접 처리
					try
					{
						Job.Function(Job.Data);
					}
					catch (...)
					{
					}

					if (Job.Counter)
					{
						Job.Counter->Value.fetch_sub(1, std::memory_order_seq_cst);
					}
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

	// 워커 종료 대기
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
		for (FFiberTaskContext* Context : AllocatedFibers)
		{
			if (Context)
			{
				if (Context->FiberHandle)
				{
					DeleteFiber(Context->FiberHandle);
				}
				delete Context;
			}
		}
		FiberPool.clear();
		AllocatedFibers.clear();
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
		// 카운터 증가
		InCounter->Value.fetch_add(InNumJobs, std::memory_order_seq_cst);
	}

	{
		// 큐에 삽입
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

	// 워커 깨우기
	WakeCondition.notify_all();
}

// 카운터 대기
void FFiberJobManager::WaitForCounter(FFiberCounter* InCounter, int32_t InTargetValue)
{
	if (!InCounter)
	{
		return;
	}

	while (InCounter->Value.load(std::memory_order_seq_cst) > InTargetValue)
	{
		FFiberJob NextJob;
		if (PopJob(NextJob))
		{
			// 대기 중 일감 직접 처리
			if (NextJob.Function)
			{
				try
				{
					NextJob.Function(NextJob.Data);
				}
				catch (...)
				{
				}
			}
			if (NextJob.Counter)
			{
				// 카운터 감소
				NextJob.Counter->Value.fetch_sub(1, std::memory_order_seq_cst);
			}
		}
		else
		{
			// 양보
			std::this_thread::yield();
		}
	}
}

// 파이버 반환
void FFiberJobManager::ReturnFiber(FFiberTaskContext* InContext)
{
	if (!InContext)
	{
		return;
	}

	std::lock_guard<std::mutex> Lock(FiberPoolMutex);
	FiberPool.push_back(InContext);
}

// 파이버 루프
void FFiberJobManager::FiberWorkerLoop(FFiberTaskContext* Context)
{
	while (bIsRunning.load(std::memory_order_relaxed))
	{
		if (Context->CurrentJob.Function)
		{
			try
			{
				Context->CurrentJob.Function(Context->CurrentJob.Data);
			}
			catch (...)
			{
			}
		}

		if (Context->CurrentJob.Counter)
		{
			// 카운터 감소
			Context->CurrentJob.Counter->Value.fetch_sub(1, std::memory_order_seq_cst);
			Context->CurrentJob.Counter = nullptr;
		}

		Context->CurrentJob.Function = nullptr;
		Context->CurrentJob.Data = nullptr;

		void* Caller = Context->CallerFiber;
		Context->CallerFiber = nullptr;

		if (Caller)
		{
			SwitchToFiber(Caller);
		}
		else
		{
			break;
		}
	}
}

// 파이버 생성
FFiberJobManager::FFiberTaskContext* FFiberJobManager::CreateJobFiber()
{
	FFiberTaskContext* Context = new FFiberTaskContext();
	Context->Manager = this;
	Context->FiberHandle = CreateFiberEx(FiberStackSize, FiberStackSize, 0, FiberEntryPoint, Context);
	if (!Context->FiberHandle)
	{
		delete Context;
		return nullptr;
	}
	return Context;
}

// 일감 획득
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

// 놀고 있는 파이버 획득
FFiberJobManager::FFiberTaskContext* FFiberJobManager::PopFreeFiber()
{
	std::lock_guard<std::mutex> Lock(FiberPoolMutex);
	if (FiberPool.empty())
	{
		return nullptr;
	}

	FFiberTaskContext* Context = FiberPool.back();
	FiberPool.pop_back();
	return Context;
}
