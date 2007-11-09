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
// console.c

#include "client.h"


int g_console_field_width = 78;


#define	NUM_CON_TIMES 4

#define		CON_TEXTSIZE	32768
typedef struct {
	qbool	initialized;

	short	text[CON_TEXTSIZE];
	int		current;		// line where next message will be printed
	int		x;				// offset in current line for next print
	int		display;		// bottom of console displays this line

	int 	linewidth;		// characters across screen
	int		totallines;		// total lines in console scrollback

	float	xadjust;		// for wide aspect screens

	float	displayFrac;	// aproaches finalFrac at scr_conspeed
	float	finalFrac;		// 0.0 to 1.0 lines of console to display

	int		vislines;		// in scanlines

	int		times[NUM_CON_TIMES];	// cls.realtime time the line was generated
								// for transparent notify lines
} console_t;

extern	console_t	con;

console_t	con;

cvar_t		*con_conspeed;
cvar_t		*con_notifytime;

#define	DEFAULT_CONSOLE_WIDTH	78

vec4_t	console_color = {1.0, 1.0, 1.0, 1.0};


/*
================
Con_ToggleConsole_f
================
*/
void Con_ToggleConsole_f (void) {
	// closing a full screen console restarts the demo loop
	if ( cls.state == CA_DISCONNECTED && cls.keyCatchers == KEYCATCH_CONSOLE ) {
		CL_StartDemoLoop();
		return;
	}

	Field_Clear( &g_consoleField );
	g_consoleField.widthInChars = g_console_field_width;

	Con_ClearNotify ();
	cls.keyCatchers ^= KEYCATCH_CONSOLE;
}

/*
================
Con_MessageMode_f
================
*/
void Con_MessageMode_f (void) {
	chat_playerNum = -1;
	chat_team = qfalse;
	Field_Clear( &chatField );
	chatField.widthInChars = 30;

	cls.keyCatchers ^= KEYCATCH_MESSAGE;
}

/*
================
Con_MessageMode2_f
================
*/
void Con_MessageMode2_f (void) {
	chat_playerNum = -1;
	chat_team = qtrue;
	Field_Clear( &chatField );
	chatField.widthInChars = 25;
	cls.keyCatchers ^= KEYCATCH_MESSAGE;
}

/*
================
Con_MessageMode3_f
================
*/
void Con_MessageMode3_f (void) {
	chat_playerNum = VM_Call( cgvm, CG_CROSSHAIR_PLAYER );
	if ( chat_playerNum < 0 || chat_playerNum >= MAX_CLIENTS ) {
		chat_playerNum = -1;
		return;
	}
	chat_team = qfalse;
	Field_Clear( &chatField );
	chatField.widthInChars = 30;
	cls.keyCatchers ^= KEYCATCH_MESSAGE;
}

/*
================
Con_MessageMode4_f
================
*/
void Con_MessageMode4_f (void) {
	chat_playerNum = VM_Call( cgvm, CG_LAST_ATTACKER );
	if ( chat_playerNum < 0 || chat_playerNum >= MAX_CLIENTS ) {
		chat_playerNum = -1;
		return;
	}
	chat_team = qfalse;
	Field_Clear( &chatField );
	chatField.widthInChars = 30;
	cls.keyCatchers ^= KEYCATCH_MESSAGE;
}


void Con_Clear_f( void )
{
	int		i;

	for ( i = 0 ; i < CON_TEXTSIZE ; i++ ) {
		con.text[i] = (ColorIndex(COLOR_WHITE)<<8) | ' ';
	}

	Con_Bottom();		// go to end
}


// save the console contents out to a file

