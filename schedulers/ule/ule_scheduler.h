
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

class UleTask;

class UleRunq {

  static constexpr int RQ_NQS	=	64;		/* Number of run queues. */
 public:
  explicit UleRunq();
  UleRunq(const UleRunq&) = delete;
  UleRunq& operator=(UleRunq&) = delete;

  
  // TODO: Add all methods required by RunQ

  // Protects this runqueue and the state of any task assoicated with the rq.
  mutable absl::Mutex mu_;

 private:
  std::list<UleTask*> runq_[RQ_NQS]; 
};

// This corresponds to (struct thread + struct td_sched) in FreeBSD
struct UleTask : public Task<> {
  explicit UleTask(Gtid d_task_gtid, ghost_sw_info sw_info)
      : Task<>(d_task_gtid, sw_info) {}
  ~UleTask() override {}

  // <Fields from struct thread - may need more of them later
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


class CpuState {

private:

/* flags kept in ts_flags */
#define	TSF_BOUND	0x0001		/* Thread can not migrate. */
#define	TSF_XFERABLE	0x0002		/* Thread was added as transferable. */

  /* Lockless accessors. */
  #define	TDQ_LOAD(tdq)		atomic_load_int(&(tdq)->tdq_load)
  #define	TDQ_TRANSFERABLE(tdq)	atomic_load_int(&(tdq)->tdq_transferable)
  #define	TDQ_SWITCHCNT(tdq)	(atomic_load_short(&(tdq)->tdq_switchcnt) + \
          atomic_load_short(&(tdq)->tdq_oldswitchcnt))
  #define	TDQ_SWITCHCNT_INC(tdq)	(atomic_store_short(&(tdq)->tdq_switchcnt, \
          atomic_load_short(&(tdq)->tdq_switchcnt) + 1))

  #define	TDQ_SELF()	((struct CpuState *)PCPU_GET(sched))
  #define	TDQ_CPU(x)	(DPCPU_ID_PTR((x), tdq))
  #define	TDQ_ID(x)	((x)->tdq_id)
  #else	/* !SMP */
  static struct tdq	tdq_cpu;

  #define	TDQ_ID(x)	(0)
  #define	TDQ_SELF()	(&tdq_cpu)
  #define	TDQ_CPU(x)	(&tdq_cpu)
  #endif

  #define	TDQ_LOCK_ASSERT(t, type)	mtx_assert(TDQ_LOCKPTR((t)), (type))
  #define	TDQ_LOCK(t)		mtx_lock_spin(TDQ_LOCKPTR((t)))
  #define	TDQ_LOCK_FLAGS(t, f)	mtx_lock_spin_flags(TDQ_LOCKPTR((t)), (f))
  #define	TDQ_TRYLOCK(t)		mtx_trylock_spin(TDQ_LOCKPTR((t)))
  #define	TDQ_TRYLOCK_FLAGS(t, f)	mtx_trylock_spin_flags(TDQ_LOCKPTR((t)), (f))
  #define	TDQ_UNLOCK(t)		mtx_unlock_spin(TDQ_LOCKPTR((t)))
#define	TDQ_LOCKPTR(t)		((struct mtx *)(&(t)->tdq_lock

#define	TD_SET_STATE(td, state)	(td)->td_state = state
#define	TD_SET_RUNNING(td)	TD_SET_STATE(td, TDS_RUNNING)
#define	TD_SET_RUNQ(td)		TD_SET_STATE(td, TDS_RUNQ)


#define	PRI_MIN_TIMESHARE	(88)
#define	PRI_MAX_TIMESHARE	(PRI_MIN_IDLE - 1)

#define	PUSER			(PRI_MIN_TIMESHARE)

#define	PRI_MIN_IDLE		(224)
#define	PRI_MAX_IDLE		(PRI_MAX)

/*
 * These macros determine priorities for non-interactive threads.  They are
 * assigned a priority based on their recent cpu utilization as expressed
 * by the ratio of ticks to the tick total.  NHALF priorities at the start
 * and end of the MIN to MAX timeshare range are only reachable with negative
 * or positive nice respectively.
 *
 * PRI_RANGE:	Priority range for utilization dependent priorities.
 * PRI_NRESV:	Number of nice values.
 * PRI_TICKS:	Compute a priority in PRI_RANGE from the ticks count and total.
 * PRI_NICE:	Determines the part of the priority inherited from nice.
 */
#define	SCHED_PRI_NRESV		(PRIO_MAX - PRIO_MIN)
#define	SCHED_PRI_NHALF		(SCHED_PRI_NRESV / 2)
#define	SCHED_PRI_MIN		(PRI_MIN_BATCH + SCHED_PRI_NHALF)
#define	SCHED_PRI_MAX		(PRI_MAX_BATCH - SCHED_PRI_NHALF)
#define	SCHED_PRI_RANGE		(SCHED_PRI_MAX - SCHED_PRI_MIN + 1)
#define	SCHED_PRI_TICKS(ts)						\
    (SCHED_TICK_HZ((ts)) /						\
    (roundup(SCHED_TICK_TOTAL((ts)), SCHED_PRI_RANGE) / SCHED_PRI_RANGE))
