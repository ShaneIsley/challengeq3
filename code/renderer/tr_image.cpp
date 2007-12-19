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
// tr_image.c
#include "tr_local.h"

/*
 * Include file for users of JPEG library.
 * You will need to have included system headers that define at least
 * the typedefs FILE and size_t before you can include jpeglib.h.
 * (stdio.h is sufficient on ANSI-conforming systems.)
 * You may also wish to include "jerror.h".
 */

extern "C"
{
	#define JPEG_INTERNALS
	#include "../jpeg-6/jpeglib.h"

	void* jpeg_get_small( j_common_ptr cinfo, size_t sizeofobject ) { return (void*)ri.Malloc(sizeofobject); }
	void jpeg_free_small( j_common_ptr cinfo, void* object, size_t sizeofobject ) { ri.Free(object); }
	void* jpeg_get_large( j_common_ptr cinfo, size_t sizeofobject ) { return jpeg_get_small( cinfo, sizeofobject ); }
	void jpeg_free_large( j_common_ptr cinfo, void* object, size_t sizeofobject ) { return jpeg_free_small( cinfo, object, sizeofobject ); }

	void error_exit( j_common_ptr cinfo )
	{
		char buffer[JMSG_LENGTH_MAX];
		(*cinfo->err->format_message)(cinfo, buffer);
		jpeg_destroy(cinfo);
		ri.Error( ERR_FATAL, "%s\n", buffer );
	}

	void output_message( j_common_ptr cinfo )
	{
		char buffer[JMSG_LENGTH_MAX];
		(*cinfo->err->format_message)(cinfo, buffer);
		ri.Printf(PRINT_ALL, "%s\n", buffer);
	}
};


// hash a filename as a case- and OS- insensitive string with no extension

long R_Hash( const char* s, int size )
{
	char ch;
	long hash = 0;

	for (int i = 0; s[i]; ++i) {
		ch = tolower(s[i]);
		if (ch == '.')
			break;			// don't include extension
		if (ch =='\\')
			ch = '/';		// damn path names
		hash += (long)ch * (i+119);
	}

	return (hash & (size-1));
}


#define IMAGE_HASH_SIZE 1024
static image_t* hashTable[IMAGE_HASH_SIZE];


static byte s_intensitytable[256];
static byte s_gammatable[256];

void R_GammaCorrect( byte* buffer, int bufSize )
{
	for (int i = 0; i < bufSize; ++i) {
		buffer[i] = s_gammatable[buffer[i]];
	}
}


static int gl_filter_min = GL_LINEAR_MIPMAP_NEAREST;
static int gl_filter_max = GL_LINEAR;

typedef struct {
	const char* name;
	int minimize, maximize;
} textureMode_t;

static const textureMode_t modes[] = {
	{ "GL_NEAREST", GL_NEAREST, GL_NEAREST },
	{ "GL_LINEAR", GL_LINEAR, GL_LINEAR },
	{ "GL_NEAREST_MIPMAP_NEAREST", GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST },
	{ "GL_LINEAR_MIPMAP_NEAREST", GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR },
	{ "GL_NEAREST_MIPMAP_LINEAR", GL_NEAREST_MIPMAP_LINEAR, GL_NEAREST },
	{ "GL_LINEAR_MIPMAP_LINEAR", GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR },
	{ 0 }
};

void GL_TextureMode( const char* string )
{
	int i;

	for (i = 0; modes[i].name; ++i) {
		if ( !Q_stricmp( modes[i].name, string ) ) {
			break;
		}
	}

	if (!modes[i].name) {
		ri.Printf (PRINT_ALL, "bad filter name\n");
		return;
	}

	gl_filter_min = modes[i].minimize;
	gl_filter_max = modes[i].maximize;

	// change all the existing mipmap texture objects
	for ( i = 0 ; i < tr.numImages ; i++ ) {
		const image_t* glt = tr.images[ i ];
		if ( glt->mipmap ) {
			GL_Bind( glt );
			qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter_min );
			qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter_max );
		}
	}
}


int R_SumOfUsedImages()
{
	int total = 0;

	for (int i = 0; i < tr.numImages; ++i) {
		if ( tr.images[i]->frameUsed == tr.frameCount ) {
			total += tr.images[i]->uploadWidth * tr.images[i]->uploadHeight;
		}
	}

	return total;
}


void R_ImageList_f( void )
{
	static const char* yesno[] = { "no ", "yes" };

	int i, texels = 0;
	const image_t* image;

	ri.Printf (PRINT_ALL, "\n      wide high mip TMU -fmt- wrap --name--\n");

	for ( i = 0 ; i < tr.numImages ; i++ ) {
		image = tr.images[ i ];

		texels += image->uploadWidth*image->uploadHeight;
		ri.Printf (PRINT_ALL, "%4i: %4i %4i %s  %d  ",
			i, image->uploadWidth, image->uploadHeight, yesno[image->mipmap], image->TMU );

		switch ( image->internalFormat ) {
		case 1:
			ri.Printf( PRINT_ALL, "I     " );
			break;
		case 2:
			ri.Printf( PRINT_ALL, "IA    " );
			break;
		case 3:
			ri.Printf( PRINT_ALL, "RGB   " );
			break;
		case 4:
			ri.Printf( PRINT_ALL, "RGBA  " );
			break;
		case GL_RGBA8:
			ri.Printf( PRINT_ALL, "RGBA8 " );
			break;
		case GL_RGB8:
			ri.Printf( PRINT_ALL, "RGB8  " );
			break;
		case GL_RGB4_S3TC:
			ri.Printf( PRINT_ALL, "S3TC  " );
			break;
		case GL_RGBA4:
			ri.Printf( PRINT_ALL, "RGBA4 " );
			break;
		case GL_RGB5:
			ri.Printf( PRINT_ALL, "RGB5  " );
			break;
		default:
			ri.Printf( PRINT_ALL, "%5i ", image->internalFormat );
		}

		switch ( image->wrapClampMode ) {
		case GL_REPEAT:
			ri.Printf( PRINT_ALL, "rept " );
			break;
		case GL_CLAMP:
			ri.Printf( PRINT_ALL, "clmp " );
			break;
		case GL_CLAMP_TO_EDGE:
			ri.Printf( PRINT_ALL, "edge " );
			break;
		default:
			ri.Printf( PRINT_ALL, "%4i ", image->wrapClampMode );
			break;
		}

		ri.Printf( PRINT_ALL, "%s\n", image->imgName );
	}

	ri.Printf (PRINT_ALL, " ---------\n");
	ri.Printf (PRINT_ALL, " %i total texels (not including mipmaps)\n", texels);
	ri.Printf (PRINT_ALL, " %i total images\n\n", tr.numImages );
}

//=======================================================================

/*
================
Used to resample images in a more general than quartering fashion.

This will only be filtered properly if the resampled size
is greater than half the original size.

If a larger shrinking is needed, use the mipmap function 
before or after.
================
*/
static void ResampleTexture( unsigned *in, int inwidth, int inheight, unsigned *out,
							int outwidth, int outheight ) {
	int		i, j;
	unsigned	*inrow, *inrow2;
	unsigned	frac, fracstep;
	unsigned	p1[2048], p2[2048];
	byte		*pix1, *pix2, *pix3, *pix4;

	if (outwidth>2048)
		ri.Error(ERR_DROP, "ResampleTexture: max width");

	fracstep = inwidth*0x10000/outwidth;

	frac = fracstep>>2;
	for ( i=0 ; i<outwidth ; i++ ) {
		p1[i] = 4*(frac>>16);
		frac += fracstep;
	}
	frac = 3*(fracstep>>2);
	for ( i=0 ; i<outwidth ; i++ ) {
		p2[i] = 4*(frac>>16);
		frac += fracstep;
	}

	for (i=0 ; i<outheight ; i++, out += outwidth) {
		inrow = in + inwidth*(int)((i+0.25)*inheight/outheight);
		inrow2 = in + inwidth*(int)((i+0.75)*inheight/outheight);
		frac = fracstep >> 1;
		for (j=0 ; j<outwidth ; j++) {
			pix1 = (byte *)inrow + p1[j];
			pix2 = (byte *)inrow + p2[j];
			pix3 = (byte *)inrow2 + p1[j];
			pix4 = (byte *)inrow2 + p2[j];
			((byte *)(out+j))[0] = (pix1[0] + pix2[0] + pix3[0] + pix4[0])>>2;
			((byte *)(out+j))[1] = (pix1[1] + pix2[1] + pix3[1] + pix4[1])>>2;
			((byte *)(out+j))[2] = (pix1[2] + pix2[2] + pix3[2] + pix4[2])>>2;
			((byte *)(out+j))[3] = (pix1[3] + pix2[3] + pix3[3] + pix4[3])>>2;
		}
	}
}


// scale up the pixel values in a texture to increase the lighting range

