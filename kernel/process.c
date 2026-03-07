/*
 * Utility functions for process management.
 *
 * Note: in Lab1, only one process (i.e., our user application) exists. Therefore,
 * PKE OS at this stage will set "current" to the loaded user application, and also
 * switch to the old "current" process after trap handling.
 */

#include "riscv.h"
#include "strap.h"
#include "config.h"
#include "process.h"
#include "elf.h"
#include "string.h"
#include "vmm.h"
#include "pmm.h"
#include "memlayout.h"
#include "sched.h"
#include "spike_interface/spike_utils.h"
#include "util/functions.h"

// Two functions defined in kernel/usertrap.S
extern char smode_trap_vector[];
extern void return_to_user(trapframe *, uint64 satp);

// trap_sec_start points to the beginning of S-mode trap segment (i.e., the entry point
// of S-mode trap vector).
extern char trap_sec_start[];

// process pool. added @lab3_1
process procs[NPROC];
static spinlock_amo_t proc_alloc_lock;
semaphore sem_table[NSEM];

// current points to the currently running user-mode application.
process *current[NCPU] = {NULL};

//
// switch to a user-mode process
//
void switch_to(process *proc)
{
  int tp = read_tp();
  assert(proc);
  current[tp] = proc;

  // write the smode_trap_vector (64-bit func. address) defined in kernel/strap_vector.S
  // to the stvec privilege register, such that trap handler pointed by smode_trap_vector
  // will be triggered when an interrupt occurs in S mode.
  write_csr(stvec, (uint64)smode_trap_vector);

  // set up trapframe values (in process structure) that smode_trap_vector will need when
  // the process next re-enters the kernel.
  proc->trapframe->kernel_sp = proc->kstack;     // process's kernel stack
  proc->trapframe->kernel_satp = read_csr(satp); // kernel page table
  proc->trapframe->kernel_trap = (uint64)smode_trap_handler;
  proc->trapframe->regs.tp = read_tp();

  // SSTATUS_SPP and SSTATUS_SPIE are defined in kernel/riscv.h
  // set S Previous Privilege mode (the SSTATUS_SPP bit in sstatus register) to User mode.
  unsigned long x = read_csr(sstatus);
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode

  // write x back to 'sstatus' register to enable interrupts, and sret destination mode.
  write_csr(sstatus, x);

  // set S Exception Program Counter (sepc register) to the elf entry pc.
  write_csr(sepc, proc->trapframe->epc);

  // make user page table. macro MAKE_SATP is defined in kernel/riscv.h. added @lab2_1
  uint64 user_satp = MAKE_SATP(proc->pagetable);

  // if (proc->trapframe->epc == 0 && proc->trapframe->regs.ra == 0) {
  //   sprint("switch_to: WARN pid=%d trapframe looks zeroed (epc=0 ra=0), ptr=%p\n",
  //          (int)proc->pid, (void *)proc->trapframe);
  // }
  // return_to_user() is defined in kernel/strap_vector.S. switch to user mode with sret.
  // note, return_to_user takes two parameters @ and after lab2_1.
  return_to_user(proc->trapframe, user_satp);
}

