/* Minimal repro: two switches in sequence on the same variable, like
 * _vprintf's "fetch value" + "process output" pair. Expected output:
 * "11" + CR LF (both switches match case 'd'). If the second switch
 * mis-dispatches into its default, output is "19". */

void outbyte(int c) {
	asm("\tVM_OFF\n\tldb 7,s ;\n\tjsr	$E803 ;\n\tVM_ON");
}

int main(void)
{
	char c = 'd';
	int r1 = 0, r2 = 0;

	switch (c) {
	case 'd':
		r1 = 1;
		break;
	default:
		r1 = 9;
		break;
	}

	switch (c) {
	case 'd':
		r2 = 1;
		break;
	case 's':
		r2 = 2;
		break;
	default:
		r2 = 9;
		break;
	}

	outbyte('0' + r1);
	outbyte('0' + r2);
	outbyte('\r');
	outbyte('\n');
	return 0;
}
