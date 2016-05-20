/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Scalable on-disk integers */

/*
 * Various on-disk structures contain integer-like structures. Stat-data
 * contain [yes, "data" is plural, check the dictionary] file size, link
 * count; extent unit contains extent width etc. To accommodate for general
 * case enough space is reserved to keep largest possible value. 64 bits in
 * all cases above. But in overwhelming majority of cases numbers actually
 * stored in these fields will be comparatively small and reserving 8 bytes is
 * a waste of precious disk bandwidth.
 *
 * Scalable integers are one way to solve this problem. dscale_write()
 * function stores __u64 value in the given area consuming from 1 to 9 bytes,
 * depending on the magnitude of the value supplied. dscale_read() reads value
 * previously stored by dscale_write().
 *
 * dscale_write() produces format not completely unlike of UTF: two highest
 * bits of the first byte are used to store "tag". One of 4 possible tag
 * values is chosen depending on the number being encoded:
 *
 *           0 ... 0x3f               => 0           [table 1]
 *        0x40 ... 0x3fff             => 1
 *      0x4000 ... 0x3fffffff         => 2
 *  0x40000000 ... 0xffffffffffffffff => 3
 *
 * (see dscale_range() function)
 *
 * Values in the range 0x40000000 ... 0xffffffffffffffff require 8 full bytes
 * to be stored, so in this case there is no place in the first byte to store
 * tag. For such values tag is stored in an extra 9th byte.
 *
 * As _highest_ bits are used for the test (which is natural) scaled integers
 * are stored in BIG-ENDIAN format in contrast with the rest of reiser4 which
 * uses LITTLE-ENDIAN.
 *
 */

#include "debug.h"
#include "dscale.h"

/* return tag of scaled integer stored at @address */
static int gettag(const unsigned char *address)
{
	/* tag is stored in two highest bits */
	return (*address) >> 6;
}

/* clear tag from value. Clear tag embedded into @value. */
static void cleartag(__u64 *value, int tag)
{
	/*
	 * W-w-what ?!
	 *
	 * Actually, this is rather simple: @value passed here was read by
	 * dscale_read(), converted from BIG-ENDIAN, and padded to __u64 by
	 * zeroes. Tag is still stored in the highest (arithmetically)
	 * non-zero bits of @value, but relative position of tag within __u64
	 * depends on @tag.
	 *
	 * For example if @tag is 0, it's stored 2 highest bits of lowest
	 * byte, and its offset (counting from lowest bit) is 8 - 2 == 6 bits.
	 *
	 * If tag is 1, it's stored in two highest bits of 2nd lowest byte,
	 * and it's offset if (2 * 8) - 2 == 14 bits.
	 *
	 * See table 1 above for details.
	 *
	 * All these cases are captured by the formula:
	 */
	*value &= ~(3 << (((1 << tag) << 3) - 2));
	/*
	 * That is, clear two (3 == 0t11) bits at the offset
	 *
	 *                  8 * (2 ^ tag) - 2,
	 *
	 * that is, two highest bits of (2 ^ tag)-th byte of @value.
	 */
}

/* return tag for @value. See table 1 above for details. */
static int dscale_range(__u64 value)
{
	if (value > 0x3fffffff)
		return 3;
	if (value > 0x3fff)
		return 2;
	if (value > 0x3f)
		return 1;
	return 0;
}

/* restore value stored at @adderss by dscale_write() and return number of
 * bytes consumed */
int dscale_read(unsigned char *address, __u64 *value)
{
	int tag;

	/* read tag */
	tag = gettag(address);
	switch (tag) {
	case 3:
		/* In this case tag is stored in an extra byte, skip this byte
		 * and decode value stored in the next 8 bytes.*/
		*value = __be64_to_cpu(get_unaligned((__be64 *)(address + 1)));
		/* worst case: 8 bytes for value itself plus one byte for
		 * tag. */
		return 9;
	case 0:
		*value = get_unaligned(address);
		break;
	case 1:
		*value = __be16_to_cpu(get_unaligned((__be16 *)address));
		break;
	case 2:
		*value = __be32_to_cpu(get_unaligned((__be32 *)address));
		break;
	default:
		return RETERR(-EIO);
	}
	/* clear tag embedded into @value */
	cleartag(value, tag);
	/* number of bytes consumed is (2 ^ tag)---see table 1. */
	return 1 << tag;
}

/* number of bytes consumed */
int dscale_bytes_to_read(unsigned char *address)
{
	int tag;

	tag = gettag(address);
	switch (tag) {
	case 0:
	case 1:
	case 2:
		return 1 << tag;
	case 3:
		return 9;
	default:
		return RETERR(-EIO);
	}
}

/* store @value at @address and return number of bytes consumed */
int dscale_write(unsigned char *address, __u64 value)
{
	int tag;
	int shift;
	__be64 v;
	unsigned char *valarr;

	tag = dscale_range(value);
	v = __cpu_to_be64(value);
	valarr = (unsigned char *)&v;
	shift = (tag == 3) ? 1 : 0;
	memcpy(address + shift, valarr + sizeof v - (1 << tag), 1 << tag);
	*address |= (tag << 6);
	return shift + (1 << tag);
}

/* number of bytes required to store @value */
int dscale_bytes_to_write(__u64 value)
{
	int bytes;

	bytes = 1 << dscale_range(value);
	if (bytes == 8)
		++bytes;
	return bytes;
}

/* returns true if @value and @other require the same number of bytes to be
 * stored. Used by detect when data structure (like stat-data) has to be
 * expanded or contracted. */
int dscale_fit(__u64 value, __u64 other)
{
	return dscale_range(value) == dscale_range(other);
}

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
