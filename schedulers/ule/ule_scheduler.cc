
#include "schedulers/ule/ule_scheduler.h"

#include <sys/timerfd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>

#include "absl/functional/any_invocable.h"
#include "absl/numeric/int128.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "lib/agent.h"
#include "lib/logging.h"
#include "lib/topology.h"

#define DPRINT_Ule(level, message)                               \
  do {                                                           \
    if (ABSL_PREDICT_TRUE(verbose() < level)) break;             \
    absl::FPrintF(stderr, "DUle: [%.6f] cpu %d: %s\n",           \
                  absl::ToDoubleSeconds(MonotonicNow() - start), \
                  sched_getcpu(), message);                      \
  } while (0)

// TODO: Remove this flag after we test idle load balancing
// thoroughly.
ABSL_FLAG(bool, experimental_enable_idle_load_balancing, true,
          "Experimental flag to enable idle load balancing.");


namespace ghost {

UleRunq::UleRunq() {
  rq_status = 0;
  CHECK(runq_[0].empty());
}

bool UleRunq::runq_check() {
  return rq_status != 0;
}


void UleRunq::runq_add(UleTask *task, int flags) {
	int pri = task->td_priority / RQ_PPQ;
	task->td_rqindex = pri;
	runq_setbit(pri);
	if (flags & SRQ_PREEMPTED) {
		runq_[pri].push_front(task);
    task->td_runq = runq_[pri].begin();
	} else {
		runq_[pri].push_back(task);
    task->td_runq = std::prev(runq_[pri].end());
	}
}
void UleRunq::runq_add_pri(UleTask *task, u_char pri, int flags) {
	CHECK(pri < RQ_NQS);
	task->td_rqindex = pri;
	runq_setbit(pri);
	if (flags & SRQ_PREEMPTED) {
		runq_[pri].push_front(task);
    task->td_runq = runq_[pri].begin();
	} else {
		runq_[pri].push_back(task);
    task->td_runq = std::prev(runq_[pri].end());
	}
}

UleTask* UleRunq::runq_choose()
{
	std::list<UleTask*> *rqh;
	UleTask *td;
	int pri;

	if ((pri = runq_findbit()) != -1) {
		rqh = &runq_[pri];
		td = rqh->front();
		CHECK(td != NULL);
		
		return (td);
	}
	return NULL;
}

UleTask* UleRunq::runq_choose_from(u_char idx)
{
	std::list<UleTask*> *rqh;
	UleTask *td;
  int pri;
	if ((pri = runq_findbit_from(idx)) != -1) {
		rqh = &runq_[pri];
		td = rqh->front();
		CHECK(td != NULL);
		return (td);
	}

	return NULL;
}

void UleRunq::runq_remove(UleTask *td)
{
	runq_remove_idx(td, NULL);
}

void UleRunq::runq_remove_idx(UleTask *td, u_char *idx)
{
	std::list<UleTask*> *rqh;
	u_char pri;

  // TODO: Check later if these checks make sense
	// CHECK(td->td_flags & TDF_INMEM); // "runq_remove_idx: thread swapped out"
	pri = td->td_rqindex;
	CHECK(pri < RQ_NQS); // ("runq_remove_idx: Invalid index %d\n", pri));
	rqh = &runq_[pri];
  	runq_[pri].erase(td->td_runq);
	td->td_rqindex = -1; // A way to denote invalid iterator
	if (rqh->empty()) {
		//CTR0(KTR_RUNQ, "runq_remove_idx: empty");
		runq_clrbit(pri);
		if (idx != NULL && *idx == pri)
			*idx = (pri + 1) % RQ_NQS;
	}
}

/*
 * This is the core of the interactivity algorithm.  Determines a score based
 * on past behavior.  It is the ratio of sleep time to run time scaled to
 * a [0, 100] integer.  This is the voluntary sleep time of a process, which
 * differs from the cpu usage because it does not account for time spent
 * waiting on a run-queue.  Would be prettier if we had floating point.
 *
 * When a thread's sleep time is greater than its run time the
 * calculation is:
 *
 *                           scaling factor
 * interactivity score =  ---------------------
 *                        sleep time / run time
 *
 *
 * When a thread's run time is greater than its sleep time the
 * calculation is:
 *
 *                                                 scaling factor
 * interactivity score = 2 * scaling factor  -  ---------------------
 *                                              run time / sleep time
 */
int UleTask::sched_interact_score()
{
	int div;

	/*
	 * The score is only needed if this is likely to be an interactive
	 * task.  Don't go through the expense of computing it if there's
	 * no chance.
	 */
	if (sched_interact <= SCHED_INTERACT_HALF &&
		ts_runtime >= ts_slptime)
			return (SCHED_INTERACT_HALF);

	if (ts_runtime > ts_slptime) {
		div = std::max(u_int(1), ts_runtime / SCHED_INTERACT_HALF);
		return (SCHED_INTERACT_HALF +
		    (SCHED_INTERACT_HALF - (ts_slptime / div)));
	}
	if (ts_slptime > ts_runtime) {
		div = std::max(u_int(1), ts_slptime / SCHED_INTERACT_HALF);
		return (ts_runtime / div);
	}
	/* runtime == slptime */
	if (ts_runtime)
		return (SCHED_INTERACT_HALF);

	/*
	 * This can happen if slptime and runtime are 0.
	 */
	return (0);
}

/*
 * Scale the scheduling priority according to the "interactivity" of this
 * process.
 */
void UleTask::sched_priority()
{
	u_int pri, score;

	if (this->basePriority() != PRI_TIMESHARE)
		return;
	/*
	 * If the score is interactive we place the thread in the realtime
	 * queue with a priority that is less than kernel and interrupt
	 * priorities.  These threads are not subject to nice restrictions.
	 *
	 * Scores greater than this are placed on the normal timeshare queue
	 * where the priority is partially decided by the most recent cpu
	 * utilization and the rest is decided by nice value.
	 *
	 * The nice value of the process has a linear effect on the calculated
	 * score.  Negative nice values make it easier for a thread to be
	 * considered interactive.
	 */
	score = std::max(0, this->sched_interact_score()+nice);
	if (score < sched_interact) {
		pri = CpuState::PRI_MIN_INTERACT;
		pri += (CpuState::PRI_MAX_INTERACT - CpuState::PRI_MIN_INTERACT + 1) * score /
		    sched_interact;
		CHECK(pri >= CpuState::PRI_MIN_INTERACT && pri <= CpuState::PRI_MAX_INTERACT);
	} else {
		pri = CpuState::SCHED_PRI_MIN;
		if (ts_ticks)
			pri += std::min(CpuState::getSchedPriTicks(this),CpuState::SCHED_PRI_RANGE - 1);
		pri += nice;
		CHECK(pri >= CpuState::PRI_MIN_BATCH && pri <= CpuState::PRI_MAX_BATCH);
	}
	this->sched_user_prio(pri);
	return;
}

/*
 * Set the base user priority, does not effect current running priority.
 */
void UleTask::sched_user_prio(u_char prio)
{
	td_base_user_pri = prio;
	if (td_lend_user_pri <= prio)
		return;
	td_user_pri = prio;
}


void PrintDebugTaskMessage(std::string message_name, CpuState* cs,
                           UleTask* task) {
  DPRINT_Ule(2, absl::StrFormat(
                    "[%s]: %s with state %s, %scurrent", message_name,
                    task->gtid.describe(),
                    task->td_state,
                    (cs && cs->tdq_curthread == task) ? "" : "!"));
}

UleScheduler::UleScheduler(Enclave* enclave, CpuList cpulist,
                           std::shared_ptr<TaskAllocator<UleTask>> allocator,
                           absl::Duration min_granularity,
                           absl::Duration latency)
    : BasicDispatchScheduler(enclave, std::move(cpulist), std::move(allocator)),
      idle_load_balancing_(
          absl::GetFlag(FLAGS_experimental_enable_idle_load_balancing)) {
	for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    cs->tdq_id = cpu.id();

    {
      //absl::MutexLock l(&cs->run_queue .mu_);
    //   cs->run_queue.SetMinGranularity(min_granularity_);
    //   cs->run_queue.SetLatency(latency_);
    }

    cs->channel = enclave->MakeChannel(GHOST_MAX_QUEUE_ELEMS, cpu.numa_node(),
                                       MachineTopology()->ToCpuList({cpu}));
    // This channel pointer is valid for the lifetime of CfsScheduler
    if (!default_channel_) {
      default_channel_ = cs->channel.get();
    }
  }


}