static void R_LightScaleTexture( unsigned* in, int width, int height, qbool only_gamma )
{
	byte* p = (byte*)in;
	int i, c = width * height;

	if ( only_gamma )
	{
		if ( !glConfig.deviceSupportsGamma )
		{
			for (i=0 ; i<c ; i++, p+=4)
			{
				p[0] = s_gammatable[p[0]];
				p[1] = s_gammatable[p[1]];
				p[2] = s_gammatable[p[2]];
			}
		}
	}
	else
	{
		if ( glConfig.deviceSupportsGamma )
		{
			for (i=0 ; i<c ; i++, p+=4)
			{
				p[0] = s_intensitytable[p[0]];
				p[1] = s_intensitytable[p[1]];
				p[2] = s_intensitytable[p[2]];
			}
		}
		else
		{
			for (i=0 ; i<c ; i++, p+=4)
			{
				p[0] = s_gammatable[s_intensitytable[p[0]]];
				p[1] = s_gammatable[s_intensitytable[p[1]]];
				p[2] = s_gammatable[s_intensitytable[p[2]]];
			}
		}
	}
}


/*
================
R_MipMap2

Operates in place, quartering the size of the texture
Proper linear filter
================
*/
static void R_MipMap2( unsigned *in, int inWidth, int inHeight )
{
	int			i, j, k;
	byte		*outpix;
	int			inWidthMask, inHeightMask;
	int			total;

	int outWidth = inWidth >> 1;
	int outHeight = inHeight >> 1;
	unsigned* temp = (unsigned*)ri.Hunk_AllocateTempMemory( outWidth * outHeight * 4 );

	inWidthMask = inWidth - 1;
	inHeightMask = inHeight - 1;

	for ( i = 0 ; i < outHeight ; i++ ) {
		for ( j = 0 ; j < outWidth ; j++ ) {
			outpix = (byte *) ( temp + i * outWidth + j );
			for ( k = 0 ; k < 4 ; k++ ) {
				total = 
					1 * ((byte *)&in[ ((i*2-1)&inHeightMask)*inWidth + ((j*2-1)&inWidthMask) ])[k] +
					2 * ((byte *)&in[ ((i*2-1)&inHeightMask)*inWidth + ((j*2)&inWidthMask) ])[k] +
					2 * ((byte *)&in[ ((i*2-1)&inHeightMask)*inWidth + ((j*2+1)&inWidthMask) ])[k] +
					1 * ((byte *)&in[ ((i*2-1)&inHeightMask)*inWidth + ((j*2+2)&inWidthMask) ])[k] +

					2 * ((byte *)&in[ ((i*2)&inHeightMask)*inWidth + ((j*2-1)&inWidthMask) ])[k] +
					4 * ((byte *)&in[ ((i*2)&inHeightMask)*inWidth + ((j*2)&inWidthMask) ])[k] +
					4 * ((byte *)&in[ ((i*2)&inHeightMask)*inWidth + ((j*2+1)&inWidthMask) ])[k] +
					2 * ((byte *)&in[ ((i*2)&inHeightMask)*inWidth + ((j*2+2)&inWidthMask) ])[k] +

					2 * ((byte *)&in[ ((i*2+1)&inHeightMask)*inWidth + ((j*2-1)&inWidthMask) ])[k] +
					4 * ((byte *)&in[ ((i*2+1)&inHeightMask)*inWidth + ((j*2)&inWidthMask) ])[k] +
					4 * ((byte *)&in[ ((i*2+1)&inHeightMask)*inWidth + ((j*2+1)&inWidthMask) ])[k] +
					2 * ((byte *)&in[ ((i*2+1)&inHeightMask)*inWidth + ((j*2+2)&inWidthMask) ])[k] +

					1 * ((byte *)&in[ ((i*2+2)&inHeightMask)*inWidth + ((j*2-1)&inWidthMask) ])[k] +
					2 * ((byte *)&in[ ((i*2+2)&inHeightMask)*inWidth + ((j*2)&inWidthMask) ])[k] +
					2 * ((byte *)&in[ ((i*2+2)&inHeightMask)*inWidth + ((j*2+1)&inWidthMask) ])[k] +
					1 * ((byte *)&in[ ((i*2+2)&inHeightMask)*inWidth + ((j*2+2)&inWidthMask) ])[k];
				outpix[k] = total / 36;
			}
		}
	}

	Com_Memcpy( in, temp, outWidth * outHeight * 4 );
	ri.Hunk_FreeTempMemory( temp );
}


/* KHB 060708  bleh - dark as hell unless we call GammaCorrect, and still marginal even then  :(
static qbool R_MipMapSGI( byte* tex, int w, int h, GLenum internalFormat )
{
	if (!fEXT_GL_SGIS_generate_mipmap || r_simpleMipMaps->integer)
		return qfalse;

	R_GammaCorrect( tex, w * h * 4 );
	qglHint( GL_GENERATE_MIPMAP_HINT_SGIS, GL_NICEST );
	qglTexParameteri( GL_TEXTURE_2D, GL_GENERATE_MIPMAP_SGIS, GL_TRUE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	qglTexImage2D( GL_TEXTURE_2D, 0, internalFormat, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex );
	qglTexParameteri( GL_TEXTURE_2D, GL_GENERATE_MIPMAP_SGIS, GL_FALSE );

	return qtrue;
}
*/


// operates in place, quartering the size of the texture

static void R_MipMap( byte* in, int width, int height)
{
	int		i, j;

	if ( !r_simpleMipMaps->integer ) {
		R_MipMap2( (unsigned *)in, width, height );
		return;
	}

	if ( width == 1 && height == 1 ) {
		return;
	}

	int row = width * 4;
	byte* out = in;
	width >>= 1;
	height >>= 1;

	if ( width == 0 || height == 0 ) {
		width += height;	// get largest
		for (i=0 ; i<width ; i++, out+=4, in+=8 ) {
			out[0] = ( in[0] + in[4] )>>1;
			out[1] = ( in[1] + in[5] )>>1;
			out[2] = ( in[2] + in[6] )>>1;
			out[3] = ( in[3] + in[7] )>>1;
		}
		return;
	}

	for (i=0 ; i<height ; i++, in+=row) {
		for (j=0 ; j<width ; j++, out+=4, in+=8) {
			out[0] = (in[0] + in[4] + in[row+0] + in[row+4])>>2;
			out[1] = (in[1] + in[5] + in[row+1] + in[row+5])>>2;
			out[2] = (in[2] + in[6] + in[row+2] + in[row+6])>>2;
			out[3] = (in[3] + in[7] + in[row+3] + in[row+7])>>2;
		}
	}
}


// apply a color blend over a set of pixels - used for mipmaps

static void R_BlendOverTexture( byte *data, int pixelCount, const byte blend[4] )
{
	int premult[3];
	int inverseAlpha = 255 - blend[3];

	premult[0] = blend[0] * blend[3];
	premult[1] = blend[1] * blend[3];
	premult[2] = blend[2] * blend[3];

	for (int i = 0; i < pixelCount; ++i, data+=4) {
		data[0] = ( data[0] * inverseAlpha + premult[0] ) >> 9;
		data[1] = ( data[1] * inverseAlpha + premult[1] ) >> 9;
		data[2] = ( data[2] * inverseAlpha + premult[2] ) >> 9;
	}
}

static const byte mipBlendColors[16][4] = {
	{0,0,0,0},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
	{255,0,0,128},
	{0,255,0,128},
	{0,0,255,128},
};


// note that the "32" here is for the image's STRIDE - it has nothing to do with the actual COMPONENTS

