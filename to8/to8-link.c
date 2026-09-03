/* TO8 pseudo-target linker stubs for TinyCC
 * Provides ELF macros required by tccelf.c / tccgen.c / tccdbg.c
 * and no-op relocate helpers (object file holds pseudo-ASM text).
 */

#ifdef TARGET_DEFS_ONLY

/* ELF machine id (none of the standard ISAs) */
#define EM_TCC_TARGET 0x4F38 /* 'TO' '8' */

/* Minimal reloc type numbers used by generic TCC code */
#define R_DATA_32   1
#define R_DATA_PTR  2
#define R_JMP_SLOT  3
#define R_GLOB_DAT  4
#define R_COPY      5
#define R_RELATIVE  6
#define R_NUM       7

#define ELF_START_ADDR  0x00010000
#define ELF_PAGE_SIZE   0x1000

#define PCRELATIVE_DLLPLT 0
#define RELOCATE_DLLPLT   0

#else /* !TARGET_DEFS_ONLY */

#include "tcc.h"

/* ---- required by tccelf.c when NEED_RELOC_TYPE ---- */

ST_FUNC int code_reloc(int reloc_type)
{
    switch (reloc_type) {
    case R_JMP_SLOT:
        return 1;
    case R_DATA_32:
    case R_DATA_PTR:
    case R_GLOB_DAT:
    case R_COPY:
    case R_RELATIVE:
        return 0;
    default:
        return -1;
    }
}

ST_FUNC int gotplt_entry_type(int reloc_type)
{
    switch (reloc_type) {
    case R_JMP_SLOT:
    case R_GLOB_DAT:
        return ALWAYS_GOTPLT_ENTRY;
    case R_DATA_32:
    case R_DATA_PTR:
    case R_COPY:
    case R_RELATIVE:
        return NO_GOTPLT_ENTRY;
    default:
        return -1;
    }
}

#if !defined ELF_OBJ_ONLY || defined TCC_IS_NATIVE
ST_FUNC unsigned create_plt_entry(TCCState *s1, unsigned got_offset,
                                  struct sym_attr *attr)
{
    (void)s1; (void)got_offset; (void)attr;
    return 0;
}

ST_FUNC void relocate_plt(TCCState *s1)
{
    (void)s1;
}
#endif

ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int type,
                      unsigned char *ptr, addr_t addr, addr_t val)
{
    (void)s1;
    (void)rel;
    (void)addr;
    /* Pseudo-ASM .text is plain text; only patch simple data slots if any */
    switch (type) {
    case R_DATA_32:
        write32le(ptr, val);
        break;
    case R_DATA_PTR:
#if PTR_SIZE == 8
        write64le(ptr, val);
#else
        write32le(ptr, val);
#endif
        break;
    case R_RELATIVE:
#if PTR_SIZE == 8
        write64le(ptr, read64le(ptr) + val);
#else
        write32le(ptr, read32le(ptr) + val);
#endif
        break;
    case R_GLOB_DAT:
    case R_JMP_SLOT:
    case R_COPY:
        break;
    default:
        fprintf(stderr, "TO8 relocate: unhandled type %d\n", type);
        break;
    }
}

#endif /* !TARGET_DEFS_ONLY */