void Con_Dump_f( void )
{
	int		l, x, i;
	short	*line;
	char	buffer[1024];

	if (Cmd_Argc() != 2)
	{
		Com_Printf ("usage: condump <filename>\n");
		return;
	}

	Q_strncpyz( buffer, Cmd_Argv(1), MAX_QPATH - 4 );
	COM_DefaultExtension( buffer, sizeof(buffer), ".txt" );

	fileHandle_t f = FS_FOpenFileWrite( buffer );
	if (!f)
	{
		Com_Printf( "ERROR: couldn't open %s\n", buffer );
		return;
	}

	Com_Printf( "Dumped console text to %s\n", buffer );

	// skip empty lines
	for (l = con.current - con.totallines + 1 ; l <= con.current ; l++)
	{
		line = con.text + (l%con.totallines)*con.linewidth;
		for (x=0 ; x<con.linewidth ; x++)
			if ((line[x] & 0xff) != ' ')
				break;
		if (x != con.linewidth)
			break;
	}

	// write the remaining lines
	buffer[con.linewidth] = 0;
	for ( ; l <= con.current ; l++)
	{
		line = con.text + (l%con.totallines)*con.linewidth;
		for(i=0; i<con.linewidth; i++)
			buffer[i] = line[i] & 0xff;
		for (x=con.linewidth-1 ; x>=0 ; x--)
		{
			if (buffer[x] == ' ')
				buffer[x] = 0;
			else
				break;
		}
		strcat( buffer, "\n" );
		FS_Write(buffer, strlen(buffer), f);
	}

	FS_FCloseFile( f );
}


/*
================
Con_ClearNotify
================
*/
void Con_ClearNotify( void ) {
	int		i;
	
	for ( i = 0 ; i < NUM_CON_TIMES ; i++ ) {
		con.times[i] = 0;
	}
}


/*
================
Con_CheckResize

If the line width has changed, reformat the buffer.
================
*/
void Con_CheckResize (void)
{
	int		i, j, width, oldwidth, oldtotallines, numlines, numchars;
	short	tbuf[CON_TEXTSIZE];

	width = (SCREEN_WIDTH / SMALLCHAR_WIDTH) - 2;

	if (width == con.linewidth)
		return;

	if (width < 1)			// video hasn't been initialized yet
	{
		width = DEFAULT_CONSOLE_WIDTH;
		con.linewidth = width;
		con.totallines = CON_TEXTSIZE / con.linewidth;
		for(i=0; i<CON_TEXTSIZE; i++)

			con.text[i] = (ColorIndex(COLOR_WHITE)<<8) | ' ';
	}
	else
	{
		oldwidth = con.linewidth;
		con.linewidth = width;
		oldtotallines = con.totallines;
		con.totallines = CON_TEXTSIZE / con.linewidth;
		numlines = oldtotallines;

		if (con.totallines < numlines)
			numlines = con.totallines;

		numchars = oldwidth;
	
		if (con.linewidth < numchars)
			numchars = con.linewidth;

		Com_Memcpy (tbuf, con.text, CON_TEXTSIZE * sizeof(short));
		for(i=0; i<CON_TEXTSIZE; i++)

			con.text[i] = (ColorIndex(COLOR_WHITE)<<8) | ' ';


		for (i=0 ; i<numlines ; i++)
		{
			for (j=0 ; j<numchars ; j++)
			{
				con.text[(con.totallines - 1 - i) * con.linewidth + j] =
						tbuf[((con.current - i + oldtotallines) %
							  oldtotallines) * oldwidth + j];
			}
		}

		Con_ClearNotify ();
	}

	con.current = con.totallines - 1;
	con.display = con.current;
}


/*
================
Con_Init
================
*/
void Con_Init (void) {
	int		i;

	con_notifytime = Cvar_Get ("con_notifytime", "3", 0);
	con_conspeed = Cvar_Get ("scr_conspeed", "3", 0);

	Field_Clear( &g_consoleField );
	g_consoleField.widthInChars = g_console_field_width;
	for ( i = 0 ; i < COMMAND_HISTORY ; i++ ) {
		Field_Clear( &historyEditLines[i] );
		historyEditLines[i].widthInChars = g_console_field_width;
	}

	Cmd_AddCommand ("toggleconsole", Con_ToggleConsole_f);
	Cmd_AddCommand ("messagemode", Con_MessageMode_f);
	Cmd_AddCommand ("messagemode2", Con_MessageMode2_f);
	Cmd_AddCommand ("messagemode3", Con_MessageMode3_f);
	Cmd_AddCommand ("messagemode4", Con_MessageMode4_f);
	Cmd_AddCommand ("clear", Con_Clear_f);
	Cmd_AddCommand ("condump", Con_Dump_f);
}


