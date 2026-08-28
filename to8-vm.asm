* to8-vm.asm kind of crt0.o
	org	$9000
	
BANKING set	0

init	bra	crt0

R0	FDB	0,0
TMP4	FDB	0,0
       
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
	ldx	#__exit-2
	pshs	d,x
	ldu	#_main
	pulu	pc
        fdb     __exit
__exit	lds	#0
	puls	d,x,y,u,dp,cc,pc


go_vm	puls	u
	pulu	pc

opADJi	pulu	d,y
	leas	b,s
	jmp	,y

opLD1r	ldx	#R0
	bra	opLD1_
opLD1	pulu	d,y
	leax	b,s
opLD1_  ldb	[2,x]   ; TODO banking
	sex		; clra pour LDU1
	std	<R0+2
	sta	<R0+1
	sta	<R0
	jmp	,y
	
opJEQ	ldd	<R0+2
	beq	opSKP+2
opSKP	pulu	d,y,pc
	ldd	<R0
	bne	opSKP
opJMP	ldu	2,u     ; TODO banking
	pulu	pc

opMOV	ldd	,u
	leax	a,s
	leay	b,s
	ldd	,y
	std	,x
	ldd	2,y
	std	2,x
	pulu	d,pc

opLDi	pulu	d,x,y
	std	<R0
	stx	<R0+2
	jmp	,y

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
	
opST	pulu	d,y
	leax	b,s
	ldd	<R0
	std	,x
	ldd	<R0+2
	std	2,x
	jmp	,y

opPUSHi pulu	d,x,y
	pshs	d,x
	jmp	,y
	
opPUSHr ldd	<R0
	ldx	<R0+2
	pshs	d,x
	pulu	pc
	
opJSRi	pulu	d,x
	pshs	d,u
	leau	,x
	pulu	pc
	
opRET	set	*
	puls	d,u
	pulu	pc

ADD	macro
	fdb	opADD,\0
	endm
ADJi	macro
	fdb	opADJi,\0
	endm
CALLi	macro	     
	fdb	opJSRi,0,\0
	endm
LD1	macro
	fdb	opLD1,\0
	endm
LD1r	macro
	fdb	opLD1r
	endm	    
LDi	macro
	fdb	opLDi,\0,\1
	endm
JEQ	macro
	fdb	opJEQ,0,\0
	endm
JRA	macro
	fdb	opJMP,0,\0
	endm
MOV	macro
	fdb	opMOV,\0*256+\1
	endm
PUSHi	macro
	fdb	opPUSHi,0,\0
	endm
PUSHr	macro
	fdb	opPUSHr
	endm
ST	macro
	fdb	opST,\0
	endm
RET	macro
	fdb	opRET
	endm	    
VM_OFF	macro
	fdb	*+2
	endm
VM_ON	macro
	jsr	<go_vm
	endm  

(info)