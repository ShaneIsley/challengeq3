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
/*
** WIN_GLIMP.C
**
** This file contains ALL Win32 specific stuff having to do with the
** OpenGL refresh.  When a port is being made the following functions
** must be implemented by the port:
**
** GLimp_EndFrame
** GLimp_Init
** GLimp_LogComment
** GLimp_Shutdown
**
** Note that the GLW_xxx functions are Windows specific GL-subsystem
** related functions that are relevant ONLY to win_glimp.c
*/
#include <assert.h>
#include "../renderer/tr_local.h"
#include "../qcommon/qcommon.h"
#include "resource.h"
#include "glw_win.h"
#include "wglext.h"
#include "win_local.h"


typedef enum {
	RSERR_OK,

	RSERR_INVALID_FULLSCREEN,
	RSERR_INVALID_MODE,

	RSERR_UNKNOWN
} rserr_t;

#define TRY_PFD_SUCCESS		0
#define TRY_PFD_FAIL_SOFT	1
#define TRY_PFD_FAIL_HARD	2

static void		GLW_InitExtensions();
static rserr_t	GLW_SetMode( int mode, int colorbits, qbool cdsFullscreen );

static qbool s_classRegistered = qfalse;

//
// variable declarations
//
glwstate_t glw_state;


static qbool GLW_StartDriverAndSetMode( int mode, int colorbits, qbool cdsFullscreen )
{
	rserr_t err = GLW_SetMode( r_mode->integer, colorbits, cdsFullscreen );

	switch ( err )
	{
	case RSERR_INVALID_FULLSCREEN:
		ri.Printf( PRINT_ALL, "...WARNING: fullscreen unavailable in this mode\n" );
		return qfalse;
	case RSERR_INVALID_MODE:
		ri.Printf( PRINT_ALL, "...WARNING: could not set the given mode (%d)\n", mode );
		return qfalse;
	}

	return qtrue;
}

/*
** ChoosePFD
**
** Helper function that replaces ChoosePixelFormat.
*/
#define MAX_PFDS 256

static int GLW_ChoosePFD( HDC hDC, PIXELFORMATDESCRIPTOR *pPFD )
{
	PIXELFORMATDESCRIPTOR pfds[MAX_PFDS+1];
	int maxPFD = 0;
	int i;
	int bestMatch = 0;

	ri.Printf( PRINT_DEVELOPER, "...GLW_ChoosePFD( %d, %d, %d )\n", ( int ) pPFD->cColorBits, ( int ) pPFD->cDepthBits, ( int ) pPFD->cStencilBits );

	maxPFD = DescribePixelFormat( hDC, 1, sizeof( PIXELFORMATDESCRIPTOR ), &pfds[0] );
	if ( maxPFD > MAX_PFDS )
	{
		ri.Printf( PRINT_WARNING, "...numPFDs > MAX_PFDS (%d > %d)\n", maxPFD, MAX_PFDS );
		maxPFD = MAX_PFDS;
	}

	ri.Printf( PRINT_DEVELOPER, "...%d PFDs found\n", maxPFD - 1 );

	// grab information
	for ( i = 1; i <= maxPFD; i++ )
	{
		DescribePixelFormat( hDC, i, sizeof( PIXELFORMATDESCRIPTOR ), &pfds[i] );
	}

	// look for a best match
	for ( i = 1; i <= maxPFD; i++ )
	{
		//
		// make sure this has hardware acceleration
		//
		if ( ( pfds[i].dwFlags & PFD_GENERIC_FORMAT ) != 0 ) 
		{
			continue;
		}

		// verify pixel type
		if ( pfds[i].iPixelType != PFD_TYPE_RGBA )
		{
			if ( r_verbose->integer )
			{
				ri.Printf( PRINT_DEVELOPER, "...PFD %d rejected, not RGBA\n", i );
			}
			continue;
		}

		// verify proper flags
		if ( ( ( pfds[i].dwFlags & pPFD->dwFlags ) & pPFD->dwFlags ) != pPFD->dwFlags ) 
		{
			if ( r_verbose->integer )
			{
				ri.Printf( PRINT_DEVELOPER, "...PFD %d rejected, improper flags (%x instead of %x)\n", i, pfds[i].dwFlags, pPFD->dwFlags );
			}
			continue;
		}

		// verify enough bits
		if ( pfds[i].cDepthBits < 15 )
		{
			continue;
		}
		if ( ( pfds[i].cStencilBits < 4 ) && ( pPFD->cStencilBits > 0 ) )
		{
			continue;
		}

		//
		// selection criteria (in order of priority):
		// 
		//  PFD_STEREO
		//  colorBits
		//  depthBits
		//  stencilBits
		//
		if ( bestMatch )
		{
			// check stereo
			if ( ( pfds[i].dwFlags & PFD_STEREO ) && ( !( pfds[bestMatch].dwFlags & PFD_STEREO ) ) && ( pPFD->dwFlags & PFD_STEREO ) )
			{
				bestMatch = i;
				continue;
			}
			
			if ( !( pfds[i].dwFlags & PFD_STEREO ) && ( pfds[bestMatch].dwFlags & PFD_STEREO ) && ( pPFD->dwFlags & PFD_STEREO ) )
			{
				bestMatch = i;
				continue;
			}

			// check color
			if ( pfds[bestMatch].cColorBits != pPFD->cColorBits )
			{
				// prefer perfect match
				if ( pfds[i].cColorBits == pPFD->cColorBits )
				{
					bestMatch = i;
					continue;
				}
				// otherwise if this PFD has more bits than our best, use it
				else if ( pfds[i].cColorBits > pfds[bestMatch].cColorBits )
				{
					bestMatch = i;
					continue;
				}
			}

			// check depth
			if ( pfds[bestMatch].cDepthBits != pPFD->cDepthBits )
			{
				// prefer perfect match
				if ( pfds[i].cDepthBits == pPFD->cDepthBits )
				{
					bestMatch = i;
					continue;
				}
				// otherwise if this PFD has more bits than our best, use it
				else if ( pfds[i].cDepthBits > pfds[bestMatch].cDepthBits )
				{
					bestMatch = i;
					continue;
				}
			}

			// check stencil
			if ( pfds[bestMatch].cStencilBits != pPFD->cStencilBits )
			{
				// prefer perfect match
				if ( pfds[i].cStencilBits == pPFD->cStencilBits )
				{
					bestMatch = i;
					continue;
				}
				// otherwise if this PFD has more bits than our best, use it
				else if ( ( pfds[i].cStencilBits > pfds[bestMatch].cStencilBits ) && 
					 ( pPFD->cStencilBits > 0 ) )
				{
					bestMatch = i;
					continue;
				}
			}
		}
		else
		{
			bestMatch = i;
		}
	}

	if ( !bestMatch )
		return 0;

	if ( ( pfds[bestMatch].dwFlags & PFD_GENERIC_FORMAT ) != 0 )
	{
		ri.Printf( PRINT_ALL, "...no hardware acceleration found\n" );
		return 0;
	}
	else if ( pfds[bestMatch].dwFlags & PFD_GENERIC_ACCELERATED )
	{
		ri.Printf( PRINT_ALL, "...MCD acceleration found\n" );
	}
	else
	{
		ri.Printf( PRINT_ALL, "...hardware acceleration found\n" );
	}

	*pPFD = pfds[bestMatch];

	return bestMatch;
}

