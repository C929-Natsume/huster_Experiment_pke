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
#include "spike_interface/spike_utils.h"

#include "util/functions.h"

// Two functions defined in kernel/usertrap.S
extern char smode_trap_vector[];
extern void return_to_user(trapframe *, uint64 satp);

// current points to the currently running user-mode application.
process *current = NULL;

// points to the first free page in our simple heap. added @lab2_2
uint64 g_ufree_page = USER_FREE_ADDRESS_START;

//
// switch to a user-mode process
//
void switch_to(process *proc)
{
  assert(proc);
  current = proc;

  // write the smode_trap_vector (64-bit func. address) defined in kernel/strap_vector.S
  // to the stvec privilege register, such that trap handler pointed by smode_trap_vector
  // will be triggered when an interrupt occurs in S mode.
  write_csr(stvec, (uint64)smode_trap_vector);

  // set up trapframe values (in process structure) that smode_trap_vector will need when
  // the process next re-enters the kernel.
  proc->trapframe->kernel_sp = proc->kstack;     // process's kernel stack
  proc->trapframe->kernel_satp = read_csr(satp); // kernel page table
  proc->trapframe->kernel_trap = (uint64)smode_trap_handler;

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

  // return_to_user() is defined in kernel/strap_vector.S. switch to user mode with sret.
  // note, return_to_user takes two parameters @ and after lab2_1.
  return_to_user(proc->trapframe, user_satp);
}

uint64 user_better_malloc(size_t size)
{
  if (current == NULL || size == 0)
    return 0;

  pagetable_t pt = (pagetable_t)(current->pagetable);

  // 从 malloc_free_list 中复用可满足大小的已释放块
  for (malloc *reuse = current->malloc_free_list; reuse != NULL; reuse = reuse->next)
  {
    if (reuse->size >= size)
    {
      uint64 va = reuse->va;

      // 从 malloc_free_list 摘除 reuse
      if (reuse->prev)
        reuse->prev->next = reuse->next;
      else
        current->malloc_free_list = reuse->next;
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
            reuse->next = current->malloc_free_list;
            if (current->malloc_free_list)
              current->malloc_free_list->prev = reuse;
            current->malloc_free_list = reuse;
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
        if (current->malloc_free_list != NULL)
        {
          rem = current->malloc_free_list;
          current->malloc_free_list = rem->next;
          if (rem->next)
            rem->next->prev = NULL;
        }
        else
        {
          rem = (malloc *)alloc_page();
          if (rem == NULL)
          {
            reuse->next = current->malloc_free_list;
            if (current->malloc_free_list)
              current->malloc_free_list->prev = reuse;
            current->malloc_free_list = reuse;
            return 0;
          }
        }
        rem->va = va + size;
        rem->size = reuse->size - size;
        rem->prev = NULL;
        rem->next = current->malloc_free_list;
        if (current->malloc_free_list)
          current->malloc_free_list->prev = rem;
        current->malloc_free_list = rem;
      }

      reuse->va = va;
      reuse->size = size;
      reuse->prev = NULL;
      reuse->next = current->malloc_list;
      if (current->malloc_list)
        current->malloc_list->prev = reuse;
      current->malloc_list = reuse;
      return va;
    }
  }

  // 未找到可复用的块，从 g_ufree_page 分配
  uint64 va = g_ufree_page;
  uint64 page_start = ROUNDDOWN(va, PGSIZE);
  uint64 remaining;

  // 优先从 malloc_free_list 复用空闲节点，否则再 alloc_page
  malloc *node;
  int node_from_free_list = 0;
  if (current->malloc_free_list != NULL)
  {
    node = current->malloc_free_list;
    current->malloc_free_list = node->next;
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
        node->next = current->malloc_free_list;
        if (current->malloc_free_list)
          current->malloc_free_list->prev = node;
        current->malloc_free_list = node;
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
    g_ufree_page = va + size;
    node->va = va;
    node->size = size;
    node->prev = NULL;
    node->next = current->malloc_list;
    if (current->malloc_list)
      current->malloc_list->prev = node;
    current->malloc_list = node;
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
        node->next = current->malloc_free_list;
        if (current->malloc_free_list)
          current->malloc_free_list->prev = node;
        current->malloc_free_list = node;
      }
      else
        free_page(node);
      return 0;
    }
    user_vm_map(pt, map_va + i * PGSIZE, PGSIZE, (uint64)pa,
                prot_to_type(PROT_READ | PROT_WRITE, 1));
  }
  g_ufree_page = va + size;

  node->va = va;
  node->size = size;
  node->prev = NULL;
  node->next = current->malloc_list;
  if (current->malloc_list)
    current->malloc_list->prev = node;
  current->malloc_list = node;

  return va;
}

// 检查页 [page_va, page_va+PGSIZE) 是否与 malloc_list 中某块相交
static int page_has_other_allocation(uint64 page_va)
{
  for (malloc *m = current->malloc_list; m != NULL; m = m->next)
  {
    if (m->va + m->size > page_va && m->va < page_va + PGSIZE)
      return 1;
  }
  return 0;
}

void user_better_free(uint64 va)
{
  if (current == NULL)
    return;

  malloc *cur = current->malloc_list;
  while (cur != NULL)
  {
    if (cur->va == va)
      break;
    cur = cur->next;
  }
  if (cur == NULL)
    return;

  size_t size = cur->size;
  pagetable_t pt = (pagetable_t)(current->pagetable);

  // 从 malloc_list 摘除加入 malloc_free_list
  if (cur->prev)
    cur->prev->next = cur->next;
  else
    current->malloc_list = cur->next;
  if (cur->next)
    cur->next->prev = cur->prev;

  cur->prev = NULL;
  cur->next = current->malloc_free_list;
  if (current->malloc_free_list)
    current->malloc_free_list->prev = cur;
  current->malloc_free_list = cur;

  // 仅当该物理页上全部被释放后才 unmap 并释放物理页
  for (uint64 page_va = ROUNDDOWN(va, PGSIZE);
       page_va < va + size;
       page_va += PGSIZE)
  {
    if (!page_has_other_allocation(page_va))
      user_vm_unmap(pt, page_va, PGSIZE, 1);
  }
}