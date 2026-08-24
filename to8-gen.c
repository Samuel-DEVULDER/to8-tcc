/* TO8 backend for TCC - single-register pseudo-ASM generator.
 *
 * Version: 8.5.0 (to8_peephole_fmov() now scans past F-transparent ops)
 *
 * Changelog:
 * - v8.5.0: to8_peephole_fmov() now performs a real forward scan
 *   instead of firing only on strictly adjacent lines. The v8.4.0
 *   version required "LDF* X ; LDG* X" back-to-back; any intervening
 *   instruction - even a plain integer comparison or a conditional
 *   jump with no side effect on F - silently defeated the fusion.
 *
 *   Reuses the same "does this opcode write F" classification already
 *   established in to8_peephole_useless_fload() (v8.1.x), factored out
 *   as to8_stops_fmov_scan(): LDF* /FADD/FSUB/FMUL/FDIV/FSCALEi/ITOF (all
 *   redefine F) and JSR/JSRi/JSRr (opaque - the callee may use F/G
 *   arbitrarily) stop the scan. Everything else - STF4/STF8/STF4m/
 *   STF8m (read F, never write it), FCMP/FSGN/FTOI (read F, write only
 *   R0), all integer instructions, and every jump (reads only R0's
 *   sign) - is transparent and the scan continues through it.
 *
 *   !scan->is_target stops the scan for the usual reason: a jump
 *   landing directly on the G-load may arrive with F holding a value
 *   unrelated to the LDF* this pass is tracking.
 *
 *   Example:
 *       LDF4    24      ; F = x
 *       CMP2    8,4     ; unrelated integer comparison
 *       JGE     @47     ; unrelated conditional jump
 *       LDG4    24      ; G = x
 *   becomes:
 *       LDF4    24      ; F = x
 *       CMP2    8,4
 *       JGE     @47
 *       FMOV            ; G = F
 *   a fusion the strictly-adjacent v8.4.0 version could never reach.
 *
 * - v8.4.0: new peephole to8_peephole_fmov(). Collapses "LDF* X ;
 *   LDG* X" (F loaded first, then G loaded with the IDENTICAL address
 *   or immediate value right after) into "LDF* X ; FMOV" - the second
 *   memory load is replaced by a cheap register-to-register copy
 *   (G := F), avoiding a redundant memory access for a value F already
 *   holds.
 *   
 *   Found while reviewing mandel()'s self-multiply pattern (x*x, y*y):
 *   gen_opf()'s left-operand loading (to8_load_fg) has no way to know,
 *   at codegen time, that the left and right operands of an expression
 *   like "x*x" happen to be the exact same slot - it loads each
 *   occurrence independently, once into F (the right operand, via
 *   gv(RC_FLOAT)) and once into G (the left operand). The two loads are
 *   structurally identical (same opcode family, same slot/symbol/
 *   immediate), and F already holds the value the second load would
 *   fetch again from memory.
 *   
 *   Implemented via to8_g_counterpart(), a small table pairing each F
 *   load opcode with its G equivalent (OP_LDFi/OP_LDGi, OP_LDF4/OP_LDG4,
 *   OP_LDF8/OP_LDG8, OP_LDF4m/OP_LDG4m, OP_LDF8m/OP_LDG8m). The pass only
 *   fires on DIRECTLY ADJACENT lines (nxt == cur->next) - unlike most
 *   other peepholes in this file, no scanning loop is needed, since
 *   adjacency alone guarantees nothing else could have touched F in
 *   between. The only remaining hazard is a jump landing directly on
 *   the second load (skipping the first): guarded by the usual
 *   !nxt->is_target check, same precedent as to8_peephole_mov()/
 *   to8_peephole_ld_deref().
 *   
 *   One-directional by construction: OP_FMOV only ever computes
 *   G := F, never the reverse, so "LDG* X ; LDF* X" (G loaded first)
 *   is NOT touched by this pass - there is no "F := G" opcode to
 *   express that direction, consistent with G being write-only/never a
 *   result destination throughout this backend's F/G design (see the
 *   v8.0.0 changelog entry above).
 *   
 *   Example (mandel(), x*x):
 *       LDF4    24      ; F = x
 *       LDG4    24      ; G = x
 *       FMUL            ; F = G * F
 *   becomes:
 *       LDF4    24      ; F = x
 *       FMOV            ; G = F
 *       FMUL            ; F = G * F
 *   trading one memory read for one register-to-register copy.
 *   
 *   Runs under -O only, like every other peephole pass in this file
 *   (gated by tcc_state->optimize via to8_peephole_run()'s early
 *   return). Complementary to, not overlapping with,
 *   to8_peephole_useless_fload() (v8.1.x): that pass removes a
 *   redundant RELOAD into the SAME register that already holds the
 *   value; this one propagates a value ALREADY in F across to G
 *   instead of re-fetching it from memory.
 *
 * - v8.3.0: Extended optim to replace STF8 by the two MOV.
 *
 * - v8.2.0: Peephole optim to replace STF4 by the faster MOV.
 *
 * - v8.1.0: Added FSCALEi.
 *
 * - v8.0.0: MAJOR BREAKING CHANGE - the single shared float accumulator
 * F0 (width tracked externally by the caller) is replaced by two
 * dedicated registers, F and G, on three orthogonal axes: register
 * (F/G), width (4=float/8=double), addressing mode (i=immediate,
 * plain=own-slot value, m=pointer/symbol dereference). All binary
 * arithmetic is now pure register-to-register: F = G op F, result
 * always landing in F. G is write-only from this backend's
 * perspective - loadable from immediate/slot/pointer, but never
 * storable back to memory and never a result destination.
 *
 * Motivation: reviewing a generated mandel() listing (2026-08-21)
 * found two classes of redundant instructions inherent to the old
 * single-accumulator design: (1) spilling a literal constant to a
 * temp slot purely to satisfy CMPF/CMPG's slot-only comparison
 * operand, and (2) spilling a just-loaded value to a temp slot
 * purely to re-read it as the second operand of ADDF/MULF/etc. in
 * self-combining expressions like x*x. Both patterns are structurally
 * impossible once the two operands live in independent registers.
 *
 * Immediates are now 32-bit float only (OP_LDFi/OP_LDGi) - there is
 * no 8-byte immediate form. This backend's F/G registers are 32-bit
 * only; "double" only ever exists in memory. A double C constant is
 * never baked into an instruction: it is always read through memory
 * (OP_LDF8m/OP_LDG8m) and narrowed to the register's actual 32-bit
 * representation by the eventual real 6809 implementation of those
 * opcodes, symmetric to the existing IEEE<->EXTRAMON conversion
 * already documented at OP_LDF4/OP_LDF8 above. OP_STF8/OP_STF8m widen
 * back to 8-byte IEEE on the way out. to8_render_line()'s ARG_FIMM
 * case is simplified accordingly: it always renders a 2-word float
 * (out_float()), never the old 4-word out_double() path, since
 * ARG_FIMM is now exclusively used by OP_LDFi/OP_LDGi.
 *
 * OP_TSTF (R0 = sign(F)) is reserved in the ISA for a future
 * peephole/codegen shortcut on the extremely common x==0/x>0/x<0 and
 * if(x) idioms; gen_opf() does not emit it yet in this revision.
 *
 * G is deliberately kept OUT of TCC's register-class system (no
 * TREG_G, no RC_G bit in reg_classes[]) - TCC's binary-op codegen
 * never materializes the LEFT operand into a register before calling
 * gen_opf() (only the RIGHT operand, via gv(RC_FLOAT)), so a TCC-
 * visible G would gain nothing; worse, G's write-only/non-storable
 * nature would violate the symmetry TCC's generic allocator assumes
 * of any register class it manages, echoing the R1 lesson from
 * v7.38.0 (a second integer register that was fully wired into the
 * class system and turned out to be entirely dead code in practice).
 *
 * Real correctness fix folded into this rewrite: the old gen_opf()'s
 * comparison branch only reloaded the LEFT operand into the register
 * when it was VT_LOCAL/VT_LLOCAL - a left operand that was itself a
 * VT_CONST (e.g. "3.0 < x") silently compared the RIGHT operand
 * against itself, since no reload ever happened. The new
 * to8_load_fg() helper (shared with load()) handles VT_LOCAL/
 * VT_LLOCAL/VT_CONST uniformly, so this gap no longer exists.
 *
 * to8_peephole_dead_r0_load()'s whitelist is updated to the new F/G
 * opcode names. OP_CMPF and OP_TSTF remain deliberately EXCLUDED from
 * it, exactly like the old OP_CMPF/OP_CMPG were - both write a signed
 * result into R0 and must stop the dead-load scan like any other
 * R0-writing instruction.
 *
 * BREAKING: every previously generated listing is invalid under this
 * revision - OP_LDF/OP_LDG/OP_STF/OP_STG/OP_ADDG/OP_SUBG/OP_MULG/
 * OP_DIVG/OP_CMPG no longer exist, and OP_LDF4/OP_LDF8/OP_STF4/OP_STF8
 * change MEANING (own-slot value instead of pointer dereference - the
 * old dereference role moves to the new *m-suffixed opcodes). This is
 * why this revision bumps the MAJOR version instead of a patch/minor
 * bump.
 * 
 * - v7.40.0: imm/fimm operand rendering now splits into 16-bit words
 *   instead of printing a single decimal number, matching the target's
 *   native word size. ARG_IMM (plain int immediates, e.g. OP_LDi,
 *   OP_ADDi) always renders as 2 words (a C int is 32 bits = 2*16).
 *   ARG_FIMM renders as 2 words for float (OP_LDFi, 32 bits) or 4
 *   words for double (OP_LDGi, 64 bits) - the word count follows the
 *   operand's own width, not a fixed rule.
 *
 *   Each word is extracted via mask/shift on a fixed-width unsigned
 *   integer obtained through memcpy(&bits, &v, sizeof bits) - not
 *   through raw byte-pointer arithmetic - to make the extraction
 *   itself trivially correct and self-documenting (bits & 0xFFFF,
 *   bits >> 16, etc.), regardless of host byte order.
 *
 *   For float/double specifically, the byte SEQUENCE deliberately
 *   matches the host's native layout (see the ARCHITECTURE NOTE above:
 *   float/double constants stay little-endian on this backend, byte-
 *   order conversion is punted to the real 6809 LDF/LDG/STF/STG). The
 *   two/four words are built from consecutive byte PAIRS taken in
 *   host memory order (first pair -> first word, etc.), each pair
 *   read as a big-endian 16-bit value. Since a real 6809 assembler's
 *   FDB directive stores each word big-endian (high byte, then low
 *   byte) at consecutive addresses, printing the words in this order
 *   reproduces the EXACT SAME byte sequence in 6809 memory as in host
 *   memory - verified on 1.0f (0x3F800000): host bytes are
 *   00,00,80,3F; FDB $0000,$803F stores 00,00,80,3F - identical.
 *
 *   out_int16() picks decimal or $hex per-word, whichever renders
 *   SHORTER (both candidates are formatted and compared by string
 *   length, not a fixed magnitude threshold - see its own comment).
 *
 * - v7.39.0: new fast path in gen_opi() for non-commutative ops (SUB,
 *   DIV, MOD, and their unsigned UDIV/UMOD variants) with a constant
 *   RIGHT-hand operand - the extremely common "variable - constant"
 *   shape (e.g. "d - 1", "p - 1", "i % 10", "return d-1;").
 *
 * - v7.38.0: removed all dead code related to the virtual R1 register.
 *   RC_INT has been #define'd to RC_R0 alone since early on, which means
 *   gv(RC_INT) can never return TREG_R1 - no caller of load()/store()
 *   ever forces r == TREG_R1 explicitly either. Every branch conditioned
 *   on TREG_R1 was therefore unreachable dead code, confirmed by
 *   re-testing swap()/copy()/f()/g()/h()/mul_complex2()/__qsort(): all
 *   listings are byte-for-byte identical before and after this cleanup.
 *
 *   Removed:
 *   - RC_R1 and the TREG_R1 enum value (NB_REGS: 3 -> 2, reg_classes[]
 *     now holds only RC_R0/RC_FLOAT).
 *   - to8_r1_slot() and its backing r1_shadow_slot/r1_shadow_valid
 *     shadow-slot machinery.
 *   - load()'s TREG_R0<->TREG_R1 short-circuit pair, and its whole
 *     save_slot mechanism (it existed solely to preserve R0 across the
 *     r == TREG_R1 case at function exit).
 *   - store()'s three "if (r == TREG_R1) e_op_slot(OP_LD, ...)" reload
 *     guards, and the from_pool/addr_slot ternaries in both the float
 *     store branch and the fr != TREG_R0 indirect-store branch.
 *   - gen_opi_shift()/gen_opi(): four "(v1 == TREG_R1) ? to8_r1_slot()
 *     : c1" ternaries collapsed to plain "c1", since v1 can only ever
 *     be TREG_R0 or a VT_CONST/slot value at those points.
 *
 *   Purely a simplification/readability pass - zero effect on emitted
 *   code, since none of this logic was ever exercised. R1 remains
 *   documented in the "Architecture" note below purely as a historical
 *   marker of the original two-integer-register design that was never
 *   completed; a future backend revision may either resurrect it as a
 *   real second register or drop the architecture comment entirely.
 *
 * - v7.37.0: gfunc_call() argument-pushing loop - the VT_SYM branch
 *   (pure symbol-address argument, e.g. &L.3 for a string literal)
 *   used to materialize the address in R0 via eop_addr(OP_LDi, ...)
 *   then push_r0(). Replaced with a direct e_push_addr(sym, c) call,
 *   saving 1 instruction per call site (puts("...") and any &symbol
 *   argument). to8_render_line()'s ARG_SYM case already renders any
 *   opcode generically, including PUSHi - no rendering change needed.
 *
 * - v7.36.0: fix gfunc_prolog() struct-return test. VT_STRUCT (7) is a
 *   VT_BTYPE enum value, not an isolated bit - "t & VT_STRUCT" matched
 *   any return type whose low bits overlap 0b111 (int, char, short,
 *   ptr...), reserving a bogus extra PTR_SIZE before the first param
 *   and shifting every param[] index by one. void-returning functions
 *   (VT_VOID=0) were immune, which is why _copy/_swap looked fine while
 *   _h/_f/_g/mul_simple didn't. Fixed: (func_vt.t & VT_BTYPE) == VT_STRUCT.
 * 
 * - v7.35.0: local[] and param[] indices in listing comments are now
 *   plain slot counts (byte offset / TO8_STACK_ALIGN), 0-based,
 *   instead of raw byte offsets. The first local (slot == -4) shows
 *   as local[0]; the first parameter (slot == PTR_SIZE, right after
 *   the return address) shows as param[0]. Gaps in the sequence are
 *   accepted wherever a value wider than one 4-byte slot is involved
 *   (a double/long long local, or a hidden struct-return pointer
 *   reserving param slot 0) - the goal is only to avoid showing raw
 *   multiples of TO8_STACK_ALIGN in the comment text, not to
 *   guarantee a gap-free sequence.
 *
 * - v7.34.0: CORRECTNESS FIX in gfunc_prolog()'s parameter address
 *   computation. The loop over function parameters did
 *   `addr += size; sym_push(..., addr);` - incrementing addr by the
 *   CURRENT parameter's own size BEFORE recording its offset, so
 *   every parameter's stored address pointed past its own storage
 *   instead of at its start. For f(char *s, char *d), s was recorded
 *   at offset 8 instead of 4, and d at offset 12 instead of 8 - each
 *   parameter shifted upward by its own width, cumulatively across
 *   all parameters of a function.
 *
 *   Fix: swap the two statements so sym_push() records addr BEFORE
 *   it is advanced for the next parameter - the standard "use, then
 *   advance" pattern, matching the stack layout at function entry
 *   (<return address> <1st arg> <2nd arg> ..., all on 4-byte slots,
 *   no saved frame pointer since this backend never pushes one).
 *
 *   This changes every displayed parameter slot number and every
 *   ADD/LD/ST/MOV instruction referencing a parameter across all
 *   existing listings - expected, since the old addresses were wrong,
 *   not just displayed differently.
 *
 * - v7.33.1: fixed a real bug in source-line tracking (introduced in
 *   v7.32.0's metadata refactor) affecting every to8_line created
 *   from inline asm() text. to8_append() read file->line_num to
 *   stamp ln->src_line, but for lines created via
 *   to8_emit_raw_asm_n() (called from asm_opcode() in to8-stubs.c),
 *   file could still point at the ":asm:" pseudo-BufferedFile opened
 *   by tcc_assemble_inline() - a fresh buffer whose line_num starts
 *   at 1, unrelated to the real source line of the asm() call.
 *
 *   Symptom: the first raw-text line of any asm()-only function (or
 *   the first asm() statement in a function) got src_line = 1. At
 *   render time, file had already moved back to the real source file,
 *   so to8_read_source_line() correctly read ITS line 1 - producing a
 *   plausible-looking but completely wrong marker (verbatim text from
 *   the first line of the file, e.g. another function's declaration).
 *
 *   Fix: when computing ln->src_line in to8_append(), walk file->prev
 *   past any BufferedFile named ":asm:" before reading line_num -
 *   the exact same defensive pattern asm_opcode() itself has used
 *   since v7.20.0, for the identical underlying reason (tcc_assemble_
 *   inline()'s virtual :asm: buffer is not the real enclosing file
 *   and must never be trusted blindly for position information).
 *
 *   Verified: _putc and _test_asm (both asm()-only bodies) now show
 *   their correct source lines (L111/L112, L116-L119) with the actual
 *   __native_asm(...) call text, instead of a stale line 1 marker
 *   borrowed from an unrelated earlier function.
 *
 * - v7.33.0: variable name recovery for slot_desc(), gated behind -g.
 *   Without -g, listings are byte-for-byte identical to before this
 *   feature existed ("local[N]"/"param[N]").
 *
 *   With -g, slot_desc() resolves the real declared name of a local
 *   variable or parameter when available, by walking local_stack for
 *   a matching Sym (matched on Sym.c, the same stack offset used by
 *   SValue for VT_LOCAL/VT_LLOCAL access; named via Sym.v through
 *   get_tok_str()). Compiler-generated temporaries (to8_temp_alloc)
 *   have no matching Sym and fall back gracefully to the previous
 *   "local[N]"/"param[N]" text.
 *
 *   Lookups go through a fixed 32-entry LRU cache (to8_slot_cache)
 *   rather than a bounded pre-built table, so there is no cap on the
 *   number of distinct variables a function can have - only the cache
 *   hit rate varies with locality, never coverage. The cache is reset
 *   once per function in gfunc_prolog(), since slot numbers are reused
 *   across functions (loc starts at 0 each time).
 *
 *   This is a debug-listing convenience only: it never affects
 *   optimization, instruction selection, or generated code, exactly
 *   like the -g source-line markers it complements.
 *
 * - v7.32.0: source-line markers are now stored as metadata on each
 *   real instruction and emitted only during final listing rendering
 *   when -g is enabled. Previously, each marker was inserted as a
 *   synthetic raw-text/OP_NOP line while the instruction list was
 *   being built.
 *
 *   This removes the possibility for source comments to affect
 *   peephole passes, instruction IDs, label numbering, target
 *   resolution, or final instruction counts. The optimized instruction
 *   list is therefore identical with and without -g; -g changes only
 *   the rendered listing.
 *
 *   Source markers are emitted when the source line changes and are
 *   reset at the beginning of every function. The source text is read
 *   at render time from the original source file. If the file or line
 *   cannot be read, rendering degrades gracefully to "; L<n>".
 *
 *   A source marker belongs to the first surviving instruction associated
 *   with that source line. If peephole optimization removes or fuses
 *   instructions, the marker follows the surviving instruction naturally
 *   because it is attached to the instruction metadata rather than to a
 *   separate list node.
 *
 *   Also fixed the LD/ST-to-MOV peephole representation: fused MOV
 *   instructions now retain the correct two-slot representation and
 *   regenerated comment, so listings show both operands and the correct
 *   destination/source description.
 *
 * - v7.31.1: BUGFIX in to8_peephole_mov() - removed the overly
 *   conservative !cur->is_target guard that silently blocked the
 *   LD/ST -> MOV fusion (and the LD X;ST X redundant-store removal)
 *   on every loop header, since a do-while-style loop's back-edge
 *   target always lands on the FIRST instruction of the loop body -
 *   exactly the cur position this pass wants to fuse. Confirmed via
 *   a controlled test: reverting the unrelated -g source-line-marker
 *   changes made NO difference, isolating the real cause to this
 *   single guard, unrelated to the source-marker investigation of
 *   v7.30.0/v7.31.0.
 *   Fix: jump targets are resolved by id via to8_by_id(), never by
 *   opcode or content, so a label attached to cur's id stays valid
 *   no matter what instruction ends up AT that id. Same precedent
 *   already established by to8_peephole_ld_deref() in v7.24.0 -
 *   fuse IN PLACE on cur (reusing its id/is_target) instead of
 *   allocating a new line and discarding cur:
 *     - LD A;ST B, A!=B -> cur becomes MOV B,A directly, nxt (the ST)
 *       is unlinked. cur's label, if any, now correctly marks the
 *       MOV instead of the LD it replaced.
 *     - LD X;ST X (pure redundant self-store) -> if cur is a jump
 *       target it cannot simply be deleted (nothing would be left
 *       to land on), so cur is turned into a harmless OP_NOP
 *       placeholder instead, and only nxt is unlinked. Still saves
 *       one instruction (the ST) even in the labeled case, rather
 *       than skipping the optimization entirely.
 *   !nxt->is_target is UNCHANGED and remains required: a jump landing
 *   directly on the ST (skipping the LD) relies on R0 already holding
 *   the right value at that entry point - fusing away nxt in that
 *   case would leave the jump with nowhere valid to land.
 *   Net effect: MOV fusion and redundant-store elimination now fire
 *   correctly on loop headers (while/do-while bodies), a very common
 *   shape (_f, _g, _copy, _puts all have this exact pattern) that was
 *   silently missing this optimization since to8_peephole_mov() was
 *   introduced.
 *
 * - v7.31.0: new source-line comment markers, gated behind -g. When the
 *   user passes -g, to8_maybe_emit_source_line() - called from
 *   to8_append() (the single choke point for every emitted instruction) -
 *   prints "; L<n>: <source text>" whenever file->line_num changes since
 *   the last marker, reusing the EXISTING to8_emit_raw_asm() raw-text
 *   passthrough (no new rendering code). Without -g, behavior is
 *   UNCHANGED - listings look exactly as before this feature existed.
 *   Deliberately reuses tcc_state->do_debug rather than introducing a
 *   new backend-specific command-line flag: source-line correlation is
 *   conceptually debug info, and -g is the standard, already-documented
 *   way to ask a compiler for it.
 *   Source text is read fresh from disk on each line change (fopen+
 *   fgets, no caching) - this is a debug listing tool, not a hot path,
 *   so simplicity was chosen over performance. Tracking resets per
 *   function (gfunc_prolog) so the first statement of a new function
 *   always gets its own marker.
 *   Known limitations, all accepted: one marker per physical source
 *   line (not per C statement); macro-expanded lines report the macro
 *   USE site; a moved/deleted source file degrades gracefully to
 *   "; L<n>" with no text, never a hard error.
 *
 * - v7.30.0: new peephole to8_peephole_jump_inversion(). Collapses the
 *   common "JCC label1 ; JMP label2 ; label1:" pattern into a single
 *   inverted branch "J!CC label2", dropping the now-redundant JMP and
 *   its target label. Found while reviewing __qsort()'s partition/
 *   recursion listing (2026-08-19): every early-return-style "if
 *   (cond) goto A; goto B; A:" shape TCC emits for compound
 *   conditions and loop exits paid for an extra unconditional jump
 *   that was never needed - the SAME control-flow decision can always
 *   be expressed as a single complemented conditional jump straight
 *   to B, since "goto A" immediately falling through to "A:" is a
 *   no-op by construction. Implemented via to8_invert_jcc(), a
 *   straight table of the six JEQ/JNE/JLT/JGT/JLE/JGE complements
 *   (no signedness ambiguity - the SAME opcode already encodes
 *   signed vs unsigned via the OP_CMP/OP_CMPU choice that produced
 *   the flag-less R0 sign check upstream, so inversion is always
 *   exact, never approximate). Only fires when the JCC's target is
 *   EXACTLY the instruction right after the JMP (the "label1"
 *   immediately following, not some other unrelated jump-to-same-
 *   place coincidence), so no dataflow analysis is needed - purely
 *   structural, unconditionally safe. Leaves "label1:" itself in
 *   place (it may still be a target of OTHER jumps elsewhere in the
 *   function) - dead, unreferenced labels are harmless and left for
 *   a future dedicated dead-label cleanup pass, not this one's job.
 *   Expected to save 1 instruction per such pattern in __qsort() and
 *   similar branch-heavy functions; no effect on straight-line code
 *   like copy()/swap().
 *
 * - v7.29.1: fix to8_peephole_useless_ld() to correctly handle ST1/ST2/ST4.
 *  These opcodes write R0 to memory via a pointer in the slot argument,
 *  but do NOT read R0 - so R0's value is preserved. However, the slot
 *  argument holds the ADDRESS, not the value, so we must NOT update
 *  r0_holds_slot with it. This allows removing redundant LDs after
 *  indirect stores, saving 1 instruction on copy() R0-only (21 -> 20).
 *
 * - v7.29.0: cleanup + new peephole to8_peephole_dead_r0_load().
 *   1) Removed all temporary fprintf(stderr, ...) instrumentation added
 *      during the 2026-08-18 swap() debugging session (LOAD entry,
 *      LOAD deref, BACKEND store, STORE about to materialize, INDIRECT
 *      store [fr==R0]/[fr!=R0] traces). The fix they were built to
 *      prove stays in place - only the debug output is gone.
 *
 *   2) CORRECTNESS FIX in store()'s "v->r & VT_LVAL" branch, case
 *      fr == TREG_R0: the previous 3-instruction dance (ST value_slot ;
 *      LD fc ; stop value_slot) had the ST1/ST2/ST4 operands INVERTED -
 *      it left the ADDRESS in R0 and the VALUE in the slot argument,
 *      exactly backwards from the documented ISA semantics
 *      ("*(char*)slot = R0" - address in slot, value in R0). Confirmed
 *      by an instrumented trace on swap(): the generated ST1 would have
 *      written the destination address, truncated to a byte, THROUGH a
 *      bogus "pointer" built from the actual byte value being written.
 *      Fixed: R0 already holds the VALUE (load(r, vtop) placed it there
 *      just above) and fc already holds the ADDRESS (save_reg_upstack's
 *      reliable spill) - the whole dance collapses to a single
 *      "e_op_slot(stop, fc)", no temp needed. swap() drops from 41 to
 *      37 instr (no -O) / 30 instr (-O).
 *
 *   3) New peephole to8_peephole_dead_r0_load(): removes a dead integer
 *      load (LD/LD1/LDU1/LD2/LDU2/LD4, slot form) when a later R0-
 *      writing load of the same family is reached with no intervening
 *      read of R0 and no jump target crossed. Uses a WHITELIST of
 *      opcodes provably transparent to R0 (MOV, NOP, ADJi, PUSH/PUSHi,
 *      and the whole F0-only float family) - any opcode NOT in this
 *      list stops the scan, so omitting a transparent one only loses
 *      an optimization, never changes semantics. Catches the residual
 *      "LD fc ; LD src_slot ; ST1/ST2/ST4 ..." pattern left by point 2
 *      above, where the first LD (reloading the destination address)
 *      is immediately made moot by the very next LD (reloading the
 *      source value). Runs under -O only (peephole passes are gated by
 *      tcc_state->optimize); expected to remove 2 more instructions
 *      from swap() -O (one per pointer assignment in the loop body).
 *
 * - v7.28.0: CRITICAL BUGFIX in to8_spill_and_reload().
 *   The old version reloaded the left operand from a potentially wrong
 *   slot (c1), causing miscompilation of expressions like a*b + c*d.
 *   Example bug: mul_complex2(a,b,c,d) computed (a*b) + ((a*b)*c) instead
 *   of (a*b) + (c*d). Fixed by requiring callers to pass the EXACT left
 *   operand slot, and asserting 4-alignment before use.
 *   Affects gen_opi() for all commutative ops (ADD, MUL, AND, OR, XOR)
 *   and CMP/CMPU. Thanks to extensive testing with mul_complex2 test case.
 *
 * - v7.27.0: new peephole to8_peephole_useless_ld() replaces the old
 *   to8_peephole_dead_ld(). Removes redundant LD slot when R0 already
 *   holds 'slot' (after prior LD/ST of same slot). Unlike the old version,
 *   uses conservative forward scan: only LD/ST preserve tracking; all other
 *   instructions (arithmetic, MOV to tracked slot, labels) invalidate it.
 *
 * - v7.26.0: fuse "LD slotA ; OP slotB" into "OP2 slotA,slotB"
 *
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

#define NB_REGS 2
#define NB_ASM_REGS 0
#define CONFIG_TCC_ASM

#define RC_R0    0x0001
#define RC_FLOAT 0x0004
#define RC_INT   RC_R0
#define RC_IRET  RC_R0
#define RC_FRET  RC_FLOAT

enum {
    TREG_R0 = 0,
    TREG_F0 = 1,
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

#define TO8_GEN_VERSION "8.5.0"

/* must be defined before gfunc_prolog/epilog call them */
ST_FUNC void gen_bounds_prolog(void) {}
ST_FUNC void gen_bounds_epilog(void) {}

