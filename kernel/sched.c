/*
 * implementing the scheduler
 */

#include "sched.h"
#include "spike_interface/spike_utils.h"
#include "sync_utils.h"

process *ready_queue_head[NCPU] = {NULL};
volatile int excounter = 0;
volatile int procounter = 0;

//
// insert a process, proc, into the END of ready queue.
//
void insert_to_ready_queue(process *proc)
{
  int tp = read_tp();
  // sprint("going to insert process %d to ready queue.\n", proc->pid);
  // if the queue is empty in the beginning
  if (ready_queue_head[tp] == NULL)
  {
    proc->status = READY;
    proc->queue_next = NULL;
    ready_queue_head[tp] = proc;
    return;
  }

  // ready queue is not empty
  process *p;
  // browse the ready queue to see if proc is already in-queue
  for (p = ready_queue_head[tp]; p->queue_next != NULL; p = p->queue_next)
    if (p == proc)
      return; // already in queue

  // p points to the last element of the ready queue
  if (p == proc)
    return;
  p->queue_next = proc;
  proc->status = READY;
  proc->queue_next = NULL;

  return;
}

//
// choose a proc from the ready queue, and put it to run.
// note: schedule() does not take care of previous current process. If the current
// process is still runnable, you should place it into the ready queue (by calling
// ready_queue_insert), and then call schedule().
//
extern process procs[NPROC];
void schedule()
{
  int tp = read_tp();
  if (!ready_queue_head[tp])
  {
    // by default, if there are no ready process, and all processes are in the status of
    // FREE and ZOMBIE, we should shutdown the emulated RISC-V machine.
    int should_shutdown = 1;
    sync_barrier(&procounter, NCPU);

    for (int i = 0; i < NPROC; i++)
      if ((procs[i].status != FREE) && (procs[i].status != ZOMBIE))
      {
        should_shutdown = 0;
        sprint("ready queue empty, but process %d is not in free/zombie state:%d\n",
               i, procs[i].status);
      }

    if (should_shutdown)
    {
      sprint("no more ready processes, system shutdown now.\n");
      sync_barrier(&excounter, NCPU);

      if (!tp)
      {
        sprint("hartid = %d: shutdown.\n", tp);
        shutdown(0);
      }
    }
    else
    {
      // 本核就绪队列为空，但仍有进程在其它核上运行，自旋等待全部结束
      while (1)
      {
        int all_done = 1;
        for (int i = 0; i < NPROC; i++)
          if (procs[i].status != FREE && procs[i].status != ZOMBIE)
          {
            all_done = 0;
            break;
          }
        if (all_done)
          break;
        asm volatile("wfi"); // 等待中断，减轻忙等
      }
      sprint("no more ready processes, system shutdown now.\n");
      sync_barrier(&excounter, NCPU);
      if (!tp)
      {
        sprint("hartid = %d: shutdown.\n", tp);
        shutdown(0);
      }
      return;
    }
  }

  current[tp] = ready_queue_head[tp];
  assert(current[tp]->status == READY);
  ready_queue_head[tp] = ready_queue_head[tp]->queue_next;

  current[tp]->status = RUNNING;
  sprint("going to schedule process %d to run.\n", current[tp]->pid);
  switch_to(current[tp]);
}
