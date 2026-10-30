/* Minimal probe for the 32-bit division/mod ops used by _cvt:
 * DIV2/DIVi/MOD2/MODi. Expected output: "50000" + CR LF.
 * val/radix = 5000 (DIV2), then digits via DIVi and MODi,
 * and one MOD2 at the end. */

void outbyte(int c) {
	asm("\tVM_OFF\n\tldb 7,s ;\n\tjsr	$E803 ;\n\tVM_ON");
}

int main(void)
{
	int val = 50000, radix = 10;
	int q = val / radix;              /* 5000 : DIV2 */

	outbyte('0' + q / 1000);          /* '5' : DIVi */
	outbyte('0' + (q / 100) % 10);    /* '0' : DIVi + MODi */
	outbyte('0' + (q / 10) % 10);     /* '0' */
	outbyte('0' + q % 10);            /* '0' : MODi */
	outbyte('0' + val % radix);       /* '0' : MOD2 */
	outbyte('\r');
	outbyte('\n');
	return 0;
}
