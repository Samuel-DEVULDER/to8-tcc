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

#ifndef __TO8__
#include <unistd.h>
#include <time.h>
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

extern void exit(int);

#define native(asm6809) asm("\tVM_OFF\n\t" asm6809 "\n\tVM_ON")

void PUTC(int c) {	
#ifdef __TO8__
	native("ldb	7,s\n\t"
	       "jsr	$E803");
#else
	char b = c;
	write(1,&b,1);
#endif
}

int GETC(void) {
#ifdef __TO8__
	int r=0;
	native("jsr	$E806\n\t"
	       "clra\n\t"
	       "std	2,s\n\t"
	       "clrb\n\t"
	       "std	,s");
	return r;
#else
	char b = 0;
	read(0,&b,1);
	return b;
#endif
}

void xplot(int x, int y, int col) {
#ifdef	__TO8__	
	native(
	"ldd	8+2,s\n\t"
	"cmpd	#200\n\t"
	"bhs	xplot_done\n\t"
	"lda	#40\n\t"
	"mul\n\t"
	"adda	#$40\n\t"
	"tfr	d,x\n\t"
	"ldd	4+2,s\n\t"
	"cmpd	#160\n\t"
	"bhs	xplot_done\n\t"
	"lda	$e7c3\n\t"
	"lsrb\n\t"
	"rora\n\t"
	"lsrb\n\t"
	"abx\n\t"
	"rola\n\t"
	"eora	#1\n\t"
	"sta	$e7c3\n\t"
	"ldb	12+3,s\n\t"
	"andb	#15\n\t"
	"bcs	*+6\n\t"
	"lslb\n\t"
	"lslb\n\t"
	"lslb\n\t"
	"lslb\n\t"
	"eorb	,x\n\t"
	"stb	,x\n"
	"xplot_done set *\n\t"
	"puls	d,u\n\t"
	"pulu	pc"
	);
#endif
}

void xbox(int x, int y, int w, int h, int col) {
	int i, j, k, one = 1;
	h -= one; k = y + h;
	for(i=w;(i-=one)>=0;) {
		j = x+i;
		xplot(j,y, col);
		xplot(j,k, col);
	}
	k = x+(w-one);
	for(i=h;i;i-=one) {
		j = y+i;
		xplot(x,j, col);
		xplot(k,j, col);
	}

}

int palette(int a, int x, int y) {
#ifdef	__TO8__
	int r = 0;
	native("lda	4+4+3,s\n\t"
	       "ldx	4+4+4+2,s\n\t"
	       "ldy	4+4+4+4+2,s\n\t"
	       "jsr	$ec00\n\t"
	       "stx	2,s");
	return r;
#endif
}

#ifdef	__TO8__
extern unsigned readMillis(void);
asm("_readMillis set  *	; \n"
    "	LDCLK 		; read clock (1/10ms)\n"
    "	MULi	0,100	; mul by 100 --> ms\n"
    "	RET			;");
#else
unsigned readMillis(void) {
	clock_t t =  clock();
	return (t*1000)/CLOCKS_PER_SEC;
}
#endif

void PUTSTR(unsigned char *s) {
	while(*s)  PUTC(*s++);
}

void cls(void) {
	PUTC('\f');
}

void cursor(int on)  {
	PUTC(on ? 0x11 : 0x14);
}

void esc(int code) {
	PUTC(0x1b);
	PUTC(code);
}

void beep(void) {
	PUTC('\a');
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
	PUTC(t - r);
}

void puti(int t) {
	if(t<0) {PUTC('-');t=-t;}
	putu(t);
}

void put_time(unsigned t) {
	char *sep="", *bl=" ";
	if(t>3600000) {
		PUTSTR(sep); sep=bl;
		putu(t/3600000);
		t %= 3600000;
		PUTSTR("h");
	}
	if(t>60000) {
		PUTSTR(sep); sep=bl;
		putu(t/60000);
		t %= 60000;
		PUTSTR("m");
	}
	if(t>1000) {
		PUTSTR(sep); sep=bl;
		putu(t/1000);
		t %= 1000;
		PUTSTR("s");
	}
	if(t>0 || !*sep) {
		PUTSTR(sep);
		putu(t);
		PUTSTR("ms");
	}
}

