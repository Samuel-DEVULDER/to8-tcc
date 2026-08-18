/* TO8 stubs — only symbols NOT defined in to8-gen.c
 * Included after to8-gen.c from libtcc.c
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

ST_FUNC void gen_le16(int v)
{
    g(v);
    g(v >> 8);
}

ST_FUNC void gen_le32(int v)
{
    gen_le16(v);
    gen_le16(v >> 16);
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
 * (tccpp.c) and writes CH_EOB ('\0') right after the valid content -
 * making file->buffer a properly NUL-terminated C string holding the
 * ORIGINAL text, verbatim, before any tokenization or substitution.
 * =================================================================== */
ST_FUNC void asm_opcode(TCCState *s1, int opcode)
{
    static void *last_file = NULL;
    (void)s1;
    (void)opcode;

    if (file && file != last_file) {
        last_file = file;
        to8_emit_raw_asm((const char *)file->buffer);
    }

    /* consume the rest of this statement so tcc_assemble_internal's
       "expect end of line" check right after this call succeeds */
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
    /* pas d'operandes geres : rien a calculer */
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
    /* le texte a deja ete emis dans asm_opcode() */
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
