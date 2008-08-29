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
	tr.numFonts = 0;

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


// why the FUCK is this sort of thing not in the FT headers, sigh
#define FLOAT_TO_FTPOS(x)  ((x) * 64.0f)
#define FTPOS_TO_FLOAT(x)  ((x) / 64.0f)


static qbool R_UploadGlyphs( FT_Face& face, fontInfo_t* font, const char* sImage )
{
	int w = 0, i;

	for (i = 0; i < GLYPHS_PER_FONT; ++i) {
		FT_Load_Char( face, i + GLYPH_START, FT_LOAD_DEFAULT );
		FT_Outline_Translate( &face->glyph->outline, -face->glyph->metrics.horiBearingX, -face->size->metrics.descender );
		// TTF hinting is invariably absolute GARBAGE
		// always take the raw, CORRECT width and just handle the spacing ourselves
		font->pitches[i] = FTPOS_TO_FLOAT( face->glyph->metrics.width );
		// SPC will have a 0 width, so we have to use the hinted value for that
		if (!font->pitches[i])
			font->pitches[i] = FTPOS_TO_FLOAT( face->glyph->metrics.horiAdvance );
		if (font->pitches[i] > font->maxpitch)
			font->maxpitch = font->pitches[i];
		w += font->pitches[i] + 1; // pad cells to avoid blerp filter bleeds
	}

	// there are all sorts of "clever" things we could do here to square the texture etc
	// but they're not worth it. if someone wants to load a 200pt font on a TNT then gfl to them  :)
	w = CeilPO2( w );

	FT_Bitmap ftb;
	ftb.pixel_mode = ft_pixel_mode_grays;
	ftb.num_grays = 256;
	ftb.rows = font->vpitch;
	ftb.pitch = ftb.width = w;
	ftb.buffer = (byte*)Z_Malloc( w * ftb.rows );

	float s = 0;
	for (i = 0; i < GLYPHS_PER_FONT; ++i) {
		FT_Load_Char( face, i + GLYPH_START, FT_LOAD_DEFAULT );
		FT_Outline_Translate( &face->glyph->outline,
				-face->glyph->metrics.horiBearingX + FLOAT_TO_FTPOS(s * w),
				-face->size->metrics.descender );
		FT_Outline_Get_Bitmap( ft, &face->glyph->outline, &ftb );
		font->s[i] = s;
		s += (float)(font->pitches[i] + 1) / w;
	}
	font->s[GLYPHS_PER_FONT] = 1.0;

	byte* img = (byte*)Z_Malloc( 4 * w * font->height );

	const byte* src = ftb.buffer;
	for (int y = 0; y < ftb.rows; ++y) {
		byte* dst = img + (4 * y * w);
		for (int x = 0; x < w; ++x) {
			*dst++ = 255;
			*dst++ = 255;
			*dst++ = 255;
			*dst++ = *src++;
		}
	}

	image_t* image = R_CreateImage( sImage, img, w, font->height, GL_RGBA, qfalse, qfalse, GL_CLAMP_TO_EDGE );
	font->shader = RE_RegisterShaderFromImage( sImage, LIGHTMAP_2D, image );

	Z_Free( img );

	Z_Free( ftb.buffer );

	return qtrue;
}


/*
since the original version of this didn't work, its design is hopelessly broken  :(
the behavior of ALL RegisterX calls exposed to the mod also allows use of them as "FindX"
THIS one doesn't, which means the MOD has to screw around maintaining its own list as well
*/
qbool RE_RegisterFont( const char* fontName, int pointSize, fontInfo_t* info )
{
	for (int i = 0; i < tr.numFonts; ++i) {
		const font_t* font = tr.fonts[i];
		if ( (font->pointsize == pointSize) && !Q_stricmp( font->name, fontName ) ) {
			*info = font->info;
			return qtrue;
		}
	}

	if (tr.numFonts == MAX_FONTS)
		ri.Error( ERR_DROP, "RE_RegisterFont: MAX_FONTS hit\n" );

	if (strlen(fontName) >= MAX_QPATH)
		ri.Error( ERR_DROP, "RE_RegisterFont: \"%s\" is too long\n", fontName );

	Com_Memset( info, 0, sizeof(*info) );

	byte* pTTF;
	int len = ri.FS_ReadFile( va("fonts/%s.ttf", fontName), (void**)&pTTF );
	if (!pTTF) {
		ri.Printf( PRINT_WARNING, "RE_RegisterFont: couldn't open fonts/%s.ttf\n", fontName );
		return qfalse;
	}

	FT_Face face;
	FT_Error err = FT_New_Memory_Face( ft, pTTF, len, 0, &face );
	ri.FS_FreeFile( pTTF );

	if (err != FT_Err_Ok) {
		ri.Printf( PRINT_ALL, "RE_RegisterFont: %s(%dpt) Failed (%d)\n", fontName, pointSize, err );
		return qfalse;
	}

	// no, this isn't a typo: we want to precompensate for the screen's aspect ratio
	//FT_Set_Pixel_Sizes( face, pointSize * glConfig.vidHeight / 640, pointSize * glConfig.vidHeight / 480 );
	// except that every damn TTF out there is already stupidly thin  :(
	FT_Set_Pixel_Sizes( face, pointSize * glConfig.vidWidth / 640, pointSize * glConfig.vidHeight / 480 );

	info->vpitch = FTPOS_TO_FLOAT( face->size->metrics.height );
	info->height = CeilPO2( info->vpitch );

	R_UploadGlyphs( face, info, va( "Font-%s-%02d", fontName, pointSize ) );

	FT_Done_Face( face );

	ri.Printf( PRINT_DEVELOPER, "Loaded %s TTF (%dpt)\n", fontName, pointSize );

	font_t* font = RI_New<font_t>();
	tr.fonts[tr.numFonts++] = font;
	strcpy( font->name, fontName );
	font->pointsize = pointSize;
	font->info = *info;

	return qtrue;
}

