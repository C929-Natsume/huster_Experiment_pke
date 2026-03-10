/*
 * routines that scan and load a (host) Executable and Linkable Format (ELF) file
 * into the (emulated) memory.
 */

#include "elf.h"
#include "string.h"
#include "riscv.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"
#include "spike_interface/spike_utils.h"
#include "sched.h"

elf_ctx elfloader[NCPU];

//
// the implementation of allocater. allocates memory space for later segment loading.
// this allocater is heavily modified @lab2_1, where we do NOT work in bare mode.
//
static void *elf_alloc_mb(elf_ctx *ctx, uint64 elf_pa, uint64 elf_va, uint64 size)
{
  elf_info *msg = (elf_info *)ctx->info;
  // we assume that size of proram segment is smaller than a page.
  kassert(size < PGSIZE);
  void *pa = alloc_page();
  if (pa == 0)
    kill_current_process("uvmalloc mem alloc falied\n", -1);

  memset((void *)pa, 0, PGSIZE);
  // sprint("elf_alloc_mb: elf_va=%p, pa=%p\n", elf_va, pa);
  // sprint("elf_alloc_mb: pagetable=%p\n", (pagetable_t)msg->p->pagetable);
  // sprint("elf_alloc_mb: msg->p=%p\n", msg->p);
  // sprint("elf_alloc_mb: msg->p->pid=%d\n", msg->p->pid);
  user_vm_map((pagetable_t)msg->p->pagetable, elf_va, PGSIZE, (uint64)pa,
              prot_to_type(PROT_WRITE | PROT_READ | PROT_EXEC, 1));
  // sprint("elf_alloc_mb: tp=%d pid=%d elf_va=%p, pa=%p\n", read_tp(), msg->p->pid, elf_va, pa);
  // sprint("elf_alloc_mb: user_vm_map done\n");
  return pa;
}

//
// actual file reading, using the vfs file interface.
//
static uint64 elf_fpread(elf_ctx *ctx, void *dest, uint64 nb, uint64 offset)
{
  elf_info *msg = (elf_info *)ctx->info;
  vfs_lseek(msg->f, offset, SEEK_SET);
  return vfs_read(msg->f, dest, nb);
}

//
// init elf_ctx, a data structure that loads the elf.
//
elf_status elf_init(elf_ctx *ctx, void *info)
{
  ctx->info = info;

  // load the elf header
  if (elf_fpread(ctx, &ctx->ehdr, sizeof(ctx->ehdr), 0) != sizeof(ctx->ehdr))
    return EL_EIO;

  // check the signature (magic value) of the elf
  if (ctx->ehdr.magic != ELF_MAGIC)
    return EL_NOTELF;

  return EL_OK;
}

// leb128 (little-endian base 128) is a variable-length
// compression algoritm in DWARF
void read_uleb128(uint64 *out, char **off)
{
  uint64 value = 0;
  int shift = 0;
  uint8 b;
  for (;;)
  {
    b = *(uint8 *)(*off);
    (*off)++;
    value |= ((uint64)b & 0x7F) << shift;
    shift += 7;
    if ((b & 0x80) == 0)
      break;
  }
  if (out)
    *out = value;
}
void read_sleb128(int64 *out, char **off)
{
  int64 value = 0;
  int shift = 0;
  uint8 b;
  for (;;)
  {
    b = *(uint8 *)(*off);
    (*off)++;
    value |= ((uint64_t)b & 0x7F) << shift;
    shift += 7;
    if ((b & 0x80) == 0)
      break;
  }
  if (shift < 64 && (b & 0x40))
    value |= -(1 << shift);
  if (out)
    *out = value;
}
// Since reading below types through pointer cast requires aligned address,
// so we can only read them byte by byte
void read_uint64(uint64 *out, char **off)
{
  *out = 0;
  for (int i = 0; i < 8; i++)
  {
    *out |= (uint64)(**off) << (i << 3);
    (*off)++;
  }
}
void read_uint32(uint32 *out, char **off)
{
  *out = 0;
  for (int i = 0; i < 4; i++)
  {
    *out |= (uint32)(**off) << (i << 3);
    (*off)++;
  }
}
void read_uint16(uint16 *out, char **off)
{
  *out = 0;
  for (int i = 0; i < 2; i++)
  {
    *out |= (uint16)(**off) << (i << 3);
    (*off)++;
  }
}