void UleScheduler::DumpAllTasks() {

}



void UleScheduler::EnclaveReady() {
  for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    Agent* agent = enclave()->GetAgent(cpu);

    // AssociateTask may fail if agent barrier is stale.
    while (!cs->channel->AssociateTask(agent->gtid(), agent->barrier(),
                                       /*status=*/nullptr)) {
      CHECK_EQ(errno, ESTALE);
    }
  }

  // Enable tick msg delivery here instead of setting AgentConfig.tick_config_
  // because the agent subscribing the default channel (mostly the
  // channel/agent for the front CPU in the enclave) can get CpuTick messages
  // for another CPU in the enclave while this function is trying to associate
  // each agent to its corresponding channel.
  enclave()->SetDeliverTicks(true);
}

// The in kernel SelectTaskRq attempts to do the following:
// - If sched_energy_enabled(), find an energy efficient CPU (not applicable to
// us)
// - If the affine flag is set, walks up the sched domain tree to see if we can
// find a cpu in the same domain as our previous cpu, but that will allow us to
// run sonner
// - If the above two fail, then we find the idlest cpu within the highest level
// sched domain assuming the sd_flag is on
// - If the above fails, we try to find the most idle core inside the same LLC
// assuming WF_TTWU is set
// - Otherwise fallback to the old cpu
// Our Ule agent has no notion of energy efficiency or scheduling domaims. So,
// we can simplify our algorithm to:
// - Check if the current CPU is idle, if so, place it there (this avoids a
// ping)
// - Otherwise, check if our prev_cpu is idle.
// - Otherwise, try to find an idle CPU in the L3 sibiling list of our prev_cpu
// - Otherwise, just use the least utilized CPU
// In general, there are many, many, many heuristic in kernel Ule, so I tried to
// just grab the general idea and translate it to ghost code. In the future, we
// will probably end up tweaking this code.
// TODO: We probably want to favor placing in a L3 cache sibling even if
// there is no idle sibling. To do this, we can introduce a load_bias variable,
// where we consider < load_bias load to be idle.
// TODO: Collect some data about placing on this cpu if idle vs an idle
// L3 sibling.
// TODO: Once we add nice values and possibly a cgroup interface, we
// need to update our load calculating logic from .Size() to something more
// robust.
// NOTE: This is inherently racy, since we only synchronize on individual rq's
// we are not guaranteed to see a consistent view of rq loads.