/*
===============
Con_Linefeed
===============
*/
void Con_Linefeed (qbool skipnotify)
{
	int		i;

	// mark time for transparent overlay
	if (con.current >= 0)
	{
		con.times[con.current % NUM_CON_TIMES] = skipnotify ? 0 : cls.realtime;
	}

	con.x = 0;
	if (con.display == con.current)
		con.display++;
	con.current++;
	for(i=0; i<con.linewidth; i++)
		con.text[(con.current%con.totallines)*con.linewidth+i] = (ColorIndex(COLOR_WHITE)<<8) | ' ';
}


/*
================
CL_ConsolePrint

Handles cursor positioning, line wrapping, etc
All console printing must go through this in order to be logged to disk
If no console is visible, the text will appear at the top of the game window
================
*/
void CL_ConsolePrint( char *txt ) {
	int		y;
	int		c, l;
	int		color;
	qbool skipnotify = qfalse;		// NERVE - SMF
	int prev;							// NERVE - SMF

	// TTimo - prefix for text that shows up in console but not in notify
	// backported from RTCW
	if ( !Q_strncmp( txt, "[skipnotify]", 12 ) ) {
		skipnotify = qtrue;
		txt += 12;
	}

	// for some demos we don't want to ever show anything on the console
	if ( cl_noprint && cl_noprint->integer ) {
		return;
	}

	if (!con.initialized) {
		con.linewidth = -1;
		Con_CheckResize ();
		con.initialized = qtrue;
	}

	color = ColorIndex(COLOR_WHITE);

	while ( (c = *txt) != 0 ) {
		if ( Q_IsColorString( txt ) ) {
			color = ColorIndex( *(txt+1) );
			txt += 2;
			continue;
		}

		// count word length
		for (l=0 ; l< con.linewidth ; l++) {
			if ( txt[l] <= ' ') {
				break;
			}

		}

		// word wrap
		if (l != con.linewidth && (con.x + l >= con.linewidth) ) {
			Con_Linefeed(skipnotify);

		}

		txt++;

		switch (c)
		{
		case '\n':
			Con_Linefeed (skipnotify);
			break;
		case '\r':
			con.x = 0;
			break;
		default:	// display character and advance
			y = con.current % con.totallines;
			con.text[y*con.linewidth+con.x] = (color << 8) | c;
			con.x++;
			if (con.x >= con.linewidth) {
				Con_Linefeed(skipnotify);
				con.x = 0;
			}
			break;
		}
	}


	// mark time for transparent overlay
	if (con.current >= 0) {
		// NERVE - SMF
		if ( skipnotify ) {
			prev = con.current % NUM_CON_TIMES - 1;
			if ( prev < 0 )
				prev = NUM_CON_TIMES - 1;
			con.times[prev] = 0;
		}
		else
		// -NERVE - SMF
			con.times[con.current % NUM_CON_TIMES] = cls.realtime;
	}
}


/*
==============================================================================

DRAWING

==============================================================================
*/


// changing colors every char is bad, but

static void Con_DrawShadowedChar( int x, int y, int c, const vec4_t color )
{
	re.SetColor( colorBlack );
	SCR_DrawSmallChar( x + 1, y + 1, c );
	re.SetColor( color );
	SCR_DrawSmallChar( x, y, c );
}


// Draw the editline after a ] prompt

static void Con_DrawInput()
{
	int y;

	if ( cls.state != CA_DISCONNECTED && !(cls.keyCatchers & KEYCATCH_CONSOLE ) ) {
		return;
	}

	y = con.vislines - ( SMALLCHAR_HEIGHT * 2 );

	Con_DrawShadowedChar( con.xadjust, y, ']', colorWhite );

	Field_Draw( &g_consoleField, con.xadjust + SMALLCHAR_WIDTH, y,
		SCREEN_WIDTH - 3 * SMALLCHAR_WIDTH, qtrue );
}


// draws the last few lines of output transparently over the game area

