/* so_util.c -- ELF .so loader and function hooker for Linux ARM32
 *
 * Ported from TheOfficialFloW/gtactw_vita (MIT License)
 * Linux adaptation: replaces PSP2 kernel APIs with POSIX mmap/mprotect.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "so_util.h"

static so_module *head = NULL, *tail = NULL;

/* ── ARM instruction-level hooks ──────────────────────────────────────── */

static void make_writable(uintptr_t addr, size_t len) {
    uintptr_t page = addr & ~(4095UL);
    mprotect((void *)page, len + (addr - page), PROT_READ | PROT_WRITE | PROT_EXEC);
}

void hook_thumb(uintptr_t addr, uintptr_t dst) {
    if (!addr) return;
    addr &= ~1;
    make_writable(addr, 8);
    if (addr & 2) {
        *(uint16_t *)addr = 0xbf00; /* NOP */
        addr += 2;
    }
    uint32_t hook[2];
    hook[0] = 0xf000f8df; /* LDR PC, [PC] */
    hook[1] = dst;
    memcpy((void *)addr, hook, sizeof(hook));
    __builtin___clear_cache((void *)addr, (void *)(addr + 8));
}

void hook_arm(uintptr_t addr, uintptr_t dst) {
    if (!addr) return;
    make_writable(addr, 8);
    uint32_t hook[2];
    hook[0] = 0xe51ff004; /* LDR PC, [PC, #-4] */
    hook[1] = dst;
    memcpy((void *)addr, hook, sizeof(hook));
    __builtin___clear_cache((void *)addr, (void *)(addr + 8));
}

/* Hook an ARM function AND keep a way to call the original.
 *
 * hook_arm() overwrites the first 8 bytes with `LDR PC,[PC,#-4]; .word dst`,
 * which is fine for a full replacement but leaves no route back.  This builds
 * a trampoline first: the two overwritten instructions, followed by a jump to
 * addr+8.  Calling the trampoline therefore behaves exactly like calling the
 * original.
 *
 * PRECONDITION: those first two instructions must be position-independent —
 * no PC-relative loads or branches — because they execute from a different
 * address.  Check the disassembly before hooking anything new.  (For
 * CPed::SetModelIndex they are `push {r3-r8,fp,lr}` and `mov r4,r0`.)
 *
 * Returns 0 on success and stores the trampoline in *orig. */
int hook_arm_trampoline(uintptr_t addr, uintptr_t dst, void **orig) {
    if (!addr || !orig) return -1;

    uint32_t *tr = mmap(NULL, 32, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tr == MAP_FAILED) return -1;

    memcpy(tr, (void *)addr, 8);       /* the two instructions we are about to clobber */
    tr[2] = 0xe51ff004;                /* LDR PC, [PC, #-4] */
    tr[3] = (uint32_t)(addr + 8);      /* ...back into the original body */
    __builtin___clear_cache((void *)tr, (void *)((uintptr_t)tr + 16));

    *orig = tr;
    hook_arm(addr, dst);
    return 0;
}

void hook_addr(uintptr_t addr, uintptr_t dst) {
    if (!addr) return;
    if (addr & 1)
        hook_thumb(addr, dst);
    else
        hook_arm(addr, dst);
}

void so_flush_caches(so_module *mod) {
    __builtin___clear_cache((void *)mod->text_base,
                            (void *)(mod->text_base + mod->text_size));
}

/* ── ELF loader ────────────────────────────────────────────────────────── */