void UleScheduler::StartMigrateTask(UleTask* task) {
  
}

void UleScheduler::StartMigrateCurrTask() {

}





void UleScheduler::TaskNew(UleTask* task, const Message& msg) {
  	const ghost_msg_payload_task_new* payload =
      static_cast<const ghost_msg_payload_task_new*>(msg.payload());


  GHOST_DPRINT(3, stderr, "TaskNew: task = %p", task);
	task->ts_cpu = MyCpu();
	CpuState *cs = &cpu_states_[task->ts_cpu];
	task->nice=payload->nice;
	task->sched_priority();
	task->seqnum = msg.seqnum();
	if (payload->runnable) {
		PrintDebugTaskMessage("TaskNew: ",cs, task);
		task->td_state = UleTask::TDS_CAN_RUN;
		/*
		* Recalculate the priority before we select the target cpu or
		* run-queue.
		*/
		if (task->basePriority() == UleTask::PRI_TIMESHARE)
			task->sched_priority();
		cs->tdq_add(task, 0);
	}
}

void UleScheduler::TaskRunnable(UleTask* task, const Message& msg) {
  const ghost_msg_payload_task_new* payload =
      static_cast<const ghost_msg_payload_task_new*>(msg.payload());

  // TODO: Adjust time slices stats

  GHOST_DPRINT(3, stderr, "TaskRunnable: task = %p", task);
	task->ts_cpu = MyCpu();
	CpuState *cs = &cpu_states_[task->ts_cpu];
  	cs->tdq_lock.AssertHeld();
    // If this is our current task, then we will defer its proccessing until
	// PickNextTask. Otherwise, use the normal wakeup logic.
	if (task->ts_cpu >= 0) {
		if (cs->tdq_curthread == task) {
		cs->tdq_curthread = nullptr;
		}
	}

	if (payload->runnable) {
		PrintDebugTaskMessage("TaskRunnable: ", cs, task);
		task->td_state = UleTask::TDS_CAN_RUN;
		/*
		* Recalculate the priority before we select the target cpu or
		* run-queue.
		*/
		if (task->basePriority() == UleTask::PRI_TIMESHARE)
			task->sched_priority();
		cs->tdq_add(task, 0);
	}
}

