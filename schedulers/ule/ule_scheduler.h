
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
  
public:
  static constexpr int RQ_NQS	=	64;		/* Number of run queues. */
  static constexpr int RQ_PPQ	=	4;		/* Priorities per queue. */
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


  bool runq_check(); // Basically isEmpty - but uses rq_status


 private:
  inline void runq_setbit(int pri) {
    rq_status |= (1ul<< (pri & 63));
  }
  inline void runq_clrbit(int pri) {
    rq_status &= ~(1ul<< (pri & 63));
  }
  inline int runq_findbit() {
    if (rq_status != 0) {
      return __builtin_ctzl(rq_status);
    }
    return -1;
  }
  inline int runq_findbit_from(int pri) {
    uint64_t mask = ((uint64_t)-1) << (pri & 63);
    mask = rq_status & mask;
    if (mask == 0)
			return -1;
		return __builtin_ctzl(mask);
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
  int		td_rqindex = -1;	/* (t) Run queue index. */
  u_char		td_base_pri = 0;	/* (t) Thread base kernel priority. */
	u_char		td_priority = 0;	/* (t) Thread active priority. */
	u_char		td_pri_class = 0;	/* (t) Scheduling class. */
	u_char		td_user_pri = 0;	/* (t) User pri from estcpu and nice. */
	u_char		td_base_user_pri = 0; /* (t) Base user pri */
  u_char		td_lend_user_pri = 0; /* (t) Lend user pri. */
  int nice = 0;
  
	int		td_flags = 0;	/* (t) TDF_* flags. */
	int		td_inhibitors = 0;	/* (t) Why can not run. */

  enum td_states {
		TDS_INACTIVE = 0x0,
		TDS_INHIBITED, // Cannot run, reason could be any of TDI_ flags
		TDS_CAN_RUN,
		TDS_RUNQ,
		TDS_RUNNING,
    TDS_FINISHED
	} td_state = TDS_INACTIVE;			/* (t) thread state */
  int		td_pinned = 0;	/* (k) Temporary cpu pin count. */

  // Fields from struct thread>
  static constexpr u_int SCHED_INTERACT_THRESH	= 30;
  static constexpr u_int	SCHED_INTERACT_MAX	= 100;
  static constexpr u_int	SCHED_INTERACT_HALF	=  (SCHED_INTERACT_MAX / 2);
  static constexpr u_int sched_interact = SCHED_INTERACT_THRESH;
  
  // <Fields from struct td_sched
  // TODO: Decide which ones are relevant later
  UleRunq	*ts_runq = nullptr;	/* Run-queue we're queued on. */
	short		ts_flags = 0;	/* TSF_* flags. */
	int		ts_cpu = 0;		/* CPU that we have affinity for. */
	int		ts_rltick = 0;	/* Real last tick, for affinity. */
	int		ts_slice = 0;	/* Ticks of slice remaining. */
	u_int		ts_slptime = 0;	/* Number of ticks we vol. slept */
	u_int		ts_runtime = 0;	/* Number of ticks we were running */
	int		ts_ltick = 0;	/* Last tick that we were running on */
	int		ts_ftick = 0;	/* First tick that we were running on */
	int		ts_ticks = 0;	/* Tick count */

  /*
  * Priority classes.
  */
  static constexpr int	PRI_ITHD = 1;	/* Interrupt thread. */
  static constexpr int	PRI_REALTIME	= 2;	/* Real time process. */
  static constexpr int	PRI_TIMESHARE	= 3;	/* Time sharing process. */
  static constexpr int PRI_IDLE	=4;	/* Idle process. */

  /*
  * PRI_FIFO is POSIX.1B SCHED_FIFO.
  */

  static constexpr int	PRI_FIFO_BIT	=	8;
  static constexpr int	PRI_FIFO	=(PRI_FIFO_BIT | PRI_REALTIME);

  // #define	PRI_NEED_RR(P)		((P) != PRI_FIFO)

    // <Fields from struct td_sched > 
  public: 
  inline int basePriority(){
    return ((td_pri_class) & ~PRI_FIFO_BIT);
  }
  inline bool isRealTime(){
    return (this->basePriority() == PRI_REALTIME);
  }
  int sched_interact_score();
  void sched_priority();
  void sched_user_prio(u_char prio);
};

struct CpuState {
  static constexpr int PRIO_MIN = -20;
  static constexpr int PRIO_MAX = 20;


  /* flags kept in ts_flags */
  static constexpr int	TSF_BOUND=0x0001;		/* Thread can not migrate. */
  static constexpr int TSF_XFERABLE=0x0002;		/* Thread was added as transferable. */