/*
** void GLW_CreatePFD
**
** Helper function zeros out then fills in a PFD
*/
static void GLW_CreatePFD( PIXELFORMATDESCRIPTOR *pPFD, int colorbits, int depthbits, int stencilbits, qbool stereo )
{
	PIXELFORMATDESCRIPTOR src = 
	{
		sizeof(PIXELFORMATDESCRIPTOR),	// size of this pfd
		1,								// version number
		PFD_DRAW_TO_WINDOW |			// support window
		PFD_SUPPORT_OPENGL |			// support OpenGL
		PFD_DOUBLEBUFFER,				// double buffered
		PFD_TYPE_RGBA,					// RGBA type
		24,								// 24-bit color depth
		0, 0, 0, 0, 0, 0,				// color bits ignored
		0,								// no alpha buffer
		0,								// shift bit ignored
		0,								// no accumulation buffer
		0, 0, 0, 0, 					// accum bits ignored
		24,								// 24-bit z-buffer	
		8,								// 8-bit stencil buffer
		0,								// no auxiliary buffer
		PFD_MAIN_PLANE,					// main layer
		0,								// reserved
		0, 0, 0							// layer masks ignored
	};

	src.cColorBits = (BYTE)colorbits;
	src.cDepthBits = (BYTE)depthbits;
	src.cStencilBits = (BYTE)stencilbits;

	if ( stereo )
	{
		ri.Printf( PRINT_ALL, "...attempting to use stereo\n" );
		src.dwFlags |= PFD_STEREO;
		glConfig.stereoEnabled = qtrue;
	}
	else
	{
		glConfig.stereoEnabled = qfalse;
	}

	*pPFD = src;
}

/*
** glw_makecontext
*/
static int GLW_MakeContext( PIXELFORMATDESCRIPTOR *pPFD )
{
	int pixelformat;

	//
	// don't putz around with pixelformat if it's already set (e.g. this is a soft
	// reset of the graphics system)
	//
	if ( !glw_state.pixelFormatSet )
	{
		//
		// choose, set, and describe our desired pixel format.  If we're
		// using a minidriver then we need to bypass the GDI functions,
		// otherwise use the GDI functions.
		//
		if (glw_state.nPendingPF)
			pixelformat = glw_state.nPendingPF;
		else
		if ( ( pixelformat = GLW_ChoosePFD( glw_state.hDC, pPFD ) ) == 0 )
		{
			ri.Printf( PRINT_ALL, "...GLW_ChoosePFD failed\n");
			return TRY_PFD_FAIL_SOFT;
		}
		ri.Printf( PRINT_DEVELOPER, "...PIXELFORMAT %d selected\n", pixelformat );

		DescribePixelFormat( glw_state.hDC, pixelformat, sizeof( *pPFD ), pPFD );

		if ( SetPixelFormat( glw_state.hDC, pixelformat, pPFD ) == FALSE )
		{
			ri.Printf (PRINT_ALL, "...SetPixelFormat failed\n", glw_state.hDC );
			return TRY_PFD_FAIL_SOFT;
		}

		glw_state.pixelFormatSet = qtrue;
	}

	//
	// startup the OpenGL subsystem by creating a context and making it current
	//
	if ( !glw_state.hGLRC )
	{
		if ( ( glw_state.hGLRC = qwglCreateContext( glw_state.hDC ) ) == 0 )
		{
			ri.Printf( PRINT_ALL, "...GL context creation failure\n" );
			return TRY_PFD_FAIL_HARD;
		}
		ri.Printf( PRINT_DEVELOPER, "...GL context created\n" );

		if ( !qwglMakeCurrent( glw_state.hDC, glw_state.hGLRC ) )
		{
			qwglDeleteContext( glw_state.hGLRC );
			glw_state.hGLRC = NULL;
			ri.Printf( PRINT_ALL, "...GL context creation currency failure\n" );
			return TRY_PFD_FAIL_HARD;
		}
		ri.Printf( PRINT_DEVELOPER, "...GL context creation made current\n" );
	}

	return TRY_PFD_SUCCESS;
}


