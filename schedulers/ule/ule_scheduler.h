
#ifndef GHOST_SCHEDULERS_ULE_SCHEDULER_H_
#define GHOST_SCHEDULERS_ULE_SCHEDULER_H_

#include <climits>
#include <cstdint>
#include <iostream>
#include <memory>
#include <ostream>
#include <set>

#include "absl/container/flat_hash_map.h"
#include "absl/functional/any_invocable.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "lib/agent.h"
#include "lib/base.h"
#include "lib/scheduler.h"

static const absl::Time start = absl::Now();

namespace ghost {

static constexpr int SRQ_BORING	= 0x0000;		/* No special circumstances. */
static constexpr int SRQ_YIELDING	= 0x0001;		/* We are yielding (from mi_switch). */
static constexpr int SRQ_OURSELF	= 0x0002;		/* It is ourself (from mi_switch). */
static constexpr int SRQ_INTR	= 0x0004;		/* It is probably urgent. */
static constexpr int SRQ_PREEMPTED = 0x0008;		/* has been preempted.. be kind */
static constexpr int SRQ_BORROWING = 0x0010;		/* Priority updated due to prio_lend */
static constexpr int SRQ_HOLD =	0x0020;		/* Return holding original td lock */
static constexpr int SRQ_HOLDTD =	0x0040;		/* Return holding td lock */


class UleTask;

class UleRunq {

  static constexpr int RQ_NQS	=	64;		/* Number of run queues. */
  static constexpr int RQ_PPQ	=	4;		/* Priorities per queue. */
public:
  explicit UleRunq();
  UleRunq(const UleRunq&) = delete;
  UleRunq& operator=(UleRunq&) = delete;

  void runq_add(UleTask *task, int flags);
  void runq_add_pri(UleTask *task, u_char pri, int flags);


  /*
  * Find the highest priority process on the run queue.
  */
  UleTask* runq_choose();
  UleTask* runq_choose_from(u_char);

  void runq_remove(UleTask *td);
  void runq_remove_idx(UleTask *td, u_char *idx);


  int runq_check(); // Basically isEmpty - but uses rq_status

  // TODO: Add all methods required by RunQ

  // Protects this runqueue and the state of any task assoicated with the rq.
  mutable absl::Mutex mu_;

 private:
  inline void runq_setbit(int pri) {
    rq_status |= (1ul<< (pri & 63));
  }
  inline void runq_clrbit(int pri) {
    rq_status &= ~(1ul<< (pri & 63));
  }
  inline int runq_findbit() {
    if (rq_status != 0) {
      return __builtin_clzl(rq_status);
    }
    return -1;
  }
  inline int runq_findbit_from(int pri) {
    uint64_t mask = ((uint64_t)-1) << (pri & 63);
    mask = rq_status & mask;
    if (mask == 0)
			return -1;
		return __builtin_clzl(mask);
  }

  
  std::list<UleTask*> runq_[RQ_NQS];
  uint64_t rq_status; // Bitset of non-empty queues

};

// This corresponds to (struct thread + struct td_sched) in FreeBSD
struct UleTask : public Task<> {
  explicit UleTask(Gtid d_task_gtid, ghost_sw_info sw_info)
      : Task<>(d_task_gtid, sw_info) {}
  ~UleTask() override {}

  // <Fields from struct thread - may need more of them later
  std::list<UleTask*>::iterator td_runq;
  u_char		td_rqindex;	/* (t) Run queue index. */
  u_char		td_base_pri;	/* (t) Thread base kernel priority. */
	u_char		td_priority;	/* (t) Thread active priority. */
	u_char		td_pri_class;	/* (t) Scheduling class. */
	u_char		td_user_pri;	/* (t) User pri from estcpu and nice. */
	u_char		td_base_user_pri; /* (t) Base user pri */
  // Fields from struct thread>

  // <Fields from struct td_sched
  // TODO: Decide which ones are relevant later
  UleRunq	*ts_runq;	/* Run-queue we're queued on. */
	short		ts_flags;	/* TSF_* flags. */
	int		ts_cpu;		/* CPU that we have affinity for. */
	int		ts_rltick;	/* Real last tick, for affinity. */
	int		ts_slice;	/* Ticks of slice remaining. */
	u_int		ts_slptime;	/* Number of ticks we vol. slept */
	u_int		ts_runtime;	/* Number of ticks we were running */
	int		ts_ltick;	/* Last tick that we were running on */
	int		ts_ftick;	/* First tick that we were running on */
	int		ts_ticks;	/* Tick count */

