/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company. 

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").  

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#pragma hdrstop
#include "../idlib/precompiled.h"


#include "tr_local.h"

/*

This file only has a single entry point:

void R_LoadImage( const char *name, byte **pic, int *width, int *height, bool makePowerOf2 );

*/

/*
 * Include file for users of JPEG library.
 * You will need to have included system headers that define at least
 * the typedefs FILE and size_t before you can include jpeglib.h.
 * (stdio.h is sufficient on ANSI-conforming systems.)
 * You may also wish to include "jerror.h".
 */

#include "jpeg-6/jpeglib.h"

// zlib is used to inflate the IDAT stream of PNG files
#include "../framework/zlib/zlib.h"

// hooks from jpeg lib to our system

void jpg_Error( const char *fmt, ... ) {
	va_list		argptr;
	char		msg[2048];

	va_start (argptr,fmt);
	vsprintf (msg,fmt,argptr);
	va_end (argptr);

	common->FatalError( "%s", msg );
}

void jpg_Printf( const char *fmt, ... ) {
	va_list		argptr;
	char		msg[2048];

	va_start (argptr,fmt);
	vsprintf (msg,fmt,argptr);
	va_end (argptr);

	common->Printf( "%s", msg );
}



/*
================
R_WriteTGA
================
*/
void R_WriteTGA( const char *filename, const byte *data, int width, int height, bool flipVertical, const char * basePath ) {
	byte	*buffer;
	int		i;
	int		bufferSize = width*height*4 + 18;
	int     imgStart = 18;

	idTempArray<byte> buf( bufferSize );
	buffer = (byte *)buf.Ptr();
	memset( buffer, 0, 18 );
	buffer[2] = 2;		// uncompressed type
	buffer[12] = width&255;
	buffer[13] = width>>8;
	buffer[14] = height&255;
	buffer[15] = height>>8;
	buffer[16] = 32;	// pixel size
	if ( !flipVertical ) {
		buffer[17] = (1<<5);	// flip bit, for normal top to bottom raster order
	}

	// swap rgb to bgr
	for ( i=imgStart ; i<bufferSize ; i+=4 ) {
		buffer[i] = data[i-imgStart+2];		// blue
		buffer[i+1] = data[i-imgStart+1];		// green
		buffer[i+2] = data[i-imgStart+0];		// red
		buffer[i+3] = data[i-imgStart+3];		// alpha
	}

	fileSystem->WriteFile( filename, buffer, bufferSize, basePath );
}

static void LoadTGA( const char *name, byte **pic, int *width, int *height, ID_TIME_T *timestamp );
static void LoadJPG( const char *name, byte **pic, int *width, int *height, ID_TIME_T *timestamp );
static void LoadPNG( const char *name, byte **pic, int *width, int *height, ID_TIME_T *timestamp );

/*
========================================================================

TGA files are used for 24/32 bit images

========================================================================
*/

typedef struct _TargaHeader {
	unsigned char 	id_length, colormap_type, image_type;
	unsigned short	colormap_index, colormap_length;
	unsigned char	colormap_size;
	unsigned short	x_origin, y_origin, width, height;
	unsigned char	pixel_size, attributes;
} TargaHeader;


/*
=========================================================

TARGA LOADING

=========================================================
*/