uint64 user_better_malloc(size_t size)
{
  int tp = read_tp();
  if (current[tp] == NULL || size == 0)
    return 0;

  pagetable_t pt = (pagetable_t)(current[tp]->pagetable);

  // 从 malloc_free_list 中复用可满足大小的已释放块
  for (malloc *reuse = current[tp]->malloc_free_list; reuse != NULL; reuse = reuse->next)
  {
    if (reuse->size >= size)
    {
      uint64 va = reuse->va;

      // 从 malloc_free_list 摘除 reuse
      if (reuse->prev)
        reuse->prev->next = reuse->next;
      else
        current[tp]->malloc_free_list = reuse->next;
      if (reuse->next)
        reuse->next->prev = reuse->prev;
      reuse->prev = reuse->next = NULL;

      // 确保 [va, va+size) 已映射
      for (uint64 page_va = ROUNDDOWN(va, PGSIZE); page_va < va + size; page_va += PGSIZE)
      {
        if (lookup_pa(pt, page_va) == 0)
        {
          void *pa = alloc_page();
          if (pa == NULL)
          {
            // reuse 插回 malloc_free_list
            reuse->next = current[tp]->malloc_free_list;
            if (current[tp]->malloc_free_list)
              current[tp]->malloc_free_list->prev = reuse;
            current[tp]->malloc_free_list = reuse;
            return 0;
          }
          user_vm_map(pt, page_va, PGSIZE, (uint64)pa,
                      prot_to_type(PROT_READ | PROT_WRITE, 1));
        }
      }

      if (reuse->size > size)
      {
        // 剩余 [va+size, va+reuse->size) 挂回 malloc_free_list
        malloc *rem;
        if (current[tp]->malloc_free_list != NULL)
        {
          rem = current[tp]->malloc_free_list;
          current[tp]->malloc_free_list = rem->next;
          if (rem->next)
            rem->next->prev = NULL;
        }
        else
        {
          rem = (malloc *)alloc_page();
          if (rem == NULL)
          {
            reuse->next = current[tp]->malloc_free_list;
            if (current[tp]->malloc_free_list)
              current[tp]->malloc_free_list->prev = reuse;
            current[tp]->malloc_free_list = reuse;
            return 0;
          }
        }
        rem->va = va + size;
        rem->size = reuse->size - size;
        rem->prev = NULL;
        rem->next = current[tp]->malloc_free_list;
        if (current[tp]->malloc_free_list)
          current[tp]->malloc_free_list->prev = rem;
        current[tp]->malloc_free_list = rem;
      }

      reuse->va = va;
      reuse->size = size;
      reuse->prev = NULL;
      reuse->next = current[tp]->malloc_list;
      if (current[tp]->malloc_list)
        current[tp]->malloc_list->prev = reuse;
      current[tp]->malloc_list = reuse;
      return va;
    }
  }

  // 未找到可复用的块
  uint64 va = current[tp]->user_heap.heap_top;
  uint64 page_start = ROUNDDOWN(va, PGSIZE);
  uint64 remaining;

  // 优先从 malloc_free_list 复用空闲节点，否则再 alloc_page
  malloc *node;
  int node_from_free_list = 0;
  if (current[tp]->malloc_free_list != NULL)
  {
    node = current[tp]->malloc_free_list;
    current[tp]->malloc_free_list = node->next;
    if (node->next)
      node->next->prev = NULL;
    node->next = node->prev = NULL;
    node_from_free_list = 1;
  }
  else
  {
    node = (malloc *)alloc_page();
    if (node == NULL)
      return 0;
  }

  int first_page_we_mapped = 0;
  if (lookup_pa(pt, page_start) == 0)
  {
    first_page_we_mapped = 1;
    void *pa = alloc_page();
    if (pa == NULL)
    {
      if (node_from_free_list)
      {
        node->next = current[tp]->malloc_free_list;
        if (current[tp]->malloc_free_list)
          current[tp]->malloc_free_list->prev = node;
        current[tp]->malloc_free_list = node;
      }
      else
        free_page(node);
      return 0;
    }
    user_vm_map(pt, page_start, PGSIZE, (uint64)pa,
                prot_to_type(PROT_READ | PROT_WRITE, 1));
  }

  remaining = page_start + PGSIZE - va;

  if (remaining >= size)
  {
    current[tp]->user_heap.heap_top = va + size;
    node->va = va;
    node->size = size;
    node->prev = NULL;
    node->next = current[tp]->malloc_list;
    if (current[tp]->malloc_list)
      current[tp]->malloc_list->prev = node;
    current[tp]->malloc_list = node;
    return va;
  }

  uint64 need = size - remaining;
  uint64 n_pages = (need + PGSIZE - 1) / PGSIZE;
  uint64 map_va = page_start + PGSIZE;

  for (uint64 i = 0; i < n_pages; i++)
  {
    void *pa = alloc_page();
    if (pa == NULL)
    {
      if (first_page_we_mapped)
        user_vm_unmap(pt, page_start, PGSIZE, 1);
      for (uint64 j = 0; j < i; j++)
        user_vm_unmap(pt, map_va + j * PGSIZE, PGSIZE, 1);
      if (node_from_free_list)
      {
        node->prev = NULL;
        node->next = current[tp]->malloc_free_list;
        if (current[tp]->malloc_free_list)
          current[tp]->malloc_free_list->prev = node;
        current[tp]->malloc_free_list = node;
      }
      else
        free_page(node);
      return 0;
    }
    user_vm_map(pt, map_va + i * PGSIZE, PGSIZE, (uint64)pa,
                prot_to_type(PROT_READ | PROT_WRITE, 1));
  }
  current[tp]->user_heap.heap_top = va + size;

  node->va = va;
  node->size = size;
  node->prev = NULL;
  node->next = current[tp]->malloc_list;
  if (current[tp]->malloc_list)
    current[tp]->malloc_list->prev = node;
  current[tp]->malloc_list = node;

  return va;
}