static void Con_DrawNotify()
{
	int		x, v;
	short	*text;
	int		i;
	int		time;
	int		skip;
	int		currentColor;

	currentColor = 7;
	re.SetColor( g_color_table[currentColor] );

	v = 0;
	for (i= con.current-NUM_CON_TIMES+1 ; i<=con.current ; i++)
	{
		if (i < 0)
			continue;
		time = con.times[i % NUM_CON_TIMES];
		if (time == 0)
			continue;
		time = cls.realtime - time;
		if (time > con_notifytime->value*1000)
			continue;
		text = con.text + (i % con.totallines)*con.linewidth;

		if (cl.snap.ps.pm_type != PM_INTERMISSION && cls.keyCatchers & (KEYCATCH_UI | KEYCATCH_CGAME) ) {
			continue;
		}

		for (x = 0 ; x < con.linewidth ; x++) {
			if ( ( text[x] & 0xff ) == ' ' ) {
				continue;
			}
			if ( (text[x]>>8) != currentColor ) {
				currentColor = (text[x]>>8);
				re.SetColor( g_color_table[currentColor] );
			}
			SCR_DrawSmallChar( cl_conXOffset->integer + con.xadjust + (x+1)*SMALLCHAR_WIDTH, v, text[x] & 0xff );
		}

		v += SMALLCHAR_HEIGHT;
	}

	re.SetColor( NULL );

	if (cls.keyCatchers & (KEYCATCH_UI | KEYCATCH_CGAME) ) {
		return;
	}

	// draw the chat line
	if ( cls.keyCatchers & KEYCATCH_MESSAGE )
	{
		if (chat_team)
		{
			SCR_DrawBigString (8, v, "say_team:", 1.0f );
			skip = 10;
		}
		else
		{
			SCR_DrawBigString (8, v, "say:", 1.0f );
			skip = 5;
		}

		Field_BigDraw( &chatField, skip * BIGCHAR_WIDTH, v,
			SCREEN_WIDTH - ( skip + 1 ) * BIGCHAR_WIDTH, qtrue );

		v += BIGCHAR_HEIGHT;
	}

}


///////////////////////////////////////////////////////////////

#if 0 // not really finished with this yet

static float CPMA_HUD_DrawChar( float x, float y, int ch )
{
	if (ch == GLYPH_START)
		return cls.fontConsole.maxpitch;

	if ((ch > GLYPH_START) && (ch <= GLYPH_END)) {
		ch -= GLYPH_START;
		//re.DrawStretchPic( con.xadjust + x, y, cls.fontConsole.widths[ch], cls.fontConsole.height, 0, 0, 1, 1, cls.fontConsole.shaders[ch] );
		//return cls.fontConsole.pitches[ch];
		int offset = 0.5 * (cls.fontConsole.maxpitch - cls.fontConsole.pitches[ch]);
		re.DrawStretchPic( con.xadjust + offset + x, y, cls.fontConsole.widths[ch], cls.fontConsole.height, 0, 0, 1, 1, cls.fontConsole.shaders[ch] );
		return cls.fontConsole.maxpitch;
	}

	return 0;
}


static float CPMA_HUD_StringWidth( const char* s )
{
	int ch;
	float w = 0;
	while (ch = *s++)
		if ((ch >= GLYPH_START) && (ch <= GLYPH_END))
			//w += cls.fontConsole.pitches[ch - GLYPH_START];
			w += cls.fontConsole.maxpitch;
	return w;
}


