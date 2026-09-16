#ifndef SO_UTIL_H
#define SO_UTIL_H

#include <stdint.h>
#include <elf.h>

typedef struct {
    const char *symbol;
    uintptr_t   func;
} so_default_dynlib;

typedef struct so_module {
    struct so_module *next;

    /* In-memory base addresses after loading */
    uintptr_t load_bias;  /* (uintptr_t)mapped_base - virt_min */
    uintptr_t text_base;
    size_t    text_size;
    uintptr_t data_base;
    size_t    data_size;

    /* ELF headers (mapped from raw file buffer) */
    Elf32_Ehdr *ehdr;
    Elf32_Phdr *phdr;
    Elf32_Shdr *shdr;
    char       *shstr;

    /* Dynamic section info */
    Elf32_Dyn  *dynamic;
    int         num_dynamic;
    char       *dynstr;
    Elf32_Sym  *dynsym;
    int         num_dynsym;
    Elf32_Rel  *reldyn;
    int         num_reldyn;
    Elf32_Rel  *relplt;
    int         num_relplt;

    /* init_array */
    void      **init_array;
    int         num_init_array;

    /* ELF hash table */
    uint32_t   *hash;

    /* SONAME string */
    char       *soname;
} so_module;

void      hook_thumb(uintptr_t addr, uintptr_t dst);
void      hook_arm(uintptr_t addr, uintptr_t dst);
void      hook_addr(uintptr_t addr, uintptr_t dst);
/* Hook, keeping a callable copy of the original in *orig.
 * The first two instructions of addr must be position-independent. */
int       hook_arm_trampoline(uintptr_t addr, uintptr_t dst, void **orig);
void      so_flush_caches(so_module *mod);
int       so_load(so_module *mod, const char *filename);
int       so_relocate(so_module *mod);
int       so_resolve(so_module *mod, so_default_dynlib *dynlib, int dynlib_size, int only_dynlib);
void      so_initialize(so_module *mod);
uintptr_t so_symbol(so_module *mod, const char *symbol);

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

#endif /* SO_UTIL_H */