// 检查页 [page_va, page_va+PGSIZE) 是否与 malloc_list 中某块相交
static int page_has_other_allocation(uint64 page_va)
{
  int tp = read_tp();
  for (malloc *m = current[tp]->malloc_list; m != NULL; m = m->next)
  {
    if (m->va + m->size > page_va && m->va < page_va + PGSIZE)
      return 1;
  }
  return 0;
}

void user_better_free(uint64 va)
{
  int tp = read_tp();
  if (current[tp] == NULL)
    return;

  malloc *cur = current[tp]->malloc_list;
  while (cur != NULL)
  {
    if (cur->va == va)
      break;
    cur = cur->next;
  }
  if (cur == NULL)
    return;

  size_t size = cur->size;
  pagetable_t pt = (pagetable_t)(current[tp]->pagetable);

  // 从 malloc_list 摘除加入 malloc_free_list
  if (cur->prev)
    cur->prev->next = cur->next;
  else
    current[tp]->malloc_list = cur->next;
  if (cur->next)
    cur->next->prev = cur->prev;

  cur->prev = NULL;
  cur->next = current[tp]->malloc_free_list;
  if (current[tp]->malloc_free_list)
    current[tp]->malloc_free_list->prev = cur;
  current[tp]->malloc_free_list = cur;

  // 仅当该物理页上全部被释放后才 unmap 并释放物理页
  for (uint64 page_va = ROUNDDOWN(va, PGSIZE);
       page_va < va + size;
       page_va += PGSIZE)
  {
    if (!page_has_other_allocation(page_va))
      user_vm_unmap(pt, page_va, PGSIZE, 1);
  }
}

//
// initialize process pool (the procs[] array). added @lab3_1
//
void init_proc_pool()
{
  memset(procs, 0, sizeof(process) * NPROC);

  for (int i = 0; i < NPROC; ++i)
  {
    procs[i].status = FREE;
    procs[i].pid = i;
  }

  for (int i = 0; i < NSEM; i++)
  {
    sem_table[i].ifuse = 0;
    sem_table[i].value = 0;
    sem_table[i].wait_queue = NULL;
  }
  spinlock_amo_init(&proc_alloc_lock);
}