// ==================== CONFIG ====================
#define FIX_FRAC 24

typedef int f8p24;

#ifdef __TO8__
extern f8p24 fmul(f8p24 a, f8p24 b);
asm(
   "_fmul set *\n"
   "	VM_OFF\n"
   "	ldx	#0\n"
   "	lda	4,s\n"
   "	ldb	8+3,s\n"
   "	mul\n"
   "	std	<R0lo\n"
   
   "	lda	4+1,s\n"
   "	ldb	8+2,s\n"
   "	mul\n"
   "	addd	<R0lo\n"
   "	std	<R0lo\n"
   "	bcc	*+4\n"
   "	leax	1,x\n"
   "	lda	4+2,s\n"
   "	ldb	8+1,s\n"
   "	mul\n"
   "	addd	<R0lo\n"
   "	std	<R0lo\n"
   "	bcc	*+4\n"
   "	leax	1,x\n"
   "	lda	4+3,s\n"
   "	ldb	8+0,s\n"
   "	mul\n"
   "	addd	<R0lo\n"
   "	std	<R0lo\n"
   "	bcc	*+4\n"
   "	leax	1,x\n"
   "	stx	<R0hi\n"

   "	lda	4,s\n"
   "	ldb	8+2,s\n"
   "	mul\n"
   "	addb	<R0lo\n"
   "	stb	<R0lo\n"
   "	adca	<R0hi+1\n"
   "	sta	<R0hi+1\n"
   "	bcc	*+4\n"
   "	inc	<R0hi\n"
   "	lda	4+1,s\n"
   "	ldb	8+1,s\n"
   "	mul\n"
   "	addb	<R0lo\n"
   "	stb	<R0lo\n"
   "	adca	<R0hi+1\n"
   "	sta	<R0hi+1\n"
   "	bcc	*+4\n"
   "	inc	<R0hi\n"
   "	lda	4+2,s\n"
   "	ldb	8+0,s\n"
   "	mul\n"
   "	addb	<R0lo\n"
   "	stb	<R0lo\n"
   "	adca	<R0hi+1\n"
   "	sta	<R0hi+1\n"
   "	bcc	*+4\n"
   "	inc	<R0hi\n"

   "	lda	4,s\n"
   "	ldb	8+1,s\n"
   "	mul\n"
   "	addd	<R0hi\n"
   "	std	<R0hi\n"
   "	lda	4+1,s\n"
   "	ldb	8,s\n"
   "	mul\n"
   "	addd	<R0hi\n"
   "	std	<R0hi\n"

   "	lda	4,s\n"
   "	ldb	8,s\n"
   "	mul\n"
   "	addb	<R0hi\n"
   "	stb	<R0hi\n"

   "	ldb	4,s\n"
   "	bpl	_fmul_a\n"
   "	ldb	<R0lo\n"
   "	subb	8+3,s\n"
   "	stb	<R0lo\n"
   "	ldd	<R0hi\n"
   "	sbcb	8+2,s\n"
   "	sbca	8+1,s\n"
   "	std	<R0hi\n"
   "_fmul_a set *\n"
   "	ldb	8,s\n"
   "	bpl	_fmul_b\n"
   "	ldb	<R0lo\n"
   "	subb	4+3,s\n"
   "	stb	<R0lo\n"
   "	ldd	<R0hi\n"
   "	sbcb	4+2,s\n"
   "	sbca	4+1,s\n"
   "	std	<R0hi\n"
   "_fmul_b set *\n"

   "	jmp	<opRET	;");

