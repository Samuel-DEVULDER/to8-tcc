* to8-vm.asm kind of crt0.o
*
* Lightweigh. No banking system here.

	org	$9000

	setdp	R0/256
	
SKIP1	macro
	fcb	$81	; CMPA #
	endm
	
SKIP2X	macro
	fcb	$8e	; LDX #
	endm
	
init	jmp	crt0

R0	FDB	0,0
R1	FDB	0,0	; 

CLK	FDB	0,0	; clock register (1/10 sec)

go_vm	puls	u
	pulu	pc

* Unsigned division of R0 by integer pointed by y. 
* Remainder in R1, /Quotient in {R0, carry}
opUDIVy clra	       ; no ldd here in dp
	clrb	       ; 1 byte maters
	std	<R1
	std	<R1+2
	ldx	#32
opUDIVl rol	<R0+3
	rol	<R0+2
	rol	<R0+1
	rol	<R0
	ldd	<R1+2	
	rolb
	rola
	std	<R1+2
	rol	<R1+1
	rol	<R1
	subd	2,y
	std	<opUDIV0+1
	ldd	<R1
	sbcb	1,y
	sbca	,y
	bcs	opUDIV1
	std	<R1
opUDIV0 ldd	#0
	std	<R1+2
opUDIV1 leax	-1,x
	bne	opUDIVl
	rts

* negate R0 or int at "x"
opNEG	ldx	#R0
opNEGx	com	,x
	com	1,x
	com	2,x
	neg	3,x
	bcc	opNEGx0
	inc	2,x
	bcc	opNEGx0
	inc	1,x
	bcc	opNEGx0
	inc	,x
opNEGx0 rts

* bool
opSNE	ldd	<R0+2
	bne	opTRUE
	ldd	<R0
	beq	opLDB
opTRUE	LDB	#1
	bra	opLDB
	
opSEQ	ldd	<R0+2
	bne	opFALS
	ldd	<R0
	beq	opTRUE
opFALS	ClRB
	bra	opLDB
	
opSGE	lda	<R0
	bpl	opTRUE
	bra	opFALS

opSLT	lda	<R0
	bmi	opTRUE
	bra	opFALS
	  
opSGT	ldd	<R0
	bmi	opFALS
	bpl	opTRUE
	ldd	<R0+2
	bne	opTRUE
	bra	opFALS+1
	
opSLE	ldd	<R0
	bmi	opTRUE
	bpl	opFALS
	ldd	<R0+2
	beq	opTRUE
	bra	opFALS

* load
opLDi	pulu	d,x,y
	std	<R0
	stx	<R0+2
	jmp	,y	 

opLEA	pulu	d,y
	leax	b,s
	clrb		;assuming  a=0
	bra	opSTR0

opLD	pulu	d,y
	leax	b,s	   
	ldd	,x
	ldx	2,x
opSTR0	std	<R0
	stx	<R0+2
	jmp	,y

opMOV	ldd	,u
	leax	a,s
	leay	b,s
	ldd	,y
	std	,x
	ldd	2,y
	std	2,x
	pulu	d,pc
	
opLD1r	ldx	#R0
	bra	opLD1a
opLD1	pulu	d
	leax	b,s
opLD1a	ldb	[2,x]	; TODO banking
	SKIP2X
opEXT1	ldb	<R0+3
opLDB	sex
	std	<R0+2
	sta	<R0+1
	sta	<R0
	pulu	pc

opLD1Ur ldx	#R0
	bra	opLD1Ua
opLD1U	pulu	d
	leax	b,s
opLD1Ua ldb	[2,x]	; TODO banking
	SKIP2X
opEXT1U ldb	<R0
	clra
	std	<R0+2
	clrb
	std	<R0
	pulu	pc

opLD2r	ldx	#R0
	bra	opLD2a
opLD2	pulu	d
	leax	b,s
opLD2a	ldd	[2,x]	; TODO banking
	std	<R0+2
	SKIP2X
opEXT2	ldb	<R0+2  
opLD2b	bge	opLD2c
	ldb	#-1
	SKIP1
opLD2c	clrb
	sex	   
	std	<R0
	pulu	pc

opLD2Ur ldx	#R0
	bra	opLD2Ua
opLD2U	pulu	d
	leax	b,s
opLD2Ua ldd	[2,x]	; TODO banking
	std	<R0+2
opEXT2U ldd	#0
	std	<R0
	pulu	pc

opLDUr	pulu	y
	ldx	#R0
	bra	opLD4a
opLD4	pulu	d,y
	leax	b,s
opLD4a	ldx	2,x    ; TODO banking
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

* stack
opADJ	pulu	d,y
	leas	b,s
	jmp	,y

opPUSH	pulu	d,y
	leax	b,s
	ldd	,x
	ldx	2,x
	pshs	d,x
	jmp	,y

opPUSHr ldd	<R0
	ldx	<R0+2
	pshs	d,x
	pulu	pc
	
opPUSHi pulu	d,x,y
	pshs	d,x
	jmp	,y

* call
opRET	puls	d,u	; TODO banking
	pulu	pc

