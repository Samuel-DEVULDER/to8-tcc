extern void exit(int);

#define native(asm6809) asm("\tVM_OFF\n\t" asm6809 "\n\tVM_ON")

void putc(int c) {
	native("ldb	7,s\n\t"
	       "jsr	$E803");
}

void plot(int x, int y, int col) {
}

void puts(char *s) {
	while(*s)  putc(*s++);
}

void cls(void) {
	putc('\f');
}

void cursor(int on)  {
	putc(on ? 0x11 : 0x14);
}

void  locate(int x, int y) {
	putc(0x1F);
	putc(0x40+y);
	putc(0x41+x);
}

void esc(int code) {
	putc(0x1b);
	putc(code);
}


void ink(int col) {
	esc(col + (col>=8 ? 0x68 : 0x40));
}

void paper(int col) {
	esc(col + (col>=8 ? 0x70 : 0x50));
}

void border(int  col) {
	esc(col + (col>=8 ? 0x78 : 0x60));
}

enum  {
	GFX_MODE_BM4 = 0x59,
	GFX_MODE_40 = 0x5A,
	GFX_MODE_80 = 0x5B,
	GFX_MODE_BM16 = 0x5E,
};

void main(int ac,  char **av) {
	cursor(0);
	border(0);
	esc(GFX_MODE_BM4);
	puts("hello, world!\a\r\n");
}


