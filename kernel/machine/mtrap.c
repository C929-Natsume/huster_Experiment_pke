#include "kernel/riscv.h"
#include "kernel/process.h"
#include "spike_interface/spike_utils.h"

#include "string.h"
#include "kernel/elf.h"

static void handle_instruction_access_fault() { panic("Instruction access fault!"); }

static void handle_load_access_fault()
{
  uint64 mepc = read_csr(mepc);
  uint64 mtval = read_csr(mtval);
  sprint("Load access fault! mepc=%p mtval=%p\n", mepc, mtval);
  panic("Load access fault!");
}

static void handle_store_access_fault() { panic("Store/AMO access fault!"); }

// static void handle_illegal_instruction() { panic("Illegal instruction!"); }

static void print_errorline()
{
  int tp = read_tp();
  uint64 epc = read_csr(mepc);
  addr_line *errorline = 0;
  code_file *errorfile = 0;
  // process *proc = ((elf_info *)(&elfloader[tp])->info)->p;
  process *proc = current[tp];
  // sprint("proc%lx\n", proc);
  // sprint("proc:%d,%s\n", proc->pid, proc->file->file);
  // sprint("epc%lx\n", epc);

  // position
  uint64 min = 0xffffffffffffffff;
  for (int i = 0; i < proc->line_ind; i++)
  {
    // sprint("ppppppp%lx\n", proc->line[i].addr);
    if ((proc->line + i)->addr >= epc)
    {
      if ((proc->line + i)->addr < min)
      {
        min = (proc->line + i)->addr;
        errorline = (proc->line) + i;
        break;
      }
    }
  }

  errorfile = proc->file + errorline->file;

  char errorpath[100];
  strcpy(errorpath, *(proc->dir + errorfile->dir));
  int dir_len = strlen(*(proc->dir + errorfile->dir));
  errorpath[dir_len] = '/';
  strcpy(errorpath + dir_len + 1, errorfile->file);

  sprint("Runtime error at %s:%d\n", errorpath, errorline->line);

  // content
  spike_file_t *open_errorfile = spike_file_open(errorpath, O_RDONLY, 0);
  int max_content = 1024 * 1024;
  char file_content[max_content];
  spike_file_pread(open_errorfile, file_content, max_content, 0);

  int code_start = 0;
  for (int i = 0; i < errorline->line - 1; i++)
  {
    while (file_content[code_start] != '\n')
    {
      code_start++;
    }
    code_start++;
  }

  int max_code = 1024;
  char errorcontent[max_code];
  for (int i = 0; i < max_code; i++)
  {
    if (file_content[code_start + i] == '\n')
    {
      break;
    }
    errorcontent[i] = file_content[code_start + i];
  }

  spike_file_close(open_errorfile);

  sprint("%s", errorcontent);
  sprint("\n");
};
// #define CODE_BUFFER_SIZE 1 << 14
// extern elf_ctx elfloader;
// char path[256];
// char code_buffer[CODE_BUFFER_SIZE];

// void print_errorline()
// {
//   // 1. read ELF .debug_line section, done in kernel/elf.c
//   // 2. parse the debug_line section, done in kernel/elf.c

//   // get the process in ctx
//   int tp = read_tp();
//   process *p = current[tp];
//   uint64 epc = read_csr(mepc);
//   // get line of epc
//   int idx = 0;
//   // bug fixed, int is not enough for minival
//   uint64 minival = 0xffffffffffffffff;
//   for (int i = 0; i < p->line_ind; i++)
//   {
//     if (p->line[i].addr >= epc)
//     {
//       if (p->line[i].addr < minival)
//       {
//         minival = p->line[i].addr;
//         idx = i;
//         break;
//       }
//     }
//   }
//   strcpy(path, p->dir[p->file[p->line[idx].file].dir]);
//   // strcat(path, "/");
//   // strcat(path, p->file[p->line[i].file].file);
//   // there's no strcat in util/string.h, so stupid!
//   sprint("????????1\n");

//   path[strlen(path)] = '/';
//   strcpy(path + strlen(path), p->file[p->line[idx].file].file);

//   sprint("Runtime error at %s:%d\n", path, p->line[idx].line);
//   // the raw code is not in .debug_line, how to get it?
//   // read the code from the file
//   spike_file_t *f = spike_file_open(path, O_RDONLY, 0);
//   if (IS_ERR_VALUE(f))
//   {
//     sprint("Fail to open the file %s\n", path);
//     return;
//   }
//   int len = spike_file_read(f, code_buffer, CODE_BUFFER_SIZE);
//   if (IS_ERR_VALUE(len))
//   {
//     sprint("Fail to read the file %s\n", path);
//     return;
//   }
//   code_buffer[len] = '\0';
//   // sprint("%s\n", code_buffer);
//   // could not print all codes, considering sprint limit
//   // sprint("\n*len = %d\n", len);
//   int line_count = 1;
//   for (int i = 0; i < len; i++)
//   {
//     if (line_count == p->line[idx].line)
//     {
//       sprint("%c", code_buffer[i]);
//       // break;
//     }
//     if (code_buffer[i] == '\n')
//     {
//       line_count++;
//     }
//   }
// }

static void handle_illegal_instruction()
{
  print_errorline();
  panic("Illegal instruction!");
}

static void handle_misaligned_load() { panic("Misaligned Load!"); }

static void handle_misaligned_store() { panic("Misaligned AMO!"); }

// added @lab1_3
static void handle_timer()
{
  int cpuid = read_csr(mhartid);
  // setup the timer fired at next time (TIMER_INTERVAL from now)
  *(uint64 *)CLINT_MTIMECMP(cpuid) = *(uint64 *)CLINT_MTIMECMP(cpuid) + TIMER_INTERVAL;

  // setup a soft interrupt in sip (S-mode Interrupt Pending) to be handled in S-mode
  write_csr(sip, SIP_SSIP);
}

//
// handle_mtrap calls a handling function according to the type of a machine mode interrupt (trap).
//
void handle_mtrap()
{
  uint64 mcause = read_csr(mcause);
  switch (mcause)
  {
  case CAUSE_MTIMER:
    handle_timer();
    break;
  case CAUSE_FETCH_ACCESS:
    handle_instruction_access_fault();
    break;
  case CAUSE_LOAD_ACCESS:
    handle_load_access_fault();
  case CAUSE_STORE_ACCESS:
    handle_store_access_fault();
    break;
  case CAUSE_ILLEGAL_INSTRUCTION:
    // TODO (lab1_2): call handle_illegal_instruction to implement illegal instruction
    // interception, and finish lab1_2.
    handle_illegal_instruction();

    break;
  case CAUSE_MISALIGNED_LOAD:
    handle_misaligned_load();
    break;
  case CAUSE_MISALIGNED_STORE:
    handle_misaligned_store();
    break;

  default:
    sprint("machine trap(): unexpected mscause %p\n", mcause);
    sprint("            mepc=%p mtval=%p\n", read_csr(mepc), read_csr(mtval));
    panic("unexpected exception happened in M-mode.\n");
    break;
  }
}
