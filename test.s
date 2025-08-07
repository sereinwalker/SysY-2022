	.text
	.attribute	4, 16
	.attribute	5, "rv64i2p0"
	.file	"sysy.module"
	.globl	print_array
	.p2align	2
	.type	print_array,@function
print_array:
	.cfi_startproc
	addi	sp, sp, -32
	.cfi_def_cfa_offset 32
	sd	ra, 24(sp)
	sd	s0, 16(sp)
	sd	s1, 8(sp)
	sd	s2, 0(sp)
	.cfi_offset ra, -8
	.cfi_offset s0, -16
	.cfi_offset s1, -24
	.cfi_offset s2, -32
	mv	s0, a1
	mv	s1, a0
.LBB0_3:
	auipc	a0, %pcrel_hi(.L.str)
	addi	a0, a0, %pcrel_lo(.LBB0_3)
	mv	a1, s1
	call	putf@plt
	li	s2, 0
	sext.w	s1, s1
	bge	s2, s1, .LBB0_2
.LBB0_1:
	slli	a0, s2, 2
	add	a0, s0, a0
	lw	a0, 0(a0)
	call	putint@plt
	li	a0, 32
	call	putch@plt
	addiw	s2, s2, 1
	blt	s2, s1, .LBB0_1
.LBB0_2:
	li	a0, 10
	call	putch@plt
	ld	ra, 24(sp)
	ld	s0, 16(sp)
	ld	s1, 8(sp)
	ld	s2, 0(sp)
	addi	sp, sp, 32
	ret
.Lfunc_end0:
	.size	print_array, .Lfunc_end0-print_array
	.cfi_endproc

	.globl	main
	.p2align	2
	.type	main,@function
main:
	.cfi_startproc
	addi	sp, sp, -80
	.cfi_def_cfa_offset 80
	sd	ra, 72(sp)
	sd	s0, 64(sp)
	sd	s1, 56(sp)
	sd	s2, 48(sp)
	sd	s3, 40(sp)
	sd	s4, 32(sp)
	.cfi_offset ra, -8
	.cfi_offset s0, -16
	.cfi_offset s1, -24
	.cfi_offset s2, -32
	.cfi_offset s3, -40
	.cfi_offset s4, -48
.LBB1_7:
	auipc	a0, %pcrel_hi(.L.str.1)
	addi	a0, a0, %pcrel_lo(.LBB1_7)
	call	putf@plt
.LBB1_8:
	auipc	a0, %pcrel_hi(.L.str.2)
	addi	a0, a0, %pcrel_lo(.LBB1_8)
	li	a1, 10
	li	a2, 20
	lui	a3, 262656
	call	putf@plt
.LBB1_9:
	auipc	a0, %pcrel_hi(.L.str.3)
	addi	a0, a0, %pcrel_lo(.LBB1_9)
	li	a1, 60
	call	putf@plt
.LBB1_10:
	auipc	a0, %pcrel_hi(.L.str.4)
	addi	a0, a0, %pcrel_lo(.LBB1_10)
	call	putf@plt
	li	a0, 5
	call	factorial@plt
	mv	s0, a0
.LBB1_11:
	auipc	a0, %pcrel_hi(.L.str.6)
	addi	a0, a0, %pcrel_lo(.LBB1_11)
	mv	a1, s0
	call	putf@plt
.LBB1_12:
	auipc	a0, %pcrel_hi(.L.str.7)
	addi	a0, a0, %pcrel_lo(.LBB1_12)
	call	putf@plt
.LBB1_13:
	auipc	a0, %pcrel_hi(SIZE)
	addi	a0, a0, %pcrel_lo(.LBB1_13)
	lw	s1, 0(a0)
	li	s2, 0
	addi	s3, sp, 12
	bge	s2, s1, .LBB1_2
.LBB1_1:
	slli	a0, s2, 2
	add	s4, s3, a0
	li	a1, 11
	mv	a0, s2
	call	__muldi3@plt
	addiw	a0, a0, 11
	sw	a0, 0(s4)
	addiw	s2, s2, 1
	blt	s2, s1, .LBB1_1
.LBB1_2:
	addi	a1, sp, 12
	mv	a0, s1
	call	print_array@plt
	li	a0, 10
	li	s2, 10
	call	putch@plt
.LBB1_14:
	auipc	a0, %pcrel_hi(.L.str.8)
	addi	a0, a0, %pcrel_lo(.LBB1_14)
	call	putf@plt
	li	s1, 0
	li	s3, 1
.LBB1_3:
	addiw	s1, s1, 1
	bge	s1, s2, .LBB1_6
	srliw	a0, s1, 31
	add	a0, s1, a0
	andi	a0, a0, -2
	subw	a0, s1, a0
	beq	a0, s3, .LBB1_3
	mv	a0, s1
	call	putint@plt
	li	a0, 32
	call	putch@plt
	j	.LBB1_3