  // <Fields from struct td_sched > 
};




class UleScheduler : public BasicDispatchScheduler<UleTask> {
 public:

  // TODO: Copy other macros from FreeBSD
  static constexpr int SCHED_INTERACT_THRESH = 30;



  explicit UleScheduler(Enclave* enclave, CpuList cpulist,
                        std::shared_ptr<TaskAllocator<UleTask>> allocator,
                        absl::Duration min_granularity, absl::Duration latency);
  ~UleScheduler() final {}

  void Schedule(const Cpu& cpu, const StatusWord& sw);

  void EnclaveReady() final;
  Channel& GetDefaultChannel() final { return *default_channel_; };
  Channel& GetAgentChannel(const Cpu& cpu) final {
    Channel* channel; // TODO: Fix this
    CHECK_NE(channel, nullptr);
    return *channel;
  }

  bool Empty(const Cpu& cpu) {

  }

  //void DumpState(const Cpu& cpu, int flags) final;
  std::atomic<bool> debug_runqueue_ = false;

  int CountAllTasks() const {
    int num_tasks = 0;
    allocator()->ForEachTask([&num_tasks](Gtid gtid, const UleTask* task) {
      ++num_tasks;
      return true;
    });
    return num_tasks;
  }

  static constexpr int kDebugRunqueue = 1;
  static constexpr int kCountAllTasks = 2;

 protected:
  // Note: this CPU's rq lock is held during the draining callbacks listed
  // below.
  void TaskNew(UleTask* task, const Message& msg) final;
  void TaskRunnable(UleTask* task, const Message& msg) final;
  void TaskDeparted(UleTask* task, const Message& msg) final;
  void TaskDead(UleTask* task, const Message& msg) final;
  void TaskYield(UleTask* task, const Message& msg) final;
  void TaskBlocked(UleTask* task, const Message& msg) final;
  void TaskPreempted(UleTask* task, const Message& msg) final;
  void TaskSwitchto(UleTask* task, const Message& msg) final;
  void TaskAffinityChanged(UleTask* task, const Message& msg) final;
  void TaskPriorityChanged(UleTask* task, const Message& msg) final;
  void CpuTick(const Message& msg) final;

 private:
  // Empties the channel associated with cpu and dispatches the messages.
  void DrainChannel(const Cpu& cpu);

  // Checks if we should preempt the current task. If so, sets preempt_curr_.
  // Note: Should be called with this CPU's rq mutex lock held.
  void CheckPreemptTick(const Cpu& cpu);

  // Kicks a given task off-cpu and puts into this CPU's run queue or initiate
  // its migration if this CPU is no longer eligible as per its affinity mask.
  // If the task was on-cpu (cs->current == task), cs->current will be reset as
  // a side effect.
  void PutPrevTask(UleTask* task);

  // UleSchedule looks at the current cpu state and its run_queue, decides what
  // to run next, and then commits a txn. REQUIRES: Called after all messages
  // have been ack'ed otherwise the txn will fail.
  void UleSchedule(const Cpu& cpu, BarrierToken agent_barrier, bool prio_boost);

  // HandleTaskDone is responsible for remvoing a task from the run queue and
  // freeing it if it is currently !cs->current, otherwise, it defers the
  // freeing to PickNextTask.
  // Note: Should be called with this CPU's rq mutex lock held.
  void HandleTaskDone(UleTask* task, bool from_switchto);

  // Dequeues the task from its RQ and initiates its migration. Places the task
  // in the migration queue and does not immediately migrate the task.
  void StartMigrateTask(UleTask* task);
  // Moves the current task off-cpu and initiates its migration. Places the task
  // in the migration queue and does not immediately migrate the task.
  void StartMigrateCurrTask();

  // Attaches tasks defined by the load balance environment.
  inline void AttachTasks(struct LoadBalanceEnv& env);

  // Detaches tasks required by the load balance environment.
  // Returns: the number of detached tasks.
  // inline int DetachTasks(struct LoadBalanceEnv& env);

  // Calculates the imbalance between source CPU and destination
  // CPU.
  // inline int CalculateImbalance(LoadBalanceEnv& env);

  // Finds the CPU with most RUNNABLE tasks. Returns the ID of the busiest CPU.
  // inline int FindBusiestQueue();

  // Determines whether to run load balancing in this context. Specifically,
  // returns true if this CPU became idle just now (`newly_idle` is true), the
  // current CPU is the first idle CPU, or (if this CPU is not idle) the first
  // CPU. This function roughly follows `should_we_balance` function in
  // `kernel/sched/fair.c`.
  // inline bool ShouldWeBalance(LoadBalanceEnv& env);

