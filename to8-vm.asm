* to8-vm.asm kind of crt0.o
*
* Lightweigh. No banking system here.

	org	$9000

init	bra	crt0

R0	FDB	0,0
R1	FDB	0,0
       
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
	fdb	__exit
__exit	lds	#0
	puls	d,x,y,u,dp,cc,pc


go_vm	puls	u
	pulu	pc

* stack
opADJi	pulu	d,y
	leas	b,s
	jmp	,y

opADJwi	pulu	d,y
	leas	d,s
	jmp	,y

opPUSHr ldd	<R0
	ldx	<R0+2
	pshs	d,x
	pulu	pc

opPUSH	pulu	d,y
	leax	b,x
	ldd	,x
	ldx	2,x
	pshs	d,x
	jmp	,y
	
opPUSHi pulu	d,x,y
	pshs	d,x
	jmp	,y
	
* load
opLD	pulu	d,y
	leax	b,s
	ldd	,x
	ldx	2,x
opLD_	std	<R0
	stx	<R0+2
	jmp	,y
	
opLDi	pulu	d,x,y
	std	<R0
	stx	<R0+2
	jmp	,y

opLD1r	ldx	#R0
	bra	opLD1_
opLD1	pulu	d,y
	leax	b,s
opLD1_	ldb	[2,x]	; TODO banking
	sex
	std	<R0+2
	sta	<R0+1
	sta	<R0
	jmp	,y

opLDU1r	ldx	#R0
	bra	opLDU1_
opLDU1	pulu	d,y
	leax	b,s
opLDU1_	ldb	[2,x]	; TODO banking
	clra
	std	<R0+2
	clrb
	std	<R0
	jmp	,y

opLD2r	ldx	#R0
	bra	opLD2_
opLD2	pulu	d,y
	leax	b,s
opLD2_	ldd	[2,x]	; TODO banking
	std	<R0+2
	bge	opLD2_0
	ldb	#-1
	fcb	$81	; CMPA#
opLD2_0 clrb
	sex	   
	std	<R0
	jmp	,y

opLDU2r	ldx	#R0
	bra	opLDU2_
opLDU2	pulu	d,y
	leax	b,s
opLDU2_ ldd	[2,x]	; TODO banking
	std	<R0+2
	ldd	#0
	std	<R0
	jmp	,y

opLDU4r	ldx	#R0
	bra	opLDU4_
opLDU4	pulu	d,y
	leax	b,s
opLDU4_ ldx	2,x    ; TODO banking
	ldd	,x
	ldx	2,x
	std	<R0
	stx	<R0+2
	jmp	,y
	
* store
opST1	pulu	d,y
	leax	b,s
	lda	<R0+3
	sta	[2,x]	; TODO banking
	jmp	,y

opST2	pulu	d,y
	leax	b,s
	ldd	<R0+2
	std	[2,x]	; TODO banking
	jmp	,y

opST4	pulu	d,y
	addb	#2
	ldx	b,s
	ldd	<R0
	std	,x
	ldd	<R0+2
	std	2,x
	jmp	,y

opST	pulu	d,y
	leax	b,s
	ldd	<R0
	std	,x
	ldd	<R0+2
	std	2,x
	jmp	,y

* jump
opJNE	ldd	<R0+2
	bne	opJRA
	ldd	<R0
	bne	opJRA
	pulu	y,pc
	
opJEQ	ldd	<R0+2
	beq	opJEQ2

opJRN	pulu	y,pc

opJEQ2	ldd	<R0
	bne	opJRN
	
opJRA	ldu	,u	; TODO banking
	pulu	pc
	
opJGE	lda	<R0
	bpl	opJRA
	pulu	y,pc

opJLT	lda	<R0
	bmi	opJRA
	pulu	y,pc
	  
opJGT	ldd	<R0
	bmi	opJRN
	bpl	opJRA
	ldd	<R0+2
	bne	opJRA
	pulu	y,pc
	
opBLE	ldd	<R0
	bmi	opJRA
	bpl	opJRN
	ldd	<R0+2
	beq	opJRA
	pulu	y,pc
	