static void Upload32( unsigned int* data,
							int width, int height,
							qbool mipmap,
							qbool picmip,
							GLenum* format,
							int *pUploadWidth, int *pUploadHeight )
{
	int scaled_width, scaled_height;

	// convert to exact power of 2 sizes
	//
	for (scaled_width = 1 ; scaled_width < width ; scaled_width<<=1)
		;
	for (scaled_height = 1 ; scaled_height < height ; scaled_height<<=1)
		;
	if ( r_roundImagesDown->integer && scaled_width > width )
		scaled_width >>= 1;
	if ( r_roundImagesDown->integer && scaled_height > height )
		scaled_height >>= 1;

	RI_AutoPtr pResampled;
	if ( scaled_width != width || scaled_height != height ) {
		pResampled.Alloc( scaled_width * scaled_height * 4 );
		ResampleTexture( data, width, height, pResampled.Get<unsigned int>(), scaled_width, scaled_height );
		data = pResampled.Get<unsigned int>();
		width = scaled_width;
		height = scaled_height;
	}

	//
	// perform optional picmip operation
	//
	if ( picmip ) {
		scaled_width >>= r_picmip->integer;
		scaled_height >>= r_picmip->integer;
	}

	//
	// clamp to minimum size
	//
	if (scaled_width < 1) {
		scaled_width = 1;
	}
	if (scaled_height < 1) {
		scaled_height = 1;
	}

	//
	// clamp to the current upper OpenGL limit
	// scale both axis down equally so we don't have to
	// deal with a half mip resampling
	//
	while ( scaled_width > glConfig.maxTextureSize
		|| scaled_height > glConfig.maxTextureSize ) {
		scaled_width >>= 1;
		scaled_height >>= 1;
	}

	// select proper internal format
	GLenum internalFormat = GL_RGB;
	switch (*format) {
	case GL_RGB:
		if ( glConfig.textureCompression == TC_S3TC )
		{
			internalFormat = GL_RGB4_S3TC;
		}
		else if ( r_texturebits->integer == 16 )
		{
			internalFormat = GL_RGB5;
		}
		else if ( r_texturebits->integer == 32 )
		{
			internalFormat = GL_RGB8;
		}
		else
		{
			internalFormat = 3;
		}
		break;

	case GL_RGBA:
		if ( r_texturebits->integer == 16 )
		{
			internalFormat = GL_RGBA4;
		}
		else if ( r_texturebits->integer == 32 )
		{
			internalFormat = GL_RGBA8;
		}
		else
		{
			internalFormat = 4;
		}
		break;

	default:
		ri.Error( ERR_DROP, "Upload32: Invalid format %d\n", *format );
	}

	RI_AutoPtr pScaled( sizeof(unsigned) * scaled_width * scaled_height );
	// copy or resample data as appropriate for first MIP level
	if ( ( scaled_width == width ) && ( scaled_height == height ) ) {
		if (!mipmap)
		{
			qglTexImage2D( GL_TEXTURE_2D, 0, internalFormat, scaled_width, scaled_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data );
			*pUploadWidth = scaled_width;
			*pUploadHeight = scaled_height;
			*format = internalFormat;
			goto done;
		}
		Com_Memcpy( pScaled, data, width * height * 4 );
	}
	/* KHB  if the card can take care of mipmaps for us, we can skip all this  :)
	if (R_MipMapSGI( (byte *)data, width, height, internalFormat )) {
		goto done;
	}*/
	else
	{
		// use the normal mip-mapping function to go down from here
		while ( width > scaled_width || height > scaled_height ) {
			R_MipMap( (byte*)data, width, height );
			width = max( width >> 1, 1 );
			height = max( height >> 1, 1 );
		}
		Com_Memcpy( pScaled, data, width * height * 4 );
	}

	R_LightScaleTexture( pScaled.Get<unsigned int>(), scaled_width, scaled_height, !mipmap );

	*pUploadWidth = scaled_width;
	*pUploadHeight = scaled_height;
	*format = internalFormat;

	qglTexImage2D( GL_TEXTURE_2D, 0, internalFormat, scaled_width, scaled_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pScaled );

	if (mipmap)
	{
		int miplevel = 0;
		while (scaled_width > 1 || scaled_height > 1)
		{
			R_MipMap( pScaled, scaled_width, scaled_height );
			scaled_width = max( scaled_width >> 1, 1 );
			scaled_height = max( scaled_height >> 1, 1 );
			++miplevel;

			if ( r_colorMipLevels->integer )
				R_BlendOverTexture( pScaled, scaled_width * scaled_height, mipBlendColors[miplevel] );

			qglTexImage2D( GL_TEXTURE_2D, miplevel, internalFormat, scaled_width, scaled_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pScaled );
		}
	}

done:

	if (mipmap)
	{
		if ( textureFilterAnisotropic )
			qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
					(GLint)Com_Clamp( 1, maxAnisotropy, r_ext_max_anisotropy->integer ) );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter_min );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter_max );
	}
	else
	{
		if ( textureFilterAnisotropic )
			qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1 );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	}

	GL_CheckErrors();
}


// this is the only way any image_t are created
// !!! i'm pretty sure this DOESN'T work correctly for non-POT images

image_t* R_CreateImage( const char* name, byte* pic, int width, int height, GLenum format,
						qbool mipmap, qbool allowPicmip, int glWrapClampMode )
{
	if (strlen(name) >= MAX_QPATH)
		ri.Error(ERR_DROP, "R_CreateImage: \"%s\" is too long\n", name);

	if ( tr.numImages == MAX_DRAWIMAGES )
		ri.Error( ERR_DROP, "R_CreateImage: MAX_DRAWIMAGES hit\n");

	image_t* image = tr.images[tr.numImages] = RI_New<image_t>();
	image->texnum = 1024 + tr.numImages;
	tr.numImages++;

	image->internalFormat = format;
	image->mipmap = mipmap;
	image->allowPicmip = allowPicmip;

	strcpy( image->imgName, name );

	image->width = width;
	image->height = height;
	image->wrapClampMode = glWrapClampMode;

	qbool isLightmap = !strncmp( name, "*lightmap", 9 );
	// lightmaps are always allocated on TMU 1
	image->TMU = (qglActiveTextureARB && isLightmap) ? 1 : 0;

	if ( qglActiveTextureARB )
		GL_SelectTexture( image->TMU );

	GL_Bind(image);

	Upload32( (unsigned int*)pic, image->width, image->height,
								image->mipmap,
								allowPicmip,
								&image->internalFormat,
								&image->uploadWidth,
								&image->uploadHeight );

	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrapClampMode );
	qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrapClampMode );

	qglBindTexture( GL_TEXTURE_2D, 0 );

	if (image->TMU == 1)
		GL_SelectTexture( 0 );

	// KHB  there are times we have no interest in naming an image at all (notably, font glyphs)
	// but atm the rest of the system is too dependent on everything being named
	//if (name) {
		long hash = R_Hash(name, IMAGE_HASH_SIZE);
		image->next = hashTable[hash];
		hashTable[hash] = image;
	//}

	return image;
}


