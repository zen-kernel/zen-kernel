/*
    LZ4 HC - High Compression Mode of LZ4
    Copyright (C) 2011-2012, Yann Collet.
    L-GPL v3 License

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License along
    with this program; if not, see <http://www.gnu.org/licenses/>,
	or write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

	You can contact the author at :
	- LZ4 homepage : http://fastcompression.blogspot.com/p/lz4.html
	- LZ4-HC source repository : http://code.google.com/p/lz4hc/
*/
#pragma once


#if defined (__cplusplus)
extern "C" {
#endif


int LZ4_compressHC (const char* source, char* dest, int isize);

/*
LZ4_compressHC :
	return : the number of bytes in compressed buffer dest
	note : destination buffer must be already allocated. 
		To avoid any problem, size it to handle worst cases situations (input data not compressible)
		Worst case size evaluation is provided by function LZ4_compressBound() (see "lz4.h")
*/


/* Note :
Decompression functions are provided within regular LZ4 source code (see "lz4.h") (BSD license)
*/

int LZ4_compressHCCtx(void* ctx,
				 const char* source, 
				 char* dest,
				 int isize);

int LZ4_contextHC_size(void);
void LZ4_contextHC_init(void *ctx, const void *base);

#if defined (__cplusplus)
}
#endif
