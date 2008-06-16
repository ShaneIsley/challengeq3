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
// cl_scrn.c -- master for refresh, console, chat, notify, etc

#include "client.h"


static qbool scr_initialized;	// ready to draw

cvar_t		*cl_timegraph;
static cvar_t* cl_graphheight;
static cvar_t* cl_graphscale;
static cvar_t* cl_graphshift;


// adjust for resolution and screen aspect ratio

void SCR_AdjustFrom640( float *x, float *y, float *w, float *h )
{
#if 0
	// adjust for wide screens
	if ( cls.glconfig.vidWidth * 480 > cls.glconfig.vidHeight * 640 ) {
		*x += 0.5 * ( cls.glconfig.vidWidth - ( cls.glconfig.vidHeight * 640 / 480 ) );
	}
#endif

	// scale for screen sizes
	float xscale = cls.glconfig.vidWidth / 640.0;
	float yscale = cls.glconfig.vidHeight / 480.0;

	if ( x ) {
		*x *= xscale;
	}
	if ( y ) {
		*y *= yscale;
	}
	if ( w ) {
		*w *= xscale;
	}
	if ( h ) {
		*h *= yscale;
	}
}


// SCR_ functions operate  at native resolution, NOT the virtual 640x480 screen


void SCR_DrawChar( float x, float y, float cw, float ch, int c )
{
	const float CELL_TCSIZE = 0.0625;

	c &= 255;

	if ( c == ' ' ) {
		return;
	}

	int xcell = c & 15;
	int ycell = c >> 4;

	float s = xcell * CELL_TCSIZE;
	float t = ycell * CELL_TCSIZE;

	re.DrawStretchPic( x, y, cw, ch, s, t, s + CELL_TCSIZE, t + CELL_TCSIZE, cls.charSetShader );
}


// draws a string with a drop shadow, optionally with colorcodes

void SCR_DrawString( float x, float y, float cw, float ch, const char* string, qbool allowColor )
{
	float xx;
	const char* s;

	// don't bother if nothing will be visible
	if ( y < -ch ) {
		return;
	}

	// draw the drop shadow
	re.SetColor( colorBlack );
	s = string;
	xx = x;
	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			s += 2;
			continue;
		}
		SCR_DrawChar( xx+1, y+1, cw, ch, *s );
		xx += cw;
		s++;
	}

	// draw the text, possibly with colors
	s = string;
	xx = x;
	re.SetColor( colorWhite );
	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			if ( allowColor ) {
				re.SetColor( ColorFromChar( s[1] ) );
			}
			s += 2;
			continue;
		}
		SCR_DrawChar( xx, y, cw, ch, *s );
		xx += cw;
		s++;
	}

	re.SetColor( NULL );
}


///////////////////////////////////////////////////////////////


static void SCR_DrawDemoRecording()
{
	if (!clc.demorecording || !clc.showAnnoyingDemoRecordMessage)
		return;

	int pos = FS_FTell( clc.demofile );
	const char* s = va( "RECORDING %s: %ik", clc.demoName, pos / 1024 );

	SCR_DrawString( 320 - strlen(s) * 4, 20, 8, 8, s, qfalse );
}


///////////////////////////////////////////////////////////////


typedef struct
{
	float	value;
	int		color;
} graphsamp_t;

static	int			current;
static	graphsamp_t	values[1024];


void SCR_DebugGraph( float value, int color )
{
	values[current&1023].value = value;
	values[current&1023].color = color;
	current++;
}


static void SCR_DrawDebugGraph()
{
	int		a, x, y, w, i, h;
	float	v;
	int		color;

	w = cls.glconfig.vidWidth;
	x = 0;
	y = cls.glconfig.vidHeight;
	re.SetColor( colorBlack );
	re.DrawStretchPic(x, y - cl_graphheight->integer, 
		w, cl_graphheight->integer, 0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );

	for (a=0 ; a<w ; a++)
	{
		i = (current-1-a+1024) & 1023;
		v = values[i].value;
		color = values[i].color;
		v = v * cl_graphscale->integer + cl_graphshift->integer;

		if (v < 0)
			v += cl_graphheight->integer * (1+(int)(-v / cl_graphheight->integer));
		h = (int)v % cl_graphheight->integer;
		re.DrawStretchPic( x+w-1-a, y - h, 1, h, 0, 0, 0, 0, cls.whiteShader );
	}
}