// Disable thread safety analysis as this function is called with rq lock held
// but it's hard for the compiler to infer. Without this annotation, the
// compiler raises safety analysis error.
void UleScheduler::HandleTaskDone(UleTask* task) {
  CpuState* cs = &cpu_states_[task->ts_cpu];
  cs->tdq_lock.AssertHeld();

  if (cs->tdq_curthread != task) {
	if (task->td_rqindex != -1) {
		cs->tdq_runq_rem(task);
	}
  	allocator()->FreeTask(task);
  } else {
	task->td_state = UleTask::TDS_FINISHED;
  }

}

void UleScheduler::TaskDeparted(UleTask* task, const Message& msg) {
  const ghost_msg_payload_task_departed* payload =
      static_cast<const ghost_msg_payload_task_departed*>(msg.payload());
  CpuState *tdq = &cpu_states_[task->ts_cpu];
  PrintDebugTaskMessage("TaskDeparted", tdq, task);
  tdq->tdq_lock.AssertHeld();
  CHECK(payload->from_switchto == false);
  HandleTaskDone(task);
}

void UleScheduler::TaskDead(UleTask* task, const Message& msg) {
  CpuState *cs = &cpu_states_[task->ts_cpu];
  PrintDebugTaskMessage("TaskDead", cs, task);
  cs->tdq_lock.AssertHeld();

  HandleTaskDone(task);
}

void UleScheduler::TaskYield(UleTask* task, const Message& msg) {
	const ghost_msg_payload_task_yield* payload =
	static_cast<const ghost_msg_payload_task_yield*>(msg.payload());
	Cpu cpu = topology()->cpu(MyCpu());
	CpuState* cs = &cpu_states_[task->ts_cpu];
	PrintDebugTaskMessage( "TaskYield: ",cs , task);
	cs->tdq_lock.AssertHeld();

	// If this task is not from a switchto chain, it should be the current task on
	// this CPU.
	if (!payload->from_switchto) {
		CHECK_EQ(cs->tdq_curthread, task);
	}

	// Updates the task state accordingly. This is safe because this task should
	// be associated with this CPU's agent and protected by this CPU's RQ lock.
	PutPrevTask(task);

	// This task was the last task in a switchto chain on a remote CPU. We should
	// ping the remote CPU to schedule a new task.
	if (payload->cpu != cpu.id()) {
		CHECK(payload->from_switchto);
		PingCpu(topology()->cpu(payload->cpu));
	}
}

void UleScheduler::TaskBlocked(UleTask* task, const Message& msg) {
    const ghost_msg_payload_task_blocked* payload =
      static_cast<const ghost_msg_payload_task_blocked*>(msg.payload());
	Cpu cpu = topology()->cpu(MyCpu());
	CpuState* cs = cpu_state(cpu);
	PrintDebugTaskMessage( "TaskBlocked: ", cs, task);
	cs->tdq_lock.AssertHeld();

	// If this task is not from a switchto chain, it should be the current task on
	// this CPU.
	if (!payload->from_switchto) {
		CHECK_EQ(cs->tdq_curthread, task);
	}

	if (cs->tdq_curthread == task) {
		cs->tdq_curthread = nullptr;
	}

	task->td_state= UleTask::TDS_INHIBITED;
}

void UleScheduler::TaskPreempted(UleTask* task, const Message& msg) {
  const ghost_msg_payload_task_preempt* payload =
      static_cast<const ghost_msg_payload_task_preempt*>(msg.payload());
  
  CpuState* cs = &cpu_states_[task->ts_cpu];
  PrintDebugTaskMessage( "TaskPreempted: ", cs, task);
  cs->tdq_lock.AssertHeld();

  // If this task is not from a switchto chain, it should be the current task on
  // this CPU.
  if (!payload->from_switchto) {
    CHECK_EQ(cs->tdq_curthread, task);
  }

  // Updates the task state accordingly. This is safe because this task should
  // be associated with this CPU's agent and protected by this CPU's RQ lock.
  PutPrevTask(task);

}

// Should never be called
void UleScheduler::TaskSwitchto(UleTask* task, const Message& msg) {
  CHECK(false);
}

// Disable thread safety analysis as this function is called with rq lock held
// but it's hard for the compiler to infer. Without this annotation, the
// compiler raises safety analysis error.
void UleScheduler::CheckPreemptTick(const Cpu& cpu) {

}