int so_load(so_module *mod, const char *filename) {
    memset(mod, 0, sizeof(*mod));

    int fd = open(filename, O_RDONLY);
    if (fd < 0) { perror("so_load: open"); return -1; }

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;

    /* Map entire file read-only for parsing */
    void *file_data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file_data == MAP_FAILED) { perror("so_load: mmap file"); return -1; }

    if (memcmp(file_data, "\177ELF", 4) != 0) {
        fprintf(stderr, "so_load: not an ELF\n");
        munmap(file_data, file_size);
        return -1;
    }

    Elf32_Ehdr *ehdr = file_data;
    Elf32_Phdr *phdr = (Elf32_Phdr *)((uint8_t *)file_data + ehdr->e_phoff);
    Elf32_Shdr *shdr = (Elf32_Shdr *)((uint8_t *)file_data + ehdr->e_shoff);

    /* First pass: find total virtual memory span */
    uintptr_t virt_min = UINTPTR_MAX, virt_max = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;
        uintptr_t start = phdr[i].p_vaddr;
        uintptr_t end   = start + ALIGN_MEM(phdr[i].p_memsz, phdr[i].p_align);
        if (start < virt_min) virt_min = start;
        if (end   > virt_max) virt_max = end;
    }
    size_t total = virt_max - virt_min;

    /* Reserve a contiguous anonymous region */
    void *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { perror("so_load: mmap anon"); munmap(file_data, file_size); return -1; }
    memset(base, 0, total);

    uintptr_t load_bias = (uintptr_t)base - virt_min;
    mod->load_bias = load_bias;

    /* Second pass: copy each PT_LOAD segment */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;
        void *dst = (void *)(load_bias + phdr[i].p_vaddr);
        void *src = (uint8_t *)file_data + phdr[i].p_offset;
        memcpy(dst, src, phdr[i].p_filesz);

        int prot = 0;
        if (phdr[i].p_flags & PF_R) prot |= PROT_READ;
        if (phdr[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (phdr[i].p_flags & PF_X) prot |= PROT_EXEC;
        /* Keep WRITE for now — we'll need it for relocations */
        mprotect(dst, ALIGN_MEM(phdr[i].p_memsz, phdr[i].p_align), prot | PROT_WRITE);

        if (phdr[i].p_flags & PF_X) {
            mod->text_base = (uintptr_t)dst;
            mod->text_size = phdr[i].p_memsz;
        } else if (!(phdr[i].p_flags & PF_X) && (phdr[i].p_flags & PF_R)) {
            if (!mod->data_base) {
                mod->data_base = (uintptr_t)dst;
                mod->data_size = phdr[i].p_memsz;
            }
        }
    }

    /* Parse section headers for dynamic linking info */
    char *shstr_base = (char *)((uint8_t *)file_data + shdr[ehdr->e_shstrndx].sh_offset);
    for (int i = 0; i < ehdr->e_shnum; i++) {
        const char *name = shstr_base + shdr[i].sh_name;
        uintptr_t addr   = load_bias + shdr[i].sh_addr;
        size_t    sz     = shdr[i].sh_size;

        if      (!strcmp(name, ".dynamic"))   { mod->dynamic    = (Elf32_Dyn *)addr; mod->num_dynamic    = sz / sizeof(Elf32_Dyn); }
        else if (!strcmp(name, ".dynstr"))    { mod->dynstr     = (char *)addr; }
        else if (!strcmp(name, ".dynsym"))    { mod->dynsym     = (Elf32_Sym *)addr; mod->num_dynsym     = sz / sizeof(Elf32_Sym); }
        else if (!strcmp(name, ".rel.dyn"))   { mod->reldyn     = (Elf32_Rel *)addr; mod->num_reldyn     = sz / sizeof(Elf32_Rel); }
        else if (!strcmp(name, ".rel.plt"))   { mod->relplt     = (Elf32_Rel *)addr; mod->num_relplt     = sz / sizeof(Elf32_Rel); }
        else if (!strcmp(name, ".init_array")){ mod->init_array = (void **)addr;     mod->num_init_array = sz / sizeof(void *); }
        else if (!strcmp(name, ".hash"))      { mod->hash       = (uint32_t *)addr; }
    }

    /* Parse SONAME from .dynamic */
    for (int i = 0; i < mod->num_dynamic; i++) {
        if (mod->dynamic[i].d_tag == DT_SONAME)
            mod->soname = mod->dynstr + mod->dynamic[i].d_un.d_ptr;
    }

    munmap(file_data, file_size);

    if (!head) { head = tail = mod; }
    else       { tail->next = mod; tail = mod; }

    return 0;
}

/* ── Relocation & symbol resolution ───────────────────────────────────── */

int so_relocate(so_module *mod) {
    int count = mod->num_reldyn + mod->num_relplt;
    for (int i = 0; i < count; i++) {
        Elf32_Rel *rel = i < mod->num_reldyn
            ? &mod->reldyn[i]
            : &mod->relplt[i - mod->num_reldyn];

        Elf32_Sym  *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
        uintptr_t  *ptr = (uintptr_t *)(mod->load_bias + rel->r_offset);
        int         type = ELF32_R_TYPE(rel->r_info);

        switch (type) {
        case R_ARM_ABS32:
            if (sym->st_shndx != SHN_UNDEF)
                *ptr += mod->load_bias + sym->st_value;
            break;
        case R_ARM_RELATIVE:
            *ptr += mod->load_bias;
            break;
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT:
            if (sym->st_shndx != SHN_UNDEF)
                *ptr = mod->load_bias + sym->st_value;
            break;
        default:
            break;
        }
    }
    return 0;
}

static uintptr_t so_resolve_link(so_module *mod, const char *symbol) {
    for (int i = 0; i < mod->num_dynamic; i++) {
        if (mod->dynamic[i].d_tag != DT_NEEDED) continue;
        so_module *curr = head;
        while (curr) {
            if (curr != mod && curr->soname &&
                strcmp(curr->soname, mod->dynstr + mod->dynamic[i].d_un.d_ptr) == 0) {
                uintptr_t link = so_symbol(curr, symbol);
                if (link) return link;
            }
            curr = curr->next;
        }
    }
    return 0;
}