/*
** - get a DC if one doesn't exist
** - create an HGLRC if one doesn't exist
*/
static qbool GLW_InitDriver( int colorbits )
{
	int		tpfd;
	int		depthbits, stencilbits;
	static PIXELFORMATDESCRIPTOR pfd;		// save between frames since 'tr' gets cleared

	ri.Printf( PRINT_DEVELOPER, "Initializing OpenGL driver\n" );

	//
	// get a DC for our window if we don't already have one allocated
	//
	if ( glw_state.hDC == NULL )
	{
		if ( ( glw_state.hDC = GetDC( g_wv.hWnd ) ) == NULL )
		{
			ri.Printf( PRINT_ALL, "...Get DC failed\n" );
			return qfalse;
		}
		ri.Printf( PRINT_DEVELOPER, "...Get DC succeeded\n" );
	}

	if ( colorbits == 0 )
	{
		colorbits = glw_state.desktopBPP;
	}

	//
	// implicitly assume Z-buffer depth == desktop color depth
	//
	if ( r_depthbits->integer == 0 ) {
		if ( colorbits > 16 ) {
			depthbits = 24;
		} else {
			depthbits = 16;
		}
	} else {
		depthbits = r_depthbits->integer;
	}

	//
	// do not allow stencil if Z-buffer depth likely won't contain it
	//
	stencilbits = r_stencilbits->integer;
	if ( depthbits < 24 )
	{
		stencilbits = 0;
	}

	//
	// make two attempts to set the PIXELFORMAT
	//

	//
	// first attempt: r_colorbits, depthbits, and r_stencilbits
	//
	if ( !glw_state.pixelFormatSet )
	{
		GLW_CreatePFD( &pfd, colorbits, depthbits, stencilbits, (qbool)r_stereo->integer );
		if ( ( tpfd = GLW_MakeContext( &pfd ) ) != TRY_PFD_SUCCESS )
		{
			if ( tpfd == TRY_PFD_FAIL_HARD )
			{
				ri.Printf( PRINT_WARNING, "...failed hard\n" );
				return qfalse;
			}

			//
			// punt if we've already tried the desktop bit depth and no stencil bits
			//
			if ( ( r_colorbits->integer == glw_state.desktopBPP ) && !stencilbits )
			{
				ReleaseDC( g_wv.hWnd, glw_state.hDC );
				glw_state.hDC = NULL;
				ri.Printf( PRINT_ALL, "...failed to find an appropriate PIXELFORMAT\n" );
				return qfalse;
			}

			//
			// second attempt: desktop's color bits and no stencil
			//
			if ( colorbits > glw_state.desktopBPP )
				colorbits = glw_state.desktopBPP;

			GLW_CreatePFD( &pfd, colorbits, depthbits, 0, (qbool)r_stereo->integer );
			if ( GLW_MakeContext( &pfd ) != TRY_PFD_SUCCESS )
			{
				if ( glw_state.hDC )
				{
					ReleaseDC( g_wv.hWnd, glw_state.hDC );
					glw_state.hDC = NULL;
				}
				ri.Printf( PRINT_ALL, "...failed to find an appropriate PIXELFORMAT\n" );
				return qfalse;
			}
		}

		// report if stereo is desired but unavailable
		//
		if ( r_stereo->integer && !( pfd.dwFlags & PFD_STEREO ) )
		{
			ri.Printf( PRINT_ALL, "...failed to select stereo pixel format\n" );
			glConfig.stereoEnabled = qfalse;
		}
	}

	// store PFD specifics
	//
	glConfig.colorBits = ( int ) pfd.cColorBits;
	glConfig.depthBits = ( int ) pfd.cDepthBits;
	glConfig.stencilBits = ( int ) pfd.cStencilBits;

	return qtrue;
}


// responsible for creating the Win32 window and initializing the OpenGL driver.

