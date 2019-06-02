/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Copyright (C) 2015-2019 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 * Copyright (C) 2017 Samuel Neves <sneves@dei.uc.pt>. All Rights Reserved.
 */

#include <linux/linkage.h>

.section .rodata.cst32.BLAKE2S_IV, "aM", @progbits, 32
.align 32
IV:	.octa 0xA54FF53A3C6EF372BB67AE856A09E667
	.octa 0x5BE0CD191F83D9AB9B05688C510E527F
.section .rodata.cst16.ROT16, "aM", @progbits, 16
.align 16
ROT16:	.octa 0x0D0C0F0E09080B0A0504070601000302
.section .rodata.cst16.ROR328, "aM", @progbits, 16
.align 16
ROR328:	.octa 0x0C0F0E0D080B0A090407060500030201
#ifdef CONFIG_AS_AVX512
.section .rodata.cst64.BLAKE2S_SIGMA, "aM", @progbits, 640
.align 64
SIGMA:
.long 0, 2, 4, 6, 1, 3, 5, 7, 8, 10, 12, 14, 9, 11, 13, 15
.long 11, 2, 12, 14, 9, 8, 15, 3, 4, 0, 13, 6, 10, 1, 7, 5
.long 10, 12, 11, 6, 5, 9, 13, 3, 4, 15, 14, 2, 0, 7, 8, 1
.long 10, 9, 7, 0, 11, 14, 1, 12, 6, 2, 15, 3, 13, 8, 5, 4
.long 4, 9, 8, 13, 14, 0, 10, 11, 7, 3, 12, 1, 5, 6, 15, 2
.long 2, 10, 4, 14, 13, 3, 9, 11, 6, 5, 7, 12, 15, 1, 8, 0
.long 4, 11, 14, 8, 13, 10, 12, 5, 2, 1, 15, 3, 9, 7, 0, 6
.long 6, 12, 0, 13, 15, 2, 1, 10, 4, 5, 11, 14, 8, 3, 9, 7
.long 14, 5, 4, 12, 9, 7, 3, 10, 2, 0, 6, 15, 11, 1, 13, 8
.long 11, 7, 13, 10, 12, 14, 0, 15, 4, 5, 6, 9, 2, 1, 8, 3
#endif /* CONFIG_AS_AVX512 */

.text
#ifdef CONFIG_AS_AVX
ENTRY(blake2s_compress_avx)
	movl		%ecx, %ecx
	testq		%rdx, %rdx
	je		.Lendofloop
	.align 32
