
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

UleRunq::UleRunq() {

}

int UleRunq::runq_check() {
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
	
	if (rqh->empty()) {
		//CTR0(KTR_RUNQ, "runq_remove_idx: empty");
		runq_clrbit(pri);
		if (idx != NULL && *idx == pri)
			*idx = (pri + 1) % RQ_NQS;
	}
}




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
      idle_load_balancing_(
          absl::GetFlag(FLAGS_experimental_enable_idle_load_balancing)) {
	for (const Cpu& cpu : cpus()) {
    CpuState* cs = cpu_state(cpu);
    cs->id = cpu.id();

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

  std::cout << "New Task arrived: " << task->gtid.describe() << "\n";
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
  while (!Finished()) {
    
  }
  
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
		ts->ts_runq->runq_add_pri(td, pri, flags);
		return;
	} else
		td->ts_runq = &tdq_idle;
	td->ts_runq->runq_add(td, flags);
}

}  //  namespace ghost
