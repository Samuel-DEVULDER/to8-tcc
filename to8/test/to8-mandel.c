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

void puts(char *s) {
	while(*s)  putc(*s++);
}

void cls(void) {
	putc('\f');
}

void cursor(int on)  {
	putc(on ? 0x11 : 0x14);
}

void  locate(int x, int y) {
	putc(0x1F);
	putc(0x40+y);
	putc(0x41+x);
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
/*
void setRGB8(int col, int rgb) {
	int c;
	rgb >>= 4;
	c = (rgb&15)<<8;
	rgb >>= 4;
	c |= rgb&0xf0;
	rgb >>= 12;
	palette(col, 0, c|rgb);
}
*/
#define setRGB8(col, rgb) palette(col,0,(((rgb<<4)&0xf00)+((rgb>>8)&0xf0)+((rgb>>20)&0xf)))

enum  {
	GFX_MODE_BM4 = 0x59,
	GFX_MODE_40 = 0x5A,
	GFX_MODE_80 = 0x5B,
	GFX_MODE_BM16 = 0x5E,
};

void setPalette(void) {
	setRGB8(0, 0x000000);  // noir = intérieur
	
	// Dégradé électrique (1-15)
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

void pattern() {
	int x,y,mask=15;
	for(y=0;y<200;++y) {
		for(x=0;x<160;++x) {
			plot(x,y,(x^(y>>1))&mask);
		}
	}
}

// ==================== CONFIG ====================
#define FIX_FRAC 13
#define FIX_ONE (1 << FIX_FRAC)

// Conversion float → fixed-point à la compilation
#define F2FIX(f) ((int)((f) * FIX_ONE + 0.5))

// --- 3 CONSTANTES UTILISATEUR (en float) ---
#define CH      2.20f   // Hauteur de la fenêtre (CI_MAX - CI_MIN)
#define CR_MIN -2.50f    // Gauche (réel minimum)
#define CI_MIN -1.10f    // Bas (imaginaire minimum)

// --- Dimensions écran ---
#define WIDTH  160
#define HEIGHT 200

// --- Calculs automatiques ---
// Aspect ratio: largeur = hauteur * (WIDTH / HEIGHT)
#define CW ((2*CH*WIDTH) / (float)HEIGHT)  // Largeur = 2.0 * (2*160/200) = 3.2

#define CR_MAX (CR_MIN + CW)  // -2.1 + 3.2 = 1.1
#define CI_MAX (CI_MIN + CH)  // -1.0 + 2.0 = 1.0

// Steps en fixed-point
#define STEP_X F2FIX(CW / WIDTH)      
#define STEP_Y F2FIX(CH / HEIGHT)

// Coin haut-gauche en fixed-point
#define CR_BASE F2FIX(CR_MAX)
#define CI_BASE F2FIX(CI_MIN)

// Constantes pour Mandelbrot
#define FIX_FOUR F2FIX(4.0f)
#define FIX_MUL_SHIFT (FIX_FRAC)
#define FIX_2MUL_SHIFT (FIX_FRAC - 1)

#define MAX_ITER 32

// ==================== MANDELBROT ====================
void mandelbrot(void) {
    int x, y, ci, cr;
    
    for(ci = CI_BASE, y = HEIGHT-1; y>=0; ci +=  STEP_Y, --y) {
        for(cr = CR_BASE, x = WIDTH-1; x>=0; cr -= STEP_X, --x) {
	    unsigned zr2 = 0, zi2 = 0;
            int zr = 0, zi = 0;
            int iter = MAX_ITER;
            
	    do {
		zi = ((zr * zi) >> FIX_2MUL_SHIFT) + ci;
		zr = zr2 - zi2 + cr;
		    
                zr2 = ((unsigned)(zr * zr)) >> FIX_MUL_SHIFT;
                zi2 = ((unsigned)(zi * zi)) >> FIX_MUL_SHIFT;
            } while(--iter && (zr2 + zi2) < (FIX_FOUR+1));
           
	    plot(x,y, iter ? (MAX_ITER - iter + ((x^y)&1))>>1 : 0);
        }
    }
}

void main(int ac,  char **av) {
	cursor(0);
	border(0);
	esc(GFX_MODE_BM16);
	setPalette();
	//pattern();
	mandelbrot();
	beep();
	while(!getc());
	esc(GFX_MODE_40);
	paper(0);ink(15);cls();
	puts("hello, world!\r\n");
}


