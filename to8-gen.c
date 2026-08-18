/* TO8 backend for TCC - single-register pseudo-ASM generator.
 *
 * Version: 7.25.1 (fix: commutative peephole no longer drops the ST)
 * Changelog:
 * - v7.25.1: to8_peephole_commute() no longer removes the leading
 *   "ST T" when fusing "ST T ; LD S ; <op> T" -> "<op> S". Real bug
 *   found in a generated __qsort() listing (2026-08-16): the "ST T"
 *   is NOT always a dead spill temp created by to8_spill_and_reload()
 *   - it can just as well be an ordinary store to a REAL, live local
 *   variable (e.g. "i = i + 1;" immediately followed, in the very
 *   next C statement, by an unrelated "... * i" that happens to read
 *   the same slot). The old v7.25.0 pass deleted BOTH the ST and the
 *   LD, silently discarding the increment whenever that coincidence
 *   occurred - i never got updated in memory again, breaking the
 *   partition loop. Fix: only the "LD S" is redundant. Right after
 *   "ST T" (T = R0 = X), R0 STILL holds X, so "<op> T" can become
 *   "<op> S" directly - R0 op S == X op S == (since <op> is
 *   commutative) S op X == S op T's stored value. The ST is now
 *   ALWAYS kept: if T is a live variable, its value is correctly
 *   preserved for later reads; if T is a genuine dead spill temp,
 *   the kept ST is merely a harmless, never-read extra instruction.
 *   Either way the transform is unconditionally correct - no
 *   liveness analysis or "is this a real temp" tagging needed at
 *   all, which is strictly simpler than the is_temp_spill flag
 *   approach considered and discarded earlier in this session.
 *   `!cur->is_target` is no longer required either, since `cur`
 *   (the ST) is never unlinked anymore - only `nxt` (the LD) is,
 *   so its `!nxt->is_target` guard is the only one still needed.
 *
 * - v7.25.0: new peephole pass to8_peephole_commute(). Found while
 *   reviewing a generated listing (2026-08-16): int f(char*a,
 *   unsigned short*b){return *a+*b;} produced
 *     ST T   ; spill *b (currently in R0) into temp T
 *     LD S   ; reload *a from its slot S into R0
 *     ADD T  ; R0 += T -> R0 = *a + *b
 *   even though R0 already held the correct *b value right before
 *   the ST. Since +/*&|^ are all commutative, this exact 3-line
 *   spill-reload-add dance (emitted by to8_spill_and_reload(), called
 *   from gen_opi()'s commutative branch whenever the front-end still
 *   thinks the left operand is sitting in R0 at that point) collapses
 *   to a single "ADD S" - R0 already has the right value, no need to
 *   touch it before adding the other operand's slot directly.
 *   SUPERSEDED by v7.25.1 above: the original version also deleted
 *   the ST, which is unsafe in general (see v7.25.1 entry). Applies
 *   to ADD, MUL, AND, OR, XOR (the same set to8_is_commutative() in
 *   gen_opi() already restricts itself to) - never to SUB/DIV/MOD/
 *   CMP, which are not commutative and would change the computed
 *   value if reordered this way.
 *
 * - v7.24.0: to8_peephole_ld_deref() no longer requires `!cur->is_target`
 *   before fusing "LD slot" + "LD1r/LDU1r/LD2r/LDU2r/LD4r" into a
 *   single "LD1/LDU1/LD2/LDU2/LD4 slot". A loop-header LD immediately
 *   after a label (the extremely common "while (*p) { ... p++; }"
 *   shape) never got fused before, because the pass required BOTH
 *   `cur` (the LD) and `nxt` (the LDxr) to be non-targets. Fusing
 *   keeps `cur`'s id/position (only `nxt` is unlinked), so any label
 *   attached to `cur` stays attached to the SAME instruction, which
 *   now performs the combined load+dereference - no jump can ever
 *   land "in the middle" of the fused pair, since the only dangerous
 *   case (something jumps directly to `nxt`, skipping the LD) is
 *   still excluded by keeping the `!nxt->is_target` check.
 *
 * - v7.23.0: __native_asm() (the gfunc_call()-interception compiler
 *   intrinsic added in v7.21.0) has been REMOVED, per the 2026-08-16
 *   testing session decision to go back to plain asm("...") as the
 *   sole raw-text passthrough mechanism. The real duplication bug
 *   that motivated part of the exploration turned out to be in
 *   asm_opcode() itself (to8-stubs.c), not something inherent to
 *   asm() - see to8-stubs.c's v7.23.0 changelog entry for the actual
 *   fix (a precise, buffer-scoped dedup key). __native_asm() may come
 *   back later if a genuinely global-scope (outside any function)
 *   raw-text mechanism turns out to be needed - asm() already
 *   supports that via asm_global_instr(), which __native_asm() could
 *   never support since it's an ordinary function call, only legal
 *   inside a function body.
 *
 * - v7.20.0: documented (comment only, near OP_LDF/OP_LDG/OP_STF/
 *   OP_STG below) the floating-point endianness/format architecture
 *   decision: this backend and tccgen.c deliberately do NOT touch
 *   the IEEE-754 bytes TCC's common front-end writes for float/
 *   double constants (host-native, little-endian). Converting to/
 *   from the target's actual runtime float format (EXTRAMON FAC/ARG,
 *   for now) is the responsibility of the eventual REAL 6809
 *   implementation of LDF/LDG/STF/STG - the one place the C type is
 *   known with certainty (a .fcb dump or ELF symbol walk cannot
 *   reliably tell a float apart from an int/pointer of the same
 *   size).
 *
 * - v7.19.0: gen_le16(v)/gen_le32(v) are now MACROS, defined in the
 *   TARGET_DEFS_ONLY block (the ONLY part of this backend included
 *   BEFORE tccgen.c in the ONESOURCE build), redirecting to
 *   gen_be16_impl()/gen_be32_impl() (to8-stubs.c). TO8 (6809) is
 *   __BIG_ENDIAN__, but gen_le16/gen_le32 were copy-pasted from a
 *   little-endian backend and wrote low-byte-first. NOTE: does not
 *   affect float/double constant bytes - see v7.20.0 above.
 *
 * - v7.18.0: three cleanups on top of 7.17.0:
 *   1) gfunc_epilog()'s frame-release ADJi now goes through
 *      to8_adjust(frame_size) (which already no-ops on n==0) instead
 *      of a hand-rolled to8_append() - no more useless "ADJi 0" for
 *      functions with no locals.
 *   2) to8_flush_pending_data() added, dumping rodata/data section
 *      contents as FCB directives (wrapped 8 bytes/line), with a
 *      trailing "; --- end of asm ---" marker; nocode_wanted (left at
 *      DATA_ONLY_WANTED after the last function) is saved/cleared/
 *      restored around it; cur_text_section is stale/NULL by the
 *      time tcc_gen_finish() runs, so it's pointed at `text_section`
 *      and `ind` resynced to its data_offset before writing;
 *      `cur_text_section->data_offset` is resynced to `ind` at the
 *      very end too, or the FCB bytes get silently truncated at
 *      final .o serialization (g() never updates data_offset itself).
 *   3) `symtab_section`/`text_section`/`cur_text_section` must be
 *      referenced BARE (no "s1->" prefix) - tcc.h #defines them as
 *      convenience macros expanding to "tcc_state->...".
 *
 * - v7.17.0: VLA support removed entirely, on top of the 7.16.0
 *   signed/unsigned sub-word load fix. gen_vla_alloc() now calls
 *   tcc_error() instead of emitting OP_ADJr (removed from the ISA).
 *   Fully compliant with C11, where VLAs are an optional feature
 *   (this backend advertises __STDC_NO_VLA__). gen_vla_sp_save()/
 *   gen_vla_sp_restore() are KEPT as harmless stubs - tccgen.c
 *   references them unconditionally at link time even though
 *   gen_vla_alloc() always rejects the VLA first.
 *
 * - v7.16.0: fixed a real correctness bug found while testing
 *   tst_ext() (int f(char *a, unsigned short *b) { return *a + *b; }).
 *   load() emitted OP_LD1/OP_LD2 for char/short lvalues but NEVER
 *   sign- or zero-extended the upper 24/16 bits of R0 afterwards.
 *   Fixed by baking extension into the load opcode itself:
 *   - OP_LD1 / OP_LD2 / OP_LD1r / OP_LD2r = load + SIGN-extend.
 *   - OP_LDU1 / OP_LDU2 / OP_LDU1r / OP_LDU2r = load + ZERO-extend.
 *   - VT_BOOL always routes to the unsigned form (LDU1/LDU1r).
 *
 * - v7.15.0 and earlier: see prior versions (float/double ISA split,
 *   raw inline-asm passthrough, etc.)
 *
 * Architecture:
 *   R0 = real integer accumulator (also REG_IRET).
 *   R1 = VIRTUAL integer register, memory-backed shadow slot.
 *   F0 = float/double accumulator (also REG_FRET) - width is tracked
 *        by the CALLER (gen_opf, load, store), never by F0 itself.
 *   NO status/flags register of any kind, for EITHER integers or
 *   floats. OP_CMP/OP_CMPU/OP_CMPi/OP_CMPUi and OP_CMPF/OP_CMPG ALL
 *   write a SIGNED integer directly into R0. Every S../J.. opcode
 *   that follows just reads the sign of whatever is currently in R0.
 *
 * Naming convention:
 * - "i" suffix = opcode is FULLY resolved with NO extra runtime
 *   memory access: a plain immediate, a symbol's ADDRESS (LDi,
 *   JSRi), or a float/double VALUE baked directly into the
 *   instruction (LDFi, LDGi).
 * - "r" suffix = opcode takes NO ARGUMENT but reads/writes R0 as
 *   the family's register-operand form (PUSHr, JSRr, LD1r/LDU1r/
 *   LD2r/LDU2r/LD4r).
 * - plain vs "U"-suffixed pair (integer family only) = signed vs
 *   unsigned. Applies to compares (CMP vs CMPU, CMPi vs CMPUi) and
 *   to sub-word loads (LD1 vs LDU1, LD2 vs LDU2, LD1r vs LDU1r,
 *   LD2r vs LDU2r): the "U" form zero-extends into R0, the plain
 *   form sign-extends.
 * - "F"/"G" suffix (float family only) = operates on a 4-byte
 *   float slot (F) or an 8-byte double slot (G). Applies to the
 *   own-slot load/store (LDF/STF vs LDG/STG), the arithmetic family
 *   (ADDF/SUBF/MULF/DIVF vs ADDG/SUBG/MULG/DIVG), the comparison
 *   (CMPF vs CMPG), and the immediates (LDFi vs LDGi).
 * - "4"/"8" numeric suffix (float DEREFERENCE family only, LDF4/
 *   LDF8/STF4/STF8) = size of the POINTEE behind a slot-held
 *   pointer or a real mutable symbol - a genuinely different axis
 *   from the F/G own-value suffix.
 * - no suffix (integer family) = opcode takes a SLOT (LD, ST, ADD,
 *   CMP, PUSH, JSR...).
 *
 * Output format (once per compiled file):
 *   ; TO8 backend <version>
 *   ; out: <outfile> src: <filename>
 *   ; cmd: <argv...>            (only if argc/argv are non-empty)
 *   then, per function:
 *   _funcname:                  ; N instr           (peephole made no change)
 *   _funcname:                  ; N instr (M before -O)  (peephole reduced M to N)
 *   @lN:
 *           MNEMONIC <arg>      ; <comment>          at fixed column
 *   then, once, at the very end (see to8_flush_pending_data()):
 *   _symbol:                    ; N bytes (data)
 *           FCB     <byte>,<byte>,...   (wrapped 8 per line)
 *   ; --- end of asm ---
 */

