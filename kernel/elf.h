#ifndef _ELF_H_
#define _ELF_H_

#include "util/types.h"
#include "process.h"

#define MAX_CMDLINE_ARGS 64

// elf header structure
typedef struct elf_header_t {
  uint32 magic;
  uint8 elf[12];
  uint16 type;      /* Object file type */
  uint16 machine;   /* Architecture */
  uint32 version;   /* Object file version */
  uint64 entry;     /* Entry point virtual address */
  uint64 phoff;     /* Program header table file offset */
  uint64 shoff;     /* Section header table file offset */
  uint32 flags;     /* Processor-specific flags */
  uint16 ehsize;    /* ELF header size in bytes */
  uint16 phentsize; /* Program header table entry size */
  uint16 phnum;     /* Program header table entry count */
  uint16 shentsize; /* Section header table entry size */
  uint16 shnum;     /* Section header table entry count */
  uint16 shstrndx;  /* Section header string table index */
} elf_header;

// Program segment header.
typedef struct elf_prog_header_t {
  uint32 type;   /* Segment type */
  uint32 flags;  /* Segment flags */
  uint64 off;    /* Segment file offset */
  uint64 vaddr;  /* Segment virtual address */
  uint64 paddr;  /* Segment physical address */
  uint64 filesz; /* Segment size in file */
  uint64 memsz;  /* Segment size in memory */
  uint64 align;  /* Segment alignment */
} elf_prog_header;

#define ELF_MAGIC 0x464C457FU  // "\x7FELF" in little endian
#define ELF_PROG_LOAD 1

typedef enum elf_status_t {
  EL_OK = 0,

  EL_EIO,
  EL_ENOMEM,
  EL_NOTELF,
  EL_ERR,

} elf_status;

typedef struct elf_ctx_t {
  void *info;
  elf_header ehdr;
} elf_ctx;

typedef struct {
	uint32	sh_name; // 4 B (B for bytes)
	uint32	sh_type; // 4 B
	uint64	sh_flags; // 8 B
	uint64	sh_addr; // 8 B
	uint64	sh_offset; // 8 B
	uint64	sh_size; // 8 B
	uint32	sh_link; // 4 B
	uint32	sh_info; // 4 B
	uint64	sh_addralign; // 8 B
	uint64	sh_entsize; // 8 B
} Elf64_Shdr;

typedef struct {
    uint32 st_name;
    uint8  st_info;
    uint8  st_other;
    uint16 st_shndx;
    uint64 st_value;
    uint64 st_size;
} Elf64_Sym;

// 符号类型宏
#define ELF64_ST_TYPE(info) ((info) & 0xf)
#define STT_FUNC 2

elf_status elf_init(elf_ctx *ctx, void *info);
elf_status elf_load(elf_ctx *ctx);

void load_bincode_from_host_elf(process *p);

char* find_func_name(uint64 return_address);

#endif