extern f8p24 fsqr(f8p24 x);
asm(
   "_fsqr set *\n"
   "	VM_OFF\n"
   "	leay	4,s\n"
   "	lda	,y\n"
   "	bpl	*+4\n"
   "	jsr	<NEG_Y\n"
   "	ldx	#0\n"
   "	lda	,y\n"
   "	ldb	3,y\n"
   "	mul\n"
   "	lslb\n"
   "	rola\n"
   "	bcc	*+4\n"
   "	leax	1,x\n"
   "	std	<R0lo\n"
   "	ldd	1,y\n"
   "	mul\n"
   "	lslb\n"
   "	rola\n"
   "	bcc	*+4\n"
   "	leax	1,x\n"
   "	addd	<R0lo\n"
   "	bcc	*+4\n"
   "	leax	1,x\n"
   "	std	<R0lo\n"
   "	stx	<R0hi\n"

   "	lda	,y\n"
   "	ldb	2,y\n"
   "	mul\n"
   "	lslb\n"
   "	rola\n"
   "	bcc	*+4\n"
   "	inc	<R0hi\n"
   "	addb	<R0lo\n"
   "	stb	<R0lo\n"
   "	adca	<R0hi+1\n"
   "	sta	<R0hi+1\n"
   "	bcc	*+4\n"
   "	inc	<R0hi\n"
   "	lda	1,y\n"
   "	ldb	1,y\n"
   "	mul\n"
   "	addb	<R0lo\n"
   "	stb	<R0lo\n"
   "	adca	<R0hi+1\n"
   "	sta	<R0hi+1\n"
   "	bcc	*+4\n"
   "	inc	<R0hi\n"

   "	ldd	,y\n"
   "	mul\n"
   "	lslb\n"
   "	rola\n"
   "	addd	<R0hi\n"
   "	std	<R0hi\n"

   "	lda	,y\n"
   "	ldb	,y\n"
   "	mul\n"
   "	addb	<R0hi\n"
   "	stb	<R0hi\n"
   
   "	jmp	<opRET");
#else
f8p24 fmul(f8p24 a, f8p24 b) {
	unsigned long long m1 = (unsigned)a, m2 = (unsigned)b, m3 = m1*m2;
	f8p24 s3 = m3>>24;
	if (a < 0) s3 -= b<<8;
	if (b < 0) s3 -= a<<8;
	return s3;
}

f8p24 fsqr(f8p24 a) {
	unsigned long long m1 = (unsigned)a, m3 = m1*m1;
	f8p24 s3 = m3>>24;
	if (a < 0) s3 -= a<<9;
	return s3;
}
#endif

// #define fsqr(x) fmul(x,x)

// --- 3 USER CONSTANTS (in float) ---
// Verbatim from to8-mandel.c (the source of truth).
#define MAX_ITER 32
#define CH      2.20f   // Window height (CI_MAX - CI_MIN)
#define CR_MIN -2.50f    // Left (minimum real part)
#define CI_MIN -1.10f    // Bottom (minimum imaginary part)


// #define MAX_ITER 2048
// #define CH      0.01f
// #define CR_MIN	-0.7515f	
// #define CI_MIN	0.1265f
// #define COLOR(it, d) ((it) ? (MAX_ITER - (it) + (MAX_ITER/2)*(d))/(MAX_ITER/16) : 0)

typedef
	unsigned
#if MAX_ITER<256
	char
#elif MAX_ITER<65536
	short
#else
	int
#endif
	iter_t;

// float -> fixed-point conversion at compile time
#define FIX_ONE (1 << FIX_FRAC)
#define F2FIX(f) ((f8p24)((f) * FIX_ONE + 0.5))

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

// Pixel color from the iteration count: SAME formula + dithering as the
// to8-mandel.c reference (the (x^y)&1 checkerboard is part of the image).
// #define COLOR(it, d) ((it) ? (MAX_ITER - (it)) : 0)

// ==================== MANDELBROT ====================

// Number of iterate() calls = number of individually computed pixels.
// Filled (non-computed) pixels = WIDTH*HEIGHT - n_iter, displayed at exit.
struct config {
	iter_t max_iter;
	f8p24 cr, ci;
	
	f8p24 cr_min, ci_min;
	f8p24 cr_rng, ci_rng; 
	f8p24 cr_dlt, ci_dlt;
	unsigned char col[1024];
	
	short n_iter;
} conf;