#ifdef TARGET_DEFS_ONLY

#define NB_REGS 3
#define NB_ASM_REGS 0
#define CONFIG_TCC_ASM

#define RC_R0    0x0001
#define RC_R1    0x0002
#define RC_FLOAT 0x0004
#define RC_INT  (RC_R0 | RC_R1)
#define RC_IRET  RC_R0
#define RC_FRET  RC_FLOAT

enum {
    TREG_R0 = 0,
    TREG_F0 = 1,
    TREG_R1 = 2,
    TREG_MEM = 0x20
};

#define REG_IRET TREG_R0
#define REG_FRET TREG_F0

#define REG_IRE2 VT_CONST
#define REG_FRE2 VT_CONST

#define PTR_SIZE      4
#define LDOUBLE_SIZE  8
#define LDOUBLE_ALIGN 8
#define MAX_ALIGN     8

#define PROMOTE_RET

/* v7.19.0: TO8 (6809) is __BIG_ENDIAN__. gen_le16/gen_le32 are
 * called from common tccgen.c code assuming little-endian byte
 * order; redirected here (the ONLY part of this backend included
 * BEFORE tccgen.c in the ONESOURCE build) to big-endian-correct
 * implementations, without touching a single line of common tccgen.c
 * code.
 *
 * Does NOT cover float/double constant bytes (see the big comment
 * near OP_LDF/OP_LDG below) - those go through a completely
 * different, common-code path that never calls gen_le16/gen_le32. */
ST_FUNC void gen_be16_impl(int v);
ST_FUNC void gen_be32_impl(int v);
#define gen_le16(v) gen_be16_impl(v)
#define gen_le32(v) gen_be32_impl(v)

#define TO8_STACK_ALIGN 4

#else

/* must be defined before gfunc_prolog/epilog call them */
ST_FUNC void gen_bounds_prolog(void) {}
ST_FUNC void gen_bounds_epilog(void) {}

#define USING_GLOBALS
#include "tcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TO8_GEN_VERSION "7.25.1"

ST_DATA const char * const target_machine_defs =
    "__TO8__\0"
    "__BIG_ENDIAN__\0"
    "__STDC_NO_VLA__\0";

ST_DATA const int reg_classes[NB_REGS] = {
    RC_R0,
    RC_FLOAT,
    RC_R1,
};

/* ===================================================================
 * The entire instruction set, in one place, with its semantics.
 * "slot" always means a stack-frame memory location (a local
 * variable or parameter). "R0"/"F0" are the two accumulators.
 *
 * There is NO flags register, for integers or floats. OP_CMP/OP_CMPU/
 * OP_CMPi/OP_CMPUi and OP_CMPF/OP_CMPG ALL write a SIGNED integer
 * directly into R0. Every S../J.. opcode that follows just reads the
 * sign of whatever is currently in R0.
 * =================================================================== */

typedef enum {
    OP_NOP,   /* no-op */
    OP_RET,   /* return from the current function */
    OP_ADJi,  /* SP -= n if n<0 (allocate), SP += n if n>=0 (release) */

    OP_LD,    /* R0 = slot (4-byte load) */
    OP_ST,    /* slot = R0 (4-byte store) */
    OP_LEA,   /* R0 = address of slot */
    OP_LD1,   /* R0 = (int)(signed char)*(char*)slot - byte load, SIGN-extend */
    OP_LDU1,  /* R0 = (int)(unsigned char)*(char*)slot - byte load, ZERO-extend */
    OP_LD2,   /* R0 = (int)(signed short)*(short*)slot - halfword load, SIGN-extend */
    OP_LDU2,  /* R0 = (int)(unsigned short)*(short*)slot - halfword load, ZERO-extend */
    OP_LD4,   /* R0 = *(int*)slot (word-sized memory load; already full width) */
    OP_LD1r,  /* R0 = (int)(signed char)*(char*)R0 - dereference pointer ALREADY in R0, SIGN-extend */
    OP_LDU1r, /* R0 = (int)(unsigned char)*(char*)R0 - dereference pointer ALREADY in R0, ZERO-extend */
    OP_LD2r,  /* R0 = (int)(signed short)*(short*)R0 - dereference, SIGN-extend */
    OP_LDU2r, /* R0 = (int)(unsigned short)*(short*)R0 - dereference, ZERO-extend */
    OP_LD4r,  /* R0 = *(int*)R0 */
    OP_ST1,   /* *(char*)slot = R0 (byte-sized memory store via pointer in slot) */
    OP_ST2,   /* *(short*)slot = R0 (halfword-sized memory store) */
    OP_ST4,   /* *(int*)slot = R0 (word-sized memory store) */

    OP_LDi,   /* R0 = immediate, OR R0 = address of a global symbol
                 (a compile/link-time-known value either way). */

    OP_ADD,   /* R0 += slot */
    OP_SUB,   /* R0 -= slot */
    OP_AND,   /* R0 &= slot */
    OP_OR,    /* R0 |= slot */
    OP_XOR,   /* R0 ^= slot */
    OP_MUL,   /* R0 *= slot */
    OP_DIV,   /* R0 /= slot */
    OP_MOD,   /* R0 %= slot */
    OP_SHL,   /* R0 <<= slot */
    OP_SHR,   /* R0 >>= slot (logical/unsigned) */
    OP_SAR,   /* R0 >>= slot (arithmetic/signed) */
    OP_CMP,   /* R0 = sign(R0 - slot), SIGNED subtraction. */
    OP_CMPU,  /* R0 = sign(R0 -u slot), UNSIGNED subtraction. */

    OP_ADDi,  /* R0 += immediate */
    OP_SUBi,  /* R0 -= immediate */
    OP_ANDi,  /* R0 &= immediate */
    OP_ORi,   /* R0 |= immediate */
    OP_XORi,  /* R0 ^= immediate */
    OP_MULi,  /* R0 *= immediate */
    OP_DIVi,  /* R0 /= immediate */
    OP_MODi,  /* R0 %= immediate */
    OP_SHLi,  /* R0 <<= immediate */
    OP_SHRi,  /* R0 >>= immediate (logical/unsigned) */
    OP_SARi,  /* R0 >>= immediate (arithmetic/signed) */
    OP_CMPi,  /* R0 = sign(R0 - immediate), SIGNED */
    OP_CMPUi, /* R0 = sign(R0 -u immediate), UNSIGNED */

    OP_SEQ,   /* R0 = (R0 == 0) ? 1 : 0 */
    OP_SNE,   /* R0 = (R0 != 0) ? 1 : 0 */
    OP_SLT,   /* R0 = (R0 < 0) ? 1 : 0 */
    OP_SGT,   /* R0 = (R0 > 0) ? 1 : 0 */
    OP_SLE,   /* R0 = (R0 <= 0) ? 1 : 0 */
    OP_SGE,   /* R0 = (R0 >= 0) ? 1 : 0 */
    OP_SNZ,   /* R0 = (R0 != 0) ? 1 : 0 (used when no dedicated S.. fits) */

    OP_MOV,   /* dst_slot = src_slot, WITHOUT touching R0 - fusion of LD+ST */

    OP_EXT1S, /* R0 = (int)(signed char)R0 */
    OP_EXT2S, /* R0 = (int)(signed short)R0 */
    OP_EXT1U, /* R0 = (int)(unsigned char)R0 */
    OP_EXT2U, /* R0 = (int)(unsigned short)R0 */

    OP_ITOF,  /* F0 = (float/double)R0 */
    OP_FTOI,  /* R0 = (int)F0 */

    /* ===============================================================
     * ARCHITECTURE NOTE - floating-point storage format:
     *
     * The 4/8 raw bytes referenced by LDF/LDG/STF/STG below are
     * written by TCC's common front-end (tccgen.c's gv(), for any
     * float/double constant that can't be used as an immediate
     * operand -> init_put_v()) as plain IEEE-754, in the HOST's
     * native (little-endian) byte order. This backend deliberately
     * does NOT rewrite them at the .fcb-dump or ELF-symbol level:
     * doing so would require GUESSING which 4/8-byte symbols are
     * floats, which is not reliably knowable from ELF symbol info
     * alone (size and address only, no C type info survives to
     * that point).
     *
     * The C type IS known with certainty right here, at the point
     * LDF/LDG/STF/STG are chosen instead of LD/LD4 - so converting
     * between IEEE-754 (as stored) and the target's actual runtime
     * float format is the responsibility of the eventual REAL 6809
     * implementation of these four opcodes, not of this pseudo-asm
     * generator and not of tccgen.c.
     *
     * Decision (2026-08-15): start with EXTRAMON's native FAC/ARG
     * format (already in ROM on the TO8, zero link cost), documented
     * in the EXTRAMON manual: 1-byte excess-128 exponent + mantissa
     * with an EXPLICIT leading "1" bit (24 bits for float/VALTYP=4,
     * 56 bits for double/VALTYP=8, zero-padded past the 53 IEEE
     * double significant bits) + a separate sign byte (bit 7 only).
     * Conversion formulas (verified against the EXTRAMON manual's own
     * worked example, 100 decimal -> exponent $87/135, mantissa
     * $C80000):
     *   FACEXP (float)  = ieee_biased_exponent + 2
     *   FACEXP (double) = ieee_biased_exponent - 894
     *   mantissa = (1 << ieee_mantissa_bits) | ieee_mantissa,
     *              left-shifted by 3 extra bits for double
     *              (56-bit EXTRAMON field vs 53 IEEE sig. bits)
     *   sign = IEEE sign bit -> bit 7 of FACSGN/ARGSGN
     * LDF/LDG must convert IEEE -> EXTRAMON on load into FAC/ARG;
     * STF/STG must convert EXTRAMON -> IEEE (little-endian) on store
     * back to the slot. Watch for FACEXP overflow/underflow: EXTRAMON's
     * 1-byte exponent covers roughly 2^-128..2^127, far narrower than
     * IEEE double's 2^-1022..2^1023 range.
     *
     * An alternative, faster+smaller-but-not-ROM-resident format
     * (LBFP, Lennart Benschop's 6809 floating point package - see
     * github.com/6809/sbc09, basic/fbasic.asm) was evaluated and
     * benchmarked faster than EXTRAMON (7465 vs 8752 cycles on a
     * reference calculation) but deliberately deferred: EXTRAMON's
     * zero integration cost (no library to link) wins for now.
     * =============================================================== */

    OP_LDF,   /* F0 = slot (own-slot value, as float, 4 bytes) */
    OP_LDG,   /* F0 = slot (own-slot value, as double, 8 bytes) */
    OP_STF,   /* slot = F0 (own-slot value, as float) */
    OP_STG,   /* slot = F0 (own-slot value, as double) */
    OP_LDF4,  /* F0 = *(float*)slot-or-symbol - dereference a
                 slot-held pointer, OR read a REAL (possibly
                 mutable) global float variable by symbol. */
    OP_LDF8,  /* F0 = *(double*)slot-or-symbol */
    OP_STF4,  /* *(float*)slot-or-symbol = F0 */
    OP_STF8,  /* *(double*)slot-or-symbol = F0 */
    OP_LDFi,  /* F0 = <float-immediate>, baked directly into the
                 instruction - no memory access at all. Only used
                 for compiler-generated anonymous constants that
                 can never change. */
    OP_LDGi,  /* F0 = <double-immediate>, same idea. */

    OP_CMPF,  /* R0 = sign(F0 - slot), comparing as FLOAT (4-byte
                 slot). Consumed by the SAME SEQ/SNE/SLT/SGT/SLE/
                 SGE and JEQ/JNE/... families integers use. */
    OP_CMPG,  /* R0 = sign(F0 - slot), comparing as DOUBLE (8-byte
                 slot). */
    OP_ADDF,  /* F0 += slot (as float, 4-byte slot) */
    OP_SUBF,  /* F0 -= slot (as float) */
    OP_MULF,  /* F0 *= slot (as float) */
    OP_DIVF,  /* F0 /= slot (as float) */
    OP_ADDG,  /* F0 += slot (as double, 8-byte slot) */
    OP_SUBG,  /* F0 -= slot (as double) */
    OP_MULG,  /* F0 *= slot (as double) */
    OP_DIVG,  /* F0 /= slot (as double) */

    OP_PUSH,  /* push slot's value onto the call-argument stack */
    OP_PUSHi, /* push an immediate onto the call-argument stack */
    OP_PUSHr, /* push R0 onto the call-argument stack */

    OP_JSR,   /* call the function pointer held in slot */
    OP_JSRi,  /* call the function named by symbol */
    OP_JSRr,  /* call the function pointer held in R0 */

    OP_JMP,   /* unconditional: goto target */
    OP_JEQ,   /* if (R0 == 0) goto target */
    OP_JNE,   /* if (R0 != 0) goto target */
    OP_JLT,   /* if (R0 < 0) goto target */
    OP_JGT,   /* if (R0 > 0) goto target */
    OP_JLE,   /* if (R0 <= 0) goto target */
    OP_JGE,   /* if (R0 >= 0) goto target */
} to8_opcode;

