/* TO8 stubs — only symbols NOT defined in to8-gen.c
 * Included after to8-gen.c from libtcc.c
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
 * v7.18.0: removed the "static void *last_file" dedup guard in
 * asm_opcode() - it caused every asm() statement AFTER THE FIRST ONE
 * in a whole compilation unit to be silently dropped, as soon as a
 * later :asm: pseudo-file's BufferedFile allocation happened to reuse
 * the exact heap address of an earlier one (very common once a few
 * asm() statements have been opened/closed). The trailing
 * "while (tok != ';' ...) next();" loop already fully consumes every
 * token of the CURRENT statement before returning, so asm_opcode()
 * can never legitimately re-fire for the same asm(); the extra guard
 * was not just unnecessary, it was actively wrong.
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
 * exist anywhere once the macro has fired everywhere they're used). */
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
 * Minimal inline-asm support for TO8: asm("text"); is dumped VERBATIM
 * into the pseudo-ASM output. No operands (":"), no constraints, no
 * clobbers, no GAS-style opcode parsing - just raw text passthrough.
 *
 * Technique: tcc_assemble_inline() (tccasm.c) opens a virtual ":asm:"
 * pseudo-file and memcpy()s the exact literal text (unmodified, as
 * long as there is no ":" operand list) into file->buffer BEFORE
 * tokenizing it. By the time asm_opcode() is first called for that
 * file, the lexer has already had to peek one char past the text to
 * find the first token's boundary, which triggers handle_eob()
 * (tccpp.c) and writes CH_EOB ('\\', NOT '\0' - see tcc.h) right
 * after the valid content.
 *
 * IMPORTANT: 'file->buffer' is therefore NOT a NUL-terminated C
 * string, and must never be scanned for a terminator of any kind.
 * The exact length is computed here as
 * (file->buf_end - file->buffer) and passed to to8_emit_raw_asm_n(),
 * which memcpy()s exactly that many bytes - no scanning whatsoever.
 * =================================================================== */
ST_FUNC void asm_opcode(TCCState *s1, int opcode)
{
    BufferedFile *bf = file;
    (void)s1;
    (void)opcode;

    /* v7.19.1: pour un corps asm() à un seul token sans opérande
     * (ex: asm("test");), le lookahead d'un seul token nécessaire
     * pour vérifier un ':' ou '=' final épuise ENTIÈREMENT le
     * buffer virtuel ":asm:", ce qui déclenche le même mécanisme
     * de dépilement qu'un #include épuisé - AVANT que cette
     * fonction ne s'exécute. `file` ne pointe donc plus sur le
     * pseudo-fichier ":asm:" mais sur le fichier source réel.
     * Le buffer dépilé reste vivant (tcc_close() n'est appelé par
     * tcc_assemble_inline() qu'après le retour complet de
     * tcc_assemble_internal()) - on le retrouve via file->prev. */
    while (bf && strcmp(bf->filename, ":asm:") != 0)
        bf = bf->prev;

    if (bf) {
        size_t len = (size_t)(bf->buf_end - bf->buffer);
        to8_emit_raw_asm_n((const char *)bf->buffer, len);
    }

    /* consume the rest of this statement so tcc_assemble_internal's
     * "expect end of line" check right after this call succeeds */
    while (tok != ';' && tok != TOK_LINEFEED && tok != CH_EOF)
        next();
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
    /* no operands handled: nothing to compute */
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
    /* the text was already emitted in asm_opcode() */
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