#define INIT(max_iter, cr, ci, h) \
	init_conf(&conf, max_iter, F2FIX(cr), F2FIX(ci), F2FIX(h))

void init_conf(struct config *conf, iter_t max_iter,
	f8p24 cr, f8p24 ci, f8p24 height) {
	const int one = 1, 
	          max = sizeof(conf->col)/sizeof(conf->col[0]);
	unsigned char * const col = conf->col;
	
	const f8p24 width = fmul(height,F2FIX(1.6));
	
	if(max_iter>max) max_iter = max;
	
	conf->n_iter   = 0;
	conf->max_iter = max_iter;// + one;
	
	conf->cr = cr;
	conf->ci = ci;
	
	conf->cr_dlt = ((conf->cr_rng = width )+WIDTH/2)/WIDTH;
	conf->ci_dlt = ((conf->ci_rng = height)+HEIGHT/2)/HEIGHT;
	
	conf->cr_min = cr - conf->cr_dlt*(WIDTH/2);
	conf->ci_min = ci - conf->ci_dlt*(HEIGHT/2);
		
	// conf->cr_min = F2FIX(CR_MIN);
	// conf->ci_min = F2FIX(CI_MIN);
	// conf->cr_dlt = STEP_X;
	// conf->ci_dlt = STEP_Y;
	
	// fill colors
	for(int i = max, c = one; i -= one;) {
		col[i] = c;
		if(i<=max_iter && (c+=one)==32) c = one; 
	}
	col[0] = 0;
}

// Plot a pixel from its iteration count (reference formula + dither).
void plot_iter(int x, int y, int iter) {
	int c = conf.col[iter];
	xplot(x, y, c ? (c + ((x ^ y) & 1))>>1 : 0);
}


int iterate(f8p24 cr, f8p24 ci) {
    const int one = 1; 
    f8p24 zr = cr, zi = ci;
    int iter = conf.max_iter;
    
    f8p24 m, zr2, zi2, w2;

    conf.n_iter += one;

    do {
        zi2 = fsqr(zi);
	zr2 = fsqr(zr);
	m   = zr2 + zi2;
	
        w2  = fsqr(zr + zi);
        zi = ci + (w2 - m);       
        zr = cr + (zr2 - zi2);

        iter -= one;
    } while (iter && m < FIX_FOUR+1);

    return iter;
}

// Iteration of the pixel (bx+bw, y): the column just right of the stripe
// being processed. Filled by the init with the off-screen column x=WIDTH,
// then rewritten column-by-column by each stripe for ALL 200 lines.
static iter_t right[HEIGHT];

// Compute and plot one line of the stripe, pixel by pixel, right to left.
// cr = cr of the stripe's RIGHT pixel (x = bx+bw-1), ci = the line's ci,
// y = the line. Stores the LEFT pixel's value into right[y] (the next
// stripe's right column) and returns the common value if the line is
// uniform, -1 if not.
int line_uniform(f8p24 cr, f8p24 ci, int bx, int bw, int y, iter_t *right) {
	int x, v, col, one = 1;
	f8p24 step = conf.cr_dlt;

	col = v = iterate(cr, ci);
	x = bw - one;
	plot_iter(bx + x, y, col);
	while(x) {
		v = iterate(cr -= step, ci);
		plot_iter(bx + (x -= one), y, v);
		if (v-col) {
			while(x) {
				v = iterate(cr -= step, ci);
				plot_iter(bx + (x -= one), y, v);
			} 
			col= -1;
			break;
		}
	}
	if(right) *right = v;          // v = left pixel (x = bx)
	return col;
}

