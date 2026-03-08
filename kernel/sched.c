// /*
//  * implementing the scheduler
//  */

// #include "sched.h"
// #include "spike_interface/spike_utils.h"
// #include "sync_utils.h"

// process *ready_queue_head = NULL;
// volatile int excounter = 0;
// volatile int procounter = 0;

// //
// // insert a process, proc, into the END of ready queue.
// //
// void insert_to_ready_queue(process *proc)
// {
//   int tp = read_tp();
//   // sprint("going to insert process %d to ready queue.\n", proc->pid);
//   // if the queue is empty in the beginning
//   if (ready_queue_head == NULL)
//   {
//     proc->status = READY;
//     proc->queue_next = NULL;
//     ready_queue_head = proc;
//     return;
//   }

//   // ready queue is not empty
//   process *p;
//   // browse the ready queue to see if proc is already in-queue
//   for (p = ready_queue_head; p->queue_next != NULL; p = p->queue_next)
//     if (p == proc)
//       return; // already in queue

//   // p points to the last element of the ready queue
//   if (p == proc)
//     return;
//   p->queue_next = proc;
//   proc->status = READY;
//   proc->queue_next = NULL;

//   return;
// }

// //
// // choose a proc from the ready queue, and put it to run.
// // note: schedule() does not take care of previous current process. If the current
// // process is still runnable, you should place it into the ready queue (by calling
// // ready_queue_insert), and then call schedule().
// //
// extern process procs[NPROC];
// void schedule()
// {
//   int tp = read_tp();
//   if (!ready_queue_head)
//   {
//     // by default, if there are no ready process, and all processes are in the status of
//     // FREE and ZOMBIE, we should shutdown the emulated RISC-V machine.
//     int should_shutdown = 1;
//     sync_barrier(&procounter, NCPU);

//     for (int i = 0; i < NPROC; i++)
//       if ((procs[i].status != FREE) && (procs[i].status != ZOMBIE))
//       {
//         should_shutdown = 0;
//         sprint("ready queue empty, but process %d is not in free/zombie state:%d\n",
//                i, procs[i].status);
//       }

//     if (should_shutdown)
//     {
//       sprint("no more ready processes, system shutdown now.\n");
//       sync_barrier(&excounter, NCPU);

//       if (!tp)
//       {
//         sprint("hartid = %d: shutdown.\n", tp);
//         shutdown(0);
//       }
//     }
//     // else
//     // {
//     //   // 本核就绪队列为空，但仍有进程在其它核上运行，自旋等待全部结束
//     //   while (1)
//     //   {
//     //     int all_done = 1;
//     //     for (int i = 0; i < NPROC; i++)
//     //       if (procs[i].status != FREE && procs[i].status != ZOMBIE)
//     //       {
//     //         all_done = 0;
//     //         break;
//     //       }
//     //     if (all_done)
//     //       break;
//     //     asm volatile("wfi"); // 等待中断，减轻忙等
//     //   }
//     //   sprint("no more ready processes, system shutdown now.\n");
//     //   sync_barrier(&excounter, NCPU);
//     //   if (!tp)
//     //   {
//     //     sprint("hartid = %d: shutdown.\n", tp);
//     //     shutdown(0);
//     //   }
//     //   return;
//     // }
//   }

//   current[tp] = ready_queue_head;
//   assert(current[tp]->status == READY);
//   ready_queue_head = ready_queue_head->queue_next;

//   current[tp]->status = RUNNING;
//   sprint("going to schedule process %d to run.\n", current[tp]->pid);
//   switch_to(current[tp]);
// }

#include "sched.h"
#include "spike_interface/spike_utils.h"
#include "sync_utils.h"

process *ready_queue_head = NULL;
volatile int excounter = 0;
volatile int procounter = 0;

static spinlock_amo_t rq_lock;
static int rq_lock_inited = 0;

static inline void rq_init_once()
{
  if (!rq_lock_inited)
  {
    spinlock_amo_init(&rq_lock);
    rq_lock_inited = 1;
  }
}

static inline int all_process_done()
{
  for (int i = 0; i < NPROC; i++)
    if (procs[i].status != FREE && procs[i].status != ZOMBIE)
      return 0;
  return 1;
}

void insert_to_ready_queue(process *proc)
{
  rq_init_once();
  spinlock_amo_lock(&rq_lock);

  // 只允许 READY/BLOCKED/RUNNING 的进程入队，ZOMBIE/FREE 不应入队
  if (proc->status == ZOMBIE || proc->status == FREE)
  {
    spinlock_amo_unlock(&rq_lock);
    return;
  }

  if (ready_queue_head == NULL)
  {
    proc->status = READY;
    proc->queue_next = NULL;
    ready_queue_head = proc;
    spinlock_amo_unlock(&rq_lock);
    return;
  }

  process *p;
  for (p = ready_queue_head; p->queue_next != NULL; p = p->queue_next)
  {
    if (p == proc)
    {
      spinlock_amo_unlock(&rq_lock);
      return; // already in queue
    }
  }
  if (p == proc)
  {
    spinlock_amo_unlock(&rq_lock);
    return;
  }

  p->queue_next = proc;
  proc->status = READY;
  proc->queue_next = NULL;

  spinlock_amo_unlock(&rq_lock);
}

extern process procs[NPROC];

void schedule()
{
  int tp = read_tp();
  rq_init_once();

  while (1)
  {
    // 1) 先尝试从就绪队列取任务（加锁保护）
    spinlock_amo_lock(&rq_lock);
    if (ready_queue_head)
    {
      current[tp] = ready_queue_head;
      ready_queue_head = ready_queue_head->queue_next;
      current[tp]->queue_next = NULL;

      // 防御性检查：理论上不应调度到非 READY 进程
      if (current[tp]->status != READY)
      {
        sprint("warn: schedule got non-READY proc pid=%d status=%d on hart %d\n",
               current[tp]->pid, current[tp]->status, tp);
      }
      current[tp]->status = RUNNING;
      spinlock_amo_unlock(&rq_lock);

      sprint("going to schedule process %d to run on hart %d.\n", current[tp]->pid, tp);
      switch_to(current[tp]);
      return;
    }
    spinlock_amo_unlock(&rq_lock);

    // 2) 队列空：检查是否全系统结束
    if (all_process_done())
    {
      if (tp == 0)
      {
        sprint("no more ready processes, system shutdown now.\n");
        sprint("hartid = %d: shutdown.\n", tp);
        shutdown(0);
      }

      // 非 0 核等待关机
      while (1)
        asm volatile("wfi");
    }

    // 3) 暂无可运行任务，等待中断再重试，避免忙等
    asm volatile("wfi");
  }
}