//
// allocate an empty process, init its vm space. returns the pointer to
// process strcuture. added @lab3_1
//
process *alloc_process()
{
  // locate the first usable process structure
  int i;

  // spinlock_amo_lock(&proc_alloc_lock);

  for (i = 0; i < NPROC; i++)
    if (procs[i].status == FREE)
      break;

  if (i >= NPROC)
  {
    // spinlock_amo_unlock(&proc_alloc_lock);
    panic("cannot find any free process structure.\n");
    return 0;
  }
  procs[i].status = READY;
  // init proc[i]'s vm space
  procs[i].trapframe = (trapframe *)alloc_page(); // trapframe, used to save context
  memset(procs[i].trapframe, 0, sizeof(trapframe));
  procs[i].trapframe->regs.tp = read_tp();

  // page directory
  procs[i].pagetable = (pagetable_t)alloc_page();
  // {
  //   static uint64 trapframe_pages[NPROC];
  //   for (int k = 0; k < NPROC; k++) {
  //     if (trapframe_pages[k] == (uint64)procs[i].trapframe) {
  //       sprint("alloc_proc: pid=%d trapframe=%p ALREADY USED BY pid=%d\n",
  //              i, (void *)procs[i].trapframe, k);
  //       break;
  //     }
  //   }
  //   trapframe_pages[i] = (uint64)procs[i].trapframe;
  // }
  memset(procs[i].trapframe, 0, sizeof(trapframe));
  procs[i].trapframe->regs.tp = read_tp();
  memset((void *)procs[i].pagetable, 0, PGSIZE);
  // sprint("in alloc_proc. pid=%d, pagetable=%p\n", i, (pagetable_t)procs[i].pagetable);

  procs[i].kstack = (uint64)alloc_page() + PGSIZE; // user kernel stack top
  uint64 user_stack = (uint64)alloc_page();        // phisical address of user stack bottom
  procs[i].trapframe->regs.sp = USER_STACK_TOP;    // virtual address of user stack top

  // allocates a page to record memory regions (segments)
  procs[i].mapped_info = (mapped_region *)alloc_page();
  memset(procs[i].mapped_info, 0, PGSIZE);

  // map user stack in userspace
  user_vm_map((pagetable_t)procs[i].pagetable, USER_STACK_TOP - PGSIZE, PGSIZE,
              user_stack, prot_to_type(PROT_WRITE | PROT_READ, 1));
  // sprint("alloc_proc pid=%d: stack VA 0x%lx -> PA 0x%lx\n",
  //        procs[i].pid, (uint64)(USER_STACK_TOP - PGSIZE), (uint64)user_stack);
  procs[i].mapped_info[STACK_SEGMENT].va = USER_STACK_TOP - PGSIZE;
  procs[i].mapped_info[STACK_SEGMENT].npages = 1;
  procs[i].mapped_info[STACK_SEGMENT].seg_type = STACK_SEGMENT;

  // map trapframe in user space (direct mapping as in kernel space).
  user_vm_map((pagetable_t)procs[i].pagetable, (uint64)procs[i].trapframe, PGSIZE,
              (uint64)procs[i].trapframe, prot_to_type(PROT_WRITE | PROT_READ, 0));
  procs[i].mapped_info[CONTEXT_SEGMENT].va = (uint64)procs[i].trapframe;
  procs[i].mapped_info[CONTEXT_SEGMENT].npages = 1;
  procs[i].mapped_info[CONTEXT_SEGMENT].seg_type = CONTEXT_SEGMENT;

  // map S-mode trap vector section in user space (direct mapping as in kernel space)
  // we assume that the size of usertrap.S is smaller than a page.
  user_vm_map((pagetable_t)procs[i].pagetable, (uint64)trap_sec_start, PGSIZE,
              (uint64)trap_sec_start, prot_to_type(PROT_READ | PROT_EXEC, 0));
  procs[i].mapped_info[SYSTEM_SEGMENT].va = (uint64)trap_sec_start;
  procs[i].mapped_info[SYSTEM_SEGMENT].npages = 1;
  procs[i].mapped_info[SYSTEM_SEGMENT].seg_type = SYSTEM_SEGMENT;

  // sprint("in alloc_proc. user frame 0x%lx, user stack 0x%lx, user kstack 0x%lx \n",
  //        procs[i].trapframe, procs[i].trapframe->regs.sp, procs[i].kstack);

  // sprint("in alloc_proc. user frame 0x%lx, user stack 0x%lx, user kstack 0x%lx \n",
  //        procs[i].trapframe, procs[i].trapframe->regs.sp, procs[i].kstack);
  // sprint("alloc_proc pid=%d: trapframe_pa=0x%lx pagetable_pa=0x%lx kstack_pa=0x%lx user_stack_pa=0x%lx mapped_info_pa=0x%lx\n",
  //        procs[i].pid,
  //        (uint64)procs[i].trapframe,
  //        (uint64)procs[i].pagetable,
  //        (uint64)(procs[i].kstack - PGSIZE),
  //        (uint64)user_stack,
  //        (uint64)procs[i].mapped_info);

  // initialize the process's heap manager
  procs[i].user_heap.heap_top = USER_FREE_ADDRESS_START;
  procs[i].user_heap.heap_bottom = USER_FREE_ADDRESS_START;
  procs[i].user_heap.free_pages_count = 0;

  // map user heap in userspace
  procs[i].mapped_info[HEAP_SEGMENT].va = USER_FREE_ADDRESS_START;
  procs[i].mapped_info[HEAP_SEGMENT].npages = 0; // no pages are mapped to heap yet.
  procs[i].mapped_info[HEAP_SEGMENT].seg_type = HEAP_SEGMENT;

  procs[i].total_mapped_region = 4;

  // initialize files_struct
  procs[i].pfiles = init_proc_file_management();
  // sprint("in alloc_proc. build proc_file_management successfully.\n");

  // spinlock_amo_unlock(&proc_alloc_lock);
  // return after initialization.
  return &procs[i];
}