// Process one vertical stripe [bx..bx+bw-1], bottom-up.
// cr = cr of the stripe's RIGHT pixel (x = bx+bw-1) - constant for the
// whole stripe. ci = ci of the bottom line (y = HEIGHT-1).
void stripe(int bx, int bw, f8p24 cr, f8p24 ci) {
	const f8p24 step_y = conf.ci_dlt, step_x = conf.cr_dlt;
	const int one = 1;
	
	// cr of the stripe's LEFT column x = bx (cr decreases leftwards)
	const f8p24 crL = cr + (one-bw)*step_x;
	
	iter_t *prev = &right[HEIGHT];
	int old, col, v, y;
	
	for (/*ci -= step_y, */y = HEIGHT-1; y;) {
		// --- phase 1: plain line scan until a uniform line (or y < 0) ---
		do {
			ci += step_y; y -= one;
			col = line_uniform(cr, ci, bx, bw, y, --prev);
		} while (col<0 && y);
		if (!y) return;              // stripe finished without rectangle
		int yu = y;

		// --- phase 2: sides only, one iterate() per line, walking up ---
		do {
			y -= one; 
			v = iterate(crL, ci += step_y);  // left pixel (x = bx), plotted
			plot_iter(bx, y, v);
			old = *--prev;              // column x = bx+bw, already known
			*prev = v;
		} while(!(v-col) && !(v-old) && y);
		int yf = y; f8p24 ciF = ci;   // ci(yf), to resume the scan (phase 4)

		// --- phase 3: interior, checked from its bottom line upward ---
		bw -= one; bx += one;
		for(v=-1; y<yu && (v-col); ci -=step_y, y += one) {
			v = line_uniform(cr, ci, bx, bw, y, NULL);
		}
		// rectangle [y-1..yu] confirmed: fill the lines above yc with no
		// iterate() at all - left edge already plotted in phase 2,
		// interior + right edge get the rectangle value
		for(; y < yu; y += one) {
			for(int x = bw; (x -= one)>=0;) {
				plot_iter(x + bx, y, col);
			}
		}
		bw += one; bx -= one;

		// --- phase 4: resume the plain scan at yf (its interior has not
		//     been computed yet; the scan recomputes the whole line) ---
		ci = ciF; y = yf;
	
		// debug
		// plot(bx, y, 15);
	}
}

void mandelbrot(void) {
	const int one = 1;
	const f8p24 cr_base = conf.cr_min + conf.cr_rng, 
	            ci_base = conf.ci_min,
		    step_x  = conf.cr_dlt*BLK_W,
		    step_y  = conf.ci_dlt;
	
	int x, y;
	f8p24 ci,cr;
	
	//todo etalonate colors

	// init right[] with the off-screen column x = WIDTH (just right of the
	// screen): computed, never plotted. ci runs bottom -> top.
	cr = cr_base + conf.cr_dlt; ci = ci_base /* - step_y */;
	for(y = HEIGHT; y;) {
		right[y -= one] = iterate(cr, ci += step_y);
	}

	// stripes right to left. cr = cr of the stripe's RIGHT pixel; ci of the
	// bottom line is CI_BASE for every stripe (each stripe sweeps the full
	// height). The first stripe's right pixel is x = WIDTH-1.
	for(cr = cr_base + step_x, x = WIDTH; (x-=BLK_W)>0;) {
		stripe(x, BLK_W, cr -= step_x, ci_base);
	}
	if((x += BLK_W)>=2) stripe(0, x, cr - step_x, ci_base);
	else if(x) for(ci = ci_base - step_y, cr -= step_x, y = HEIGHT; y;) {
		plot_iter(0, y -= one, iterate(cr, ci += step_y));
	}
}

void main(int ac,  char **av) {
	unsigned t;
	int fill;
	cursor(0);
	border(0);
	esc(GFX_MODE_BM16);
	setPalette();

	INIT(32, -0.74f ,0.00f, 2.20f);

	t = readMillis();
	mandelbrot();
	t = readMillis() - t;

	beep(); while(!GETC());
	
	esc(GFX_MODE_40);
	paper(0);ink(15);cls();
	PUTSTR("Mandelbrot stripes ");
	putu(BLK_W);
	PUTSTR("px in ");
	put_time(t);
	PUTSTR("\r\n");
	PUTSTR("calc: ");
	putu(conf.n_iter);
	PUTSTR("  fill: ");
	fill = (WIDTH+1)*HEIGHT - conf.n_iter;
	puti(fill);
	PUTSTR("\r\n");
}