opMOV	ldd	,u
	leax	a,s
	leay	b,s
	ldd	,y
	std	,x
	ldd	2,y
	std	2,x
	pulu	d,pc

* arith
opADD2	pulu	d
	leax	a,s
	leay	b,s
	bra	opADD1
opADDi	leay	,u
	leau	4,u
	bra	opADD0
opADD	pulu	d
	leay	b,s
opADD0	ldx	#R0
opADD1	ldd	2,x
	addd	2,y
	std	<R0+2
	ldd	,x
	adcb	1,y
	adca	,y
	std	<R0
	pulu	pc
	
opSUB2	pulu	d
	leax	a,s
	leay	b,s
	bra	opSUB1
opSUBi	leay	,u
	leau	4,u
	bra	opSUB0
opSUB	pulu	d
	leay	b,s
opSUB0	ldx	#R0
opSUB1	ldd	2,x
	subd	2,y
	std	<R0+2
	ldd	,x
	sbcb	1,y
	sbca	,y
	std	<R0
	pulu	pc

opLOG	macro
op\02	pulu	d
	leax	a,s
	leay	b,s
	bra	op\0b
op\0i	leay	,u
	leau	4,u
	bra	op\0a
op\0	pulu	d
	leay	b,s
op\0a	ldx	#R0
op\0b	ldd	2,x
	\0B	3,y
	\0A	2,y
	std	<R0+2
	ldd	,x
	\0B	1,y
	\0A	,y
	std	<R0
	pulu	pc
	endm

	opLOG	AND
	opLOG	OR
	opLOG	EOR

opMUL2	pulu	dp
	leax	a,s
	leay	b,s
	bra	opMUL1+3
opMULi	leay	,u
	leau	4,u
	bra	opMUL0
opMUL	pulu	d
	leay	b,s
opMUL0	ldd	<R0+2
	std	<R1+2
	ldd	<R0
	bne	opMUL1
	ldx	,y
	beq	opMUL16
opMUL1	ldx	#R1
	std	,x
	
	lda	3,y
	mul
	std	<R0
	
	lda	3,y
	ldb	3,x
	mul
	std	<R0+2
	
	ldd	#$0302
	bsr	opMULa
	ldd	#$0203
	bsr	opMULa	

        ldd	#$0202
	bsr	opMULb
	ldd	#$0103
	bsr	opMULb
	
	ldd	#$0300
	bsr	opMULc
	ldd	#$0201
	bsr	opMULc
	ldd	#$0102
	bsr	opMULc
	ldd	#$0003
	bsr	opMULc
	pulu	pc

opMULa	lda	a,x
	ldb	b,y
	mul
	beq	opMULd
	addd	<R0+1
	std	<R0+1
	bcc	opMULd
	inc	<R0
opMULd	rts	   

opMULb	lda	a,x
	ldb	b,y
	mul
	beq	opMULd
	addd	<R0
	std	<R0
	rts	   

opMULc	lda	a,x
	ldb	b,y
        beq	opMULd
	mul
	addb	<R0
	stb	<R0
	rts

opMUL16 lda	3,x
	ldb	3,y
	mul
	std	<R0+2
	lda	2,x
	ldb	2,y
	mul
	std	<R0
	lda	2,x
        beq     opMULe
	ldb	3,y
	mul
	addd	<R0+1
	std	<R0+1
	bcc	opMULe
	inc	<R0
opMULe  lda     3,x
	ldb	2,y
        beq     opMULf
	mul
	addd	<R0+1
	std	<R0+1
	bcc	opMULf
	inc	<R0
opMULf	pulu	pc


opCMP2	pulu	d
	leax	a,s
	leay	b,s
	bra	opCMP1
opCMPi	leay	,u
	leau	4,u
	bra	opCMP0
opCMP	pulu	d
	leay	b,s
opCMP0	ldx	#R0
opCMP1	ldd	,x
	SUBD	,y
	BGT	opPOS
	BLT	opNEG
opCMP4	ldd	2,x
	subd	2,y
	BHI	opPOS
	BLO	opNEG
opCMP3	std	<R0
	std	<R0+2
	pulu	pc
opPOS	ldd	#1
	bra	opCMP3
