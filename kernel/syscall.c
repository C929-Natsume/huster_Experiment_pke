/*
 * contains the implementation of all syscalls.
 */

#include <stdint.h>
#include <errno.h>

#include "util/types.h"
#include "syscall.h"
#include "string.h"
#include "process.h"
#include "util/functions.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "proc_file.h"

#include "vfs.h"

#include "spike_interface/spike_utils.h"

#include "process.h"
#include "elf.h"

#include "sync_utils.h"
//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char *buf, size_t n)
{
  int tp = read_tp();
  // buf is now an address in user space of the given app's user stack,
  // so we have to transfer it into phisical address (kernel is running in direct mapping).
  assert(current[tp]);
  char *pa = (char *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), (void *)buf);
  sprint(pa);
  return 0;
}

ssize_t sys_user_scanf(const char *buf)
{
  int tp = read_tp();
  // buf is now an address in user space of the given app's user stack,
  // so we have to transfer it into phisical address (kernel is running in direct mapping).
  assert(current[tp]);
  char *pa = (char *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), (void *)buf);
  spike_file_read(stderr, pa, 256);
  return 0;
}

ssize_t sys_user_printpa(uint64 va)
{
  int tp = read_tp();
  uint64 pa = (uint64)user_va_to_pa((pagetable_t)(current[tp]->pagetable), (void *)va);
  // sprint("[printpa] tp=%d current[tp]->pid=%d va=0x%lx -> pa=0x%lx\n",
  //   tp, current[tp] ? (int)current[tp]->pid : -1, va, pa);
  sprint("%lx\n", pa);
  return 0;
}

//
// implement the SYS_user_exit syscall
//
// ssize_t sys_user_exit(uint64 code)
// {
//   int tp = read_tp();
//   sprint("hartid = %d: User exit with code:%d.\n", tp, code);
//   // reclaim the current process, and reschedule. added @lab3_1
//   free_process(current[tp]);

//   if (current[tp]->parent && current[tp]->parent->status == BLOCKED)
//   {
//     current[tp]->parent->status = READY;
//     insert_to_ready_queue(current[tp]->parent);
//   }

//   schedule();
//   return 0;
// }

ssize_t sys_user_exit(uint64 code)
{
  int tp = read_tp();
  process *self = current[tp];
  process *parent = self->parent;

  sprint("hartid = %d: User exit with code:%d.\n", tp, code);

  // 当前进程置为 ZOMBIE
  free_process(self);

  // 唤醒阻塞在 wait 的父进程（若有）
  if (parent && parent->status == BLOCKED)
  {
    parent->status = READY;
    insert_to_ready_queue(parent);
  }

  schedule();
  return 0;
}

//
// maybe, the simplest implementation of malloc in the world ... added @lab2_2
//
uint64 sys_user_allocate_page()
{

  int tp = read_tp();
  // sprint("sys_alloc: tp=%d pid=%d heap_top_before=%lx\n", read_tp(), (int)current[tp]->pid, current[tp]->user_heap.heap_top);
  void *pa = alloc_page();

  uint64 va;
  // if there are previously reclaimed pages, use them first (this does not change the
  // size of the heap)
  if (current[tp]->user_heap.free_pages_count > 0)
  {
    va = current[tp]->user_heap.free_pages_address[--current[tp]->user_heap.free_pages_count];
    assert(va < current[tp]->user_heap.heap_top);
  }
  else
  {
    // otherwise, allocate a new page (this increases the size of the heap by one page)
    va = current[tp]->user_heap.heap_top;
    current[tp]->user_heap.heap_top += PGSIZE;

    current[tp]->mapped_info[HEAP_SEGMENT].npages++;
  }
  user_vm_map((pagetable_t)current[tp]->pagetable, va, PGSIZE, (uint64)pa,
              prot_to_type(PROT_WRITE | PROT_READ, 1));

  // sprint("sys_alloc: tp=%d pid=%d heap_top_after=%lx\n", read_tp(), (int)current[tp]->pid, current[tp]->user_heap.heap_top);

  return va;
}

//
// reclaim a page, indicated by "va". added @lab2_2
//
uint64 sys_user_free_page(uint64 va)
{
  int tp = read_tp();
  user_vm_unmap((pagetable_t)current[tp]->pagetable, va, PGSIZE, 1);
  // add the reclaimed page to the free page list
  current[tp]->user_heap.free_pages_address[current[tp]->user_heap.free_pages_count++] = va;
  return 0;
}

uint64 sys_user_better_allocate_page(size_t n)
{
  uint64 va = user_better_malloc(n);
  return va;
}

//
// reclaim a page, indicated by "va". added @lab2_2
//
uint64 sys_user_better_free_page(uint64 va)
{
  user_better_free(va);
  return 0;
}

//
// kerenl entry point of naive_fork
//
ssize_t sys_user_fork()
{
  int tp = read_tp();
  sprint("User call fork.\n");
  return do_fork(current[tp]);
}