static const char *to8_opcode_name(to8_opcode op)
{
    switch (op) {
    case OP_NOP: return "NOP"; case OP_RET: return "RET";
    case OP_ADJi: return "ADJi";
    case OP_LD: return "LD"; case OP_ST: return "ST"; case OP_LEA: return "LEA";
    case OP_LD1: return "LD1"; case OP_LDU1: return "LDU1";
    case OP_LD2: return "LD2"; case OP_LDU2: return "LDU2";
    case OP_LD4: return "LD4";
    case OP_LD1r: return "LD1r"; case OP_LDU1r: return "LDU1r";
    case OP_LD2r: return "LD2r"; case OP_LDU2r: return "LDU2r";
    case OP_LD4r: return "LD4r";
    case OP_ST1: return "ST1"; case OP_ST2: return "ST2"; case OP_ST4: return "ST4";
    case OP_LDi: return "LDi";
    case OP_ADD: return "ADD"; case OP_SUB: return "SUB"; case OP_AND: return "AND";
    case OP_OR: return "OR"; case OP_XOR: return "XOR"; case OP_MUL: return "MUL";
    case OP_DIV: return "DIV"; case OP_MOD: return "MOD";
    case OP_SHL: return "SHL"; case OP_SHR: return "SHR"; case OP_SAR: return "SAR";
    case OP_CMP: return "CMP"; case OP_CMPU: return "CMPU";
    case OP_ADDi: return "ADDi"; case OP_SUBi: return "SUBi"; case OP_ANDi: return "ANDi";
    case OP_ORi: return "ORi"; case OP_XORi: return "XORi"; case OP_MULi: return "MULi";
    case OP_DIVi: return "DIVi"; case OP_MODi: return "MODi";
    case OP_SHLi: return "SHLi"; case OP_SHRi: return "SHRi"; case OP_SARi: return "SARi";
    case OP_CMPi: return "CMPi"; case OP_CMPUi: return "CMPUi";
    case OP_SEQ: return "SEQ"; case OP_SNE: return "SNE"; case OP_SLT: return "SLT"; case OP_SGT: return "SGT";
    case OP_SLE: return "SLE"; case OP_SGE: return "SGE";
    case OP_SNZ: return "SNZ";
    case OP_MOV: return "MOV";
    case OP_EXT1S: return "EXT1S"; case OP_EXT2S: return "EXT2S";
    case OP_EXT1U: return "EXT1U"; case OP_EXT2U: return "EXT2U";
    case OP_ITOF: return "ITOF"; case OP_FTOI: return "FTOI";
    case OP_LDF: return "LDF"; case OP_LDG: return "LDG";
    case OP_STF: return "STF"; case OP_STG: return "STG";
    case OP_LDF4: return "LDF4"; case OP_LDF8: return "LDF8";
    case OP_STF4: return "STF4"; case OP_STF8: return "STF8";
    case OP_LDFi: return "LDFi"; case OP_LDGi: return "LDGi";
    case OP_CMPF: return "CMPF"; case OP_CMPG: return "CMPG";
    case OP_ADDF: return "ADDF"; case OP_SUBF: return "SUBF";
    case OP_MULF: return "MULF"; case OP_DIVF: return "DIVF";
    case OP_ADDG: return "ADDG"; case OP_SUBG: return "SUBG";
    case OP_MULG: return "MULG"; case OP_DIVG: return "DIVG";
    case OP_PUSH: return "PUSH"; case OP_PUSHi: return "PUSHi"; case OP_PUSHr: return "PUSHr";
    case OP_JSR: return "JSR"; case OP_JSRi: return "JSRi"; case OP_JSRr: return "JSRr";
    case OP_JMP: return "JMP"; case OP_JEQ: return "JEQ"; case OP_JNE: return "JNE";
    case OP_JLT: return "JLT"; case OP_JGT: return "JGT"; case OP_JLE: return "JLE"; case OP_JGE: return "JGE";
    }
    return "?";
}

/* ===================================================================
 * Temp-slot management: a strictly-monotonic base allocator (used for
 * genuinely function-lifetime slots, like r1_shadow_slot) plus a
 * size-keyed LIFO free-list pool for ephemeral temps that this
 * backend can PROVE are dead right after their next use.
 * =================================================================== */

static int to8_new_temp(int size, int align)
{
    loc = (loc - size) & -align;
    return loc;
}

static int *free4_stack; static int free4_n, free4_cap;
static int *free8_stack; static int free8_n, free8_cap;

static void to8_temp_pool_reset(void) { free4_n = 0; free8_n = 0; }

static int to8_temp_alloc(int size, int align)
{
    if (size == 4 && align == 4 && free4_n > 0)
        return free4_stack[--free4_n];
    if (size == 8 && align == 4 && free8_n > 0)
        return free8_stack[--free8_n];
    return to8_new_temp(size, align);
}

static void to8_temp_free(int slot, int size, int align)
{
    if (size == 4 && align == 4) {
        if (free4_n >= free4_cap) {
            int nc = free4_cap ? free4_cap * 2 : 32;
            free4_stack = tcc_realloc(free4_stack, nc * sizeof(int));
            free4_cap = nc;
        }
        free4_stack[free4_n++] = slot;
    } else if (size == 8 && align == 4) {
        if (free8_n >= free8_cap) {
            int nc = free8_cap ? free8_cap * 2 : 32;
            free8_stack = tcc_realloc(free8_stack, nc * sizeof(int));
            free8_cap = nc;
        }
        free8_stack[free8_n++] = slot;
    }
}

static int r1_shadow_slot;
static int r1_shadow_valid;

static int to8_r1_slot(void)
{
    if (!r1_shadow_valid) {
        r1_shadow_slot = to8_new_temp(4, 4);
        r1_shadow_valid = 1;
    }
    return r1_shadow_slot;
}

static int cur_push_depth;

/* ===================================================================
 * Doubly-linked instruction list for the function being generated.
 * =================================================================== */

enum { ARG_NONE = 0, ARG_SLOT, ARG_SLOT2, ARG_IMM, ARG_SYM, ARG_JMP, ARG_FIMM };

typedef struct to8_line {
    to8_opcode op;
    int kind;
    int slot_a, slot_b, push_depth;
    int imm_val;
    double f_val;
    Sym *sym; int sym_addend;
    struct to8_line *jmp_chain;
    int jmp_target_id;
    const char *jump_prefix;
    char comment[56];
    int has_comment;
    int is_target;
    int id;
    struct to8_line *redirect;
    struct to8_line *prev, *next;
    /* --- raw inline-asm passthrough (asm("text");) --- */
    int is_raw_text;
    char rawtext[1024];
} to8_line;

static to8_line *g_head, *g_tail;
static int g_next_id;
static int to8_func_start_ind;

static to8_line **g_by_id;
static int g_by_id_cap;

static int g_pending_target_id;
static int g_frame_size;

static int g_count_before, g_count_after;
static int g_banner_done;
static int g_last_end_ind = -1;

/* Tracks whether we are currently between a gfunc_prolog()/gfunc_epilog()
 * pair. g_head/g_tail/to8_func_start_ind are ONLY valid while this is
 * set - a global/file-scope asm() statement executes OUTSIDE any such
 * pair, so to8_emit_raw_asm_n() must never touch g_head/g_tail when
 * this is 0. */
static int g_in_function = 0;

static void to8_by_id_set(int id, to8_line *ln)
{
    if (id > g_by_id_cap) {
        int nc = g_by_id_cap ? g_by_id_cap * 2 : 256;
        while (nc < id) nc *= 2;
        g_by_id = tcc_realloc(g_by_id, nc * sizeof(*g_by_id));
        g_by_id_cap = nc;
    }
    g_by_id[id - 1] = ln;
}

static to8_line *to8_by_id(int id)
{
    to8_line *ln;
    if (id < 1 || id > g_next_id) {
        tcc_error("TO8: internal error - to8_by_id() called with invalid id %d "
                  "(valid range is 1..%d)", id, g_next_id);
    }
    ln = g_by_id[id - 1];
    if (!ln) {
        tcc_error("TO8: internal error - to8_by_id(%d) resolved to a NULL "
                  "line (never registered or already freed)", id);
    }
    while (ln->redirect) ln = ln->redirect;
    return ln;
}

static void to8_list_reset(void)
{
    g_head = g_tail = NULL;
    g_next_id = 0;
    g_pending_target_id = -1;
}

static to8_line *to8_append(to8_opcode op)
{
    to8_line *ln = tcc_mallocz(sizeof(to8_line));
    ln->op = op;
    ln->id = ++g_next_id;
    to8_by_id_set(ln->id, ln);
    ln->prev = g_tail;
    if (g_tail) g_tail->next = ln; else g_head = ln;
    g_tail = ln;
    if (g_pending_target_id == ln->id) {
        ln->is_target = 1;
        g_pending_target_id = -1;
    }
    ind = to8_func_start_ind + ln->id + 1;
    return ln;
}

static void to8_unlink(to8_line *ln)
{
    if (ln->prev) ln->prev->next = ln->next; else g_head = ln->next;
    if (ln->next) ln->next->prev = ln->prev; else g_tail = ln->prev;
}

static void to8_insert_after(to8_line *after, to8_line *ln)
{
    ln->prev = after;
    ln->next = after->next;
    if (after->next) after->next->prev = ln; else g_tail = ln;
    after->next = ln;
}

static void to8_mark_target(int target_id)
{
    if (target_id >= 1 && target_id <= g_next_id)
        to8_by_id(target_id)->is_target = 1;
    else
        g_pending_target_id = target_id;
}

static int to8_count_lines(void)
{
    int n = 0;
    to8_line *ln;
    for (ln = g_head; ln; ln = ln->next) n++;
    return n;
}

static int g_col;
static void out_char(int c) { g((unsigned char)c); if (c == '\n') g_col = 0; else g_col++; }
static void out_str(const char *s) { while (*s) out_char((unsigned char)*s++); }
static void out_int(int v) { char buf[16]; snprintf(buf, sizeof buf, "%d", v); out_str(buf); }
static void out_double(double v) { char buf[32]; snprintf(buf, sizeof buf, "%g", v); out_str(buf); }
static void out_tab(void)
{
    int next = ((g_col / 8) + 1) * 8;
    while (g_col < next) out_char(' ');
}