/*
=============
LoadTGA
=============
*/
static void LoadTGA( const char *name, byte **pic, int *width, int *height, ID_TIME_T *timestamp ) {
	int		columns, rows, numPixels, fileSize, numBytes;
	byte	*pixbuf;
	int		row, column;
	byte	*buf_p;
	byte	*buffer;
	TargaHeader	targa_header;
	byte		*targa_rgba;

	if ( !pic ) {
		fileSystem->ReadFile( name, NULL, timestamp );
		return;	// just getting timestamp
	}

	*pic = NULL;

	//
	// load the file
	//
	fileSize = fileSystem->ReadFile( name, (void **)&buffer, timestamp );
	if ( !buffer ) {
		return;
	}

	buf_p = buffer;

	targa_header.id_length = *buf_p++;
	targa_header.colormap_type = *buf_p++;
	targa_header.image_type = *buf_p++;
	
	targa_header.colormap_index = LittleShort ( *(short *)buf_p );
	buf_p += 2;
	targa_header.colormap_length = LittleShort ( *(short *)buf_p );
	buf_p += 2;
	targa_header.colormap_size = *buf_p++;
	targa_header.x_origin = LittleShort ( *(short *)buf_p );
	buf_p += 2;
	targa_header.y_origin = LittleShort ( *(short *)buf_p );
	buf_p += 2;
	targa_header.width = LittleShort ( *(short *)buf_p );
	buf_p += 2;
	targa_header.height = LittleShort ( *(short *)buf_p );
	buf_p += 2;
	targa_header.pixel_size = *buf_p++;
	targa_header.attributes = *buf_p++;

	if ( targa_header.image_type != 2 && targa_header.image_type != 10 && targa_header.image_type != 3 ) {
		common->Error( "LoadTGA( %s ): Only type 2 (RGB), 3 (gray), and 10 (RGB) TGA images supported\n", name );
	}

	if ( targa_header.colormap_type != 0 ) {
		common->Error( "LoadTGA( %s ): colormaps not supported\n", name );
	}

	if ( ( targa_header.pixel_size != 32 && targa_header.pixel_size != 24 ) && targa_header.image_type != 3 ) {
		common->Error( "LoadTGA( %s ): Only 32 or 24 bit images supported (no colormaps)\n", name );
	}

	if ( targa_header.image_type == 2 || targa_header.image_type == 3 ) {
		numBytes = targa_header.width * targa_header.height * ( targa_header.pixel_size >> 3 );
		if ( numBytes > fileSize - 18 - targa_header.id_length ) {
			common->Error( "LoadTGA( %s ): incomplete file\n", name );
		}
	}

	columns = targa_header.width;
	rows = targa_header.height;
	numPixels = columns * rows;

	if ( width ) {
		*width = columns;
	}
	if ( height ) {
		*height = rows;
	}

	targa_rgba = (byte *)R_StaticAlloc(numPixels*4, TAG_IMAGE);
	*pic = targa_rgba;

	if ( targa_header.id_length != 0 ) {
		buf_p += targa_header.id_length;  // skip TARGA image comment
	}
	
	if ( targa_header.image_type == 2 || targa_header.image_type == 3 )
	{ 
		// Uncompressed RGB or gray scale image
		for( row = rows - 1; row >= 0; row-- )
		{
			pixbuf = targa_rgba + row*columns*4;
			for( column = 0; column < columns; column++)
			{
				unsigned char red,green,blue,alphabyte;
				switch( targa_header.pixel_size )
				{
					
				case 8:
					blue = *buf_p++;
					green = blue;
					red = blue;
					*pixbuf++ = red;
					*pixbuf++ = green;
					*pixbuf++ = blue;
					*pixbuf++ = 255;
					break;

				case 24:
					blue = *buf_p++;
					green = *buf_p++;
					red = *buf_p++;
					*pixbuf++ = red;
					*pixbuf++ = green;
					*pixbuf++ = blue;
					*pixbuf++ = 255;
					break;
				case 32:
					blue = *buf_p++;
					green = *buf_p++;
					red = *buf_p++;
					alphabyte = *buf_p++;
					*pixbuf++ = red;
					*pixbuf++ = green;
					*pixbuf++ = blue;
					*pixbuf++ = alphabyte;
					break;
				default:
					common->Error( "LoadTGA( %s ): illegal pixel_size '%d'\n", name, targa_header.pixel_size );
					break;
				}
			}
		}
	}
	else if ( targa_header.image_type == 10 ) {   // Runlength encoded RGB images
		unsigned char red,green,blue,alphabyte,packetHeader,packetSize,j;

		red = 0;
		green = 0;
		blue = 0;
		alphabyte = 0xff;

		for( row = rows - 1; row >= 0; row-- ) {
			pixbuf = targa_rgba + row*columns*4;
			for( column = 0; column < columns; ) {
				packetHeader= *buf_p++;
				packetSize = 1 + (packetHeader & 0x7f);
				if ( packetHeader & 0x80 ) {        // run-length packet
					switch( targa_header.pixel_size ) {
						case 24:
								blue = *buf_p++;
								green = *buf_p++;
								red = *buf_p++;
								alphabyte = 255;
								break;
						case 32:
								blue = *buf_p++;
								green = *buf_p++;
								red = *buf_p++;
								alphabyte = *buf_p++;
								break;
						default:
							common->Error( "LoadTGA( %s ): illegal pixel_size '%d'\n", name, targa_header.pixel_size );
							break;
					}
	
					for( j = 0; j < packetSize; j++ ) {
						*pixbuf++=red;
						*pixbuf++=green;
						*pixbuf++=blue;
						*pixbuf++=alphabyte;
						column++;
						if ( column == columns ) { // run spans across rows
							column = 0;
							if ( row > 0) {
								row--;
							}
							else {
								goto breakOut;
							}
							pixbuf = targa_rgba + row*columns*4;
						}
					}
				}
				else {                            // non run-length packet
					for( j = 0; j < packetSize; j++ ) {
						switch( targa_header.pixel_size ) {
							case 24:
									blue = *buf_p++;
									green = *buf_p++;
									red = *buf_p++;
									*pixbuf++ = red;
									*pixbuf++ = green;
									*pixbuf++ = blue;
									*pixbuf++ = 255;
									break;
							case 32:
									blue = *buf_p++;
									green = *buf_p++;
									red = *buf_p++;
									alphabyte = *buf_p++;
									*pixbuf++ = red;
									*pixbuf++ = green;
									*pixbuf++ = blue;
									*pixbuf++ = alphabyte;
									break;
							default:
								common->Error( "LoadTGA( %s ): illegal pixel_size '%d'\n", name, targa_header.pixel_size );
								break;
						}
						column++;
						if ( column == columns ) { // pixel packet run spans across rows
							column = 0;
							if ( row > 0 ) {
								row--;
							}
							else {
								goto breakOut;
							}
							pixbuf = targa_rgba + row*columns*4;
						}						
					}
				}
			}
			breakOut: ;
		}
	}

	if ( (targa_header.attributes & (1<<5)) ) {			// image flp bit
		if ( width != NULL && height != NULL ) {
			R_VerticalFlip( *pic, *width, *height );
		}
	}

	fileSystem->FreeFile( buffer );
}