  /* Lockless accessors. */
  #define	TDQ_SWITCHCNT(tdq)	(atomic_load_short(&(tdq)->tdq_switchcnt) + \
          atomic_load_short(&(tdq)->tdq_oldswitchcnt))
  #define	TDQ_SWITCHCNT_INC(tdq)	(atomic_store_short(&(tdq)->tdq_switchcnt, \
          atomic_load_short(&(tdq)->tdq_switchcnt) + 1))

  #define	TDQ_SELF()	((struct CpuState *)PCPU_GET(sched))
  #define	TDQ_CPU(x)	(DPCPU_ID_PTR((x), tdq))

  //static CpuState	tdq_cpu;


  static constexpr int MA_OWNED =9;

  #define	TDQ_LOCK_ASSERT(t, type)	mtx_assert(TDQ_LOCKPTR((t)), (type))
  #define	TDQ_LOCK(t)		mtx_lock_spin(TDQ_LOCKPTR((t)))
  #define	TDQ_LOCK_FLAGS(t, f)	mtx_lock_spin_flags(TDQ_LOCKPTR((t)), (f))
  #define	TDQ_TRYLOCK(t)		mtx_trylock_spin(TDQ_LOCKPTR((t)))
  #define	TDQ_TRYLOCK_FLAGS(t, f)	mtx_trylock_spin_flags(TDQ_LOCKPTR((t)), (f))
  #define	TDQ_UNLOCK(t)		mtx_unlock_spin(TDQ_LOCKPTR((t)))

#define	TD_SET_STATE(td, state)	(td)->td_state = state


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
static constexpr int	PRI_MIN = 0;		/* Highest priority. */
static constexpr int	PRI_MAX = 255;		/* Lowest priority. */
static constexpr int PRI_MIN_TIMESHARE=88;
static constexpr int PRI_MIN_IDLE = 224;
static constexpr int PRI_MAX_TIMESHARE=PRI_MIN_IDLE - 1;
static constexpr int	PRI_MAX_BATCH =		PRI_MAX_TIMESHARE;
static constexpr int SCHED_PRI_NRESV =	(PRIO_MAX - PRIO_MIN);
static constexpr int	PRI_TIMESHARE_RANGE= (PRI_MAX_TIMESHARE - PRI_MIN_TIMESHARE + 1);
static constexpr int	PRI_INTERACT_RANGE =	((PRI_TIMESHARE_RANGE - SCHED_PRI_NRESV) / 2);
static constexpr int	PRI_MAX_INTERACT =	(PRI_MIN_TIMESHARE + PRI_INTERACT_RANGE - 1);
static constexpr int	PRI_MIN_BATCH =		(PRI_MIN_TIMESHARE + PRI_INTERACT_RANGE);

static constexpr int SCHED_PRI_NHALF =	(SCHED_PRI_NRESV / 2);
static constexpr int SCHED_PRI_MIN = PRI_MIN_BATCH + SCHED_PRI_NHALF;
static constexpr int SCHED_PRI_MAX =	PRI_MAX_BATCH - SCHED_PRI_NHALF;
static constexpr int SCHED_PRI_RANGE =	SCHED_PRI_MAX - SCHED_PRI_MIN + 1;
static constexpr int	PRI_MIN_INTERACT =	PRI_MIN_TIMESHARE;
static constexpr int PRI_MAX_IDLE	= PRI_MAX;
static constexpr int	PRI_BATCH_RANGE	= (PRI_TIMESHARE_RANGE - PRI_INTERACT_RANGE);
static constexpr int	PUSER	= (PRI_MIN_TIMESHARE);
static constexpr int	SCHED_TICK_SHIFT= 10;
//TODO: find the hz value
static constexpr int hz=10;
#define	SCHED_TICK_HZ(ts)	((ts)->ts_ticks >> SCHED_TICK_SHIFT)
#define	SCHED_TICK_TOTAL(ts)	(std::max((ts)->ts_ltick - (ts)->ts_ftick, hz))
#define roundup(x, y)   ((((x) % (y)) == 0) ? \
	                (x) : ((x) + ((y) - ((x) % (y)))))

static constexpr int 	PRI_MIN_KERN = 48;
static constexpr int preempt_thresh = PRI_MIN_KERN;

static constexpr int	TDF_INMEM = 0x00000004; /* Thread's stack is in memory. */
static constexpr int	TDF_NOLOAD=0x00040000; /* Ignore during load avg calculations. */

  // current points to the CfsTask that we most recently picked to run on the
  // cpu. Note, we say most recently picked as a txn could fail leaving us with
  // current pointing to a task that is not currently on cpu.
  UleTask* tdq_curthread = nullptr;