/*
 * analyzis the data in the debug_line section
 *
 * the function needs 3 parameters: elf context, data in the debug_line section
 * and length of debug_line section
 *
 * make 3 arrays:
 * "process->dir" stores all directory paths of code files
 * "process->file" stores all code file names of code files and their directory path index of array "dir"
 * "process->line" stores all relationships map instruction addresses to code line numbers
 * and their code file name index of array "file"
 */
void make_addr_line(elf_ctx *ctx, char *debug_line, uint64 length)
{
  process *p = ((elf_info *)ctx->info)->p;
  p->debugline = debug_line;
  // directory name char pointer array
  p->dir = (char **)((((uint64)debug_line + length + 7) >> 3) << 3);
  int dir_ind = 0, dir_base;
  // file name char pointer array
  p->file = (code_file *)(p->dir + 64);
  int file_ind = 0, file_base;
  // table array
  p->line = (addr_line *)(p->file + 64);
  p->line_ind = 0;
  char *off = debug_line;
  while (off < debug_line + length)
  { // iterate each compilation unit(CU)
    debug_header *dh = (debug_header *)off;
    off += sizeof(debug_header);
    dir_base = dir_ind;
    file_base = file_ind;
    // get directory name char pointer in this CU
    while (*off != 0)
    {
      p->dir[dir_ind++] = off;
      while (*off != 0)
        off++;
      off++;
    }
    off++;
    // get file name char pointer in this CU
    while (*off != 0)
    {
      p->file[file_ind].file = off;
      while (*off != 0)
        off++;
      off++;
      uint64 dir;
      read_uleb128(&dir, &off);
      p->file[file_ind++].dir = dir - 1 + dir_base;
      read_uleb128(NULL, &off);
      read_uleb128(NULL, &off);
    }
    off++;
    addr_line regs;
    regs.addr = 0;
    regs.file = 1;
    regs.line = 1;
    // simulate the state machine op code
    for (;;)
    {
      uint8 op = *(off++);
      switch (op)
      {
      case 0: // Extended Opcodes
        read_uleb128(NULL, &off);
        op = *(off++);
        switch (op)
        {
        case 1: // DW_LNE_end_sequence
          if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr)
            p->line_ind--;
          p->line[p->line_ind] = regs;
          p->line[p->line_ind].file += file_base - 1;
          p->line_ind++;
          goto endop;
        case 2: // DW_LNE_set_address
          read_uint64(&regs.addr, &off);
          break;
        // ignore DW_LNE_define_file
        case 4: // DW_LNE_set_discriminator
          read_uleb128(NULL, &off);
          break;
        }
        break;
      case 1: // DW_LNS_copy
        if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr)
          p->line_ind--;
        p->line[p->line_ind] = regs;
        p->line[p->line_ind].file += file_base - 1;
        p->line_ind++;
        break;
      case 2:
      { // DW_LNS_advance_pc
        uint64 delta;
        read_uleb128(&delta, &off);
        regs.addr += delta * dh->min_instruction_length;
        break;
      }
      case 3:
      { // DW_LNS_advance_line
        int64 delta;
        read_sleb128(&delta, &off);
        regs.line += delta;
        break;
      }
      case 4: // DW_LNS_set_file
        read_uleb128(&regs.file, &off);
        break;
      case 5: // DW_LNS_set_column
        read_uleb128(NULL, &off);
        break;
      case 6: // DW_LNS_negate_stmt
      case 7: // DW_LNS_set_basic_block
        break;
      case 8:
      { // DW_LNS_const_add_pc
        int adjust = 255 - dh->opcode_base;
        int delta = (adjust / dh->line_range) * dh->min_instruction_length;
        regs.addr += delta;
        break;
      }
      case 9:
      { // DW_LNS_fixed_advanced_pc
        uint16 delta;
        read_uint16(&delta, &off);
        regs.addr += delta;
        break;
      }
        // ignore 10, 11 and 12
      default:
      { // Special Opcodes
        int adjust = op - dh->opcode_base;
        int addr_delta = (adjust / dh->line_range) * dh->min_instruction_length;
        int line_delta = dh->line_base + (adjust % dh->line_range);
        regs.addr += addr_delta;
        regs.line += line_delta;
        if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr)
          p->line_ind--;
        p->line[p->line_ind] = regs;
        p->line[p->line_ind].file += file_base - 1;
        p->line_ind++;
        break;
      }
      }
    }
  endop:;
  }
  // for (int i = 0; i < p->line_ind; i++)
  //   sprint("%p %d %d %s\n", p->line[i].addr, p->line[i].line, p->line[i].file, p->file[p->line[i].file].file);
}