/*
BMP LOADING - DO NOT WANT!

typedef struct
{
	char id[2];
	unsigned long fileSize;
	unsigned long reserved0;
	unsigned long bitmapDataOffset;
	unsigned long bitmapHeaderSize;
	unsigned long width;
	unsigned long height;
	unsigned short planes;
	unsigned short bitsPerPixel;
	unsigned long compression;
	unsigned long bitmapDataSize;
	unsigned long hRes;
	unsigned long vRes;
	unsigned long colors;
	unsigned long importantColors;
	unsigned char palette[256][4];
} BMPHeader_t;

static void LoadBMP( const char *name, byte **pic, int *width, int *height )
{
	int		columns, rows;
	unsigned	numPixels;
	byte	*pixbuf;
	int		row, column;
	byte	*buf_p;
	byte	*buffer;
	int		length;
	BMPHeader_t bmpHeader;
	byte		*bmpRGBA;

	*pic = NULL;

	//
	// load the file
	//
	length = ri.FS_ReadFile( ( char * ) name, (void **)&buffer);
	if (!buffer) {
		return;
	}

	buf_p = buffer;

	bmpHeader.id[0] = *buf_p++;
	bmpHeader.id[1] = *buf_p++;
	bmpHeader.fileSize = LittleLong( * ( long * ) buf_p );
	buf_p += 4;
	bmpHeader.reserved0 = LittleLong( * ( long * ) buf_p );
	buf_p += 4;
	bmpHeader.bitmapDataOffset = LittleLong( * ( long * ) buf_p );
	buf_p += 4;
	bmpHeader.bitmapHeaderSize = LittleLong( * ( long * ) buf_p );
	buf_p += 4;
	bmpHeader.width = LittleLong( * ( long * ) buf_p );
	buf_p += 4;
	bmpHeader.height = LittleLong( * ( long * ) buf_p );
	buf_p += 4;
	bmpHeader.planes = LittleShort( * ( short * ) buf_p );
	buf_p += 2;
	bmpHeader.bitsPerPixel = LittleShort( * ( short * ) buf_p );
	buf_p += 2;
	bmpHeader.compression = LittleLong( * ( long * ) buf_p );
	buf_p += 4;
	bmpHeader.bitmapDataSize = LittleLong( * ( long * ) buf_p );
	buf_p += 4;
	bmpHeader.hRes = LittleLong( * ( long * ) buf_p );
	buf_p += 4;
	bmpHeader.vRes = LittleLong( * ( long * ) buf_p );
	buf_p += 4;
	bmpHeader.colors = LittleLong( * ( long * ) buf_p );
	buf_p += 4;
	bmpHeader.importantColors = LittleLong( * ( long * ) buf_p );
	buf_p += 4;

	Com_Memcpy( bmpHeader.palette, buf_p, sizeof( bmpHeader.palette ) );

	if ( bmpHeader.bitsPerPixel == 8 )
		buf_p += 1024;

	if ( bmpHeader.id[0] != 'B' && bmpHeader.id[1] != 'M' ) 
	{
		ri.Error( ERR_DROP, "LoadBMP: only Windows-style BMP files supported (%s)\n", name );
	}
	if ( bmpHeader.fileSize != length )
	{
		ri.Error( ERR_DROP, "LoadBMP: header size does not match file size (%d vs. %d) (%s)\n", bmpHeader.fileSize, length, name );
	}
	if ( bmpHeader.compression != 0 )
	{
		ri.Error( ERR_DROP, "LoadBMP: only uncompressed BMP files supported (%s)\n", name );
	}
	if ( bmpHeader.bitsPerPixel < 8 )
	{
		ri.Error( ERR_DROP, "LoadBMP: monochrome and 4-bit BMP files not supported (%s)\n", name );
	}

	columns = bmpHeader.width;
	rows = bmpHeader.height;
	if ( rows < 0 )
		rows = -rows;
	numPixels = columns * rows;

	if(columns <= 0 || !rows || numPixels > 0x1FFFFFFF // 4*1FFFFFFF == 0x7FFFFFFC < 0x7FFFFFFF
	    || ((numPixels * 4) / columns) / 4 != rows)
	{
	  ri.Error (ERR_DROP, "LoadBMP: %s has an invalid image size\n", name);
	}

	if ( width ) 
		*width = columns;
	if ( height )
		*height = rows;

	bmpRGBA = ri.Malloc( numPixels * 4 );
	*pic = bmpRGBA;


	for ( row = rows-1; row >= 0; row-- )
	{
		pixbuf = bmpRGBA + row*columns*4;

		for ( column = 0; column < columns; column++ )
		{
			unsigned char red, green, blue, alpha;
			int palIndex;
			unsigned short shortPixel;

			switch ( bmpHeader.bitsPerPixel )
			{
			case 8:
				palIndex = *buf_p++;
				*pixbuf++ = bmpHeader.palette[palIndex][2];
				*pixbuf++ = bmpHeader.palette[palIndex][1];
				*pixbuf++ = bmpHeader.palette[palIndex][0];
				*pixbuf++ = 0xff;
				break;
			case 16:
				shortPixel = * ( unsigned short * ) pixbuf;
				pixbuf += 2;
				*pixbuf++ = ( shortPixel & ( 31 << 10 ) ) >> 7;
				*pixbuf++ = ( shortPixel & ( 31 << 5 ) ) >> 2;
				*pixbuf++ = ( shortPixel & ( 31 ) ) << 3;
				*pixbuf++ = 0xff;
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
				alpha = *buf_p++;
				*pixbuf++ = red;
				*pixbuf++ = green;
				*pixbuf++ = blue;
				*pixbuf++ = alpha;
				break;
			default:
				ri.Error( ERR_DROP, "LoadBMP: illegal pixel_size '%d' in file '%s'\n", bmpHeader.bitsPerPixel, name );
				break;
			}
		}
	}

	ri.FS_FreeFile( buffer );
}


static void LoadPCX ( const char *filename, byte **pic, byte **palette, int *width, int *height)
{
	byte	*raw;
	pcx_t	*pcx;
	int		x, y;
	int		len;
	int		dataByte, runLength;
	byte	*out, *pix;
	unsigned		xmax, ymax;

	*pic = NULL;
	*palette = NULL;

	//
	// load the file
	//
	len = ri.FS_ReadFile( ( char * ) filename, (void **)&raw);
	if (!raw) {
		return;
	}

	//
	// parse the PCX file
	//
	pcx = (pcx_t *)raw;
	raw = &pcx->data;

  	xmax = LittleShort(pcx->xmax);
    ymax = LittleShort(pcx->ymax);

	if (pcx->manufacturer != 0x0a
		|| pcx->version != 5
		|| pcx->encoding != 1
		|| pcx->bits_per_pixel != 8
		|| xmax >= 1024
		|| ymax >= 1024)
	{
		ri.Printf (PRINT_ALL, "Bad pcx file %s (%i x %i) (%i x %i)\n", filename, xmax+1, ymax+1, pcx->xmax, pcx->ymax);
		return;
	}

	out = ri.Malloc ( (ymax+1) * (xmax+1) );

	*pic = out;

	pix = out;

	if (palette)
	{
		*palette = ri.Malloc(768);
		Com_Memcpy (*palette, (byte *)pcx + len - 768, 768);
	}

	if (width)
		*width = xmax+1;
	if (height)
		*height = ymax+1;
// FIXME: use bytes_per_line here?

	for (y=0 ; y<=ymax ; y++, pix += xmax+1)
	{
		for (x=0 ; x<=xmax ; )
		{
			dataByte = *raw++;

			if((dataByte & 0xC0) == 0xC0)
			{
				runLength = dataByte & 0x3F;
				dataByte = *raw++;
			}
			else
				runLength = 1;

			while(runLength-- > 0)
				pix[x++] = dataByte;
		}

	}

	if ( raw - (byte *)pcx > len)
	{
		ri.Printf (PRINT_DEVELOPER, "PCX file %s was malformed", filename);
		ri.Free (*pic);
		*pic = NULL;
	}

	ri.FS_FreeFile (pcx);
}


static void LoadPCX32 ( const char *filename, byte **pic, int *width, int *height) {
	byte	*palette;
	byte	*pic8;
	int		i, c, p;
	byte	*pic32;

	LoadPCX (filename, &pic8, &palette, width, height);
	if (!pic8) {
		*pic = NULL;
		return;
	}

	// LoadPCX32 ensures width, height < 1024
	c = (*width) * (*height);
	pic32 = *pic = ri.Malloc(4 * c );
	for (i = 0 ; i < c ; i++) {
		p = pic8[i];
		pic32[0] = palette[p*3];
		pic32[1] = palette[p*3 + 1];
		pic32[2] = palette[p*3 + 2];
		pic32[3] = 255;
		pic32 += 4;
	}

	ri.Free (pic8);
	ri.Free (palette);
}

*/


static qbool LoadTGA( const char* name, byte** pic, int* w, int* h, GLenum* format )
{
	*pic = NULL;

	byte* buffer;
	ri.FS_ReadFile( name, (void**)&buffer );
	if (!buffer)
		return qfalse;

	byte* p = buffer;

	TargaHeader targa_header;
	targa_header.id_length = p[0];
	targa_header.colormap_type = p[1];
	targa_header.image_type = p[2];
	targa_header.width = LittleShort( *(short*)&p[12] );
	targa_header.height = LittleShort( *(short*)&p[14] );
	targa_header.pixel_size = p[16];
	targa_header.attributes = p[17];

	p += sizeof(TargaHeader);

	//if ((targa_header.image_type != 2) && (targa_header.image_type != 10) && (targa_header.image_type != 3))
	//	ri.Error(ERR_DROP, "LoadTGA: Only type 2 (RGB), 3 (gray), and 10 (RGB) TGA images supported\n");
	if ((targa_header.image_type != 2) && (targa_header.image_type != 10))
		ri.Error( ERR_DROP, "LoadTGA %s: Only type 2 and 10 (RGB/A) TGA images supported\n", name );

	if ( targa_header.colormap_type )
		ri.Error( ERR_DROP, "LoadTGA %s: Colormaps not supported\n", name );

	//if ( ( targa_header.pixel_size != 32 && targa_header.pixel_size != 24 ) && targa_header.image_type != 3 )
	if ( targa_header.pixel_size != 32 && targa_header.pixel_size != 24 )
		ri.Error( ERR_DROP, "LoadTGA %s: Only 32 or 24 bit images supported\n", name );

	*format = (targa_header.pixel_size == 32) ? GL_RGBA : GL_RGB;

	int columns = targa_header.width;
	int rows = targa_header.height;
	int numPixels = columns * rows * 4;

	if (w)
		*w = columns;
	if (h)
		*h= rows;

	if (!columns || !rows || numPixels > 0x7FFFFFFF || numPixels / columns / 4 != rows)
		ri.Error(ERR_DROP, "LoadTGA: %s has an invalid image size\n", name);

	*pic = (byte*)ri.Malloc(numPixels);
	byte* dst;

	if (targa_header.id_length != 0)
		p += targa_header.id_length;  // skip TARGA image comment

	byte pixel[4] = { 0, 0, 0, 255 };
	int bpp = (targa_header.pixel_size >> 3);

	// one of these days we'll actually just use GL_BGRA_EXT, but until then...
	#define UNMUNGE_TGA_PIXEL { *dst++ = pixel[2]; *dst++ = pixel[1]; *dst++ = pixel[0]; *dst++ = pixel[3]; }

	// uncompressed BGR(A)
	if (targa_header.image_type == 2) {
		for (int y = rows-1; y >= 0; --y) {
			dst = *pic + y*columns*4;
			for (int x = 0; x < columns; ++x) {
				for (int i = 0; i < bpp; ++i)
					pixel[i] = *p++;
				UNMUNGE_TGA_PIXEL;
			}
		}
	}

	#define WRAP_TGA if ((++x == columns) && y--) { x = 0; dst = *pic + y*columns*4; }

	// RLE BGR(A)
	if (targa_header.image_type == 10) {
		int y = rows-1;
		while (y >= 0) {
			dst = *pic + y*columns*4;
			int x = 0;
			while (x < columns) {
				int rle = *p++;
				int n = 1 + (rle & 0x7F);
				if (rle & 0x80) { // RLE packet, 1 pixel n times
					for (int i = 0; i < bpp; ++i)
						pixel[i] = *p++;
					while (n--) {
						UNMUNGE_TGA_PIXEL;
						WRAP_TGA;
					}
				}
				else while (n--) { // n distinct pixels
					for (int i = 0; i < bpp; ++i)
						pixel[i] = *p++;
					UNMUNGE_TGA_PIXEL;
					WRAP_TGA;
				}
			}
		}
	}

	#undef WRAP_TGA

	#undef UNMUNGE_TGA_PIXEL

#if 0 
  // TTimo: this is the chunk of code to ensure a behavior that meets TGA specs 
  // bk0101024 - fix from Leonardo
  // bit 5 set => top-down
  if (targa_header.attributes & 0x20) {
    unsigned char *flip = (unsigned char*)malloc (columns*4);
    unsigned char *src, *dst;

    for (row = 0; row < rows/2; row++) {
      src = targa_rgba + row * 4 * columns;
      dst = targa_rgba + (rows - row - 1) * 4 * columns;

      memcpy (flip, src, columns*4);
      memcpy (src, dst, columns*4);
      memcpy (dst, flip, columns*4);
    }
    free (flip);
  }
  // instead we just print a warning
#endif

	if (targa_header.attributes & 0x20)
		ri.Printf( PRINT_WARNING, "WARNING: '%s' TGA file header declares top-down image, ignoring\n", name);

	ri.FS_FreeFile( buffer );
	return qtrue;
}


