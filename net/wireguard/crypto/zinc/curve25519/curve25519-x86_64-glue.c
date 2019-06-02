// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright (C) 2015-2019 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <asm/cpufeature.h>
#include <asm/processor.h>

#include "curve25519-x86_64.c"

static bool curve25519_use_bmi2 __ro_after_init;
static bool curve25519_use_adx __ro_after_init;
static bool *const curve25519_nobs[] __initconst = {
	&curve25519_use_bmi2, &curve25519_use_adx };

static void __init curve25519_fpu_init(void)
{
	curve25519_use_bmi2 = boot_cpu_has(X86_FEATURE_BMI2);
	curve25519_use_adx = boot_cpu_has(X86_FEATURE_BMI2) &&
			     boot_cpu_has(X86_FEATURE_ADX);
}

static inline bool curve25519_arch(u8 mypublic[CURVE25519_KEY_SIZE],
				   const u8 secret[CURVE25519_KEY_SIZE],
				   const u8 basepoint[CURVE25519_KEY_SIZE])
{
	if (curve25519_use_adx) {
		curve25519_adx(mypublic, secret, basepoint);
		return true;
	} else if (curve25519_use_bmi2) {
		curve25519_bmi2(mypublic, secret, basepoint);
		return true;
	}
	return false;
}

static inline bool curve25519_base_arch(u8 pub[CURVE25519_KEY_SIZE],
					const u8 secret[CURVE25519_KEY_SIZE])
{
	if (curve25519_use_adx) {
		curve25519_adx_base(pub, secret);
		return true;
	} else if (curve25519_use_bmi2) {
		curve25519_bmi2_base(pub, secret);
		return true;
	}
	return false;
}