//
// load the elf segments to memory regions.
//
elf_status elf_load(elf_ctx *ctx)
{
  // elf_prog_header structure is defined in kernel/elf.h
  elf_prog_header ph_addr;
  int i, off;

  // traverse the elf program segment headers
  for (i = 0, off = ctx->ehdr.phoff; i < ctx->ehdr.phnum; i++, off += sizeof(ph_addr))
  {
    // read segment headers
    if (elf_fpread(ctx, (void *)&ph_addr, sizeof(ph_addr), off) != sizeof(ph_addr))
      return EL_EIO;

    if (ph_addr.type != ELF_PROG_LOAD)
      continue;
    if (ph_addr.memsz < ph_addr.filesz)
      return EL_ERR;
    if (ph_addr.vaddr + ph_addr.memsz < ph_addr.vaddr)
      return EL_ERR;

    // allocate memory block before elf loading
    void *dest = elf_alloc_mb(ctx, ph_addr.vaddr, ph_addr.vaddr, ph_addr.memsz);

    // actual loading
    if (elf_fpread(ctx, dest, ph_addr.memsz, ph_addr.off) != ph_addr.memsz)
      return EL_EIO;

    // record the vm region in proc->mapped_info. added @lab3_1
    int j;
    for (j = 0; j < PGSIZE / sizeof(mapped_region); j++) // seek the last mapped region
      if ((process *)(((elf_info *)(ctx->info))->p)->mapped_info[j].va == 0x0)
        break;

    ((process *)(((elf_info *)(ctx->info))->p))->mapped_info[j].va = ph_addr.vaddr;
    ((process *)(((elf_info *)(ctx->info))->p))->mapped_info[j].npages = 1;

    // SEGMENT_READABLE, SEGMENT_EXECUTABLE, SEGMENT_WRITABLE are defined in kernel/elf.h
    if (ph_addr.flags == (SEGMENT_READABLE | SEGMENT_EXECUTABLE))
    {
      ((process *)(((elf_info *)(ctx->info))->p))->mapped_info[j].seg_type = CODE_SEGMENT;
      // sprint("CODE_SEGMENT added at mapped info offset:%d\n", j);
    }
    else if (ph_addr.flags == (SEGMENT_READABLE | SEGMENT_WRITABLE))
    {
      ((process *)(((elf_info *)(ctx->info))->p))->mapped_info[j].seg_type = DATA_SEGMENT;
      // sprint("DATA_SEGMENT added at mapped info offset:%d\n", j);
    }
    else
      kill_current_process("unknown program segment encountered, segment flag:%d.\n", ph_addr.flags);

    ((process *)(((elf_info *)(ctx->info))->p))->total_mapped_region++;
  }

  return EL_OK;
}