/* Pad with spaces up to a FIXED column, instead of always advancing
 * by one full tab stop the way out_tab() does. Used just before the
 * comment, so every line's comment starts at the same place whether
 * or not it printed an argument. */
static void out_pad_to(int col)
{
    while (g_col < col) out_char(' ');
}

#define TO8_COMMENT_COL 24

static void to8_print_banner(void)
{
    g_col = 0;
    out_str("; TO8 backend "); out_str(TO8_GEN_VERSION); out_char('\n');

    out_str("; out: ");
    out_str((tcc_state && tcc_state->outfile) ? tcc_state->outfile : "(unknown)");
    out_str(" src: ");
    out_str((file && file->filename[0]) ? file->filename : "(unknown)");
    out_char('\n');

    if (tcc_state && tcc_state->argc > 0 && tcc_state->argv) {
        int i;
        out_str("; cmd:");
        for (i = 0; i < tcc_state->argc; i++) {
            if (!tcc_state->argv[i]) continue;
            out_char(' ');
            out_str(tcc_state->argv[i]);
        }
        out_char('\n');
    }
}

/* ===================================================================
 * Raw inline-asm passthrough: emit a NOP-carrier line whose text is
 * dumped verbatim by to8_render_line(), bypassing normal opcode/arg
 * rendering entirely. Called from asm_opcode() (to8-stubs.c) for
 * asm("text"); statements.
 *
 * IMPORTANT: 'text' is NOT a NUL-terminated C string, and must never
 * be scanned for a terminator of any kind - the caller MUST pass the
 * exact length, and this function copies EXACTLY that many bytes,
 * with no scanning whatsoever, into a manually NUL-terminated local
 * buffer.
 *
 * Inside a function, append normally to the function's own line list
 * (rendered later by gfunc_epilog()). At global/file scope, write
 * directly to the output stream instead, bypassing to8_append().
 * =================================================================== */

ST_FUNC void to8_emit_raw_asm_n(const char *text, size_t len)
{
    size_t cap = sizeof(((to8_line *)0)->rawtext) - 1;
    if (len > cap) len = cap;

    if (g_in_function) {
        to8_line *ln = to8_append(OP_NOP);
        ln->kind = ARG_NONE;
        ln->is_raw_text = 1;
        memcpy(ln->rawtext, text, len);
        ln->rawtext[len] = '\0';
        return;
    }

    /* global/file-scope asm(): write directly, no g_head/g_tail,
     * no NUL-scanning - byte-count loop only. */
    if (!g_banner_done) {
        to8_print_banner();
        g_banner_done = 1;
    }
    g_col = 0;
    {
        size_t i;
        for (i = 0; i < len; i++)
            out_char((unsigned char)text[i]);
    }
    out_char('\n');
    g_last_end_ind = ind;
}

/* ===================================================================
 * Comment templates - switch on the enum, computed eagerly.
 * =================================================================== */

static void slot_desc(char *out, size_t outsz, int slot)
{
    if (slot < 0) snprintf(out, outsz, "local[%d]", -slot);
    else snprintf(out, outsz, "param[%d]", slot);
}

static const char *to8_size_name(to8_opcode op)
{
    switch (op) {
    case OP_LD1: case OP_ST1: return "signed char";
    case OP_LDU1: return "unsigned char";
    case OP_LD2: case OP_ST2: return "signed short";
    case OP_LDU2: return "unsigned short";
    default: return "int";
    }
}

static void slot_comment(char *out, size_t outsz, to8_opcode op, const char *desc)
{
    switch (op) {
    case OP_LD: snprintf(out, outsz, "R0 = %s", desc); return;
    case OP_ST: snprintf(out, outsz, "%s = R0", desc); return;
    case OP_LEA: snprintf(out, outsz, "R0 = &%s", desc); return;
    case OP_ADD: snprintf(out, outsz, "R0 += %s", desc); return;
    case OP_SUB: snprintf(out, outsz, "R0 -= %s", desc); return;
    case OP_AND: snprintf(out, outsz, "R0 &= %s", desc); return;
    case OP_OR: snprintf(out, outsz, "R0 |= %s", desc); return;
    case OP_XOR: snprintf(out, outsz, "R0 ^= %s", desc); return;
    case OP_MUL: snprintf(out, outsz, "R0 *= %s", desc); return;
    case OP_DIV: snprintf(out, outsz, "R0 /= %s", desc); return;
    case OP_MOD: snprintf(out, outsz, "R0 %%= %s", desc); return;
    case OP_SHL: snprintf(out, outsz, "R0 <<= %s", desc); return;
    case OP_SHR: snprintf(out, outsz, "R0 >>= %s", desc); return;
    case OP_SAR: snprintf(out, outsz, "R0 >>= %s (arith)", desc); return;
    case OP_CMP: snprintf(out, outsz, "R0 = sign(R0 - %s)", desc); return;
    case OP_CMPU: snprintf(out, outsz, "R0 = sign(R0 -u %s)", desc); return;
    case OP_CMPF: snprintf(out, outsz, "R0 = sign(F0 - %s) (float)", desc); return;
    case OP_CMPG: snprintf(out, outsz, "R0 = sign(F0 - %s) (double)", desc); return;
    case OP_ADDF: snprintf(out, outsz, "F0 += %s (float)", desc); return;
    case OP_SUBF: snprintf(out, outsz, "F0 -= %s (float)", desc); return;
    case OP_MULF: snprintf(out, outsz, "F0 *= %s (float)", desc); return;
    case OP_DIVF: snprintf(out, outsz, "F0 /= %s (float)", desc); return;
    case OP_ADDG: snprintf(out, outsz, "F0 += %s (double)", desc); return;
    case OP_SUBG: snprintf(out, outsz, "F0 -= %s (double)", desc); return;
    case OP_MULG: snprintf(out, outsz, "F0 *= %s (double)", desc); return;
    case OP_DIVG: snprintf(out, outsz, "F0 /= %s (double)", desc); return;
    case OP_STF: case OP_STG: snprintf(out, outsz, "%s = F0", desc); return;
    case OP_LDF: case OP_LDG: snprintf(out, outsz, "F0 = %s", desc); return;
    case OP_STF4: case OP_STF8: snprintf(out, outsz, "*%s = F0", desc); return;
    case OP_LDF4: case OP_LDF8: snprintf(out, outsz, "F0 = *%s", desc); return;
    case OP_PUSH: snprintf(out, outsz, "push %s", desc); return;
    case OP_JSR: snprintf(out, outsz, "call *%s", desc); return;
    case OP_LD1: case OP_LDU1: case OP_LD2: case OP_LDU2: case OP_LD4:
        snprintf(out, outsz, "R0 = *(%s*)%s", to8_size_name(op), desc); return;
    case OP_ST1: case OP_ST2: case OP_ST4:
        snprintf(out, outsz, "*(%s*)%s = R0", to8_size_name(op), desc); return;
    default: snprintf(out, outsz, "%s", desc); return;
    }
}

static void imm_comment(char *out, size_t outsz, to8_opcode op, int v)
{
    char num[16];
    snprintf(num, sizeof num, "%d", v);
    switch (op) {
    case OP_LDi: snprintf(out, outsz, "R0 = %s", num); return;
    case OP_ADDi: snprintf(out, outsz, "R0 += %s", num); return;
    case OP_SUBi: snprintf(out, outsz, "R0 -= %s", num); return;
    case OP_ANDi: snprintf(out, outsz, "R0 &= %s", num); return;
    case OP_ORi: snprintf(out, outsz, "R0 |= %s", num); return;
    case OP_XORi: snprintf(out, outsz, "R0 ^= %s", num); return;
    case OP_MULi: snprintf(out, outsz, "R0 *= %s", num); return;
    case OP_DIVi: snprintf(out, outsz, "R0 /= %s", num); return;
    case OP_MODi: snprintf(out, outsz, "R0 %%= %s", num); return;
    case OP_SHLi: snprintf(out, outsz, "R0 <<= %s", num); return;
    case OP_SHRi: snprintf(out, outsz, "R0 >>= %s", num); return;
    case OP_SARi: snprintf(out, outsz, "R0 >>= %s (arith)", num); return;
    case OP_CMPi: snprintf(out, outsz, "R0 = sign(R0 - %s)", num); return;
    case OP_CMPUi: snprintf(out, outsz, "R0 = sign(R0 -u %s)", num); return;
    case OP_ADJi: snprintf(out, outsz, v < 0 ? "sp -= %s" : "sp += %s", v < 0 ? num + 1 : num); return;
    case OP_PUSHi: snprintf(out, outsz, "push %s", num); return;
    default: snprintf(out, outsz, "%s", num); return;
    }
}

static void addr_comment(char *out, size_t outsz, to8_opcode op, const char *desc)
{
    switch (op) {
    case OP_LDi: snprintf(out, outsz, "R0 = &%s", desc); return;
    case OP_JSRi: snprintf(out, outsz, "call %s", desc); return;
    case OP_LDF4: case OP_LDF8: snprintf(out, outsz, "F0 = %s", desc); return;
    case OP_STF4: case OP_STF8: snprintf(out, outsz, "%s = F0", desc); return;
    default: snprintf(out, outsz, "%s", desc); return;
    }
}

static const char *bare_comment(to8_opcode op)
{
    switch (op) {
    case OP_RET: return "return";
    case OP_PUSHr: return "push R0";
    case OP_JSRr: return "call *R0";
    case OP_LD1r: return "R0 = (int)(signed char)*(char*)R0";
    case OP_LDU1r: return "R0 = (int)(unsigned char)*(char*)R0";
    case OP_LD2r: return "R0 = (int)(signed short)*(short*)R0";
    case OP_LDU2r: return "R0 = (int)(unsigned short)*(short*)R0";
    case OP_LD4r: return "R0 = *(int*)R0";
    case OP_ITOF: return "R0 -> F0 (int to float)";
    case OP_FTOI: return "F0 -> R0 (float to int)";
    case OP_SEQ: return "R0 = (== ? 1 : 0)";
    case OP_SNE: return "R0 = (!= ? 1 : 0)";
    case OP_SLT: return "R0 = (< ? 1 : 0)";
    case OP_SGT: return "R0 = (> ? 1 : 0)";
    case OP_SLE: return "R0 = (<= ? 1 : 0)";
    case OP_SGE: return "R0 = (>= ? 1 : 0)";
    case OP_SNZ: return "R0 = (R0 != 0 ? 1 : 0)";
    case OP_EXT1S: return "R0 = (int)(char)R0";
    case OP_EXT2S: return "R0 = (int)(short)R0";
    case OP_EXT1U: return "R0 = (int)(unsigned char)R0";
    case OP_EXT2U: return "R0 = (int)(unsigned short)R0";
    case OP_NOP: return "no-op";
    default: return NULL;
    }
}

static const char *to8_jump_prefix(to8_opcode op)
{
    switch (op) {
    case OP_JMP: return "goto";
    case OP_JEQ: return "if (==) goto";
    case OP_JNE: return "if (!=) goto";
    case OP_JLT: return "if (<) goto";
    case OP_JGT: return "if (>) goto";
    case OP_JLE: return "if (<=) goto";
    case OP_JGE: return "if (>=) goto";
    default: return "goto";
    }
}

/* ===================================================================
 * Append helpers - plain, no fusion bookkeeping.
 * =================================================================== */