//
// reclaim a process. added @lab3_1
//
int free_process(process *proc)
{
  // we set the status to ZOMBIE, but cannot destruct its vm space immediately.
  // since proc can be current process, and its user kernel stack is currently in use!
  // but for proxy kernel, it (memory leaking) may NOT be a really serious issue,
  // as it is different from regular OS, which needs to run 7x24.
  proc->status = ZOMBIE;

  return 0;
}

//
// implements fork syscal in kernel. added @lab3_1
// basic idea here is to first allocate an empty process (child), then duplicate the
// context and data segments of parent process to the child, and lastly, map other
// segments (code, system) of the parent to child. the stack segment remains unchanged
// for the child.
//
int do_fork(process *parent)
{
  int tp = read_tp();
  sprint("will fork a child from parent %d.\n", parent->pid);
  process *child = alloc_process();

  for (int i = 0; i < parent->total_mapped_region; i++)
  {
    // browse parent's vm space, and copy its trapframe and data segments,
    // map its code segment.
    switch (parent->mapped_info[i].seg_type)
    {
    case CONTEXT_SEGMENT:
      *child->trapframe = *parent->trapframe;
      break;
    case STACK_SEGMENT:
      memcpy((void *)lookup_pa(child->pagetable, child->mapped_info[STACK_SEGMENT].va),
             (void *)lookup_pa(parent->pagetable, parent->mapped_info[i].va), PGSIZE);
      break;
    case HEAP_SEGMENT:
      // build a same heap for child process.

      // convert free_pages_address into a filter to skip reclaimed blocks in the heap
      // when mapping the heap blocks
      int free_block_filter[MAX_HEAP_PAGES];
      memset(free_block_filter, 0, MAX_HEAP_PAGES);
      uint64 heap_bottom = parent->user_heap.heap_bottom;
      for (int i = 0; i < parent->user_heap.free_pages_count; i++)
      {
        int index = (parent->user_heap.free_pages_address[i] - heap_bottom) / PGSIZE;
        free_block_filter[index] = 1;
      }

      // copy and map the heap blocks
      for (uint64 heap_block = current[tp]->user_heap.heap_bottom;
           heap_block < current[tp]->user_heap.heap_top; heap_block += PGSIZE)
      {
        if (free_block_filter[(heap_block - heap_bottom) / PGSIZE]) // skip free blocks
          continue;

        // void *child_pa = alloc_page();
        // memcpy(child_pa, (void *)lookup_pa(parent->pagetable, heap_block), PGSIZE);
        // user_vm_map((pagetable_t)child->pagetable, heap_block, PGSIZE, (uint64)child_pa,
        //             prot_to_type(PROT_WRITE | PROT_READ, 1));
        uint64 pa = lookup_pa(parent->pagetable, heap_block);
        if (pa == 0)
          continue;
        user_vm_map_cow((pagetable_t)child->pagetable, heap_block, PGSIZE, pa);
        user_vm_set_cow((pagetable_t)parent->pagetable, heap_block);
        inc_page_ref((void *)pa);
      }

      child->mapped_info[HEAP_SEGMENT].npages = parent->mapped_info[HEAP_SEGMENT].npages;

      // copy the heap manager from parent to child
      memcpy((void *)&child->user_heap, (void *)&parent->user_heap, sizeof(parent->user_heap));
      break;
    case CODE_SEGMENT:
      // TODO (lab3_1): implment the mapping of child code segment to parent's
      // code segment.
      // hint: the virtual address mapping of code segment is tracked in mapped_info
      // page of parent's process structure. use the information in mapped_info to
      // retrieve the virtual to physical mapping of code segment.
      // after having the mapping information, just map the corresponding virtual
      // address region of child to the physical pages that actually store the code
      // segment of parent process.
      // DO NOT COPY THE PHYSICAL PAGES, JUST MAP THEM.
      uint64 va = parent->mapped_info[i].va;
      uint64 pa = lookup_pa(parent->pagetable, va);
      user_vm_map((pagetable_t)child->pagetable, va, parent->mapped_info[i].npages * PGSIZE, pa,
                  prot_to_type(PROT_READ | PROT_EXEC, 1));
      // sprint("do_fork map code segment at pa:%lx of parent to child at va:%lx.\n", pa, va);

      // after mapping, register the vm region (do not delete codes below!)
      child->mapped_info[child->total_mapped_region].va = parent->mapped_info[i].va;
      child->mapped_info[child->total_mapped_region].npages =
          parent->mapped_info[i].npages;
      child->mapped_info[child->total_mapped_region].seg_type = CODE_SEGMENT;
      child->total_mapped_region++;
      break;

    case DATA_SEGMENT:
      uint64 data_va = parent->mapped_info[i].va;
      for (int pg = 0; pg < parent->mapped_info[i].npages; pg++)
      {
        uint64 page_va = data_va + pg * PGSIZE;
        uint64 page_pa = lookup_pa(parent->pagetable, page_va);
        void *new_pa = alloc_page();
        memcpy(new_pa, (void *)page_pa, PGSIZE);
        user_vm_map((pagetable_t)child->pagetable, page_va, PGSIZE,
                    (uint64)new_pa, prot_to_type(PROT_WRITE | PROT_READ, 1));
      }

      child->mapped_info[child->total_mapped_region].va = parent->mapped_info[i].va;
      child->mapped_info[child->total_mapped_region].npages =
          parent->mapped_info[i].npages;
      child->mapped_info[child->total_mapped_region].seg_type = DATA_SEGMENT;
      child->total_mapped_region++;
      break;
    }
  }

  child->status = READY;
  child->trapframe->regs.a0 = 0;
  child->parent = parent;
  insert_to_ready_queue(child);

  return child->pid;
}