//
// load the elf of user application, by using the spike file interface.
//
void load_bincode_from_host_elf(process *p, char *filename)
{
  int tp = read_tp();
  // sprint("Application: %s\n", filename);

  // elf loading. elf_ctx is defined in kernel/elf.h, used to track the loading process.
  // elf_ctx elfloader;
  // elf_info is defined above, used to tie the elf file and its corresponding process.
  elf_info info;

  info.f = vfs_open(filename, O_RDONLY);
  info.p = p;
  // IS_ERR_VALUE is a macro defined in spike_interface/spike_htif.h
  if (IS_ERR_VALUE(info.f))
    kill_current_process("Fail on openning the input application program.\n", -1);

  // init elfloader context. elf_init() is defined above.
  if (elf_init(&elfloader[tp], &info) != EL_OK)
    kill_current_process("fail to init elfloader.\n", -1);

  // load elf. elf_load() is defined above.
  if (elf_load(&elfloader[tp]) != EL_OK)
    kill_current_process("Fail on loading elf.\n", -1);

  // load .debug_line
  if (load_debug_line(&elfloader[tp]) != EL_OK)
    kill_current_process("Fail on loading .debug_line.\n", -1);

  // entry (virtual, also physical in lab1_x) address
  p->trapframe->epc = elfloader[tp].ehdr.entry;

  // close the vfs file
  vfs_close(info.f);

  // sprint("Application program entry point (virtual address): 0x%lx\n", p->trapframe->epc);
}

// load
#define MAX_ARGV_LEN 256

int do_exec(process *p, char *path, char *argv)
{
  int tp = read_tp();
  char argv_buf[MAX_ARGV_LEN];
  if (argv)
  {
    size_t len = 0;
    while (len < MAX_ARGV_LEN - 1 && ((char *)argv)[len] != '\0')
      len++;
    len++;
    if (len > MAX_ARGV_LEN)
      len = MAX_ARGV_LEN;
    memcpy(argv_buf, (void *)argv, len);
    argv_buf[MAX_ARGV_LEN - 1] = '\0';
  }
  else
  {
    argv_buf[0] = '\0';
  }

  clr_proc(p);
  // sprint("Application: %s\n", path);

  // elf_ctx elfloader;
  elf_info info;

  info.f = vfs_open(path, O_RDONLY);
  info.p = p;
  if (IS_ERR_VALUE(info.f))
    kill_current_process("Fail on openning the input application program.\n", -1);

  if (elf_init(&elfloader[tp], &info) != EL_OK)
    kill_current_process("fail to init elfloader.\n", -1);

  if (elf_load(&elfloader[tp]) != EL_OK)
    kill_current_process("Fail on loading elf.\n", -1);

  // load .debug_line
  if (load_debug_line(&elfloader[tp]) != EL_OK)
    kill_current_process("Fail on loading .debug_line.\n", -1);

  p->trapframe->epc = elfloader[tp].ehdr.entry;
  vfs_close(info.f);

  // sprint("Application program entry point (virtual address): 0x%lx\n", p->trapframe->epc);

  uint64 va = p->user_heap.heap_top;
  void *pa = alloc_page();
  p->user_heap.heap_top += PGSIZE;
  p->mapped_info[HEAP_SEGMENT].npages++;
  user_vm_map((pagetable_t)p->pagetable, va, PGSIZE, (uint64)pa, prot_to_type(PROT_WRITE | PROT_READ, 1));

  p->trapframe->regs.a0 = 1;
  p->trapframe->regs.a1 = va;
  p->trapframe->regs.tp = read_tp();

  uint64 va_str = p->user_heap.heap_top;
  void *pa_str = alloc_page();
  p->user_heap.heap_top += PGSIZE;
  p->mapped_info[HEAP_SEGMENT].npages++;
  memset(pa_str, 0, PGSIZE);
  memcpy(pa_str, argv_buf, MAX_ARGV_LEN);
  user_vm_map((pagetable_t)p->pagetable, va_str, PGSIZE, (uint64)pa_str, prot_to_type(PROT_WRITE | PROT_READ, 1));

  *((char **)pa) = (char *)va_str;

  return 0;
}