static qbool LoadJPG( const char *filename, unsigned char **pic, int *width, int *height )
{
  /* This struct contains the JPEG decompression parameters and pointers to
   * working space (which is allocated as needed by the JPEG library).
   */
  struct jpeg_decompress_struct cinfo = {NULL};
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
  unsigned row_stride;		/* physical row width in output buffer */
  unsigned pixelcount, memcount;
  byte	*fbuffer;
  byte  *buf;

  /* In this example we want to open the input file before doing anything else,
   * so that the setjmp() error recovery below can assume the file is open.
   * VERY IMPORTANT: use "b" option to fopen() if you are on a machine that
   * requires it in order to read binary files.
   */

  ri.FS_ReadFile ( ( char * ) filename, (void **)&fbuffer);
  if (!fbuffer) {
	return qfalse;
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

  (void) jpeg_read_header(&cinfo, TRUE);
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

  pixelcount = cinfo.output_width * cinfo.output_height;

  if(!cinfo.output_width || !cinfo.output_height
      || ((pixelcount * 4) / cinfo.output_width) / 4 != cinfo.output_height
      || pixelcount > 0x1FFFFFFF || cinfo.output_components > 4) // 4*1FFFFFFF == 0x7FFFFFFC < 0x7FFFFFFF
  {
    ri.Error (ERR_DROP, "LoadJPG: %s has an invalid image size: %dx%d*4=%d, components: %d\n", filename,
		    cinfo.output_width, cinfo.output_height, pixelcount * 4, cinfo.output_components);
  }

  memcount = pixelcount * 4;
  row_stride = cinfo.output_width * cinfo.output_components;

  byte* out = (byte*)ri.Malloc(memcount);

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
	buf = ((out+(row_stride*cinfo.output_scanline)));
	buffer = &buf;
    (void) jpeg_read_scanlines(&cinfo, buffer, 1);
  }
  
  buf = out;

  // If we are processing an 8-bit JPEG (greyscale), we'll have to convert
  // the greyscale values to RGBA.
  if(cinfo.output_components == 1)
  {
  	int sindex = pixelcount, dindex = memcount;
	unsigned char greyshade;

	// Only pixelcount number of bytes have been written.
	// Expand the color values over the rest of the buffer, starting
	// from the end.
	do
	{
		greyshade = buf[--sindex];

		buf[--dindex] = 255;
		buf[--dindex] = greyshade;
		buf[--dindex] = greyshade;
		buf[--dindex] = greyshade;
	} while(sindex);
  }
  else
  {
	// clear all the alphas to 255
	int	i;

	for ( i = 3 ; i < memcount ; i+=4 )
	{
		buf[i] = 255;
	}
  }

  *pic = out;

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
  ri.FS_FreeFile (fbuffer);

  /* At this point you may want to check to see whether any corrupt-data
   * warnings occurred (test whether jerr.pub.num_warnings is nonzero).
   */

  /* And we're done! */
  return qtrue;
}


/* Expanded data destination object for stdio output */

typedef struct {
  struct jpeg_destination_mgr pub; /* public fields */

  byte* outfile;		/* target stream */
  int	size;
} my_destination_mgr;

typedef my_destination_mgr * my_dest_ptr;


/*
 * Initialize destination --- called by jpeg_start_compress
 * before any data is actually written.
 */

void init_destination (j_compress_ptr cinfo)
{
  my_dest_ptr dest = (my_dest_ptr) cinfo->dest;

  dest->pub.next_output_byte = dest->outfile;
  dest->pub.free_in_buffer = dest->size;
}


/*
 * Empty the output buffer --- called whenever buffer fills up.
 *
 * In typical applications, this should write the entire output buffer
 * (ignoring the current state of next_output_byte & free_in_buffer),
 * reset the pointer & count to the start of the buffer, and return TRUE
 * indicating that the buffer has been dumped.
 *
 * In applications that need to be able to suspend compression due to output
 * overrun, a FALSE return indicates that the buffer cannot be emptied now.
 * In this situation, the compressor will return to its caller (possibly with
 * an indication that it has not accepted all the supplied scanlines).  The
 * application should resume compression after it has made more room in the
 * output buffer.  Note that there are substantial restrictions on the use of
 * suspension --- see the documentation.
 *
 * When suspending, the compressor will back up to a convenient restart point
 * (typically the start of the current MCU). next_output_byte & free_in_buffer
 * indicate where the restart point will be if the current call returns FALSE.
 * Data beyond this point will be regenerated after resumption, so do not
 * write it out when emptying the buffer externally.
 */

boolean empty_output_buffer (j_compress_ptr cinfo)
{
  return TRUE;
}


/*
 * Compression initialization.
 * Before calling this, all parameters and a data destination must be set up.
 *
 * We require a write_all_tables parameter as a failsafe check when writing
 * multiple datastreams from the same compression object.  Since prior runs
 * will have left all the tables marked sent_table=TRUE, a subsequent run
 * would emit an abbreviated stream (no tables) by default.  This may be what
 * is wanted, but for safety's sake it should not be the default behavior:
 * programmers should have to make a deliberate choice to emit abbreviated
 * images.  Therefore the documentation and examples should encourage people
 * to pass write_all_tables=TRUE; then it will take active thought to do the
 * wrong thing.
 */

GLOBAL void
jpeg_start_compress (j_compress_ptr cinfo, boolean write_all_tables)
{
  if (cinfo->global_state != CSTATE_START)
    ERREXIT1(cinfo, JERR_BAD_STATE, cinfo->global_state);

  if (write_all_tables)
    jpeg_suppress_tables(cinfo, FALSE);	/* mark all tables to be written */

  /* (Re)initialize error mgr and destination modules */
  (*cinfo->err->reset_error_mgr) ((j_common_ptr) cinfo);
  (*cinfo->dest->init_destination) (cinfo);
  /* Perform master selection of active modules */
  jinit_compress_master(cinfo);
  /* Set up for the first pass */
  (*cinfo->master->prepare_for_pass) (cinfo);
  /* Ready for application to drive first pass through jpeg_write_scanlines
   * or jpeg_write_raw_data.
   */
  cinfo->next_scanline = 0;
  cinfo->global_state = (cinfo->raw_data_in ? CSTATE_RAW_OK : CSTATE_SCANNING);
}


/*
 * Write some scanlines of data to the JPEG compressor.
 *
 * The return value will be the number of lines actually written.
 * This should be less than the supplied num_lines only in case that
 * the data destination module has requested suspension of the compressor,
 * or if more than image_height scanlines are passed in.
 *
 * Note: we warn about excess calls to jpeg_write_scanlines() since
 * this likely signals an application programmer error.  However,
 * excess scanlines passed in the last valid call are *silently* ignored,
 * so that the application need not adjust num_lines for end-of-image
 * when using a multiple-scanline buffer.
 */

