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


static const cvar_t* con_conspeed;
static const cvar_t* con_notifytime;
static const cvar_t* cl_noprint;
static const cvar_t* cl_conXOffset;


#define CON_NOTIFYLINES	4

#define CON_TEXTSIZE	32768

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

	int		times[CON_NOTIFYLINES];	// cls.realtime time the line was generated
								// for transparent notify lines
} console_t;

static console_t con;

#define DEFAULT_CONSOLE_WIDTH 78
int g_console_field_width = DEFAULT_CONSOLE_WIDTH;


void Con_ToggleConsole_f( void )
{
	// closing a full screen console restarts the demo loop
	if ( cls.state == CA_DISCONNECTED && cls.keyCatchers == KEYCATCH_CONSOLE ) {
		CL_StartDemoLoop();
		return;
	}

	Field_Clear( &g_consoleField );
	g_consoleField.widthInChars = g_console_field_width;

	Con_ClearNotify();
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


static void Con_Clear_f( void )
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
		FS_Write( buffer, strlen(buffer), f );
	}

	FS_FCloseFile( f );
}


void Con_ClearNotify()
{
	for (int i = 0; i < CON_NOTIFYLINES; ++i) {
		con.times[i] = 0;
	}
}


// if the line width has changed, reformat the buffer
// KHB !!!  this is pointless other than at init, and the comment is a lie - the console is ALWAYS 78 chars wide

static void Con_CheckResize()
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

		Con_ClearNotify();
	}

	con.current = con.totallines - 1;
	con.display = con.current;
}


void Con_Init()
{
	int		i;

	con_notifytime = Cvar_Get( "con_notifytime", "3", 0 );
	con_conspeed = Cvar_Get( "scr_conspeed", "3", 0 );
	cl_noprint = Cvar_Get( "cl_noprint", "0", 0 );
	cl_conXOffset = Cvar_Get( "cl_conXOffset", "0", 0 );

	Field_Clear( &g_consoleField );
	g_consoleField.widthInChars = g_console_field_width;
	for ( i = 0 ; i < COMMAND_HISTORY ; i++ ) {
		Field_Clear( &historyEditLines[i] );
		historyEditLines[i].widthInChars = g_console_field_width;
	}

	Cmd_AddCommand( "toggleconsole", Con_ToggleConsole_f );
	Cmd_AddCommand( "messagemode", Con_MessageMode_f );
	Cmd_AddCommand( "messagemode2", Con_MessageMode2_f );
	Cmd_AddCommand( "messagemode3", Con_MessageMode3_f );
	Cmd_AddCommand( "messagemode4", Con_MessageMode4_f );
	Cmd_AddCommand( "clear", Con_Clear_f );
	Cmd_AddCommand( "condump", Con_Dump_f );
}


static void Con_Linefeed()
{
	// mark time for transparent overlay
	if (con.current >= 0) {
		con.times[con.current % CON_NOTIFYLINES] = cls.realtime;
	}

	con.x = 0;
	if (con.display == con.current)
		con.display++;
	con.current++;

	for ( int i = 0; i < con.linewidth; ++i ) {
		con.text[(con.current%con.totallines)*con.linewidth+i] = (ColorIndex(COLOR_WHITE)<<8) | ' ';
	}
}