void UleScheduler::PutPrevTask(UleTask* task) {
	CpuState* cs = &cpu_states_[MyCpu()];
	cs->tdq_lock.AssertHeld();

	CHECK_NE(task, nullptr);

	// If this task is currently running, kick it off-cpu.
	if (cs->tdq_curthread == task) {
		cs->tdq_curthread = nullptr;
	}

	// We have a notable deviation from the upstream's behavior here. In upstream,
	// put_prev_task does not update the state, while we update the state here.
	task->td_state = UleTask::TDS_CAN_RUN;

	/*
	 * Recalculate the priority before we select the target cpu or
	 * run-queue.
	 */
	if (task->basePriority() == UleTask::PRI_TIMESHARE)
		task->sched_priority();
	cs->tdq_add(task,0);
}

void UleScheduler::CpuTick(const Message& msg) {

}

UleTask* UleScheduler::sched_choose(CpuState *tdq) {
  UleTask *td = tdq->tdq_choose();
	if (td != NULL) {
		tdq->tdq_runq_rem(td);
		tdq->tdq_lowpri = td->td_priority;
	} else { 
		tdq->tdq_lowpri = CpuState::PRI_MAX_IDLE;
		td = nullptr;
	}
	tdq->tdq_curthread = td;
	return (td);
}


//-----------------------------------------------------------------------------
// Load Balance
//-----------------------------------------------------------------------------



//-----------------------------------------------------------------------------
// Schedule
//-----------------------------------------------------------------------------

void UleScheduler::UleSchedule(const Cpu& cpu, BarrierToken agent_barrier,
                               bool prio_boost) {
  RunRequest* req = enclave()->GetRunRequest(cpu);
  CpuState* cs = cpu_state(cpu);

  UleTask* prev = cs->tdq_curthread;
  GHOST_DPRINT(3, stderr, "UleSchedule: prev = %p", prev);

  if (prio_boost) {
    // If we are currently running a task, we need to put it back onto the
    // queue.
    if (prev) {
      absl::MutexLock l(&cs->tdq_lock);
      switch (prev->td_state) {
        // case CfsTaskState::State::kNumStates:
        //   CHECK(false);
        //   break;
        case UleTask::TDS_INHIBITED:
          break;
        // case CfsTaskState::State::kDone:
        //   cs->run_queue.DequeueTask(prev);
        //   allocator()->FreeTask(prev);
        //   break;
        case UleTask::TDS_CAN_RUN:
          // This case exclusively handles a task yield:
          // - TaskYield: task->state goes from kRunning -> kRunnable
          // - PickNextTask: we need to put the task back in the rq.
          cs->tdq_runq_add(prev, 0);
          break;
        case UleTask::TDS_RUNNING:
          cs->tdq_runq_add(prev, 0);
          prev->td_state = UleTask::TDS_CAN_RUN;
          break;
      }

      //cs->preempt_curr = false;
      cs->tdq_curthread = nullptr;
      // cs->run_queue.UpdateMinVruntime(cs); 
    }
    // If we are prio_boost'ed, then we are temporarily running at a higher
    // priority than (kernel) CFS. The purpose of this is so that we can
    // reconcile our state with the fact that any task we wanted to be running
    // on the CPU will no longer be running. In our case, since we only sync
    // up our CpuState in PickNextTask, we can simply RTLA yield. This works
    // because:
    // - We get prio_boosted
    // - We rtla yield
    // - eventually the cpu goes idle
    // - we go directly back into the scheduling loop (without consuming any
    // new messages as none will be generated).
    req->LocalYield(agent_barrier, RTLA_ON_IDLE);
    return;
  }

  cs->tdq_lock.Lock();
  if (prev && (prev->td_state == UleTask::TDS_RUNNING || prev->td_state == UleTask::TDS_CAN_RUN)) {
	cs->tdq_add(prev, 0);
  } 
  UleTask* next = sched_choose(cs);
  GHOST_DPRINT(3, stderr, "UleSchedule: next = %p", next);

  if (next) {
  	next->td_state = UleTask::TDS_RUNNING;
  }
  cs->tdq_curthread = next;
  cs->tdq_lock.Unlock();

  if (next) {
    DPRINT_Ule(3, absl::StrFormat("[%s]: Picked via sched_choose",
                                  next->gtid.describe()));

    req->Open({
        .target = next->gtid,
        .target_barrier = next->seqnum,
        .agent_barrier = agent_barrier,
        .commit_flags = COMMIT_AT_TXN_COMMIT,
    });

    // Although unlikely it's possible for an oncpu task to enter ghOSt on
    // any cpu. In this case there is a race window between producing the
    // MSG_TASK_NEW and getting off that cpu (a race that is exacerbated
    // by CFS dropping the rq->lock in PNT). During this window an agent
    // can observe the MSG_TASK_NEW on the default queue and because the
    // task is runnable it becomes a candidate to be put oncpu immediately.
    //
    // In this case we wait for `next` to fully get offcpu before trying
    // to Commit().
    while (next->status_word.on_cpu()) {
      Pause();
    }

    uint64_t before_runtime = next->status_word.runtime();
    if (req->Commit()) {
      GHOST_DPRINT(3, stderr, "Task %s oncpu %d", next->gtid.describe(),
                   cpu.id());
      // Update task's vruntime, which is the physical runtime multiplied by
      // the inverse of the weight for the task's nice value. We additionally
      // divide the product by 2^22 (right shift by 22 bits) to make a nice
      // value 0's vruntime equal to the wall runtime. This is because the
      // pre-computed weight values are scaled up by 2^10 (the load weight for
      // nice value = 0 becomes 1024). The weight values then get inverted
      // (which turns scale-up to scale-down) and scaled up by 2^32 to
      // pre-compute their inverse weights, leaving us the final scale up of
      // 2^22.
      //
      // i.e., vruntime = wall_runtime / (precomputed_weight / 2^10)
      //         = wall_runtime * 2^10 / precomputed_weight
      //         = wall_runtime * 2^10 / (2^32 / precomputed_inverse_weight)
      //         = wall_runtime * precomputed_inverse_weight / 2^22

      // TODO: Update this
      // uint64_t runtime = next->status_word.runtime() - before_runtime;
      // next->vruntime += absl::Nanoseconds(static_cast<uint64_t>(
      //     static_cast<absl::uint128>(next->inverse_weight) * runtime >> 22));
    } else {
      GHOST_DPRINT(3, stderr, "UleSchedule: commit failed (state=%d)",
                   req->state());
      // If our transaction failed, it is because our agent was stale.
      // Processing the remaining messages will bring our view up to date.
      // Since only the last state of cs->current matters, it is okay to keep
      // cs->current as what was picked by PickNextTask.
    }
  } else {
    req->LocalYield(agent_barrier, 0);
  }
}