GLOBAL JDIMENSION
jpeg_write_scanlines (j_compress_ptr cinfo, JSAMPARRAY scanlines,
		      JDIMENSION num_lines)
{
  JDIMENSION row_ctr, rows_left;

  if (cinfo->global_state != CSTATE_SCANNING)
    ERREXIT1(cinfo, JERR_BAD_STATE, cinfo->global_state);
  if (cinfo->next_scanline >= cinfo->image_height)
    WARNMS(cinfo, JWRN_TOO_MUCH_DATA);

  /* Call progress monitor hook if present */
  if (cinfo->progress != NULL) {
    cinfo->progress->pass_counter = (long) cinfo->next_scanline;
    cinfo->progress->pass_limit = (long) cinfo->image_height;
    (*cinfo->progress->progress_monitor) ((j_common_ptr) cinfo);
  }

  /* Give master control module another chance if this is first call to
   * jpeg_write_scanlines.  This lets output of the frame/scan headers be
   * delayed so that application can write COM, etc, markers between
   * jpeg_start_compress and jpeg_write_scanlines.
   */
  if (cinfo->master->call_pass_startup)
    (*cinfo->master->pass_startup) (cinfo);

  /* Ignore any extra scanlines at bottom of image. */
  rows_left = cinfo->image_height - cinfo->next_scanline;
  if (num_lines > rows_left)
    num_lines = rows_left;

  row_ctr = 0;
  (*cinfo->main->process_data) (cinfo, scanlines, &row_ctr, num_lines);
  cinfo->next_scanline += row_ctr;
  return row_ctr;
}

/*
 * Terminate destination --- called by jpeg_finish_compress
 * after all data has been written.  Usually needs to flush buffer.
 *
 * NB: *not* called by jpeg_abort or jpeg_destroy; surrounding
 * application must deal with any cleanup that should happen even
 * for error exit.
 */

static int hackSize;

void term_destination (j_compress_ptr cinfo)
{
  my_dest_ptr dest = (my_dest_ptr) cinfo->dest;
  size_t datacount = dest->size - dest->pub.free_in_buffer;
  hackSize = datacount;
}


/*
 * Prepare for output to a stdio stream.
 * The caller must have already opened the stream, and is responsible
 * for closing it after finishing compression.
 */

void jpegDest (j_compress_ptr cinfo, byte* outfile, int size)
{
  my_dest_ptr dest;

  /* The destination object is made permanent so that multiple JPEG images
   * can be written to the same file without re-executing jpeg_stdio_dest.
   * This makes it dangerous to use this manager and a different destination
   * manager serially with the same JPEG object, because their private object
   * sizes may be different.  Caveat programmer.
   */
  if (cinfo->dest == NULL) {	/* first time for this JPEG object? */
    cinfo->dest = (struct jpeg_destination_mgr *)
      (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
				  sizeof(my_destination_mgr));
  }

  dest = (my_dest_ptr) cinfo->dest;
  dest->pub.init_destination = init_destination;
  dest->pub.empty_output_buffer = empty_output_buffer;
  dest->pub.term_destination = term_destination;
  dest->outfile = outfile;
  dest->size = size;
}

void SaveJPG( const char* filename, int quality, int image_width, int image_height, unsigned char *image_buffer )
{
  /* This struct contains the JPEG compression parameters and pointers to
   * working space (which is allocated as needed by the JPEG library).
   * It is possible to have several such structures, representing multiple
   * compression/decompression processes, in existence at once.  We refer
   * to any one struct (and its associated working data) as a "JPEG object".
   */
  struct jpeg_compress_struct cinfo;
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
  JSAMPROW row_pointer[1];	/* pointer to JSAMPLE row[s] */
  int row_stride;		/* physical row width in image buffer */

  /* Step 1: allocate and initialize JPEG compression object */

  /* We have to set up the error handler first, in case the initialization
   * step fails.  (Unlikely, but it could happen if you are out of memory.)
   * This routine fills in the contents of struct jerr, and returns jerr's
   * address which we place into the link field in cinfo.
   */
  cinfo.err = jpeg_std_error(&jerr);
  /* Now we can initialize the JPEG compression object. */
  jpeg_create_compress(&cinfo);

  /* Step 2: specify data destination (eg, a file) */
  /* Note: steps 2 and 3 can be done in either order. */

  /* Here we use the library-supplied code to send compressed data to a
   * stdio stream.  You can also write your own code to do something else.
   * VERY IMPORTANT: use "b" option to fopen() if you are on a machine that
   * requires it in order to write binary files.
   */
  byte* out = (byte*)ri.Hunk_AllocateTempMemory(image_width*image_height*4);
  jpegDest(&cinfo, out, image_width*image_height*4);

  /* Step 3: set parameters for compression */

  /* First we supply a description of the input image.
   * Four fields of the cinfo struct must be filled in:
   */
  cinfo.image_width = image_width; 	/* image width and height, in pixels */
  cinfo.image_height = image_height;
  cinfo.input_components = 4;		/* # of color components per pixel */
  cinfo.in_color_space = JCS_RGB; 	/* colorspace of input image */
  /* Now use the library's routine to set default compression parameters.
   * (You must set at least cinfo.in_color_space before calling this,
   * since the defaults depend on the source color space.)
   */
  jpeg_set_defaults(&cinfo);
  /* Now you can set any non-default parameters you wish to.
   * Here we just illustrate the use of quality (quantization table) scaling:
   */
  jpeg_set_quality(&cinfo, quality, TRUE /* limit to baseline-JPEG values */);

  /* Step 4: Start compressor */

  /* TRUE ensures that we will write a complete interchange-JPEG file.
   * Pass TRUE unless you are very sure of what you're doing.
   */
  jpeg_start_compress(&cinfo, TRUE);

  /* Step 5: while (scan lines remain to be written) */
  /*           jpeg_write_scanlines(...); */

  /* Here we use the library's state variable cinfo.next_scanline as the
   * loop counter, so that we don't have to keep track ourselves.
   * To keep things simple, we pass one scanline per call; you can pass
   * more if you wish, though.
   */
  row_stride = image_width * 4;	/* JSAMPLEs per row in image_buffer */

  while (cinfo.next_scanline < cinfo.image_height) {
    /* jpeg_write_scanlines expects an array of pointers to scanlines.
     * Here the array is only one element long, but you could pass
     * more than one scanline at a time if that's more convenient.
     */
    row_pointer[0] = & image_buffer[((cinfo.image_height-1)*row_stride)-cinfo.next_scanline * row_stride];
    (void) jpeg_write_scanlines(&cinfo, row_pointer, 1);
  }

  /* Step 6: Finish compression */

  jpeg_finish_compress(&cinfo);
  /* After finish_compress, we can close the output file. */
  ri.FS_WriteFile( filename, out, hackSize );

  ri.Hunk_FreeTempMemory(out);

  /* Step 7: release JPEG compression object */

  /* This is an important step since it will release a good deal of memory. */
  jpeg_destroy_compress(&cinfo);

  /* And we're done! */
}

/*
=================
SaveJPGToBuffer
=================
*/
int SaveJPGToBuffer( byte *buffer, int quality,
    int image_width, int image_height,
    byte *image_buffer )
{
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  JSAMPROW row_pointer[1];	/* pointer to JSAMPLE row[s] */
  int row_stride;		/* physical row width in image buffer */

  /* Step 1: allocate and initialize JPEG compression object */
  cinfo.err = jpeg_std_error(&jerr);
  /* Now we can initialize the JPEG compression object. */
  jpeg_create_compress(&cinfo);

  /* Step 2: specify data destination (eg, a file) */
  /* Note: steps 2 and 3 can be done in either order. */
  jpegDest(&cinfo, buffer, image_width*image_height*4);

  /* Step 3: set parameters for compression */
  cinfo.image_width = image_width; 	/* image width and height, in pixels */
  cinfo.image_height = image_height;
  cinfo.input_components = 4;		/* # of color components per pixel */
  cinfo.in_color_space = JCS_RGB; 	/* colorspace of input image */

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE /* limit to baseline-JPEG values */);

  /* Step 4: Start compressor */
  jpeg_start_compress(&cinfo, TRUE);

  /* Step 5: while (scan lines remain to be written) */
  /*           jpeg_write_scanlines(...); */
  row_stride = image_width * 4;	/* JSAMPLEs per row in image_buffer */

  while (cinfo.next_scanline < cinfo.image_height) {
    /* jpeg_write_scanlines expects an array of pointers to scanlines.
     * Here the array is only one element long, but you could pass
     * more than one scanline at a time if that's more convenient.
     */
    row_pointer[0] = & image_buffer[((cinfo.image_height-1)*row_stride)-cinfo.next_scanline * row_stride];
    (void) jpeg_write_scanlines(&cinfo, row_pointer, 1);
  }

  /* Step 6: Finish compression */
  jpeg_finish_compress(&cinfo);

  /* Step 7: release JPEG compression object */
  jpeg_destroy_compress(&cinfo);

  /* And we're done! */
  return hackSize;
}

//===================================================================