static qbool GLW_CreateWindow( int width, int height, int colorbits, qbool cdsFullscreen )
{
	RECT			r;
	cvar_t			*vid_xpos, *vid_ypos;
	int				stylebits;
	int				x, y, w, h;
	int				exstyle;

	//
	// register the window class if necessary
	//
	if ( !s_classRegistered )
	{
		WNDCLASS wc;

		memset( &wc, 0, sizeof( wc ) );

		wc.style         = CS_OWNDC;
		wc.lpfnWndProc   = MainWndProc;
		wc.cbClsExtra    = 0;
		wc.cbWndExtra    = 0;
		wc.hInstance     = g_wv.hInstance;
		wc.hIcon         = LoadIcon( g_wv.hInstance, MAKEINTRESOURCE(IDI_ICON1));
		wc.hCursor       = LoadCursor (NULL,IDC_ARROW);
		wc.hbrBackground = (HBRUSH)COLOR_GRAYTEXT;
		wc.lpszMenuName  = 0;
		wc.lpszClassName = CLIENT_WINDOW_TITLE;

		if ( !RegisterClass( &wc ) )
		{
			ri.Error( ERR_FATAL, "GLW_CreateWindow: could not register window class" );
		}
		s_classRegistered = qtrue;
		ri.Printf( PRINT_DEVELOPER, "...registered window class\n" );
	}

	//
	// create the HWND if one does not already exist
	//
	if ( !g_wv.hWnd )
	{
		//
		// compute width and height
		//
		r.left = 0;
		r.top = 0;
		r.right  = width;
		r.bottom = height;

		if ( cdsFullscreen )
		{
			exstyle = WS_EX_TOPMOST;
			stylebits = WS_POPUP|WS_VISIBLE|WS_SYSMENU;
		}
		else
		{
			exstyle = 0;
			stylebits = WS_OVERLAPPED|WS_BORDER|WS_CAPTION|WS_VISIBLE|WS_SYSMENU;
			AdjustWindowRect (&r, stylebits, FALSE);
		}

		w = r.right - r.left;
		h = r.bottom - r.top;

		if ( cdsFullscreen )
		{
			x = 0;
			y = 0;
		}
		else
		{
			vid_xpos = ri.Cvar_Get ("vid_xpos", "", 0);
			vid_ypos = ri.Cvar_Get ("vid_ypos", "", 0);
			x = vid_xpos->integer;
			y = vid_ypos->integer;

			// adjust window coordinates if necessary 
			// so that the window is completely on screen
			if ( x < 0 )
				x = 0;
			if ( y < 0 )
				y = 0;

			if ( w < glw_state.desktopWidth &&
				 h < glw_state.desktopHeight )
			{
				if ( x + w > glw_state.desktopWidth )
					x = ( glw_state.desktopWidth - w );
				if ( y + h > glw_state.desktopHeight )
					y = ( glw_state.desktopHeight - h );
			}
		}

		g_wv.hWnd = CreateWindowEx (
			 exstyle, 
			 CLIENT_WINDOW_TITLE,
			 CLIENT_WINDOW_TITLE,
			 stylebits,
			 x, y, w, h,
			 NULL,
			 NULL,
			 g_wv.hInstance,
			 NULL);

		if ( !g_wv.hWnd )
		{
			ri.Error (ERR_FATAL, "GLW_CreateWindow() - Couldn't create window");
		}

		ShowWindow( g_wv.hWnd, SW_SHOW );
		UpdateWindow( g_wv.hWnd );
		ri.Printf( PRINT_DEVELOPER, "...created window@%d,%d (%dx%d)\n", x, y, w, h );
	}
	else
	{
		ri.Printf( PRINT_DEVELOPER, "...window already present, CreateWindowEx skipped\n" );
	}

	if ( !GLW_InitDriver( colorbits ) )
	{
		ShowWindow( g_wv.hWnd, SW_HIDE );
		DestroyWindow( g_wv.hWnd );
		g_wv.hWnd = NULL;
		return qfalse;
	}

	SetForegroundWindow( g_wv.hWnd );
	SetFocus( g_wv.hWnd );

	return qtrue;
}

static void PrintCDSError( int value )
{
	switch ( value )
	{
	case DISP_CHANGE_RESTART:
		ri.Printf( PRINT_ALL, "restart required\n" );
		break;
	case DISP_CHANGE_BADPARAM:
		ri.Printf( PRINT_ALL, "bad param\n" );
		break;
	case DISP_CHANGE_BADFLAGS:
		ri.Printf( PRINT_ALL, "bad flags\n" );
		break;
	case DISP_CHANGE_FAILED:
		ri.Printf( PRINT_ALL, "DISP_CHANGE_FAILED\n" );
		break;
	case DISP_CHANGE_BADMODE:
		ri.Printf( PRINT_ALL, "bad mode\n" );
		break;
	case DISP_CHANGE_NOTUPDATED:
		ri.Printf( PRINT_ALL, "not updated\n" );
		break;
	default:
		ri.Printf( PRINT_ALL, "unknown error %d\n", value );
		break;
	}
}


// GL_multisample is SUCH a fucking mess  >:(

static void GLW_AttemptFSAA()
{
	static const float ar[] = { 0, 0 };
	// ignore r_xyzbits vars - FSAA requires 32-bit color, and anyone using it is implicitly on decent HW
	static int anAttributes[] = {
		WGL_DRAW_TO_WINDOW_ARB, GL_TRUE,
		WGL_SUPPORT_OPENGL_ARB, GL_TRUE,
		WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
		WGL_DOUBLE_BUFFER_ARB, GL_TRUE,
		WGL_COLOR_BITS_ARB, 32,
		WGL_ALPHA_BITS_ARB, 0,
		WGL_DEPTH_BITS_ARB, 24,
		WGL_STENCIL_BITS_ARB, 8,
		WGL_SAMPLE_BUFFERS_ARB, GL_TRUE,
		WGL_SAMPLES_ARB, 4,
		0, 0
	};

	qwglChoosePixelFormatARB = (PFNWGLCHOOSEPIXELFORMATARBPROC)qwglGetProcAddress( "wglChoosePixelFormatARB" );
	if (!r_ext_multisample->integer || !qwglChoosePixelFormatARB) {
		glDisable(GL_MULTISAMPLE_ARB);
		return;
	}

	int iPFD;
	UINT cPFD;
	anAttributes[19] = r_ext_multisample->integer;	// !!! UGH
	if (!qwglChoosePixelFormatARB(glw_state.hDC, anAttributes, ar, 1, &iPFD, &cPFD) || !cPFD)
		return;

	// now bounce the ENTIRE fucking subsytem thanks to WGL+ARB stupidity
	// we can't use GLimp_Shutdown() for this, because that does CDS and logfile poking that we don't want
	assert( glw_state.hGLRC && glw_state.hDC && g_wv.hWnd );

	qwglMakeCurrent( glw_state.hDC, NULL );

	if ( glw_state.hGLRC ) {
		qwglDeleteContext( glw_state.hGLRC );
		glw_state.hGLRC = NULL;
	}

	if ( glw_state.hDC ) {
		ReleaseDC( g_wv.hWnd, glw_state.hDC );
		glw_state.hDC = NULL;
	}

	if ( g_wv.hWnd ) {
		DestroyWindow( g_wv.hWnd );
		g_wv.hWnd = NULL;
	}

	ri.Printf( PRINT_ALL, "...enabling FSAA\n" );

	glw_state.nPendingPF = iPFD;
	glw_state.pixelFormatSet = qfalse;
	GLW_CreateWindow( glConfig.vidWidth, glConfig.vidHeight, glConfig.colorBits, glw_state.cdsFullscreen );
	glw_state.nPendingPF = 0;

	glEnable(GL_MULTISAMPLE_ARB);
}


