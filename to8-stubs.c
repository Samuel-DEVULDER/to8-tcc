/* TO8 stubs — only symbols NOT defined in to8-gen.c
 * Included after to8-gen.c from libtcc.c
 *
 * v7.22.0: asm("text"); is now REJECTED outright with a clear
 * tcc_error(), instead of attempting the fragile raw-buffer-read
 * passthrough. Root-cause diagnosis (2026-08-15 session) proved that
 * technique cannot be made reliable without patching common
 * tccpp.c/tccasm.c code (the internal tokenizer's own lookahead can
 * corrupt file->buf_end/file->buffer for certain short asm() bodies,
 * before asm_opcode() ever runs) - so rather than keep a known-buggy
 * code path around, this backend now refuses it explicitly and
 * points users at __native_asm() (to8-gen.c's gfunc_call()
 * interception), which has NO such limitation for any text length,
 * since it goes through TCC's ordinary (robust) string-literal
 * tokenizer instead of the special ":asm:" pseudo-file mechanism.
 *
 * Users who want to keep writing asm("...") syntax can opt back in
 * themselves, entirely from their own C source, with:
 *     void __native_asm(const char *s);
 *     #define asm __native_asm
 * (macro substitution happens before the parser ever treats "asm" as
 * the special GNU keyword, since asm is not a real C reserved word -
 * it's an extension identifier registered like any other token, so
 * ordinary #define rules apply cleanly). Extended GNU asm syntax
 * with operands ("asm("code" : "=r"(out) : ...)") is NOT covered by
 * that #define (the colons aren't valid in a function-call argument
 * list) - but that syntax never actually worked on this backend
 * anyway (asm_compute_constraints()/asm_gen_code() below are, and
 * always were, no-op stubs), so nothing of value is lost.
 *
 * v7.19.0: gen_le16/gen_le32 replaced by gen_be16_impl/gen_be32_impl,
 * redirected via a macro defined in to8-gen.c's TARGET_DEFS_ONLY
 * block (visible early enough to rewrite tccgen.c's calls, without
 * touching a single line of common tccgen.c code). The TO8 (6809)
 * target is __BIG_ENDIAN__ (see target_machine_defs), but gen_le16/
 * gen_le32 were copy-pasted from a little-endian backend and wrote
 * low-byte-first - exactly backwards for any multi-byte value they
 * encode on this target.
 *
 * NOTE: this redirect does NOT affect float/double constant bytes
 * (see the architecture comment near OP_LDF/OP_LDG in to8-gen.c) -
 * those bytes are written by a completely different, common-code
 * path (gv()'s float-constant materialization -> init_put_v()) that
 * never calls gen_le16/gen_le32 at all.
 */
#ifndef TO8_STUBS_INCLUDED
#define TO8_STUBS_INCLUDED

/* Byte emitters used by emit_* in to8-gen.c */
ST_FUNC void g(int c)
{
    int ind1;
    if (nocode_wanted)
        return;
    ind1 = ind + 1;
    if (ind1 > (int)cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind1);
    cur_text_section->data[ind] = (unsigned char)c;
    ind = ind1;
}

/* v7.19.0: MSB-first (big-endian) emitters. Called via the
 * gen_le16(v)/gen_le32(v) macros defined in to8-gen.c's
 * TARGET_DEFS_ONLY block, which rewrite every gen_le16/gen_le32
 * call site in tccgen.c (and anywhere else in the single ONESOURCE
 * translation unit compiled AFTER that macro definition) into a
 * call to these functions - no common code touched, no symbol name
 * collision (the bare names "gen_le16"/"gen_le32" no longer need to
 * exist anywhere once the macro has fired everywhere they're used).
 *
 * Does NOT cover float/double constant bytes - see the note above
 * and the architecture comment in to8-gen.c near OP_LDF/OP_LDG. */
ST_FUNC void gen_be16_impl(int v)
{
    g(v >> 8);
    g(v);
}

ST_FUNC void gen_be32_impl(int v)
{
    gen_be16_impl(v >> 16);
    gen_be16_impl(v);
}

#ifdef CONFIG_TCC_ASM

/* ===================================================================
 * v7.22.0: asm("text"); is REJECTED. Use __native_asm("text") (see
 * to8-gen.c's gfunc_call()) instead - it is strictly more robust and
 * has no known limitation. See the file-level changelog above for
 * the full rationale and the #define asm __native_asm opt-in for
 * anyone who still wants the asm(...) spelling.
 * =================================================================== */
ST_FUNC void asm_opcode(TCCState *s1, int opcode)
{
    (void)s1;
    (void)opcode;
    tcc_error("TO8: asm(\"...\") is not supported by this backend - "
              "use __native_asm(\"...\") instead (declare it as "
              "'void __native_asm(const char *s);' - it is a compiler "
              "intrinsic, never actually linked). If you need the "
              "asm(...) spelling, add '#define asm __native_asm' to "
              "your own source.");
}

ST_FUNC void gen_expr32(ExprValue *pe)
{
    (void)pe;
}

ST_FUNC int asm_parse_regvar(int t)
{
    (void)t;
    return -1;
}

ST_FUNC void asm_compute_constraints(ASMOperand *operands, int nb_operands,
                                      int nb_outputs, const uint8_t *clobber_regs,
                                      int *pout_reg)
{
    (void)operands;
    (void)nb_operands;
    (void)nb_outputs;
    (void)clobber_regs;
    (void)pout_reg;
    /* no operands handled: nothing to compute. Extended GNU asm
     * syntax never worked on this backend regardless of the v7.22.0
     * asm_opcode() rejection above - this stub predates it. */
}

ST_FUNC void asm_gen_code(ASMOperand *operands, int nb_operands, int nb_outputs,
                           int is_output, uint8_t *clobber_regs, int out_reg)
{
    (void)operands;
    (void)nb_operands;
    (void)nb_outputs;
    (void)is_output;
    (void)clobber_regs;
    (void)out_reg;
    /* no-op: asm_opcode() now always tcc_error()s before this could
     * matter for any real asm() statement. */
}

ST_FUNC void subst_asm_operand(CString *add_str, SValue *sv, int modifier)
{
    (void)add_str;
    (void)sv;
    (void)modifier;
}

ST_FUNC void asm_clobber(uint8_t *clobber_regs, const char *str)
{
    (void)clobber_regs;
    (void)str;
}

#endif /* CONFIG_TCC_ASM */

#endif /* TO8_STUBS_INCLUDED */
