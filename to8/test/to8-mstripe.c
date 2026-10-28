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

void xplot(int x, int y, int col) {
#ifdef  __TO8__ 
        native(
        "ldd    8+2,s\n\t"
        "cmpd   #200\n\t"
        "bhs    xplot_done\n\t"
        "lda    #40\n\t"
        "mul\n\t"
        "adda   #$40\n\t"
        "tfr    d,x\n\t"
        "ldd    4+2,s\n\t"
        "cmpd   #160\n\t"
        "bhs    xplot_done\n\t"
        "lda    $e7c3\n\t"
        "lsrb\n\t"
        "rora\n\t"
        "lsrb\n\t"
        "abx\n\t"
        "rola\n\t"
        "eora   #1\n\t"
        "sta    $e7c3\n\t"
        "ldb    12+3,s\n\t"
        "andb   #15\n\t"
        "bcs    *+6\n\t"
        "lslb\n\t"
        "lslb\n\t"
        "lslb\n\t"
        "lslb\n\t"
        "eorb   ,x\n\t"
        "stb    ,x\n"
        "xplot_done set *\n\t"
        "puls   d,u\n\t"
        "pulu   pc"
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

#ifdef  __TO8__
extern unsigned readMillis(void);
asm("_readMillis set  * ; \n"
    "   LDCLK           ; read clock (1/10ms)\n"
    "   MULi    0,100   ; mul by 100 --> ms\n"
    "   RET                     ;");
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

enum  {
        GFX_MODE_BM4 = 0x59,
        GFX_MODE_40 = 0x5A,
        GFX_MODE_80 = 0x5B,
        GFX_MODE_BM16 = 0x5E,
};

/* Palette table for ONE whole-palette SETP call (A=$FF, X=table): 16
 * unsigned short words, $0BGR format (Y = (B<<8)+(G<<4)+R, M=0). Restored
 * from git HEAD 378e4632 ("better palette"): the Google "Turbo" colormap
 * (A. Mikhailov et al., 2019; matplotlib 'turbo') sampled at t = i/14,
 * quantized to the HIGH nibbles of RGB888 -> $0BVR word (as the old
 * per-color SETP path was fed). Index 1 = t 0.0 (dark purple,
 * outer bands), 15 = t 1.0 (dark red, next to the set): the c-cycle wrap
 * dithers two L~50 darks, red(198,0,0) <-> purple(149,103,149), instead of
 * clashing full-intensity red(255) on navy.
 *                                                R,V,B     word  display */
#if 0
static const unsigned short pal[16] = {  /* DCMOTO arc (full-intensity red) */
        0x0000, 0x0100, 0x0410, 0x0630, 0x0760, 0x0690, 0x04B0, 0x02D0,
        0x00F0, 0x00F1, 0x00E3, 0x00D5, 0x00C8, 0x008B, 0x004E, 0x000F,
};
#else
static const unsigned short pal[16] = {  /* Turbo sampled t=i/14 */
        0x0000, /*  0 (  0,  0,  0) black = interior            */
        0x0313, /*  1 (149,103,149) t=0.000 rgb(48,18,59)       */
        0x0A44, /*  2 (164,164,223) t=0.071 rgb(65,70,172)      */
        0x0E74, /*  3 (164,198,243) t=0.143 rgb(71,118,238)     */
        0x0FA3, /*  4 (149,223,255) t=0.214 rgb(58,163,252)     */
        0x0DC2, /*  5 (130,237,243) t=0.286 rgb(27,208,213)     */
        0x0AE2, /*  6 (130,243,223) t=0.357 rgb(37,236,167)     */
        0x06F6, /*  7 (188,255,188) t=0.429 rgb(97,252,108)     */
        0x04FA, /*  8 (223,255,164) t=0.500 rgb(164,252,60)     */
        0x03EC, /*  9 (237,243,149) t=0.571 rgb(210,233,53)     */
        0x03CE, /* 10 (243,237,149) t=0.643 rgb(244,199,58)     */
        0x039F, /* 11 (255,215,149) t=0.714 rgb(254,155,45)     */
        0x016E, /* 12 (243,188,103) t=0.786 rgb(243,99,21)      */
        0x003D, /* 13 (243,149,  0) t=0.857 rgb(218,57,7)       */
        0x002A, /* 14 (223,130,  0) t=0.929 rgb(178,26,1)       */
        0x0007, /* 15 (198,  0,  0) t=1.000 rgb(122,4,3) DARK RED */
};
#endif

void setPalette(void) {
        SETP(0xFF, (int)pal, 0);
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

// float -> fixed-point conversion at compile time
#define FIX_ONE (1 << FIX_FRAC)
#define F2FIX(f) ((f8p24)((f) * FIX_ONE + 0.5))

// Mandelbrot constants
#define FIX_FOUR F2FIX(4.0f)

// --- Screen dimensions ---
#define WIDTH  160
#define HEIGHT 200

// --- Stripe width (the left-most stripe is narrower if BLK_W does not
//     divide WIDTH) ---
#define BLK_W 5

typedef int f8p24;

#ifdef __TO8__
extern f8p24 fmul(f8p24 a, f8p24 b);
asm(
   "_fmul set *\n"
   "    VM_OFF\n"
   "    ldx     #0\n"
   "    lda     4,s\n"
   "    ldb     8+3,s\n"
   "    mul\n"
   "    std     <R0lo\n"
   
   "    lda     4+1,s\n"
   "    ldb     8+2,s\n"
   "    mul\n"
   "    addd    <R0lo\n"
   "    std     <R0lo\n"
   "    bcc     *+4\n"
   "    leax    1,x\n"
   "    lda     4+2,s\n"
   "    ldb     8+1,s\n"
   "    mul\n"
   "    addd    <R0lo\n"
   "    std     <R0lo\n"
   "    bcc     *+4\n"
   "    leax    1,x\n"
   "    lda     4+3,s\n"
   "    ldb     8+0,s\n"
   "    mul\n"
   "    addd    <R0lo\n"
   "    std     <R0lo\n"
   "    bcc     *+4\n"
   "    leax    1,x\n"
   "    stx     <R0hi\n"

   "    lda     4,s\n"
   "    ldb     8+2,s\n"
   "    mul\n"
   "    addb    <R0lo\n"
   "    stb     <R0lo\n"
   "    adca    <R0hi+1\n"
   "    sta     <R0hi+1\n"
   "    bcc     *+4\n"
   "    inc     <R0hi\n"
   "    lda     4+1,s\n"
   "    ldb     8+1,s\n"
   "    mul\n"
   "    addb    <R0lo\n"
   "    stb     <R0lo\n"
   "    adca    <R0hi+1\n"
   "    sta     <R0hi+1\n"
   "    bcc     *+4\n"
   "    inc     <R0hi\n"
   "    lda     4+2,s\n"
   "    ldb     8+0,s\n"
   "    mul\n"
   "    addb    <R0lo\n"
   "    stb     <R0lo\n"
   "    adca    <R0hi+1\n"
   "    sta     <R0hi+1\n"
   "    bcc     *+4\n"
   "    inc     <R0hi\n"

   "    lda     4,s\n"
   "    ldb     8+1,s\n"
   "    mul\n"
   "    addd    <R0hi\n"
   "    std     <R0hi\n"
   "    lda     4+1,s\n"
   "    ldb     8,s\n"
   "    mul\n"
   "    addd    <R0hi\n"
   "    std     <R0hi\n"

   "    lda     4,s\n"
   "    ldb     8,s\n"
   "    mul\n"
   "    addb    <R0hi\n"
   "    stb     <R0hi\n"

   "    ldb     4,s\n"
   "    bpl     _fmul_a\n"
   "    ldb     <R0lo\n"
   "    subb    8+3,s\n"
   "    stb     <R0lo\n"
   "    ldd     <R0hi\n"
   "    sbcb    8+2,s\n"
   "    sbca    8+1,s\n"
   "    std     <R0hi\n"
   "_fmul_a set *\n"
   "    ldb     8,s\n"
   "    bpl     _fmul_b\n"
   "    ldb     <R0lo\n"
   "    subb    4+3,s\n"
   "    stb     <R0lo\n"
   "    ldd     <R0hi\n"
   "    sbcb    4+2,s\n"
   "    sbca    4+1,s\n"
   "    std     <R0hi\n"
   "_fmul_b set *\n"

   "    jmp     <opRET  ;");

extern f8p24 fsqr(f8p24 x);
asm(
   "_fsqr set *\n"
   "    VM_OFF\n"
   "    leay    4,s\n"
   "    lda     ,y\n"
   "    bpl     *+4\n"
   "    jsr     <NEG_Y\n"
   "    ldx     #0\n"
   "    lda     ,y\n"
   "    ldb     3,y\n"
   "    mul\n"
   "    lslb\n"
   "    rola\n"
   "    bcc     *+4\n"
   "    leax    1,x\n"
   "    std     <R0lo\n"
   "    ldd     1,y\n"
   "    mul\n"
   "    lslb\n"
   "    rola\n"
   "    bcc     *+4\n"
   "    leax    1,x\n"
   "    addd    <R0lo\n"
   "    bcc     *+4\n"
   "    leax    1,x\n"
   "    std     <R0lo\n"
   "    stx     <R0hi\n"

   "    lda     ,y\n"
   "    ldb     2,y\n"
   "    mul\n"
   "    lslb\n"
   "    rola\n"
   "    bcc     *+4\n"
   "    inc     <R0hi\n"
   "    addb    <R0lo\n"
   "    stb     <R0lo\n"
   "    adca    <R0hi+1\n"
   "    sta     <R0hi+1\n"
   "    bcc     *+4\n"
   "    inc     <R0hi\n"
   "    lda     1,y\n"
   "    ldb     1,y\n"
   "    mul\n"
   "    addb    <R0lo\n"
   "    stb     <R0lo\n"
   "    adca    <R0hi+1\n"
   "    sta     <R0hi+1\n"
   "    bcc     *+4\n"
   "    inc     <R0hi\n"

   "    ldd     ,y\n"
   "    mul\n"
   "    lslb\n"
   "    rola\n"
   "    addd    <R0hi\n"
   "    std     <R0hi\n"

   "    lda     ,y\n"
   "    ldb     ,y\n"
   "    mul\n"
   "    addb    <R0hi\n"
   "    stb     <R0hi\n"
   
   "    jmp     <opRET");
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

typedef unsigned short iter_t;


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
        
        // iterate() returns up to max_iter: keep it <= 1023 so the col[]
        // access in plot_iter stays in bounds (fixes the latent col[1024]
        // overread of the full view - the pixels that escaped on the first
        // check now fall on the outermost band instead of reading garbage).
        if(max_iter >= max) max_iter = max - 1;
        
        conf->n_iter   = 0;
        conf->max_iter = max_iter + one;
        
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
        
        // fill colors: cyclic palette of 31 levels, c = 1..31 = 15 hardware
        // colors x (solid + 50/50 checkerboard toward the NEXT color), the
        // 31 -> 1 wrap being graded by the same checkerboard. Stored
        // pre-shifted by +1 so that plot_iter only does (p + ((x+y)&1))>>1.
        for(int c = one, i = max; i -= one;) {
                col[i] = c + 1;
                if(i<=max_iter && (c+=one)==32) c = one;
        }
        col[0] = 0;
}

// Plot a pixel from its iteration count. conf.col[] maps the iteration to
// the 31-level cycle, pre-shifted by +1 at fill time, so the only work is
// (p + ((x+y)&1)) >> 1: level p becomes the 50/50 (x+y)&1 checkerboard of
// hardware colors p..p+1, and 16 folds back to 1 (the red <-> purple wrap).
// col[0] = 0 still gives the black interior with no extra test.
void plot_iter(int x, int y, int iter) {
	int p = conf.col[iter];
	if(p) {
		p = (p+((x+y)&1))>>1;
		xplot(x, y, p==16?1:p);
	}
}


int iterate(f8p24 cr, f8p24 ci) {
    const int one = 1; 
    f8p24 zr = cr, zi = ci;
    int iter = conf.max_iter;
    
    f8p24 m, zr2, zi2, w2;

    conf.n_iter = one + conf.n_iter;   /* global load on the right: LD2m + ADD direct, no spill */

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
        
        for (/*ci -= step_y,*/ y = HEIGHT; y;) {
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
        
        // TODO: calibrate colors

        // init right[] with the off-screen column x = WIDTH (just right of the
        // screen): computed, never plotted. ci runs bottom -> top.
        cr = cr_base + conf.cr_dlt; ci = ci_base /*- step_y*/;
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

        // INIT(32, -0.74f ,0.00f, 2.20f);
        /* ── The classics ───────────────────────────────────────────── */
        INIT(32,  -0.740000, 0.000000, 2.2000);  /* full view (yours)          */
        // INIT(1024,  -0.744300, 0.131800, 0.0050); /* Seahorse Valley     */
        // INIT(1024, -0.7436439, 0.1318259, 0.0025);  /* Misiurewicz point, centered */
        // INIT(1024, -0.7268953, 0.1888871, 0.0025);  /* Triple Spiral / Julia Island */
        // INIT(256, -1.764878, 0.000000, 0.0600);
        // INIT(96,   0.277000, 0.004000, 0.0040);  /* Elephant Valley (trunks)         */
        // INIT(128, -0.726900, 0.188900, 0.0040);  /* Triple Spiral / "Julia Island"    */
        // INIT(96,  -0.745300, 0.112700, 0.0080);  /* double spiral ("Sandstorm")      */
        // INIT(96,  -0.101100, 0.956300, 0.0050);  /* Scepter Valley (M3,1)             */
        
        /* ── Self-similar structures on the real axis ────────────────── */
        // INIT(128, -1.401155, 0.000000, 0.0200);  /* Feigenbaum point (cascade)     */
        // INIT(96,  -1.754878, 0.000000, 0.0150);  /* perfect mini-Mandelbrot (period 3) */
        // INIT(160, -1.940800, 0.000000, 0.0030);  /* period-4 mini-Mandel (deeper) */
        // INIT(96,  -0.152800, 1.039700, 0.0300);  /* 2nd Feigenbaum point (p3 bulb) */

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
