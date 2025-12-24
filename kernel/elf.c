/*
 * routines that scan and load a (host) Executable and Linkable Format (ELF) file
 * into the (emulated) memory.
 */

#include "elf.h"
#include "string.h"
#include "riscv.h"
#include "spike_interface/spike_utils.h"

typedef struct elf_info_t {
  spike_file_t *f;
  process *p;
} elf_info;

//
// the implementation of allocater. allocates memory space for later segment loading
//
static void *elf_alloc_mb(elf_ctx *ctx, uint64 elf_pa, uint64 elf_va, uint64 size) {
  // directly returns the virtual address as we are in the Bare mode in lab1_x
  return (void *)elf_va;
}

//
// actual file reading, using the spike file interface.
//
static uint64 elf_fpread(elf_ctx *ctx, void *dest, uint64 nb, uint64 offset) {
  elf_info *msg = (elf_info *)ctx->info;
  // call spike file utility to load the content of elf file into memory.
  // spike_file_pread will read the elf file (msg->f) from offset to memory (indicated by
  // *dest) for nb bytes.
  return spike_file_pread(msg->f, dest, nb, offset);
}

//
// init elf_ctx, a data structure that loads the elf.
//
elf_status elf_init(elf_ctx *ctx, void *info) {
  ctx->info = info;

  // load the elf header
  if (elf_fpread(ctx, &ctx->ehdr, sizeof(ctx->ehdr), 0) != sizeof(ctx->ehdr)) return EL_EIO;

  // check the signature (magic value) of the elf
  if (ctx->ehdr.magic != ELF_MAGIC) return EL_NOTELF;

  return EL_OK;
}

//
// load the elf segments to memory regions as we are in Bare mode in lab1
//
elf_status elf_load(elf_ctx *ctx) {
  // elf_prog_header structure is defined in kernel/elf.h
  elf_prog_header ph_addr;
  int i, off;

  // traverse the elf program segment headers
  for (i = 0, off = ctx->ehdr.phoff; i < ctx->ehdr.phnum; i++, off += sizeof(ph_addr)) {
    // read segment headers
    if (elf_fpread(ctx, (void *)&ph_addr, sizeof(ph_addr), off) != sizeof(ph_addr)) return EL_EIO;

    if (ph_addr.type != ELF_PROG_LOAD) continue;
    if (ph_addr.memsz < ph_addr.filesz) return EL_ERR;
    if (ph_addr.vaddr + ph_addr.memsz < ph_addr.vaddr) return EL_ERR;

    // allocate memory block before elf loading
    void *dest = elf_alloc_mb(ctx, ph_addr.vaddr, ph_addr.vaddr, ph_addr.memsz);

    // actual loading
    if (elf_fpread(ctx, dest, ph_addr.memsz, ph_addr.off) != ph_addr.memsz)
      return EL_EIO;
  }

  return EL_OK;
}

typedef union {
  uint64 buf[MAX_CMDLINE_ARGS];
  char *argv[MAX_CMDLINE_ARGS];
} arg_buf;

//
// returns the number (should be 1) of string(s) after PKE kernel in command line.
// and store the string(s) in arg_bug_msg.
//
static size_t parse_args(arg_buf *arg_bug_msg) {
  // HTIFSYS_getmainvars frontend call reads command arguments to (input) *arg_bug_msg
  long r = frontend_syscall(HTIFSYS_getmainvars, (uint64)arg_bug_msg,
      sizeof(*arg_bug_msg), 0, 0, 0, 0, 0);
  kassert(r == 0);

  size_t pk_argc = arg_bug_msg->buf[0];
  uint64 *pk_argv = &arg_bug_msg->buf[1];

  int arg = 1;  // skip the PKE OS kernel string, leave behind only the application name
  for (size_t i = 0; arg + i < pk_argc; i++)
    arg_bug_msg->argv[i] = (char *)(uintptr_t)pk_argv[arg + i];

  //returns the number of strings after PKE kernel in command line
  return pk_argc - arg;
}

//
// load the elf of user application, by using the spike file interface.
//
void load_bincode_from_host_elf(process *p) {
  arg_buf arg_bug_msg;

  // retrieve command line arguements
  size_t argc = parse_args(&arg_bug_msg);
  if (!argc) panic("You need to specify the application program!\n");

  sprint("Application: %s\n", arg_bug_msg.argv[0]);

  //elf loading. elf_ctx is defined in kernel/elf.h, used to track the loading process.
  elf_ctx elfloader;
  // elf_info is defined above, used to tie the elf file and its corresponding process.
  elf_info info;

  info.f = spike_file_open(arg_bug_msg.argv[0], O_RDONLY, 0);
  info.p = p;
  // IS_ERR_VALUE is a macro defined in spike_interface/spike_htif.h
  if (IS_ERR_VALUE(info.f)) panic("Fail on openning the input application program.\n");

  // init elfloader context. elf_init() is defined above.
  if (elf_init(&elfloader, &info) != EL_OK)
    panic("fail to init elfloader.\n");

  // load elf. elf_load() is defined above.
  if (elf_load(&elfloader) != EL_OK) panic("Fail on loading elf.\n");

  // entry (virtual, also physical in lab1_x) address
  p->trapframe->epc = elfloader.ehdr.entry;

  // close the host spike file
  spike_file_close( info.f );

  sprint("Application program entry point (virtual address): 0x%lx\n", p->trapframe->epc);
}