.LBB1_6:
	li	a0, 10
	call	putch@plt
	li	a0, 10
	call	putch@plt
.LBB1_15:
	auipc	a0, %pcrel_hi(PI)
	addi	a0, a0, %pcrel_lo(.LBB1_15)
	lw	s1, 0(a0)
	lui	a1, 262656
	mv	a0, s1
	call	__mulsf3@plt
	mv	a3, a0
.LBB1_16:
	auipc	a0, %pcrel_hi(.L.str.9)
	addi	a0, a0, %pcrel_lo(.LBB1_16)
	lui	a2, 262656
	mv	a1, s1
	call	putf@plt
.LBB1_17:
	auipc	a0, %pcrel_hi(global_var)
	addi	a0, a0, %pcrel_lo(.LBB1_17)
	lw	a1, 0(a0)
	addw	a1, a1, s0
	sw	a1, 0(a0)
.LBB1_18:
	auipc	a0, %pcrel_hi(.L.str.10)
	addi	a0, a0, %pcrel_lo(.LBB1_18)
	call	putf@plt
.LBB1_19:
	auipc	a0, %pcrel_hi(.L.str.11)
	addi	a0, a0, %pcrel_lo(.LBB1_19)
	call	putf@plt
	li	a0, 0
	ld	ra, 72(sp)
	ld	s0, 64(sp)
	ld	s1, 56(sp)
	ld	s2, 48(sp)
	ld	s3, 40(sp)
	ld	s4, 32(sp)
	addi	sp, sp, 80
	ret
.Lfunc_end1:
	.size	main, .Lfunc_end1-main
	.cfi_endproc

	.globl	factorial
	.p2align	2
	.type	factorial,@function
factorial:
	.cfi_startproc
	addi	sp, sp, -16
	.cfi_def_cfa_offset 16
	sd	ra, 8(sp)
	sd	s0, 0(sp)
	.cfi_offset ra, -8
	.cfi_offset s0, -16
	mv	s0, a0
	sext.w	a1, a0
	li	a0, 1
	bge	a0, a1, .LBB2_2
	addiw	a0, s0, -1
	call	factorial@plt
	mv	a1, s0
	call	__muldi3@plt
.LBB2_2:
	ld	ra, 8(sp)
	ld	s0, 0(sp)
	addi	sp, sp, 16
	ret
.Lfunc_end2:
	.size	factorial, .Lfunc_end2-factorial
	.cfi_endproc

	.type	SIZE,@object
	.comm	SIZE,4,4
	.type	PI,@object
	.comm	PI,4,4
	.type	global_var,@object
	.comm	global_var,4,4
	.type	.L.str,@object
	.section	.rodata.str1.1,"aMS",@progbits,1
.L.str:
	.asciz	"Printing array of size %d:\n"
	.size	.L.str, 28

	.type	.L.str.1,@object
.L.str.1:
	.asciz	"--- SysY Compiler Test Program ---\n\n"
	.size	.L.str.1, 37

	.type	.L.str.2,@object
.L.str.2:
	.asciz	"1. Basic values: a=%d, b=%d, c=%f\n"
	.size	.L.str.2, 35

	.type	.L.str.3,@object
.L.str.3:
	.asciz	"2. Expression (a+b)*2 = %d (expected 60)\n"
	.size	.L.str.3, 42

	.type	.L.str.4,@object
.L.str.4:
	.asciz	"   Condition (result > 50 && result == 60) is true.\n"
	.size	.L.str.4, 53

	.type	.L.str.5,@object
.L.str.5:
	.asciz	"   Condition is false. (Error)\n"
	.size	.L.str.5, 32

	.type	.L.str.6,@object
.L.str.6:
	.asciz	"3. Recursive factorial(5) = %d (expected 120)\n\n"
	.size	.L.str.6, 48

	.type	.L.str.7,@object
.L.str.7:
	.asciz	"4. Array test:\n"
	.size	.L.str.7, 16

	.type	.L.str.8,@object
.L.str.8:
	.asciz	"5. Loop with break/continue (printing even nums from 2 to 8):\n"
	.size	.L.str.8, 63

	.type	.L.str.9,@object
.L.str.9:
	.asciz	"6. Float test: %f * %f = %f (expected 7.85)\n"
	.size	.L.str.9, 45

	.type	.L.str.10,@object
.L.str.10:
	.asciz	"7. Global variable updated: %d (expected 220)\n"
	.size	.L.str.10, 47

	.type	.L.str.11,@object
.L.str.11:
	.asciz	"\n--- Test Program Finished ---\n"
	.size	.L.str.11, 32

	.section	".note.GNU-stack","",@progbits
