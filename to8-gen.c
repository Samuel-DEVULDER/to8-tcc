/*
 * TO8 backend for TCC - single-register pseudo-ASM generator.
 *
 * Version: 4.13.0
 * Changelog:
 * - v4.13.0: NEW peephole: "LD a; ST b" immediately followed by an
 *   instruction that unconditionally overwrites R0 WITHOUT reading
 *   its current value (another LD/LDi/LEA/LEAm/LD1/LD2/LD4, or the
 *   return from a call JSRi/JSRs/JSR, since the callee's return value
 *   always lands in R0) is fused into a single "MOV b, a" (b := a).
 *
 *   This is a DEFERRED, look-ahead-free design, same spirit as the
 *   EXT1S/EXT2S fusion: "LD a" and "ST b" are emitted normally and
 *   immediately (correct on their own, unconditionally). Only once
 *   the VERY NEXT instruction is seen do we decide: if it provably
 *   clobbers R0 before reading it, R0's value from "LD a" was never
 *   used for anything beyond the "ST b" that already consumed it -
 *   so the LD+ST pair is retroactively commented out (one byte each,
 *   same trick used throughout) and a fresh "MOV b, a" is emitted in
 *   their place. If the next instruction reads R0 instead (another
 *   ST, an arithmetic op, a comparison, a conditional jump, an
 *   unconditional jump/label - anything NOT in the whitelist), the
 *   candidate is silently dropped and both original lines are left
 *   exactly as they were: still correct, just not compressed.
 *
 *   The classification is a narrow WHITELIST (not a blacklist) on
 *   purpose: anything not explicitly known to overwrite R0 without
 *   reading it is treated as "unsafe to fuse", so the peephole can
 *   never silently break a program - a missed fusion just leaves
 *   the two original, already-correct lines in place.
 *
 *   Every emission entry point (emit_op, emit_op_imm, emit_op_slot,
 *   emit_op_addr, emit_label, to8_push_imm, to8_push_slot,
 *   to8_emit_jmp, gjmp_addr) now resolves any pending MOV candidate
 *   FIRST, before writing its own text, so a confirmed MOV line lands
 *   exactly where the commented-out LD+ST used to be.
 *
 * - v4.12.0: renamed SEX1/SEX2/UEX1/UEX2 to EXT1S/EXT2S/EXT1U/EXT2U.
 * - v4.11.1/v4.11.0: shifts by a constant emit a single SHLi/SARi/
 *   SHRi with no memory traffic; EXT fusion comments out that one
 *   line. See prior versions for the full history.
 *
 * Architecture:
 * R0 = real integer accumulator (also REG_IRET).
 * R1 = VIRTUAL integer register, memory-backed shadow slot.
 * F0 = float accumulator (also REG_FRET)
 * ADJ <signed n> is the sole stack-pointer adjustment.
 * Call targets: JSRi <sym>, JSRs <slot>, JSR (via R0).
 * SHL/SAR/SHR by a compile-time-constant amount emit a single
 * SHLi/SARi/SHRi <n>. EXT1S/EXT2S/EXT1U/EXT2U fuse the SHLi+SARi/SHRi
 * pair for narrow-to-int extension. "LD a; ST b" fuses into
 * "MOV b, a" whenever the following instruction provably clobbers R0
 * without reading it.
 */

#ifdef TARGET_DEFS_ONLY

#define NB_REGS 3
#define NB_ASM_REGS 0
#define CONFIG_TCC_ASM

#define RC_R0    0x0001
#define RC_R1    0x0002
#define RC_FLOAT 0x0004
#define RC_INT   (RC_R0 | RC_R1)
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

#define PTR_SIZE 4
#define LDOUBLE_SIZE 8
#define LDOUBLE_ALIGN 4
#define MAX_ALIGN 4
#define TO8_STACK_ALIGN 4

#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER BIG_ENDIAN
#endif

#define PROMOTE_RET

#else

/* must be defined before gfunc_prolog/epilog call them */
ST_FUNC void gen_bounds_prolog(void) {}
ST_FUNC void gen_bounds_epilog(void) {}

#define USING_GLOBALS
#include "tcc.h"
#include <assert.h>
#include <stdio.h>
#include <ctype.h>

ST_DATA const char * const target_machine_defs =
    "__TO8__\0"
    "__BIG_ENDIAN__\0";

/* index order matches TREG_R0=0, TREG_F0=1, TREG_R1=2 */
ST_DATA const int reg_classes[NB_REGS] = {
    RC_R0,
    RC_FLOAT,
    RC_R1,
};

/* ===================================================================
 * Private, strictly-monotonic temp-slot allocator.
 * =================================================================== */