#define USING_GLOBALS
#include "tcc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

ST_DATA const char * const target_machine_defs =
    "__TO8__\0"
    "__BIG_ENDIAN__\0"
    "__STDC_NO_VLA__\0";

ST_DATA const int reg_classes[NB_REGS] = {
    RC_R0,
    RC_FLOAT,
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

    OP_FTOI,  /* R0 = (int)F0 */
    OP_ITOF,  /* F0 = (float/double)R0 */

    OP_FMOV,  /* G = F (register-to-register transfer, no memory access) */

    OP_LDFi,  /* F = <float literal>, baked directly into the instruction */
    OP_LDGi,  /* G = <float literal>, baked directly into the instruction */
    OP_LDF4,  /* F = slot (own-slot value, as float, 4 bytes) */
    OP_LDG4,  /* G = slot (own-slot value, as float, 4 bytes) */
    OP_LDF8,  /* F = slot (own-slot value, as double, 8 bytes; narrowed) */
    OP_LDG8,  /* G = slot (own-slot value, as double, 8 bytes; narrowed) */
    OP_LDF4m, /* F = *(float*)slot-or-symbol */
    OP_LDG4m, /* G = *(float*)slot-or-symbol */
    OP_LDF8m, /* F = *(double*)slot-or-symbol (narrowed) */
    OP_LDG8m, /* G = *(double*)slot-or-symbol (narrowed) */
    
    OP_STF4,  /* slot = F (own-slot value, as float) */
    OP_STF8,  /* slot = F (own-slot value, as double; widened) */
    OP_STF4m, /* *(float*)slot-or-symbol = F */
    OP_STF8m, /* *(double*)slot-or-symbol = F (widened) */
    
    /* opertions */
    OP_FADD,     /* F = G + F */
    OP_FSUB,     /* F = G - F */
    OP_FMUL,     /* F = G * F */
    OP_FDIV,     /* F = G / F */
    OP_FSCALEi,  /* F = F * 2^n */
    OP_FCMP,     /* R0 = sign(G - F) (F and G both survive) */
    OP_FSGN,     /* R0 = sign(F) (F survives; reserved, not yet emitted) */

    /* stack */
    OP_ADJi,  /* SP += n if n<0 => allocate otherwise release */
    OP_PUSH,  /* push slot's value onto the call-argument stack */
    OP_PUSHi, /* push an immediate onto the call-argument stack */
    OP_PUSHr, /* push R0 onto the call-argument stack */

    /*  call */
    OP_RET,   /* return from the current function */
    OP_JSR,   /* call the function pointer held in slot */
    OP_JSRi,  /* call the function named by symbol */
    OP_JSRr,  /* call the function pointer held in R0 */

    /* jump */
    OP_JMP,   /* unconditional: goto target */
    OP_JEQ,   /* if (R0 == 0) goto target */
    OP_JNE,   /* if (R0 != 0) goto target */
    OP_JLT,   /* if (R0 < 0) goto target */
    OP_JGT,   /* if (R0 > 0) goto target */
    OP_JLE,   /* if (R0 <= 0) goto target */
    OP_JGE,   /* if (R0 >= 0) goto target */
    
    /* OP2 */
    OP_ADD2,   /* R0 = slotA + slotB   (fusion de "LD slotA ; ADD slotB") */
    OP_SUB2,   /* R0 = slotA - slotB */
    OP_MUL2,   /* R0 = slotA * slotB */
    OP_AND2,   /* R0 = slotA & slotB */
    OP_OR2,    /* R0 = slotA | slotB */
    OP_XOR2,   /* R0 = slotA ^ slotB */
    OP_CMP2,   /* R0 = sign(slotA - slotB), SIGNED */
    OP_CMPU2,  /* R0 = sign(slotA -u slotB), UNSIGNED */
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
 
    case OP_EXT1S: return "EXT1S"; case OP_EXT2S: return "EXT2S";
    case OP_EXT1U: return "EXT1U"; case OP_EXT2U: return "EXT2U";
    case OP_ITOF: return "ITOF"; case OP_FTOI: return "FTOI";

    case OP_LDFi: return "LDFi"; case OP_LDGi: return "LDGi";
    case OP_LDF4: return "LDF4"; case OP_LDG4: return "LDG4";
    case OP_LDF8: return "LDF8"; case OP_LDG8: return "LDG8";
    case OP_LDF4m: return "LDF4m"; case OP_LDG4m: return "LDG4m";
    case OP_LDF8m: return "LDF8m"; case OP_LDG8m: return "LDG8m";
    case OP_STF4: return "STF4"; case OP_STF8: return "STF8";
    case OP_STF4m: return "STF4m"; case OP_STF8m: return "STF8m";
    
    case OP_FMOV: return "FMOV";
    case OP_FSCALEi: return "FSCALEi";
    case OP_FADD: return "FADD"; case OP_FSUB: return "FSUB";
    case OP_FMUL: return "FMUL"; case OP_FDIV: return "FDIV";  
    case OP_FCMP: return "FCMP"; case OP_FSGN: return "FSGN";

    case OP_PUSH: return "PUSH"; case OP_PUSHi: return "PUSHi"; case OP_PUSHr: return "PUSHr";
    case OP_JSR: return "JSR"; case OP_JSRi: return "JSRi"; case OP_JSRr: return "JSRr";
    case OP_JMP: return "JMP"; case OP_JEQ: return "JEQ"; case OP_JNE: return "JNE";
    case OP_JLT: return "JLT"; case OP_JGT: return "JGT"; case OP_JLE: return "JLE"; case OP_JGE: return "JGE";
    
    case OP_MOV:   return "MOV";
    case OP_ADD2:  return "ADD2";
    case OP_SUB2:  return "SUB2";
    case OP_MUL2:  return "MUL2";
    case OP_AND2:  return "AND2";
    case OP_OR2:   return "OR2";
    case OP_XOR2:  return "XOR2";
    case OP_CMP2:  return "CMP2";
    case OP_CMPU2: return "CMPU2";
    }
    return "?";
}