void UleScheduler::Schedule(const Cpu& cpu, const StatusWord& agent_sw) {
  BarrierToken agent_barrier = agent_sw.barrier();
  CpuState* cs = cpu_state(cpu);

  GHOST_DPRINT(3, stderr, "Schedule: agent_barrier[%d] = %d\n", cpu.id(),
               agent_barrier);

  Message msg;
  {
    absl::MutexLock l(&cs->tdq_lock);
    while (!(msg = Peek(cs->channel.get())).empty()) {
      DispatchMessage(msg);
      Consume(cs->channel.get(), msg);
    }
  }
//   MigrateTasks(cs);
  UleSchedule(cpu, agent_barrier, agent_sw.boosted_priority());
}

void UleScheduler::PingCpu(const Cpu& cpu) {
  Agent* agent = enclave()->GetAgent(cpu);
  if (agent) {
    agent->Ping();
  }
}

// Disable thread safety analysis as this function is called with rq lock held
// but it's hard for the compiler to infer. Without this annotation, the
// compiler raises safety analysis error.
void UleScheduler::TaskAffinityChanged(UleTask* task, const Message& msg) {
  
}

void UleScheduler::TaskPriorityChanged(UleTask* task, const Message& msg) { 
}

std::unique_ptr<UleScheduler> MultiThreadedUleScheduler(
    Enclave* enclave, CpuList cpulist, absl::Duration min_granularity,
    absl::Duration latency) {
  auto allocator = std::make_shared<ThreadSafeMallocTaskAllocator<UleTask>>();
  auto scheduler = std::make_unique<UleScheduler>(enclave, std::move(cpulist),
                                                  std::move(allocator),
                                                  min_granularity, latency);
  return scheduler;
}

void UleAgent::AgentThread() {
  gtid().assign_name("Agent:" + std::to_string(cpu().id()));
  if (verbose() > 1) {
    printf("Agent tid:=%d\n", gtid().tid());
  }
  SignalReady();
  WaitForEnclaveReady();

  PeriodicEdge debug_out(absl::Seconds(1));

  while (!Finished() || !scheduler_->Empty(cpu())) {
    scheduler_->Schedule(cpu(), status_word());

    if (verbose() && debug_out.Edge()) {
      static const int flags = verbose() > 1 ? Scheduler::kDumpStateEmptyRQ : 0;
      if (scheduler_->debug_runqueue_) {
        scheduler_->debug_runqueue_ = false;
        scheduler_->DumpState(cpu(), Scheduler::kDumpAllTasks);
      } else {
        scheduler_->DumpState(cpu(), flags);
      }
    }
  }
  
}

