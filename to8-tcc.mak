# to8-tcc.mak - to be included in Makefile to add TO8 "support"

DEF-to8 = -DTCC_TARGET_TO8 -w # TO8 pseudo-architecture (single-register backend)

TCC_X += to8

# TO8: pseudo single-register backend (like c67: gen only + shared core)
to8_FILES = $(CORE_FILES) to8-gen.c to8-link.c to8-stubs.c

# to8 has no standard runtime support library
LIBTCC1_X := $(filter-out to8,$(LIBTCC1_X))

# convenience: build only the TO8 cross compiler (no libtcc1)
cross-to8: to8-tcc$(EXESUF) ;

# full reset + rebuild + smoke-test cycle - the actual point of this file
cross-to8-dbg:
	$(MAKE) --no-print-directory cross-to8 "CC=gcc -g" "CFLAGS=-O0"
	gcc -g -o to8-tcc$(EXESUF) to8-tcc.o
	

tst.s: tst.c cross-to8-dbg
	./to8-tcc tst.c -c -O -g
	@strings -n 2 tst.o | sed -n '/; TO8 backend/,/; --- end of asm ---/p' | tee $@

.DEFAULT_GOAL := all

# help: extends the base Makefile's help instead of replacing it -
# runs the ORIGINAL help as a separate sub-make invocation pinned
# explicitly to the base Makefile, then appends our own section.
help::
	@echo "make cross-to8"
	@echo "  build only the TO8 cross compiler (no libtcc1)"
	