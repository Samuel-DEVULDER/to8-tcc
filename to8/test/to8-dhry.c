#define _STDIO_H_

#define TIME
#define HZ 10
asm("__readMicroseconds set  *	; \n"
    "	LDCLK 			; read clock (1/10ms)\n"
    "	MULi	$1,$86A0	; mul by 100 000 --> usec\n"
    "	RET			;");

void PUTC(int c) {
	asm("\tVM_OFF\n\tldb 7,s ;\n\tjsr	$E803 ;\n\tVM_ON");
}


int SETP(int a, int x, int y) {
	int r = 0;
	asm("\tVM_OFF\n\t"
	    "lda	4+4+3,s\n\t"
	    "ldx	4+4+4+2,s\n\t"
	    "ldy	4+4+4+4+2,s\n\t"
	    "jsr	$ec00\n\t"
	    "stx	2,s\n\t"
	    "VM_ON");
	return r;
}

void outbyte(int c)  {
	 PUTC(c);
}

void memmove(unsigned char *d, unsigned char *s, int l) {
	int one = 1;
	if(l)  do {
		*d = *s;
		d += one;
		s += one;
	} while(l -= one);
}

void strcpy(unsigned char *d, unsigned char *s) {
	int one = 1;
	while(*d = *s)  {
		d += one;
		s += one;
	}
}

int strcmp(unsigned char *d, unsigned char *s) {
	int one = 1, r;
	while(r = (*d - *s))  {
		d += one;
		s += one;
	}
	return r;
}


/* TINY would reduce small_printf to a no-op stub; keep the built-in
 * tinyprintf (_vprintf -> _diag_write_char -> outbyte) active so the
 * benchmark actually prints its results. */
/* #define TINY */

/* No system headers on this target: KISS varargs shim. The ABI pushes
 * all arguments contiguously on the S stack, first C argument at the
 * lowest address (verified in the generated call sequences), and every
 * argument occupies one 4-byte slot (default promotions). */
#define va_list  char *
#define va_start(ap, fmt)  ((ap) = (char *)&(fmt) + 4)
#define va_arg(ap, ty)     (*(ty *)((ap) += 4, (ap) - 4))
#define va_end(ap)         ((ap) = 0)

#define main dhry_main
#include  "dhry_1.c"
#include  "dhry_2.c"

#undef main
void main(void)  {
	SETP(0,0,0xFF0);
	SETP(1,0,0xF00);
	PUTC(27); PUTC(0x60);
	PUTC(27); PUTC(0x5B);
	Number_Of_Runs = 5000; // overide
	dhry_main();
}