  // Tries to load balance when this CPU is about to become idle and attempts
  // to take some tasks from another CPU. Should only be called inside the
  // schedule loop.
  // Returns: one of the pulled tasks picked via `PickNextTask` or nullptr if
  // failed pull any task.
  // inline UleTask* NewIdleBalance(CpuState* cs);

  // Tries to balance the load across different CPUs to make sure each CPU has
  // about an equal amount of work. The gist of the algorithm is to balance the
  // busiest and least busy core.
  // Following this check, we find the rq with the heaviest load and balance it
  // with the rq with the lightest load.
  // int LoadBalance(CpuState* cs, CpuIdleType idle_type);

  // Migrate takes task and places it on cpu's run queue.
  // bool Migrate(UleTask* task, Cpu cpu, BarrierToken seqnum);
  // Migrates pending tasks in the migration queue.
 
  // Cpu SelectTaskRq(UleTask* task);
  void DumpAllTasks();

  void PingCpu(const Cpu& cpu);

  // CpuState* cpu_state(const Cpu& cpu) { return &cpu_states_[cpu.id()]; }

  // CpuState* cpu_state_of(const UleTask* task) {
  //   CHECK_GE(task->cpu, 0);
  //   CHECK_LT(task->cpu, MAX_CPUS);
  //   return &cpu_states_[task->cpu];
  // }

  // If called with is_agent_thread = true, then we use the cache'd TLS cpu id
  // as agent threads are local to a single cpu, otherwise, issue a syscall.
  int MyCpu(bool is_agent_thread = true) {
    if (!is_agent_thread) return sched_getcpu();
    // "When thread_local is applied to a variable of block scope the
    // storage-class-specifier static is implied if it does not appear
    // explicitly" - C++ standard.
    // This isn't obvious, so keep the static modifier.
    static thread_local int my_cpu = -1;
    if (my_cpu == -1) {
      my_cpu = sched_getcpu();
    }
    return my_cpu;
  }

  Channel* default_channel_ = nullptr;

  absl::Duration min_granularity_;
  absl::Duration latency_;

  bool idle_load_balancing_;
};

std::unique_ptr<UleScheduler> MultiThreadedUleScheduler(
    Enclave* enclave, CpuList cpulist, absl::Duration min_granularity,
    absl::Duration latency);
class UleAgent : public LocalAgent {
 public:
  UleAgent(Enclave* enclave, Cpu cpu, UleScheduler* scheduler)
      : LocalAgent(enclave, cpu), scheduler_(scheduler) {}

  void AgentThread() override;
  Scheduler* AgentScheduler() const override { return scheduler_; }

 private:
  UleScheduler* scheduler_;
};

class UleConfig : public AgentConfig {
 public:
  UleConfig() {}
  UleConfig(Topology* topology, CpuList cpulist)
      : AgentConfig(topology, std::move(cpulist)) {}
  UleConfig(Topology* topology, CpuList cpulist, absl::Duration min_granularity,
            absl::Duration latency)
      : AgentConfig(topology, std::move(cpulist)),
        min_granularity_(min_granularity),
        latency_(latency) {}

  absl::Duration min_granularity_;
  absl::Duration latency_;
};

// TODO: Pull these classes out into different files.
template <class EnclaveType>
class FullUleAgent : public FullAgent<EnclaveType> {
 public:
  explicit FullUleAgent(UleConfig config) : FullAgent<EnclaveType>(config) {
    scheduler_ =
        MultiThreadedUleScheduler(&this->enclave_, *this->enclave_.cpus(),
                                  config.min_granularity_, config.latency_);
    this->StartAgentTasks();
    this->enclave_.Ready();
  }

  ~FullUleAgent() override { this->TerminateAgentTasks(); }

  std::unique_ptr<Agent> MakeAgent(const Cpu& cpu) override {
    return std::make_unique<UleAgent>(&this->enclave_, cpu, scheduler_.get());
  }

  void RpcHandler(int64_t req, const AgentRpcArgs& args,
                  AgentRpcResponse& response) override {
    switch (req) {
      case UleScheduler::kDebugRunqueue:
        scheduler_->debug_runqueue_ = true;
        response.response_code = 0;
        return;
      case UleScheduler::kCountAllTasks:
        response.response_code = scheduler_->CountAllTasks();
        return;
      default:
        response.response_code = -1;
        return;
    }
  }

 private:
  std::unique_ptr<UleScheduler> scheduler_;
};

}  // namespace ghost

#endif  // GHOST_SCHEDULERS_ULE_SCHEDULER_H_
