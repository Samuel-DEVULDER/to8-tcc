/* TO8 stubs — only symbols NOT defined in to8-gen.c
 * Included after to8-gen.c from libtcc.c
 *
 * v7.23.0: fixed a real duplication bug in asm_opcode() found while
 * testing a multi-line/multi-label asm() body (2026-08-16 session):
 *     asm("\ttest2 a,b\nlabel:\n  code 0x1111");
 * printed its ENTIRE text TWICE. Root cause: tcc_assemble_internal()
 * (tccasm.c) retokenizes the ":asm:" buffer as a mini multi-statement
 * assembly file, calling asm_opcode() ONCE PER detected instruction
 * (every IDENT not followed by ':' or '='), independently of ':'
 * labels (handled separately via asm_new_label()). The body above
 * has TWO such instructions ("test2 a,b" and "code 0x1111"), so
 * asm_opcode() fired twice - and since it dumps the WHOLE buffer
 * every time regardless of which statement triggered it, the text
 * was emitted twice. Single-instruction, single-line bodies (the
 * only kind tested before) never hit this: only one instruction
 * token per buffer, hence only one call.
 *
 * Fix: track (bf->buffer pointer, length) of the last buffer we
 * actually dumped in g_asm_dump_key/g_asm_dump_len, and skip the
 * dump if the CURRENT call's buffer matches - but ONLY for as long
 * as that specific buffer is still open. Right after the token-
 * consuming loop, if `file` no longer equals `bf` (i.e. the
 * tokenizer has moved past this :asm: buffer entirely, either via a
 * real pop-to-parent or by exhausting it), the key is immediately
 * invalidated. This is what makes it safe against the EXACT failure
 * mode that got the old static "last_file" guard removed in v7.18.0
 * (a later, UNRELATED :asm: buffer coincidentally reusing the same
 * freed heap address, and getting wrongly treated as a repeat): the
 * key can never survive past the lifetime of the specific buffer it
 * was recorded for, so it can never alias a future, different one.
 *
 * v7.20.0: asm_opcode() rewritten to walk the file->prev chain
 * looking for the ":asm:" pseudo-file, instead of trusting the
 * CURRENT file pointer blindly. For a single-token asm body with no
 * operand characters at all (e.g. asm("test");), tcc_assemble_
 * internal()'s own one-token lookahead (checking whether ':' or '='
 * follows an identifier) EXHAUSTS the entire virtual :asm: buffer
 * before asm_opcode() ever runs. That triggers the same pop-to-
 * parent mechanism used for ordinary include exhaustion, so `file`
 * no longer points at the :asm: pseudo-file by the time this
 * function executes - it points at the REAL enclosing source file
 * instead. tcc_assemble_inline() (tccasm.c) only tcc_close()s the
 * :asm: BufferedFile AFTER tcc_assemble_internal() fully returns, so
 * even once popped off `file`, that struct is still alive in memory,
 * reachable via file->prev. This version walks that chain looking
 * for the ":asm:" name set by tcc_open_bf(s1, ":asm:", len) in
 * tcc_assemble_inline(), instead of assuming `file` itself is it.
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
 *
 * v7.18.0: removed the "static void *last_file" dedup guard in
 * asm_opcode() - it caused every asm() statement AFTER THE FIRST ONE
 * in a whole compilation unit to be silently dropped, as soon as a
 * later :asm: pseudo-file's BufferedFile allocation happened to reuse
 * the exact heap address of an earlier one.
 *
 * IMPORTANT: 'buffer' is NOT a NUL-terminated C string, and must
 * never be scanned for a terminator of any kind. The exact length is
 * computed as (buf_end - buffer) and passed to to8_emit_raw_asm_n(),
 * which memcpy()s exactly that many bytes - no scanning whatsoever.
 *
 * KNOWN LIMITATION (unchanged since v7.14.4, not fixed here): a
 * multi-STATEMENT asm() body (embedded labels, multiple leading
 * identifiers) is dumped as ONE opaque blob, verbatim, exactly as
 * written - this backend does NOT parse/relocate/rewrite any of it.
 * That's by design (raw passthrough, no operand substitution), just
 * worth remembering when reading a generated listing.
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
    BufferedFile *bf;
    (void)s1;
    (void)opcode;

    bf = file;
    while (bf && strcmp(bf->filename, ":asm:") != 0)
        bf = bf->prev;

    if (bf) {
        size_t len = (size_t)(bf->buf_end - bf->buffer);
        /* v7.23.0: only dump if this SPECIFIC still-open buffer
         * hasn't already been dumped by an earlier asm_opcode() call
         * for the SAME asm("...") statement (multi-instruction body -
         * see changelog). Comparing both pointer AND length, and
         * invalidating the key the moment we leave this buffer
         * (below), means this can never alias a later, unrelated
         * :asm: buffer that happens to reuse the same freed address -
         * that was the exact failure mode that got the old blanket
         * "last_file" guard removed in v7.18.0. */
        if (bf->buffer != g_asm_dump_key || len != g_asm_dump_len) {
            to8_emit_raw_asm_n((const char *)bf->buffer, len);
            g_asm_dump_key = bf->buffer;
            g_asm_dump_len = len;
        }
    }

    /* consume the rest of this statement so tcc_assemble_internal's
     * "expect end of line" check right after this call succeeds */
    while (tok != ';' && tok != TOK_LINEFEED && tok != CH_EOF)
        next();

    /* v7.23.0: if `file` no longer points at the :asm: buffer we
     * just processed, the tokenizer has moved past it for good
     * (either a real pop-to-parent, or it was exhausted) - no more
     * asm_opcode() calls will ever come in for it. Invalidate the
     * dedup key immediately so it can't collide with a later,
     * unrelated :asm: buffer at the same heap address. */
    if (file != bf) {
        g_asm_dump_key = NULL;
        g_asm_dump_len = 0;
    }
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