opSBRr	ldx	#R0
	bra	opSBRa
opSBR	pulu	d
	leax	b,s
opSBRa	clrb		; TODO banking
	pshs	d,u
	ldd	,x
	ldx	2,x	; TODO banking
	bra	opSBRb
opSBRi	pulu	x	; TODO load B with curr BANK
	pshs	d,u
opSBRb	leau	,x	; TODO banking
	pulu	pc
	
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
	
opJLE	ldd	<R0
	bmi	opJRA
	bpl	opJRN
	ldd	<R0+2
	beq	opJRA
	pulu	y,pc
	
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

opCMP2	pulu	d
	leax	a,s
	leay	b,s
	bra	opCMPb
opCMPi	leay	,u
	leau	4,u
	bra	opCMPa
opCMP	pulu	d
	leay	b,s
opCMPa	ldx	#R0
opCMPb	ldd	,x
	SUBD	,y
	BGT	opSET1
	BLT	opSET_1
opCMPc	ldd	2,x
	subd	2,y
	BHI	opSET1
	BLO	opSET_1
opCMPd	std	<R0
	std	<R0+2
	pulu	pc
opSET1	ldd	#1
	bra	opCMPd	;writes $00010001 which makes B<CC> faster
opSET_1 ldd	#-1
	bra	opCMPd
	
opUCMP2 pulu	d
	leax	a,s
	leay	b,s
	bra	opUCMPb
opUCMPi leay	,u
	leau	4,u
	bra	opUCMPa
opUCMP	pulu	d
	leay	b,s
opUCMPa ldx	#R0
opUCMPb ldd	,x
	SUBD	,y
	BHI	opSET1
	BLO	opSET_1
	BRA	opCMPc	

opMUL2	pulu	d
	leax	a,s
	leay	b,s
	bra	opMUL1+3
opMULi	leay	,u
	leau	4,u
	bra	opMUL0
opMULT	pulu	d
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
	beq	opMULe
	ldb	3,y
	mul
	addd	<R0+1
	std	<R0+1
	bcc	opMULe
	inc	<R0
opMULe	lda	3,x
	ldb	2,y
	beq	opMULf
	mul
	addd	<R0+1
	std	<R0+1
	bcc	opMULf
	inc	<R0
opMULf	pulu	pc

opDIV2	pulu	d
	leax	a,s
	leay	b,s
	ldd	,x
	std	<R0
	ldd	2,x
	std	<R0+2
	bra	opDIVa
opDIVi	leay	,u
	leau	4,u
	bra	opDIVa
opDIV	pulu	d
	leay	b,s
opDIVa	ldb	,y
	stb	,-s
	bpl	opDIVb
	jsr	<opNEG
	leax	,y
	jsr	<opNEGx
opDIVb	ldb	<R0
	stb	,-s
	bpl	opDIVc
	jsr	<opNEG
opDIVc	jsr	<opUDIVy
	rol	<R0+3
	rol	<R0+2
	ldd	<R0
	rolb
	rola
	comb
	coma
	std	<R0
	com	<R0+2
	com	<R0+3
	ldb	,s+
	bpl	opDIVd
	jsr	<opNEG
opDIVd	ldb	,s+
	bpl	opDIVe
	leax	,y
	jsr	<opNEGx
opDIVe	pulu	pc	  

opMOD2	pulu	d
	leay	b,s
	leax	a,s
	ldd	,x
	std	<R0
	ldd	2,x
	std	<R0+2
	bra	opMODa
opMODi	leay	,u
	leau	4,u
	bra	opMODa
opMOD	pulu	d
	leay	b,s
opMODa	ldb	<R0	; real	work starts here
	stb	,-s	; save	R0 sign
	bpl	opMODb
	jsr	<opNEG	; make it positive 
opMODb	ldb	,y	
	stb	,-s	; save "y" sign
	bpl	opMODc
	leax	,y
	jsr	<opNEGx ; make "y" positive
opMODc	jsr	<opUDIVy
	ldd	<R1	; move remainder to R0
	std	<R0
	ldd	<R1+2
	std	<R0+2
	ldb	,s+	; restore "y"  sign
	bpl	opMODd
	leax	,y
	jsr	<opNEGx
opMODd	ldb	,s+	; get initial sign of R0
	bpl	opMODe
	jsr	<opNEG	; make (R0%y)of the samesign as R0
opMODe	pulu	pc	  

opUDIV2 pulu	d
	leax	a,s
	leay	b,s
	ldd	,x
	std	<R0
	ldd	2,x
	std	<R0+2
	bra	opUDIVa
opUDIVi leay	,u
	leau	4,u
	bra	opUDIVa
opUDIV	pulu	d
	leay	b,s
opUDIVa jsr	<opUDIVy
	rol	<R0+3
	rol	<R0+2
	ldd	<R0
	rolb
	rola
	comb
	coma
	std	<R0
	com	<R0+2
	com	<R0+3
	pulu	pc	  