/*
 * These parameters determine the slice behavior for batch work.
 */
  static constexpr int	SCHED_SLICE_DEFAULT_DIVISOR =	10;	/* ~94 ms, 12 stathz ticks. */
 static constexpr int	SCHED_SLICE_MIN_DIVISOR	= 6;	/* DEFAULT/MIN = ~16 ms. */

  // sched_slice:		Runtime of each thread before rescheduling.
  static constexpr int sched_slice = 10;	/* reset during boot. */
  static constexpr int sched_slice_min=1;	/* reset during boot. */

  // pointer to the kernel ipc queue.
  std::unique_ptr<Channel> channel = nullptr;

  
  // the run queue responsible from scheduling tasks on this cpu.
  UleRunq tdq_realtime;/* (t) real-time run queue. */
  UleRunq tdq_timeshare;/* (t) timeshare run queue. */
  UleRunq tdq_idle;/* (t) Queue of IDLE threads. */

	int		tdq_load = 0;	/* (ts) Aggregate load. */
	int		tdq_sysload = 0;	/* (ts) For loadavg, !ITHD load. */
	int		tdq_cpu_idle = 0;	/* (ls) cpu_idle() is active. */
	int		tdq_transferable = 0; /* (ts) Transferable thread count. */
	short		tdq_switchcnt = 0;	/* (l) Switches this tick. */
	short		tdq_oldswitchcnt = 0; /* (l) Switches last tick. */
	u_char		tdq_lowpri = 0; // TODO:Fix this	/* (ts) Lowest priority thread. */
	u_char		tdq_owepreempt = 0;	/* (f) Remote preemption pending. */
	u_char		tdq_idx = 0;	/* (t) Current insert index. */
	u_char		tdq_ridx = 0;	/* (t) Current removal index. */

  // ID of the cpu.
  int tdq_id = -1;
  mutable absl::Mutex tdq_lock;

    /* Operations on per processor queues */
  UleTask* tdq_choose();
  void tdq_setup(int);
  inline int tdq_slice();
  void tdq_load_add(UleTask *);
  void tdq_load_rem(UleTask *);
  __inline void tdq_runq_add(UleTask *, int);
  __inline void tdq_runq_rem(UleTask *);
  inline int sched_shouldpreempt(int, int, int);  
  int tdq_add(UleTask*, int);
  void tdq_setlowpri(UleTask *ctd);

  bool tdq_isempty();
  int tdq_move(CpuState *);
  int tdq_idled();
  void tdq_notify(CpuState *, int);
  UleTask *tdq_steal(int);
  UleTask *runq_steal(UleRunq *, int);
  int sched_pickcpu(UleTask*, int);
  void sched_balance(void);
  bool sched_balance_pair(struct CpuState *);
  inline CpuState *sched_setcpu(UleTask *, int, int);
  inline void thread_unblock_switch(UleTask *, struct mtx *);

  static int getSchedPriTicks(UleTask* ts){						
    return (SCHED_TICK_HZ((ts))/(roundup(SCHED_TICK_TOTAL((ts)), SCHED_PRI_RANGE) / SCHED_PRI_RANGE));
  }
  
  //Can be considered later if we enable CPU topology
  // int sysctl_kern_sched_topology_spec(SYSCTL_HANDLER_ARGS);
  // int sysctl_kern_sched_topology_spec_internal(struct sbuf *sb, 
  // struct cpu_group *cg, int indent);

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
    Channel* channel = cpu_state(cpu)->channel.get();
    CHECK_NE(channel, nullptr);
    return *channel;
  }

  bool Empty(const Cpu& cpu) {
    CpuState* cs = cpu_state(cpu);
    absl::MutexLock l(&cs->tdq_lock);
    bool res = cs->tdq_isempty();
    return res;
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
  void sched_thread_priority(UleTask* td, u_char prio);
  void sched_add(UleTask *td, int flags);
  void sched_rem(UleTask *td);


 private:

  UleTask* sched_choose(CpuState *tdq);

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

  // HandleTaskDone is responsible for removing a task from the run queue and
  // freeing it if it is currently !cs->current, otherwise, it defers the
  // freeing to PickNextTask.
  // Note: Should be called with this CPU's rq mutex lock held.
  void HandleTaskDone(UleTask* task);

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

  CpuState* cpu_state(const Cpu& cpu) { return &cpu_states_[cpu.id()]; }

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

  CpuState cpu_states_[MAX_CPUS];
  Channel* default_channel_ = nullptr;

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