.Lbeginofloop:
	addq		%rcx, 32(%rdi)
	vmovdqu		IV+16(%rip), %xmm1
	vmovdqu		(%rsi), %xmm4
	vpxor		32(%rdi), %xmm1, %xmm1
	vmovdqu		16(%rsi), %xmm3
	vshufps		$136, %xmm3, %xmm4, %xmm6
	vmovdqa		ROT16(%rip), %xmm7
	vpaddd		(%rdi), %xmm6, %xmm6
	vpaddd		16(%rdi), %xmm6, %xmm6
	vpxor		%xmm6, %xmm1, %xmm1
	vmovdqu		IV(%rip), %xmm8
	vpshufb		%xmm7, %xmm1, %xmm1
	vmovdqu		48(%rsi), %xmm5
	vpaddd		%xmm1, %xmm8, %xmm8
	vpxor		16(%rdi), %xmm8, %xmm9
	vmovdqu		32(%rsi), %xmm2
	vpblendw	$12, %xmm3, %xmm5, %xmm13
	vshufps		$221, %xmm5, %xmm2, %xmm12
	vpunpckhqdq	%xmm2, %xmm4, %xmm14
	vpslld		$20, %xmm9, %xmm0
	vpsrld		$12, %xmm9, %xmm9
	vpxor		%xmm0, %xmm9, %xmm0
	vshufps		$221, %xmm3, %xmm4, %xmm9
	vpaddd		%xmm9, %xmm6, %xmm9
	vpaddd		%xmm0, %xmm9, %xmm9
	vpxor		%xmm9, %xmm1, %xmm1
	vmovdqa		ROR328(%rip), %xmm6
	vpshufb		%xmm6, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm8, %xmm8
	vpxor		%xmm8, %xmm0, %xmm0
	vpshufd		$147, %xmm1, %xmm1
	vpshufd		$78, %xmm8, %xmm8
	vpslld		$25, %xmm0, %xmm10
	vpsrld		$7, %xmm0, %xmm0
	vpxor		%xmm10, %xmm0, %xmm0
	vshufps		$136, %xmm5, %xmm2, %xmm10
	vpshufd		$57, %xmm0, %xmm0
	vpaddd		%xmm10, %xmm9, %xmm9
	vpaddd		%xmm0, %xmm9, %xmm9
	vpxor		%xmm9, %xmm1, %xmm1
	vpaddd		%xmm12, %xmm9, %xmm9
	vpblendw	$12, %xmm2, %xmm3, %xmm12
	vpshufb		%xmm7, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm8, %xmm8
	vpxor		%xmm8, %xmm0, %xmm10
	vpslld		$20, %xmm10, %xmm0
	vpsrld		$12, %xmm10, %xmm10
	vpxor		%xmm0, %xmm10, %xmm0
	vpaddd		%xmm0, %xmm9, %xmm9
	vpxor		%xmm9, %xmm1, %xmm1
	vpshufb		%xmm6, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm8, %xmm8
	vpxor		%xmm8, %xmm0, %xmm0
	vpshufd		$57, %xmm1, %xmm1
	vpshufd		$78, %xmm8, %xmm8
	vpslld		$25, %xmm0, %xmm10
	vpsrld		$7, %xmm0, %xmm0
	vpxor		%xmm10, %xmm0, %xmm0
	vpslldq		$4, %xmm5, %xmm10
	vpblendw	$240, %xmm10, %xmm12, %xmm12
	vpshufd		$147, %xmm0, %xmm0
	vpshufd		$147, %xmm12, %xmm12
	vpaddd		%xmm9, %xmm12, %xmm12
	vpaddd		%xmm0, %xmm12, %xmm12
	vpxor		%xmm12, %xmm1, %xmm1
	vpshufb		%xmm7, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm8, %xmm8
	vpxor		%xmm8, %xmm0, %xmm11
	vpslld		$20, %xmm11, %xmm9
	vpsrld		$12, %xmm11, %xmm11
	vpxor		%xmm9, %xmm11, %xmm0
	vpshufd		$8, %xmm2, %xmm9
	vpblendw	$192, %xmm5, %xmm3, %xmm11
	vpblendw	$240, %xmm11, %xmm9, %xmm9
	vpshufd		$177, %xmm9, %xmm9
	vpaddd		%xmm12, %xmm9, %xmm9
	vpaddd		%xmm0, %xmm9, %xmm11
	vpxor		%xmm11, %xmm1, %xmm1
	vpshufb		%xmm6, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm8, %xmm8
	vpxor		%xmm8, %xmm0, %xmm9
	vpshufd		$147, %xmm1, %xmm1
	vpshufd		$78, %xmm8, %xmm8
	vpslld		$25, %xmm9, %xmm0
	vpsrld		$7, %xmm9, %xmm9
	vpxor		%xmm0, %xmm9, %xmm0
	vpslldq		$4, %xmm3, %xmm9
	vpblendw	$48, %xmm9, %xmm2, %xmm9
	vpblendw	$240, %xmm9, %xmm4, %xmm9
	vpshufd		$57, %xmm0, %xmm0
	vpshufd		$177, %xmm9, %xmm9
	vpaddd		%xmm11, %xmm9, %xmm9
	vpaddd		%xmm0, %xmm9, %xmm9
	vpxor		%xmm9, %xmm1, %xmm1
	vpshufb		%xmm7, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm8, %xmm11
	vpxor		%xmm11, %xmm0, %xmm0
	vpslld		$20, %xmm0, %xmm8
	vpsrld		$12, %xmm0, %xmm0
	vpxor		%xmm8, %xmm0, %xmm0
	vpunpckhdq	%xmm3, %xmm4, %xmm8
	vpblendw	$12, %xmm10, %xmm8, %xmm12
	vpshufd		$177, %xmm12, %xmm12
	vpaddd		%xmm9, %xmm12, %xmm9
	vpaddd		%xmm0, %xmm9, %xmm9
	vpxor		%xmm9, %xmm1, %xmm1
	vpshufb		%xmm6, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm11, %xmm11
	vpxor		%xmm11, %xmm0, %xmm0
	vpshufd		$57, %xmm1, %xmm1
	vpshufd		$78, %xmm11, %xmm11
	vpslld		$25, %xmm0, %xmm12
	vpsrld		$7, %xmm0, %xmm0
	vpxor		%xmm12, %xmm0, %xmm0
	vpunpckhdq	%xmm5, %xmm2, %xmm12
	vpshufd		$147, %xmm0, %xmm0
	vpblendw	$15, %xmm13, %xmm12, %xmm12
	vpslldq		$8, %xmm5, %xmm13
	vpshufd		$210, %xmm12, %xmm12
	vpaddd		%xmm9, %xmm12, %xmm9
	vpaddd		%xmm0, %xmm9, %xmm9
	vpxor		%xmm9, %xmm1, %xmm1
	vpshufb		%xmm7, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm11, %xmm11
	vpxor		%xmm11, %xmm0, %xmm0
	vpslld		$20, %xmm0, %xmm12
	vpsrld		$12, %xmm0, %xmm0
	vpxor		%xmm12, %xmm0, %xmm0
	vpunpckldq	%xmm4, %xmm2, %xmm12
	vpblendw	$240, %xmm4, %xmm12, %xmm12
	vpblendw	$192, %xmm13, %xmm12, %xmm12
	vpsrldq		$12, %xmm3, %xmm13
	vpaddd		%xmm12, %xmm9, %xmm9
	vpaddd		%xmm0, %xmm9, %xmm9
	vpxor		%xmm9, %xmm1, %xmm1
	vpshufb		%xmm6, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm11, %xmm11
	vpxor		%xmm11, %xmm0, %xmm0
	vpshufd		$147, %xmm1, %xmm1
	vpshufd		$78, %xmm11, %xmm11
	vpslld		$25, %xmm0, %xmm12
	vpsrld		$7, %xmm0, %xmm0
	vpxor		%xmm12, %xmm0, %xmm0
	vpblendw	$60, %xmm2, %xmm4, %xmm12
	vpblendw	$3, %xmm13, %xmm12, %xmm12
	vpshufd		$57, %xmm0, %xmm0
	vpshufd		$78, %xmm12, %xmm12
	vpaddd		%xmm9, %xmm12, %xmm9
	vpaddd		%xmm0, %xmm9, %xmm9
	vpxor		%xmm9, %xmm1, %xmm1
	vpshufb		%xmm7, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm11, %xmm11
	vpxor		%xmm11, %xmm0, %xmm12
	vpslld		$20, %xmm12, %xmm13
	vpsrld		$12, %xmm12, %xmm0
	vpblendw	$51, %xmm3, %xmm4, %xmm12
	vpxor		%xmm13, %xmm0, %xmm0
	vpblendw	$192, %xmm10, %xmm12, %xmm10
	vpslldq		$8, %xmm2, %xmm12
	vpshufd		$27, %xmm10, %xmm10
	vpaddd		%xmm9, %xmm10, %xmm9
	vpaddd		%xmm0, %xmm9, %xmm9
	vpxor		%xmm9, %xmm1, %xmm1
	vpshufb		%xmm6, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm11, %xmm11
	vpxor		%xmm11, %xmm0, %xmm0
	vpshufd		$57, %xmm1, %xmm1
	vpshufd		$78, %xmm11, %xmm11
	vpslld		$25, %xmm0, %xmm10
	vpsrld		$7, %xmm0, %xmm0
	vpxor		%xmm10, %xmm0, %xmm0
	vpunpckhdq	%xmm2, %xmm8, %xmm10
	vpshufd		$147, %xmm0, %xmm0
	vpblendw	$12, %xmm5, %xmm10, %xmm10
	vpshufd		$210, %xmm10, %xmm10
	vpaddd		%xmm9, %xmm10, %xmm9
	vpaddd		%xmm0, %xmm9, %xmm9
	vpxor		%xmm9, %xmm1, %xmm1
	vpshufb		%xmm7, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm11, %xmm11
	vpxor		%xmm11, %xmm0, %xmm10
	vpslld		$20, %xmm10, %xmm0
	vpsrld		$12, %xmm10, %xmm10
	vpxor		%xmm0, %xmm10, %xmm0
	vpblendw	$12, %xmm4, %xmm5, %xmm10
	vpblendw	$192, %xmm12, %xmm10, %xmm10
	vpunpckldq	%xmm2, %xmm4, %xmm12
	vpshufd		$135, %xmm10, %xmm10
	vpaddd		%xmm9, %xmm10, %xmm9
	vpaddd		%xmm0, %xmm9, %xmm9
	vpxor		%xmm9, %xmm1, %xmm1
	vpshufb		%xmm6, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm11, %xmm13
	vpxor		%xmm13, %xmm0, %xmm0
	vpshufd		$147, %xmm1, %xmm1
	vpshufd		$78, %xmm13, %xmm13
	vpslld		$25, %xmm0, %xmm10
	vpsrld		$7, %xmm0, %xmm0
	vpxor		%xmm10, %xmm0, %xmm0
	vpblendw	$15, %xmm3, %xmm4, %xmm10
	vpblendw	$192, %xmm5, %xmm10, %xmm10
	vpshufd		$57, %xmm0, %xmm0
	vpshufd		$198, %xmm10, %xmm10
	vpaddd		%xmm9, %xmm10, %xmm10
	vpaddd		%xmm0, %xmm10, %xmm10
	vpxor		%xmm10, %xmm1, %xmm1
	vpshufb		%xmm7, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm13, %xmm13
	vpxor		%xmm13, %xmm0, %xmm9
	vpslld		$20, %xmm9, %xmm0
	vpsrld		$12, %xmm9, %xmm9
	vpxor		%xmm0, %xmm9, %xmm0
	vpunpckhdq	%xmm2, %xmm3, %xmm9
	vpunpcklqdq	%xmm12, %xmm9, %xmm15
	vpunpcklqdq	%xmm12, %xmm8, %xmm12
	vpblendw	$15, %xmm5, %xmm8, %xmm8
	vpaddd		%xmm15, %xmm10, %xmm15
	vpaddd		%xmm0, %xmm15, %xmm15
	vpxor		%xmm15, %xmm1, %xmm1
	vpshufd		$141, %xmm8, %xmm8
	vpshufb		%xmm6, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm13, %xmm13
	vpxor		%xmm13, %xmm0, %xmm0
	vpshufd		$57, %xmm1, %xmm1
	vpshufd		$78, %xmm13, %xmm13
	vpslld		$25, %xmm0, %xmm10
	vpsrld		$7, %xmm0, %xmm0
	vpxor		%xmm10, %xmm0, %xmm0
	vpunpcklqdq	%xmm2, %xmm3, %xmm10
	vpshufd		$147, %xmm0, %xmm0
	vpblendw	$51, %xmm14, %xmm10, %xmm14
	vpshufd		$135, %xmm14, %xmm14
	vpaddd		%xmm15, %xmm14, %xmm14
	vpaddd		%xmm0, %xmm14, %xmm14
	vpxor		%xmm14, %xmm1, %xmm1
	vpunpcklqdq	%xmm3, %xmm4, %xmm15
	vpshufb		%xmm7, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm13, %xmm13
	vpxor		%xmm13, %xmm0, %xmm0
	vpslld		$20, %xmm0, %xmm11
	vpsrld		$12, %xmm0, %xmm0
	vpxor		%xmm11, %xmm0, %xmm0
	vpunpckhqdq	%xmm5, %xmm3, %xmm11
	vpblendw	$51, %xmm15, %xmm11, %xmm11
	vpunpckhqdq	%xmm3, %xmm5, %xmm15
	vpaddd		%xmm11, %xmm14, %xmm11
	vpaddd		%xmm0, %xmm11, %xmm11
	vpxor		%xmm11, %xmm1, %xmm1
	vpshufb		%xmm6, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm13, %xmm13
	vpxor		%xmm13, %xmm0, %xmm0
	vpshufd		$147, %xmm1, %xmm1
	vpshufd		$78, %xmm13, %xmm13
	vpslld		$25, %xmm0, %xmm14
	vpsrld		$7, %xmm0, %xmm0
	vpxor		%xmm14, %xmm0, %xmm14
	vpunpckhqdq	%xmm4, %xmm2, %xmm0
	vpshufd		$57, %xmm14, %xmm14
	vpblendw	$51, %xmm15, %xmm0, %xmm15
	vpaddd		%xmm15, %xmm11, %xmm15
	vpaddd		%xmm14, %xmm15, %xmm15
	vpxor		%xmm15, %xmm1, %xmm1
	vpshufb		%xmm7, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm13, %xmm13
	vpxor		%xmm13, %xmm14, %xmm14
	vpslld		$20, %xmm14, %xmm11
	vpsrld		$12, %xmm14, %xmm14
	vpxor		%xmm11, %xmm14, %xmm14
	vpblendw	$3, %xmm2, %xmm4, %xmm11
	vpslldq		$8, %xmm11, %xmm0
	vpblendw	$15, %xmm5, %xmm0, %xmm0
	vpshufd		$99, %xmm0, %xmm0
	vpaddd		%xmm15, %xmm0, %xmm15
	vpaddd		%xmm14, %xmm15, %xmm15
	vpxor		%xmm15, %xmm1, %xmm0
	vpaddd		%xmm12, %xmm15, %xmm15
	vpshufb		%xmm6, %xmm0, %xmm0
	vpaddd		%xmm0, %xmm13, %xmm13
	vpxor		%xmm13, %xmm14, %xmm14
	vpshufd		$57, %xmm0, %xmm0
	vpshufd		$78, %xmm13, %xmm13
	vpslld		$25, %xmm14, %xmm1
	vpsrld		$7, %xmm14, %xmm14
	vpxor		%xmm1, %xmm14, %xmm14
	vpblendw	$3, %xmm5, %xmm4, %xmm1
	vpshufd		$147, %xmm14, %xmm14
	vpaddd		%xmm14, %xmm15, %xmm15
	vpxor		%xmm15, %xmm0, %xmm0
	vpshufb		%xmm7, %xmm0, %xmm0
	vpaddd		%xmm0, %xmm13, %xmm13
	vpxor		%xmm13, %xmm14, %xmm14
	vpslld		$20, %xmm14, %xmm12
	vpsrld		$12, %xmm14, %xmm14
	vpxor		%xmm12, %xmm14, %xmm14
	vpsrldq		$4, %xmm2, %xmm12
	vpblendw	$60, %xmm12, %xmm1, %xmm1
	vpaddd		%xmm1, %xmm15, %xmm15
	vpaddd		%xmm14, %xmm15, %xmm15
	vpxor		%xmm15, %xmm0, %xmm0
	vpblendw	$12, %xmm4, %xmm3, %xmm1
	vpshufb		%xmm6, %xmm0, %xmm0
	vpaddd		%xmm0, %xmm13, %xmm13
	vpxor		%xmm13, %xmm14, %xmm14
	vpshufd		$147, %xmm0, %xmm0
	vpshufd		$78, %xmm13, %xmm13
	vpslld		$25, %xmm14, %xmm12
	vpsrld		$7, %xmm14, %xmm14
	vpxor		%xmm12, %xmm14, %xmm14
	vpsrldq		$4, %xmm5, %xmm12
	vpblendw	$48, %xmm12, %xmm1, %xmm1
	vpshufd		$33, %xmm5, %xmm12
	vpshufd		$57, %xmm14, %xmm14
	vpshufd		$108, %xmm1, %xmm1
	vpblendw	$51, %xmm12, %xmm10, %xmm12
	vpaddd		%xmm15, %xmm1, %xmm15
	vpaddd		%xmm14, %xmm15, %xmm15
	vpxor		%xmm15, %xmm0, %xmm0
	vpaddd		%xmm12, %xmm15, %xmm15
	vpshufb		%xmm7, %xmm0, %xmm0
	vpaddd		%xmm0, %xmm13, %xmm1
	vpxor		%xmm1, %xmm14, %xmm14
	vpslld		$20, %xmm14, %xmm13
	vpsrld		$12, %xmm14, %xmm14
	vpxor		%xmm13, %xmm14, %xmm14
	vpslldq		$12, %xmm3, %xmm13
	vpaddd		%xmm14, %xmm15, %xmm15
	vpxor		%xmm15, %xmm0, %xmm0
	vpshufb		%xmm6, %xmm0, %xmm0
	vpaddd		%xmm0, %xmm1, %xmm1
	vpxor		%xmm1, %xmm14, %xmm14
	vpshufd		$57, %xmm0, %xmm0
	vpshufd		$78, %xmm1, %xmm1
	vpslld		$25, %xmm14, %xmm12
	vpsrld		$7, %xmm14, %xmm14
	vpxor		%xmm12, %xmm14, %xmm14
	vpblendw	$51, %xmm5, %xmm4, %xmm12
	vpshufd		$147, %xmm14, %xmm14
	vpblendw	$192, %xmm13, %xmm12, %xmm12
	vpaddd		%xmm12, %xmm15, %xmm15
	vpaddd		%xmm14, %xmm15, %xmm15
	vpxor		%xmm15, %xmm0, %xmm0
	vpsrldq		$4, %xmm3, %xmm12
	vpshufb		%xmm7, %xmm0, %xmm0
	vpaddd		%xmm0, %xmm1, %xmm1
	vpxor		%xmm1, %xmm14, %xmm14
	vpslld		$20, %xmm14, %xmm13
	vpsrld		$12, %xmm14, %xmm14
	vpxor		%xmm13, %xmm14, %xmm14
	vpblendw	$48, %xmm2, %xmm5, %xmm13
	vpblendw	$3, %xmm12, %xmm13, %xmm13
	vpshufd		$156, %xmm13, %xmm13
	vpaddd		%xmm15, %xmm13, %xmm15
	vpaddd		%xmm14, %xmm15, %xmm15
	vpxor		%xmm15, %xmm0, %xmm0
	vpshufb		%xmm6, %xmm0, %xmm0
	vpaddd		%xmm0, %xmm1, %xmm1
	vpxor		%xmm1, %xmm14, %xmm14
	vpshufd		$147, %xmm0, %xmm0
	vpshufd		$78, %xmm1, %xmm1
	vpslld		$25, %xmm14, %xmm13
	vpsrld		$7, %xmm14, %xmm14
	vpxor		%xmm13, %xmm14, %xmm14
	vpunpcklqdq	%xmm2, %xmm4, %xmm13
	vpshufd		$57, %xmm14, %xmm14
	vpblendw	$12, %xmm12, %xmm13, %xmm12
	vpshufd		$180, %xmm12, %xmm12
	vpaddd		%xmm15, %xmm12, %xmm15
	vpaddd		%xmm14, %xmm15, %xmm15
	vpxor		%xmm15, %xmm0, %xmm0
	vpshufb		%xmm7, %xmm0, %xmm0
	vpaddd		%xmm0, %xmm1, %xmm1
	vpxor		%xmm1, %xmm14, %xmm14
	vpslld		$20, %xmm14, %xmm12
	vpsrld		$12, %xmm14, %xmm14
	vpxor		%xmm12, %xmm14, %xmm14
	vpunpckhqdq	%xmm9, %xmm4, %xmm12
	vpshufd		$198, %xmm12, %xmm12
	vpaddd		%xmm15, %xmm12, %xmm15
	vpaddd		%xmm14, %xmm15, %xmm15
	vpxor		%xmm15, %xmm0, %xmm0
	vpaddd		%xmm15, %xmm8, %xmm15
	vpshufb		%xmm6, %xmm0, %xmm0
	vpaddd		%xmm0, %xmm1, %xmm1
	vpxor		%xmm1, %xmm14, %xmm14
	vpshufd		$57, %xmm0, %xmm0
	vpshufd		$78, %xmm1, %xmm1
	vpslld		$25, %xmm14, %xmm12
	vpsrld		$7, %xmm14, %xmm14
	vpxor		%xmm12, %xmm14, %xmm14
	vpsrldq		$4, %xmm4, %xmm12
	vpshufd		$147, %xmm14, %xmm14
	vpaddd		%xmm14, %xmm15, %xmm15
	vpxor		%xmm15, %xmm0, %xmm0
	vpshufb		%xmm7, %xmm0, %xmm0
	vpaddd		%xmm0, %xmm1, %xmm1
	vpxor		%xmm1, %xmm14, %xmm14
	vpslld		$20, %xmm14, %xmm8
	vpsrld		$12, %xmm14, %xmm14
	vpxor		%xmm14, %xmm8, %xmm14
	vpblendw	$48, %xmm5, %xmm2, %xmm8
	vpblendw	$3, %xmm12, %xmm8, %xmm8
	vpunpckhqdq	%xmm5, %xmm4, %xmm12
	vpshufd		$75, %xmm8, %xmm8
	vpblendw	$60, %xmm10, %xmm12, %xmm10
	vpaddd		%xmm15, %xmm8, %xmm15
	vpaddd		%xmm14, %xmm15, %xmm15
	vpxor		%xmm0, %xmm15, %xmm0
	vpshufd		$45, %xmm10, %xmm10
	vpshufb		%xmm6, %xmm0, %xmm0
	vpaddd		%xmm15, %xmm10, %xmm15
	vpaddd		%xmm0, %xmm1, %xmm1
	vpxor		%xmm1, %xmm14, %xmm14
	vpshufd		$147, %xmm0, %xmm0
	vpshufd		$78, %xmm1, %xmm1
	vpslld		$25, %xmm14, %xmm8
	vpsrld		$7, %xmm14, %xmm14
	vpxor		%xmm14, %xmm8, %xmm8
	vpshufd		$57, %xmm8, %xmm8
	vpaddd		%xmm8, %xmm15, %xmm15
	vpxor		%xmm0, %xmm15, %xmm0
	vpshufb		%xmm7, %xmm0, %xmm0
	vpaddd		%xmm0, %xmm1, %xmm1
	vpxor		%xmm8, %xmm1, %xmm8
	vpslld		$20, %xmm8, %xmm10
	vpsrld		$12, %xmm8, %xmm8
	vpxor		%xmm8, %xmm10, %xmm10
	vpunpckldq	%xmm3, %xmm4, %xmm8
	vpunpcklqdq	%xmm9, %xmm8, %xmm9
	vpaddd		%xmm9, %xmm15, %xmm9
	vpaddd		%xmm10, %xmm9, %xmm9
	vpxor		%xmm0, %xmm9, %xmm8
	vpshufb		%xmm6, %xmm8, %xmm8
	vpaddd		%xmm8, %xmm1, %xmm1
	vpxor		%xmm1, %xmm10, %xmm10
	vpshufd		$57, %xmm8, %xmm8
	vpshufd		$78, %xmm1, %xmm1
	vpslld		$25, %xmm10, %xmm12
	vpsrld		$7, %xmm10, %xmm10
	vpxor		%xmm10, %xmm12, %xmm10
	vpblendw	$48, %xmm4, %xmm3, %xmm12
	vpshufd		$147, %xmm10, %xmm0
	vpunpckhdq	%xmm5, %xmm3, %xmm10
	vpshufd		$78, %xmm12, %xmm12
	vpunpcklqdq	%xmm4, %xmm10, %xmm10
	vpblendw	$192, %xmm2, %xmm10, %xmm10
	vpshufhw	$78, %xmm10, %xmm10
	vpaddd		%xmm10, %xmm9, %xmm10
	vpaddd		%xmm0, %xmm10, %xmm10
	vpxor		%xmm8, %xmm10, %xmm8
	vpshufb		%xmm7, %xmm8, %xmm8
	vpaddd		%xmm8, %xmm1, %xmm1
	vpxor		%xmm0, %xmm1, %xmm9
	vpslld		$20, %xmm9, %xmm0
	vpsrld		$12, %xmm9, %xmm9
	vpxor		%xmm9, %xmm0, %xmm0
	vpunpckhdq	%xmm5, %xmm4, %xmm9
	vpblendw	$240, %xmm9, %xmm2, %xmm13
	vpshufd		$39, %xmm13, %xmm13
	vpaddd		%xmm10, %xmm13, %xmm10
	vpaddd		%xmm0, %xmm10, %xmm10
	vpxor		%xmm8, %xmm10, %xmm8
	vpblendw	$12, %xmm4, %xmm2, %xmm13
	vpshufb		%xmm6, %xmm8, %xmm8
	vpslldq		$4, %xmm13, %xmm13
	vpblendw	$15, %xmm5, %xmm13, %xmm13
	vpaddd		%xmm8, %xmm1, %xmm1
	vpxor		%xmm1, %xmm0, %xmm0
	vpaddd		%xmm13, %xmm10, %xmm13
	vpshufd		$147, %xmm8, %xmm8
	vpshufd		$78, %xmm1, %xmm1
	vpslld		$25, %xmm0, %xmm14
	vpsrld		$7, %xmm0, %xmm0
	vpxor		%xmm0, %xmm14, %xmm14
	vpshufd		$57, %xmm14, %xmm14
	vpaddd		%xmm14, %xmm13, %xmm13
	vpxor		%xmm8, %xmm13, %xmm8
	vpaddd		%xmm13, %xmm12, %xmm12
	vpshufb		%xmm7, %xmm8, %xmm8
	vpaddd		%xmm8, %xmm1, %xmm1
	vpxor		%xmm14, %xmm1, %xmm14
	vpslld		$20, %xmm14, %xmm10
	vpsrld		$12, %xmm14, %xmm14
	vpxor		%xmm14, %xmm10, %xmm10
	vpaddd		%xmm10, %xmm12, %xmm12
	vpxor		%xmm8, %xmm12, %xmm8
	vpshufb		%xmm6, %xmm8, %xmm8
	vpaddd		%xmm8, %xmm1, %xmm1
	vpxor		%xmm1, %xmm10, %xmm0
	vpshufd		$57, %xmm8, %xmm8
	vpshufd		$78, %xmm1, %xmm1
	vpslld		$25, %xmm0, %xmm10
	vpsrld		$7, %xmm0, %xmm0
	vpxor		%xmm0, %xmm10, %xmm10
	vpblendw	$48, %xmm2, %xmm3, %xmm0
	vpblendw	$15, %xmm11, %xmm0, %xmm0
	vpshufd		$147, %xmm10, %xmm10
	vpshufd		$114, %xmm0, %xmm0
	vpaddd		%xmm12, %xmm0, %xmm0
	vpaddd		%xmm10, %xmm0, %xmm0
	vpxor		%xmm8, %xmm0, %xmm8
	vpshufb		%xmm7, %xmm8, %xmm8
	vpaddd		%xmm8, %xmm1, %xmm1
	vpxor		%xmm10, %xmm1, %xmm10
	vpslld		$20, %xmm10, %xmm11
	vpsrld		$12, %xmm10, %xmm10
	vpxor		%xmm10, %xmm11, %xmm10
	vpslldq		$4, %xmm4, %xmm11
	vpblendw	$192, %xmm11, %xmm3, %xmm3
	vpunpckldq	%xmm5, %xmm4, %xmm4
	vpshufd		$99, %xmm3, %xmm3
	vpaddd		%xmm0, %xmm3, %xmm3
	vpaddd		%xmm10, %xmm3, %xmm3
	vpxor		%xmm8, %xmm3, %xmm11
	vpunpckldq	%xmm5, %xmm2, %xmm0
	vpblendw	$192, %xmm2, %xmm5, %xmm2
	vpshufb		%xmm6, %xmm11, %xmm11
	vpunpckhqdq	%xmm0, %xmm9, %xmm0
	vpblendw	$15, %xmm4, %xmm2, %xmm4
	vpaddd		%xmm11, %xmm1, %xmm1
	vpxor		%xmm1, %xmm10, %xmm10
	vpshufd		$147, %xmm11, %xmm11
	vpshufd		$201, %xmm0, %xmm0
	vpslld		$25, %xmm10, %xmm8
	vpsrld		$7, %xmm10, %xmm10
	vpxor		%xmm10, %xmm8, %xmm10
	vpshufd		$78, %xmm1, %xmm1
	vpaddd		%xmm3, %xmm0, %xmm0
	vpshufd		$27, %xmm4, %xmm4
	vpshufd		$57, %xmm10, %xmm10
	vpaddd		%xmm10, %xmm0, %xmm0
	vpxor		%xmm11, %xmm0, %xmm11
	vpaddd		%xmm0, %xmm4, %xmm0
	vpshufb		%xmm7, %xmm11, %xmm7
	vpaddd		%xmm7, %xmm1, %xmm1
	vpxor		%xmm10, %xmm1, %xmm10
	vpslld		$20, %xmm10, %xmm8
	vpsrld		$12, %xmm10, %xmm10
	vpxor		%xmm10, %xmm8, %xmm8
	vpaddd		%xmm8, %xmm0, %xmm0
	vpxor		%xmm7, %xmm0, %xmm7
	vpshufb		%xmm6, %xmm7, %xmm6
	vpaddd		%xmm6, %xmm1, %xmm1
	vpxor		%xmm1, %xmm8, %xmm8
	vpshufd		$78, %xmm1, %xmm1
	vpshufd		$57, %xmm6, %xmm6
	vpslld		$25, %xmm8, %xmm2
	vpsrld		$7, %xmm8, %xmm8
	vpxor		%xmm8, %xmm2, %xmm8
	vpxor		(%rdi), %xmm1, %xmm1
	vpshufd		$147, %xmm8, %xmm8
	vpxor		%xmm0, %xmm1, %xmm0
	vmovups		%xmm0, (%rdi)
	vpxor		16(%rdi), %xmm8, %xmm0
	vpxor		%xmm6, %xmm0, %xmm6
	vmovups		%xmm6, 16(%rdi)
	addq		$64, %rsi
	decq		%rdx
	jnz .Lbeginofloop