/* ===================================================================
 * Temp-slot management: a strictly-monotonic base allocator (used for
 * genuinely function-lifetime slots plus a size-keyed LIFO free-list
 * pool for ephemeral temps that this backend can PROVE are dead right
 * after their next use.
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
    int src_line; 
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

    /*
     * Store source information on the real instruction itself.
     *
     * file may still point at the ":asm:" pseudo-BufferedFile opened by
     * tcc_assemble_inline() when this line is created from inline asm
     * text (see asm_opcode() in to8-stubs.c) - that buffer's line_num
     * starts fresh at 1 and has nothing to do with the real source line
     * of the asm() call. Walk file->prev past any such pseudo-buffers to
     * reach the real enclosing file before reading line_num, exactly
     * like asm_opcode() already does for the SAME reason (see its
     * v7.20.0 changelog entry).
     */
    if (tcc_state && tcc_state->do_debug && g_in_function && file) {
        BufferedFile *bf = file;
        while (bf && strcmp(bf->filename, ":asm:") == 0)
            bf = bf->prev;
        if (bf)
            ln->src_line = bf->line_num;
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
/*
 * Render one 16-bit word as decimal or $hex, whichever produces the
 * SHORTER text - not a fixed magnitude threshold. Both candidate
 * strings are actually formatted and their lengths compared, so this
 * stays correct regardless of how many hex digits get printed (no
 * zero-padding assumed) or how the negative/decimal side behaves.
 * Ties go to decimal (equally short, but more readable at a glance).
 */
static void out_int16(int v)
{
    char dec[8], hex[8];
    unsigned short uv = (unsigned short)v;   /* two's complement for hex */
    int dec_len, hex_len;

    snprintf(dec, sizeof dec, "%d", v);
    snprintf(hex, sizeof hex, "$%x", uv);

    dec_len = (int)strlen(dec);
    hex_len = (int)strlen(hex);

    out_str(hex_len < dec_len || hex_len == dec_len && (uv&0x8000) ? hex : dec);
}
/* render little-endian float with 2 big-endian words */
static void out_float(float v) {
    unsigned char *t = (unsigned char*)&v;
    out_int16(t[0]*256+t[1]); out_char(',');
    out_int16(t[2]*256+t[3]);
}
/* render little-endian double with 4 big-endian words */
static void out_double(double v) {
    unsigned char *t = (unsigned char*)&v;
    out_int16(t[0]*256+t[1]); out_char(',');
    out_int16(t[2]*256+t[3]); out_char(',');
    out_int16(t[4]*256+t[5]); out_char(',');
    out_int16(t[6]*256+t[7]);
}	
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
 * Source-line comment markers.
 *
 * Prints the current C source line right before the block of asm
 * instructions it produced, e.g.:
 *
 *   ; L6: *d = *s;
 *           MOV     4,16
 *           LDU1    12
 *           ...
 *
 * Design: track the last line number we already marked. At the
 * single choke point to8_append(), before creating any real
 * instruction, check if file->line_num changed. If so, read that
 * line straight from the source file (no caching - this is a debug
 * listing tool, not a hot path) and emit it as a raw-text line,
 * reusing the SAME mechanism already used for asm() passthrough.
 *
 * Limitations (accepted, not bugs):
 *   - one marker per PHYSICAL line, not per C statement (several
 *     asm blocks on the same source line only get the first marker)
 *   - macro-expanded code reports the line of the macro USE
 *   - if the source file can't be reopened (moved/deleted since
 *     compilation started), markers silently show just "; L<n>"
 *     with no text - never a hard error
 * =================================================================== */

