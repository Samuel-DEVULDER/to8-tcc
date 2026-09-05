extern void exit(int);

#define native(asm6809) asm("\tVM_OFF\n\t" asm6809 "\n\tVM_ON")

void putc(int c) {
	native("ldb	7,s\n\t"
	       "jsr	$E803");
}

char getc(void) {
	int r=0;
	native("jsr	$E806\n\t"
	       "stb	3,s");
	return r;
}

void plot(int x, int y, int col) {
	native("ldx	6,s\n\t"
	       "ldy	10,s\n\t"
	       "ldb	15,s\n\t"
	       "stb	$6038\n\t"
	       "jsr	$E80f");
}

int palette(int a, int x, int y) {
	int r = 0;
	native("lda	4+4+3,s\n\t"
	       "ldx	4+4+4+2,s\n\t"
	       "ldy	4+4+4+4+2,s\n\t"
	       "jsr	$ec00\n\t"
	       "stx	2,s");
	return r;
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


void beep(void) {
	putc('\a');
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

void pattern() {
	int x,y,mask=15;
	for(x=0;x<16;++x) palette(x,0,x*(1+16+256));
	for(y=0;y<200;++y) {
		for(x=0;x<160;++x) {
			plot(x,y,(x^(y>>1))&mask);
		}
	}
}

void main(int ac,  char **av) {
	cursor(0);
	border(0);
	esc(GFX_MODE_BM16);
	pattern();
	beep();
	while(!getc());
	esc(GFX_MODE_40);
	paper(0);ink(15);cls();
	puts("hello, world!\r\n");
}


