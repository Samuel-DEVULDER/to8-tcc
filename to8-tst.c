#if 1
char *f(char *s, char *d) {
	while(*d++ = *s++);
	return d-1;
}

// swap two elements of given size
void swap(void* v1, void* v2, int size)
{
	unsigned char *d=v1,*s=v2;
	if(size)  do {
		unsigned char t = *d;
		*d = *s;
		*s = t;
		++s; ++d;
	} while(--size);
}

char *g(char  *s, char *d) {
	while((unsigned)(*d = *s)){++s;++d;}
	return d;
}

char *h(char *s, char *d) {
    *d = *s;
    return d;
}

void copy(char *s, char *d) { while (*d++ = *s++); }
char *wrapper(char *a, char *b) {
    copy(a, b);
    return b;
}



// generic quicksort
// v: array, size: element size
// left/right: range
// comp: comparison function
void _qsort(void* v, int size, int left, int right,
                      int (*comp)(void*, void*))
{
    void *vt, *v3;
    int i, last, mid = (left + right) / 2;

    if (left >= right)
        return;

    // cast to char* for pointer arithmetic
    void* vl = (char*)(v + (left * size));
    void* vr = (char*)(v + (mid * size));

    swap(vl, vr, size);
    last = left;

    for (i = left + 1; i <= right; i++) {

        // element address
        vt = (char*)(v + (i * size));

        if ((*comp)(vl, vt) > 0) {
            ++last;
            v3 = (char*)(v + (last * size));
            swap(vt, v3, size);
        }
    }

    v3 = (char*)(v + (last * size));
    swap(vl, v3, size);

    _qsort(v, size, left, last - 1, comp);
    _qsort(v, size, last + 1, right, comp);
}

int mandel(int count, float cx,  float cy) {
	float x=0, y=0, x2, y2; int i;
	for(i=0; i<count && ((x2=x*x)+(y2=y*y))<=4; ++i) {
		y = 2*x*y + cy;
		x = x2 - y2 + cx;
	}
	return i;
}

int mandel2(int count, double cx, double cy) {
	double x=0, y=0, x2=0, y2=0; int i;
	for(i=0; i<count && (x2+y2)<=4; ++i) {
		y = 2*x*y + cy;
		x = x2 - y2 + cx;
		x2=x*x; y2=y*y;
	}
	return i;
}

int tst_ext(char  *a, unsigned short *b) {
	return *a+*b;
}

/*
int* tst_vla(int  size) {
	int tab[size];
	tab[0] = 1;
	tab[size-1] =  2;
	return tab;
}
*/
void putc(char c);

void puts(char *s) {
	while(*s)  putc(*s++);
}


void test_puts(void) {
	puts("hello, world!");
}

#define __native_asm(x)  asm(x "\t;")

void putc(char c) {
	__native_asm("\tldb\t7,s");
	__native_asm("\tjsr\t$E803");
}

void test_asm(void) {	
	__native_asm("; a,a");
	__native_asm("test rien ");
	__native_asm("test2 ");
	__native_asm("test $123");
}

__native_asm ("\ttest2 a,b\nlabel:\n  code 0x1111");

int mul_simple(int a, int b) { return a * b; }
int mul_complex(int a, int b, int c) { return a * b + c; }
int mul_complex2(int a, int b, int c, int d) { return a * b + c*d; }

int mandel3(int count, float cx,  float cy) {
	float x, y, x2, y2; int i;
	y2=x2=x=y=0;
	for(i=0; i<count && y2+x2<=4; ++i) {
		y = 2*x*y + cy;
		x = x2 - y2 + cx;
		x2 = x*x;
		y2 = y*y;
	}
	return i;
}


#endif