void clr_proc(process *proc)
{
  // init proc[i]'s vm space
  proc->trapframe = (trapframe *)alloc_page(); // trapframe, used to save context
  memset(proc->trapframe, 0, sizeof(trapframe));

  // page directory
  proc->pagetable = (pagetable_t)alloc_page();
  memset((void *)proc->pagetable, 0, PGSIZE);

  proc->kstack = (uint64)alloc_page() + PGSIZE; // user kernel stack top
  uint64 user_stack = (uint64)alloc_page();     // phisical address of user stack bottom
  proc->trapframe->regs.sp = USER_STACK_TOP;    // virtual address of user stack top

  // allocates a page to record memory regions (segments)
  proc->mapped_info = (mapped_region *)alloc_page();
  memset(proc->mapped_info, 0, PGSIZE);

  // map user stack in userspace
  user_vm_map((pagetable_t)proc->pagetable, USER_STACK_TOP - PGSIZE, PGSIZE,
              user_stack, prot_to_type(PROT_WRITE | PROT_READ, 1));
  proc->mapped_info[STACK_SEGMENT].va = USER_STACK_TOP - PGSIZE;
  proc->mapped_info[STACK_SEGMENT].npages = 1;
  proc->mapped_info[STACK_SEGMENT].seg_type = STACK_SEGMENT;

  // map trapframe in user space (direct mapping as in kernel space).
  user_vm_map((pagetable_t)proc->pagetable, (uint64)proc->trapframe, PGSIZE,
              (uint64)proc->trapframe, prot_to_type(PROT_WRITE | PROT_READ, 0));
  proc->mapped_info[CONTEXT_SEGMENT].va = (uint64)proc->trapframe;
  proc->mapped_info[CONTEXT_SEGMENT].npages = 1;
  proc->mapped_info[CONTEXT_SEGMENT].seg_type = CONTEXT_SEGMENT;

  // map S-mode trap vector section in user space (direct mapping as in kernel space)
  // we assume that the size of usertrap.S is smaller than a page.
  user_vm_map((pagetable_t)proc->pagetable, (uint64)trap_sec_start, PGSIZE,
              (uint64)trap_sec_start, prot_to_type(PROT_READ | PROT_EXEC, 0));
  proc->mapped_info[SYSTEM_SEGMENT].va = (uint64)trap_sec_start;
  proc->mapped_info[SYSTEM_SEGMENT].npages = 1;
  proc->mapped_info[SYSTEM_SEGMENT].seg_type = SYSTEM_SEGMENT;

  // initialize the process's heap manager
  proc->user_heap.heap_top = USER_FREE_ADDRESS_START;
  proc->user_heap.heap_bottom = USER_FREE_ADDRESS_START;
  proc->user_heap.free_pages_count = 0;

  // map user heap in userspace
  proc->mapped_info[HEAP_SEGMENT].va = USER_FREE_ADDRESS_START;
  proc->mapped_info[HEAP_SEGMENT].npages = 0; // no pages are mapped to heap yet.
  proc->mapped_info[HEAP_SEGMENT].seg_type = HEAP_SEGMENT;

  proc->total_mapped_region = 4;

  // initialize files_struct
  proc->pfiles = (proc_file_management *)alloc_page();
  proc->pfiles->cwd = vfs_root_dentry; // by default, cwd is the root
  proc->pfiles->nfiles = 0;

  for (int fd = 0; fd < MAX_FILES; ++fd)
    proc->pfiles->opened_files[fd].status = FD_NONE;

  proc->status = READY;
}