/*
=========================================================

JPG LOADING

Interfaces with the huge libjpeg
=========================================================
*/

/*
=============
LoadJPG
=============
*/
static void LoadJPG( const char *filename, unsigned char **pic, int *width, int *height, ID_TIME_T *timestamp ) {
  /* This struct contains the JPEG decompression parameters and pointers to
   * working space (which is allocated as needed by the JPEG library).
   */
  struct jpeg_decompress_struct cinfo;
  /* We use our private extension JPEG error handler.
   * Note that this struct must live as long as the main JPEG parameter
   * struct, to avoid dangling-pointer problems.
   */
  /* This struct represents a JPEG error handler.  It is declared separately
   * because applications often want to supply a specialized error handler
   * (see the second half of this file for an example).  But here we just
   * take the easy way out and use the standard error handler, which will
   * print a message on stderr and call exit() if compression fails.
   * Note that this struct must live as long as the main JPEG parameter
   * struct, to avoid dangling-pointer problems.
   */
  struct jpeg_error_mgr jerr;
  /* More stuff */
  JSAMPARRAY buffer;		/* Output row buffer */
  int row_stride;		/* physical row width in output buffer */
  unsigned char *out;
  byte	*fbuffer;
  byte  *bbuf;

  /* In this example we want to open the input file before doing anything else,
   * so that the setjmp() error recovery below can assume the file is open.
   * VERY IMPORTANT: use "b" option to fopen() if you are on a machine that
   * requires it in order to read binary files.
   */

	// JDC: because fill_input_buffer() blindly copies INPUT_BUF_SIZE bytes,
	// we need to make sure the file buffer is padded or it may crash
  if ( pic ) {
	*pic = NULL;		// until proven otherwise
  }
  {
		int		len;
		idFile *f;

		f = fileSystem->OpenFileRead( filename );
		if ( !f ) {
			return;
		}
		len = f->Length();
		if ( timestamp ) {
			*timestamp = f->Timestamp();
		}
		if ( !pic ) {
			fileSystem->CloseFile( f );
			return;	// just getting timestamp
		}
		fbuffer = (byte *)Mem_ClearedAlloc( len + 4096, TAG_JPG );
		f->Read( fbuffer, len );
		fileSystem->CloseFile( f );
  }


  /* Step 1: allocate and initialize JPEG decompression object */

  /* We have to set up the error handler first, in case the initialization
   * step fails.  (Unlikely, but it could happen if you are out of memory.)
   * This routine fills in the contents of struct jerr, and returns jerr's
   * address which we place into the link field in cinfo.
   */
  cinfo.err = jpeg_std_error(&jerr);

  /* Now we can initialize the JPEG decompression object. */
  jpeg_create_decompress(&cinfo);

  /* Step 2: specify data source (eg, a file) */

  jpeg_stdio_src(&cinfo, fbuffer);

  /* Step 3: read file parameters with jpeg_read_header() */

  (void) jpeg_read_header(&cinfo, true );
  /* We can ignore the return value from jpeg_read_header since
   *   (a) suspension is not possible with the stdio data source, and
   *   (b) we passed TRUE to reject a tables-only JPEG file as an error.
   * See libjpeg.doc for more info.
   */

  /* Step 4: set parameters for decompression */

  /* In this example, we don't need to change any of the defaults set by
   * jpeg_read_header(), so we do nothing here.
   */

  /* Step 5: Start decompressor */

  (void) jpeg_start_decompress(&cinfo);
  /* We can ignore the return value since suspension is not possible
   * with the stdio data source.
   */

  /* We may need to do some setup of our own at this point before reading
   * the data.  After jpeg_start_decompress() we have the correct scaled
   * output image dimensions available, as well as the output colormap
   * if we asked for color quantization.
   * In this example, we need to make an output work buffer of the right size.
   */ 
  /* JSAMPLEs per row in output buffer */
  row_stride = cinfo.output_width * cinfo.output_components;

  if (cinfo.output_components!=4) {
		common->DWarning( "JPG %s is unsupported color depth (%d)", 
			filename, cinfo.output_components);
  }
  out = (byte *)R_StaticAlloc(cinfo.output_width*cinfo.output_height*4, TAG_IMAGE);

  *pic = out;
  *width = cinfo.output_width;
  *height = cinfo.output_height;

  /* Step 6: while (scan lines remain to be read) */
  /*           jpeg_read_scanlines(...); */

  /* Here we use the library's state variable cinfo.output_scanline as the
   * loop counter, so that we don't have to keep track ourselves.
   */
  while (cinfo.output_scanline < cinfo.output_height) {
    /* jpeg_read_scanlines expects an array of pointers to scanlines.
     * Here the array is only one element long, but you could ask for
     * more than one scanline at a time if that's more convenient.
     */
	bbuf = ((out+(row_stride*cinfo.output_scanline)));
	buffer = &bbuf;
    (void) jpeg_read_scanlines(&cinfo, buffer, 1);
  }

  // clear all the alphas to 255
  {
	  int	i, j;
		byte	*buf;

		buf = *pic;

	  j = cinfo.output_width * cinfo.output_height * 4;
	  for ( i = 3 ; i < j ; i+=4 ) {
		  buf[i] = 255;
	  }
  }

  /* Step 7: Finish decompression */

  (void) jpeg_finish_decompress(&cinfo);
  /* We can ignore the return value since suspension is not possible
   * with the stdio data source.
   */

  /* Step 8: Release JPEG decompression object */

  /* This is an important step since it will release a good deal of memory. */
  jpeg_destroy_decompress(&cinfo);

  /* After finish_decompress, we can close the input file.
   * Here we postpone it until after no more JPEG errors are possible,
   * so as to simplify the setjmp error logic above.  (Actually, I don't
   * think that jpeg_destroy can do an error exit, but why assume anything...)
   */
  Mem_Free( fbuffer );

  /* At this point you may want to check to see whether any corrupt-data
   * warnings occurred (test whether jerr.pub.num_warnings is nonzero).
   */

  /* And we're done! */
}

