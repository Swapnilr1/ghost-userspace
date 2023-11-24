
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

// void PrintDebugTaskMessage(std::string message_name, CpuState* cs,
//                            UleTask* task) {
//   DPRINT_Ule(2, absl::StrFormat(
//                     "[%s]: %s with state %s, %scurrent", message_name,
//                     task->gtid.describe(),
//                     absl::FormatStreamed(task->task_state),
//                     (cs && cs->current == task) ? "" : "!"));
// }

UleScheduler::UleScheduler(Enclave* enclave, CpuList cpulist,
                           std::shared_ptr<TaskAllocator<UleTask>> allocator,
                           absl::Duration min_granularity,
                           absl::Duration latency)
    : BasicDispatchScheduler(enclave, std::move(cpulist), std::move(allocator)),
      min_granularity_(min_granularity),
      latency_(latency),
      idle_load_balancing_(
          absl::GetFlag(FLAGS_experimental_enable_idle_load_balancing)) {
  
}

void UleScheduler::DumpAllTasks() {

}



void UleScheduler::EnclaveReady() {
  
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
 
}

void UleScheduler::TaskRunnable(UleTask* task, const Message& msg) {
}

// Disable thread safety analysis as this function is called with rq lock held
// but it's hard for the compiler to infer. Without this annotation, the
// compiler raises safety analysis error.
void UleScheduler::HandleTaskDone(UleTask* task, bool from_switchto) {
 
}

void UleScheduler::TaskDeparted(UleTask* task, const Message& msg) {
 
}

void UleScheduler::TaskDead(UleTask* task, const Message& msg) {

}

void UleScheduler::TaskYield(UleTask* task, const Message& msg) {

}

void UleScheduler::TaskBlocked(UleTask* task, const Message& msg) {
  
}

void UleScheduler::TaskPreempted(UleTask* task, const Message& msg) {
  
}

void UleScheduler::TaskSwitchto(UleTask* task, const Message& msg) {
 
}

// Disable thread safety analysis as this function is called with rq lock held
// but it's hard for the compiler to infer. Without this annotation, the
// compiler raises safety analysis error.
void UleScheduler::CheckPreemptTick(const Cpu& cpu) {

}

void UleScheduler::PutPrevTask(UleTask* task) {
}

void UleScheduler::CpuTick(const Message& msg) {

}

//-----------------------------------------------------------------------------
// Load Balance
//-----------------------------------------------------------------------------



//-----------------------------------------------------------------------------
// Schedule
//-----------------------------------------------------------------------------

void UleScheduler::UleSchedule(const Cpu& cpu, BarrierToken agent_barrier,
                               bool prio_boost) {

}

void UleScheduler::Schedule(const Cpu& cpu, const StatusWord& agent_sw) {

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

  
}

/*
 * Print the status of a per-cpu thread queue.  Should be a ddb show cmd.
 */
static void __unused
tdq_print(int cpu)
{
	struct CpuState *tdq;

	tdq = TDQ_CPU(cpu);

	printf("tdq %d:\n", TDQ_ID(tdq));
	printf("\tlock            %p\n", TDQ_LOCKPTR(tdq));
	printf("\tLock name:      %s\n", tdq->tdq_name);
	printf("\tload:           %d\n", tdq->tdq_load);
	printf("\tswitch cnt:     %d\n", tdq->tdq_switchcnt);
	printf("\told switch cnt: %d\n", tdq->tdq_oldswitchcnt);
	printf("\ttimeshare idx:  %d\n", tdq->tdq_idx);
	printf("\ttimeshare ridx: %d\n", tdq->tdq_ridx);
	printf("\tload transferable: %d\n", tdq->tdq_transferable);
	printf("\tlowest priority:   %d\n", tdq->tdq_lowpri);
	printf("\trealtime runq:\n");
	runq_print(&tdq->tdq_realtime);
	printf("\ttimeshare runq:\n");
	runq_print(&tdq->tdq_timeshare);
	printf("\tidle runq:\n");
	runq_print(&tdq->tdq_idle);
}

/*
 * Add a thread to the actual run-queue.  Keeps transferable counts up to
 * date with what is actually on the run-queue.  Selects the correct
 * queue position for timeshare threads.
 */
__inline void CpuState::tdq_runq_add(UleTask *td, int flags)
{
	UleTask *ts;
	u_char pri;

  //TODO: modify this to assert for locks we are using 
	// TDQ_LOCK_ASSERT(this, MA_OWNED);
	// THREAD_LOCK_BLOCKED_ASSERT(td, MA_OWNED);

	pri = td->td_priority;
	// ts = td_get_sched(td);
	TD_SET_RUNQ(td);
	if (THREAD_CAN_MIGRATE(td)) {
		tdq_transferable++;
		td->ts_flags |= TSF_XFERABLE;
	}
	if (pri < PRI_MIN_BATCH) {
		td->ts_runq = &tdq_realtime;
	} else if (pri <= PRI_MAX_BATCH) {
		td->ts_runq = &tdq_timeshare;
		CHECK(pri <= PRI_MAX_BATCH && pri >= PRI_MIN_BATCH,
			("Invalid priority %d on timeshare runq", pri));
		/*
		 * This queue contains only priorities between MIN and MAX
		 * batch.  Use the whole queue to represent these values.
		 */
		if ((flags & (SRQ_BORROWING|SRQ_PREEMPTED)) == 0) {
			pri = RQ_NQS * (pri - PRI_MIN_BATCH) / PRI_BATCH_RANGE;
			pri = (pri + tdq_idx) % RQ_NQS;
			/*
			 * This effectively shortens the queue by one so we
			 * can have a one slot difference between idx and
			 * ridx while we wait for threads to drain.
			 */
			if (tdq_ridx != tdq_idx &&
			    pri == tdq_ridx)
				pri = (unsigned char)(pri - 1) % RQ_NQS;
		} else
			pri = tdq->tdq_ridx;
		runq_add_pri(ts->ts_runq, td, pri, flags);
		return;
	} else
		td->ts_runq = &tdq_idle;
	(td->ts_runq, td, flags);
}

}  //  namespace ghost