.Lendofloop:
	ret
ENDPROC(blake2s_compress_avx)
#endif /* CONFIG_AS_AVX */

#ifdef CONFIG_AS_AVX512
ENTRY(blake2s_compress_avx512)
	vmovdqu		(%rdi),%xmm0
	vmovdqu		0x10(%rdi),%xmm1
	vmovdqu		0x20(%rdi),%xmm4
	vmovq		%rcx,%xmm5
	vmovdqa		IV(%rip),%xmm14
	vmovdqa		IV+16(%rip),%xmm15
	jmp		.Lblake2s_compress_avx512_mainloop
.align 32
.Lblake2s_compress_avx512_mainloop:
	vmovdqa		%xmm0,%xmm10
	vmovdqa		%xmm1,%xmm11
	vpaddq		%xmm5,%xmm4,%xmm4
	vmovdqa		%xmm14,%xmm2
	vpxor		%xmm15,%xmm4,%xmm3
	vmovdqu		(%rsi),%ymm6
	vmovdqu		0x20(%rsi),%ymm7
	addq		$0x40,%rsi
	leaq		SIGMA(%rip),%rax
	movb		$0xa,%cl
.Lblake2s_compress_avx512_roundloop:
	addq		$0x40,%rax
	vmovdqa		-0x40(%rax),%ymm8
	vmovdqa		-0x20(%rax),%ymm9
	vpermi2d	%ymm7,%ymm6,%ymm8
	vpermi2d	%ymm7,%ymm6,%ymm9
	vmovdqa		%ymm8,%ymm6
	vmovdqa		%ymm9,%ymm7
	vpaddd		%xmm8,%xmm0,%xmm0
	vpaddd		%xmm1,%xmm0,%xmm0
	vpxor		%xmm0,%xmm3,%xmm3
	vprord		$0x10,%xmm3,%xmm3
	vpaddd		%xmm3,%xmm2,%xmm2
	vpxor		%xmm2,%xmm1,%xmm1
	vprord		$0xc,%xmm1,%xmm1
	vextracti128	$0x1,%ymm8,%xmm8
	vpaddd		%xmm8,%xmm0,%xmm0
	vpaddd		%xmm1,%xmm0,%xmm0
	vpxor		%xmm0,%xmm3,%xmm3
	vprord		$0x8,%xmm3,%xmm3
	vpaddd		%xmm3,%xmm2,%xmm2
	vpxor		%xmm2,%xmm1,%xmm1
	vprord		$0x7,%xmm1,%xmm1
	vpshufd		$0x39,%xmm1,%xmm1
	vpshufd		$0x4e,%xmm2,%xmm2
	vpshufd		$0x93,%xmm3,%xmm3
	vpaddd		%xmm9,%xmm0,%xmm0
	vpaddd		%xmm1,%xmm0,%xmm0
	vpxor		%xmm0,%xmm3,%xmm3
	vprord		$0x10,%xmm3,%xmm3
	vpaddd		%xmm3,%xmm2,%xmm2
	vpxor		%xmm2,%xmm1,%xmm1
	vprord		$0xc,%xmm1,%xmm1
	vextracti128	$0x1,%ymm9,%xmm9
	vpaddd		%xmm9,%xmm0,%xmm0
	vpaddd		%xmm1,%xmm0,%xmm0
	vpxor		%xmm0,%xmm3,%xmm3
	vprord		$0x8,%xmm3,%xmm3
	vpaddd		%xmm3,%xmm2,%xmm2
	vpxor		%xmm2,%xmm1,%xmm1
	vprord		$0x7,%xmm1,%xmm1
	vpshufd		$0x93,%xmm1,%xmm1
	vpshufd		$0x4e,%xmm2,%xmm2
	vpshufd		$0x39,%xmm3,%xmm3
	decb		%cl
	jne		.Lblake2s_compress_avx512_roundloop
	vpxor		%xmm10,%xmm0,%xmm0
	vpxor		%xmm11,%xmm1,%xmm1
	vpxor		%xmm2,%xmm0,%xmm0
	vpxor		%xmm3,%xmm1,%xmm1
	decq		%rdx
	jne		.Lblake2s_compress_avx512_mainloop
	vmovdqu		%xmm0,(%rdi)
	vmovdqu		%xmm1,0x10(%rdi)
	vmovdqu		%xmm4,0x20(%rdi)
	vzeroupper
	retq
ENDPROC(blake2s_compress_avx512)
#endif /* CONFIG_AS_AVX512 */
