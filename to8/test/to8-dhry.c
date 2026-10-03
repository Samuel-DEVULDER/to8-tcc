#define _STDIO_H_

#define TIME
#define HZ 10
asm("__readMicroseconds set  *	; \n"
    "	LDCLK 			; read clock (1/10ms)\n"
    "	MULi	$1,$86A0	; mul by 100 000 --> usec\n"
    "	RET			;");
	
void outbyte(int c)  {
	asm("\tldb 7,s ;\n	jsr	$E803 ;");
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

/*
 * __divdi3 / __moddi3 / __udivdi3 / __umoddi3
 * -------------------------------------------
 * Reference C implementation of the 64-bit division helpers that
 * GCC/TCC link in automatically whenever the source does a `long long`
 * division/modulo on a target with no native 64-bit divide instruction.
 *
 * Core algorithm: schoolbook binary long division (shift-subtract),
 * 64 iterations, one bit of the quotient per iteration - the direct
 * 64-bit generalization of the shift-subtract technique already used
 * by opUDIVy in the 6809 runtime. No native 64-bit arithmetic op is
 * required to derive the algorithm; `uint64_t` is used here purely as
 * a 8-byte *container*, exactly like the to8 backend's own comment on
 * G being "a 8-byte value, never computed on directly, only stored".
 *
 * If you port this to real 6809 asm, replace the uint64_t container
 * with two 32-bit register pairs and carry the shift/borrow by hand -
 * the control flow below maps 1:1 onto that structure.
 */

#define NULL 0
typedef unsigned long      uint32_t;
typedef unsigned long long uint64_t;
typedef          long long int64_t;

/* Unsigned 64-bit division+remainder, from scratch. */
static uint64_t udivmod64(uint64_t num, uint64_t den, uint64_t *rem_out)
{
    uint64_t quot = 0;
    uint64_t rem  = 0;
    int i;

    if (den == 0) {
        /* Match libgcc convention: division by zero traps/undefined.
         * Returning 0 here just avoids UB in this reference version -
         * a real target should raise a trap instead. */
        if (rem_out) *rem_out = 0;
        return 0;
    }

    /* Fast path: divisor fits in the low 32 bits and numerator too -
     * avoids 64 iterations for the extremely common case of small
     * operands (array indices, loop counters promoted to long long). */
    if ((num >> 32) == 0 && (den >> 32) == 0) {
        uint32_t n32 = (uint32_t)num, d32 = (uint32_t)den;
        if (rem_out) *rem_out = n32 % d32;
        return n32 / d32;
    }

    for (i = 63; i >= 0; i--) {
        rem <<= 1;
        rem |= (num >> i) & 1ULL;
        if (rem >= den) {
            rem -= den;
            quot |= (1ULL << i);
        }
    }

    if (rem_out) *rem_out = rem;
    return quot;
}

uint64_t __udivdi3(uint64_t a, uint64_t b)
{
    return udivmod64(a, b, NULL);
}

uint64_t __umoddi3(uint64_t a, uint64_t b)
{
    uint64_t rem;
    udivmod64(a, b, &rem);
    return rem;
}

/* Signed forms: take abs() of both operands, divide unsigned,
 * then fix up the sign of quotient and remainder.
 * C89/C99 truncating-toward-zero semantics: quotient sign is
 * negative iff operand signs differ; remainder takes the sign
 * of the dividend (numerator). */
int64_t __divdi3(int64_t a, int64_t b)
{
    uint64_t ua = (a < 0) ? (uint64_t)(-a) : (uint64_t)a;
    uint64_t ub = (b < 0) ? (uint64_t)(-b) : (uint64_t)b;
    int neg = (a < 0) != (b < 0);
    uint64_t uq = udivmod64(ua, ub, NULL);
    return neg ? -(int64_t)uq : (int64_t)uq;
}

int64_t __moddi3(int64_t a, int64_t b)
{
    uint64_t ua = (a < 0) ? (uint64_t)(-a) : (uint64_t)a;
    uint64_t ub = (b < 0) ? (uint64_t)(-b) : (uint64_t)b;
    uint64_t ur;
    udivmod64(ua, ub, &ur);
    /* remainder sign follows the dividend (a), never the divisor */
    return (a < 0) ? -(int64_t)ur : (int64_t)ur;
}


#define TINY

#include  "dhry_1.c"
#include  "dhry_2.c"