static rserr_t GLW_SetMode( int mode, int colorbits, qbool cdsFullscreen )
{
	const char *win_fs[] = { "W", "FS" };
	int		cdsRet;

	//
	// print out informational messages
	//
	if ( !R_GetModeInfo( &glConfig.vidWidth, &glConfig.vidHeight, &glConfig.windowAspect, mode ) )
	{
		ri.Printf( PRINT_ALL, "...invalid mode %d\n", mode );
		return RSERR_INVALID_MODE;
	}
	ri.Printf( PRINT_DEVELOPER, "...setting mode %d: %d %d %s\n", mode, glConfig.vidWidth, glConfig.vidHeight, win_fs[cdsFullscreen] );

	//
	// check our desktop attributes
	//
	HDC hDC = GetDC( GetDesktopWindow() );
	glw_state.desktopBPP = GetDeviceCaps( hDC, BITSPIXEL );
	glw_state.desktopWidth = GetDeviceCaps( hDC, HORZRES );
	glw_state.desktopHeight = GetDeviceCaps( hDC, VERTRES );
	ReleaseDC( GetDesktopWindow(), hDC );

	// verify desktop bit depth
	if ( glw_state.desktopBPP < 15 || glw_state.desktopBPP == 24 )
	{
		if ( colorbits == 0 || ( !cdsFullscreen && colorbits >= 15 ) )
		{
			if ( MessageBox( NULL,
						"It is highly unlikely that a correct\n"
						"windowed display can be initialized with\n"
						"the current desktop display depth.\n"
						"Select 'OK' to try anyway.\n"
						"Press 'Cancel' if you wish to quit.",
						"Low Desktop Color Depth",
						MB_OKCANCEL | MB_ICONEXCLAMATION ) != IDOK )
			{
				return RSERR_INVALID_MODE;
			}
		}
	}

	DEVMODE dm;
	// do a CDS if needed
	if ( cdsFullscreen )
	{
		memset( &dm, 0, sizeof( dm ) );
		dm.dmSize = sizeof( dm );

		dm.dmPelsWidth  = glConfig.vidWidth;
		dm.dmPelsHeight = glConfig.vidHeight;
		dm.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT;

		if ( r_displayRefresh->integer != 0 )
		{
			dm.dmDisplayFrequency = r_displayRefresh->integer;
			dm.dmFields |= DM_DISPLAYFREQUENCY;
		}

		// try to change color depth if possible
		if ( colorbits != 0 )
		{
			dm.dmBitsPerPel = colorbits;
			dm.dmFields |= DM_BITSPERPEL;
			ri.Printf( PRINT_DEVELOPER, "...using colorsbits of %d\n", colorbits );
		}
		else
		{
			ri.Printf( PRINT_DEVELOPER, "...using desktop display depth of %d\n", glw_state.desktopBPP );
		}

		//
		// if we're already in fullscreen then just create the window
		//
		if ( glw_state.cdsFullscreen )
		{
			ri.Printf( PRINT_DEVELOPER, "...already fullscreen, avoiding redundant CDS\n" );

			if ( !GLW_CreateWindow( glConfig.vidWidth, glConfig.vidHeight, colorbits, qtrue ) )
			{
				ri.Printf( PRINT_ALL, "...restoring display settings\n" );
				ChangeDisplaySettings( 0, 0 );
				return RSERR_INVALID_MODE;
			}
		}
		//
		// need to call CDS
		//
		else
		{
			ri.Printf( PRINT_DEVELOPER, "...calling CDS: " );

			// try setting the exact mode requested, because some drivers don't report
			// the low res modes in EnumDisplaySettings, but still work
			if ( ( cdsRet = ChangeDisplaySettings( &dm, CDS_FULLSCREEN ) ) == DISP_CHANGE_SUCCESSFUL )
			{
				ri.Printf( PRINT_DEVELOPER, "ok\n" );

				if ( !GLW_CreateWindow( glConfig.vidWidth, glConfig.vidHeight, colorbits, qtrue) )
				{
					ri.Printf( PRINT_ALL, "...restoring display settings\n" );
					ChangeDisplaySettings( 0, 0 );
					return RSERR_INVALID_MODE;
				}

				glw_state.cdsFullscreen = qtrue;
			}
			else
			{
				//
				// the exact mode failed, so scan EnumDisplaySettings for the next largest mode
				//
				DEVMODE		devmode;
				int			modeNum;

				ri.Printf( PRINT_ALL, "failed, " );

				PrintCDSError( cdsRet );

				ri.Printf( PRINT_ALL, "...trying next higher resolution:" );

				// we could do a better matching job here...
				for ( modeNum = 0 ; ; modeNum++ ) {
					if ( !EnumDisplaySettings( NULL, modeNum, &devmode ) ) {
						modeNum = -1;
						break;
					}
					if ( devmode.dmPelsWidth >= glConfig.vidWidth
						&& devmode.dmPelsHeight >= glConfig.vidHeight
						&& devmode.dmBitsPerPel >= 15 ) {
						break;
					}
				}

				if ( modeNum != -1 && ( cdsRet = ChangeDisplaySettings( &devmode, CDS_FULLSCREEN ) ) == DISP_CHANGE_SUCCESSFUL )
				{
					ri.Printf( PRINT_ALL, " ok\n" );
					if ( !GLW_CreateWindow( glConfig.vidWidth, glConfig.vidHeight, colorbits, qtrue) )
					{
						ri.Printf( PRINT_ALL, "...restoring display settings\n" );
						ChangeDisplaySettings( 0, 0 );
						return RSERR_INVALID_MODE;
					}
					
					glw_state.cdsFullscreen = qtrue;
				}
				else
				{
					ri.Printf( PRINT_ALL, " failed, " );

					PrintCDSError( cdsRet );

					ri.Printf( PRINT_ALL, "...restoring display settings\n" );
					ChangeDisplaySettings( 0, 0 );

					glw_state.cdsFullscreen = qfalse;
					glConfig.isFullscreen = qfalse;
					if ( !GLW_CreateWindow( glConfig.vidWidth, glConfig.vidHeight, colorbits, qfalse) )
					{
						return RSERR_INVALID_MODE;
					}
					return RSERR_INVALID_FULLSCREEN;
				}
			}
		}
	}
	else
	{
		if ( glw_state.cdsFullscreen )
		{
			ChangeDisplaySettings( 0, 0 );
		}

		glw_state.cdsFullscreen = qfalse;
		if ( !GLW_CreateWindow( glConfig.vidWidth, glConfig.vidHeight, colorbits, qfalse ) )
		{
			return RSERR_INVALID_MODE;
		}
	}

	//
	// success, now check display frequency
	//
	memset( &dm, 0, sizeof( dm ) );
	dm.dmSize = sizeof( dm );
	if ( EnumDisplaySettings( NULL, ENUM_CURRENT_SETTINGS, &dm ) )
	{
		glConfig.displayFrequency = dm.dmDisplayFrequency;
	}

	glConfig.isFullscreen = cdsFullscreen;

	GLW_AttemptFSAA();

	return RSERR_OK;
}


