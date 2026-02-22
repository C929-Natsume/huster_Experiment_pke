#include "kernel/riscv.h"
#include "kernel/process.h"
#include "spike_interface/spike_utils.h"
#include "string.h"

static void handle_instruction_access_fault() { panic("Instruction access fault!"); }

static void handle_load_access_fault() { panic("Load access fault!"); }

static void handle_store_access_fault() { panic("Store/AMO access fault!"); }

static void print_errorline()
{
  uint64 epc = read_csr(mepc);
  addr_line *errorline = 0;
  code_file *errorfile = 0;

  // position
  for (int i = 0; i < current->line_ind; i++)
  {
    if ((current->line + i)->addr == epc)
    {
      errorline = (current->line) + i;
      break;
    }
    if (i == current->line_ind)
    {
      panic("Fail to get errorline address.\n");
    }
  }
  errorfile = current->file + errorline->file;

  char errorpath[100];
  strcpy(errorpath, *(current->dir + errorfile->dir));
  int dir_len = strlen(*(current->dir + errorfile->dir));
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
  int cpuid = 0;
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
