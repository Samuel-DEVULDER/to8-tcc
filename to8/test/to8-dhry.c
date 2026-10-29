#define _STDIO_H_

#define TIME
#define HZ 10
asm("__readMicroseconds set  *	; \n"
    "	LDCLK 			; read clock (1/10ms)\n"
    "	MULi	$1,$86A0	; mul by 100 000 --> usec\n"
    "	RET			;");
	
void outbyte(int c)  {
	/* VM_OFF first: its "fdb *+2" is the dispatch word opCALLm pops into
	 * PC ([target] must be an address, not native code bytes), and it
	 * masks the timer hook during the native code - same shape as
	 * mstripe's PUTC. */
	asm("\tVM_OFF\n\tldb 7,s ;\n\tjsr	$E803 ;\n\tVM_ON");
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

#include  "dhry_1.c"
#include  "dhry_2.c"
