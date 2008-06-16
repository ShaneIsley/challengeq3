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
	int		y;				// virtual screen coordinate of bottom of console
	int		display;		// bottom of console displays this line

	float	cw, ch;
	float	xadjust;		// supposedly for wide aspect screens, but never actually set properly

	int 	linewidth;		// characters across screen
	int		totallines;		// total lines in console scrollback

	float	displayFrac;	// aproaches finalFrac at scr_conspeed
	float	finalFrac;		// 0.0 to 1.0 lines of console to display

	int		times[CON_NOTIFYLINES];	// cls.realtime time the line was generated
								// for transparent notify lines
} console_t;

static console_t con;

#define CONCHAR_WIDTH	8
#define CONCHAR_HEIGHT	12
#define CONSOLE_WIDTH	78
int g_console_field_width = CONSOLE_WIDTH;


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
	int i;

	for ( i = 0 ; i < CON_TEXTSIZE ; i++ ) {
		con.text[i] = (COLOR_WHITE << 8) | ' ';
	}

	Con_Bottom();
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


// called on demand by the first CL_ConsolePrint
// before the client subsystem is actually up and running properly

static void Con_Init()
{
	con.initialized = qtrue;

	con.linewidth = CONSOLE_WIDTH;
	con.totallines = CON_TEXTSIZE / con.linewidth;
	con.current = con.totallines - 1;
	Con_Clear_f();

	con.cw = CONCHAR_WIDTH;
	con.ch = CONCHAR_HEIGHT;
}


static void Con_ResizeFont()
{
	if (!cls.rendererStarted)
		return;

	con.cw = CONCHAR_WIDTH;
	con.ch = CONCHAR_HEIGHT;
	SCR_AdjustFrom640( &con.cw, &con.ch, NULL, NULL );
	con.xadjust = con.cw;
}


void CL_ConInit()
{
	int i;

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
	Con_ClearNotify();

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
		con.text[(con.current%con.totallines)*con.linewidth+i] = (COLOR_WHITE << 8) | ' ';
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

	// cl_noprint disables ALL console functionality
	// use con_notifytime 0 to log the text without the annoying overlay
	if ( cl_noprint && cl_noprint->integer ) {
		return;
	}

	if (!con.initialized)
		Con_Init();

	char color = COLOR_WHITE;

	while ( c = *s ) {
		if ( Q_IsColorString( s ) ) {
			color = s[1];
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
	if ( cls.state != CA_DISCONNECTED && !(cls.keyCatchers & KEYCATCH_CONSOLE ) ) {
		return;
	}

	float y = con.y - (con.ch * 1.5);

	re.SetColor( colorBlack );
	SCR_DrawChar( con.xadjust + 1, y + 1, con.cw, con.ch, ']' );
	re.SetColor( colorWhite );
	SCR_DrawChar( con.xadjust, y, con.cw, con.ch, ']' );

	Field_Draw( &g_consoleField, con.xadjust + con.cw, y, con.cw, con.ch );
}


// draws the last few lines of output transparently over the game area

static void Con_DrawNotify()
{
	int		x;
	short	*text;
	int		i;
	int		time;
	int		skip;

	char color = COLOR_WHITE;
	re.SetColor( ColorFromChar( color ) );

	int y = 0;
	for (i = con.current-CON_NOTIFYLINES+1 ; i <= con.current ; i++)
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
			if ( (text[x] >> 8) != color ) {
				color = (text[x] >> 8);
				re.SetColor( ColorFromChar( color ) );
			}
			SCR_DrawChar( cl_conXOffset->integer + con.xadjust + (x+1)*con.cw, y, con.cw, con.ch, text[x] & 0xff );
		}

		y += con.ch;
	}

	re.SetColor( NULL );

	if (cls.keyCatchers & (KEYCATCH_UI | KEYCATCH_CGAME)) {
		return;
	}

	// draw the chat line - this shit has NO business being in the engine >:(
	if ( cls.keyCatchers & KEYCATCH_MESSAGE )
	{
		if (chat_team)
		{
			SCR_DrawString( 8, y, BIGCHAR_WIDTH, BIGCHAR_HEIGHT, "say_team:", qtrue );
			skip = 10;
		}
		else
		{
			SCR_DrawString( 8, y, BIGCHAR_WIDTH, BIGCHAR_HEIGHT, "say:", qtrue );
			skip = 5;
		}

		Field_Draw( &chatField, skip * BIGCHAR_WIDTH, y, BIGCHAR_WIDTH, BIGCHAR_HEIGHT );
		y += BIGCHAR_HEIGHT;
	}

}


///////////////////////////////////////////////////////////////


static void Con_FillRect( float x, float y, float w, float h, const vec4_t color )
{
	re.SetColor( color );
	SCR_AdjustFrom640( &x, &y, &w, &h );
	re.DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );
}


static void Con_DrawSolidConsole( float frac )
{
	int		i, x, y;
	int		rows;
	int		row;
	vec4_t	fill;

	int scanlines = Com_Clamp( 0, cls.glconfig.vidHeight, cls.glconfig.vidHeight * frac );
	if (scanlines <= 0)
		return;

	// draw the background
	y = frac * SCREEN_HEIGHT - 2;
	MAKERGBA( fill, 0.44f, 0.44f, 0.44f, 1.0 );
	Con_FillRect( 0, 0, SCREEN_WIDTH, y, fill );

	MAKERGBA( fill, 0.33f, 0.33f, 0.33f, 1.0 );
	Con_FillRect( 0, y, SCREEN_WIDTH, 2, fill );

	i = sizeof( Q3_VERSION )/sizeof(char) - 1;
	x = cls.glconfig.vidWidth;
	while (--i >= 0) {
		x -= SMALLCHAR_WIDTH;
		SCR_DrawChar( x, scanlines - (SMALLCHAR_HEIGHT * 1.5), SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, Q3_VERSION[i] );
	}

	re.SetColor( NULL );
	rows = (scanlines - con.ch) / con.ch;
	con.y = scanlines;

	y = scanlines - (con.ch * 3);

	// draw the console text from the bottom up
	if (con.display != con.current) {
		// draw arrows to show the buffer is backscrolled
		re.SetColor( colorBlack );
		for (x = 0; x < con.linewidth; x += 4)
			SCR_DrawChar( con.xadjust + (x * con.cw), y, con.cw, con.ch, '^' );
		y -= con.ch;
		--rows;
	}

	row = con.display;
	if ( con.x == 0 ) {
		row--;
	}

	char color = COLOR_WHITE;
	re.SetColor( ColorFromChar( color ) );

	for (i = 0; i < rows; ++i, --row, y -= con.ch )
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
			SCR_DrawChar( 1 + con.xadjust + i * con.cw, 1 + y, con.cw, con.ch, (text[i] & 0xFF) );
		}

		re.SetColor( colorWhite );
		for (int i = 0; i < con.linewidth; ++i) {
			if ((text[i] >> 8) != color) {
				color = (text[i] >> 8);
				re.SetColor( ColorFromChar( color ) );
			}
			SCR_DrawChar( con.xadjust + i * con.cw, y, con.cw, con.ch, (text[i] & 0xFF) );
		}
	}

	Con_DrawInput();

	re.SetColor( NULL );
}


///////////////////////////////////////////////////////////////


void Con_DrawConsole()
{
	// check for console width changes from a vid mode change
	Con_ResizeFont();

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
		con.finalFrac = 0;			// none visible

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
