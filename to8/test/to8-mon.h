#ifndef TO8_MON_H
#define TO8_MON_H

#define native(asm6809) asm("\tVM_OFF\n\t" asm6809 "\n\tVM_ON")

void PUTC(int c) {      
#ifdef __TO8__
        native("ldb     7,s\n\t"
               "jsr     $E803");
#else
        char b = c;
        write(1,&b,1);
#endif
}

int GETC(void) {
#ifdef __TO8__
        int r=0;
        native("jsr     $E806\n\t"
               "clra\n\t"
               "std     2,s\n\t"
               "clrb\n\t"
               "std     ,s");
        return r;
#else
        char b = 0;
        read(0,&b,1);
        return b;
#endif
}

void PLOT(int x, int y, int col) {
	native("ldx	4+2,s\n\t"
	       "ldy	8+2,s\n\t"
	       "ldb	12+3,s\n\t"
	       "stb	$6038\n\t"
	       "jsr	$E80f");
}

int SETP(int a, int x, int y) {
#ifdef  __TO8__
        int r = 0;
        native("lda     4+4+3,s\n\t"
               "ldx     4+4+4+2,s\n\t"
               "ldy     4+4+4+4+2,s\n\t"
               "jsr     $ec00\n\t"
               "stx     2,s");
        return r;
#endif
}

#define KTST()  (1 & *(volatile char*)0xE7C8)

#define BEEP()      PUTC('\a')
#define CLS()  	    PUTC('\f')
#define CURSOR(on)  PUTC(on ? 0x11 : 0x14)
enum GFX_MODE {
        GFX_MODE_BM4 = 0x59,
        GFX_MODE_40 = 0x5A,
        GFX_MODE_80 = 0x5B,
        GFX_MODE_BM16 = 0x5E,
};
#define GFX(mode)   do {PUTC(0x1b); PUTC(mode);} while(0)
#define INK(col)    GFX(col + (col>=8 ? 0x68 : 0x40))
#define PAPER(col)  GFX(col + (col>=8 ? 0x70 : 0x50))
#define BORDER(col) GFX(col + (col>=8 ? 0x78 : 0x60))

#endif