static int to8_new_temp(int size, int align)
{
    loc = (loc - size) & -align;
    return loc;
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
 * Fixed-width ASCII decimal helpers for patchable operand fields.
 * =================================================================== */

#define TO8_FIELD_WIDTH 8
#define TO8_LINE_WIDTH 10  /* "l" + up to 7 digits + ":" + 1 pad, or blank */

static void write_int_fixed(unsigned char *p, int v, int width)
{
    char tmp[16];
    int len, i;
    len = snprintf(tmp, sizeof tmp, "%d", v);
    if (len > width) len = width;
    for (i = 0; i < len; i++) p[i] = tmp[i];
    for (i = len; i < width; i++) p[i] = ' ';
}

static int read_int_fixed(const unsigned char *p, int width)
{
    int i = 0, v = 0, neg = 0;
    if (p[0] == '-') { neg = 1; i = 1; }
    for (; i < width; i++) {
        if (p[i] < '0' || p[i] > '9') break;
        v = v * 10 + (p[i] - '0');
    }
    return neg ? -v : v;
}

static int emit_placeholder_field(int width)
{
    int offset = ind, i;
    g('0');
    for (i = 1; i < width; i++) g(' ');
    return offset;
}

/* Label DEFINITION form: "l<v>:" - used only where a line's own
 * leading label field is being revealed (this line IS the target). */
static void write_label_def(unsigned char *p, int v, int width)
{
    char tmp[16];
    int len, i;
    len = snprintf(tmp, sizeof tmp, "l%d:", v);
    if (len > width) len = width;
    for (i = 0; i < len; i++) p[i] = tmp[i];
    for (i = len; i < width; i++) p[i] = ' ';
}

/* Label REFERENCE form: "l<v>" (no colon) - used for every jump
 * instruction's own target operand and its "goto" comment copy. */
static void write_label_ref(unsigned char *p, int v, int width)
{
    char tmp[16];
    int len, i;
    len = snprintf(tmp, sizeof tmp, "l%d", v);
    if (len > width) len = width;
    for (i = 0; i < len; i++) p[i] = tmp[i];
    for (i = len; i < width; i++) p[i] = ' ';
}

static int pending_line_mark = -1;

static void mark_or_defer_target(int a)
{
    if (a < 0) return;
    if (a < ind)
        write_label_def(cur_text_section->data + a, a, TO8_LINE_WIDTH);
    else
        pending_line_mark = a;
}

/* ===================================================================
 * Per-function patch lists.
 * =================================================================== */

typedef struct { int text_offset; int raw_slot; int push_depth; } to8_slot_patch;
static to8_slot_patch *slot_patches;
static int slot_patches_n, slot_patches_cap;

typedef struct { int op_off; int cmt_off; } to8_jmp_cmt_patch;
static to8_jmp_cmt_patch *jmp_cmt_patches;
static int jmp_cmt_patches_n, jmp_cmt_patches_cap;

static int pend_store_valid;
static int pend_store_slot;

/* EXT fusion candidate: set right after emitting "SHLi <amount>" for
 * amount 24 or 16. */
static int pending_sex_amount;
static int pending_sex_line_start;

/* MOV fusion candidates. last_ld_* tracks whether the IMMEDIATELY
 * preceding emission was a bare "LD <slot>" (any other instruction
 * clears it). pending_mov_* tracks a "LD a; ST b" pair awaiting a
 * verdict from whatever instruction comes next. */
static int last_ld_valid;
static int last_ld_slot;
static int last_ld_start;

static int pending_mov_valid;
static int pending_mov_ld_start;
static int pending_mov_st_start;
static int pending_mov_src;
static int pending_mov_dst;

static void slot_patch_reset(void)
{
    slot_patches_n = 0;
    jmp_cmt_patches_n = 0;
    pend_store_valid = 0;
    pending_line_mark = -1;
    r1_shadow_valid = 0;
    cur_push_depth = 0;
    pending_sex_amount = 0;
    last_ld_valid = 0;
    pending_mov_valid = 0;
}

static void slot_patch_add(int text_offset, int raw_slot)
{
    if (slot_patches_n >= slot_patches_cap) {
        int newcap = slot_patches_cap ? slot_patches_cap * 2 : 64;
        slot_patches = tcc_realloc(slot_patches, newcap * sizeof(*slot_patches));
        slot_patches_cap = newcap;
    }
    slot_patches[slot_patches_n].text_offset = text_offset;
    slot_patches[slot_patches_n].raw_slot = raw_slot;
    slot_patches[slot_patches_n].push_depth = cur_push_depth;
    slot_patches_n++;
}

static void jmp_cmt_patch_add(int op_off, int cmt_off)
{
    if (jmp_cmt_patches_n >= jmp_cmt_patches_cap) {
        int newcap = jmp_cmt_patches_cap ? jmp_cmt_patches_cap * 2 : 32;
        jmp_cmt_patches = tcc_realloc(jmp_cmt_patches, newcap * sizeof(*jmp_cmt_patches));
        jmp_cmt_patches_cap = newcap;
    }
    jmp_cmt_patches[jmp_cmt_patches_n].op_off = op_off;
    jmp_cmt_patches[jmp_cmt_patches_n].cmt_off = cmt_off;
    jmp_cmt_patches_n++;
}

/* ===================================================================
 * Emit helpers
 * =================================================================== */

static void emit_str(const char *s) { while (*s) g((unsigned char)*s++); }

static void emit_uint(unsigned int v)
{
    char buf[12]; int n = 0, i;
    if (v == 0) { g('0'); return; }
    while (v && n < 12) { buf[n++] = '0' + (v % 10); v /= 10; }
    for (i = n - 1; i >= 0; i--) g(buf[i]);
}

static void emit_int(int v)
{
    if (v < 0) { g('-'); v = -v; }
    emit_uint((unsigned)v);
}

static void begin_line(void)
{
    int i;
    if (pending_line_mark == ind) {
        char tmp[16]; int len, j;
        len = snprintf(tmp, sizeof tmp, "l%d:", ind);
        if (len > TO8_LINE_WIDTH) len = TO8_LINE_WIDTH;
        for (j = 0; j < len; j++) g(tmp[j]);
        for (j = len; j < TO8_LINE_WIDTH; j++) g(' ');
        pending_line_mark = -1;
    } else {
        for (i = 0; i < TO8_LINE_WIDTH; i++) g(' ');
    }
}

static void emit_nl(void) { g('\n'); }

static void emit_comment(const char *text)
{
    emit_str("  ; ");
    emit_str(text);
}

/* ===================================================================
 * C-style comment templates
 * =================================================================== */

static const char *to8_type_name(char digit)
{
    if (digit == '1') return "char";
    if (digit == '2') return "short";
    return "int";
}

static void slot_comment(char *out, size_t outsz, const char *op, const char *desc)
{
    int n = (int)strlen(op);
    if (!strcmp(op, "LD")) { snprintf(out, outsz, "R0 = %s", desc); return; }
    if (!strcmp(op, "ST")) { snprintf(out, outsz, "%s = R0", desc); return; }
    if (!strcmp(op, "LEA")) { snprintf(out, outsz, "R0 = &%s", desc); return; }
    if (!strcmp(op, "ADD")) { snprintf(out, outsz, "R0 += %s", desc); return; }
    if (!strcmp(op, "SUB")) { snprintf(out, outsz, "R0 -= %s", desc); return; }
    if (!strcmp(op, "AND")) { snprintf(out, outsz, "R0 &= %s", desc); return; }
    if (!strcmp(op, "OR"))  { snprintf(out, outsz, "R0 |= %s", desc); return; }
    if (!strcmp(op, "XOR")) { snprintf(out, outsz, "R0 ^= %s", desc); return; }
    if (!strcmp(op, "MUL")) { snprintf(out, outsz, "R0 *= %s", desc); return; }
    if (!strcmp(op, "DIV")) { snprintf(out, outsz, "R0 /= %s", desc); return; }
    if (!strcmp(op, "MOD")) { snprintf(out, outsz, "R0 %%= %s", desc); return; }
    if (!strcmp(op, "SHL")) { snprintf(out, outsz, "R0 <<= %s", desc); return; }
    if (!strcmp(op, "SHR")) { snprintf(out, outsz, "R0 >>= %s", desc); return; }
    if (!strcmp(op, "SAR")) { snprintf(out, outsz, "R0 >>= %s (arith)", desc); return; }
    if (!strcmp(op, "CMP")) { snprintf(out, outsz, "compare R0, %s", desc); return; }
    if (!strcmp(op, "FCMP")) { snprintf(out, outsz, "compare F0, %s", desc); return; }
    if (!strcmp(op, "STF32") || !strcmp(op, "STF64")) { snprintf(out, outsz, "%s = F0", desc); return; }
    if (!strcmp(op, "LDF32") || !strcmp(op, "LDF64")) { snprintf(out, outsz, "F0 = %s", desc); return; }
    if (!strcmp(op, "STF32ms") || !strcmp(op, "STF64ms")) { snprintf(out, outsz, "*%s = F0", desc); return; }
    if (!strcmp(op, "LDF32ms") || !strcmp(op, "LDF64ms")) { snprintf(out, outsz, "F0 = *%s", desc); return; }
    if (!strcmp(op, "PSH")) { snprintf(out, outsz, "push %s", desc); return; }
    if (!strcmp(op, "JSRs")) { snprintf(out, outsz, "call *%s", desc); return; }
    if (n >= 3 && op[0] == 'L' && op[1] == 'D' && isdigit((unsigned char)op[2])) {
        snprintf(out, outsz, "R0 = *(%s*)%s", to8_type_name(op[2]), desc); return;
    }
    if (n >= 3 && op[0] == 'S' && op[1] == 'T' && isdigit((unsigned char)op[2])) {
        snprintf(out, outsz, "*(%s*)%s = R0", to8_type_name(op[2]), desc); return;
    }
    snprintf(out, outsz, "%s", desc);
}

static void imm_comment(char *out, size_t outsz, const char *op, int v)
{
    char num[16];
    snprintf(num, sizeof num, "%d", v);
    if (!strcmp(op, "LDi")) { snprintf(out, outsz, "R0 = %s", num); return; }
    if (!strcmp(op, "ADDi")) { snprintf(out, outsz, "R0 += %s", num); return; }
    if (!strcmp(op, "SUBi")) { snprintf(out, outsz, "R0 -= %s", num); return; }
    if (!strcmp(op, "ANDi")) { snprintf(out, outsz, "R0 &= %s", num); return; }
    if (!strcmp(op, "ORi"))  { snprintf(out, outsz, "R0 |= %s", num); return; }
    if (!strcmp(op, "XORi")) { snprintf(out, outsz, "R0 ^= %s", num); return; }
    if (!strcmp(op, "MULi")) { snprintf(out, outsz, "R0 *= %s", num); return; }
    if (!strcmp(op, "DIVi")) { snprintf(out, outsz, "R0 /= %s", num); return; }
    if (!strcmp(op, "MODi")) { snprintf(out, outsz, "R0 %%= %s", num); return; }
    if (!strcmp(op, "SHLi")) { snprintf(out, outsz, "R0 <<= %s", num); return; }
    if (!strcmp(op, "SHRi")) { snprintf(out, outsz, "R0 >>= %s", num); return; }
    if (!strcmp(op, "SARi")) { snprintf(out, outsz, "R0 >>= %s (arith)", num); return; }
    if (!strcmp(op, "CMPi")) { snprintf(out, outsz, "compare R0, %s", num); return; }
    if (!strcmp(op, "ADJ")) { snprintf(out, outsz, v < 0 ? "sp -= %s" : "sp += %s", v < 0 ? num + 1 : num); return; }
    if (!strcmp(op, "PUSHi")) { snprintf(out, outsz, "push %s", num); return; }
    snprintf(out, outsz, "%s", num);
}

static void addr_comment(char *out, size_t outsz, const char *op, const char *desc)
{
    int n = (int)strlen(op);
    if (!strcmp(op, "LEAm")) { snprintf(out, outsz, "R0 = &%s", desc); return; }
    if (!strcmp(op, "JSRi")) { snprintf(out, outsz, "call %s", desc); return; }
    if (!strcmp(op, "LDF32m") || !strcmp(op, "LDF64m")) { snprintf(out, outsz, "F0 = %s", desc); return; }
    if (!strcmp(op, "STF32m") || !strcmp(op, "STF64m")) { snprintf(out, outsz, "%s = F0", desc); return; }
    if (n >= 2 && op[0] == 'L' && op[1] == 'D' && isdigit((unsigned char)op[2])) {
        snprintf(out, outsz, "R0 = *(%s*)%s", to8_type_name(op[2]), desc); return;
    }
    if (n >= 2 && op[0] == 'S' && op[1] == 'T' && isdigit((unsigned char)op[2])) {
        snprintf(out, outsz, "*(%s*)%s = R0", to8_type_name(op[2]), desc); return;
    }
    snprintf(out, outsz, "%s", desc);
}

static const char *bare_comment(const char *op)
{
    if (!strcmp(op, "RET")) return "return";
    if (!strcmp(op, "PUSH")) return "push R0";
    if (!strcmp(op, "JSR")) return "call *R0";
    if (!strcmp(op, "ITOF")) return "R0 -> F0 (int to float)";
    if (!strcmp(op, "FTOI")) return "F0 -> R0 (float to int)";
    if (!strcmp(op, "TST")) return "compare R0, 0";
    if (!strcmp(op, "SEQ")) return "R0 = (== ? 1 : 0)";
    if (!strcmp(op, "SNE")) return "R0 = (!= ? 1 : 0)";
    if (!strcmp(op, "SLT")) return "R0 = (< ? 1 : 0)";
    if (!strcmp(op, "SGT")) return "R0 = (> ? 1 : 0)";
    if (!strcmp(op, "SLE")) return "R0 = (<= ? 1 : 0)";
    if (!strcmp(op, "SGE")) return "R0 = (>= ? 1 : 0)";
    if (!strcmp(op, "SLTU")) return "R0 = (< unsigned ? 1 : 0)";
    if (!strcmp(op, "SGTU")) return "R0 = (> unsigned ? 1 : 0)";
    if (!strcmp(op, "SLEU")) return "R0 = (<= unsigned ? 1 : 0)";
    if (!strcmp(op, "SGEU")) return "R0 = (>= unsigned ? 1 : 0)";
    if (!strcmp(op, "SNZ")) return "R0 = (R0 != 0 ? 1 : 0)";
    if (!strcmp(op, "FADD")) return "F0 += (top)";
    if (!strcmp(op, "FSUB")) return "F0 -= (top)";
    if (!strcmp(op, "FMUL")) return "F0 *= (top)";
    if (!strcmp(op, "FDIV")) return "F0 /= (top)";
    if (!strcmp(op, "FSEQ")) return "R0 = (F0 == ? 1 : 0)";
    if (!strcmp(op, "FSNE")) return "R0 = (F0 != ? 1 : 0)";
    if (!strcmp(op, "FSLT")) return "R0 = (F0 < ? 1 : 0)";
    if (!strcmp(op, "FSGT")) return "R0 = (F0 > ? 1 : 0)";
    if (!strcmp(op, "FSLE")) return "R0 = (F0 <= ? 1 : 0)";
    if (!strcmp(op, "FSGE")) return "R0 = (F0 >= ? 1 : 0)";
    if (!strcmp(op, "EXT1S")) return "R0 = (int)(char)R0";
    if (!strcmp(op, "EXT2S")) return "R0 = (int)(short)R0";
    if (!strcmp(op, "EXT1U")) return "R0 = (int)(unsigned char)R0";
    if (!strcmp(op, "EXT2U")) return "R0 = (int)(unsigned short)R0";
    if (!strcmp(op, "VLA_ALLOC")) return "alloc VLA";
    if (!strcmp(op, "NOP")) return "no-op";
    return NULL;
}

static const char *jump_comment(const char *op)
{
    if (!strcmp(op, "JMP")) return "goto";
    if (!strcmp(op, "JEQ")) return "if (==) goto";
    if (!strcmp(op, "JNE")) return "if (!=) goto";
    if (!strcmp(op, "JLT")) return "if (<) goto";
    if (!strcmp(op, "JGT")) return "if (>) goto";
    if (!strcmp(op, "JLE")) return "if (<=) goto";
    if (!strcmp(op, "JGE")) return "if (>=) goto";
    if (!strcmp(op, "JLTU")) return "if (< unsigned) goto";
    if (!strcmp(op, "JGTU")) return "if (> unsigned) goto";
    if (!strcmp(op, "JLEU")) return "if (<= unsigned) goto";
    if (!strcmp(op, "JGEU")) return "if (>= unsigned) goto";
    return "goto";
}

static void slot_desc(char *out, size_t outsz, int slot)
{
    if (slot < 0) snprintf(out, outsz, "local[%d]", -slot);
    else snprintf(out, outsz, "param[%d]", slot);
}

/* ===================================================================
 * MOV fusion: whitelist of mnemonics known to overwrite R0
 * unconditionally WITHOUT reading its current value. Deliberately
 * narrow - anything not listed is treated as unsafe, so a missed
 * fusion just leaves two already-correct lines in place. */
static int to8_clobbers_r0(const char *op)
{
    static const char *table[] = {
        "LD", "LDi", "LEA", "LEAm", "LD1", "LD2", "LD4",
        "JSRi", "JSRs", "JSR",
        NULL
    };
    int i;
    for (i = 0; table[i]; i++)
        if (!strcmp(op, table[i])) return 1;
    return 0;
}

static void emit_mov(int dst_slot, int src_slot)
{
    int patch_dst, patch_src;
    char desc_dst[24], desc_src[24], cmt[56];

    begin_line();
    emit_str("    MOV ");
    patch_dst = emit_placeholder_field(TO8_FIELD_WIDTH);
    slot_patch_add(patch_dst, dst_slot);
    emit_str(", ");
    patch_src = emit_placeholder_field(TO8_FIELD_WIDTH);
    slot_patch_add(patch_src, src_slot);

    slot_desc(desc_dst, sizeof desc_dst, dst_slot);
    slot_desc(desc_src, sizeof desc_src, src_slot);
    snprintf(cmt, sizeof cmt, "%s = %s", desc_dst, desc_src);
    emit_comment(cmt);
    emit_nl();
    pend_store_valid = 0;
}

/* Called at the very top of every emission entry point, before any
 * text for the upcoming instruction (mnemonic op_about_to_be_emitted)
 * is written. Resolves any outstanding "LD a; ST b" MOV candidate:
 * confirms (comments out both lines, emits "MOV b, a") if op_about_
 * to_be_emitted provably clobbers R0 first, drops it otherwise. */
static void to8_resolve_pending_mov(const char *op_about_to_be_emitted)
{
    if (!pending_mov_valid) return;
    pending_mov_valid = 0;
    if (!to8_clobbers_r0(op_about_to_be_emitted)) return;

    cur_text_section->data[pending_mov_ld_start] = ';';
    cur_text_section->data[pending_mov_st_start] = ';';
    emit_mov(pending_mov_dst, pending_mov_src);
}

/* ===================================================================
 * Emit primitives
 * =================================================================== */

static void emit_op(const char *op)
{
    const char *c;
    to8_resolve_pending_mov(op);
    last_ld_valid = 0;
    c = bare_comment(op);
    begin_line();
    emit_str("    ");
    emit_str(op);
    if (c) emit_comment(c);
    emit_nl();
    pend_store_valid = 0;
}

static int emit_op_imm(const char *op, int v)
{
    int line_start;
    char cmt[48];
    to8_resolve_pending_mov(op);
    last_ld_valid = 0;
    line_start = ind;
    begin_line();
    emit_str("    ");
    emit_str(op);
    g(' ');
    emit_int(v);
    imm_comment(cmt, sizeof cmt, op, v);
    emit_comment(cmt);
    emit_nl();
    pend_store_valid = 0;
    return line_start;
}

static void emit_op_slot(const char *op, int slot)
{
    int patch_off;
    char desc[24], cmt[48];
    int line_start;

    to8_resolve_pending_mov(op);

    if (strcmp(op, "LD") == 0 && pend_store_valid && slot == pend_store_slot) {
        last_ld_valid = 0;
        return;
    }

    line_start = ind;
    begin_line();
    emit_str("    ");
    emit_str(op);
    g(' ');
    patch_off = emit_placeholder_field(TO8_FIELD_WIDTH);
    slot_patch_add(patch_off, slot);

    slot_desc(desc, sizeof desc, slot);
    slot_comment(cmt, sizeof cmt, op, desc);
    emit_comment(cmt);
    emit_nl();

    if (strcmp(op, "ST") == 0) {
        pend_store_valid = 1;
        pend_store_slot = slot;
        if (last_ld_valid) {
            pending_mov_valid = 1;
            pending_mov_ld_start = last_ld_start;
            pending_mov_st_start = line_start;
            pending_mov_src = last_ld_slot;
            pending_mov_dst = slot;
        }
        last_ld_valid = 0;
    } else {
        pend_store_valid = 0;
        if (strcmp(op, "LD") == 0) {
            last_ld_valid = 1;
            last_ld_slot = slot;
            last_ld_start = line_start;
        } else {
            last_ld_valid = 0;
        }
    }
}

static void emit_op_addr(const char *op, Sym *sym, int c)
{
    char desc[32], cmt[48];

    to8_resolve_pending_mov(op);
    last_ld_valid = 0;

    begin_line();
    emit_str("    ");
    emit_str(op);
    g(' ');
    if (sym) {
        const char *name = get_tok_str(sym->v, NULL);
        snprintf(desc, sizeof desc, "%s", name ? name : "?");
        greloca(cur_text_section, sym, ind, R_X86_64_PC32, c);
        cur_text_section->data[ind]     = '0';
        cur_text_section->data[ind + 1] = '0';
        cur_text_section->data[ind + 2] = '0';
        cur_text_section->data[ind + 3] = '0';
        ind += 4;
    } else {
        snprintf(desc, sizeof desc, "%d", c);
        emit_int(c);
    }
    addr_comment(cmt, sizeof cmt, op, desc);
    emit_comment(cmt);
    emit_nl();
    pend_store_valid = 0;
}

static void emit_label(const char *name)
{
    to8_resolve_pending_mov("LABEL");
    last_ld_valid = 0;
    begin_line();
    emit_str(name);
    g(':');
    emit_nl();
    pend_store_valid = 0;
}

static void to8_track_push(void)
{
    cur_push_depth += PTR_SIZE;
}

static void to8_adjust(int n)
{
    emit_op_imm("ADJ", n);
    cur_push_depth -= n;
}

static void to8_push_imm(int v)
{
    to8_resolve_pending_mov("PUSHi");
    last_ld_valid = 0;
    begin_line();
    emit_str("    PUSHi ");
    emit_int(v);
    {
        char cmt[32];
        imm_comment(cmt, sizeof cmt, "PUSHi", v);
        emit_comment(cmt);
    }
    emit_nl();
    pend_store_valid = 0;
    to8_track_push();
}

static void to8_push_slot(int slot)
{
    int patch_off;
    char desc[24], cmt[40];

    to8_resolve_pending_mov("PSH");
    last_ld_valid = 0;

    begin_line();
    emit_str("    PSH ");
    patch_off = emit_placeholder_field(TO8_FIELD_WIDTH);
    slot_patch_add(patch_off, slot);
    slot_desc(desc, sizeof desc, slot);
    slot_comment(cmt, sizeof cmt, "PSH", desc);
    emit_comment(cmt);
    emit_nl();
    pend_store_valid = 0;
    to8_track_push();
}

static void to8_push_r0(void)
{
    emit_op("PUSH");
    to8_track_push();
}

/* ===================================================================
 * Jump backpatching
 * =================================================================== */

ST_FUNC void gsym_addr(int t, int a)
{
    int target = (a < 0) ? -a : a;
    while (t) {
        unsigned char *ptr = cur_text_section->data + t;
        int n = read_int_fixed(ptr, TO8_FIELD_WIDTH);
        write_label_ref(ptr, target, TO8_FIELD_WIDTH);
        {
            int i;
            for (i = 0; i < jmp_cmt_patches_n; i++) {
                if (jmp_cmt_patches[i].op_off == t) {
                    write_label_ref(cur_text_section->data + jmp_cmt_patches[i].cmt_off,
                                     target, TO8_FIELD_WIDTH);
                    break;
                }
            }
        }
        t = n;
    }
    mark_or_defer_target(target);
}

/* ===================================================================
 * Helpers for operations
 * =================================================================== */

static int to8_is_commutative(int op)
{
    return op == '+' || op == '*' || op == '&' || op == '|' || op == '^';
}

static int to8_get_arith_ops(int op, const char **slot_op, const char **imm_op)
{
    switch (op) {
    case '+': *slot_op = "ADD"; *imm_op = "ADDi"; return 0;
    case '-': *slot_op = "SUB"; *imm_op = "SUBi"; return 0;
    case '&': *slot_op = "AND"; *imm_op = "ANDi"; return 0;
    case '|': *slot_op = "OR";  *imm_op = "ORi";  return 0;
    case '^': *slot_op = "XOR"; *imm_op = "XORi"; return 0;
    case '*': *slot_op = "MUL"; *imm_op = "MULi"; return 0;
    case '/': *slot_op = "DIV"; *imm_op = "DIVi"; return 0;
    case '%': *slot_op = "MOD"; *imm_op = "MODi"; return 0;
    case TOK_SHL: *slot_op = "SHL"; *imm_op = "SHLi"; return 0;
    case TOK_SHR: *slot_op = "SHR"; *imm_op = "SHRi"; return 0;
    case TOK_SAR: *slot_op = "SAR"; *imm_op = "SARi"; return 0;
    case TOK_UDIV: *slot_op = "DIV"; *imm_op = "DIVi"; return 0;
    case TOK_UMOD: *slot_op = "MOD"; *imm_op = "MODi"; return 0;
    default: *slot_op = NULL; *imm_op = NULL; return -1;
    }
}

static const char *to8_get_set_cond(int op)
{
    switch (op) {
    case TOK_EQ: return "SEQ";
    case TOK_NE: return "SNE";
    case TOK_LT: return "SLT";
    case TOK_GT: return "SGT";
    case TOK_LE: return "SLE";
    case TOK_GE: return "SGE";
    case TOK_ULT: return "SLTU";
    case TOK_UGT: return "SGTU";
    case TOK_ULE: return "SLEU";
    case TOK_UGE: return "SGEU";
    default: return NULL;
    }
}

static const char *to8_get_jcond(int op)
{
    switch (op) {
    case TOK_EQ: return "JEQ";
    case TOK_NE: return "JNE";
    case TOK_LT: return "JLT";
    case TOK_GT: return "JGT";
    case TOK_LE: return "JLE";
    case TOK_GE: return "JGE";
    case TOK_ULT: return "JLTU";
    case TOK_UGT: return "JGTU";
    case TOK_ULE: return "JLEU";
    case TOK_UGE: return "JGEU";
    default: return NULL;
    }
}

static const char *to8_byte_suffix(int bt)
{
    if (bt == VT_BYTE || bt == VT_BOOL) return "1";
    if (bt == VT_SHORT) return "2";
    return "4";
}

/* Fuse a pending "SHLi 24/16" with an immediately-following constant
 * SAR/SHR of the same amount: comment out the single SHLi line and
 * emit EXT1S/EXT2S (signed) or EXT1U/EXT2U (unsigned) in its place.
 * Returns 1 if handled, 0 if not applicable (any stale candidate is
 * cleared). Does NOT touch the vstack - the caller is responsible. */
static int to8_try_sex_fuse(int op, int amount_is_const, int amount_val)
{
    const char *rep;

    if (!pending_sex_amount) return 0;
    if (!amount_is_const || amount_val != pending_sex_amount ||
        (op != TOK_SAR && op != TOK_SHR)) {
        pending_sex_amount = 0;
        return 0;
    }

    rep = (op == TOK_SAR)
        ? (pending_sex_amount == 24 ? "EXT1S" : "EXT2S")
        : (pending_sex_amount == 24 ? "EXT1U" : "EXT2U");

    cur_text_section->data[pending_sex_line_start] = ';';
    emit_op(rep);

    pending_sex_amount = 0;
    return 1;
}

/* ===================================================================
 * Load / Store
 * =================================================================== */

void load(int r, SValue *sv)
{
    int v, ft, fc, bt;
    char buf[16];
    const char *suf;
    int save_slot;

    ft = sv->type.t & ~VT_DEFSIGN;
    fc = sv->c.i;
    v = sv->r & VT_VALMASK;
    ft &= ~(VT_VOLATILE | VT_CONSTANT);
    bt = ft & VT_BTYPE;
    suf = to8_byte_suffix(bt);

    if (r == TREG_F0) {
        if (bt == VT_FLOAT || bt == VT_DOUBLE) {
            const char *ld = (bt == VT_FLOAT) ? "LDF32" : "LDF64";
            if (sv->r & VT_LVAL) {
                if (v == VT_LOCAL) {
                    emit_op_slot(ld, fc);
                } else if (v == VT_LLOCAL) {
                    snprintf(buf, sizeof buf, "%sms", ld);
                    emit_op_slot(buf, fc);
                } else if (v == VT_CONST) {
                    snprintf(buf, sizeof buf, "%sm", ld);
                    emit_op_addr(buf, sv->sym, fc);
                } else if (v < VT_CONST) {
                    int temp;
                    if (v == TREG_R1) {
                        temp = to8_r1_slot();
                    } else {
                        temp = to8_new_temp(4, 4);
                        emit_op_slot("ST", temp);
                    }
                    snprintf(buf, sizeof buf, "%sms", ld);
                    emit_op_slot(buf, temp);
                } else {
                    tcc_error("TO8: unsupported float load addressing mode (v=%#x)", v);
                }
            } else {
                if (v == VT_CONST)
                    emit_op_addr(ld, sv->sym, fc);
                else if (v == VT_LOCAL) {
                    emit_op_slot(ld, fc);
                } else
                    emit_op("ITOF");
            }
            return;
        }
    }

    if (!(sv->r & VT_LVAL) && v < TREG_MEM && v != VT_CONST && v != VT_LOCAL &&
        v != VT_LLOCAL && v != VT_CMP && v != VT_JMP && v != VT_JMPI) {
        if (v == r) return;
        if (v == TREG_R1 && r == TREG_R0) { emit_op_slot("LD", to8_r1_slot()); return; }
        if (v == TREG_R0 && r == TREG_R1) { emit_op_slot("ST", to8_r1_slot()); return; }
    }

    save_slot = -1;
    if (r == TREG_R1) {
        save_slot = to8_new_temp(4, 4);
        emit_op_slot("ST", save_slot);
    }

    if (sv->r & VT_LVAL) {
        if (v == VT_LOCAL) {
            emit_op_slot("LD", fc);
        } else if (v == VT_LLOCAL) {
            snprintf(buf, sizeof buf, "LD%s", suf);
            emit_op_slot(buf, fc);
        } else if (v == VT_CONST) {
            snprintf(buf, sizeof buf, "LD%s", suf);
            emit_op_addr(buf, sv->sym, fc);
        } else if (v < VT_CONST) {
            int temp;
            if (v == TREG_R1) {
                temp = to8_r1_slot();
            } else {
                temp = to8_new_temp(4, 4);
                emit_op_slot("ST", temp);
            }
            snprintf(buf, sizeof buf, "LD%s", suf);
            emit_op_slot(buf, temp);
        } else {
            tcc_error("TO8: unsupported load addressing mode (v=%#x)", v);
        }
    } else {
        switch (v) {
        case VT_CONST:
            if (sv->r & VT_SYM)
                emit_op_addr("LEAm", sv->sym, fc);
            else
                emit_op_imm("LDi", fc);
            break;
        case VT_LOCAL:
            emit_op_slot("LEA", fc);
            break;
        case VT_LLOCAL:
            emit_op_slot("LD", fc);
            break;
        case VT_CMP: {
            const char *cc = to8_get_set_cond(sv->c.i);
            emit_op(cc ? cc : "SNZ");
            break;
        }
        case VT_JMP:
            emit_op_imm("LDi", 0);
            gsym(fc);
            emit_op_imm("LDi", 1);
            break;
        case VT_JMPI:
            emit_op_imm("LDi", 1);
            gsym(fc);
            emit_op_imm("LDi", 0);
            break;
        default:
            if (v == r) {
                if (save_slot >= 0) emit_op_slot("LD", save_slot);
                return;
            }
            if (v >= TREG_MEM) {
                snprintf(buf, sizeof buf, "LD%s", suf);
                emit_op_slot(buf, fc);
            }
            break;
        }
    }

    if (r == TREG_R1) {
        emit_op_slot("ST", to8_r1_slot());
        emit_op_slot("LD", save_slot);
    }
}

void store(int r, SValue *v)
{
    int fr, bt, fc;
    char buf[16];
    const char *suffix;

    if (r >= NB_REGS) {
        int vbt = vtop->type.t & VT_BTYPE;
        r = (vbt == VT_FLOAT || vbt == VT_DOUBLE) ? TREG_F0 : TREG_R0;
        load(r, vtop);
    }

    fr = v->r & VT_VALMASK;
    bt = v->type.t & VT_BTYPE;
    fc = v->c.i;
    bt &= ~(VT_VOLATILE | VT_CONSTANT);
    suffix = to8_byte_suffix(bt);

    if (r == TREG_F0) {
        const char *st = (bt == VT_FLOAT) ? "STF32" : "STF64";
        if (fr == VT_LOCAL) {
            emit_op_slot(st, fc);
        } else if (fr == VT_LLOCAL) {
            snprintf(buf, sizeof buf, "%sms", st);
            emit_op_slot(buf, fc);
        } else if (fr == VT_CONST) {
            snprintf(buf, sizeof buf, "%sm", st);
            emit_op_addr(buf, v->sym, fc);
        } else if (fr < VT_CONST) {
            int temp;
            if (fr == TREG_R1) {
                temp = to8_r1_slot();
            } else {
                temp = to8_new_temp(4, 4);
                emit_op_slot("ST", temp);
            }
            snprintf(buf, sizeof buf, "%sms", st);
            emit_op_slot(buf, temp);
        } else {
            tcc_error("TO8: unsupported float store addressing mode (fr=%#x)", fr);
        }
        return;
    }

    if (fr == VT_LOCAL) {
        if (r == TREG_R1) emit_op_slot("LD", to8_r1_slot());
        emit_op_slot("ST", fc);
    } else if (fr == VT_LLOCAL) {
        if (r == TREG_R1) emit_op_slot("LD", to8_r1_slot());
        snprintf(buf, sizeof buf, "ST%s", suffix);
        emit_op_slot(buf, fc);
    } else if (fr == VT_CONST && (v->r & VT_SYM)) {
        if (r == TREG_R1) emit_op_slot("LD", to8_r1_slot());
        snprintf(buf, sizeof buf, "ST%s", suffix);
        emit_op_addr(buf, v->sym, fc);
    } else if (v->r & VT_LVAL) {
        int addr_slot;
        if (fr == TREG_R1) {
            addr_slot = to8_r1_slot();
        } else {
            addr_slot = to8_new_temp(4, 4);
            emit_op_slot("ST", addr_slot);
        }
        if (r == TREG_R1) {
            emit_op_slot("LD", to8_r1_slot());
        }
        snprintf(buf, sizeof buf, "ST%s", suffix);
        emit_op_slot(buf, addr_slot);
    }
}

/* ===================================================================
 * Binary integer operations
 * =================================================================== */

static int to8_spill_and_reload(int v1, int c1)
{
    int temp = to8_new_temp(4, 4);
    emit_op_slot("ST", temp);
    if (v1 == TREG_R1)
        emit_op_slot("LD", to8_r1_slot());
    else
        emit_op_slot("LD", c1);
    return temp;
}

/* Dedicated path for SHL/SAR/SHR: if the shift amount (vtop, the
 * SECOND operand) is a compile-time constant, it is popped without
 * emitting any code for it at all, x is materialized into R0, and a
 * single "SHLi/SARi/SHRi <amount>" is emitted - exactly like x86's
 * "sarl $n,%eax", ARM's "asr #n" or RISC-V's "srai" immediate forms.
 * A non-constant amount falls back to routing it through a slot,
 * since this backend has no separate "shift count" register. */
static void gen_opi_shift(int op)
{
    const char *slot_op, *imm_op;
    int amount_is_const = (vtop->r & VT_VALMASK) == VT_CONST && !(vtop->r & VT_SYM);
    int amount_val = amount_is_const ? vtop->c.i : 0;

    to8_get_arith_ops(op, &slot_op, &imm_op);

    if ((op == TOK_SAR || op == TOK_SHR) &&
        to8_try_sex_fuse(op, amount_is_const, amount_val)) {
        vpop();               /* discard the shift-amount operand */
        vtop->r = TREG_R0;    /* x's own entry (now vtop) is the result */
        vtop->r2 = VT_CONST;
        return;
    }
    pending_sex_amount = 0;

    if (amount_is_const) {
        vpop();               /* amount becomes an immediate, no load */
        gv(RC_INT);           /* materialize x (now vtop) into R0 */
        {
            int line_start = emit_op_imm(imm_op, amount_val);
            if (op == TOK_SHL && (amount_val == 24 || amount_val == 16)) {
                pending_sex_amount = amount_val;
                pending_sex_line_start = line_start;
            }
        }
        vtop->r = TREG_R0;
        vtop->r2 = VT_CONST;
    } else {
        int v1, c1, temp;
        v1 = vtop[-1].r & VT_VALMASK;
        c1 = vtop[-1].c.i;
        if (v1 == TREG_R0) {
            int pre_spill = to8_new_temp(4, 4);
            emit_op_slot("ST", pre_spill);
            c1 = pre_spill;
        }
        gv(RC_INT);
        if (v1 != TREG_R0) {
            v1 = vtop[-1].r & VT_VALMASK;
            c1 = vtop[-1].c.i;
        }
        temp = to8_spill_and_reload(v1, c1);
        emit_op_slot(slot_op, temp);
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

    v1 = vtop[-1].r & VT_VALMASK;
    c1 = vtop[-1].c.i;

    if (v1 == TREG_R0) {
        int pre_spill = to8_new_temp(4, 4);
        emit_op_slot("ST", pre_spill);
        c1 = pre_spill;
    }

    gv(RC_INT);

    if (v1 != TREG_R0) {
        v1 = vtop[-1].r & VT_VALMASK;
        c1 = vtop[-1].c.i;
    }

    if (op == TOK_LT || op == TOK_GT || op == TOK_LE || op == TOK_GE ||
        op == TOK_EQ || op == TOK_NE || op == TOK_ULT || op == TOK_UGT ||
        op == TOK_ULE || op == TOK_UGE) {
        if (v1 == VT_CONST && c1 == 0) {
            emit_op("TST");
        } else if (v1 == VT_CONST) {
            emit_op_imm("CMPi", c1);
        } else {
            int temp = to8_spill_and_reload(v1, c1);
            emit_op_slot("CMP", temp);
        }
        vtop--;
        vset_VT_CMP(op);
        return;
    }

    const char *slot_op, *imm_op;
    if (to8_get_arith_ops(op, &slot_op, &imm_op) < 0) {
        emit_op("; unsupported integer op");
        vtop--; return;
    }

    if (to8_is_commutative(op)) {
        if (v1 == VT_CONST) emit_op_imm(imm_op, c1);
        else {
            int temp = to8_spill_and_reload(v1, c1);
            emit_op_slot(slot_op, temp);
        }
    } else {
        int temp;
        if (v1 == VT_CONST) {
            temp = to8_new_temp(4, 4);
            emit_op_slot("ST", temp);
            emit_op_imm("LDi", c1);
            emit_op_slot(slot_op, temp);
        } else {
            temp = to8_spill_and_reload(v1, c1);
            emit_op_slot(slot_op, temp);
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
    gv(RC_FLOAT);

    if (op == TOK_LT || op == TOK_GT || op == TOK_LE || op == TOK_GE ||
        op == TOK_EQ || op == TOK_NE) {
        const char *fset;
        switch (op) {
        case TOK_EQ: fset = "FSEQ"; break;
        case TOK_NE: fset = "FSNE"; break;
        case TOK_LT: fset = "FSLT"; break;
        case TOK_GT: fset = "FSGT"; break;
        case TOK_LE: fset = "FSLE"; break;
        case TOK_GE: fset = "FSGE"; break;
        default: fset = NULL; break;
        }
        if (fset) {
            int fv1 = vtop[-1].r & VT_VALMASK;
            int fc1 = vtop[-1].c.i;
            int temp = to8_new_temp(8, 4);
            emit_op_slot("STF64", temp);
            if (fv1 == VT_LOCAL || fv1 == VT_LLOCAL)
                emit_op_slot("LDF64", fc1);
            emit_op_slot("FCMP", temp);
            emit_op(fset);
        } else {
            emit_op("; unsupported float comparison");
        }
        vtop--;
        vtop->r = TREG_R0;
        vtop->r2 = VT_CONST;
        return;
    }

    switch (op) {
    case '+': emit_op("FADD"); break;
    case '-': emit_op("FSUB"); break;
    case '*': emit_op("FMUL"); break;
    case '/': emit_op("FDIV"); break;
    default: emit_op("; unsupported float op"); break;
    }
    vtop--;
    vtop->r = TREG_F0;
    vtop->r2 = VT_CONST;
}

/* ===================================================================
 * Type conversions
 * =================================================================== */

void gen_cvt_itof(int t) { emit_op("ITOF"); }
void gen_cvt_ftoi(int t) { emit_op("FTOI"); }
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
            to8_push_imm(sv->c.i);
        } else if (v == VT_LOCAL || v == VT_LLOCAL) {
            to8_push_slot(sv->c.i);
        } else if (sv->r & VT_SYM) {
            emit_op_addr("LEAm", sv->sym, sv->c.i);
            to8_push_r0();
        } else {
            load(TREG_R0, sv);
            to8_push_r0();
        }
    }
    save_regs(0);

    if ((func->r & VT_VALMASK) == VT_CONST && (func->r & VT_SYM)) {
        emit_op_addr("JSRi", func->sym, func->c.i);
    } else if ((func->r & VT_LVAL) &&
               ((func->r & VT_VALMASK) == VT_LOCAL || (func->r & VT_VALMASK) == VT_LLOCAL)) {
        emit_op_slot("JSRs", func->c.i);
    } else {
        load(TREG_R0, func);
        emit_op("JSR");
    }

    if (nb_args > 0) {
        size = nb_args * PTR_SIZE;
        to8_adjust(size);
    }
    vtop -= nb_args + 1;
}

/* ===================================================================
 * Function prolog / epilog
 * =================================================================== */

static int to8_func_sp_offset;
static int to8_func_line_start;
static int to8_func_ret_sub;
#define TO8_PROLOG_SIZE TO8_FIELD_WIDTH

void gfunc_prolog(Sym *func_sym)
{
    CType *func_type = &func_sym->type;
    Sym *sym;
    int addr, size, align;

    to8_func_ret_sub = 0;
    loc = 0;
    slot_patch_reset();

    if (funcname)
        emit_label(funcname);

    to8_func_line_start = ind;
    begin_line();
    emit_str("    ADJ ");
    to8_func_sp_offset = emit_placeholder_field(TO8_PROLOG_SIZE);
    emit_str("  ; sp -= frame size");
    emit_nl();
    pend_store_valid = 0;

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
    int frame_size, i;

#ifdef CONFIG_TCC_BCHECK
    if (tcc_state->do_bounds_check)
        gen_bounds_epilog();
#endif

    frame_size = -loc;

    if (frame_size > 0) {
        begin_line();
        emit_str("    ADJ ");
        emit_int(frame_size);
        emit_str("  ; sp += ");
        emit_int(frame_size);
        emit_nl();
        pend_store_valid = 0;
    }

    emit_op("RET");

    if (frame_size > 0) {
        write_int_fixed(cur_text_section->data + to8_func_sp_offset,
                         -frame_size, TO8_PROLOG_SIZE);
    } else {
        cur_text_section->data[to8_func_line_start] = ';';
    }

    for (i = 0; i < slot_patches_n; i++) {
        int val = frame_size + slot_patches[i].raw_slot + slot_patches[i].push_depth;
        write_int_fixed(cur_text_section->data + slot_patches[i].text_offset,
                         val, TO8_FIELD_WIDTH);
    }
}

/* ===================================================================
 * Jump generation
 * =================================================================== */

static int to8_emit_jmp(const char *mnemonic)
{
    int offset, cmt_off;

    to8_resolve_pending_mov(mnemonic);
    last_ld_valid = 0;

    begin_line();
    emit_str("    ");
    emit_str(mnemonic);
    g(' ');
    offset = emit_placeholder_field(TO8_FIELD_WIDTH);
    emit_str("  ; ");
    emit_str(jump_comment(mnemonic));
    g(' ');
    cmt_off = emit_placeholder_field(TO8_FIELD_WIDTH);
    jmp_cmt_patch_add(offset, cmt_off);
    emit_nl();
    pend_store_valid = 0;
    return offset;
}

ST_FUNC int gjmp(int t)
{
    int off = to8_emit_jmp("JMP");
    if (t) gsym_addr(off, t);
    return off;
}

ST_FUNC void gjmp_addr(int a)
{
    int off;

    to8_resolve_pending_mov("JMP");
    last_ld_valid = 0;

    begin_line();
    emit_str("    JMP ");
    off = emit_placeholder_field(TO8_FIELD_WIDTH);
    write_label_ref(cur_text_section->data + off, a, TO8_FIELD_WIDTH);
    emit_str("  ; goto l");
    emit_int(a);
    emit_nl();
    pend_store_valid = 0;
    mark_or_defer_target(a);
}

ST_FUNC int gjmp_cond(int op, int t)
{
    const char *jcond = to8_get_jcond(op);
    int off = to8_emit_jmp(jcond ? jcond : "JNE");
    if (t) gsym_addr(off, t);
    return off;
}

/* ===================================================================
 * VLA support
 * =================================================================== */

ST_FUNC void gen_vla_sp_save(int addr) { emit_op_slot("ST", addr); }
ST_FUNC void gen_vla_sp_restore(int addr) { emit_op_slot("LD", addr); }
ST_FUNC void gen_vla_sp_alloc(int size) { to8_adjust(-size); }

/* ===================================================================
 * Misc backend helpers required by tccgen/tccelf
 * =================================================================== */

ST_FUNC int gjmp_append(int n, int t)
{
    if (n) {
        int n1 = n, n2;
        while ((n2 = read_int_fixed(cur_text_section->data + n1, TO8_FIELD_WIDTH)))
            n1 = n2;
        write_int_fixed(cur_text_section->data + n1, t, TO8_FIELD_WIDTH);
        return n;
    }
    return t;
}

ST_FUNC void gen_fill_nops(int bytes)
{
    while (bytes-- > 0)
        emit_op("NOP");
}

ST_FUNC void ggoto(void)
{
    gfunc_call(1);
}

ST_FUNC void gen_vla_alloc(CType *type, int align)
{
    (void)type;
    (void)align;
    gv(RC_INT);
    emit_op("VLA_ALLOC");
    vpop();
}

#endif /* TARGET_DEFS_ONLY */