bool CpuState::tdq_isempty() {
  return ((!tdq_realtime.runq_check()) || 
   (!tdq_timeshare.runq_check()) || (!tdq_idle.runq_check())) ;
}


/*
 * Add a thread to the actual run-queue.  Keeps transferable counts up to
 * date with what is actually on the run-queue.  Selects the correct
 * queue position for timeshare threads.
 */
__inline void CpuState::tdq_runq_add(UleTask *td, int flags)
{
	u_char pri;

  //TODO: modify this to assert for locks we are using 
	tdq_lock.AssertHeld();// TDQ_LOCK_ASSERT(this, MA_OWNED);
	// TODO: THREAD_LOCK_BLOCKED_ASSERT(td, MA_OWNED);

	pri = td->td_priority;
	TD_SET_STATE(td, UleTask::TDS_RUNQ);
	if (td->td_pinned == 0) { // Thread can migrate
		tdq_transferable++;
		td->ts_flags |= TSF_XFERABLE;
	}
	if (pri < PRI_MIN_BATCH) {
		td->ts_runq = &tdq_realtime;
	} else if (pri <= PRI_MAX_BATCH) {
		td->ts_runq = &tdq_timeshare;
		CHECK(pri <= PRI_MAX_BATCH && pri >= PRI_MIN_BATCH);
		/*
		 * This queue contains only priorities between MIN and MAX
		 * batch.  Use the whole queue to represent these values.
		 */
		if ((flags & (SRQ_BORROWING|SRQ_PREEMPTED)) == 0) {
			pri = UleRunq::RQ_NQS * (pri - PRI_MIN_BATCH) / PRI_BATCH_RANGE;
			pri = (pri + tdq_idx) % UleRunq::RQ_NQS;
			/*
			 * This effectively shortens the queue by one so we
			 * can have a one slot difference between idx and
			 * ridx while we wait for threads to drain.
			 */
			if (tdq_ridx != tdq_idx &&
			    pri == tdq_ridx)
				pri = (unsigned char)(pri - 1) % UleRunq::RQ_NQS;
		} else
			pri = tdq_ridx;
		td->ts_runq->runq_add_pri(td, pri, flags);
		return;
	} else
		td->ts_runq = &tdq_idle;
	td->ts_runq->runq_add(td, flags);
}

/*
 * Initialize a thread queue.
 */
void CpuState::tdq_setup(int id)
{
	this->tdq_id=id;
	//TODO: initialize mutex
}

inline int CpuState::sched_shouldpreempt(int pri, int cpri, int remote)
{
	/*
	 * If the new priority is not better than the current priority there is
	 * nothing to do.
	 */
	if (pri >= cpri)
		return (0);
	/*
	 * Always preempt idle.
	 */
	if (cpri >= PRI_MIN_IDLE)
		return (1);
	/*
	 * If preemption is disabled don't preempt others.
	 */
	if (preempt_thresh == 0)
		return (0);
	/*
	 * Preempt if we exceed the threshold.
	 */
	if (pri <= preempt_thresh)
		return (1);
	/*
	 * If we're interactive or better and there is non-interactive
	 * or worse running preempt only remote processors.
	 */
	if (remote && pri <= PRI_MAX_INTERACT && cpri > PRI_MAX_INTERACT)
		return (1);
	return (0);
}



/*
 * Bound timeshare latency by decreasing slice size as load increases.  We
 * consider the maximum latency as the sum of the threads waiting to run
 * aside from curthread and target no more than sched_slice latency but
 * no less than sched_slice_min runtime.
 */
inline int CpuState::tdq_slice(){
	int load;
		/*
	 * It is safe to use sys_load here because this is called from
	 * contexts where timeshare threads are running and so there
	 * cannot be higher priority load in the system.
	 */
	load = tdq_sysload - 1;
	if (load >= SCHED_SLICE_MIN_DIVISOR)
		return (sched_slice_min);
	if (load <= 1)
		return (sched_slice);
	return (sched_slice / load);
}

/* 
 * Remove a thread from a run-queue.  This typically happens when a thread
 * is selected to run.  Running threads are not on the queue and the
 * transferable count does not reflect them.
 */
