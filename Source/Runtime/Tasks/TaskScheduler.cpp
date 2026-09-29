#include "EnginePCH.h"
#include "TaskScheduler.h"
#include <algorithm>

namespace Tasks {
// 스레드 로컬 워커 식별자
static thread_local int32_t TLS_WorkerIndex = -1;

FTaskScheduler &FTaskScheduler::Get() {
  static FTaskScheduler Instance;
  return Instance;
}

FTaskScheduler::~FTaskScheduler() { Shutdown(); }

int32_t FTaskScheduler::GetCurrentWorkerIndex() const {
  return TLS_WorkerIndex;
}

void FTaskScheduler::Initialize(uint32_t InNumWorkers) {
  if (bIsRunning.load(std::memory_order_relaxed)) {
    return;
  }

  if (InNumWorkers == 0) {
    uint32_t HardwareThreads = std::thread::hardware_concurrency();
    NumWorkers = HardwareThreads > 1 ? HardwareThreads - 1 : 1;
  } else {
    NumWorkers = InNumWorkers;
  }

  WorkerQueues.clear();
  WorkerQueues.reserve(NumWorkers);
  for (uint32_t Index = 0; Index < NumWorkers; ++Index) {
    WorkerQueues.push_back(std::make_unique<FWorkStealingQueue>());
  }

  bIsRunning.store(true, std::memory_order_release);

  Workers.clear();
  Workers.reserve(NumWorkers);
  for (uint32_t Index = 0; Index < NumWorkers; ++Index) {
    Workers.emplace_back(&FTaskScheduler::WorkerLoop, this,
                         static_cast<int32_t>(Index));
  }
}

void FTaskScheduler::Shutdown() {
  if (!bIsRunning.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  {
    std::lock_guard<std::mutex> Lock(WakeMutex);
    WakeCondition.notify_all();
  }

  for (std::thread &Worker : Workers) {
    if (Worker.joinable()) {
      Worker.join();
    }
  }
  Workers.clear();
  WorkerQueues.clear();

  std::lock_guard<std::mutex> Lock(GlobalQueueMutex);
  GlobalQueue.clear();
}

void FTaskScheduler::Schedule(const FLowLevelTask &Task) {
  const int32_t CurrentWorker = TLS_WorkerIndex;

  // 현재 스레드가 워커인 경우 로컬 큐에 우선 배치
  if (CurrentWorker >= 0 &&
      static_cast<size_t>(CurrentWorker) < WorkerQueues.size()) {
    if (WorkerQueues[CurrentWorker]->Push(Task)) {
      if (SleepingWorkerCount.load(std::memory_order_relaxed) > 0) {
        std::lock_guard<std::mutex> Lock(WakeMutex);
        WakeCondition.notify_one();
      }
      return;
    }
  }

  // 로컬 큐가 가득 찼거나 외부 스레드인 경우 글로벌 큐에 배치
  {
    std::lock_guard<std::mutex> Lock(GlobalQueueMutex);
    GlobalQueue.push_back(Task);
  }

  {
    std::lock_guard<std::mutex> Lock(WakeMutex);
    WakeCondition.notify_one();
  }
}

bool FTaskScheduler::ExecuteOneTask() {
  const int32_t CurrentWorker = TLS_WorkerIndex;
  FLowLevelTask TaskToExecute{};

  // 로컬 큐 우선 인출
  if (CurrentWorker >= 0 &&
      static_cast<size_t>(CurrentWorker) < WorkerQueues.size()) {
    if (WorkerQueues[CurrentWorker]->Pop(TaskToExecute)) {
      TaskToExecute.Execute();
      return true;
    }
  }

  // 글로벌 큐 인출
  {
    std::unique_lock<std::mutex> Lock(GlobalQueueMutex);
    if (!GlobalQueue.empty()) {
      TaskToExecute = GlobalQueue.front();
      GlobalQueue.pop_front();
      Lock.unlock();
      TaskToExecute.Execute();
      return true;
    }
  }

  // 타 워커 큐 강탈 시도
  const size_t QueueCount = WorkerQueues.size();
  if (QueueCount > 0) {
    const size_t StartOffset =
        CurrentWorker >= 0 ? static_cast<size_t>(CurrentWorker + 1) : 0;
    for (size_t Step = 0; Step < QueueCount; ++Step) {
      const size_t TargetIndex = (StartOffset + Step) % QueueCount;
      if (static_cast<int32_t>(TargetIndex) == CurrentWorker) {
        continue;
      }

      if (WorkerQueues[TargetIndex]->Steal(TaskToExecute)) {
        TaskToExecute.Execute();
        return true;
      }
    }
  }

  return false;
}

void FTaskScheduler::HelpSteal(const std::function<bool()> &StopCondition) {
  while (bIsRunning.load(std::memory_order_relaxed)) {
    if (StopCondition && StopCondition()) {
      break;
    }

    if (!ExecuteOneTask()) {
      std::this_thread::yield();
    }
  }
}

void FTaskScheduler::WorkerLoop(const int32_t WorkerIndex) {
  TLS_WorkerIndex = WorkerIndex;

  while (bIsRunning.load(std::memory_order_relaxed)) {
    if (ExecuteOneTask()) {
      continue;
    }

    // 처리할 일감이 없으면 대기 상태로 전환
    std::unique_lock<std::mutex> Lock(WakeMutex);
    SleepingWorkerCount.fetch_add(1, std::memory_order_relaxed);

    WakeCondition.wait_for(Lock, std::chrono::milliseconds(1), [this]() {
      if (!bIsRunning.load(std::memory_order_relaxed)) {
        return true;
      }
      return false;
    });

    SleepingWorkerCount.fetch_sub(1, std::memory_order_relaxed);
  }
}
} // namespace Tasks