char *find_func_name(char *path, uint64 return_address)
{
  // sprint("-------------%s---------------", path);
  static char func_name_buf[256];

// 定义静态缓冲区
#define MAX_SECTIONS 256
#define MAX_STRTAB_SIZE 65536
#define MAX_SYMTAB_SIZE 1048576

  static Elf64_Shdr shdr_buf[MAX_SECTIONS];
  static char shstrtab_buf[MAX_STRTAB_SIZE];
  static char strtab_buf[MAX_STRTAB_SIZE];
  static Elf64_Sym symtab_buf[MAX_SYMTAB_SIZE / sizeof(Elf64_Sym)];

  struct file *f = vfs_open(path, O_RDONLY);
  if (!f)
    return NULL;

  // 读取 ELF header
  elf_header ehdr;
  vfs_lseek(f, 0, SEEK_SET);
  if (vfs_read(f, (char *)&ehdr, sizeof(ehdr)) != sizeof(ehdr))
  {
    vfs_close(f);
    return NULL;
  }

  // 检查 magic number
  if (ehdr.magic != ELF_MAGIC)
  {
    vfs_close(f);
    return NULL;
  }

  // 检查 Section Header 数量
  if (ehdr.shnum > MAX_SECTIONS)
  {
    vfs_close(f);
    return NULL;
  }

  // 读取所有 Section Headers
  vfs_lseek(f, ehdr.shoff, SEEK_SET);
  if (vfs_read(f, (char *)shdr_buf,
               ehdr.shnum * sizeof(Elf64_Shdr)) != (ssize_t)(ehdr.shnum * sizeof(Elf64_Shdr)))
  {
    vfs_close(f);
    return NULL;
  }

  // 读取 .shstrtab
  Elf64_Shdr *shstrtab_shdr = &shdr_buf[ehdr.shstrndx];
  if (shstrtab_shdr->sh_size > MAX_STRTAB_SIZE)
  {
    vfs_close(f);
    return NULL;
  }

  vfs_lseek(f, shstrtab_shdr->sh_offset, SEEK_SET);
  if (vfs_read(f, shstrtab_buf, shstrtab_shdr->sh_size) != (ssize_t)shstrtab_shdr->sh_size)
  {
    vfs_close(f);
    return NULL;
  }

  // .symtab & .strtab
  Elf64_Shdr *symtab_shdr = NULL;
  Elf64_Shdr *strtab_shdr = NULL;

  for (int i = 0; i < ehdr.shnum; i++)
  {
    const char *name = shstrtab_buf + shdr_buf[i].sh_name;
    if (strcmp(name, ".symtab") == 0)
    {
      symtab_shdr = &shdr_buf[i];
    }
    else if (strcmp(name, ".strtab") == 0)
    {
      strtab_shdr = &shdr_buf[i];
    }
  }

  if (!symtab_shdr || !strtab_shdr)
  {
    vfs_close(f);
    return NULL;
  }

  // 检查大小
  if (strtab_shdr->sh_size > MAX_STRTAB_SIZE ||
      symtab_shdr->sh_size > MAX_SYMTAB_SIZE)
  {
    vfs_close(f);
    return NULL;
  }

  // 读取 .strtab
  vfs_lseek(f, strtab_shdr->sh_offset, SEEK_SET);
  if (vfs_read(f, strtab_buf, strtab_shdr->sh_size) != (ssize_t)strtab_shdr->sh_size)
  {
    vfs_close(f);
    return NULL;
  }

  // 读取 .symtab
  int num_symbols = symtab_shdr->sh_size / sizeof(Elf64_Sym);
  if (num_symbols > (MAX_SYMTAB_SIZE / sizeof(Elf64_Sym)))
  {
    vfs_close(f);
    return NULL;
  }

  vfs_lseek(f, symtab_shdr->sh_offset, SEEK_SET);
  if (vfs_read(f, (char *)symtab_buf, symtab_shdr->sh_size) != (ssize_t)symtab_shdr->sh_size)
  {
    vfs_close(f);
    return NULL;
  }

  // 查找匹配的符号
  Elf64_Sym *best_match = NULL;

  for (int i = 0; i < num_symbols; i++)
  {
    uint8_t type = ELF64_ST_TYPE(symtab_buf[i].st_info);
    if (type != STT_FUNC)
      continue;
    if (symtab_buf[i].st_value == 0)
      continue;

    if (symtab_buf[i].st_value <= return_address)
    {
      if (symtab_buf[i].st_size == 0)
      {
        if (symtab_buf[i].st_value == return_address)
        {
          if (!best_match || symtab_buf[i].st_value > best_match->st_value)
          {
            best_match = &symtab_buf[i];
          }
        }
      }
      else
      {
        if (return_address < symtab_buf[i].st_value + symtab_buf[i].st_size)
        {
          if (!best_match || symtab_buf[i].st_value > best_match->st_value)
          {
            best_match = &symtab_buf[i];
          }
        }
      }
    }
  }

  char *result = NULL;
  if (best_match)
  {
    const char *name = strtab_buf + best_match->st_name;
    char *dest = func_name_buf;
    size_t max_len = sizeof(func_name_buf) - 1;
    size_t i = 0;
    while (i < max_len && name[i] != '\0')
    {
      dest[i] = name[i];
      i++;
    }
    dest[i] = '\0';
    result = func_name_buf;
  }

  vfs_close(f);
  return result;
}