static int g_last_printed_line = -1;

/* Reads physical line `line_num` (1-based) from `filename` into
 * `out` (leading whitespace trimmed, trailing newline stripped).
 * Returns 1 on success, 0 if the file could not be opened or the
 * line does not exist (out is left as an empty string in that case -
 * callers must never treat this as a fatal error). */
static int to8_read_source_line(const char *filename, int line_num,
                                 char *out, size_t outsz)
{
    FILE *f;
    char buf[512];
    int n = 0;

    out[0] = 0;
    if (line_num < 1 || !filename)
        return 0;

    f = fopen(filename, "r");
    if (!f)
        return 0;

    while (fgets(buf, sizeof buf, f)) {
        n++;
        if (n == line_num) {
            size_t len = strlen(buf);
            const char *p = buf;

            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
                buf[--len] = 0;
            while (*p == ' ' || *p == '\t')
                p++;

            snprintf(out, outsz, "%s", p);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

/* ===================================================================
 * Code to obtain real param/argument name
 * ===================================================================*/

#define TO8_SLOT_NAME_CACHE_SIZE 32

typedef struct to8_slot_cache_entry {
    struct to8_slot_cache_entry *prev;
    struct to8_slot_cache_entry *next;
    int slot;
    char name[64];
    bool in_use;     /* pool slot currently holds a live entry */
    bool has_name;   /* true = name[] is valid, false = cached "no name found" */
} to8_slot_cache_entry;

typedef struct to8_slot_cache {
    to8_slot_cache_entry pool[TO8_SLOT_NAME_CACHE_SIZE];
    to8_slot_cache_entry *head;   /* most recently used */
    to8_slot_cache_entry *tail;   /* least recently used */
    int count;                    /* number of pool slots currently in use */
} to8_slot_cache;

static to8_slot_cache g_slot_cache;

static void to8_slot_cache_unlink(to8_slot_cache *c, to8_slot_cache_entry *e)
{
    if (e->prev) e->prev->next = e->next; else c->head = e->next;
    if (e->next) e->next->prev = e->prev; else c->tail = e->prev;
}

static void to8_slot_cache_push_front(to8_slot_cache *c, to8_slot_cache_entry *e)
{
    e->prev = NULL;
    e->next = c->head;
    if (c->head)
        c->head->prev = e;
    c->head = e;
    if (!c->tail)
        c->tail = e;
}

static void to8_slot_cache_reset(to8_slot_cache *c)
{
    int i;
    for (i = 0; i < TO8_SLOT_NAME_CACHE_SIZE; i++) {
        c->pool[i].in_use = false;
        c->pool[i].has_name = false;
        c->pool[i].prev = NULL;
        c->pool[i].next = NULL;
    }
    c->head = NULL;
    c->tail = NULL;
    c->count = 0;
}

static to8_slot_cache_entry *to8_slot_cache_find(to8_slot_cache *c, int slot)
{
    to8_slot_cache_entry *e;

    for (e = c->head; e; e = e->next) {
        if (e->slot == slot) {
            if (e != c->head) {
                to8_slot_cache_unlink(c, e);
                to8_slot_cache_push_front(c, e);
            }
            return e;
        }
    }
    return NULL;
}

static to8_slot_cache_entry *to8_slot_cache_acquire(to8_slot_cache *c)
{
    to8_slot_cache_entry *e;

    if (c->count < TO8_SLOT_NAME_CACHE_SIZE) {
        e = &c->pool[c->count++];
    } else {
        e = c->tail;
        to8_slot_cache_unlink(c, e);
    }
    e->in_use = true;
    return e;
}

static void to8_slot_cache_insert(to8_slot_cache *c, int slot, const char *name)
{
    to8_slot_cache_entry *e = to8_slot_cache_acquire(c);

    e->slot = slot;
    if (name && name[0]) {
        snprintf(e->name, sizeof e->name, "%s", name);
        e->has_name = true;
    } else {
        e->name[0] = '\0';
        e->has_name = false;
    }
    to8_slot_cache_push_front(c, e);
}

/*
 * Look up slot -> variable name, through an LRU cache backed by a
 * linear local_stack scan on miss. Debug-listing helper, not a hot
 * path - the fallback scan is O(n) in the number of active locals,
 * but rendering accesses are highly localized, so a modest cache
 * absorbs almost all repeat lookups. No cap on the number of
 * DISTINCT variables a function can have - only cache hit rate
 * varies with locality, never coverage.
 */
static const char *to8_slot_name(int slot)
{
    to8_slot_cache_entry *e;
    Sym *s;
    const char *found_name;

    /*
     * Variable-name recovery is a debug-info feature like the
     * source-line markers - gated behind -g so it costs nothing
     * (no local_stack scan, no cache lookup) unless the user asked
     * for it. Without -g, listings render exactly as before this
     * feature existed: "local[N]" / "param[N]".
     */
    if (!tcc_state || !tcc_state->do_debug)
        return NULL;
	
    e = to8_slot_cache_find(&g_slot_cache, slot);
    if (e)
        return e->has_name ? e->name : NULL;

    found_name = NULL;
    for (s = local_stack; s; s = s->prev) {
        if ((s->r & VT_VALMASK) != VT_LOCAL && (s->r & VT_VALMASK) != VT_LLOCAL)
            continue;
        if (!s->v || s->v >= SYM_FIRST_ANOM)
            continue;
        if (s->c != slot)
            continue;
        found_name = get_tok_str(s->v, NULL);
        break;
    }

    to8_slot_cache_insert(&g_slot_cache, slot, found_name);

    return found_name && found_name[0] ? found_name : NULL;
}

/* ===================================================================
 * Comment templates - switch on the enum, computed eagerly.
 * =================================================================== */

static void slot_desc(char *out, size_t outsz, int slot)
{
    const char *name = to8_slot_name(slot);

    if (name) {
        snprintf(out, outsz, "%s", name);
    } else if (slot < 0) {
        snprintf(out, outsz, "local[%d]", (-slot) / TO8_STACK_ALIGN - 1);
    } else {
        snprintf(out, outsz, "param[%d]", slot / TO8_STACK_ALIGN - 1);
    }
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
    case OP_LDF4: case OP_LDF8: snprintf(out, outsz, "F = %s", desc); return;
    case OP_LDG4: case OP_LDG8: snprintf(out, outsz, "G = %s", desc); return;
    case OP_LDF4m: case OP_LDF8m: snprintf(out, outsz, "F = *%s", desc); return;
    case OP_LDG4m: case OP_LDG8m: snprintf(out, outsz, "G = *%s", desc); return;
    case OP_STF4: case OP_STF8: snprintf(out, outsz, "%s = F", desc); return;
    case OP_STF4m: case OP_STF8m: snprintf(out, outsz, "*%s = F", desc); return;
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
    case OP_FSCALEi: snprintf(out, outsz, "F *= 2^%s", num); return;
    default: snprintf(out, outsz, "%s", num); return;
    }
}

static void addr_comment(char *out, size_t outsz, to8_opcode op, const char *desc)
{
    switch (op) {
    case OP_LDi: snprintf(out, outsz, "R0 = &%s", desc); return;
    case OP_JSRi: snprintf(out, outsz, "call %s", desc); return;
    case OP_LDF4m: case OP_LDF8m: snprintf(out, outsz, "F = %s", desc); return;
    case OP_LDG4m: case OP_LDG8m: snprintf(out, outsz, "G = %s", desc); return;
    case OP_STF4m: case OP_STF8m: snprintf(out, outsz, "%s = F", desc); return;
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
    case OP_FMOV: return "G = F";
    case OP_FADD: return "F = G + F";
    case OP_FSUB: return "F = G - F";
    case OP_FMUL: return "F = G * F";
    case OP_FDIV: return "F = G / F";
    case OP_FCMP: return "R0 = sign(G - F)";
    case OP_FSGN: return "R0 = sign(F)";
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

static void op2_comment(char *out, size_t outsz, to8_opcode op2,
                         const char *desc_a, const char *desc_b)
{
    switch (op2) {
    case OP_ADD2:  snprintf(out, outsz, "R0 = %s + %s", desc_a, desc_b); return;
    case OP_SUB2:  snprintf(out, outsz, "R0 = %s - %s", desc_a, desc_b); return;
    case OP_MUL2:  snprintf(out, outsz, "R0 = %s * %s", desc_a, desc_b); return;
    case OP_AND2:  snprintf(out, outsz, "R0 = %s & %s", desc_a, desc_b); return;
    case OP_OR2:   snprintf(out, outsz, "R0 = %s | %s", desc_a, desc_b); return;
    case OP_XOR2:  snprintf(out, outsz, "R0 = %s ^ %s", desc_a, desc_b); return;
    case OP_CMP2:  snprintf(out, outsz, "R0 = sign(%s - %s)", desc_a, desc_b); return;
    case OP_CMPU2: snprintf(out, outsz, "R0 = sign(%s -u %s)", desc_a, desc_b); return;
    default:       snprintf(out, outsz, "%s , %s", desc_a, desc_b); return;
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
    snprintf(ln->comment, sizeof ln->comment, "%s = %g",
             (op == OP_LDGi) ? "G" : "F", v);
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

static void e_push_addr(Sym *sym, int c)
{
    to8_line *ln = to8_append(OP_PUSHi);
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
    snprintf(ln->comment, sizeof ln->comment, "push %s", desc);
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

static to8_opcode to8_op2_variant(to8_opcode op1)
{
    switch (op1) {
    case OP_ADD:  return OP_ADD2;
    case OP_SUB:  return OP_SUB2;
    case OP_MUL:  return OP_MUL2;
    case OP_AND:  return OP_AND2;
    case OP_OR:   return OP_OR2;
    case OP_XOR:  return OP_XOR2;
    case OP_CMP:  return OP_CMP2;
    case OP_CMPU: return OP_CMPU2;
    /* OP_DIV / OP_MOD deliberement exclus : la division a une
     * semantique plus lourde (piege sur division par zero, quotient
     * ET reste couples sur la future implementation 6809 reelle) -
     * pas de gain net a fusionner ce cas, contrairement a une simple
     * elimination de "LD mort". */
    default: return OP_NOP;
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
/* Loads a float/double SValue's addressing mode (VT_LOCAL / VT_LLOCAL /
 * VT_CONST) into either F (want_g=0) or G (want_g=1). Shared by
 * load()'s TREG_F0 branch and gen_opf()'s left-operand load - the
 * "not VT_LVAL" fallback (symbol/local address-of quirk, and the
 * OP_ITOF int->float conversion path) only ever applies to F: G is
 * populated exclusively from a genuine memory value, since gen_opf()
 * only ever calls this on an already float-typed operand's location,
 * never a bare register/conversion case. */
static void to8_load_fg(SValue *sv, int bt, int want_g)
{
    int v = sv->r & VT_VALMASK;
    int fc = sv->c.i;
    to8_opcode own   = want_g ? (bt == VT_FLOAT ? OP_LDG4  : OP_LDG8)
                               : (bt == VT_FLOAT ? OP_LDF4  : OP_LDF8);
    to8_opcode deref = want_g ? (bt == VT_FLOAT ? OP_LDG4m : OP_LDG8m)
                               : (bt == VT_FLOAT ? OP_LDF4m : OP_LDF8m);
    to8_opcode imm   = want_g ? OP_LDGi : OP_LDFi;

    if (sv->r & VT_LVAL) {
        if (v == VT_LOCAL) {
            e_op_slot(own, fc);
        } else if (v == VT_LLOCAL) {
            e_op_slot(deref, fc);
        } else if (v == VT_CONST) {
            if (bt == VT_FLOAT) {
                double fval;
                if (to8_try_read_float_const(sv->sym, bt, &fval))
                    e_op_fimm(imm, fval);
                else
                    e_op_addr(deref, sv->sym, fc);
            } else {
                e_op_addr(deref, sv->sym, fc);
            }
        } else if (v < VT_CONST) {
            int temp = to8_temp_alloc(4, 4);
            e_op_slot(OP_ST, temp);
            e_op_slot(deref, temp);
            to8_temp_free(temp, 4, 4);
        } else {
            tcc_error("TO8: unsupported float load addressing mode (v=%#x)", v);
        }
    } else if (v == VT_CONST) {
        /* Bare compile-time constant, NO VT_LVAL: TCC carries the
         * literal directly in sv->c, no data-section symbol involved
         * at all - a different case from VT_LVAL+VT_CONST above.
         * Narrowed to float precision either way - this backend's
         * F/G registers are 32-bit only, same end result a memory-
         * backed double constant would get on load. */
        double fval = (bt == VT_FLOAT) ? (double)sv->c.f : sv->c.d;
        e_op_fimm(imm, fval);
    } else if (!want_g) {
        if (v == VT_LOCAL)
            e_op_slot(own, fc);
        else
            e_op(OP_ITOF);
    } else {
        tcc_error("TO8: internal error - to8_load_fg(want_g=1) called "
                  "on an unsupported non-lvalue operand (v=%#x)", v);
    }
}

void load(int r, SValue *sv)
{
    int v, ft, fc, bt;
    int is_unsigned;
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
            to8_load_fg(sv, bt, 0);
        } else {
            tcc_error("TO8: internal error - load(TREG_F0, ...) called with "
                      "non-float/double type (bt=%#x)", bt);
        }
        return;
    }

    if (!(sv->r & VT_LVAL) && v < TREG_MEM && v != VT_CONST && v != VT_LOCAL &&
        v != VT_LLOCAL && v != VT_CMP && v != VT_JMP && v != VT_JMPI) {
        if (v == r) return;
    }

    if (sv->r & VT_LVAL) {
        if (v == VT_LOCAL) {
            e_op_slot(OP_LD, fc);
        } else if (v == VT_LLOCAL) {
            e_op_slot(ldop, fc);
        } else if (v == VT_CONST) {
            e_op_addr(ldop, sv->sym, fc);
        } else if (v == TREG_R0) {
            e_op(to8_byte_suffix_ld_r(bt, is_unsigned));
        } else if (v < VT_CONST) {
            int temp = to8_temp_alloc(4, 4);
            e_op_slot(OP_ST, temp);
            e_op_slot(ldop, temp);
            to8_temp_free(temp, 4, 4);
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
            if (v == r) return;
            if (v >= TREG_MEM) {
                e_op_slot(ldop, fc);
            }
            break;
        }
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
        to8_opcode st = (bt == VT_FLOAT) ? OP_STF4 : OP_STF8;
        to8_opcode stm = (bt == VT_FLOAT) ? OP_STF4m : OP_STF8m;
        if (fr == VT_LOCAL) {
            e_op_slot(st, fc);
        } else if (fr == VT_LLOCAL) {
            e_op_slot(stm, fc);
        } else if (fr == VT_CONST) {
            e_op_addr(stm, v->sym, fc);
        } else if (fr < VT_CONST) {
            int temp = to8_temp_alloc(4, 4);
            e_op_slot(OP_ST, temp);
            e_op_slot(stm, temp);
            to8_temp_free(temp, 4, 4);
        } else {
            tcc_error("TO8: unsupported float store addressing mode (fr=%#x)", fr);
        }
        return;
    }

    if (fr == VT_LOCAL) {
        e_op_slot(OP_ST, fc);
    } else if (fr == VT_LLOCAL) {
        e_op_slot(stop, fc);
    } else if (fr == VT_CONST && (v->r & VT_SYM)) {
        e_op_addr(stop, v->sym, fc);
    } else if (v->r & VT_LVAL) {
        /*
         * L'adresse de destination est representee par `fr`.
         *
         * Cas critique a un seul registre :
         * si fr == TREG_R0, R0 contenait l'adresse a l'entree,
         * mais contient maintenant la valeur a ecrire apres load(r, vtop).
         * fc est le slot ou TCC a spillee cette adresse.
         */
        if (fr == TREG_R0) {
            /*
             * R0 contient déjà la VALEUR à écrire (load(r, vtop) l'y a placée
             * ci-dessus). fc contient déjà l'ADRESSE destination (backup fiable
             * fait par save_reg_upstack avant que R0 ne soit réutilisé).
             * Sémantique ST1/ST2/ST4 : *(type*)slot_arg = R0.
             * R0 et fc sont donc DÉJÀ dans la position correcte - aucun temp
             * n'est nécessaire, contrairement à la version précédente qui
             * inversait valeur et adresse.
             */
            e_op_slot(stop, fc);
        } else {
            int addr_slot = to8_temp_alloc(4, 4);
            e_op_slot(OP_ST, addr_slot);
        
            e_op_slot(stop, addr_slot);
        
            to8_temp_free(addr_slot, 4, 4);
        }
    }
}

/* ===================================================================
 * Binary integer operations
 * =================================================================== */

/* ===================================================================
 * to8_spill_and_reload - spill R0, then reload the LEFT operand.
 *
 * FIXED (v7.27.1): the old version reloaded from 'c1' even when c1
 * was not the left operand's slot, causing wrong code like:
 *   ST 8       ; save a*b
 *   ST 4       ; save d
 *   LD 8       ; BUG: should reload d (slot 4), not a*b (slot 8)
 *   MUL 32     ; R0 *= c
 *
 * The fix: callers must pass the EXACT slot of the left operand.
 * This function now asserts that 'left_slot' is 4-aligned and uses it
 * directly, instead of reusing a potentially wrong 'c1' temp.
 * =================================================================== */
static int to8_spill_and_reload(int left_slot)
{
    int temp = to8_temp_alloc(4, 4);

    /* Spill current R0 (the right operand) into a fresh temp. */
    e_op_slot(OP_ST, temp);

    /* Reload the LEFT operand from its known slot. */
    e_op_slot(OP_LD, left_slot);

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
        /* v7.28.1 fix: pass the EXACT left operand slot, handling TREG_R1 too */
        int left_slot = c1;
        temp = to8_spill_and_reload(left_slot);
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
            int left_slot = c1;
            int temp = to8_spill_and_reload(left_slot);
	    e_op_slot(is_unsigned_cmp ? OP_CMPU : OP_CMP, temp);
            to8_temp_free(temp, 4, 4);
        }

        vtop--;
        vset_VT_CMP(op);
        return;
    }

    v1 = vtop[-1].r & VT_VALMASK;
    c1 = vtop[-1].c.i;
    
    /*
     * v7.39.0: capture whether the RIGHT operand (top of stack) is a
     * plain constant BEFORE gv(RC_INT) materializes it into R0 and
     * destroys that information. Needed for the fast path below, which
     * handles the extremely common "variable - constant" shape (SUB/
     * DIV/MOD with a constant right-hand side, e.g. "d - 1", "p - 1",
     * "i % 10") without spilling the constant to a temp slot first.
     *
     * The existing code only special-cased a constant LEFT operand
     * (e.g. "1 - d"); a constant RIGHT operand with SUB/DIV/MOD fell
     * through to the generic to8_spill_and_reload() dance instead,
     * wasting one instruction: LDi <const> ; ST <temp> ; LD <left> ;
     * <op> <temp> (the last two fused into <op>2 by the peephole),
     * instead of the two-instruction LD <left> ; <op>i <const>.
     */
    int v0_is_const = (vtop->r & VT_VALMASK) == VT_CONST && !(vtop->r & VT_SYM);
    int c0 = v0_is_const ? vtop->c.i : 0;
    
    if (!to8_is_commutative(op) && v0_is_const && v1 != VT_CONST) {
        to8_opcode slot_op, imm_op;
        if (to8_get_arith_ops(op, &slot_op, &imm_op) < 0) { vpop(); return; }
        vpop();                 /* drop the constant right operand */
        gv(RC_INT);             /* materialize the LEFT operand into R0 */
        e_op_imm(imm_op, c0);   /* R0 = R0 op constant, one instruction, no spill */
        vtop->r = TREG_R0;
        vtop->r2 = VT_CONST;
        return;
    }


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
            } else if (v1 == TREG_R0) {
                /* v1 is in R0: spill R0, reload left operand from its slot. */
                int temp = to8_spill_and_reload(c1);
                e_op_slot(slot_op, temp);
                to8_temp_free(temp, 4, 4);
            } else {
                e_op_slot(slot_op, c1);
            }
        } else {
            /* Non-commutative: SUB, DIV, MOD */
            if (v1 == VT_CONST) {
                /* Left operand is a constant: load it as an immediate, not a slot. */
                int temp = to8_temp_alloc(4, 4);
                e_op_slot(OP_ST, temp);      /* spill current R0 (right operand) */
                e_op_imm(OP_LDi, c1);        /* R0 = constant */
                e_op_slot(slot_op, temp);    /* R0 = const OP spilled_right */
                to8_temp_free(temp, 4, 4);
            } else {
               int temp = to8_spill_and_reload(c1);
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

/* If sv is a compile-time float/double constant - either a bare
 * literal carried directly in sv->c, or a reference to an anonymous
 * data-section symbol - returns its value in *out and 1. Returns 0
 * otherwise, examining sv for nothing beyond its addressing mode. */
static int to8_float_const_value(SValue *sv, int bt, double *out)
{
    int v = sv->r & VT_VALMASK;
    if (v != VT_CONST)
        return 0;
    if (sv->r & VT_LVAL)
        return to8_try_read_float_const(sv->sym, bt, out);
    *out = (bt == VT_FLOAT) ? (double)sv->c.f : sv->c.d;
    return 1;
}

/* v == 2^n for some integer n? Exact test via frexp(): v = m * 2^e
 * with 0.5 <= m < 1 for any normal positive v; v is an exact power
 * of two iff m == 0.5 exactly, in which case v == 2^(e-1). Negative
 * v is rejected here - a negated power of two (e.g. "-2.0 * x") is a
 * known, deliberately unhandled case for now. */
static int to8_pow2_exponent(double v, int *out_n)
{
    int e;
    double m;
    if (v <= 0.0)
        return 0;
    m = frexp(v, &e);
    if (m != 0.5)
        return 0;
    *out_n = e - 1;
    return 1;
}


void gen_opf(int op)
{
    int bt;
    int left_was_f0 = (vtop[-1].r & VT_VALMASK) == TREG_F0;
    int is_commutative = (op == '+' || op == '*');
    int is_cmp = (op == TOK_LT || op == TOK_GT || op == TOK_LE ||
                  op == TOK_GE || op == TOK_EQ || op == TOK_NE);

    /* try to use FSCALE */
    if (op == '*') {
        int n;
        double cval;

        /* Right operand is a power-of-two constant - drop it, scale
         * the left operand directly. Covers "x * 2.0". */
        bt = vtop->type.t & VT_BTYPE;
        if (to8_float_const_value(vtop, bt, &cval) && to8_pow2_exponent(cval, &n)) {
            vpop();
            gv(RC_FLOAT);
            if (n != 0) /* n==0 means "* 1" - nothing to emit at all */
                e_op_imm(OP_FSCALEi, n);
            vtop->r = TREG_F0;
            vtop->r2 = VT_CONST;
            return;
        }

        /* Left operand is a power-of-two constant - the mandel()
         * shape, "2 * x". Swap first so the SAME logic above applies
         * uniformly, then fall through the swapped positions. */
        bt = vtop[-1].type.t & VT_BTYPE;
        if (to8_float_const_value(&vtop[-1], bt, &cval) && to8_pow2_exponent(cval, &n)) {
            vswap();
            vpop();
            gv(RC_FLOAT);
            if (n != 0)
                e_op_imm(OP_FSCALEi, n);
            vtop->r = TREG_F0;
            vtop->r2 = VT_CONST;
            return;
        }
    }
    
    /*
     * Fast path: left operand already in F, and we can avoid ever
     * materializing the right operand into F at all - thanks to
     * commutativity for +/*, or a free operator inversion for
     * comparisons. This bypasses gv(RC_FLOAT) entirely for the right
     * operand, which also bypasses TCC's own save_reg()-driven spill
     * of the left operand that gv() would otherwise trigger (see
     * v8.0.1 changelog: confirmed empirically on mandel() - a
     * redundant STF4 appeared right after FMOV precisely because
     * gv() has no way to know the left operand was already safely
     * copied to G).
     */
    if (left_was_f0 && (is_commutative || is_cmp)) {
        bt = vtop->type.t & VT_BTYPE;
        to8_load_fg(vtop, bt, 1); /* G = right operand; F untouched (= left) */

        if (is_cmp) {
            e_op(OP_FCMP); /* R0 = sign(G - F) = sign(right - left) */
            vtop--;
            vset_VT_CMP(to8_swap_cmp_op(op)); /* corrects the flipped sign */
            return;
        }

        e_op(op == '+' ? OP_FADD : OP_FMUL); /* F = G op F = right op left */
        vtop--;
        vtop->r = TREG_F0;
        vtop->r2 = VT_CONST;
        return;
    }

    /* FSUB/FDIV (or a comparison whose left wasn't in F to begin with)
     * cannot use the trick above - preserve the left operand via
     * FMOV before it gets clobbered by gv() below. */
    if (left_was_f0)
        e_op(OP_FMOV);

    gv(RC_FLOAT); /* forces the RIGHT operand (vtop) into F */
    bt = vtop->type.t & VT_BTYPE;

    if (is_cmp) {
        if (!left_was_f0)
            to8_load_fg(&vtop[-1], bt, 1);
        e_op(OP_FCMP);
        vtop--;
        vset_VT_CMP(op);
        return;
    }

    {
        to8_opcode fop;
        switch (op) {
        case '+': fop = OP_FADD; break;
        case '-': fop = OP_FSUB; break;
        case '*': fop = OP_FMUL; break;
        case '/': fop = OP_FDIV; break;
        default: fop = OP_NOP; break;
        }
        if (fop != OP_NOP) {
            if (!left_was_f0)
                to8_load_fg(&vtop[-1], bt, 1);
            e_op(fop);
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
            e_push_addr(sv->sym, sv->c.i);
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
        if (cur->op == OP_LD &&
            nxt && !nxt->is_target && nxt->op == OP_ST) {
            if (cur->slot_a == nxt->slot_a &&
                cur->push_depth == nxt->push_depth) {
                /*
                 * LD X; ST X is a redundant self-store.
                 * Keep a target line alive when necessary.
                 */
                if (cur->is_target) {
                    cur->op = OP_NOP;
                    cur->kind = ARG_NONE;
                    cur->has_comment = 0;
                    to8_unlink(nxt);
                } else {
                    to8_line *scan_from = nxt->next;
                    to8_unlink(cur);
                    to8_unlink(nxt);
                    cur = scan_from;
                    changed = 1;
                    continue;
                }
            } else {
                int src_slot = cur->slot_a;
                int dst_slot = nxt->slot_a;
                char dst_desc[24];
                char src_desc[24];
            
                /*
                 * LD A; ST B becomes MOV B,A.
                 * Mutate cur in place so its id, label, and list position survive.
                 */
                cur->op = OP_MOV;
                cur->kind = ARG_SLOT2;
                cur->slot_a = dst_slot;
                cur->slot_b = src_slot;
            
                slot_desc(dst_desc, sizeof dst_desc, dst_slot);
                slot_desc(src_desc, sizeof src_desc, src_slot);
                snprintf(cur->comment, sizeof cur->comment,
                         "%s = %s", dst_desc, src_desc);
                cur->has_comment = 1;
            
                to8_unlink(nxt);
            }
            
            changed = 1;
            nxt = cur->next;
        }
        cur = nxt;
    }
    return changed;
}

static int to8_peephole_useless_ld(void)
{
    int changed = 0;
    to8_line *cur = g_head;
    int r0_holds_slot = -1;  /* -1 = unknown, otherwise R0 holds that slot */
    
    while (cur) {
        to8_line *nxt = cur->next;
        
        /* A jump target invalidates our knowledge */
        if (cur->is_target)
            r0_holds_slot = -1;
        
        /* Check for useless LD */
        if (cur->op == OP_LD && cur->kind == ARG_SLOT &&
            r0_holds_slot != -1 && cur->slot_a == r0_holds_slot) {
            to8_unlink(cur);
            changed = 1;
            cur = nxt;
            continue;
        }
        
        /* Update tracking */
        switch (cur->op) {
        case OP_LD:
            if (cur->kind == ARG_SLOT)
                r0_holds_slot = cur->slot_a;
            else
                r0_holds_slot = -1;
            break;
        case OP_ST:
            if (cur->kind == ARG_SLOT)
                r0_holds_slot = cur->slot_a;
	/* fall through */
        case OP_ST1:
        case OP_ST2:
        case OP_ST4:
            break;
        case OP_MOV:
            if (cur->kind == ARG_SLOT2 && cur->slot_a == r0_holds_slot)
                r0_holds_slot = -1;
            break;
        default:
            r0_holds_slot = -1;
            break;
        }
        
        cur = nxt;
    }
    
    return changed;
}

/*
 * Remove a dead integer load whose R0 value is overwritten later
 * without being read in between.
 *
 * The scan deliberately uses a whitelist of operations transparent to
 * R0. Any opcode not listed here stops the scan. This is conservative:
 * omitting a transparent opcode loses an optimization, but cannot change
 * program semantics.
 *
 * A load is removable only when:
 *   - it is a slot load that writes R0 (LD/LD1/LDU1/LD2/LDU2/LD4);
 *   - no instruction in between reads R0;
 *   - a later R0-writing load of the same family is encountered;
 *   - no jump target is crossed.
 *
 * ST/ST1/ST2/ST4, PUSHr, all calls and jumps, RET, arithmetic,
 * comparisons, conversions, and all register-deref loads are
 * intentionally NOT in the whitelist because they read R0 or may
 * observe/control-flow through it.
 */
static int to8_peephole_dead_r0_load(void)
{
    int changed = 0;
    to8_line *first;

    for (first = g_head; first; first = first->next) {
        to8_line *scan;

        if (first->is_target || first->kind != ARG_SLOT)
            continue;
        if (first->op != OP_LD && first->op != OP_LD1 &&
            first->op != OP_LDU1 && first->op != OP_LD2 &&
            first->op != OP_LDU2 && first->op != OP_LD4)
            continue;

        for (scan = first->next; scan; scan = scan->next) {
            if (scan->is_target)
                break;

            if (scan->op == OP_MOV || scan->op == OP_NOP ||
                scan->op == OP_ADJi || scan->op == OP_PUSH ||
                scan->op == OP_PUSHi ||
                scan->op == OP_LDFi || scan->op == OP_LDGi ||
                scan->op == OP_LDF4 || scan->op == OP_LDG4 ||
                scan->op == OP_LDF8 || scan->op == OP_LDG8 ||
                scan->op == OP_LDF4m || scan->op == OP_LDG4m ||
                scan->op == OP_LDF8m || scan->op == OP_LDG8m ||
                scan->op == OP_STF4 || scan->op == OP_STF8 ||
                scan->op == OP_STF4m || scan->op == OP_STF8m ||
                scan->op == OP_FMOV || scan->op == OP_FSCALEi ||
                scan->op == OP_FADD || scan->op == OP_FSUB ||
                scan->op == OP_FMUL || scan->op == OP_FDIV)
                continue;
            /* OP_FCMP and OP_FSGN are deliberately EXCLUDED - both write a
             * signed result into R0, exactly like the old OP_FCMP/OP_CMPG they
             * replace, so they must stop the scan like any other R0-writing
             * instruction. */

            if (scan->kind == ARG_SLOT &&
                (scan->op == OP_LD || scan->op == OP_LD1 ||
                 scan->op == OP_LDU1 || scan->op == OP_LD2 ||
                 scan->op == OP_LDU2 || scan->op == OP_LD4)) {
                to8_unlink(first);
                changed = 1;
                break;
            }

            break;
        }
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

/* ===================================================================
 * v7.26.0: to8_peephole_op2() - fusionne "LD slotA ; OP slotB" en un
 * seul "OP2 slotA,slotB" (R0 = slotA op slotB), en supprimant le LD.
 *
 * Justification: R0 est un accumulateur unique, sans autre lecteur
 * entre deux instructions consecutives. Quand un simple "LD slotA"
 * est immediatement suivi d'un "OP slotB" a un seul slot, la valeur
 * que le LD vient d'ecrire dans R0 ne sert QU'A UNE SEULE CHOSE -
 * etre l'operande gauche de ce OP - puis est immediatement ecrasee
 * par le resultat. Ce LD est donc du pur gaspillage : une valeur est
 * recopiee de la memoire vers R0 seulement pour etre relue l'instruction
 * d'apres et aussitot detruite. OP2 permet au futur generateur 6809 reel
 * de lire slotA directement dans le registre dont il a besoin, sans
 * ce detour inutile par "R0 contient une copie de slotA".
 *
 * Securite: seul `nxt` (le OP) doit etre `!is_target`, PAS `cur` (le
 * LD) - si l'execution saute directement sur `cur`, le LD s'execute
 * puis on enchaine normalement sur le OP, comme avant fusion. Mais si
 * un saut pouvait atterrir sur `nxt` (en sautant le LD), R0 contiendrait
 * autre chose a cet instant, et fusionner substituerait silencieusement
 * "slotA" comme s'il avait ete charge - donc `nxt` ne doit JAMAIS etre
 * une cible de saut. C'est exactement le meme raisonnement que
 * to8_peephole_ld_deref() (v7.24.0) plus haut.
 *
 * DIV/MOD sont exclus via to8_op2_variant() - voir son commentaire.
 * =================================================================== */
static int to8_peephole_op2(void)
{
    int changed = 0;
    to8_line *cur = g_head;

    while (cur) {
        to8_line *nxt = cur->next;

        if (cur->op == OP_LD && cur->kind == ARG_SLOT &&
            nxt && !nxt->is_target && nxt->kind == ARG_SLOT &&
            cur->push_depth == nxt->push_depth) {
            to8_opcode op2 = to8_op2_variant(nxt->op);

            if (op2 != OP_NOP) {
                char desc_a[24], desc_b[24];
                int slot_a = cur->slot_a;
                int slot_b = nxt->slot_a;

                /* on modifie `cur` en place (garde son id/position,
                 * donc toute cible de saut existante reste valide) */
                cur->op = op2;
                cur->kind = ARG_SLOT2;
                cur->slot_a = slot_a;
                cur->slot_b = slot_b;

                slot_desc(desc_a, sizeof desc_a, slot_a);
                slot_desc(desc_b, sizeof desc_b, slot_b);
                op2_comment(cur->comment, sizeof cur->comment, op2, desc_a, desc_b);
                cur->has_comment = 1;

                to8_unlink(nxt);
                changed = 1;
                cur = cur->next;
                continue;
            }
        }
        cur = nxt;
    }
    return changed;
}

static to8_opcode to8_invert_jcc(to8_opcode op)
{
    switch (op) {
    case OP_JEQ: return OP_JNE;
    case OP_JNE: return OP_JEQ;
    case OP_JLT: return OP_JGE;
    case OP_JGT: return OP_JLE;
    case OP_JLE: return OP_JGT;
    case OP_JGE: return OP_JLT;
    default: 
	tcc_error("internal error in to8_invert_jcc(%d)", op);
	return op;  /* should not happen */
    }
}

static int to8_peephole_jump_inversion(void)
{
    int changed = 0;
    to8_line *cur = g_head;
    
    while (cur) {
        to8_line *nxt = cur->next;
        
        /* Look for JCC followed by JMP */
        if (cur->op >= OP_JEQ && cur->op <= OP_JGE &&  /* JCC family */
            nxt && nxt->op == OP_JMP) {
            
            to8_line *target = to8_by_id(cur->jmp_target_id);
            to8_line *jmp_target = to8_by_id(nxt->jmp_target_id);
            
            /* Only if the JCC jumps to the instruction right after the JMP */
            if (target == nxt->next) {
                /* Complement the condition */
                to8_opcode inv = to8_invert_jcc(cur->op);
                cur->op = inv;
                cur->jmp_target_id = nxt->jmp_target_id;
                
                /* Remove the JMP */
                to8_unlink(nxt);
                changed = 1;
                
                /* If the original label is now unused, it will be cleaned
                 * up by a later pass (or left as dead code - harmless) */
            }
        }
        
        cur = cur->next;
    }
    
    return changed;
}

static int to8_same_float_load(const to8_line *a, const to8_line *b)
{
    if (!a || !b || a->op != b->op || a->kind != b->kind)
        return 0;

    switch (a->kind) {
    case ARG_FIMM:
        return a->f_val == b->f_val;

    case ARG_SLOT:
        return a->slot_a == b->slot_a &&
               a->push_depth == b->push_depth;

    case ARG_SYM:
        return a->sym == b->sym &&
               a->sym_addend == b->sym_addend;

    default:
        return 0;
    }
}

/*
 * Remove a redundant repeated load into F or G when the same register
 * still contains the identical value and no intervening instruction
 * can alter that register or make the previous value unreachable.
 *
 * This pass deliberately tracks only exact repeated loads. It does not
 * perform general floating-point liveness analysis.
 */
static int to8_peephole_useless_ldf(void)
{
    int changed = 0;
    to8_line *cur;
    to8_line *last_f_load = NULL;
    to8_line *last_g_load = NULL;

    for (cur = g_head; cur; ) {
        to8_line *next = cur->next;

        if (cur->is_target) {
            last_f_load = NULL;
            last_g_load = NULL;
        }

        if (cur->op == OP_LDFi || cur->op == OP_LDF4 ||
            cur->op == OP_LDF8 || cur->op == OP_LDF4m ||
            cur->op == OP_LDF8m) {
            if (last_f_load && to8_same_float_load(cur, last_f_load)) {
                to8_unlink(cur);
                changed = 1;
                cur = next;
                continue;
            }
            last_f_load = cur;
            cur = next;
            continue;
        }

        if (cur->op == OP_LDGi || cur->op == OP_LDG4 ||
            cur->op == OP_LDG8 || cur->op == OP_LDG4m ||
            cur->op == OP_LDG8m) {
            if (last_g_load && to8_same_float_load(cur, last_g_load)) {
                to8_unlink(cur);
                changed = 1;
                cur = next;
                continue;
            }
            last_g_load = cur;
            cur = next;
            continue;
        }

        switch (cur->op) {
        case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV:
        case OP_ITOF: case OP_FSCALEi:
            last_f_load = NULL;
            break;
        case OP_FMOV:
            last_g_load = NULL;
            break;
        case OP_JSR: case OP_JSRi: case OP_JSRr:
            last_f_load = NULL;
            last_g_load = NULL;
            break;
        default:
            break;
        }

        cur = next;
    }

    return changed;
}

/*
 * Two consecutive own-slot float stores (STF4 A ; STF4 B) write the
 * SAME F value to two different slots with no instruction in between
 * that could change F. The second store is replaced by a plain word
 * copy (MOV B,A) instead of re-running the float store - on the real
 * 6809 target, STF4 performs a register-to-IEEE conversion (see the
 * ARCHITECTURE NOTE above OP_LDF4/OP_STF4); paying that conversion
 * cost twice for an identical value is wasteful. A raw 4-byte copy of
 * already-converted bytes is unconditionally cheaper and correct.
 *
 * !nxt->is_target is required: if something jumps directly to the
 * second store, F may hold an unrelated value at that entry point,
 * and mutating it into "copy from slot A" would silently substitute
 * the wrong value - same precedent as to8_peephole_mov()/
 * to8_peephole_ld_deref().
 */
static int to8_peephole_float_store_dup(void)
{
    int changed = 0;
    to8_line *cur = g_head;

    while (cur) {
        to8_line *nxt = cur->next;

        if (cur->op == OP_STF4 && cur->kind == ARG_SLOT &&
            nxt && !nxt->is_target &&
            nxt->op == OP_STF4 && nxt->kind == ARG_SLOT &&
            cur->push_depth == nxt->push_depth &&
            cur->slot_a != nxt->slot_a) {
            char dst_desc[24], src_desc[24];

            nxt->op = OP_MOV;
            nxt->kind = ARG_SLOT2;
            nxt->slot_b = cur->slot_a; /* source = first store's slot */
            /* nxt->slot_a already holds the destination */
            slot_desc(dst_desc, sizeof dst_desc, nxt->slot_a);
            slot_desc(src_desc, sizeof src_desc, nxt->slot_b);
            snprintf(nxt->comment, sizeof nxt->comment, "%s = %s", dst_desc, src_desc);
            nxt->has_comment = 1;
            changed = 1;
        }
	
	if (cur->op == OP_STF8 && cur->kind == ARG_SLOT &&
            nxt && !nxt->is_target &&
            nxt->op == OP_STF8 && nxt->kind == ARG_SLOT &&
            cur->push_depth == nxt->push_depth &&
            cur->slot_a != nxt->slot_a) {
            to8_line *second = tcc_mallocz(sizeof(to8_line));
            char dst_desc[24], src_desc[24];
            int dst = nxt->slot_a, src = cur->slot_a;
         
            /* Mutate nxt in place into the first (low-word) MOV - same
             * technique as to8_peephole_mov()'s LD/ST fusion. */
            nxt->op = OP_MOV;
            nxt->kind = ARG_SLOT2;
            nxt->slot_b = src;
            slot_desc(dst_desc, sizeof dst_desc, dst);
            slot_desc(src_desc, sizeof src_desc, src);
            snprintf(nxt->comment, sizeof nxt->comment, "%s = %s", dst_desc, src_desc);
            nxt->has_comment = 1;
         
            /* Splice a second, freshly synthesized MOV right after it for
             * the high word - same precedent as to8_peephole_ext(): never
             * registered by id, never a jump target. */
            second->op = OP_MOV;
            second->kind = ARG_SLOT2;
            second->slot_a = dst + 4;
            second->slot_b = src + 4;
            second->push_depth = nxt->push_depth;
            slot_desc(dst_desc, sizeof dst_desc, second->slot_a);
            slot_desc(src_desc, sizeof src_desc, second->slot_b);
            snprintf(second->comment, sizeof second->comment, "%s = %s", dst_desc, src_desc);
            second->has_comment = 1;
            to8_insert_after(nxt, second);
         
            changed = 1;
            nxt = second->next;
        }
         
        cur = nxt;
    }
    return changed;
}

static to8_opcode to8_g_counterpart(to8_opcode f_op)
{
    switch (f_op) {
    case OP_LDFi:  return OP_LDGi;
    case OP_LDF4:  return OP_LDG4;
    case OP_LDF8:  return OP_LDG8;
    case OP_LDF4m: return OP_LDG4m;
    case OP_LDF8m: return OP_LDG8m;
    default:       return OP_NOP; /* not an F-load opcode */
    }
}

static int to8_stops_fmov_scan(to8_opcode op)
{
    switch (op) {
    case OP_LDFi: case OP_LDF4: case OP_LDF8: case OP_LDF4m: case OP_LDF8m:
    case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FDIV:
    case OP_FSCALEi: case OP_ITOF:
    case OP_JSR: case OP_JSRi: case OP_JSRr:
        return 1;
    default:
        return 0;
    }
}


/*
 * "LDF* X ; LDG* X" (identical address/value, F loaded first) becomes
 * "LDF* X ; FMOV" - the second memory load is replaced by a register-
 * to-register copy. Adjacency (nxt == cur->next) means there is
 * nothing "in between" to invalidate F by construction - the only
 * remaining hazard is a jump landing directly on the second load,
 * hence !nxt->is_target.
 */
static int to8_peephole_fmov(void)
{
    int changed = 0;
    to8_line *cur = g_head;

    while (cur) {
        to8_line *cur_next = cur->next;
        to8_opcode g_equiv = to8_g_counterpart(cur->op);

        if (g_equiv != OP_NOP) {
            to8_line *scan;

            for (scan = cur->next; scan; scan = scan->next) {
                if (scan->is_target)
                    break;

                if (scan->op == g_equiv && scan->kind == cur->kind) {
                    int same_operand = 0;

                    switch (cur->kind) {
                    case ARG_SLOT:
                        same_operand = (cur->slot_a == scan->slot_a &&
                                         cur->push_depth == scan->push_depth);
                        break;
                    case ARG_SYM:
                        same_operand = (cur->sym == scan->sym &&
                                         cur->sym_addend == scan->sym_addend);
                        break;
                    case ARG_FIMM:
                        same_operand = (cur->f_val == scan->f_val);
                        break;
                    default:
                        break;
                    }

                    if (same_operand) {
                        scan->op = OP_FMOV;
                        scan->kind = ARG_NONE;
                        snprintf(scan->comment, sizeof scan->comment, "G = F");
                        scan->has_comment = 1;
                        changed = 1;
                    }
                    break;
                }

                if (to8_stops_fmov_scan(scan->op))
                    break;
            }
        }

        cur = cur_next;
    }
    return changed;
}

static void to8_debug_dump_list(const char *tag)
{
    to8_line *ln;
    fprintf(stderr, "--- %s ---\n", tag);
    for (ln = g_head; ln; ln = ln->next)
        fprintf(stderr, "  id=%d op=%s slot_a=%d kind=%d target=%d\n",
                ln->id, to8_opcode_name(ln->op), ln->slot_a, ln->kind, ln->is_target);
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
        changed |= to8_peephole_useless_ld();
	changed |= to8_peephole_useless_ldf(); /* NEW v8.0.1 */
	changed |= to8_peephole_fmov(); /* v8.4.0 */
	changed |= to8_peephole_float_store_dup(); /* NEW v8.0.2 */
	changed |= to8_peephole_dead_r0_load();   /* NEW v7.29.0 */
        changed |= to8_peephole_ld_deref();
	changed |= to8_peephole_commute();
	changed |= to8_peephole_op2();      /* NEW v7.26.0 */
	changed |= to8_peephole_jump_inversion();
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
    if (tcc_state && tcc_state->do_debug && ln->src_line
        && ln->src_line != g_last_printed_line) {
        char text[512], marker[600];
        g_last_printed_line = ln->src_line;
        if (to8_read_source_line(file->filename, ln->src_line, text, sizeof text) && text[0])
            snprintf(marker, sizeof marker, "; L%d: %s", ln->src_line, text);
        else
            snprintf(marker, sizeof marker, "; L%d", ln->src_line);
        g_col = 0;
        out_str(marker);
        out_char('\n');
    }
    
    if (ln->is_target) {
        g_col = 0;
        out_char('@'); out_int(ln->id); out_char(':'); out_char('\n');
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
	if(ln->op==OP_ADJi || ln->op==OP_FSCALEi) {
	    out_int(ln->imm_val);
	} else {
            out_int16(ln->imm_val>>16);
	    out_char(',');
            out_int16((short)ln->imm_val);
	}
        break;
    case ARG_FIMM:
        out_tab();
	/* v8.0.0: ARG_FIMM is exclusively a 32-bit float immediate now -
         * OP_LDFi and OP_LDGi both target it (F or G). out_double() is
         * no longer reachable from here. */
        out_float(ln->f_val);
        break;
    case ARG_SYM:
        out_tab();
        if (ln->sym) {
            const char *name = get_tok_str(ln->sym->v, NULL);
	    out_str("_");
            out_str(name ? name : "?");
        } else {
            out_int(ln->sym_addend);
        }
        break;
    case ARG_JMP:
        out_tab();
	out_str("@");
        out_int(to8_by_id(ln->jmp_target_id)->id);
        break;
    }

    out_pad_to(TO8_COMMENT_COL);
    out_char(';'); out_char(' ');
    if (ln->kind == ARG_JMP) {
        out_str(ln->jump_prefix);
        out_str(" @");
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
    to8_slot_cache_reset(&g_slot_cache);
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

    if ((func_vt.t & VT_BTYPE) == VT_STRUCT) {
        func_vc = addr;
        addr += PTR_SIZE;
    }

    sym = func_type->ref;
    while ((sym = sym->next) != NULL) {
        size = type_size(&sym->type, &align);
        size = (size + TO8_STACK_ALIGN - 1) & ~(TO8_STACK_ALIGN - 1);
        sym_push(sym->v & ~SYM_FIELD, &sym->type, VT_LOCAL | VT_LVAL, addr);  /* utilise D'ABORD */
        addr += size;                                                        /* puis incrémente */
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

    g_last_printed_line = -1;
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
