* to8-vm.asm kind of crt0.o
	org	$9000

R0	FDB	0,0
       
crt0	pshs	d,x,y,u,dp,cc
	ldd	#R0&$FF00
	setdp	R0/256
	tfr	a,dp
	sts	<__exit+2
*  ac=0	 av=NULL
	clra
	tfr	d,x
	pshs	d,x
	pshs	d,x
	ldx	#__exit
	pshs	d,x
	ldu	#_main
	pulu	pc
__exit	lds	#0
	puls	d,x,y,u,dp,cc,pc


to8_vm	puls	u
	pulu	pc

TO8_VM	macro
	jsr	<to8_vm
	endm
  
NATIVE	macro
	fdb	*+2
	endm
	
opADJi	pulu	d,y
	leas	b,s
	jmp	,y
ADJi	macro
	fdb	opADJi,\0
	endm

opLD1	pulu	d,y
	leax    b,s
	ldb	[2,x]
	sex
	std	<R0+2
	sta	<R0+1
	sta	<R0
	jmp	,y
LD1	macro
	fdb	opLD1,\0
	endm
	
opJEQ	ldd	<R0+2
	beq	opSKP+2
opSKP	pulu	d,y,pc
	ldd	<R0
	bne	opSKP
opJMP	pulu	d,x
        leau    2,x
        jmp     ,x
JEQ	macro
	fdb	opJEQ,0,\0
	endm
JRA	macro
	fdb	opJMP,0,\0
	endm

opMOV	ldd	,u
	leax	a,s
	leay	b,s
	ldd	,y
	std	,x
	ldd	2,y
	std	2,x
	pulu	d,pc
MOV	macro
	fdb	opMOV,\0*256+\1
	endm

opLDi	pulu	d,x,y
	std	<R0
	stx	<R0+2
	jmp	,y
LDi	macro
	fdb	opLDi,\0,\1
	endm

opADD	pulu	d,y
	leax	b,s
	ldd	2,x
	addd	<R0+2
	std	<R0+2
	bcc	opADD1
	ldd	,x
	adcb	<R0+1
	adca	<R0
	std	<R0
opADD1	jmp	,y
ADD	macro
	fdb	opADD,\0
	endm
	
opST	pulu	d,y
	leax	b,s
	ldd	<R0
	std	,x
	ldd	<R0+2
	std	2,x
	jmp	,y
ST	macro
	fdb	opST,\0
	endm

opPUSHi pulu    d,x,y
        pshs    d,x
        jmp     ,y
PUSHi   macro
        fdb     opPUSHi,0,\0
        endm
        
opPUSHr ldd	<R0
	ldx	<R0+2
	pshs	d,x
	pulu	pc
PUSHr	macro
	fdb	opPUSHr
	endm
	
opJSRi	pulu	d,x
	pshs	d,u
        leau    ,x
        pulu    pc
CALLi	macro	     
	fdb	opJSRi,0,\0
	endm
	
opRET	puls	d,u
	pulu	pc
RET	macro
	fdb	opRET
	endm	    

(info)