char* find_func_name(uint64 return_address){
  static char func_name_buf[256];
  
  // 定义静态缓冲区
  #define MAX_SECTIONS 256
  #define MAX_STRTAB_SIZE 65536
  #define MAX_SYMTAB_SIZE 1048576
  
  static Elf64_Shdr shdr_buf[MAX_SECTIONS];
  static char shstrtab_buf[MAX_STRTAB_SIZE];
  static char strtab_buf[MAX_STRTAB_SIZE];
  static Elf64_Sym symtab_buf[MAX_SYMTAB_SIZE / sizeof(Elf64_Sym)];

  arg_buf arg_bug_msg;
  size_t argc = parse_args(&arg_bug_msg);
  if (!argc) return NULL;
  
  const char *elf_path = arg_bug_msg.argv[0]; 
  spike_file_t *f = spike_file_open(elf_path, O_RDONLY, 0);
  if (IS_ERR_VALUE(f)) 
    return NULL;
  
  // 读取 ELF header
  elf_header ehdr;
  if (spike_file_pread(f, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr)) {
    spike_file_close(f);
    return NULL;
  }

  // 检查 magic number
  if (ehdr.magic != ELF_MAGIC) {
    spike_file_close(f);
    return NULL;
  }

  // 检查 Section Header 数量
  if (ehdr.shnum > MAX_SECTIONS) {
    spike_file_close(f);
    return NULL;
  }

  // 读取所有 Section Headers
  if (spike_file_pread(f, shdr_buf, 
                      ehdr.shnum * sizeof(Elf64_Shdr), 
                      ehdr.shoff) != ehdr.shnum * sizeof(Elf64_Shdr)) {
    spike_file_close(f);
    return NULL;
  }

  // 读取 .shstrtab
  Elf64_Shdr *shstrtab_shdr = &shdr_buf[ehdr.shstrndx];
  if (shstrtab_shdr->sh_size > MAX_STRTAB_SIZE) {
    spike_file_close(f);
    return NULL;
  }
  
  if (spike_file_pread(f, shstrtab_buf, shstrtab_shdr->sh_size, 
                      shstrtab_shdr->sh_offset) != shstrtab_shdr->sh_size) {
    spike_file_close(f);
    return NULL;
  }

  // .symtab & .strtab
  Elf64_Shdr *symtab_shdr = NULL;
  Elf64_Shdr *strtab_shdr = NULL;
  
  for (int i = 0; i < ehdr.shnum; i++) {
    const char *name = shstrtab_buf + shdr_buf[i].sh_name;
    if (strcmp(name, ".symtab") == 0) {
      symtab_shdr = &shdr_buf[i];
    } else if (strcmp(name, ".strtab") == 0) {
      strtab_shdr = &shdr_buf[i];
    }
  }

  if (!symtab_shdr || !strtab_shdr) {
    spike_file_close(f);
    return NULL;
  }

  // 检查大小
  if (strtab_shdr->sh_size > MAX_STRTAB_SIZE || 
      symtab_shdr->sh_size > MAX_SYMTAB_SIZE) {
    spike_file_close(f);
    return NULL;
  }

  // 读取 .strtab
  if (spike_file_pread(f, strtab_buf, strtab_shdr->sh_size, 
                      strtab_shdr->sh_offset) != strtab_shdr->sh_size) {
    spike_file_close(f);
    return NULL;
  }

  // 读取 .symtab
  int num_symbols = symtab_shdr->sh_size / sizeof(Elf64_Sym);
  if (num_symbols > (MAX_SYMTAB_SIZE / sizeof(Elf64_Sym))) {
    spike_file_close(f);
    return NULL;
  }
  
  if (spike_file_pread(f, symtab_buf, symtab_shdr->sh_size, 
                      symtab_shdr->sh_offset) != symtab_shdr->sh_size) {
    spike_file_close(f);
    return NULL;
  }

  // 查找匹配的符号
  Elf64_Sym *best_match = NULL;
  
  for (int i = 0; i < num_symbols; i++) {
    uint8_t type = ELF64_ST_TYPE(symtab_buf[i].st_info);
    if (type != STT_FUNC) continue;
    if (symtab_buf[i].st_value == 0) continue;
    
    if (symtab_buf[i].st_value <= return_address) {
      if (symtab_buf[i].st_size == 0) {
        if (symtab_buf[i].st_value == return_address) {
          if (!best_match || symtab_buf[i].st_value > best_match->st_value) {
            best_match = &symtab_buf[i];
          }
        }
      } else {
        if (return_address < symtab_buf[i].st_value + symtab_buf[i].st_size) {
          if (!best_match || symtab_buf[i].st_value > best_match->st_value) {
            best_match = &symtab_buf[i];
          }
        }
      }
    }
  }

  char *result = NULL;
  if (best_match) {
    const char *name = strtab_buf + best_match->st_name;
    char *dest = func_name_buf;
    size_t max_len = sizeof(func_name_buf) - 1;
    size_t i = 0;
    while (i < max_len && name[i] != '\0') {
      dest[i] = name[i];
      i++;
    }
    dest[i] = '\0'; 
    result = func_name_buf;
  }

  spike_file_close(f);
  return result;
}