int do_semNew(uint64 init_val)
{
  for (int i = 0; i < NSEM; i++)
  {
    if (sem_table[i].ifuse == 0)
    {
      sem_table[i].ifuse = 1;
      sem_table[i].value = (int)init_val;
      sem_table[i].wait_queue = NULL;
      return i;
    }
  }
  return -1;
}

void do_semP(uint64 sid)
{
  int tp = read_tp();
  if (sid >= (uint64)NSEM || sem_table[sid].ifuse == 0)
  {
    panic("do_semP: invalid sid\n");
    return;
  }
  semaphore *sem = &sem_table[sid];
  if (sem->value > 0)
  {
    sem->value--;
    return;
  }

  current[tp]->queue_next = NULL;
  if (sem->wait_queue == NULL)
  {
    sem->wait_queue = current[tp];
  }
  else
  {
    process *p = sem->wait_queue;
    while (p->queue_next != NULL)
      p = p->queue_next;
    p->queue_next = current[tp];
  }
  current[tp]->status = BLOCKED;
  schedule();
}

void do_semV(uint64 sid)
{
  if (sid >= (uint64)NSEM || sem_table[sid].ifuse == 0)
  {
    panic("do_semV: invalid sid\n");
    return;
  }
  semaphore *sem = &sem_table[sid];
  sem->value++;
  if (sem->wait_queue != NULL)
  {
    process *p = sem->wait_queue;
    sem->wait_queue = p->queue_next;
    p->queue_next = NULL;
    sem->value--;
    insert_to_ready_queue(p);
  }
}