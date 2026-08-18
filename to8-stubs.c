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
 * v7.14.4 fix: CH_EOB (tcc.h) is '\\' (backslash), NOT '\0'. Never
 * scan file->buffer for a terminator of any kind (no strlen(),
 * pstrcpy(), or "while(*s)" loops). Instead, compute the EXACT length
 * TCC itself already tracks: buf_end - buffer, set directly from
 * 'initlen' by tcc_open_bf() at open time - always exactly right,
 * with zero guessing.
 *
 * Works identically for asm() inside a function AND at global/file
 * scope - to8_emit_raw_asm_n() (in to8-gen.c) picks the right
 * strategy based on g_in_function.
 * =================================================================== */
ST_FUNC void asm_opcode(TCCState *s1, int opcode)
{
    (void)s1;
    (void)opcode;

    if (file) {
        size_t len = (size_t)(file->buf_end - file->buffer);
        to8_emit_raw_asm_n((const char *)file->buffer, len);
    }

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