static void Con_DrawSolidConsole( float frac )
{
	int		i, x, y;
	int		rows;
	short	*text;
	int		row;
	vec4_t	color;

	int scanlines = Com_Clamp( 0, cls.glconfig.vidHeight, cls.glconfig.vidHeight * frac );
	if (scanlines <= 0)
		return;

	// on wide screens, we will center the text
	con.xadjust = 0;
	SCR_AdjustFrom640( &con.xadjust, NULL, NULL, NULL );
	con.xadjust += SMALLCHAR_WIDTH;

	// draw the background
	y = frac * SCREEN_HEIGHT - 2;
	if ( y < 1 ) {
		y = 0;
	}
	else {
		MAKERGBA( color, 0.44f, 0.44f, 0.44f, 1.0 );
		SCR_FillRect( 0, 0, SCREEN_WIDTH, y, color );
	}

	MAKERGBA( color, 0.33f, 0.33f, 0.33f, 1.0 );
	SCR_FillRect( 0, y, SCREEN_WIDTH, 2, color );

	// draw the text
	con.vislines = scanlines;
	rows = (scanlines - cls.fontConsole.vpitch) / cls.fontConsole.vpitch;

	y = scanlines - (cls.fontConsole.vpitch * 3);

	i = 0;
	x = cls.glconfig.vidWidth - CPMA_HUD_StringWidth(Q3_VERSION) - SMALLCHAR_WIDTH;
	for (i = 0; Q3_VERSION[i]; ++i) {
		x += CPMA_HUD_DrawChar( x, scanlines - cls.fontConsole.height, Q3_VERSION[i] );
	}
	re.SetColor( NULL );

	// draw from the bottom up
	if (con.display != con.current)
	{
		// draw arrows to show the buffer is backscrolled
		re.SetColor( colorBlack );
		for (x = 0; x < con.linewidth; x += 4)
			CPMA_HUD_DrawChar( x * SMALLCHAR_WIDTH, y, '^' );
		y -= cls.fontConsole.vpitch;
		--rows;
	}

	row = con.display;

	if ( con.x == 0 ) {
		row--;
	}

	int colorCode = 7; // *not* COLOR_WHITE: that's an *ASCII* 7
	re.SetColor( g_color_table[colorCode] );

	for (i = 0; i < rows; ++i, --row, y -= cls.fontConsole.vpitch )
	{
		if (row < 0)
			break;
		if (con.current - row >= con.totallines) {
			// past scrollback wrap point
			continue;
		}

		text = con.text + (row % con.totallines)*con.linewidth;

/* grr - this can pass the 1Kverts surface limit now if you use a small enough font...  :P
		// since fonts are now texture-per-char, minimize the glBinds rather than the glColors
		x = 0;
		for (int i = 0; i < con.linewidth; ++i) {
			re.SetColor( colorBlack );
			CPMA_HUD_DrawChar( 1 + x, y + 1, (text[i] & 0xFF) );
			re.SetColor( colorWhite );
			x += CPMA_HUD_DrawChar( x, y, (text[i] & 0xFF) );
		}
*/
		re.SetColor( colorBlack );
		x = 0;
		for (int i = 0; i < con.linewidth; ++i) {
			x += CPMA_HUD_DrawChar( 1 + x, 1 + y, (text[i] & 0xFF) );
		}

		re.SetColor( colorWhite );
		x = 0;
		for (int i = 0; i < con.linewidth; ++i) {
			if ((text[i] >> 8) != colorCode) {
				colorCode = (text[i] >> 8);
				re.SetColor( g_color_table[colorCode] );
			}
			x += CPMA_HUD_DrawChar( x, y, (text[i] & 0xFF) );
		}
	}

	// draw the input prompt, user text, and cursor if desired
	Con_DrawInput();

	re.SetColor( NULL );
}

#endif

///////////////////////////////////////////////////////////////


