/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// this code uses the FreeType library, LKGV 2.3.5, www.freetype.org

#include "tr_local.h"

#include <ft2build.h>
#include <freetype/freetype.h>
#include <freetype/ftoutln.h>


static FT_Library ft = NULL;


void R_InitFreeType()
{
	if (ft)
		ri.Error( ERR_DROP, "R_InitFreeType: Multiple initialization\n" );

	FT_Error err = FT_Init_FreeType(&ft);
	if (err != FT_Err_Ok)
		ri.Error( ERR_DROP, "R_InitFreeType: Failed (%d)\n", err );

	ri.Printf( PRINT_ALL, "R_InitFreeType: OK\n" );
}


void R_DoneFreeType()
{
	if (ft)
		FT_Done_FreeType(ft);
	ft = NULL;
}


static __inline qbool IsPO2( unsigned int x )
{
	return !(x & (x - 1));
}

static __inline unsigned int NextPO2( unsigned int x )
{
	x |= (x >> 1);
	x |= (x >> 2);
	x |= (x >> 4);
	x |= (x >> 8);
	x |= (x >> 16);
	return (++x);
}

static __inline unsigned int CeilPO2( unsigned int x )
{
	return (IsPO2(x) ? x : NextPO2(x));
}


#define GLYPH_TRUNC(x)  ((x) >> 6)


static qbool R_UploadGlyph( FT_Face& face, fontInfo_t* font, int i, const char* s )
{
	FT_Error err;
	err = FT_Load_Char( face, i, FT_LOAD_DEFAULT );
	if (face->glyph->format != ft_glyph_format_outline) {
		ri.Printf( PRINT_ALL, "R_RenderGlyph: Format %d not supported\n", face->glyph->format );
		return qfalse;
	}
	FT_Outline_Embolden( &face->glyph->outline, 32 ); // 25% extra weight (stupid 26.6 numbers)

	FT_Bitmap ftb;
	ftb.pixel_mode = ft_pixel_mode_grays;
	ftb.num_grays = 256;

	FT_Outline_Translate( &face->glyph->outline, -face->glyph->metrics.horiBearingX, -face->size->metrics.descender );
	ftb.rows = font->vpitch;
	ftb.width = GLYPH_TRUNC( face->glyph->metrics.width );
	ftb.pitch = ftb.width;
	// several fonts have pitch<width on some chars, which obviously doesn't work very well...
	font->pitches[i - GLYPH_START] = max( ftb.pitch, GLYPH_TRUNC( face->glyph->metrics.horiAdvance ) );
	if (font->pitches[i - GLYPH_START] > font->maxpitch)
		font->maxpitch = font->pitches[i - GLYPH_START];

	ftb.buffer = (byte*)Z_Malloc( ftb.pitch * ftb.rows );
	FT_Outline_Get_Bitmap( ft, &face->glyph->outline, &ftb );

	int w = CeilPO2( ftb.pitch );
	font->widths[i - GLYPH_START] = w;

	byte* img = (byte*)Z_Malloc( 4 * w * font->height );

	const byte* src = ftb.buffer;
	for (int y = 0; y < ftb.rows; ++y) {
		byte* dst = img + (4 * y * w);
		for (int x = 0; x < ftb.pitch; ++x) {
			*dst++ = 255;
			*dst++ = 255;
			*dst++ = 255;
			*dst++ = *src++;
		}
	}

	image_t* image = R_CreateImage( s, img, w, font->height, qfalse, qfalse, GL_CLAMP );
	font->shaders[i - GLYPH_START] = RE_RegisterShaderFromImage( s, LIGHTMAP_2D, image, qfalse );

	Z_Free( img );

	Z_Free( ftb.buffer );

	return qtrue;
}


void RE_RegisterFont( const char* fontName, int pointSize, fontInfo_t* font )
{
	Com_Memset( font, 0, sizeof(fontInfo_t) );

	byte* pTTF;
	int len = ri.FS_ReadFile( va("fonts/%s.ttf", fontName), (void**)&pTTF );

	FT_Face face;
	FT_Error err = FT_New_Memory_Face( ft, pTTF, len, 0, &face );
	ri.FS_FreeFile( pTTF );

	if (err != FT_Err_Ok) {
		ri.Printf( PRINT_ALL, "RE_RegisterFont: %s(%dpt) Failed (%d)\n", fontName, pointSize, err );
		return;
	}

	// no, this isn't a typo: we want to precompensate for the screen's aspect ratio
	FT_Set_Pixel_Sizes( face, pointSize * glConfig.vidHeight / 640, pointSize * glConfig.vidHeight / 480 );

	font->vpitch = GLYPH_TRUNC( face->size->metrics.height );
	font->height = CeilPO2( font->vpitch );

	for (int i = GLYPH_START; i <= GLYPH_END; ++i) {
		// we don't really WANT this stuff named, but the system can't cope with anon images+shaders. yet.  :P
		const char* s = va( "Font-%s-%02d-%02X", fontName, pointSize, i );
		R_UploadGlyph( face, font, i, s );
	}

	FT_Done_Face( face );

	ri.Printf( PRINT_ALL, "Loaded %s TTF (%dpt)\n", fontName, pointSize );
}