int so_resolve(so_module *mod, so_default_dynlib *dynlib, int dynlib_size, int only_dynlib) {
    int count = mod->num_reldyn + mod->num_relplt;
    for (int i = 0; i < count; i++) {
        Elf32_Rel *rel = i < mod->num_reldyn
            ? &mod->reldyn[i]
            : &mod->relplt[i - mod->num_reldyn];

        Elf32_Sym *sym  = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
        uintptr_t *ptr  = (uintptr_t *)(mod->load_bias + rel->r_offset);
        int        type = ELF32_R_TYPE(rel->r_info);

        if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT)
            continue;
        if (sym->st_shndx != SHN_UNDEF)
            continue;

        const char *name = mod->dynstr + sym->st_name;
        uintptr_t resolved = 0;

        /* Strip "@VERSION" suffix (e.g. "raise@LIBC") for dynlib table lookup */
        char name_bare[256];
        const char *at = strchr(name, '@');
        if (at && (size_t)(at - name) < sizeof(name_bare)) {
            size_t len = (size_t)(at - name);
            memcpy(name_bare, name, len);
            name_bare[len] = '\0';
        } else {
            strncpy(name_bare, name, sizeof(name_bare) - 1);
            name_bare[sizeof(name_bare) - 1] = '\0';
        }

        if (!only_dynlib)
            resolved = so_resolve_link(mod, name);

        for (int j = 0; j < dynlib_size / (int)sizeof(so_default_dynlib); j++) {
            if (strcmp(name_bare, dynlib[j].symbol) == 0) {
                resolved = dynlib[j].func;
                break;
            }
        }

        if (resolved) {
            *ptr = resolved;
        } else {
            /* Nothing supplied this import, so the GOT slot keeps whatever it
             * held — usually 0.  The game then calls through a null pointer at
             * some arbitrary later moment, with a stale LR that points at
             * whatever ran last, which makes the crash almost impossible to
             * attribute.  Naming them here turns that into a one-line fix. */
            /* Only meaningful on a full pass: the module-table passes use
             * only_dynlib=1 and correctly skip every symbol they do not own. */
            if (!only_dynlib)
                fprintf(stderr, "[so_resolve] UNRESOLVED: %s\n", name_bare);
        }
    }
    return 0;
}

void so_initialize(so_module *mod) {
    for (int i = 0; i < mod->num_init_array; i++) {
        void (*fn)(void) = (void (*)(void))mod->init_array[i];
        if (fn && (uintptr_t)fn != 0xffffffff) {
#ifdef SO_TRACE_INIT
            /* One line per initialiser, flushed, so a crash names the entry
             * that faulted rather than leaving the whole array as a suspect. */
            fprintf(stderr, "[init] %d/%d -> %p (+0x%x)\n", i, mod->num_init_array,
                    (void *)fn, (unsigned)((uintptr_t)fn - mod->load_bias));
            fflush(stderr);
#endif
            fn();
        }
    }
#ifdef SO_TRACE_INIT
    fprintf(stderr, "[init] all %d initialisers returned\n", mod->num_init_array);
    fflush(stderr);
#endif
}

/* ── Symbol lookup ─────────────────────────────────────────────────────── */

static uint32_t so_hash(const uint8_t *name) {
    uint64_t h = 0, g;
    while (*name) {
        h = (h << 4) + *name++;
        if ((g = h & 0xf0000000ULL)) h ^= g >> 24;
        h &= 0x0fffffff;
    }
    return (uint32_t)h;
}

uintptr_t so_symbol(so_module *mod, const char *symbol) {
    if (mod->hash) {
        uint32_t  h       = so_hash((const uint8_t *)symbol);
        uint32_t  nbucket = mod->hash[0];
        uint32_t *bucket  = &mod->hash[2];
        uint32_t *chain   = bucket + nbucket;
        for (uint32_t i = bucket[h % nbucket]; i; i = chain[i]) {
            if (mod->dynsym[i].st_shndx == SHN_UNDEF) continue;
            if (strcmp(mod->dynstr + mod->dynsym[i].st_name, symbol) == 0)
                return mod->load_bias + mod->dynsym[i].st_value;
        }
    }
    for (int i = 0; i < mod->num_dynsym; i++) {
        if (mod->dynsym[i].st_shndx == SHN_UNDEF) continue;
        if (strcmp(mod->dynstr + mod->dynsym[i].st_name, symbol) == 0)
            return mod->text_base + mod->dynsym[i].st_value;
    }
    return 0;
}