static void e_op(to8_opcode op)
{
    to8_line *ln = to8_append(op);
    const char *c = bare_comment(op);
    ln->kind = ARG_NONE;
    if (c) { snprintf(ln->comment, sizeof ln->comment, "%s", c); ln->has_comment = 1; }
}

static to8_line *e_op_imm(to8_opcode op, int v)
{
    to8_line *ln = to8_append(op);
    ln->kind = ARG_IMM;
    ln->imm_val = v;
    imm_comment(ln->comment, sizeof ln->comment, op, v);
    ln->has_comment = 1;
    return ln;
}

static to8_line *e_op_fimm(to8_opcode op, double v)
{
    to8_line *ln = to8_append(op);
    ln->kind = ARG_FIMM;
    ln->f_val = v;
    snprintf(ln->comment, sizeof ln->comment, "F0 = %g", v);
    ln->has_comment = 1;
    return ln;
}

static void e_op_slot(to8_opcode op, int slot)
{
    to8_line *ln = to8_append(op);
    char desc[24];
    if (slot & (TO8_STACK_ALIGN-1)) {
        tcc_error("TO8 internal error: %s on non-4-aligned slot %d",
                  to8_opcode_name(op), slot);
    }
    ln->kind = ARG_SLOT;
    ln->slot_a = slot;
    ln->push_depth = cur_push_depth;
    slot_desc(desc, sizeof desc, slot);
    slot_comment(ln->comment, sizeof ln->comment, op, desc);
    ln->has_comment = 1;
}

static void e_op_addr(to8_opcode op, Sym *sym, int c)
{
    to8_line *ln = to8_append(op);
    char desc[32];
    ln->kind = ARG_SYM;
    ln->sym = sym;
    ln->sym_addend = c;
    if (sym) {
        const char *name = get_tok_str(sym->v, NULL);
        snprintf(desc, sizeof desc, "%s", name ? name : "?");
    } else {
        snprintf(desc, sizeof desc, "%d", c);
    }
    addr_comment(ln->comment, sizeof ln->comment, op, desc);
    ln->has_comment = 1;
}

static void to8_track_push(void) { cur_push_depth += PTR_SIZE; }

/* single point of truth for "don't emit a no-op stack adjustment".
 * Used by gfunc_call() (always n>0 there in practice, guarded by
 * "if (nb_args > 0)" at the call site already) AND by gfunc_epilog()'s
 * frame-release, so a function with no locals never prints a useless
 * "ADJi 0". */
static void to8_adjust(int n)
{
    if (n == 0)
        return;
    e_op_imm(OP_ADJi, n);
    cur_push_depth -= n;
}

static void e_push_imm(int v)
{
    to8_line *ln = to8_append(OP_PUSHi);
    ln->kind = ARG_IMM;
    ln->imm_val = v;
    imm_comment(ln->comment, sizeof ln->comment, OP_PUSHi, v);
    ln->has_comment = 1;
    to8_track_push();
}

static void e_push_slot(int slot)
{
    to8_line *ln = to8_append(OP_PUSH);
    char desc[24];
    ln->kind = ARG_SLOT;
    ln->slot_a = slot;
    ln->push_depth = cur_push_depth;
    slot_desc(desc, sizeof desc, slot);
    slot_comment(ln->comment, sizeof ln->comment, OP_PUSH, desc);
    ln->has_comment = 1;
    to8_track_push();
}

static void e_push_r0(void) { e_op(OP_PUSHr); to8_track_push(); }

/* ===================================================================
 * Jump backpatching.
 * =================================================================== */

ST_FUNC void gsym_addr(int t, int a)
{
    int abs_target = (a < 0) ? -a : a;
    int target = abs_target - to8_func_start_ind;
    while (t) {
        to8_line *ln = to8_by_id(t);
        to8_line *next = ln->jmp_chain;
        ln->jmp_target_id = target;
        ln->jmp_chain = NULL;
        t = next ? next->id : 0;
    }
    to8_mark_target(target);
}

/* ===================================================================
 * Helpers for operations
 * =================================================================== */

static int to8_is_commutative(int op)
{
    return op == '+' || op == '*' || op == '&' || op == '|' || op == '^';
}

static int to8_get_arith_ops(int op, to8_opcode *slot_op, to8_opcode *imm_op)
{
    switch (op) {
    case '+': *slot_op = OP_ADD; *imm_op = OP_ADDi; return 0;
    case '-': *slot_op = OP_SUB; *imm_op = OP_SUBi; return 0;
    case '&': *slot_op = OP_AND; *imm_op = OP_ANDi; return 0;
    case '|': *slot_op = OP_OR; *imm_op = OP_ORi; return 0;
    case '^': *slot_op = OP_XOR; *imm_op = OP_XORi; return 0;
    case '*': *slot_op = OP_MUL; *imm_op = OP_MULi; return 0;
    case '/': *slot_op = OP_DIV; *imm_op = OP_DIVi; return 0;
    case '%': *slot_op = OP_MOD; *imm_op = OP_MODi; return 0;
    case TOK_SHL: *slot_op = OP_SHL; *imm_op = OP_SHLi; return 0;
    case TOK_SHR: *slot_op = OP_SHR; *imm_op = OP_SHRi; return 0;
    case TOK_SAR: *slot_op = OP_SAR; *imm_op = OP_SARi; return 0;
    case TOK_UDIV: *slot_op = OP_DIV; *imm_op = OP_DIVi; return 0;
    case TOK_UMOD: *slot_op = OP_MOD; *imm_op = OP_MODi; return 0;
    default: return -1;
    }
}

static to8_opcode to8_get_set_cond(int op)
{
    switch (op) {
    case TOK_EQ: return OP_SEQ;
    case TOK_NE: return OP_SNE;
    case TOK_LT: case TOK_ULT: return OP_SLT;
    case TOK_GT: case TOK_UGT: return OP_SGT;
    case TOK_LE: case TOK_ULE: return OP_SLE;
    case TOK_GE: case TOK_UGE: return OP_SGE;
    default: return OP_SNZ;
    }
}

static to8_opcode to8_get_jcond(int op)
{
    switch (op) {
    case TOK_EQ: return OP_JEQ;
    case TOK_NE: return OP_JNE;
    case TOK_LT: case TOK_ULT: return OP_JLT;
    case TOK_GT: case TOK_UGT: return OP_JGT;
    case TOK_LE: case TOK_ULE: return OP_JLE;
    case TOK_GE: case TOK_UGE: return OP_JGE;
    default: return OP_JNE;
    }
}

static int to8_swap_cmp_op(int op)
{
    switch (op) {
    case TOK_LT: return TOK_GT;
    case TOK_GT: return TOK_LT;
    case TOK_LE: return TOK_GE;
    case TOK_GE: return TOK_LE;
    case TOK_ULT: return TOK_UGT;
    case TOK_UGT: return TOK_ULT;
    case TOK_ULE: return TOK_UGE;
    case TOK_UGE: return TOK_ULE;
    default: return op;
    }
}

/* takes the operand's signedness explicitly instead of only its base
 * type. VT_BOOL is special-cased: a _Bool has no signed variant in C,
 * so it always routes to the unsigned (zero-extending) form
 * regardless of what VT_UNSIGNED happens to say. */
static to8_opcode to8_byte_suffix_ld(int bt, int is_unsigned)
{
    if (bt == VT_BOOL) return OP_LDU1;
    if (bt == VT_BYTE) return is_unsigned ? OP_LDU1 : OP_LD1;
    if (bt == VT_SHORT) return is_unsigned ? OP_LDU2 : OP_LD2;
    return OP_LD4;
}

static to8_opcode to8_byte_suffix_ld_r(int bt, int is_unsigned)
{
    if (bt == VT_BOOL) return OP_LDU1r;
    if (bt == VT_BYTE) return is_unsigned ? OP_LDU1r : OP_LD1r;
    if (bt == VT_SHORT) return is_unsigned ? OP_LDU2r : OP_LD2r;
    return OP_LD4r;
}

/* Stores never need a signedness argument: truncation on write does
 * not care about sign, so this one is unchanged. */
static to8_opcode to8_byte_suffix_st(int bt)
{
    if (bt == VT_BYTE || bt == VT_BOOL) return OP_ST1;
    if (bt == VT_SHORT) return OP_ST2;
    return OP_ST4;
}

/* ===================================================================
 * Float literal inlining: if `sym` is a compiler-generated anonymous
 * constant (never a real, possibly-mutable named global), read its
 * already-written bytes straight out of its section and hand back
 * the real value - so the caller can emit a true LDFi/LDGi immediate
 * instead of a symbol reference. Any doubt at all (wrong symbol
 * range, missing section, out-of-bounds offset) returns 0 and the
 * caller falls back to the existing symbol-based path.
 * =================================================================== */

static int to8_try_read_float_const(Sym *sym, int bt, double *out_val)
{
    ElfSym *esym;
    Section *sec;
    int size = (bt == VT_FLOAT) ? 4 : 8;
    unsigned long offset;

    if (!sym || sym->v < SYM_FIRST_ANOM)
        return 0;
    esym = elfsym(sym);
    if (!esym || esym->st_shndx == 0)
        return 0;
    if (!tcc_state || esym->st_shndx >= tcc_state->nb_sections)
        return 0;

    sec = tcc_state->sections[esym->st_shndx];
    if (!sec || !sec->data)
        return 0;

    offset = (unsigned long)esym->st_value;
    if (offset + size > sec->data_offset)
        return 0;

    if (bt == VT_FLOAT) {
        float f;
        memcpy(&f, sec->data + offset, sizeof f);
        *out_val = (double)f;
    } else {
        double d;
        memcpy(&d, sec->data + offset, sizeof d);
        *out_val = d;
    }
    return 1;
}

/* ===================================================================
 * Load / Store
 * =================================================================== */