/*
Handles cursor positioning, line wrapping, etc
All console printing must go through this in order to be logged to disk
If no console is visible, the text will appear at the top of the game window
*/
void CL_ConsolePrint( const char* s )
{
	int		y;
	int		c, w;
	int		color;

	// cl_noprint disables ALL console functionality
	// use con_notifytime 0 to log the text without the annoying overlay
	if ( cl_noprint && cl_noprint->integer ) {
		return;
	}

	if (!con.initialized) {
		con.linewidth = -1;
		Con_CheckResize();
		con.initialized = qtrue;
	}

	color = ColorIndex(COLOR_WHITE);

	while ( c = *s ) {
		if ( Q_IsColorString( s ) ) {
			color = ColorIndex( *(s+1) );
			s += 2;
			continue;
		}

		// count word length and wordwrap if needed
		for (w = 0; w < con.linewidth; ++w) {
			if ( s[w] <= ' ') {
				break;
			}
		}
		if (w != con.linewidth && (con.x + w >= con.linewidth) ) {
			Con_Linefeed();
		}

		++s;

		switch (c)
		{
		case '\n':
			Con_Linefeed();
			break;
		case '\r':
			con.x = 0;
			break;
		default:	// display character and advance
			y = con.current % con.totallines;
			con.text[y*con.linewidth+con.x] = (color << 8) | c;
			con.x++;
			if (con.x >= con.linewidth) {
				Con_Linefeed();
				con.x = 0;
			}
			break;
		}
	}

	// mark time for transparent overlay
	if (con.current >= 0) {
		con.times[con.current % CON_NOTIFYLINES] = cls.realtime;
	}
}


/*
==============================================================================

DRAWING

==============================================================================
*/


// draw the editline with a prompt in front of it

static void Con_DrawInput()
{
	int y;

	if ( cls.state != CA_DISCONNECTED && !(cls.keyCatchers & KEYCATCH_CONSOLE ) ) {
		return;
	}

	y = con.vislines - ( SMALLCHAR_HEIGHT * 2 );

	re.SetColor( colorBlack );
	SCR_DrawSmallChar( con.xadjust + 1, y + 1, ']' );
	re.SetColor( colorWhite );
	SCR_DrawSmallChar( con.xadjust, y, ']' );

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

	int currentColor = 7; // *not* COLOR_WHITE: that's an *ASCII* 7
	re.SetColor( g_color_table[currentColor] );

	v = 0;
	for (i= con.current-CON_NOTIFYLINES+1 ; i<=con.current ; i++)
	{
		if (i < 0)
			continue;
		time = con.times[i % CON_NOTIFYLINES];
		if (time == 0)
			continue;
		time = cls.realtime - time;
		if (time >= con_notifytime->value*1000)
			continue;
		text = con.text + (i % con.totallines)*con.linewidth;

		if ( cls.keyCatchers & (KEYCATCH_UI | KEYCATCH_CGAME) ) {
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
			SCR_DrawBigString (8, v, "say_team:" );
			skip = 10;
		}
		else
		{
			SCR_DrawBigString (8, v, "say:" );
			skip = 5;
		}

		Field_BigDraw( &chatField, skip * BIGCHAR_WIDTH, v,
			SCREEN_WIDTH - ( skip + 1 ) * BIGCHAR_WIDTH, qtrue );

		v += BIGCHAR_HEIGHT;
	}

}


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

	i = sizeof( Q3_VERSION )/sizeof(char) - 1;
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


void Con_DrawConsole()
{
	// check for console width changes from a vid mode change
	Con_CheckResize();

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
			Con_DrawNotify();
		}
	}
}


///////////////////////////////////////////////////////////////


// slide the console onto/off the screen

void Con_RunConsole()
{
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


static const int CON_PAGELINES = 4;

void Con_PageUp()
{
	con.display -= CON_PAGELINES;
	if ( con.current - con.display >= con.totallines ) {
		con.display = con.current - con.totallines + 1;
	}
}

void Con_PageDown()
{
	con.display += CON_PAGELINES;
	if (con.display > con.current) {
		con.display = con.current;
	}
}


void Con_Top()
{
	con.display = con.totallines;
	if ( con.current - con.display >= con.totallines ) {
		con.display = con.current - con.totallines + 1;
	}
}

void Con_Bottom()
{
	con.display = con.current;
}


void Con_Close()
{
	if ( !com_cl_running->integer ) {
		return;
	}
	Field_Clear( &g_consoleField );
	Con_ClearNotify();
	cls.keyCatchers &= ~KEYCATCH_CONSOLE;
	con.finalFrac = 0;				// none visible
	con.displayFrac = 0;
}