//
// kerenl entry point of yield. added @lab3_2
//
ssize_t sys_user_yield()
{
  int tp = read_tp();
  // TODO (lab3_2): implment the syscall of yield.
  // hint: the functionality of yield is to give up the processor. therefore,
  // we should set the status of currently running process to READY, insert it in
  // the rear of ready queue, and finally, schedule a READY process to run.
  current[tp]->status = READY;
  insert_to_ready_queue(current[tp]);
  schedule();

  return 0;
}

//
// open file
//
ssize_t sys_user_open(char *pathva, int flags)
{
  int tp = read_tp();
  char *pathpa = (char *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), pathva);
  return do_open(pathpa, flags);
}

//
// read file
//
ssize_t sys_user_read(int fd, char *bufva, uint64 count)
{
  int tp = read_tp();
  int i = 0;
  while (i < count)
  { // count can be greater than page size
    uint64 addr = (uint64)bufva + i;
    uint64 pa = lookup_pa((pagetable_t)current[tp]->pagetable, addr);
    uint64 off = addr - ROUNDDOWN(addr, PGSIZE);
    uint64 len = count - i < PGSIZE - off ? count - i : PGSIZE - off;
    uint64 r = do_read(fd, (char *)pa + off, len);
    i += r;
    if (r < len)
      return i;
  }
  return count;
}

//
// write file
//
ssize_t sys_user_write(int fd, char *bufva, uint64 count)
{
  int tp = read_tp();
  int i = 0;
  while (i < count)
  { // count can be greater than page size
    uint64 addr = (uint64)bufva + i;
    uint64 pa = lookup_pa((pagetable_t)current[tp]->pagetable, addr);
    uint64 off = addr - ROUNDDOWN(addr, PGSIZE);
    uint64 len = count - i < PGSIZE - off ? count - i : PGSIZE - off;
    uint64 r = do_write(fd, (char *)pa + off, len);
    i += r;
    if (r < len)
      return i;
  }
  return count;
}

//
// lseek file
//
ssize_t sys_user_lseek(int fd, int offset, int whence)
{
  return do_lseek(fd, offset, whence);
}

//
// read vinode
//
ssize_t sys_user_stat(int fd, struct istat *istat)
{
  int tp = read_tp();
  struct istat *pistat = (struct istat *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), istat);
  return do_stat(fd, pistat);
}

//
// read disk inode
//
ssize_t sys_user_disk_stat(int fd, struct istat *istat)
{
  int tp = read_tp();
  struct istat *pistat = (struct istat *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), istat);
  return do_disk_stat(fd, pistat);
}

//
// close file
//
ssize_t sys_user_close(int fd)
{
  return do_close(fd);
}

//
// lib call to opendir
//
ssize_t sys_user_opendir(char *pathva)
{
  int tp = read_tp();
  char *pathpa = (char *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), pathva);
  return do_opendir(pathpa);
}

//
// lib call to readdir
//
ssize_t sys_user_readdir(int fd, struct dir *vdir)
{
  int tp = read_tp();
  struct dir *pdir = (struct dir *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), vdir);
  return do_readdir(fd, pdir);
}

//
// lib call to mkdir
//
ssize_t sys_user_mkdir(char *pathva)
{
  int tp = read_tp();
  char *pathpa = (char *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), pathva);
  return do_mkdir(pathpa);
}

//
// lib call to closedir
//
ssize_t sys_user_closedir(int fd)
{
  return do_closedir(fd);
}

//
// lib call to link
//
ssize_t sys_user_link(char *vfn1, char *vfn2)
{
  int tp = read_tp();
  char *pfn1 = (char *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), (void *)vfn1);
  char *pfn2 = (char *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), (void *)vfn2);
  return do_link(pfn1, pfn2);
}

//
// lib call to unlink
//
ssize_t sys_user_unlink(char *vfn)
{
  int tp = read_tp();
  char *pfn = (char *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), (void *)vfn);
  return do_unlink(pfn);
}

ssize_t sys_user_wait(int pid)
{
  int tp = read_tp();
  if (pid == -1)
  {
    while (1)
    {
      for (int i = 0; i < NPROC; i++)
      {
        if (procs[i].parent == current[tp] && procs[i].status == ZOMBIE)
        {
          pid = procs[i].pid;
          free_process(&procs[i]);
          return pid;
        }
      }
      current[tp]->status = BLOCKED;
      schedule();
    }
  }

  if (pid < 0 || pid >= NPROC || procs[pid].parent != current[tp])
  {
    return -1;
  }

  while (procs[pid].status != ZOMBIE)
  {
    current[tp]->status = BLOCKED;
    schedule();
    if (procs[pid].parent != current[tp] || procs[pid].status == FREE)
    {
      return -1;
    }
  }

  free_process(&procs[pid]);
  return pid;
}

ssize_t sys_user_exec(char *pathva, const char *argv)
{
  int tp = read_tp();
  char *pathpa = (char *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), pathva);
  char *argvpa = (char *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), (void *)argv);
  return do_exec(current[tp], pathpa, argvpa);
}