// 定义静态缓冲区大小
#define MAX_SHSTRTAB_SIZE (64 * 1024)     // 节名称字符串表最大 64 KB
#define MAX_DEBUG_LINE_SIZE (1024 * 1024) // .debug_line 节最大 1 MB

static char shstrtab_buf[MAX_SHSTRTAB_SIZE];
static char debug_line_buf[MAX_DEBUG_LINE_SIZE];

elf_status load_debug_line(elf_ctx *ctx)
{
  // 读取节头字符串表节头
  elf_sect_header shstr_hdr;
  size_t shstr_off = ctx->ehdr.shoff + ctx->ehdr.shstrndx * sizeof(elf_sect_header);
  if (elf_fpread(ctx, &shstr_hdr, sizeof(shstr_hdr), shstr_off) != sizeof(shstr_hdr))
  {
    return EL_ERR;
  }
  if (shstr_hdr.size > MAX_SHSTRTAB_SIZE)
  {
    return EL_ERR;
  }

  // 读取所有节名称
  if (elf_fpread(ctx, shstrtab_buf, shstr_hdr.size, shstr_hdr.offset) != shstr_hdr.size)
  {
    return EL_ERR;
  }

  // 查找 .debug_line 节
  elf_sect_header debug_hdr;
  int found = 0;
  for (uint16_t i = 0; i < ctx->ehdr.shnum; ++i)
  {
    elf_sect_header sh;
    size_t off = ctx->ehdr.shoff + i * sizeof(elf_sect_header);
    if (elf_fpread(ctx, &sh, sizeof(sh), off) != sizeof(sh))
    {
      return EL_ERR;
    }
    if (strcmp(shstrtab_buf + sh.name, ".debug_line") == 0)
    {
      debug_hdr = sh;
      found = 1;
      break;
    }
  }

  // sprint("\n\nfound:%d\n\n", found);

  if (debug_hdr.size > MAX_DEBUG_LINE_SIZE)
  {
    return EL_ERR;
  }

  // 读取节数据
  if (elf_fpread(ctx, debug_line_buf, debug_hdr.size, debug_hdr.offset) != debug_hdr.size)
  {
    return EL_ERR;
  }

  // sprint("debug:%s\n", debug_line_buf);

  // 解析行号信息
  make_addr_line(ctx, debug_line_buf, debug_hdr.size);
  return EL_OK;
}