#define	SCHED_PRI_NICE(nice)	(nice)

/*
 * Priority ranges used for interactive and non-interactive timeshare
 * threads.  The timeshare priorities are split up into four ranges.
 * The first range handles interactive threads.  The last three ranges
 * (NHALF, x, and NHALF) handle non-interactive threads with the outer
 * ranges supporting nice values.
 */
#define	PRI_TIMESHARE_RANGE	(PRI_MAX_TIMESHARE - PRI_MIN_TIMESHARE + 1)
#define	PRI_INTERACT_RANGE	((PRI_TIMESHARE_RANGE - SCHED_PRI_NRESV) / 2)
#define	PRI_BATCH_RANGE		(PRI_TIMESHARE_RANGE - PRI_INTERACT_RANGE)

#define	PRI_MIN_INTERACT	PRI_MIN_TIMESHARE
#define	PRI_MAX_INTERACT	(PRI_MIN_TIMESHARE + PRI_INTERACT_RANGE - 1)
#define	PRI_MIN_BATCH		(PRI_MIN_TIMESHARE + PRI_INTERACT_RANGE)
#define	PRI_MAX_BATCH		PRI_MAX_TIMESHARE

/* sched_add arguments (formerly setrunqueue) */
// #define	SRQ_BORING	0x0000		/* No special circumstances. */
// #define	SRQ_YIELDING	0x0001		/* We are yielding (from mi_switch). */
// #define	SRQ_OURSELF	0x0002		/* It is ourself (from mi_switch). */
// #define	SRQ_INTR	0x0004		/* It is probably urgent. */
// #define	SRQ_HOLD	0x0020		/* Return holding original td lock */
// #define	SRQ_HOLDTD	0x0040		/* Return holding td lock */

constexpr uint16_t SRQ_PREEMPTED = static_cast<uint16_t>(0x0008);		/* has been preempted.. be kind */
constexpr uint16_t SRQ_BORROWING = static_cast<uint16_t>(0x0010); /* Priority updated due to prio_lend */	


  // current points to the CfsTask that we most recently picked to run on the
  // cpu. Note, we say most recently picked as a txn could fail leaving us with
  // current pointing to a task that is not currently on cpu.
  UleTask* current = nullptr;


  // pointer to the kernel ipc queue.
  std::unique_ptr<Channel> channel = nullptr;
  // the run queue responsible from scheduling tasks on this cpu.
  UleRunq tdq_realtime;/* (t) real-time run queue. */
  UleRunq tdq_timeshare;/* (t) timeshare run queue. */
  UleRunq tdq_idle;/* (t) Queue of IDLE threads. */

	int		tdq_load;	/* (ts) Aggregate load. */
	int		tdq_sysload;	/* (ts) For loadavg, !ITHD load. */
	int		tdq_cpu_idle;	/* (ls) cpu_idle() is active. */
	int		tdq_transferable; /* (ts) Transferable thread count. */
	short		tdq_switchcnt;	/* (l) Switches this tick. */
	short		tdq_oldswitchcnt; /* (l) Switches last tick. */
	u_char		tdq_lowpri;	/* (ts) Lowest priority thread. */
	u_char		tdq_owepreempt;	/* (f) Remote preemption pending. */
	u_char		tdq_idx;	/* (t) Current insert index. */
	u_char		tdq_ridx;	/* (t) Current removal index. */

  // ID of the cpu.
  int id = -1;
  mutable absl::Mutex mu_;

  protected:
  // Protects this runqueue and the state of any task assoicated with the rq.
  bool IsIdle() const { return current == nullptr; }
  
  //TODO: Enable if required
  // bool LocklessRqEmpty() const { return run_queue.LocklessSize() == 0; }

    /* Operations on per processor queues */
  UleTask* tdq_choose();
  void tdq_setup(int i);
  void tdq_load_add(UleTask *);
  void tdq_load_rem(UleTask *);
  __inline void tdq_runq_add(UleTask *, int);
  __inline void tdq_runq_rem(UleTask *);
  inline int sched_shouldpreempt(int, int, int);
  void tdq_print(int cpu);
  void runq_print(struct UleRunq *rq);
  int tdq_add(UleTask*, int);

  int tdq_move(struct CpuState *);
  int tdq_idled();
  void tdq_notify(struct tdq *, int lowpri);
  UleTask *tdq_steal(int);
  UleTask *runq_steal(UleRunq *, int);
  int sched_pickcpu(UleTask*, int);
  void sched_balance(void);
  bool sched_balance_pair(struct CpuState *);
  inline CpuState *sched_setcpu(UleTask *, int, int);
  inline void thread_unblock_switch(UleTask *, struct mtx *);
  
  //Can be considered later if we enable CPU topology
  // int sysctl_kern_sched_topology_spec(SYSCTL_HANDLER_ARGS);
  // int sysctl_kern_sched_topology_spec_internal(struct sbuf *sb, 
  // struct cpu_group *cg, int indent);

} ABSL_CACHELINE_ALIGNED;