static void Con_DrawSolidConsole( float frac )
{
	int		i, x, y;
	int		rows;
	int		row;
	vec4_t	color;

	int scanlines = Com_Clamp( 0, cls.glconfig.vidHeight, cls.glconfig.vidHeight * frac );
	if (scanlines <= 0)
		return;

	// on wide screens, we will center the text
	con.xadjust = 0;
	SCR_AdjustFrom640( &con.xadjust, NULL, NULL, NULL );
	con.xadjust += SMALLCHAR_WIDTH;

	// draw the background
	y = frac * SCREEN_HEIGHT - 2;
	if ( y < 1 ) {
		y = 0;
	}
	else {
		MAKERGBA( color, 0.44f, 0.44f, 0.44f, 1.0 );
		SCR_FillRect( 0, 0, SCREEN_WIDTH, y, color );
	}

	MAKERGBA( color, 0.33f, 0.33f, 0.33f, 1.0 );
	SCR_FillRect( 0, y, SCREEN_WIDTH, 2, color );

	i = strlen( Q3_VERSION );
	x = cls.glconfig.vidWidth;
	while (--i >= 0) {
		x -= SMALLCHAR_WIDTH;
		SCR_DrawSmallChar( x, scanlines - (SMALLCHAR_HEIGHT * 1.5), Q3_VERSION[i] );
	}

	re.SetColor( NULL );
	con.vislines = scanlines;
	rows = (scanlines - SMALLCHAR_HEIGHT) / SMALLCHAR_HEIGHT;
	y = scanlines - (SMALLCHAR_HEIGHT * 3);

	// draw the console text from the bottom up
	if (con.display != con.current)
	{
		// draw arrows to show the buffer is backscrolled
		re.SetColor( colorBlack );
		for (x = 0; x < con.linewidth; x += 4)
			SCR_DrawSmallChar( con.xadjust + (x * SMALLCHAR_WIDTH), y, '^' );
		y -= SMALLCHAR_HEIGHT;
		--rows;
	}

	row = con.display;
	if ( con.x == 0 ) {
		row--;
	}

	int colorCode = 7; // *not* COLOR_WHITE: that's an *ASCII* 7
	re.SetColor( g_color_table[colorCode] );

	for (i = 0; i < rows; ++i, --row, y -= SMALLCHAR_HEIGHT )
	{
		if (row < 0)
			break;
		if (con.current - row >= con.totallines) {
			// past scrollback wrap point
			continue;
		}

		const short* text = con.text + (row % con.totallines)*con.linewidth;

		re.SetColor( colorBlack );
		for (int i = 0; i < con.linewidth; ++i) {
			SCR_DrawSmallChar( 1 + con.xadjust + i * SMALLCHAR_WIDTH, 1 + y, (text[i] & 0xFF) );
		}

		re.SetColor( colorWhite );
		for (int i = 0; i < con.linewidth; ++i) {
			if ((text[i] >> 8) != colorCode) {
				colorCode = (text[i] >> 8);
				re.SetColor( g_color_table[colorCode] );
			}
			SCR_DrawSmallChar( con.xadjust + i * SMALLCHAR_WIDTH, y, (text[i] & 0xFF) );
		}
	}

	// draw the input prompt, user text, and cursor if desired
	Con_DrawInput();

	re.SetColor( NULL );
}


///////////////////////////////////////////////////////////////


/*
==================
Con_DrawConsole
==================
*/
void Con_DrawConsole( void ) {
	// check for console width changes from a vid mode change
	Con_CheckResize ();

	// if disconnected, render console full screen
	if ( cls.state == CA_DISCONNECTED ) {
		if ( !( cls.keyCatchers & (KEYCATCH_UI | KEYCATCH_CGAME)) ) {
			Con_DrawSolidConsole( 1.0 );
			return;
		}
	}

	if ( con.displayFrac ) {
		Con_DrawSolidConsole( con.displayFrac );
	} else {
		// draw notify lines
		if ( cls.state == CA_ACTIVE ) {
			Con_DrawNotify ();
		}
	}
}

//================================================================

/*
==================
Con_RunConsole

Scroll it up or down
==================
*/
void Con_RunConsole (void) {
	// decide on the destination height of the console
	if ( cls.keyCatchers & KEYCATCH_CONSOLE )
		con.finalFrac = 0.5;		// half screen
	else
		con.finalFrac = 0;				// none visible
	
	// scroll towards the destination height
	if (con.finalFrac < con.displayFrac)
	{
		con.displayFrac -= con_conspeed->value*cls.realFrametime*0.001;
		if (con.finalFrac > con.displayFrac)
			con.displayFrac = con.finalFrac;

	}
	else if (con.finalFrac > con.displayFrac)
	{
		con.displayFrac += con_conspeed->value*cls.realFrametime*0.001;
		if (con.finalFrac < con.displayFrac)
			con.displayFrac = con.finalFrac;
	}

}


void Con_PageUp( void ) {
	con.display -= 2;
	if ( con.current - con.display >= con.totallines ) {
		con.display = con.current - con.totallines + 1;
	}
}

void Con_PageDown( void ) {
	con.display += 2;
	if (con.display > con.current) {
		con.display = con.current;
	}
}

void Con_Top( void ) {
	con.display = con.totallines;
	if ( con.current - con.display >= con.totallines ) {
		con.display = con.current - con.totallines + 1;
	}
}

void Con_Bottom( void ) {
	con.display = con.current;
}


void Con_Close( void ) {
	if ( !com_cl_running->integer ) {
		return;
	}
	Field_Clear( &g_consoleField );
	Con_ClearNotify ();
	cls.keyCatchers &= ~KEYCATCH_CONSOLE;
	con.finalFrac = 0;				// none visible
	con.displayFrac = 0;
}