/*
=========================================================

PNG LOADING

Uses the in-tree zlib to inflate the IDAT stream, then
reconstructs the PNG row filters and expands the result to
the engine's canonical 32 bit RGBA format.

Supported color types (all 8 bit depth, non-interlaced):
   0 - grayscale
   2 - truecolor (RGB)
   3 - indexed (PLTE, with optional tRNS alpha)
   4 - grayscale + alpha
   6 - truecolor + alpha (RGBA)

Malformed or unsupported files are handled non-fatally with
common->Warning and *pic = NULL so a bad asset cannot crash
the engine.
=========================================================
*/

/*
=============
PNG_ReadUInt32

PNG stores chunk lengths and IHDR integers in big-endian order.
Read them byte-by-byte so the result is correct on any platform.
=============
*/
static ID_INLINE unsigned int PNG_ReadUInt32( const byte *p ) {
	return ( (unsigned int)p[0] << 24 ) | ( (unsigned int)p[1] << 16 ) |
		   ( (unsigned int)p[2] << 8 )  |   (unsigned int)p[3];
}

/*
=============
PNG_PaethPredictor
=============
*/
static ID_INLINE int PNG_PaethPredictor( int a, int b, int c ) {
	int p = a + b - c;
	int pa = abs( p - a );
	int pb = abs( p - b );
	int pc = abs( p - c );
	if ( pa <= pb && pa <= pc ) {
		return a;
	}
	if ( pb <= pc ) {
		return b;
	}
	return c;
}