///////////////////////////////////////////////////////////////


void SCR_Init()
{
	cl_timegraph = Cvar_Get( "timegraph", "0", CVAR_CHEAT );
	cl_graphheight = Cvar_Get( "graphheight", "32", CVAR_CHEAT );
	cl_graphscale = Cvar_Get( "graphscale", "1", CVAR_CHEAT );
	cl_graphshift = Cvar_Get( "graphshift", "0", CVAR_CHEAT );

	scr_initialized = qtrue;
}


///////////////////////////////////////////////////////////////


// this will be called twice if rendering in stereo mode

static void SCR_DrawScreenField( stereoFrame_t stereoFrame )
{
	if (!cls.rendererStarted)
		return;

	re.BeginFrame( stereoFrame );

	// wide aspect ratio screens need to have the sides cleared
	// unless they are displaying game renderings
	if ( cls.state != CA_ACTIVE && cls.state != CA_CINEMATIC ) {
		if ( cls.glconfig.vidWidth * 480 > cls.glconfig.vidHeight * 640 ) {
			re.SetColor( colorBlack );
			re.DrawStretchPic( 0, 0, cls.glconfig.vidWidth, cls.glconfig.vidHeight, 0, 0, 0, 0, cls.whiteShader );
			re.SetColor( NULL );
		}
	}

	if ( !uivm ) {
		Com_DPrintf( "draw screen without UI loaded\n" );
		return;
	}

	// if the menu is going to cover the entire screen, we
	// don't need to render anything under it
	if ( !VM_Call( uivm, UI_IS_FULLSCREEN )) {
		switch( cls.state ) {
		default:
			Com_Error( ERR_FATAL, "SCR_DrawScreenField: bad cls.state" );
			break;
		case CA_CINEMATIC:
			SCR_DrawCinematic();
			break;
		case CA_DISCONNECTED:
			// force menu up
			S_StopAllSounds();
			VM_Call( uivm, UI_SET_ACTIVE_MENU, UIMENU_MAIN );
			break;
		case CA_CONNECTING:
		case CA_CHALLENGING:
		case CA_CONNECTED:
			// connecting clients will only show the connection dialog
			// refresh to update the time
			VM_Call( uivm, UI_REFRESH, cls.realtime );
			VM_Call( uivm, UI_DRAW_CONNECT_SCREEN, qfalse );
			break;
		case CA_LOADING:
		case CA_PRIMED:
			// draw the game information screen and loading progress
			CL_CGameRendering( stereoFrame );
			// also draw the connection information, so it doesn't
			// flash away too briefly on local or lan games
			// refresh to update the time
			VM_Call( uivm, UI_REFRESH, cls.realtime );
			VM_Call( uivm, UI_DRAW_CONNECT_SCREEN, qtrue );
			break;
		case CA_ACTIVE:
			CL_CGameRendering( stereoFrame );
			SCR_DrawDemoRecording();
			break;
		}
	}

	// the menu draws next
	if ( cls.keyCatchers & KEYCATCH_UI && uivm ) {
		VM_Call( uivm, UI_REFRESH, cls.realtime );
	}

	// console draws next
	Con_DrawConsole();

	// debug graphs can be drawn on top of anything
	if ( cl_timegraph->integer || cl_debugMove->integer ) {
		SCR_DrawDebugGraph ();
	}
}


// called every frame, and can also be called explicitly to flush text to the screen

void SCR_UpdateScreen()
{
	static int recursive = 0;

	if ( !scr_initialized ) {
		return;				// not initialized yet
	}

	// there are several cases where this IS called twice in one frame
	// easiest example is: conn to a server, kill the server
	if ( ++recursive > 2 ) {
		Com_Error( ERR_FATAL, "SCR_UpdateScreen: recursively called" );
	}

	// if running in stereo, we need to draw the frame twice
	if ( cls.glconfig.stereoEnabled ) {
		SCR_DrawScreenField( STEREO_LEFT );
		SCR_DrawScreenField( STEREO_RIGHT );
	} else {
		SCR_DrawScreenField( STEREO_CENTER );
	}

	if ( com_speeds->integer ) {
		re.EndFrame( &time_frontend, &time_backend );
	} else {
		re.EndFrame( NULL, NULL );
	}

	--recursive;
}

