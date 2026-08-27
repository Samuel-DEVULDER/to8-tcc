# to8-tcc.mak - to be included in Makefile to add TO8 "support"

DEF-to8 = -DTCC_TARGET_TO8 -w # TO8 pseudo-architecture (single-register backend)

TCC_X += to8

# TO8: pseudo single-register backend (like c67: gen only + shared core)
to8_FILES = $(CORE_FILES) to8-gen.c to8-link.c to8-stubs.c

# to8 has no standard runtime support library
LIBTCC1_X := $(filter-out to8,$(LIBTCC1_X))

# help: extends the base Makefile's help instead of replacing it -
# runs the ORIGINAL help as a separate sub-make invocation pinned
# explicitly to the base Makefile, then appends our own section.
help::
	@echo "make cross-to8"
	@echo "  build only the TO8 cross compiler (no libtcc1)"

# convenience: build only the TO8 cross compiler (no libtcc1)
cross-to8: to8-tcc$(EXESUF) ;

# full reset + rebuild + smoke-test cycle - the actual point of this file
cross-to8-dbg:
	$(MAKE) --no-print-directory cross-to8 "CC=gcc -g" "CFLAGS=-O0"
	gcc -g -o to8-tcc$(EXESUF) to8-tcc.o
	

# produces self-contained asm file
%.asm: %.c cross-to8 to8-vm.asm
	@echo "(main)main" >$@
	@cat to8-vm.asm >>$@
	./to8-tcc $< -c -O -g -o .$*.o 
	strings -n 2 .$*.o | \
	sed -n '/\* TO8 backend/,/\* --- end of asm ---/p' >> $@
	@echo "(info)" >>$@
	@echo "	end	crt0" >>$@
	@rm .$*.o

C6809=/cygdrive/c/Users/Utilisateur/Desktop/Thomson/c6809-0.83/Thomson-Projects/c6809/c6809.exe
%.bin: %.asm
	$(C6809) -oOP $< $@
	#less codes.lst
	
SAPFS=/cygdrive/c/Users/Utilisateur/Desktop/Thomson/c6809-0.83/Thomson-Projects/sapfs/sapfs.exe
%.sap: %.bin	
	$(SAPFS) -c $@
	$(SAPFS) -a $@ $(subst a,A,$(subst b,B,$(subst c,C,$(subst d,D,$(subst e,E,$(subst f,F,$(subst g,G,$(subst h,H,$(subst i,I,$(subst j,J,$(subst k,K,$(subst l,L,$(subst m,M,$(subst n,N,$(subst o,O,$(subst p,P,$(subst q,Q,$(subst r,R,$(subst s,S,$(subst t,T,$(subst u,U,$(subst v,V,$(subst w,W,$(subst x,X,$(subst y,Y,$(subst z,Z,$<))))))))))))))))))))))))))

tst: cross-to8-dbg to8-tst.asm 
	cat to8-tst.asm | tee /dev/clipboard

.DEFAULT_GOAL := all