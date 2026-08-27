extern void exit(int);

#define native(asm6809) asm("\tNATIVE\n\t" asm6809 "\n\tTO8_VM")

void putc(char c) {
	native("ldb	7,s\n\t"
	       "jsr	$E803");
}

void puts(char *s) {
	while(*s)  putc(*s++);
}

void main(int ac,  char **av) {
	puts("hello, world!");
}


