# to8-tcc.mak - to be included in Makefile to add TO8 "support"

DEF-to8 = -DTCC_TARGET_TO8 -w # TO8 pseudo-architecture (single-register backend)

TCC_X += to8

# TO8: pseudo single-register backend (like c67: gen only + shared core)
to8_FILES = $(CORE_FILES) to8/to8-gen.c to8/to8-link.c to8/to8-stubs.c

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
	$(MAKE) --no-print-directory cross-to8 "CC=$(CC) -g" "CFLAGS=-O0"
	$(CC) -g -o to8-tcc$(EXESUF) to8-tcc.o -L.

git-pull-all:
	git pull --rebase
	git fetch thomson
	mkdir c6809 sapfs
	git subtree split --prefix=c6809 -b split-c6809 thomson/main
	git subtree split --prefix=sapfs -b split-sapfs thomson/main
	rmdir c6809 sapfs
	git subtree merge --prefix=to8/c6809 split-c6809
	git subtree merge --prefix=to8/sapfs split-sapfs
	git branch -D split-c6809 split-sapfs
	git push

# produces self-contained asm file
%.asm: %.c cross-to8 to8/to8-vm.asm
	@echo "(main)main" >$@
	@cat to8/to8-vm.asm >>$@
	@echo "* start of code" >>$@
	@echo "__start set *" >>$@
	./to8-tcc $< -c -O -g -o '.$(shell basename "$*").o'
	@strings -n 2 '.$(shell basename "$*").o' | \
	sed -n '/\* TO8 backend/,/\* --- end of asm ---/p' >> $@
	@echo "	echo Code  size = &(*-__start) bytes (&((*-__start+1023)/1024) kb)" >>$@
	@echo "__start set __start+0" >>$@
	@echo "	echo Total size = &(*-init) bytes (&((*-init+1023)/1024) kb)" >>$@
	@echo "	end	init" >>$@
	@rm '.$(shell basename "$*").o'

C6809=$(TOP)/to8/c6809/c6809$(EXESUF)
%.bin: %.asm $(C6809)
	$(C6809) -oOP $< $@

$(C6809): $(TOP)/to8/c6809/c6809.c
	$(CC) $< -O -o $@

SAPFS=$(TOP)/to8/sapfs/sapfs$(EXESUF)
%.sap: %.bin $(SAPFS) AUTO.BAT
	@$(SAPFS) -c $@
	@$(SAPFS) -a $@ AUTO.BAT
	@BASE=$$(basename $< .bin | tr 'a-z' 'A-Z' | cut -c1-8); \
	BIN="$$BASE.BIN"; mv $< .$$BIN; mv .$$BIN $$BIN;\
	printf >AUTO.BAS "\\r\\n10 BANK2:CLEAR,&H71FF:LOADM\"%s\"" "$$BIN"; \
	$(SAPFS) -a $@ AUTO.BAS; \
	$(SAPFS) -a $@ $$BIN
	$(SAPFS) -t $@

$(SAPFS): $(TOP)/to8/sapfs/
	$(MAKE) -C $< --no-print-directory $*

clean::
	test ! -e $(C6809) || rm $(C6809) 
	$(MAKE) -C $(TOP)/to8/sapfs --no-print-directory clean

AUTO.BAT:
	echo /wATABAACoggIkFVVE8uQkFTAAAA | base64 -d > $@

tst: cross-to8-dbg to8-tst.asm
	cat to8-tst.asm | tee /dev/clipboard

.DEFAULT_GOAL := all