/*
=============
LoadPNG
=============
*/
static void LoadPNG( const char *name, byte **pic, int *width, int *height, ID_TIME_T *timestamp ) {
	byte	*buffer;
	int		fileSize;

	if ( !pic ) {
		fileSystem->ReadFile( name, NULL, timestamp );
		return;	// just getting timestamp
	}

	*pic = NULL;

	//
	// load the file
	//
	fileSize = fileSystem->ReadFile( name, (void **)&buffer, timestamp );
	if ( !buffer ) {
		// The decl/image system lowercases asset names, but a loose file on disk
		// may be mixed-case; find the real filename by a case-insensitive match in
		// its directory and retry with the exact on-disk name.
		idStr want = name;
		idStr dir = want;
		dir.StripFilename();
		idStr wantBase = want;
		wantBase.StripPath();
		idFileList * fl = fileSystem->ListFilesTree( dir.c_str(), ".png", false );
		if ( fl != NULL ) {
			for ( int i = 0; i < fl->GetNumFiles() && !buffer; i++ ) {
				idStr candBase = fl->GetFile( i );
				candBase.StripPath();
				if ( candBase.Icmp( wantBase ) == 0 ) {
					fileSize = fileSystem->ReadFile( fl->GetFile( i ), (void **)&buffer, timestamp );
				}
			}
			fileSystem->FreeFileList( fl );
		}
		if ( !buffer ) {
			return;	// not found - normal for many assets in a dataless boot; stay quiet
		}
	}

	//
	// verify the 8 byte PNG signature
	//
	static const byte pngSignature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	if ( fileSize < 8 || memcmp( buffer, pngSignature, 8 ) != 0 ) {
		common->Warning( "LoadPNG( %s ): not a PNG file\n", name );
		fileSystem->FreeFile( buffer );
		return;
	}

	const byte	*end = buffer + fileSize;

	int		imgWidth = 0, imgHeight = 0;
	int		bitDepth = 0, colorType = 0, interlace = 0;
	bool	sawIHDR = false;

	byte	palette[256 * 3];
	int		paletteSize = 0;		// number of palette entries
	byte	trns[256];
	int		trnsSize = 0;			// number of tRNS alpha entries
	bool	haveTrns = false;

	int		idatTotal = 0;			// total size of concatenated IDAT data

	//
	// first pass: read IHDR / PLTE / tRNS and measure the IDAT stream
	//
	const byte *p = buffer + 8;
	while ( p + 8 <= end ) {
		unsigned int len = PNG_ReadUInt32( p );
		const byte *type = p + 4;
		const byte *data = p + 8;
		// make sure the chunk data actually fits in the file
		if ( len > (unsigned int)( end - data ) ) {
			break;
		}
		if ( memcmp( type, "IHDR", 4 ) == 0 ) {
			if ( len >= 13 ) {
				imgWidth   = (int)PNG_ReadUInt32( data );
				imgHeight  = (int)PNG_ReadUInt32( data + 4 );
				bitDepth   = data[8];
				colorType  = data[9];
				// data[10] = compression method, data[11] = filter method
				interlace  = data[12];
				sawIHDR = true;
			}
		} else if ( memcmp( type, "PLTE", 4 ) == 0 ) {
			paletteSize = len / 3;
			if ( paletteSize > 256 ) {
				paletteSize = 256;
			}
			memcpy( palette, data, paletteSize * 3 );
		} else if ( memcmp( type, "tRNS", 4 ) == 0 ) {
			trnsSize = len;
			if ( trnsSize > 256 ) {
				trnsSize = 256;
			}
			memcpy( trns, data, trnsSize );
			haveTrns = true;
		} else if ( memcmp( type, "IDAT", 4 ) == 0 ) {
			idatTotal += len;
		} else if ( memcmp( type, "IEND", 4 ) == 0 ) {
			break;
		}
		// advance past data + 4 byte CRC
		p = data + len + 4;
	}

	if ( !sawIHDR || imgWidth <= 0 || imgHeight <= 0 || idatTotal <= 0 ) {
		common->Warning( "LoadPNG( %s ): malformed or empty PNG\n", name );
		fileSystem->FreeFile( buffer );
		return;
	}

	if ( bitDepth != 8 ) {
		common->Warning( "LoadPNG( %s ): only 8 bit depth is supported (got %d)\n", name, bitDepth );
		fileSystem->FreeFile( buffer );
		return;
	}

	if ( interlace != 0 ) {
		common->Warning( "LoadPNG( %s ): interlaced PNG files are not supported\n", name );
		fileSystem->FreeFile( buffer );
		return;
	}

	int channels;
	switch ( colorType ) {
		case 0:	channels = 1; break;	// grayscale
		case 2:	channels = 3; break;	// RGB
		case 3:	channels = 1; break;	// palette index
		case 4:	channels = 2; break;	// grayscale + alpha
		case 6:	channels = 4; break;	// RGBA
		default:
			common->Warning( "LoadPNG( %s ): unsupported color type %d\n", name, colorType );
			fileSystem->FreeFile( buffer );
			return;
	}

	if ( colorType == 3 && paletteSize == 0 ) {
		common->Warning( "LoadPNG( %s ): indexed PNG has no PLTE chunk\n", name );
		fileSystem->FreeFile( buffer );
		return;
	}

	//
	// second pass: gather the IDAT bytes into one contiguous zlib stream
	//
	byte *idat = (byte *)Mem_Alloc( idatTotal, TAG_TEMP );
	int idatOffset = 0;
	p = buffer + 8;
	while ( p + 8 <= end ) {
		unsigned int len = PNG_ReadUInt32( p );
		const byte *type = p + 4;
		const byte *data = p + 8;
		if ( len > (unsigned int)( end - data ) ) {
			break;
		}
		if ( memcmp( type, "IDAT", 4 ) == 0 ) {
			if ( idatOffset + (int)len > idatTotal ) {
				break;
			}
			memcpy( idat + idatOffset, data, len );
			idatOffset += len;
		} else if ( memcmp( type, "IEND", 4 ) == 0 ) {
			break;
		}
		p = data + len + 4;
	}

	// the raw file is no longer needed; everything is copied out
	fileSystem->FreeFile( buffer );

	//
	// inflate the zlib stream: one filter byte per row plus the pixel data
	//
	int rowBytes = imgWidth * channels;
	int stride = 1 + rowBytes;
	int rawSize = imgHeight * stride;

	byte *raw = (byte *)Mem_Alloc( rawSize, TAG_TEMP );

	uLongf destLen = (uLongf)rawSize;
	int zret = uncompress( raw, &destLen, idat, (uLong)idatOffset );

	Mem_Free( idat );

	if ( zret != Z_OK || (int)destLen != rawSize ) {
		common->Warning( "LoadPNG( %s ): zlib inflate failed (%d)\n", name, zret );
		Mem_Free( raw );
		return;
	}

	//
	// undo the PNG row filters in place (None/Sub/Up/Average/Paeth)
	//
	int bpp = channels;		// bytes per pixel for 8 bit depth
	for ( int y = 0; y < imgHeight; y++ ) {
		byte filterType = raw[y * stride];
		byte *row = raw + y * stride + 1;
		byte *prev = ( y > 0 ) ? ( raw + ( y - 1 ) * stride + 1 ) : NULL;

		for ( int i = 0; i < rowBytes; i++ ) {
			int a = ( i >= bpp ) ? row[i - bpp] : 0;			// left
			int b = prev ? prev[i] : 0;							// above
			int c = ( prev && i >= bpp ) ? prev[i - bpp] : 0;	// upper-left
			int val = row[i];

			switch ( filterType ) {
				case 0:	break;								// None
				case 1:	val += a; break;					// Sub
				case 2:	val += b; break;					// Up
				case 3:	val += ( a + b ) >> 1; break;		// Average
				case 4:	val += PNG_PaethPredictor( a, b, c ); break;	// Paeth
				default:
					common->Warning( "LoadPNG( %s ): unknown row filter %d\n", name, filterType );
					Mem_Free( raw );
					return;
			}
			row[i] = (byte)( val & 0xff );
		}
	}

	//
	// expand to 32 bit RGBA, matching LoadTGA's byte order (R,G,B,A)
	//
	byte *out = (byte *)R_StaticAlloc( imgWidth * imgHeight * 4, TAG_IMAGE );

	for ( int y = 0; y < imgHeight; y++ ) {
		const byte *src = raw + y * stride + 1;
		byte *dst = out + y * imgWidth * 4;

		for ( int x = 0; x < imgWidth; x++ ) {
			const byte *s = src + x * channels;
			switch ( colorType ) {
				case 0:	// grayscale
					dst[0] = s[0];
					dst[1] = s[0];
					dst[2] = s[0];
					dst[3] = 255;
					break;
				case 2:	// RGB
					dst[0] = s[0];
					dst[1] = s[1];
					dst[2] = s[2];
					dst[3] = 255;
					break;
				case 3: {	// palette
					int idx = s[0];
					if ( idx >= paletteSize ) {
						idx = 0;
					}
					dst[0] = palette[idx * 3 + 0];
					dst[1] = palette[idx * 3 + 1];
					dst[2] = palette[idx * 3 + 2];
					dst[3] = ( haveTrns && idx < trnsSize ) ? trns[idx] : 255;
					break;
				}
				case 4:	// grayscale + alpha
					dst[0] = s[0];
					dst[1] = s[0];
					dst[2] = s[0];
					dst[3] = s[1];
					break;
				case 6:	// RGBA
					dst[0] = s[0];
					dst[1] = s[1];
					dst[2] = s[2];
					dst[3] = s[3];
					break;
			}
			dst += 4;
		}
	}

	Mem_Free( raw );

	*pic = out;
	if ( width ) {
		*width = imgWidth;
	}
	if ( height ) {
		*height = imgHeight;
	}
}