void load(int r, SValue *sv)
{
    int v, ft, fc, bt;
    int is_unsigned;
    int save_slot;
    to8_opcode ldop;

    ft = sv->type.t & ~VT_DEFSIGN;
    fc = sv->c.i;
    v = sv->r & VT_VALMASK;
    ft &= ~(VT_VOLATILE | VT_CONSTANT);
    bt = ft & VT_BTYPE;
    is_unsigned = (sv->type.t & VT_UNSIGNED) != 0;
    ldop = to8_byte_suffix_ld(bt, is_unsigned);

    if (r == TREG_F0) {
        if (bt == VT_FLOAT || bt == VT_DOUBLE) {
            to8_opcode ld = (bt == VT_FLOAT) ? OP_LDF : OP_LDG;
            to8_opcode ld4or8 = (bt == VT_FLOAT) ? OP_LDF4 : OP_LDF8;
            to8_opcode ldi = (bt == VT_FLOAT) ? OP_LDFi : OP_LDGi;
            if (sv->r & VT_LVAL) {
                if (v == VT_LOCAL) {
                    e_op_slot(ld, fc);
                } else if (v == VT_LLOCAL) {
                    e_op_slot(ld4or8, fc);
                } else if (v == VT_CONST) {
                    double fval;
                    if (to8_try_read_float_const(sv->sym, bt, &fval))
                        e_op_fimm(ldi, fval);
                    else
                        e_op_addr(ld4or8, sv->sym, fc);
                } else if (v < VT_CONST) {
                    int temp;
                    int from_pool = (v != TREG_R1);
                    if (v == TREG_R1) temp = to8_r1_slot();
                    else { temp = to8_temp_alloc(4, 4); e_op_slot(OP_ST, temp); }
                    e_op_slot(ld4or8, temp);
                    if (from_pool) to8_temp_free(temp, 4, 4);
                } else {
                    tcc_error("TO8: unsupported float load addressing mode (v=%#x)", v);
                }
            } else {
                if (v == VT_CONST)
                    e_op_addr(ld, sv->sym, fc);
                else if (v == VT_LOCAL)
                    e_op_slot(ld, fc);
                else
                    e_op(OP_ITOF);
            }
            return;
        }
    }

    if (!(sv->r & VT_LVAL) && v < TREG_MEM && v != VT_CONST && v != VT_LOCAL &&
        v != VT_LLOCAL && v != VT_CMP && v != VT_JMP && v != VT_JMPI) {
        if (v == r) return;
        if (v == TREG_R1 && r == TREG_R0) { e_op_slot(OP_LD, to8_r1_slot()); return; }
        if (v == TREG_R0 && r == TREG_R1) { e_op_slot(OP_ST, to8_r1_slot()); return; }
    }

    save_slot = -1;
    if (r == TREG_R1) {
        save_slot = to8_temp_alloc(4, 4);
        e_op_slot(OP_ST, save_slot);
    }

    if (sv->r & VT_LVAL) {
        if (v == VT_LOCAL) {
            e_op_slot(OP_LD, fc);
        } else if (v == VT_LLOCAL) {
            e_op_slot(ldop, fc);
        } else if (v == VT_CONST) {
            e_op_addr(ldop, sv->sym, fc);
        } else if (v == TREG_R0) {
            /* Pointer already sitting in R0: dereference directly,
             * no temp-slot spill needed. */
            e_op(to8_byte_suffix_ld_r(bt, is_unsigned));
        } else if (v < VT_CONST) {
            int temp;
            int from_pool = (v != TREG_R1);
            if (v == TREG_R1) temp = to8_r1_slot();
            else { temp = to8_temp_alloc(4, 4); e_op_slot(OP_ST, temp); }
            e_op_slot(ldop, temp);
            if (from_pool) to8_temp_free(temp, 4, 4);
        } else {
            tcc_error("TO8: unsupported load addressing mode (v=%#x)", v);
        }
    } else {
        switch (v) {
        case VT_CONST:
            if (sv->r & VT_SYM)
                e_op_addr(OP_LDi, sv->sym, fc);
            else
                e_op_imm(OP_LDi, fc);
            break;
        case VT_LOCAL:
            e_op_slot(OP_LEA, fc);
            break;
        case VT_LLOCAL:
            e_op_slot(OP_LD, fc);
            break;
        case VT_CMP:
            e_op(to8_get_set_cond(sv->c.i));
            break;
        case VT_JMP:
            e_op_imm(OP_LDi, 0);
            gsym(fc);
            e_op_imm(OP_LDi, 1);
            break;
        case VT_JMPI:
            e_op_imm(OP_LDi, 1);
            gsym(fc);
            e_op_imm(OP_LDi, 0);
            break;
        default:
            if (v == r) {
                if (save_slot >= 0) e_op_slot(OP_LD, save_slot);
                return;
            }
            if (v >= TREG_MEM) {
                e_op_slot(ldop, fc);
            }
            break;
        }
    }

    if (r == TREG_R1) {
        e_op_slot(OP_ST, to8_r1_slot());
        e_op_slot(OP_LD, save_slot);
        to8_temp_free(save_slot, 4, 4);
    }
}

void store(int r, SValue *v)
{
    int fr, bt, fc;
    to8_opcode stop;

    if (r >= NB_REGS) {
        int vbt = vtop->type.t & VT_BTYPE;
        r = (vbt == VT_FLOAT || vbt == VT_DOUBLE) ? TREG_F0 : TREG_R0;
        load(r, vtop);
    }

    fr = v->r & VT_VALMASK;
    bt = v->type.t & VT_BTYPE;
    fc = v->c.i;
    bt &= ~(VT_VOLATILE | VT_CONSTANT);
    stop = to8_byte_suffix_st(bt);

    if (r == TREG_F0) {
        to8_opcode st = (bt == VT_FLOAT) ? OP_STF : OP_STG;
        to8_opcode st4or8 = (bt == VT_FLOAT) ? OP_STF4 : OP_STF8;
        if (fr == VT_LOCAL) {
            e_op_slot(st, fc);
        } else if (fr == VT_LLOCAL) {
            e_op_slot(st4or8, fc);
        } else if (fr == VT_CONST) {
            e_op_addr(st4or8, v->sym, fc);
        } else if (fr < VT_CONST) {
            int temp;
            int from_pool = (fr != TREG_R1);
            if (fr == TREG_R1) temp = to8_r1_slot();
            else { temp = to8_temp_alloc(4, 4); e_op_slot(OP_ST, temp); }
            e_op_slot(st4or8, temp);
            if (from_pool) to8_temp_free(temp, 4, 4);
        } else {
            tcc_error("TO8: unsupported float store addressing mode (fr=%#x)", fr);
        }
        return;
    }

    if (fr == VT_LOCAL) {
        if (r == TREG_R1) e_op_slot(OP_LD, to8_r1_slot());
        e_op_slot(OP_ST, fc);
    } else if (fr == VT_LLOCAL) {
        if (r == TREG_R1) e_op_slot(OP_LD, to8_r1_slot());
        e_op_slot(stop, fc);
    } else if (fr == VT_CONST && (v->r & VT_SYM)) {
        if (r == TREG_R1) e_op_slot(OP_LD, to8_r1_slot());
        e_op_addr(stop, v->sym, fc);
    } else if (v->r & VT_LVAL) {
        int addr_slot;
        int from_pool = (fr != TREG_R1);
        if (fr == TREG_R1) addr_slot = to8_r1_slot();
        else { addr_slot = to8_temp_alloc(4, 4); e_op_slot(OP_ST, addr_slot); }
        if (r == TREG_R1) e_op_slot(OP_LD, to8_r1_slot());
        e_op_slot(stop, addr_slot);
        if (from_pool) to8_temp_free(addr_slot, 4, 4);
    }
}

/* ===================================================================
 * Binary integer operations
 * =================================================================== */

static int to8_spill_and_reload(int v1, int c1)
{
    int temp = to8_temp_alloc(4, 4);
    e_op_slot(OP_ST, temp);
    if (v1 == TREG_R1) {
        e_op_slot(OP_LD, to8_r1_slot());
    } else {
        e_op_slot(OP_LD, c1);
        if (v1 == TREG_R0)
            to8_temp_free(c1, 4, 4);
    }
    return temp;
}

static void gen_opi_shift(int op)
{
    to8_opcode slot_op, imm_op;
    int amount_is_const = (vtop->r & VT_VALMASK) == VT_CONST && !(vtop->r & VT_SYM);
    int amount_val = amount_is_const ? vtop->c.i : 0;

    to8_get_arith_ops(op, &slot_op, &imm_op);

    if (amount_is_const) {
        vpop();
        gv(RC_INT);
        e_op_imm(imm_op, amount_val);
        vtop->r = TREG_R0;
        vtop->r2 = VT_CONST;
    } else {
        int v1, c1, temp;
        v1 = vtop[-1].r & VT_VALMASK;
        c1 = vtop[-1].c.i;
        if (v1 == TREG_R0) {
            int pre_spill = to8_temp_alloc(4, 4);
            e_op_slot(OP_ST, pre_spill);
            c1 = pre_spill;
        }

        gv(RC_INT);
        if (v1 != TREG_R0) {
            v1 = vtop[-1].r & VT_VALMASK;
            c1 = vtop[-1].c.i;
        }

        temp = to8_spill_and_reload(v1, c1);
        e_op_slot(slot_op, temp);
        to8_temp_free(temp, 4, 4);
        vtop--;
        vtop->r = TREG_R0;
        vtop->r2 = VT_CONST;
    }
}

void gen_opi(int op)
{
    int v1, c1;

    if (op == TOK_SHL || op == TOK_SAR || op == TOK_SHR) {
        gen_opi_shift(op);
        return;
    }

    if (op == TOK_LT || op == TOK_GT || op == TOK_LE || op == TOK_GE ||
        op == TOK_EQ || op == TOK_NE || op == TOK_ULT || op == TOK_UGT ||
        op == TOK_ULE || op == TOK_UGE) {

        int is_unsigned_cmp = (op == TOK_ULT || op == TOK_UGT ||
                                op == TOK_ULE || op == TOK_UGE);
        int tst_ok = (op == TOK_EQ || op == TOK_NE ||
                      op == TOK_LT || op == TOK_GT || op == TOK_LE || op == TOK_GE);

        if ((vtop->r & VT_VALMASK) == VT_CONST && !(vtop->r & VT_SYM)) {
            int c0 = vtop->c.i;
            vpop();
            gv(RC_R0);
            if (!(c0 == 0 && tst_ok)) {
                e_op_imm(is_unsigned_cmp ? OP_CMPUi : OP_CMPi, c0);
            }
            vset_VT_CMP(op);
            return;
        }

        if ((vtop[-1].r & VT_VALMASK) == VT_CONST && !(vtop[-1].r & VT_SYM)) {
            vswap();
            gen_opi(to8_swap_cmp_op(op));
            return;
        }

        gv(RC_R0);
        v1 = vtop[-1].r & VT_VALMASK;
        c1 = vtop[-1].c.i;

        {
            int temp = to8_spill_and_reload(v1, c1);
            e_op_slot(is_unsigned_cmp ? OP_CMPU : OP_CMP, temp);
            to8_temp_free(temp, 4, 4);
        }

        vtop--;
        vset_VT_CMP(op);
        return;
    }

    v1 = vtop[-1].r & VT_VALMASK;
    c1 = vtop[-1].c.i;

    if (v1 == TREG_R0) {
        int pre_spill = to8_temp_alloc(4, 4);
        e_op_slot(OP_ST, pre_spill);
        c1 = pre_spill;
    }

    gv(RC_INT);

    if (v1 != TREG_R0) {
        v1 = vtop[-1].r & VT_VALMASK;
        c1 = vtop[-1].c.i;
    }

    {
        to8_opcode slot_op, imm_op;
        if (to8_get_arith_ops(op, &slot_op, &imm_op) < 0) {
            vtop--; return;
        }

        if (to8_is_commutative(op)) {
            if (v1 == VT_CONST) {
                e_op_imm(imm_op, c1);
            } else if (v1 == TREG_R0 || v1 == TREG_R1) {
                int temp = to8_spill_and_reload(v1, c1);
                e_op_slot(slot_op, temp);
                to8_temp_free(temp, 4, 4);
            } else {
                e_op_slot(slot_op, c1);
            }
        } else {
            int temp;
            if (v1 == VT_CONST) {
                temp = to8_temp_alloc(4, 4);
                e_op_slot(OP_ST, temp);
                e_op_imm(OP_LDi, c1);
                e_op_slot(slot_op, temp);
                to8_temp_free(temp, 4, 4);
            } else {
                temp = to8_spill_and_reload(v1, c1);
                e_op_slot(slot_op, temp);
                to8_temp_free(temp, 4, 4);
            }
        }
    }

    vtop--;
    vtop->r = TREG_R0;
    vtop->r2 = VT_CONST;
}

/* ===================================================================
 * Binary float operations
 * =================================================================== */