uint64 sys_user_semNew(uint64 init_val)
{
  return do_semNew(init_val);
};
uint64 sys_user_semP(uint64 sid)
{
  do_semP(sid);
  return 0;
};
uint64 sys_user_semV(uint64 sid)
{
  do_semV(sid);
  return 0;
};

ssize_t sys_user_print_backtrace(int depth)
{
  int tp = read_tp();
  trapframe *tf = current[tp]->trapframe;
  pagetable_t pagetable = (pagetable_t)current[tp]->pagetable;
  uint64 user_fp = tf->regs.s0;
  // sprint("S0%lx----------------\n", user_fp);

  if (user_fp == 0)
    return 0;

  void *pa = user_va_to_pa(pagetable, (void *)(user_fp - 8));
  // sprint("Pa%lx----------------\n", pa);
  if (!pa)
    return 0;
  uint64 user_fp_next = *(uint64 *)pa;

  for (int i = 0; i < depth; i++)
  {
    // sprint("User%lx----------------\n", user_fp_next);
    if (user_fp_next == 0)
      break;
    // sprint("?????????????\n");
    /* 从 (frame - 8) 读返回地址 */
    pa = user_va_to_pa(pagetable, (void *)(user_fp_next - 8));
    // sprint("?????????????\n");

    if (!pa)
      break;
    uint64 return_address = *(uint64 *)pa;
    // sprint("?????????????\n");

    char *name = find_func_name("bin/app_print_backtrace", return_address);
    // sprint("%lx\n", name);

    sprint("%s\n", name);

    if (strcmp(name, "main") == 0)
      break;

    pa = user_va_to_pa(pagetable, (void *)(user_fp_next - 16));
    if (!pa)
      break;
    user_fp_next = *(uint64 *)pa;
  }
  return 0;
}

ssize_t sys_user_read_cwd(char *pathva)
{
  int tp = read_tp();
  char *pathpa = (char *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), pathva);
  struct dentry *cwd = current[tp]->pfiles->cwd;
  return do_rcwd(cwd, pathpa);
}

ssize_t sys_user_change_cwd(char *pathva)
{
  int tp = read_tp();
  char *pathpa = (char *)user_va_to_pa((pagetable_t)(current[tp]->pagetable), pathva);
  struct dentry *cwd = current[tp]->pfiles->cwd;
  return do_ccwd(cwd, pathpa);
}

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7)
{
  switch (a0)
  {
  case SYS_user_print:
    return sys_user_print((const char *)a1, a2);
  case SYS_user_scanf:
    return sys_user_scanf((const char *)a1);
  case SYS_user_printpa:
    return sys_user_printpa(a1);
  case SYS_user_exit:
    return sys_user_exit(a1);
  // added @lab2_2
  case SYS_user_allocate_page:
    return sys_user_allocate_page();
  case SYS_user_free_page:
    return sys_user_free_page(a1);
  case SYS_user_better_allocate_page:
    return sys_user_better_allocate_page(a1);
  case SYS_user_better_free_page:
    return sys_user_better_free_page(a1);
  case SYS_user_fork:
    return sys_user_fork();
  case SYS_user_yield:
    return sys_user_yield();
  // added @lab4_1
  case SYS_user_open:
    return sys_user_open((char *)a1, a2);
  case SYS_user_read:
    return sys_user_read(a1, (char *)a2, a3);
  case SYS_user_write:
    return sys_user_write(a1, (char *)a2, a3);
  case SYS_user_lseek:
    return sys_user_lseek(a1, a2, a3);
  case SYS_user_stat:
    return sys_user_stat(a1, (struct istat *)a2);
  case SYS_user_disk_stat:
    return sys_user_disk_stat(a1, (struct istat *)a2);
  case SYS_user_close:
    return sys_user_close(a1);
  // added @lab4_2
  case SYS_user_opendir:
    return sys_user_opendir((char *)a1);
  case SYS_user_readdir:
    return sys_user_readdir(a1, (struct dir *)a2);
  case SYS_user_mkdir:
    return sys_user_mkdir((char *)a1);
  case SYS_user_closedir:
    return sys_user_closedir(a1);
  // added @lab4_3
  case SYS_user_link:
    return sys_user_link((char *)a1, (char *)a2);
  case SYS_user_unlink:
    return sys_user_unlink((char *)a1);
  case SYS_user_exec:
    return sys_user_exec((char *)a1, (char *)a2);
  case SYS_user_wait:
    return sys_user_wait(a1);

  case SYS_user_semNew:
    return sys_user_semNew(a1);
  case SYS_user_semP:
    return sys_user_semP(a1);
  case SYS_user_semV:
    return sys_user_semV(a1);

  case SYS_user_print_backtrace:
    return sys_user_print_backtrace(a1);

  // added lab4_c1
  case SYS_user_rcwd:
    return sys_user_read_cwd((char *)a1);
  case SYS_user_ccwd:
    return sys_user_change_cwd((char *)a1);

  default:
    panic("Unknown syscall %ld \n", a0);
  }
}