static void GLW_InitExtensions( void )
{
	if ( !r_allowExtensions->integer )
	{
		ri.Printf( PRINT_ALL, "*** IGNORING OPENGL EXTENSIONS ***\n" );
		return;
	}

	ri.Printf( PRINT_DEVELOPER, "Initializing OpenGL extensions\n" );

	// GL_S3_s3tc
	glConfig.textureCompression = TC_NONE;
	if ( strstr( glConfig.extensions_string, "GL_S3_s3tc" ) )
	{
		if ( r_ext_compressed_textures->integer )
		{
			glConfig.textureCompression = TC_S3TC;
			ri.Printf( PRINT_DEVELOPER, "...using GL_S3_s3tc\n" );
		}
		else
		{
			glConfig.textureCompression = TC_NONE;
			ri.Printf( PRINT_DEVELOPER, "...ignoring GL_S3_s3tc\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_DEVELOPER, "...GL_S3_s3tc not found\n" );
	}

	// GL_EXT_texture_env_add
	glConfig.textureEnvAddAvailable = qfalse;
	if ( strstr( glConfig.extensions_string, "EXT_texture_env_add" ) )
	{
		if ( r_ext_texture_env_add->integer )
		{
			glConfig.textureEnvAddAvailable = qtrue;
			ri.Printf( PRINT_DEVELOPER, "...using GL_EXT_texture_env_add\n" );
		}
		else
		{
			glConfig.textureEnvAddAvailable = qfalse;
			ri.Printf( PRINT_DEVELOPER, "...ignoring GL_EXT_texture_env_add\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_DEVELOPER, "...GL_EXT_texture_env_add not found\n" );
	}

	// WGL_EXT_swap_control
	qwglSwapIntervalEXT = ( BOOL (WINAPI *)(int)) qwglGetProcAddress( "wglSwapIntervalEXT" );
	if ( qwglSwapIntervalEXT )
	{
		ri.Printf( PRINT_DEVELOPER, "...using WGL_EXT_swap_control\n" );
		r_swapInterval->modified = qtrue;	// force a set next frame
	}
	else
	{
		ri.Printf( PRINT_DEVELOPER, "...WGL_EXT_swap_control not found\n" );
	}

	// GL_ARB_multitexture
	qglMultiTexCoord2fARB = NULL;
	qglActiveTextureARB = NULL;
	qglClientActiveTextureARB = NULL;
	if ( strstr( glConfig.extensions_string, "GL_ARB_multitexture" )  )
	{
		if ( r_ext_multitexture->integer )
		{
			qglMultiTexCoord2fARB = ( PFNGLMULTITEXCOORD2FARBPROC ) qwglGetProcAddress( "glMultiTexCoord2fARB" );
			qglActiveTextureARB = ( PFNGLACTIVETEXTUREARBPROC ) qwglGetProcAddress( "glActiveTextureARB" );
			qglClientActiveTextureARB = ( PFNGLCLIENTACTIVETEXTUREARBPROC ) qwglGetProcAddress( "glClientActiveTextureARB" );

			if ( qglActiveTextureARB )
			{
				qglGetIntegerv( GL_MAX_ACTIVE_TEXTURES_ARB, &glConfig.maxActiveTextures );

				if ( glConfig.maxActiveTextures > 1 )
				{
					ri.Printf( PRINT_DEVELOPER, "...using GL_ARB_multitexture\n" );
				}
				else
				{
					qglMultiTexCoord2fARB = NULL;
					qglActiveTextureARB = NULL;
					qglClientActiveTextureARB = NULL;
					ri.Printf( PRINT_DEVELOPER, "...not using GL_ARB_multitexture, < 2 texture units\n" );
				}
			}
		}
		else
		{
			ri.Printf( PRINT_DEVELOPER, "...ignoring GL_ARB_multitexture\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_DEVELOPER, "...GL_ARB_multitexture not found\n" );
	}

	// GL_EXT_compiled_vertex_array
	qglLockArraysEXT = NULL;
	qglUnlockArraysEXT = NULL;
	if ( strstr( glConfig.extensions_string, "GL_EXT_compiled_vertex_array" ) )
	{
		if ( r_ext_compiled_vertex_array->integer )
		{
			ri.Printf( PRINT_DEVELOPER, "...using GL_EXT_compiled_vertex_array\n" );
			qglLockArraysEXT = ( void ( APIENTRY * )( int, int ) ) qwglGetProcAddress( "glLockArraysEXT" );
			qglUnlockArraysEXT = ( void ( APIENTRY * )( void ) ) qwglGetProcAddress( "glUnlockArraysEXT" );
			if (!qglLockArraysEXT || !qglUnlockArraysEXT) {
				ri.Error (ERR_FATAL, "bad getprocaddress");
			}
		}
		else
		{
			ri.Printf( PRINT_DEVELOPER, "...ignoring GL_EXT_compiled_vertex_array\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_DEVELOPER, "...GL_EXT_compiled_vertex_array not found\n" );
	}

	int maxAnisotropy = 0;
	if ( strstr( glConfig.extensions_string, "GL_EXT_texture_filter_anisotropic" ) )
	{
		if (r_ext_max_anisotropy->integer > 1) {
			qglGetIntegerv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAnisotropy );
			if ( maxAnisotropy <= 0 ) {
				ri.Printf( PRINT_DEVELOPER, "...GL_EXT_texture_filter_anisotropic not properly supported!\n" );
				maxAnisotropy = 0;
			}
			else
			{
				ri.Printf( PRINT_DEVELOPER, "...using GL_EXT_texture_filter_anisotropic (max: %i)\n", maxAnisotropy );
			}
		}
		else
		{
			ri.Printf( PRINT_DEVELOPER, "...ignoring GL_EXT_texture_filter_anisotropic\n" );
		}
	}
	else
	{
		ri.Printf( PRINT_DEVELOPER, "...GL_EXT_texture_filter_anisotropic not found\n" );
	}
	Cvar_Set( "r_ext_max_anisotropy", va("%i", maxAnisotropy) );

}


static qbool GLW_CheckOSVersion()
{
	OSVERSIONINFO vinfo;
	vinfo.dwOSVersionInfoSize = sizeof(vinfo);
	return (qbool)GetVersionEx( &vinfo );
}


static qbool GLW_LoadOpenGL()
{
	// only real GL implementations are acceptable
	const char* OPENGL_DRIVER_NAME = "opengl32";
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;

	//
	// load the driver and bind our function pointers to it
	//
	if ( QGL_Init( OPENGL_DRIVER_NAME ) )
	{
		qbool cdsFullscreen = (qbool)r_fullscreen->integer;

		// create the window and set up the context
		if ( !GLW_StartDriverAndSetMode( r_mode->integer, r_colorbits->integer, cdsFullscreen ) )
		{
			// fallback mode is 640x480x16 fullscreen
			if ( r_colorbits->integer != 16 || !cdsFullscreen || r_mode->integer != 3 )
			{
				if ( !GLW_StartDriverAndSetMode( 3, 16, qtrue ) )
				{
					goto fail;
				}
			}
			else
			{
				goto fail;
			}
		}

		return qtrue;
	}
fail:

	QGL_Shutdown();

	return qfalse;
}

/*
** GLimp_EndFrame
*/
void GLimp_EndFrame (void)
{
	//
	// swapinterval stuff
	//
	if ( r_swapInterval->modified ) {
		r_swapInterval->modified = qfalse;

		if ( !glConfig.stereoEnabled ) {	// why?
			if ( qwglSwapIntervalEXT ) {
				qwglSwapIntervalEXT( r_swapInterval->integer );
			}
		}
	}


	// don't flip if drawing to front buffer
	if ( Q_stricmp( r_drawBuffer->string, "GL_FRONT" ) != 0 )
	{
		SwapBuffers( glw_state.hDC );
	}

	// check logging
	QGL_EnableLogging( (qbool)r_logFile->integer );
}


/*
This is the platform specific OpenGL initialization function.
It is responsible for loading OpenGL, initializing it, setting
extensions, creating a window of the appropriate size, doing
fullscreen manipulations, etc.  Its overall responsibility is
to make sure that a functional OpenGL subsystem is operating
when it returns to the ref.
*/
void GLimp_Init()
{
	ri.Printf( PRINT_DEVELOPER, "Initializing OpenGL subsystem\n" );

	// check OS version to see if we can do fullscreen display changes
	if ( !GLW_CheckOSVersion() )
		ri.Error( ERR_FATAL, "GLimp_Init() - incorrect operating system\n" );

	// save off hInstance for the subsystems
	const cvar_t* cv = ri.Cvar_Get( "win_hinstance", "", 0 );
	sscanf( cv->string, "%i", (int *)&g_wv.hInstance );

	// load appropriate DLL and initialize subsystem
	if (!GLW_LoadOpenGL())
		ri.Error( ERR_FATAL, "GLimp_Init() - could not load OpenGL subsystem\n" );

	// get our config strings
	Q_strncpyz( glConfig.vendor_string, (const char*)qglGetString (GL_VENDOR), sizeof( glConfig.vendor_string ) );
	Q_strncpyz( glConfig.renderer_string, (const char*)qglGetString (GL_RENDERER), sizeof( glConfig.renderer_string ) );
	Q_strncpyz( glConfig.version_string, (const char*)qglGetString (GL_VERSION), sizeof( glConfig.version_string ) );
	Q_strncpyz( glConfig.extensions_string, (const char*)qglGetString (GL_EXTENSIONS), sizeof( glConfig.extensions_string ) );

	GLW_InitExtensions();
	WG_CheckHardwareGamma();
}


/*
** GLimp_Shutdown
**
** This routine does all OS specific shutdown procedures for the OpenGL
** subsystem.
*/
void GLimp_Shutdown( void )
{
	const char *success[] = { "failed", "success" };
	int retVal;

	// FIXME: Brian, we need better fallbacks from partially initialized failures
	if ( !qwglMakeCurrent ) {
		return;
	}

	ri.Printf( PRINT_ALL, "Shutting down OpenGL subsystem\n" );

	WG_RestoreGamma();

	// set current context to NULL
	if ( qwglMakeCurrent )
	{
		retVal = qwglMakeCurrent( NULL, NULL ) != 0;
		ri.Printf( PRINT_ALL, "...wglMakeCurrent( NULL, NULL ): %s\n", success[retVal] );
	}

	// delete HGLRC
	if ( glw_state.hGLRC )
	{
		retVal = qwglDeleteContext( glw_state.hGLRC ) != 0;
		ri.Printf( PRINT_ALL, "...deleting GL context: %s\n", success[retVal] );
		glw_state.hGLRC = NULL;
	}

	// release DC
	if ( glw_state.hDC )
	{
		retVal = ReleaseDC( g_wv.hWnd, glw_state.hDC ) != 0;
		ri.Printf( PRINT_ALL, "...releasing DC: %s\n", success[retVal] );
		glw_state.hDC   = NULL;
	}

	// destroy window
	if ( g_wv.hWnd )
	{
		ri.Printf( PRINT_ALL, "...destroying window\n" );
		ShowWindow( g_wv.hWnd, SW_HIDE );
		DestroyWindow( g_wv.hWnd );
		g_wv.hWnd = NULL;
		glw_state.pixelFormatSet = qfalse;
	}

	// close the r_logFile
	if ( glw_state.log_fp )
	{
		fclose( glw_state.log_fp );
		glw_state.log_fp = 0;
	}

	// reset display settings
	if ( glw_state.cdsFullscreen )
	{
		ri.Printf( PRINT_ALL, "...resetting display\n" );
		ChangeDisplaySettings( 0, 0 );
		glw_state.cdsFullscreen = qfalse;
	}

	// shutdown QGL subsystem
	QGL_Shutdown();

	memset( &glConfig, 0, sizeof( glConfig ) );
	memset( &glState, 0, sizeof( glState ) );
}

/*
** GLimp_LogComment
*/
void GLimp_LogComment( const char* comment )
{
	if ( glw_state.log_fp ) {
		fprintf( glw_state.log_fp, "%s", comment );
	}
}


/*
===========================================================

SMP acceleration

===========================================================
*/

static HANDLE renderCommandsEvent;
static HANDLE renderCompletedEvent;
static HANDLE renderActiveEvent;

static void (*glimpRenderThread)( void );

static void GLimp_RenderThreadWrapper()
{
	glimpRenderThread();
	// unbind the context before we die
	qwglMakeCurrent( glw_state.hDC, NULL );
}

qbool GLimp_SpawnRenderThread( void (*function)( void ) )
{
	renderCommandsEvent = CreateEvent( NULL, TRUE, FALSE, NULL );
	renderCompletedEvent = CreateEvent( NULL, TRUE, FALSE, NULL );
	renderActiveEvent = CreateEvent( NULL, TRUE, FALSE, NULL );

	glimpRenderThread = function;

	DWORD renderThreadId;
	HANDLE renderThreadHandle = CreateThread(
	   NULL,	// LPSECURITY_ATTRIBUTES lpsa,
	   0,		// DWORD cbStack,
	   (LPTHREAD_START_ROUTINE)GLimp_RenderThreadWrapper,	// LPTHREAD_START_ROUTINE lpStartAddr,
	   0,			// LPVOID lpvThreadParm,
	   0,			//   DWORD fdwCreate,
	   &renderThreadId );

	return (renderThreadHandle != NULL);
}


static	void	*smpData;
static	int		wglErrors;

void *GLimp_RendererSleep( void ) {
	void	*data;

	if ( !qwglMakeCurrent( glw_state.hDC, NULL ) ) {
		wglErrors++;
	}

	ResetEvent( renderActiveEvent );

	// after this, the front end can exit GLimp_FrontEndSleep
	SetEvent( renderCompletedEvent );

	WaitForSingleObject( renderCommandsEvent, INFINITE );

	if ( !qwglMakeCurrent( glw_state.hDC, glw_state.hGLRC ) ) {
		wglErrors++;
	}

	ResetEvent( renderCompletedEvent );
	ResetEvent( renderCommandsEvent );

	data = smpData;

	// after this, the main thread can exit GLimp_WakeRenderer
	SetEvent( renderActiveEvent );

	return data;
}


void GLimp_FrontEndSleep( void ) {
	WaitForSingleObject( renderCompletedEvent, INFINITE );

	if ( !qwglMakeCurrent( glw_state.hDC, glw_state.hGLRC ) ) {
		wglErrors++;
	}
}


void GLimp_WakeRenderer( void *data ) {
	smpData = data;

	if ( !qwglMakeCurrent( glw_state.hDC, NULL ) ) {
		wglErrors++;
	}

	// after this, the renderer can continue through GLimp_RendererSleep
	SetEvent( renderCommandsEvent );

	WaitForSingleObject( renderActiveEvent, INFINITE );
}

