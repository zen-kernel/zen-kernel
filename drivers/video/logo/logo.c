// SPDX-License-Identifier: GPL-2.0-only

/*
 *  Linux logo to be displayed on boot
 *
 *  Copyright (C) 1996 Larry Ewing (lewing@isc.tamu.edu)
 *  Copyright (C) 1996,1998 Jakub Jelinek (jj@sunsite.mff.cuni.cz)
 *  Copyright (C) 2001 Greg Banks <gnb@alphalink.com.au>
 *  Copyright (C) 2001 Jan-Benedict Glaw <jbglaw@lug-owl.de>
 *  Copyright (C) 2003 Geert Uytterhoeven <geert@linux-m68k.org>
 */

#include <linux/linux_logo.h>
#include <linux/stddef.h>
#include <linux/module.h>

#ifdef CONFIG_M68K
#include <asm/setup.h>
#endif

static bool nologo;
module_param(nologo, bool, 0);
MODULE_PARM_DESC(nologo, "Disables startup logo");

/*
 * Logos are located in the initdata, and will be freed in kernel_init.
 * Use late_init to mark the logos as freed to prevent any further use.
 */

static bool logos_freed;

static int __init fb_logo_late_init(void)
{
	logos_freed = true;
	return 0;
}

late_initcall_sync(fb_logo_late_init);

/* logo's are marked __initdata. Use __ref to tell
 * modpost that it is intended that this function uses data
 * marked __initdata.
 */
const struct linux_logo * __ref fb_find_logo(int depth)
{
	const struct linux_logo *logo = NULL;

	if (nologo || logos_freed)
		return NULL;

	if (depth >= 1) {
#ifdef CONFIG_LOGO_LINUX_MONO
		/* Generic Linux logo */
		logo = &logo_linux_mono;
#endif
#ifdef CONFIG_LOGO_SUPERH_MONO
		/* SuperH Linux logo */
		logo = &logo_superh_mono;
#endif
	}
	
	if (depth >= 4) {
#ifdef CONFIG_LOGO_LINUX_VGA16
		/* Generic Linux logo */
		logo = &logo_linux_vga16;
#endif
#ifdef CONFIG_LOGO_SUPERH_VGA16
		/* SuperH Linux logo */
		logo = &logo_superh_vga16;
#endif
	}
	
	if (depth >= 8) {
#ifdef CONFIG_LOGO_LINUX_CLUT224
		/* Generic Linux logo */
		logo = &logo_linux_clut224;
#endif
#ifdef CONFIG_LOGO_DEC_CLUT224
		/* DEC Linux logo on MIPS/MIPS64 or ALPHA */
		logo = &logo_dec_clut224;
#endif
#ifdef CONFIG_LOGO_MAC_CLUT224
		/* Macintosh Linux logo on m68k */
		if (MACH_IS_MAC)
			logo = &logo_mac_clut224;
#endif
#ifdef CONFIG_LOGO_PARISC_CLUT224
		/* PA-RISC Linux logo */
		logo = &logo_parisc_clut224;
#endif
#ifdef CONFIG_LOGO_SGI_CLUT224
		/* SGI Linux logo on MIPS/MIPS64 */
		logo = &logo_sgi_clut224;
#endif
#ifdef CONFIG_LOGO_SUN_CLUT224
		/* Sun Linux logo */
		logo = &logo_sun_clut224;
#endif
#ifdef CONFIG_LOGO_SUPERH_CLUT224
		/* SuperH Linux logo */
		logo = &logo_superh_clut224;
#endif
#ifdef CONFIG_LOGO_ZEN_CLUT224
		/* Zen-Kernel logo */
		logo = &logo_zen_clut224;
#endif
#ifdef CONFIG_LOGO_OLDZEN_CLUT224
		/* Old Zen-Kernel logo */
		logo = &logo_oldzen_clut224;
#endif
#ifdef CONFIG_LOGO_ARCH_CLUT224
		/* Arch Linux logo */
		logo = &logo_arch_clut224;
#endif
#ifdef CONFIG_LOGO_GENTOO_CLUT224
		/* Gentoo Linux logo */
		logo = &logo_gentoo_clut224;
#endif
#ifdef CONFIG_LOGO_EXHERBO_CLUT224
		/* Exherbo Linux logo */
		logo = &logo_exherbo_clut224;
#endif
#ifdef CONFIG_LOGO_SLACKWARE_CLUT224
		/* Slackware Linux logo */
		logo = &logo_slackware_clut224;
#endif
#ifdef CONFIG_LOGO_DEBIAN_CLUT224
		/* Debian Linux logo */
		logo = &logo_debian_clut224;
#endif
#ifdef CONFIG_LOGO_FEDORASIMPLE_CLUT224
		/* Fedora Simple logo */
		logo = &logo_fedorasimple_clut224;
#endif
#ifdef CONFIG_LOGO_FEDORAGLOSSY_CLUT224
		/* Fedora Glossy logo */
		logo = &logo_fedoraglossy_clut224;
#endif
#ifdef CONFIG_LOGO_TITS_CLUT224
		/* Tits logo */
		logo = &logo_tits_clut224;
#endif
#ifdef CONFIG_LOGO_BSD_CLUT224
		/* BSD logo */
		logo = &logo_bsd_clut224;
#endif
#ifdef CONFIG_LOGO_FBSD_CLUT224
		/* Free BSD logo */
		logo = &logo_fbsd_clut224;
#endif
	}
	return logo;
}
EXPORT_SYMBOL_GPL(fb_find_logo);
