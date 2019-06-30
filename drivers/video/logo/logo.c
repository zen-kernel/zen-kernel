
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

#ifdef CONFIG_LOGO_RANDOM
#include <linux/random.h>
#endif

#ifdef CONFIG_M68K
#include <asm/setup.h>
#endif

static bool nologo;
module_param(nologo, bool, 0);
MODULE_PARM_DESC(nologo, "Disables startup logo");

/* Monochromatic logos */
static const struct linux_logo *logo_mono[] = {
#ifdef CONFIG_LOGO_LINUX_MONO
      &logo_linux_mono,		/* Generic Linux logo */
#endif
#ifdef CONFIG_LOGO_SUPERH_MONO
      &logo_superh_mono,		/* SuperH Linux logo */
#endif
};

/* 16-colour logos */
static const struct linux_logo *logo_vga16[] = {
#ifdef CONFIG_LOGO_LINUX_VGA16
      &logo_linux_vga16,		/* Generic Linux logo */
#endif
#ifdef CONFIG_LOGO_SUPERH_VGA16
      &logo_superh_vga16,		/* SuperH Linux logo */
#endif
};

/* 224-colour logos */
static const struct linux_logo *logo_clut224[] = {
#ifdef CONFIG_LOGO_LINUX_CLUT224
      &logo_linux_clut224,		/* Generic Linux logo */
#endif
#ifdef CONFIG_LOGO_DEC_CLUT224
      &logo_dec_clut224,		/* DEC Linux logo on MIPS/MIPS64 or ALPHA */
#endif
#ifdef CONFIG_LOGO_MAC_CLUT224
      &logo_mac_clut224,		/* Macintosh Linux logo on m68k */
#endif
#ifdef CONFIG_LOGO_PARISC_CLUT224
      &logo_parisc_clut224,		/* PA-RISC Linux logo */
#endif
#ifdef CONFIG_LOGO_SGI_CLUT224
	&logo_sgi_clut224,		/* SGI Linux logo on MIPS/MIPS64 */
#endif
#ifdef CONFIG_LOGO_SUN_CLUT224
	&logo_sun_clut224,		/* Sun Linux logo */
#endif
#ifdef CONFIG_LOGO_SUPERH_CLUT224
	&logo_superh_clut224,		/* SuperH Linux logo */
#endif
#ifdef CONFIG_LOGO_ZEN_CLUT224
	&logo_zen_clut224,		/* Zen-Kernel logo */
#endif
#ifdef CONFIG_LOGO_OLDZEN_CLUT224
	&logo_oldzen_clut224,		/* Old Zen-Kernel logo */
#endif
#ifdef CONFIG_LOGO_ARCH_CLUT224
	&logo_arch_clut224,		/* Arch Linux logo */
#endif
#ifdef CONFIG_LOGO_GENTOO_CLUT224
	&logo_gentoo_clut224,		/* Gentoo Linux logo */
#endif
#ifdef CONFIG_LOGO_EXHERBO_CLUT224
	&logo_exherbo_clut224,		/* Exherbo Linux logo */
#endif
#ifdef CONFIG_LOGO_SLACKWARE_CLUT224
	&logo_slackware_clut224,	/* Slackware Linux logo */
#endif
#ifdef CONFIG_LOGO_DEBIAN_CLUT224
	&logo_debian_clut224,		/* Debian Linux logo */
#endif
#ifdef CONFIG_LOGO_FEDORASIMPLE_CLUT224
	&logo_fedorasimple_clut224,	/* Fedora Simple logo */
#endif
#ifdef CONFIG_LOGO_FEDORAGLOSSY_CLUT224
	&logo_fedoraglossy_clut224,	/* Fedora Glossy logo */
#endif
#ifdef CONFIG_LOGO_TITS_CLUT224
	&logo_tits_clut224,		/* Tits logo */
#endif
#ifdef CONFIG_LOGO_BSD_CLUT224
	&logo_bsd_clut224,		/* BSD logo */
#endif
#ifdef CONFIG_LOGO_FBSD_CLUT224
	&logo_fbsd_clut224,		/* Free BSD logo */
#endif
};

#ifdef CONFIG_LOGO_RANDOM
#define LOGO_INDEX(s)	(get_random_int() % s)
#else
#define LOGO_INDEX(s)	(s - 1)
#endif

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
	const struct linux_logo **array = NULL;
	unsigned int size;

	if (nologo || logos_freed)
		return NULL;

	/* Select logo array */
	if (depth >= 1) {
		array = logo_mono;
		size = ARRAY_SIZE(logo_mono);
	}
	if (depth >= 4) {
		array = logo_vga16;
		size = ARRAY_SIZE(logo_vga16);
	}
	if (depth >= 8) {
		array = logo_clut224;
		size = ARRAY_SIZE(logo_clut224);
	}

	/* We've got some logos to display */
	if (array && size)
		logo = array[LOGO_INDEX(size)];

	return logo;
}
EXPORT_SYMBOL_GPL(fb_find_logo);
