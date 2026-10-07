/* to8-mstripe.c - Mandelbrot by vertical stripes with uniform rectangles
 *
 * Variant of to8-mandel.c: same window, same fixed point, same iterate(),
 * same primitives (BM16 mode, monitor plot, readMillis). The screen is cut
 * into vertical stripes of BLK_W pixels, scanned right to left, each stripe
 * drawn bottom-up (y = HEIGHT-1 down to 0).
 *
 * right[y] always holds the iteration of the pixel (bx+bw, y): the column
 * JUST RIGHT of the stripe being processed. Before the first stripe it is
 * initialized with the off-screen column x = WIDTH (computed, not plotted).
 * When a stripe ends, every right[y] has been rewritten with its left
 * column (x = bx), which is exactly the right neighbour of the next
 * stripe - no pixel is ever computed twice between stripes.
 *
 * Per stripe, 4 phases:
 *
 *   1. Line scan (bottom-up): compute + plot ALL bw pixels of the line,
 *      test uniformity, store the left pixel into right[y]. First uniform
 *      line found = yu (the rectangle's base). If none until y < 0: the
 *      stripe is done.
 *   2. Sides only, walking up: one iterate() per line - the left pixel
 *      (x = bx, plotted) compared BOTH to right[y] (horizontal continuity
 *      with the column just right of the stripe) and to the rectangle
 *      value col (vertical continuity). right[y] is then updated with the
 *      new value. First mismatch at yf (the rectangle stops above yf); if
 *      the sides hold up to y < 0, yf = -1.
 *   3. Interior, checked from its BOTTOM line (y = yf+1) upward: each
 *      line's interior + right edge (x = bx+1..bx+bw-1) is computed,
 *      plotted and tested against col. First uniform line yc: the
 *      rectangle [yc..yu] is confirmed - the lines yc+1..yu-1 above it are
 *      FILLED (plot only, zero iterate()). Failed lines were already fully
 *      computed and plotted while being tested. A constant line exists for
 *      sure at yu (phase 1), so the rectangle is never empty.
 *   4. Resume the plain line scan at yf (its left pixel is known already;
 *      the scan recomputes it - one iterate() spent per rectangle).
 *
 * Costs (bw = stripe width):
 *   scanned line  : bw iterate()
 *   failed side   : 1 + (bw-1) iterate() (the line is fully computed)
 *   checked line  : same (interior computed during the check)
 *   filled line   : 0 iterate(), bw-1 plots
 * The fill trusts the sides + one full line: an interior blob that touches
 * neither the sides nor line yc would be painted wrong - the same bet as
 * the perimeter test of the 4x8 block variant.
 */

extern void exit(int);

#define native(asm6809) asm("\tVM_OFF\n\t" asm6809 "\n\tVM_ON")

void putc(int c) {
	native("ldb	7,s\n\t"
	       "jsr	$E803");
}