void gen_opf(int op)
{
    int bt;

    gv(RC_FLOAT);
    bt = vtop->type.t & VT_BTYPE; /* VT_FLOAT or VT_DOUBLE - width of THIS operation */

    if (op == TOK_LT || op == TOK_GT || op == TOK_LE || op == TOK_GE ||
        op == TOK_EQ || op == TOK_NE) {
        to8_opcode own_st = (bt == VT_FLOAT) ? OP_STF : OP_STG;
        to8_opcode own_ld = (bt == VT_FLOAT) ? OP_LDF : OP_LDG;
        to8_opcode cmp_op = (bt == VT_FLOAT) ? OP_CMPF : OP_CMPG;
        int tsize = (bt == VT_FLOAT) ? 4 : 8;
        int fv1 = vtop[-1].r & VT_VALMASK;
        int fc1 = vtop[-1].c.i;
        int temp = to8_temp_alloc(tsize, 4);
        e_op_slot(own_st, temp);
        if (fv1 == VT_LOCAL || fv1 == VT_LLOCAL)
            e_op_slot(own_ld, fc1);
        e_op_slot(cmp_op, temp);
        to8_temp_free(temp, tsize, 4);

        vtop--;
        vset_VT_CMP(op);
        return;
    }

    {
        to8_opcode fop;
        to8_opcode own_st = (bt == VT_FLOAT) ? OP_STF : OP_STG;
        to8_opcode own_ld = (bt == VT_FLOAT) ? OP_LDF : OP_LDG;
        int tsize = (bt == VT_FLOAT) ? 4 : 8;
        int fv1 = vtop[-1].r & VT_VALMASK;
        int fc1 = vtop[-1].c.i;
        int commutative;

        switch (op) {
        case '+': fop = (bt == VT_FLOAT) ? OP_ADDF : OP_ADDG; break;
        case '-': fop = (bt == VT_FLOAT) ? OP_SUBF : OP_SUBG; break;
        case '*': fop = (bt == VT_FLOAT) ? OP_MULF : OP_MULG; break;
        case '/': fop = (bt == VT_FLOAT) ? OP_DIVF : OP_DIVG; break;
        default: fop = OP_NOP; break;
        }

        commutative = (op == '+' || op == '*');

        if (fop != OP_NOP) {
            if (commutative && (fv1 == VT_LOCAL || fv1 == VT_LLOCAL)) {
                e_op_slot(fop, fc1);
            } else {
                int temp = to8_temp_alloc(tsize, 4);
                e_op_slot(own_st, temp);
                if (fv1 == VT_LOCAL || fv1 == VT_LLOCAL)
                    e_op_slot(own_ld, fc1);
                e_op_slot(fop, temp);
                to8_temp_free(temp, tsize, 4);
            }
        }

        vtop--;
        vtop->r = TREG_F0;
        vtop->r2 = VT_CONST;
    }
}

/* ===================================================================
 * Type conversions
 * =================================================================== */

void gen_cvt_itof(int t) { e_op(OP_ITOF); }
void gen_cvt_ftoi(int t) { e_op(OP_FTOI); }
void gen_cvt_ftof(int t) { /* precision change placeholder */ }

/* ===================================================================
 * Function call ABI
 * =================================================================== */

ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret,
                        int *ret_align, int *regsize)
{
    int size, align;
    *ret_align = TO8_STACK_ALIGN;
    *regsize = 4;
    size = type_size(vt, &align);
    if (size > 4) return 0;
    if (size == 4) ret->t = VT_INT;
    else if (size == 2) ret->t = VT_SHORT;
    else ret->t = VT_BYTE;
    ret->ref = NULL;
    return 1;
}

void gfunc_call(int nb_args)
{
    int i, size;
    SValue *func = &vtop[-nb_args];

    for (i = 0; i < nb_args; i++) {
        SValue *sv = &vtop[-i];
        int v = sv->r & VT_VALMASK;
        if (v == VT_CONST && !(sv->r & VT_SYM)) {
            e_push_imm(sv->c.i);
        } else if (v == VT_LOCAL || v == VT_LLOCAL) {
            e_push_slot(sv->c.i);
        } else if (sv->r & VT_SYM) {
            e_op_addr(OP_LDi, sv->sym, sv->c.i);
            e_push_r0();
        } else {
            load(TREG_R0, sv);
            e_push_r0();
        }
    }

    save_regs(0);

    if ((func->r & VT_VALMASK) == VT_CONST && (func->r & VT_SYM)) {
        e_op_addr(OP_JSRi, func->sym, func->c.i);
    } else if ((func->r & VT_LVAL) &&
               ((func->r & VT_VALMASK) == VT_LOCAL || (func->r & VT_VALMASK) == VT_LLOCAL)) {
        e_op_slot(OP_JSR, func->c.i);
    } else {
        load(TREG_R0, func);
        e_op(OP_JSRr);
    }

    if (nb_args > 0) {
        size = nb_args * PTR_SIZE;
        to8_adjust(size);
    }

    vtop -= nb_args + 1;
}

/* ===================================================================
 * Peephole passes - run to a FIXED POINT, in gfunc_epilog, over the
 * finished list, and ONLY when the user opted in via -O1 or higher.
 * Each pass returns 1 if it changed the list, 0 otherwise; the driver
 * loop re-runs all three until a full round changes nothing.
 * =================================================================== */

static int to8_peephole_ext(void)
{
    int changed = 0;
    to8_line *cur = g_head;
    while (cur) {
        to8_line *nxt = cur->next;
        if (cur->op == OP_SHLi && !cur->is_target &&
            (cur->imm_val == 24 || cur->imm_val == 16) &&
            nxt && !nxt->is_target &&
            (nxt->op == OP_SARi || nxt->op == OP_SHRi) &&
            nxt->imm_val == cur->imm_val) {
            to8_opcode rep = (nxt->op == OP_SARi)
                ? (cur->imm_val == 24 ? OP_EXT1S : OP_EXT2S)
                : (cur->imm_val == 24 ? OP_EXT1U : OP_EXT2U);
            to8_line *ext = tcc_mallocz(sizeof(to8_line));
            to8_line *scan_from;
            ext->op = rep;
            ext->kind = ARG_NONE;
            {
                const char *c = bare_comment(rep);
                if (c) { snprintf(ext->comment, sizeof ext->comment, "%s", c); ext->has_comment = 1; }
            }
            to8_insert_after(cur, ext);
            to8_unlink(cur);
            to8_unlink(nxt);
            scan_from = ext->next;
            cur = scan_from;
            changed = 1;
            continue;
        }
        cur = nxt;
    }
    return changed;
}

static int to8_peephole_mov(void)
{
    int changed = 0;
    to8_line *cur = g_head;
    while (cur) {
        to8_line *nxt = cur->next;
        if (cur->op == OP_LD && !cur->is_target &&
            nxt && !nxt->is_target && nxt->op == OP_ST) {
            if (cur->slot_a == nxt->slot_a && cur->push_depth == nxt->push_depth) {
                to8_line *scan_from = nxt->next;
                to8_unlink(cur);
                to8_unlink(nxt);
                cur = scan_from;
                changed = 1;
                continue;
            }
            {
                to8_line *mov = tcc_mallocz(sizeof(to8_line));
                to8_line *scan_from;
                char desc_dst[24], desc_src[24];
                mov->op = OP_MOV;
                mov->kind = ARG_SLOT2;
                mov->slot_a = nxt->slot_a;
                mov->slot_b = cur->slot_a;
                mov->push_depth = nxt->push_depth;
                slot_desc(desc_dst, sizeof desc_dst, mov->slot_a);
                slot_desc(desc_src, sizeof desc_src, mov->slot_b);
                snprintf(mov->comment, sizeof mov->comment, "%s = %s", desc_dst, desc_src);
                mov->has_comment = 1;
                to8_insert_after(cur, mov);
                to8_unlink(cur);
                to8_unlink(nxt);
                scan_from = mov->next;
                cur = scan_from;
                changed = 1;
                continue;
            }
        }
        cur = nxt;
    }
    return changed;
}

static int to8_peephole_dead_ld(void)
{
    int changed = 0;
    to8_line *cur = g_head;
    int known_valid = 0, known_slot = 0;

    while (cur) {
        to8_line *nxt = cur->next;

        if (cur->is_target)
            known_valid = 0;

        if (cur->op == OP_LD && known_valid && !cur->is_target &&
            cur->slot_a == known_slot) {
            to8_unlink(cur);
            cur = nxt;
            changed = 1;
            continue;
        }

        switch (cur->op) {
        case OP_LD:
        case OP_ST:
            known_valid = 1;
            known_slot = cur->slot_a;
            break;
        case OP_MOV:
            break;
        default:
            known_valid = 0;
            break;
        }
        cur = nxt;
    }
    return changed;
}

static int to8_peephole_ld_deref(void)
{
    int changed = 0;
    to8_line *cur = g_head;
    while (cur) {
        to8_line *nxt = cur->next;
        if (cur->op == OP_LD &&
            nxt && !nxt->is_target &&
            (nxt->op == OP_LD1r || nxt->op == OP_LDU1r ||
             nxt->op == OP_LD2r || nxt->op == OP_LDU2r ||
             nxt->op == OP_LD4r) &&
            cur->push_depth == nxt->push_depth) {
            to8_opcode rep = (nxt->op == OP_LD1r) ? OP_LD1
                            : (nxt->op == OP_LDU1r) ? OP_LDU1
                            : (nxt->op == OP_LD2r) ? OP_LD2
                            : (nxt->op == OP_LDU2r) ? OP_LDU2
                            : OP_LD4;
            char desc[24];

            cur->op = rep;
            slot_desc(desc, sizeof desc, cur->slot_a);
            slot_comment(cur->comment, sizeof cur->comment, rep, desc);
            cur->has_comment = 1;

            to8_unlink(nxt);
            changed = 1;
            cur = cur->next;
            continue;
        }
        cur = nxt;
    }
    return changed;
}

static int to8_is_commutative_op(to8_opcode op)
{
    return op == OP_ADD || op == OP_MUL || op == OP_AND ||
           op == OP_OR || op == OP_XOR;
}

/* ===================================================================
 * v7.25.1: fuse the "ST T ; LD S ; <op> T" spill-reload dance
 * (emitted by to8_spill_and_reload(), from gen_opi()'s commutative
 * branch) into "ST T ; <op> S" - the ST is ALWAYS kept, only the LD
 * is dropped. R0 already holds the correct value right before "ST T"
 * (that's the whole reason the ST exists), so once T = R0 = X, the
 * "LD S" that follows is redundant for the sole purpose of computing
 * "<op> T": R0 can go straight from X to "X <op> S" via a single
 * "<op> S" (valid because <op> is commutative: X <op> S == S <op> X
 * == S <op> T's just-stored value). The ST is kept UNCONDITIONALLY,
 * because this backend cannot tell, from the 3-line shape alone,
 * whether T is:
 *   (a) a genuine to8_temp_alloc()'d scratch slot, dead right after
 *       (in which case the kept ST is a harmless, never-read extra
 *       instruction - one wasted store, no correctness impact), or
 *   (b) a REAL, live local/param slot that a later, unrelated C
 *       statement happens to also reference by the same slot number
 *       (e.g. "i = i + 1;" immediately followed by "... = a[i] * x;"
 *       compiled into consecutive lines by coincidence) - in which
 *       case DROPPING the ST (as the original v7.25.0 pass did)
 *       silently discards the write to that variable. This exact
 *       case broke a __qsort() partition loop in testing (2026-08-16):
 *       "i" stopped advancing because its increment's ST got deleted.
 * Keeping the ST is unconditionally correct in BOTH cases, so no
 * origin-tracking/tagging of "is this really a temp" is needed -
 * simpler AND safer than the alternative (an is_temp_spill flag)
 * considered and discarded in the same session.
 * =================================================================== */