static void R_LoadImage( const char* name, byte** pic, int* w, int* h, GLenum* format )
{
	*pic = NULL;

	*w = 0;
	*h = 0;

	int len = strlen(name);
	if (len < 5) {
		ri.Printf( PRINT_ALL, "ERROR: invalid image name %s\n", name );
		return;
	}

	if (!Q_stricmp( name+len-4, ".tga" ) && LoadTGA( name, pic, w, h, format ))
		return;

	*format = GL_RGB;
#if defined(_DEBUG)
	// either this is REALLY a jpg, or just as likely some moron got the extension wrong
	if (!Q_stricmp( name+len-4, ".jpg" ) && LoadJPG( name, pic, w, h ))
		return;

	ri.Printf( PRINT_DEVELOPER, "WARNING: idiot has misnamed %s\n", name );
#endif

	char altname[MAX_QPATH];
	strcpy( altname, name );
	len = strlen( altname );
	altname[len-3] = 'j';
	altname[len-2] = 'p';
	altname[len-1] = 'g';
	LoadJPG( altname, pic, w, h );

	return;
/*
	}else if ( !Q_stricmp(name+len-4, ".pcx") ) {
		LoadPCX32( name, pic, width, height );
	} else if ( !Q_stricmp( name+len-4, ".bmp" ) ) {
		LoadBMP( name, pic, width, height );
	} else if ( !Q_stricmp( name+len-4, ".jpg" ) ) {
		LoadJPG( name, pic, width, height );
	}
*/
}


// finds or loads the given image - returns NULL if it fails, not a default image

image_t* R_FindImageFile( const char* name, qbool mipmap, qbool allowPicmip, int glWrapClampMode )
{
	if (!name)
		return NULL;

	long hash = R_Hash(name, IMAGE_HASH_SIZE);

	// see if the image is already loaded
	//
	image_t* image;
	for (image=hashTable[hash]; image; image=image->next) {
		if ( !strcmp( name, image->imgName ) ) {
			// the white image can be used with any set of parms, but other mismatches are errors
			if ( strcmp( name, "*white" ) ) {
				if ( image->mipmap != mipmap ) {
					ri.Printf( PRINT_DEVELOPER, "WARNING: reused image %s with mixed mipmap parm\n", name );
				}
				if ( image->allowPicmip != allowPicmip ) {
					ri.Printf( PRINT_DEVELOPER, "WARNING: reused image %s with mixed allowPicmip parm\n", name );
				}
				if ( image->wrapClampMode != glWrapClampMode ) {
					ri.Printf( PRINT_ALL, "WARNING: reused image %s with mixed glWrapClampMode parm\n", name );
				}
			}
			return image;
		}
	}

	// load the pic from disk
	//
	byte* pic;
	int width, height;
	GLenum format;
	R_LoadImage( name, &pic, &width, &height, &format );

/* no, the quake fs is case-insensitive, which means this case can only trip for developer/impure
which means either they should get their shit RIGHT, or we don't care
	if ( pic == NULL ) {                                    // if we dont get a successful load
	  char altname[MAX_QPATH];                              // copy the name
    int len;                                              //  
    strcpy( altname, name );                              //
    len = strlen( altname );                              // 
    altname[len-3] = toupper(altname[len-3]);             // and try upper case extension for unix systems
    altname[len-2] = toupper(altname[len-2]);             //
    altname[len-1] = toupper(altname[len-1]);             //
		ri.Printf( PRINT_ALL, "trying %s...\n", altname );    // 
	  R_LoadImage( altname, &pic, &width, &height );        //
    if (pic == NULL) {                                    // if that fails
      return NULL;                                        // bail
    }
	}
*/

	if (!pic)
		return NULL;

	image = R_CreateImage( name, pic, width, height, format, mipmap, allowPicmip, glWrapClampMode );
	ri.Free( pic );
	return image;
}


#define DLIGHT_SIZE 64

static void R_CreateDlightImage( void )
{
	int		x, y;
	byte	data[DLIGHT_SIZE][DLIGHT_SIZE][4];

	// KHB 060701  how about we use the RIGHT math for this
	for (x = 0; x < DLIGHT_SIZE; ++x) {
		for (y = 0; y < DLIGHT_SIZE; ++y) {
			float dx = (DLIGHT_SIZE/2 - x + 0.5) / (DLIGHT_SIZE/2);
			float dy = (DLIGHT_SIZE/2 - y + 0.5) / (DLIGHT_SIZE/2);
			float r = cos(M_PI / 2.0 * sqrt(dx * dx + dy * dy));
			byte b = 255 * ((r <= 0) ? 0 : r*r);
			data[y][x][0] = data[y][x][1] = data[y][x][2] = b;
			data[y][x][3] = 255;
		}
	}

	// this is a large enough image now that it should be mipmapped
	tr.dlightImage = R_CreateImage( "*dlight", (byte*)data, DLIGHT_SIZE, DLIGHT_SIZE, GL_RGBA, qtrue, qfalse, GL_CLAMP );
}


void R_InitFogTable()
{
	const float exp = 0.5;

	for (int i = 0; i < FOG_TABLE_SIZE; ++i) {
		tr.fogTable[i] = pow( (float)i/(FOG_TABLE_SIZE-1), exp );
	}
}


/*
Returns a 0.0 to 1.0 fog density value
This is called for each texel of the fog texture on startup
and for each vertex of transparent shaders in fog dynamically
*/
float R_FogFactor( float s, float t )
{
	s -= 1.0/512;
	if ( s < 0 ) {
		return 0;
	}
	if ( t < 1.0/32 ) {
		return 0;
	}
	if ( t < 31.0/32 ) {
		s *= (t - 1.0f/32.0f) / (30.0f/32.0f);
	}

	// we need to leave a lot of clamp range
	s *= 8;

	if ( s > 1.0 ) {
		s = 1.0;
	}

	return tr.fogTable[ (int)(s * (FOG_TABLE_SIZE-1)) ];
}


static void R_CreateFogImage()
{
	const int FOG_S = 256;
	const int FOG_T = 32;

	RI_AutoPtr ap( FOG_S * FOG_T * 4 );
	byte* p = ap;

	// S is distance, T is depth
	for (int x = 0; x < FOG_S; ++x) {
		for (int y = 0; y < FOG_T; ++y) {
			float d = R_FogFactor( ( x + 0.5f ) / FOG_S, ( y + 0.5f ) / FOG_T );
			p[(y*FOG_S+x)*4+0] = p[(y*FOG_S+x)*4+1] = p[(y*FOG_S+x)*4+2] = 255;
			p[(y*FOG_S+x)*4+3] = 255*d;
		}
	}

	tr.fogImage = R_CreateImage( "*fog", p, FOG_S, FOG_T, GL_RGBA, qfalse, qfalse, GL_CLAMP_TO_EDGE );
	qglTexParameterfv( GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, colorWhite );
}


const int DEFAULT_SIZE = 16;


static void R_CreateDefaultImage()
{
	byte data[DEFAULT_SIZE][DEFAULT_SIZE][4];

	// the default image will be a box, to allow you to see the mapping coordinates
	Com_Memset( data, 32, sizeof( data ) );
	for (int i = 0; i < DEFAULT_SIZE; ++i) {
		data[0][i][0] =
		data[0][i][1] =
		data[0][i][2] =
		data[0][i][3] = 255;

		data[i][0][0] =
		data[i][0][1] =
		data[i][0][2] =
		data[i][0][3] = 255;

		data[DEFAULT_SIZE-1][i][0] =
		data[DEFAULT_SIZE-1][i][1] =
		data[DEFAULT_SIZE-1][i][2] =
		data[DEFAULT_SIZE-1][i][3] = 255;

		data[i][DEFAULT_SIZE-1][0] =
		data[i][DEFAULT_SIZE-1][1] =
		data[i][DEFAULT_SIZE-1][2] =
		data[i][DEFAULT_SIZE-1][3] = 255;
	}

	tr.defaultImage = R_CreateImage( "*default", (byte*)data, DEFAULT_SIZE, DEFAULT_SIZE, GL_RGBA, qtrue, qfalse, GL_REPEAT );
}


static void R_CreateBuiltinImages()
{
	int x, y;
	byte data[DEFAULT_SIZE][DEFAULT_SIZE][4];

	R_CreateDefaultImage();

	// we use a solid white image instead of disabling texturing
	Com_Memset( data, 255, sizeof( data ) );
	tr.whiteImage = R_CreateImage( "*white", (byte*)data, 8, 8, GL_RGBA, qfalse, qfalse, GL_REPEAT );

	// with overbright bits active, we need an image which is some fraction of full color,
	// for default lightmaps, etc
	for (x = 0; x < DEFAULT_SIZE; ++x) {
		for (y = 0; y < DEFAULT_SIZE; ++y) {
			data[y][x][0] = 
			data[y][x][1] = 
			data[y][x][2] = tr.identityLightByte;
			data[y][x][3] = 255;
		}
	}

	tr.identityLightImage = R_CreateImage( "*identityLight", (byte*)data, 8, 8, GL_RGBA, qfalse, qfalse, GL_REPEAT );

	// scratchimages usually used for cinematic drawing (signal-quality effects)
	// these are just placeholders: RE_StretchRaw will regenerate them when it wants them
	for (x = 0; x < 32; ++x)
		tr.scratchImage[x] = R_CreateImage( "*scratch", (byte*)data, DEFAULT_SIZE, DEFAULT_SIZE, GL_RGBA, qfalse, qtrue, GL_CLAMP );

	R_CreateDlightImage();
	R_CreateFogImage();
}