opUMOD2 pulu	d
	leay	b,s
	leax	a,s
	ldd	,x
	std	<R0
	ldd	2,x
	std	<R0+2
	bra	opUMODa
opUMODi leay	,u
	leau	4,u
	bra	opMODa
opUMOD	pulu	d
	leay	b,s
opUMODa jsr	<opUDIVy
	ldd	<R1	; move remainder to R0
	std	<R0
	ldd	<R1+2
	std	<R0+2
	pulu	pc	  

(info)
       
crt0	pshs	d,x,y,u,dp,cc
	ldd	#R0&$FF00
	tfr	a,dp
	sts	__exit+2
	clra
	std	<CLK	; clear clock
	std	<CLK+1
	tfr	d,x	; clear ac,av
	pshs	d,x
	pshs	d,x
	ldx	#__exit-2
	pshs	d,x	; return to __exit
	
	bsr	startCLK
	
	ldu	#_main	; jmp to main
	pulu	pc
	fdb	__exit
__exit	lds	#0
	bsr	stopCLK
	puls	d,x,y,u,dp,cc,pc

***************************************
* Timer
***************************************
TIMEPT	 EQU   $6027
STATUS	 EQU   $6019
IRQPT	 EQU   $6021
KBIN	 EQU   $E830

stopCLK orcc	#$50
	ldx	#STATUS
	ldb	,x
	andb	#%11011011
stopCL1 orb	#0	  
	stb	,x
stopCL2 ldd	#0
	std	TIMEPT-STATUS,x
	tfr	cc,b
	andb	#$AF
stopCL3 orb	#0
	tfr	b,cc
	rts

startCLK
	ldx	#STATUS
	
	ldd	TIMEPT-STATUS,x
	std	stopCL2+1
	
	tfr	cc,b
	andb	#$50
	stb	stopCL3+1

	orcc	#$50
	ldb	,x
	andb	#%00100100
	stb	stopCL1+1
	ldb	,x 
	orb	#%00100100
	stb	,x	
	
	ldd	#interCLK
	std	TIMEPT-STATUS,x
	andcc	#$AF
	rts

interCLK
	inc	<CLK+3
	bne	interCLK0
	inc	<CLK+2
	bne	interCLK0
	inc	<CLK+1
	bne	interCLK0
	inc	<CLK
interCLK0
	jmp	KBIN

opLDCLK pshs	cc
	orcc	#$50
	ldd	<CLK
	ldx	<CLK+2
	puls	cc
	std	<R0
	stx	<R0+2
	pulu	pc

(info)
	
*************************************************************************
* macros
*************************************************************************

* load
LDi	macro
	fdb	opLDi,\0,\1
	endm
LEA	macro
	fdb	opLEA,\0
	endm
MOV	macro
	fdb	opMOV,\0*256+\1
	endm
LD	macro
	fdb	opLD,\0
	endm
LD1	macro
	fdb	opLD1,\0
	endm
LD1r	macro
	fdb	opLD1r
	endm	    
LD1U	macro
	fdb	opLD1U,\0
	endm
LD1Ur	macro
	fdb	opLD1Ur
	endm	    
LD2	macro
	fdb	opLD2,\0
	endm
LD2r	macro
	fdb	opLD2r
	endm	    
LD2U	macro
	fdb	opLD2U,\0
	endm
LD2Ur	macro
	fdb	opLD2Ur
	endm	    
LD4	macro
	fdb	opLD4,\0
	endm
LD4r	macro
	fdb	opLD4r
	endm	    

* store
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

* stack
ADJ	macro
	fdb	opADJ,\0
	endm	    
PUSH	macro
	fdb	opPUSH,\0
	endm
PUSHi	macro
	fdb	opPUSHi,\0,\1
	endm
PUSHr	macro
	fdb	opPUSHr
	endm

* subroutine
RET	macro
	fdb	opRET
	endm	    
SBR	macro	     
	fdb	opSBR,\0
	endm
SBRi	macro	     
	fdb	opSBRi,\0
	endm
SBRr	macro	     
	fdb	opSBRr
	endm

* bool
SEQ	macro
	fdb	opSEQ
	endm
SNE	macro
	fdb	opSNE
	endm
SLT	macro
	fdb	opSLT
	endm
SGT	macro
	fdb	opSGT
	endm
SGE	macro
	fdb	opSGE
	endm
SLE	macro
	fdb	opSLE
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
MULT	macro
	fdb	opMULT,\0
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
MULi	macro
	fdb	opMULi,\0,\1
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
MUL2	macro
	fdb	opMUL2,256*\0+\1
	endm
MOD2	macro
	fdb	opMOD2,256*\0+\1
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
* cast
EXT1	macro
	fdb	opEXT1
	endm
EXT2	macro
	fdb	opEXT2
	endm	    
EXT1U	macro
	fdb	opEXT1U
	endm
EXT2U	macro
	fdb	opEXT2U
	endm	       
	
* misc
VM_OFF	macro
	fdb	*+2
	endm
VM_ON	macro
	jsr	<go_vm
	endm  
LDCLK	macro
	fdb	opLDCLK
	endm

(info)