__inline void CpuState::tdq_runq_rem(UleTask *ts)
{
	tdq_lock.AssertHeld();

	// TODO: THREAD_LOCK_BLOCKED_ASSERT(td, MA_OWNED);
	CHECK(ts->ts_runq != NULL);
	if (ts->ts_flags & TSF_XFERABLE) {
		tdq_transferable--;
		ts->ts_flags &= ~TSF_XFERABLE;
	}
	if (ts->ts_runq == & tdq_timeshare) {
		if (tdq_idx != tdq_ridx)
			ts->ts_runq->runq_remove_idx(ts, &tdq_ridx);
		else
			ts->ts_runq->runq_remove_idx(ts, NULL);
	} else
		ts->ts_runq->runq_remove(ts);
}

/*
 * Load is maintained for all threads RUNNING and ON_RUNQ.  Add the load
 * for this thread to the referenced thread queue.
 */
void CpuState::tdq_load_add(UleTask *td)
{

	tdq_lock.AssertHeld();//TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	//TODO: THREAD_LOCK_BLOCKED_ASSERT(td, MA_OWNED);

	tdq_load++;
	if ((td->td_flags & TDF_NOLOAD) == 0)
		tdq_sysload++;
}

/*
 * Remove the load from a thread that is transitioning to a sleep state or
 * exiting.
 */
void CpuState::tdq_load_rem(UleTask *td)
{
	tdq_lock.AssertHeld();//TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	//TODO: THREAD_LOCK_BLOCKED_ASSERT(td, MA_OWNED);
	CHECK(tdq_load != 0);

	tdq_load--;
	if ((td->td_flags & TDF_NOLOAD) == 0)
		tdq_sysload--;
}

/*
 * Pick the highest priority task we have and return it.
 */
UleTask* CpuState::tdq_choose()
{
	UleTask *td;
	tdq_lock.AssertHeld();
	td = tdq_realtime.runq_choose();
	if (td != NULL)
		return (td);
	td = tdq_timeshare.runq_choose_from(tdq_ridx);
	if (td != NULL) {
		CHECK(td->td_priority >= PRI_MIN_BATCH);
		return (td);
	}
	td = tdq_idle.runq_choose();
	if (td != NULL) {
		CHECK(td->td_priority >= PRI_MIN_IDLE);
		return (td);
	}
	return (NULL);
}
	

/*
 * Set lowpri to its exact value by searching the run-queue and
 * evaluating curthread.  curthread may be passed as an optimization.
 */
void CpuState::tdq_setlowpri(UleTask *ctd)
{
	UleTask *td;
	tdq_lock.AssertHeld();
	if (ctd == NULL)
		ctd = tdq_curthread;
	td = this->tdq_choose();
	if (td == NULL || td->td_priority > ctd->td_priority)
		tdq_lowpri = ctd->td_priority;
	else
		tdq_lowpri = td->td_priority;
}


/*
 * Add a thread to a thread queue.  Select the appropriate runq and add the
 * thread to it.  This is the internal function called when the tdq is
 * predetermined.
 */
int CpuState::tdq_add(UleTask *td, int flags)
{
	int lowpri;

	tdq_lock.AssertHeld();
	// THREAD_LOCK_BLOCKED_ASSERT(td, MA_OWNED);
	CHECK((td->td_inhibitors == 0));
	CHECK(td->td_state== UleTask::TDS_RUNNING || td->td_state == UleTask::TDS_CAN_RUN);
	// CHECK(td->td_flags & TDF_INMEM); -- Removed because not relevant to ghOSt impl

	lowpri = tdq_lowpri;
	if (td->td_priority < lowpri)
		tdq_lowpri = td->td_priority;
	this->tdq_runq_add(td, flags);
	this->tdq_load_add(td);
	return (lowpri);
}


/*
 * Attempt to steal a thread in priority order from a thread queue.
 */
UleTask * CpuState::tdq_steal(int cpu)
{
	UleTask *td;
	tdq_lock.AssertHeld();
	//TODO: runq_steal needs to be implemented 
	// if ((td = tdq_realtime.runq_steal(cpu)) != NULL)
	// 	return td;
	// if ((td = tdq_timeshare.runq_steal_from(cpu, tdq_ridx)) != NULL)
	// 	return td;
	// return (tdq_idle.runq_steal(cpu));

	return NULL;
}




}  //  namespace ghost