void R_SetColorMappings()
{
	// allow 2 overbright bits in 24 bit, but only 1 in 16 bit
	tr.overbrightBits = Com_Clamp( 0, (glConfig.colorBits > 16) ? 2 : 1, r_overBrightBits->integer );

	// setup the overbright lighting - needs hw support and fullscreen
	if (!glConfig.deviceSupportsGamma || !glConfig.isFullscreen)
		tr.overbrightBits = 0;

	tr.identityLight = 1.0f / (1 << tr.overbrightBits);
	tr.identityLightByte = 255 * tr.identityLight;

	if ( r_intensity->value < 1 )
		ri.Cvar_Set( "r_intensity", "1" );

	if ( r_gamma->value < 0.5f )
		ri.Cvar_Set( "r_gamma", "0.5" );
	if ( r_gamma->value > 3.0f )
		ri.Cvar_Set( "r_gamma", "3.0" );

	for (int i = 0; i < 256; ++i) {
		int n = 255 * pow( i / 255.0f, 1.0f / r_gamma->value ) + 0.5f;
		s_gammatable[i] = Com_Clamp( 0, 255, n << tr.overbrightBits );
		s_intensitytable[i] = min( i * r_intensity->value, 255 );
	}

	if (glConfig.deviceSupportsGamma)
		GLimp_SetGamma( s_gammatable, s_gammatable, s_gammatable );
}


void R_InitImages()
{
	Com_Memset( hashTable, 0, sizeof(hashTable) );
	R_SetColorMappings(); // build brightness translation tables
	R_CreateBuiltinImages(); // create default textures (white, dlight, etc)
}


void R_DeleteTextures()
{
	for (int i = 0; i < tr.numImages; ++i)
		qglDeleteTextures( 1, &tr.images[i]->texnum );

	tr.numImages = 0;
	Com_Memset( tr.images, 0, sizeof( tr.images ) );
	Com_Memset( glState.currenttextures, 0, sizeof( glState.currenttextures ) );

	if ( qglBindTexture ) {
		if ( qglActiveTextureARB ) {
			GL_SelectTexture( 1 );
			qglBindTexture( GL_TEXTURE_2D, 0 );
			GL_SelectTexture( 0 );
		}
		qglBindTexture( GL_TEXTURE_2D, 0 );
	}
}


/*
============================================================================

SKINS

============================================================================
*/


// unfortunatly, skin files aren't compatible with our normal parsing rules. oops  :/
#if 0 // meh, skin files are like 3 lines - they don't need all this bs
static char *CommaParse( char **data_p ) {
	int c = 0, len;
	char *data;
	static	char	com_token[MAX_TOKEN_CHARS];

	data = *data_p;
	len = 0;
	com_token[0] = 0;

	// make sure incoming data is valid
	if ( !data ) {
		*data_p = NULL;
		return com_token;
	}

	while ( 1 ) {
		// skip whitespace
		while( (c = *data) <= ' ') {
			if( !c ) {
				break;
			}
			data++;
		}


		c = *data;

		// skip double slash comments
		if ( c == '/' && data[1] == '/' )
		{
			while (*data && *data != '\n')
				data++;
		}
		// skip /* */ comments
		else if ( c=='/' && data[1] == '*' ) 
		{
			while ( *data && ( *data != '*' || data[1] != '/' ) ) 
			{
				data++;
			}
			if ( *data ) 
			{
				data += 2;
			}
		}
		else
		{
			break;
		}
	}

	if ( c == 0 ) {
		return "";
	}

	// handle quoted strings
	if (c == '\"')
	{
		data++;
		while (1)
		{
			c = *data++;
			if (c=='\"' || !c)
			{
				com_token[len] = 0;
				*data_p = ( char * ) data;
				return com_token;
			}
			if (len < MAX_TOKEN_CHARS)
			{
				com_token[len] = c;
				len++;
			}
		}
	}

	// parse a regular word
	do
	{
		if (len < MAX_TOKEN_CHARS)
		{
			com_token[len] = c;
			len++;
		}
		data++;
		c = *data;
	} while (c>32 && c != ',' );

	if (len == MAX_TOKEN_CHARS)
	{
//		Com_Printf ("Token exceeded %i chars, discarded.\n", MAX_TOKEN_CHARS);
		len = 0;
	}
	com_token[len] = 0;

	*data_p = ( char * ) data;
	return com_token;
}
#endif

static const char* CommaParse( const char** data )
{
	static char com_token[MAX_TOKEN_CHARS];

	int c = 0;
	const char* p = *data;

	while (*p && (*p < 32))
		++p;

	while ((*p > 32) && (*p != ',') && (c < MAX_TOKEN_CHARS-1))
		com_token[c++] = *p++;

	*data = p;
	com_token[c] = 0;
	return com_token;
}


qhandle_t RE_RegisterSkin( const char* name )
{
	if (!name || !name[0] || (strlen(name) >= MAX_QPATH))
		ri.Error( ERR_DROP, "RE_RegisterSkin: invalid name [%s]\n", name ? name : "NULL" );

	skin_t* skin;
	qhandle_t hSkin;
	// see if the skin is already loaded
	for (hSkin = 1; hSkin < tr.numSkins; ++hSkin) {
		skin = tr.skins[hSkin];
		if ( !Q_stricmp( skin->name, name ) ) {
			return (skin->numSurfaces ? hSkin : 0);
		}
	}

	// allocate a new skin
	if ( tr.numSkins == MAX_SKINS ) {
		ri.Printf( PRINT_WARNING, "WARNING: RE_RegisterSkin( '%s' ) MAX_SKINS hit\n", name );
		return 0;
	}

	tr.numSkins++;
	skin = RI_New<skin_t>();
	tr.skins[hSkin] = skin;
	Q_strncpyz( skin->name, name, sizeof( skin->name ) );
	skin->numSurfaces = 0;

	// make sure the render thread is stopped
	// KHB why? we're not uploading anything...   R_SyncRenderThread();

	// if not a .skin file, load as a single shader
	if ( strcmp( name + strlen( name ) - 5, ".skin" ) ) {
		skin->numSurfaces = 1;
		skin->surfaces[0] = RI_New<skinSurface_t>();
		skin->surfaces[0]->shader = R_FindShader( name, LIGHTMAP_NONE, qtrue );
		return hSkin;
	}

	char* text;
	// load and parse the skin file
	ri.FS_ReadFile( name, (void **)&text );
	if (!text)
		return 0;

	const char* token;
	const char* p = text;
	char surfName[MAX_QPATH];

	while (p && *p) {
		// get surface name
		token = CommaParse( &p );
		Q_strncpyz( surfName, token, sizeof( surfName ) );

		if ( !token[0] )
			break;

		// lowercase the surface name so skin compares are faster
		Q_strlwr( surfName );

		if (*p == ',') 
			++p;

		if ( strstr( token, "tag_" ) )
			continue;

		// parse the shader name
		token = CommaParse( &p );

		skinSurface_t* surf = skin->surfaces[ skin->numSurfaces ] = RI_New<skinSurface_t>();
		Q_strncpyz( surf->name, surfName, sizeof( surf->name ) );
		surf->shader = R_FindShader( token, LIGHTMAP_NONE, qtrue );
		skin->numSurfaces++;
	}

	ri.FS_FreeFile( text );

	return (skin->numSurfaces ? hSkin : 0); // never let a skin have 0 shaders
}


void R_InitSkins()
{
	tr.numSkins = 1;

	// make the default skin have all default shaders
	tr.skins[0] = RI_New<skin_t>();
	tr.skins[0]->numSurfaces = 1;
	tr.skins[0]->surfaces[0] = RI_New<skinSurface_t>();
	tr.skins[0]->surfaces[0]->shader = tr.defaultShader;
	Q_strncpyz( tr.skins[0]->name, "<default skin>", sizeof( tr.skins[0]->name ) );
}


skin_t* R_GetSkinByHandle( qhandle_t hSkin )
{
	return ((hSkin > 0) && (hSkin < tr.numSkins) ? tr.skins[hSkin] : tr.skins[0]);
}


void R_SkinList_f( void )
{
	ri.Printf (PRINT_ALL, "------------------\n");

	for (int i = 0; i < tr.numSkins; ++i) {
		const skin_t* skin = tr.skins[i];

		ri.Printf( PRINT_ALL, "%3i:%s\n", i, skin->name );
		for (int j = 0; j < skin->numSurfaces; ++j) {
			ri.Printf( PRINT_ALL, "       %s = %s\n",
				skin->surfaces[j]->name, skin->surfaces[j]->shader->name );
		}
	}

	ri.Printf (PRINT_ALL, "------------------\n");
}