//===================================================================

/*
=================
R_LoadImage

Loads any of the supported image types into a cannonical
32 bit format.

Automatically attempts to load .jpg files if .tga files fail to load.

*pic will be NULL if the load failed.

Anything that is going to make this into a texture would use
makePowerOf2 = true, but something loading an image as a lookup
table of some sort would leave it in identity form.

It is important to do this at image load time instead of texture load
time for bump maps.

Timestamp may be NULL if the value is going to be ignored

If pic is NULL, the image won't actually be loaded, it will just find the
timestamp.
=================
*/
void R_LoadImage( const char *cname, byte **pic, int *width, int *height, ID_TIME_T *timestamp, bool makePowerOf2 ) {
	idStr name = cname;

	if ( pic ) {
		*pic = NULL;
	}
	if ( timestamp ) {
		*timestamp = FILE_NOT_FOUND_TIMESTAMP;
	}
	if ( width ) {
		*width = 0;
	}
	if ( height ) {
		*height = 0;
	}

	name.DefaultFileExtension( ".tga" );

	if (name.Length()<5) {
		return;
	}

	name.ToLower();
	idStr ext;
	name.ExtractFileExtension( ext );

	if ( ext == "tga" ) {
		LoadTGA( name.c_str(), pic, width, height, timestamp );            // try tga first
		if ( ( pic && *pic == 0 ) || ( timestamp && *timestamp == -1 ) ) { //-V595
			name.StripFileExtension();
			name.DefaultFileExtension( ".jpg" );
			LoadJPG( name.c_str(), pic, width, height, timestamp );
		}
		if ( ( pic && *pic == 0 ) || ( timestamp && *timestamp == -1 ) ) { //-V595
			name.StripFileExtension();
			name.DefaultFileExtension( ".png" );
			LoadPNG( name.c_str(), pic, width, height, timestamp );
		}
	} else if ( ext == "jpg" ) {
		LoadJPG( name.c_str(), pic, width, height, timestamp );
	} else if ( ext == "png" ) {
		LoadPNG( name.c_str(), pic, width, height, timestamp );
	}

	if ( ( width && *width < 1 ) || ( height && *height < 1 ) ) {
		if ( pic && *pic ) {
			R_StaticFree( *pic );
			*pic = 0;
		}
	}

	//
	// convert to exact power of 2 sizes
	//
	/*
	if ( pic && *pic && makePowerOf2 ) {
		int		w, h;
		int		scaled_width, scaled_height;
		byte	*resampledBuffer;

		w = *width;
		h = *height;

		for (scaled_width = 1 ; scaled_width < w ; scaled_width<<=1)
			;
		for (scaled_height = 1 ; scaled_height < h ; scaled_height<<=1)
			;

		if ( scaled_width != w || scaled_height != h ) {
			resampledBuffer = R_ResampleTexture( *pic, w, h, scaled_width, scaled_height );
			R_StaticFree( *pic );
			*pic = resampledBuffer;
			*width = scaled_width;
			*height = scaled_height;
		}
	}
	*/
}