static int to8_peephole_commute(void)
{
    int changed = 0;
    to8_line *cur = g_head;
    while (cur) {
        to8_line *nxt = cur->next;
        to8_line *third = nxt ? nxt->next : NULL;

        if (cur->op == OP_ST &&
            nxt && !nxt->is_target && nxt->op == OP_LD &&
            third && !third->is_target &&
            to8_is_commutative_op(third->op) &&
            third->kind == ARG_SLOT &&
            third->slot_a == cur->slot_a &&
            cur->push_depth == nxt->push_depth &&
            nxt->push_depth == third->push_depth) {
            char desc[24];

            /* only "LD S" (nxt) is redundant - "ST T" (cur) must
             * stay: it may be a real store to a live variable, not
             * just a dead spill temp. See comment block above. */
            third->slot_a = nxt->slot_a;
            slot_desc(desc, sizeof desc, third->slot_a);
            slot_comment(third->comment, sizeof third->comment, third->op, desc);
            third->has_comment = 1;

            to8_unlink(nxt);
            changed = 1;
            cur = third;
            continue;
        }
        cur = nxt;
    }
    return changed;
}

static void to8_peephole_run(void)
{
    int changed;
    int guard = 0;

    if (tcc_state->optimize == 0)
        return;

    do {
        changed = 0;
        changed |= to8_peephole_ext();
        changed |= to8_peephole_mov();
        changed |= to8_peephole_dead_ld();
        changed |= to8_peephole_ld_deref();
        changed |= to8_peephole_commute();
    } while (changed && ++guard < 256);
}

/* ===================================================================
 * Function prolog / epilog
 * =================================================================== */

static to8_line *to8_func_adj_line;
static char to8_func_name[256];

static int to8_final_slot(int raw_slot, int push_depth)
{
    return g_frame_size + raw_slot + push_depth;
}

static void to8_render_line(to8_line *ln)
{
    if (ln->is_target) {
        g_col = 0;
        out_char('@'); out_char('l'); out_int(ln->id); out_char(':'); out_char('\n');
    }

    /* --- raw inline-asm passthrough: bypass ALL normal rendering --- */
    if (ln->is_raw_text) {
        g_col = 0;
        out_str(ln->rawtext);
        out_char('\n');
        return;
    }

    g_col = 0;
    out_tab();
    out_str(to8_opcode_name(ln->op));
    switch (ln->kind) {
    case ARG_NONE:
        break;
    case ARG_SLOT:
        out_tab();
        out_int(to8_final_slot(ln->slot_a, ln->push_depth));
        break;
    case ARG_SLOT2:
        out_tab();
        out_int(to8_final_slot(ln->slot_a, ln->push_depth));
        out_char(',');
        out_int(to8_final_slot(ln->slot_b, ln->push_depth));
        break;
    case ARG_IMM:
        out_tab();
        out_int(ln->imm_val);
        break;
    case ARG_FIMM:
        out_tab();
        out_double(ln->f_val);
        break;
    case ARG_SYM:
        out_tab();
        if (ln->sym) {
            const char *name = get_tok_str(ln->sym->v, NULL);
            out_char('_');
            out_str(name ? name : "?");
        } else {
            out_int(ln->sym_addend);
        }
        break;
    case ARG_JMP:
        out_tab();
        out_char('@'); out_char('l');
        out_int(to8_by_id(ln->jmp_target_id)->id);
        break;
    }

    out_pad_to(TO8_COMMENT_COL);
    out_char(';'); out_char(' ');
    if (ln->kind == ARG_JMP) {
        out_str(ln->jump_prefix);
        out_str(" @l");
        out_int(to8_by_id(ln->jmp_target_id)->id);
    } else if (ln->has_comment) {
        out_str(ln->comment);
    }
    out_char('\n');
}

static void to8_render_function(void)
{
    to8_line *ln;
    ind = to8_func_start_ind;
    g_col = 0;

    out_char('_'); out_str(to8_func_name); out_char(':');
    out_pad_to(TO8_COMMENT_COL);
    out_char(';'); out_char(' ');
    out_int(g_count_after);
    out_str(" instr");
    if (g_count_after != g_count_before) {
        out_str(" (");
        out_int(g_count_before);
        out_str(" before -O)");
    }
    out_char('\n');

    for (ln = g_head; ln; ln = ln->next)
        to8_render_line(ln);
}

static void to8_free_list(void)
{
    int i;
    for (i = 0; i < g_next_id; i++)
        tcc_free(g_by_id[i]);
    tcc_free(g_by_id);
    g_by_id = NULL;
    g_by_id_cap = 0;
}

void gfunc_prolog(Sym *func_sym)
{
    CType *func_type = &func_sym->type;
    Sym *sym;
    int addr, size, align;

    loc = 0;
    to8_list_reset();
    to8_temp_pool_reset();
    r1_shadow_valid = 0;
    cur_push_depth = 0;
    g_in_function = 1;

    if (!g_banner_done) {
        to8_print_banner();
        g_banner_done = 1;
    }

    if (g_last_end_ind >= 0 && ind > g_last_end_ind) {
        int save = ind;
        ind = g_last_end_ind;
        g_col = 0;
        while (ind < save)
            out_char('.');
        ind = save;
    }

    to8_func_start_ind = ind;

    snprintf(to8_func_name, sizeof to8_func_name, "%s", funcname ? funcname : "?");

    to8_func_adj_line = to8_append(OP_ADJi);
    to8_func_adj_line->kind = ARG_IMM;

    addr = PTR_SIZE;

    if (func_vt.t & VT_STRUCT) {
        func_vc = addr;
        addr += PTR_SIZE;
    }

    sym = func_type->ref;
    while ((sym = sym->next) != NULL) {
        size = type_size(&sym->type, &align);
        size = (size + TO8_STACK_ALIGN - 1) & ~(TO8_STACK_ALIGN - 1);
        addr += size;
        sym_push(sym->v & ~SYM_FIELD, &sym->type, VT_LOCAL | VT_LVAL, addr);
    }

#ifdef CONFIG_TCC_BCHECK
    if (tcc_state->do_bounds_check)
        gen_bounds_prolog();
#endif
}

void gfunc_epilog(void)
{
    int frame_size;

#ifdef CONFIG_TCC_BCHECK
    if (tcc_state->do_bounds_check)
        gen_bounds_epilog();
#endif

    frame_size = -loc;
    g_frame_size = frame_size;

    if (frame_size == 0) {
        to8_unlink(to8_func_adj_line);
        if (g_head)
            to8_func_adj_line->redirect = g_head;
    } else {
        to8_func_adj_line->imm_val = -frame_size;
        imm_comment(to8_func_adj_line->comment, sizeof to8_func_adj_line->comment, OP_ADJi, -frame_size);
        to8_func_adj_line->has_comment = 1;
    }

    to8_adjust(frame_size);

    e_op(OP_RET);

    g_count_before = to8_count_lines();
    to8_peephole_run();
    g_count_after = to8_count_lines();

    to8_render_function();
    g_last_end_ind = ind;
    to8_free_list();
    g_in_function = 0;
}

/* ===================================================================
 * Jump generation
 * =================================================================== */

static int to8_emit_jmp(to8_opcode op)
{
    to8_line *ln = to8_append(op);
    ln->kind = ARG_JMP;
    ln->jump_prefix = to8_jump_prefix(op);
    ln->jmp_chain = NULL;
    ln->jmp_target_id = 0;
    return ln->id;
}

ST_FUNC int gjmp(int t)
{
    int off = to8_emit_jmp(OP_JMP);
    return gjmp_append(off, t);
}

ST_FUNC void gjmp_addr(int a)
{
    to8_line *ln;
    int target = a - to8_func_start_ind;
    ln = to8_append(OP_JMP);
    ln->kind = ARG_JMP;
    ln->jump_prefix = "goto";
    ln->jmp_target_id = target;
    to8_mark_target(target);
}

ST_FUNC int gjmp_cond(int op, int t)
{
    to8_opcode jcond = to8_get_jcond(op);
    int off = to8_emit_jmp(jcond);
    return gjmp_append(off, t);
}

/* ===================================================================
 * VLA support - REMOVED (C11 optional feature, v7.17.0)
 * =================================================================== */

ST_FUNC void gen_vla_alloc(CType *type, int align)
{
    (void)type;
    (void)align;
    tcc_error("TO8: variable-length arrays (VLA) are not supported by this backend "
              "(C11 optional feature; define __STDC_NO_VLA__ to indicate this).");
}

ST_FUNC void gen_vla_sp_save(int addr) { e_op_slot(OP_ST, addr); }
ST_FUNC void gen_vla_sp_restore(int addr) { e_op_slot(OP_LD, addr); }

/* ===================================================================
 * v7.18.0/7.20.0: dump rodata/data section contents as readable FCB
 * byte directives into the SAME pseudo-asm text stream that already
 * carries the code.
 * =================================================================== */

ST_FUNC void to8_flush_pending_data(TCCState *s1)
{
    int save_nocode_wanted = nocode_wanted;
    Section *symtab = symtab_section; /* bare - macro already resolves to tcc_state->symtab_section */
    int nb_syms, i;

    nocode_wanted = 0;
    cur_text_section = text_section; /* bare, same reasoning */
    ind = cur_text_section->data_offset;

    if (!symtab || !symtab->data) {
        nocode_wanted = save_nocode_wanted;
        return;
    }

    nb_syms = symtab->data_offset / sizeof(ElfSym);

    for (i = 1; i < s1->nb_sections; i++) {
        Section *sec = s1->sections[i];
        int j;

        if (!sec || !sec->data || sec->data_offset == 0)
            continue;
        if (sec->sh_type == SHT_NOBITS) /* .bss: no bytes to show */
            continue;
        if (!(sec->sh_flags & SHF_ALLOC)) /* debug/symtab/etc: not runtime data */
            continue;
        if (sec == cur_text_section) /* code: already rendered as pseudo-asm */
            continue;

        for (j = 1; j < nb_syms; j++) {
            ElfSym *sym = (ElfSym *)symtab->data + j;
            const char *name;
            unsigned long off, n, k;

            if (sym->st_shndx != sec->sh_num)
                continue;
            if (!sym->st_name)
                continue;

            name = (const char *)symtab->link->data + sym->st_name;
            off = sym->st_value;
            n = sym->st_size ? sym->st_size : 1;

            if (!g_banner_done) { to8_print_banner(); g_banner_done = 1; }

            g_col = 0;
            out_char('_'); out_str(name); out_char(':');
            out_pad_to(TO8_COMMENT_COL);
            out_char(';'); out_char(' ');
            out_int((int)n); out_str(" bytes (data)");

            for (k = 0; k < n; k++) {
                if ((k & 7) == 0) {
                    out_char('\n');
                    g_col = 0;
                    out_tab();
                    out_str("FCB");
                    out_tab();
                } else {
                    out_char(',');
                }
                out_int(sec->data[off + k]);
            }
            out_char('\n');
            g_col = 0;
        }
    }

    if (!g_banner_done) { to8_print_banner(); g_banner_done = 1; }
    g_col = 0;
    out_str("; --- end of asm ---");
    out_char('\n');

    cur_text_section->data_offset = ind;
    nocode_wanted = save_nocode_wanted;
}

/* ===================================================================
 * Misc backend helpers required by tccgen/tccelf
 * =================================================================== */

ST_FUNC int gjmp_append(int n, int t)
{
    if (n) {
        to8_line *ln = to8_by_id(n);
        while (ln->jmp_chain) ln = ln->jmp_chain;
        ln->jmp_chain = t ? to8_by_id(t) : NULL;
        return n;
    }
    return t;
}

ST_FUNC void gen_fill_nops(int bytes)
{
    while (bytes-- > 0)
        e_op(OP_NOP);
}

ST_FUNC void ggoto(void)
{
    gfunc_call(1);
}

#endif /* TARGET_DEFS_ONLY */