int getc(void) {
	int r=0;
	native("jsr	$E806\n\t"
	       "clra\n\t"
	       "std	2,s\n\t"
	       "clrb\n\t"
	       "std	,s");
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

extern unsigned readMillis(void);
asm("_readMillis set  *	; \n"
    "	LDCLK 		; read clock (1/10ms)\n"
    "	MULi	0,100	; mul by 100 --> ms\n"
    "	RET			;");

void puts(unsigned char *s) {
	while(*s)  putc(*s++);
}

void cls(void) {
	putc('\f');
}

void cursor(int on)  {
	putc(on ? 0x11 : 0x14);
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

void border(int col) {
	esc(col + (col>=8 ? 0x78 : 0x60));
}

#define setRGB8(col, rgb) palette(col,0,(((rgb<<4)&0xf00)+((rgb>>8)&0xf0)+((rgb>>20)&0xf)))

enum  {
	GFX_MODE_BM4 = 0x59,
	GFX_MODE_40 = 0x5A,
	GFX_MODE_80 = 0x5B,
	GFX_MODE_BM16 = 0x5E,
};

void setPalette(void) {
	setRGB8(0, 0x000000);  // black = set interior

	// Electric gradient (1-15)
	setRGB8( 1, 0x000040);
	setRGB8( 2, 0x000080);
	setRGB8( 3, 0x004080);
	setRGB8( 4, 0x008080);
	setRGB8( 5, 0x008040);
	setRGB8( 6, 0x008000);
	setRGB8( 7, 0x408000);
	setRGB8( 8, 0x808000);
	setRGB8( 9, 0x804000);
	setRGB8(10, 0x800000);
	setRGB8(11, 0x800040);
	setRGB8(12, 0x800080);
	setRGB8(13, 0x400080);
	setRGB8(14, 0x808080);

	setRGB8(15, 0xffffff);
}

void putu(unsigned t)  {
	unsigned  r = t/10u;
	if(r) putu(r);
	r *= 10u;
	t += '0';
	putc(t - r);
}

// ==================== CONFIG ====================
#define MAX_ITER 32
#define FIX_FRAC 13 /* 8  = fast  13 = accurate */

// --- 3 USER CONSTANTS (in float) ---
// Verbatim from to8-mandel.c (the source of truth).
#define CH      2.20f   // Window height (CI_MAX - CI_MIN)
#define CR_MIN -2.50f    // Left (minimum real part)
#define CI_MIN -1.10f    // Bottom (minimum imaginary part)

// float -> fixed-point conversion at compile time
#define FIX_ONE (1 << FIX_FRAC)
#define F2FIX(f) ((int)((f) * FIX_ONE + 0.5))

// --- Screen dimensions ---
#define WIDTH  160
#define HEIGHT 200

// --- Stripe width (the left-most stripe is narrower if BLK_W does not
//     divide WIDTH) ---
#define BLK_W 5

// --- Automatic computations ---
#define CW ((2*CH*WIDTH) / (float)HEIGHT)

#define CR_MAX (CR_MIN + CW)
#define CI_MAX (CI_MIN + CH)

// Steps in fixed point
#define STEP_X F2FIX(CW / WIDTH)
#define STEP_Y F2FIX(CH / HEIGHT)

// cr of the RIGHT-most pixel x=WIDTH-1 (the line scan starts there,
// exactly like to8-mandel.c) and ci of the BOTTOM line y=HEIGHT-1
#define CR_BASE F2FIX(CR_MAX)
#define CI_BASE F2FIX(CI_MIN)

// Mandelbrot constants
#define FIX_FOUR F2FIX(4.0f)
#define FIX_MUL_SHIFT (FIX_FRAC)
#define FIX_2MUL_SHIFT (FIX_FRAC - 1)

// Pixel color from the iteration count: SAME formula + dithering as the
// to8-mandel.c reference (the (x^y)&1 checkerboard is part of the image).
#define COLOR(it, d) ((it) ? (MAX_ITER - (it) + (d)) >> 1 : 0)

// ==================== MANDELBROT ====================

// Number of iterate() calls = number of individually computed pixels.
// Filled (non-computed) pixels = WIDTH*HEIGHT - n_iter, displayed at exit.
int n_iter;

int iterate(int cr, int ci) {
    unsigned zr2 = 0, zi2 = 0;
    int zr = 0, zi = 0, one = 1;
    int iter = MAX_ITER;

    n_iter++;

    do {
	zi = ci + ((zr * zi)>>FIX_2MUL_SHIFT);
	zr = cr + (zr2 - zi2);

        zi2 = ((unsigned)(zi * zi))>>FIX_MUL_SHIFT;
        zr2 = ((unsigned)(zr * zr))>>FIX_MUL_SHIFT;

	iter -= one;
    } while(iter && (zr2 + zi2) < (FIX_FOUR+1));
    return iter;
}

// Iteration of the pixel (bx+bw, y): the column just right of the stripe
// being processed. Filled by the init with the off-screen column x=WIDTH,
// then rewritten column-by-column by each stripe for ALL 200 lines.
static int right[HEIGHT];

// Plot a pixel from its iteration count (reference formula + dither).
void plot_iter(int x, int y, int iter) {
    plot(x, y, COLOR(iter, (x ^ y) & 1));
}

// Compute and plot one line of the stripe, pixel by pixel, right to left.
// cr = cr of the stripe's RIGHT pixel (x = bx+bw-1), ci = the line's ci,
// y = the line. Stores the LEFT pixel's value into right[y] (the next
// stripe's right column) and returns the common value if the line is
// uniform, -1 if not.
int line_uniform(int cr, int ci, int bx, int bw, int y) {
    int x, v, col, one = 1;

    col = v = iterate(cr, ci);
    x = bw - one;
    plot_iter(bx + x, y, col);
    while((x-=one) >= 0) {
        cr -= STEP_X;
        v = iterate(cr, ci);
        plot_iter(bx + x, y, v);
        if (col>=0 && v != col) col= -1;
    }
    right[y] = v;               // v = left pixel (x = bx)
    return col;
}

// Process one vertical stripe [bx..bx+bw-1], bottom-up.
// cr = cr of the stripe's RIGHT pixel (x = bx+bw-1) - constant for the
// whole stripe. ci = ci of the bottom line (y = HEIGHT-1).
void stripe(int bx, int bw, int cr, int ci) {
    int y, yu, yf, yc, col, v, old, unif, crL, crI, ciF, x;
    int one = 1, step_y = STEP_Y, step_x = STEP_X;

    // cr of the stripe's LEFT column x = bx (cr decreases leftwards)
    crL = cr - (bw-1)*STEP_X;

    y = HEIGHT - one;
    for (;;) {
        // --- phase 1: plain line scan until a uniform line (or y < 0) ---
        col = -1;
        while (y >= 0) {
            col = line_uniform(cr, ci, bx, bw, y);
            if (col >= 0) break;
            ci += step_y;
            y -= one;
        }
        if (y < 0) return;              // stripe finished without rectangle
        yu = y;

        // --- phase 2: sides only, one iterate() per line, walking up ---
        ci += step_y;
        yf = -1;
        y -= one;
        while (y >= 0) {
            v = iterate(crL, ci);       // left pixel (x = bx), plotted
            plot_iter(bx, y, v);
            old = right[y];             // column x = bx+bw, already known
            right[y] = v;               // becomes the next stripe's column
            if (v != col || v != old) { yf = y; break; }
            ci += step_y;
            y -= one;
        }
        ciF = ci;                       // ci(yf), to resume the scan (phase 4)

        // --- phase 3: interior, checked from its bottom line upward ---
        // position y/ci on the first candidate line yf+1
        yc = -1;
        if (yf < 0) {
            ci -= step_y;               // stepped one past y=0 in phase 2
            y = 0;                      // sides held up to the top: ci was
        } else {
            ci += step_y;
            y = yf + one;
        }
        while (y < yu) {
            // interior + right edge x = bx+1..bx+bw-1: the left pixel (x=bx)
            // is already known from phase 2 (and equals col, or the line
            // would not be a candidate)
            unif = -1;
            crI = cr;                   // cr of the RIGHT pixel of the line
            for (x = bx + bw - 1; x > bx; x -= one) {
                v = iterate(crI, ci);
                plot_iter(x, y, v);
                if (unif && v != col) unif = 0;
                crI -= step_x;
            }
            if (unif) { yc = y; break; }
            y += one;
            ci -= STEP_Y;
        }
        if (yc >= 0) {
            // rectangle [yc..yu] confirmed: fill the lines above yc with no
            // iterate() at all - left edge already plotted in phase 2,
            // interior + right edge get the rectangle value
            for (y = yc + one; y < yu; y += one)
                for (x = bx + bw - 1; x > bx; x -= one)
                    plot_iter(x, y, col);
        }

        // --- phase 4: resume the plain scan at yf (its interior has not
        //     been computed yet; the scan recomputes the whole line) ---
        if (yf < 0) return;             // rectangle reached the top
        y = yf;
        ci = ciF;
    }
}

void mandelbrot(void) {
    int y, xprev, bw, cr, cr0, ci, one = 1;

    // init right[] with the off-screen column x = WIDTH (just right of the
    // screen): computed, never plotted. ci runs bottom -> top.
    cr0 = CR_BASE + STEP_X;
    for (y = HEIGHT - one, ci = CI_BASE; y >= 0; y -= one) {
        right[y] = iterate(cr0, ci);
        ci += STEP_Y;
    }

    // stripes right to left. cr = cr of the stripe's RIGHT pixel; ci of the
    // bottom line is CI_BASE for every stripe (each stripe sweeps the full
    // height). The first stripe's right pixel is x = WIDTH-1.
    xprev = WIDTH;
    cr = CR_BASE;
    while (xprev > 0) {
        if (xprev < BLK_W) bw = xprev; else bw = BLK_W;
        stripe(xprev - bw, bw, cr, CI_BASE);
        cr -= bw*STEP_X;                // cr of pixel x = bx-1: next stripe's right pixel
        xprev -= bw;
    }
}

void print_time(unsigned t) {
	char *sep="", *bl=" ";
	if(t>3600000) {
		puts(sep); sep=bl;
		putu(t/3600000);
		t %= 3600000;
		puts("h");
	}
	if(t>60000) {
		puts(sep); sep=bl;
		putu(t/60000);
		t %= 60000;
		puts("m");
	}
	if(t>1000) {
		puts(sep); sep=bl;
		putu(t/1000);
		t %= 1000;
		puts("s");
	}
	if(t>0 || !*sep) {
		puts(sep);
		putu(t);
		puts("ms");
	}
}

void main(int ac,  char **av) {
	unsigned t;
	int fill;
	cursor(0);
	border(0);
	esc(GFX_MODE_BM16);
	setPalette();
	n_iter = 0;         // LOADM does not clear BSS: explicit init mandatory
	t = readMillis();
	mandelbrot();
	t = readMillis() - t;
	beep();
	while(!getc());
	esc(GFX_MODE_40);
	paper(0);ink(15);cls();
	puts("Mandelbrot stripes ");
	putu(BLK_W);
	puts("px in ");
	print_time(t);
	puts("\r\n");
	puts("calc: ");
	putu(n_iter);
	puts("  fill: ");
	fill = WIDTH*HEIGHT - n_iter;
	if (fill < 0) fill = 0;
	putu(fill);
	puts("\r\n");
}