/*
=======================
R_LoadCubeImages

Loads six files with proper extensions
=======================
*/
bool R_LoadCubeImages( const char *imgName, cubeFiles_t extensions, byte *pics[6], int *outSize, ID_TIME_T *timestamp ) {
	int		i, j;
	char	*cameraSides[6] =  { "_forward.tga", "_back.tga", "_left.tga", "_right.tga", 
		"_up.tga", "_down.tga" };
	char	*axisSides[6] =  { "_px.tga", "_nx.tga", "_py.tga", "_ny.tga", 
		"_pz.tga", "_nz.tga" };
	char	**sides;
	char	fullName[MAX_IMAGE_NAME];
	int		width, height, size = 0;

	if ( extensions == CF_CAMERA ) {
		sides = cameraSides;
	} else {
		sides = axisSides;
	}

	// FIXME: precompressed cube map files
	if ( pics ) {
		memset( pics, 0, 6*sizeof(pics[0]) );
	}
	if ( timestamp ) {
		*timestamp = 0;
	}

	for ( i = 0 ; i < 6 ; i++ ) {
		idStr::snPrintf( fullName, sizeof( fullName ), "%s%s", imgName, sides[i] );

		ID_TIME_T thisTime;
		if ( !pics ) {
			// just checking timestamps
			R_LoadImageProgram( fullName, NULL, &width, &height, &thisTime );
		} else {
			R_LoadImageProgram( fullName, &pics[i], &width, &height, &thisTime );
		}
		if ( thisTime == FILE_NOT_FOUND_TIMESTAMP ) {
			break;
		}
		if ( i == 0 ) {
			size = width;
		}
		if ( width != size || height != size ) {
			common->Warning( "Mismatched sizes on cube map '%s'", imgName );
			break;
		}
		if ( timestamp ) {
			if ( thisTime > *timestamp ) {
				*timestamp = thisTime;
			}
		}
		if ( pics && extensions == CF_CAMERA ) {
			// convert from "camera" images to native cube map images
			switch( i ) {
			case 0:	// forward
				R_RotatePic( pics[i], width);
				break;
			case 1:	// back
				R_RotatePic( pics[i], width);
				R_HorizontalFlip( pics[i], width, height );
				R_VerticalFlip( pics[i], width, height );
				break;
			case 2:	// left
				R_VerticalFlip( pics[i], width, height );
				break;
			case 3:	// right
				R_HorizontalFlip( pics[i], width, height );
				break;
			case 4:	// up
				R_RotatePic( pics[i], width);
				break;
			case 5: // down
				R_RotatePic( pics[i], width);
				break;
			}
		}
	}

	if ( i != 6 ) {
		// we had an error, so free everything
		if ( pics ) {
			for ( j = 0 ; j < i ; j++ ) {
				R_StaticFree( pics[j] );
			}
		}

		if ( timestamp ) {
			*timestamp = 0;
		}
		return false;
	}

	if ( outSize ) {
		*outSize = size;
	}
	return true;
}
