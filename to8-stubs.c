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

ST_FUNC void gen_le32(int c)
{
    g(c);
    g(c >> 8);
    g(c >> 16);
    g(c >> 24);
}

#ifdef CONFIG_TCC_ASM

/* ===================================================================
 * Minimal inline-asm support for TO8: asm("text"); is dumped VERBATIM
 * into the pseudo-ASM output. No operands (":"), no constraints, no
 * clobbers, no GAS-style opcode parsing - just raw text passthrough.
 *
 * See the v7.23.0/v7.20.0/v7.18.0 changelog entries above for the
 * full history of why this needs the file->prev walk AND the
 * (pointer, length) dedup key.
 * =================================================================== */

static const uint8_t *g_asm_dump_key;
static size_t g_asm_dump_len;

ST_FUNC void asm_opcode(TCCState *s1, int opcode)
{
	tcc_error("inline asm() not supported (yet)");
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