opNEG	ldd	#-1
	bra	opCMP3
	
opUCMP2 pulu	d
	leax	a,s
	leay	b,s
	bra	opUCMP1
opUCMPi leay	,u
	leau	4,u
	bra	opUCMP0
opUCMP	pulu	d
	leay	b,s
opUCMP0	ldx	#R0
opUCMP1 ldd	,x
	SUBD	,y
	BHI	opPOS
	BLO	opNEG
	BRA	opCMP4	

opJSRi	pulu	d,x
	pshs	d,u
	leau	,x
	pulu	pc
	
opRET	set	*
	puls	d,u
	pulu	pc

ADJi	macro
	fdb	opADJi,\0
	endm
CALLi	macro	     
	fdb	opJSRi,\0
	endm
RET	macro
	fdb	opRET
	endm	    
* operations	    
ADD	macro
	fdb	opADD,\0
	endm
SUB	macro
	fdb	opSUB,\0
	endm
AND	macro
	fdb	opAND,\0
	endm
OR	macro
	fdb	opOR,\0
	endm
XOR	macro
	fdb	opEOR,\0
	endm
CMP	macro
	fdb	opCMP,\0
	endm
UCMP	macro
	fdb	opUCMP,\0
	endm

ADDi	macro
	fdb	opADDi,\0,\1
	endm
SUBi	macro
	fdb	opSUBi,\0,\1
	endm
ANDi	macro
	fdb	opANDi,\0,\1
	endm
ORi	macro
	fdb	opORi,\0,\1
	endm
XORi	macro
	fdb	opEORi,\0,\1
	endm
CMPi	macro
	fdb	opCMPi,\0,\1
	endm
UCMPi	macro
	fdb	opUCMPi,\0,\1
	endm

ADD2	macro
	fdb	opADD2,256*\0+\1
	endm
SUB2	macro
	fdb	opSUB2,256*\0+\1
	endm
AND2	macro
	fdb	opAND2,256*\0+\1
	endm
OR2	macro
	fdb	opOR2,256*\0+\1
	endm
XOR2	macro
	fdb	opEOR2,256*\0+\1
	endm
CMP2	macro
	fdb	opCMP2,256*\0+\1
	endm
UCMP2	macro
	fdb	opUCMP2,256*\0+\1
	endm

* load
LD	macro
	fdb	opLD,\0
	endm
LD1	macro
	fdb	opLD1,\0
	endm
LD1r	macro
	fdb	opLD1r
	endm	    
LDU1	macro
	fdb	opLDU1,\0
	endm
LDU1r	macro
	fdb	opLDU1r
	endm	    
LD2	macro
	fdb	opLD2,\0
	endm
LD2r	macro
	fdb	opLD2r
	endm	    
LDU2	macro
	fdb	opLDU2,\0
	endm
LDU2r	macro
	fdb	opLDU2r
	endm	    
LD4	macro
	fdb	opLD4,\0
	endm
LD4r	macro
	fdb	opLD4r
	endm	    
LDi	macro
	fdb	opLDi,\0,\1
	endm
*  store
ST	macro
	fdb	opST,\0
	endm
ST1	macro
	fdb	opST1,\0
	endm
ST2	macro
	fdb	opST2,\0
	endm
ST4	macro
	fdb	opST4,\0
	endm
* jump
JRA	macro
	fdb	opJRA,\0
	endm
JEQ	macro
	fdb	opJEQ,\0
	endm
JNE	macro
	fdb	opJNE,\0
	endm
JLT	macro
	fdb	opJLT,\0
	endm
JGT	macro
	fdb	opJGT,\0
	endm
JGE	macro
	fdb	opJGE,\0
	endm
JLE	macro
	fdb	opJLE,\0
	endm
* stack	       
PUSH	macro
	fdb	opPUSH,\0
	endm
PUSHi	macro
	fdb	opPUSHi,0,\0
	endm
PUSHr	macro
	fdb	opPUSHr
	endm
* misc
MOV	macro
	fdb	opMOV,\0*256+\1
	endm
VM_OFF	macro
	fdb	*+2
	endm
VM_ON	macro
	jsr	<go_vm
	endm  

(info)