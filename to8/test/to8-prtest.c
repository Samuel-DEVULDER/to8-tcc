/* Minimal %d printf repro: same shape as dhry's tinyprintf
 * (va shim + _vprintf + _cvt + outbyte). Expected output:
 * "N=50000 OK" + CR LF. If the number is blank or garbage,
 * the bug reproduces outside the full Dhrystone context. */

#define NULL 0

/* KISS varargs shim (no system headers): args pushed contiguously,
 * first C arg at the lowest address, one 4-byte slot per arg. */
#define va_list  char *
#define va_start(ap, fmt)  ((ap) = (char *)&(fmt) + 4)
#define va_arg(ap, ty)     (*(ty *)((ap) += 4, (ap) - 4))
#define va_end(ap)         ((ap) = 0)

void outbyte(int c) {
	asm("\tVM_OFF\n\tldb 7,s ;\n\tjsr	$E803 ;\n\tVM_ON");
}

static int _cvt(int val, char *buf, int radix, char *digits)
{
    char temp[16];
    char *cp = temp;
    int length = 0;

    outbyte('D'); outbyte(digits[0]); outbyte(digits[1]); /* expect '0','1' */
    outbyte('{'); outbyte(buf[0]); outbyte('}');          /* caller's buf[0] */

    if (val == 0) {
        *cp++ = '0';
    } else {
        while (val) {
            *cp++ = digits[val % radix];
            val /= radix;
        }
    }
    while (cp != temp) {
        *buf++ = *--cp;
        length++;
    }
    *buf = '\0';
    return length;
}

static void _vprintf(void (*putc)(), const char *fmt, va_list ap)
{
    char buf[16];
    int c;
    char *cp = buf;
    int val = 0;
    int length = 0;

    while ((c = *fmt++) != '\0') {
        if (c == '%') {
            c = *fmt++;
            if (c == 'd') {
                val = va_arg(ap, int);
                outbyte('V');
                outbyte('0' + (val / 10000));      /* '5' if val==50000 */
                outbyte('A' + ((int)ap & 0xF));    /* low nibble of ap */
                length = _cvt(val, buf, 10, "0123456789");
                outbyte('L');
                outbyte('0' + length);
                cp = buf;
            } else {
                (*putc)('%');
                (*putc)(c);
                continue;
            }
            while (length-- > 0) {
                c = *cp++;
                outbyte('[');          /* probe: loop body reached */
                outbyte(c);            /* DIRECT call instead of (*putc)(c) */
                outbyte(']');
            }
        } else {
            (*putc)(c);
        }
    }
}

static void my_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vprintf(outbyte, fmt, ap);
    va_end(ap);
}

int main(void)
{
    int runs = 50000;
    my_printf("N=%d OK\r\n", runs);
    return 0;
}
