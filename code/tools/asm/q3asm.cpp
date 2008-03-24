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

#include <hash_map>

struct VanillaStringCmp
{
	// parameters for hash table
	enum { bucket_size = 4, min_buckets = 8 };

	size_t operator()( const char* s ) const // X31 hash
	{
		size_t h = 0;
		const unsigned char* p = (const unsigned char*)s;
		while (*p)
			h = (h << 5) - h + (*p++);
		return h;
	}

	bool operator()( const char* lhs, const char* rhs) const
	{
		return (strcmp(lhs, rhs) < 0);
	}
};

#include "../../qcommon/vm_local.h"

extern "C" {
#include "cmdlib.h"
};


struct SourceOp {
	const char* name;
	int opcode;
};

static const SourceOp aSourceOps[] = {
	#include "opstrings.h"
};

typedef stdext::hash_map< const char*, int, VanillaStringCmp > OpTable;
OpTable aOpTable;


char	outputFilename[MAX_OS_PATH];

// the zero page size is just used for detecting run time faults
#define	ZERO_PAGE_SIZE	0		// 256

typedef struct {
	int		imageBytes;		// after decompression
	int		entryPoint;
	int		stackBase;
	int		stackSize;
} executableHeader_t;

typedef enum {
	CODESEG,
	DATASEG,	// initialized 32 bit data, will be byte swapped
	LITSEG,		// strings
	BSSSEG,		// 0 filled
	JTRGSEG,	// psuedo-segment that contains only jump table targets
	NUM_SEGMENTS
} segmentName_t;

#define	MAX_IMAGE	0x400000

typedef struct {
	byte	image[MAX_IMAGE];
	int		imageUsed;
	int		segmentBase;		// only valid on second pass
} segment_t;

typedef struct {
	const segment_t* segment;
	int value;
} symbol_t;

typedef stdext::hash_map< const char*, symbol_t*, VanillaStringCmp > SymTable;
SymTable aSymTable;


segment_t	segment[NUM_SEGMENTS];
segment_t	*currentSegment;

int		passNumber;

int		numSymbols;
int		errorCount;

typedef struct options_s {
	qboolean verbose;
	qboolean writeMapFile;
} options_t;

static options_t options;

symbol_t	*symbols;
symbol_t	*lastSymbol = 0;  /* Most recent symbol defined. */


#define	MAX_ASM_FILES	256
int		numAsmFiles;
char	*asmFiles[MAX_ASM_FILES];
char	*asmFileNames[MAX_ASM_FILES];

int		currentFileIndex;
char	*currentFileName;
int		currentFileLine;

int		stackSize = 0x10000;

// we need to convert arg and ret instructions to
// stores to the local stack frame, so we need to track the
// characteristics of the current functions stack frame
int		currentLocals;			// bytes of locals needed by this function
int		currentArgs;			// bytes of largest argument list called from this function
int		currentArgOffset;		// byte offset in currentArgs to store next arg, reset each call

#define	MAX_LINE_LENGTH	1024
char	lineBuffer[MAX_LINE_LENGTH];
int		lineParseOffset;
char	token[MAX_LINE_LENGTH];

int		instructionCount;


int
vreport (const char* fmt, va_list vp)
{
  if (options.verbose != qtrue)
      return 0;
  return vprintf(fmt, vp);
}

int
report (const char *fmt, ...)
{
  va_list va;
  int retval;

  va_start(va, fmt);
  retval = vreport(fmt, va);
  va_end(va);
  return retval;
}


#ifdef _MSC_VER
#define INT64 __int64
#define atoi64 _atoi64
#else
#define INT64 long long int
#define atoi64 atoll
#endif

/*
 Problem:
	BYTE values are specified as signed decimal string.  A properly functional
	atoip() will cap large signed values at 0x7FFFFFFF.  Negative word values are
	often specified as very large decimal values by lcc.  Therefore, values that
	should be between 0x7FFFFFFF and 0xFFFFFFFF come out as 0x7FFFFFFF when using
	atoi().  Bad.

 This function is one big evil hack to work around this problem.
*/
int atoiNoCap (const char *s)
{
  INT64 l;
  union {
    unsigned int u;
    signed int i;
  } retval;

  l = atoi64(s);
  /* Now smash to signed 32 bits accordingly. */
  if (l < 0) {
    retval.i = (int)l;
  } else {
    retval.u = (unsigned int)l;
  }
  return retval.i;  /* <- union hackage.  I feel dirty with this.  -PH */
}


/*
============
CodeError
============
*/
void CodeError( char *fmt, ... ) {
	va_list		argptr;

	errorCount++;

	report( "%s:%i ", currentFileName, currentFileLine );

	va_start( argptr,fmt );
	vprintf( fmt,argptr );
	va_end( argptr );
}

/*
============
EmitByte
============
*/
void EmitByte( segment_t *seg, int v ) {
	if ( seg->imageUsed >= MAX_IMAGE ) {
		Error( "MAX_IMAGE" );
	}
	seg->image[ seg->imageUsed ] = v;
	seg->imageUsed++;
}

/*
============
EmitInt
============
*/
void EmitInt( segment_t *seg, int v ) {
	if ( seg->imageUsed >= MAX_IMAGE - 4) {
		Error( "MAX_IMAGE" );
	}
	seg->image[ seg->imageUsed ] = v & 255;
	seg->image[ seg->imageUsed + 1 ] = ( v >> 8 ) & 255;
	seg->image[ seg->imageUsed + 2 ] = ( v >> 16 ) & 255;
	seg->image[ seg->imageUsed + 3 ] = ( v >> 24 ) & 255;
	seg->imageUsed += 4;
}


static void DefineSymbol( const char* symbol, int value )
{
	// symbols can only be defined on pass 0
	if ( passNumber == 1 )
		return;

	// add the file prefix to local symbols to guarantee uniqueness
	// !!! note that this doesn't fucking work, and everything is a global
	char expanded[MAX_LINE_LENGTH];
	if ( symbol[0] == '$' ) {
		sprintf( expanded, "%s_%i", symbol, currentFileIndex );
		symbol = expanded;
	}

	SymTable::const_iterator it = aSymTable.find( symbol );

	if (it != aSymTable.end()) {
		CodeError( "Multiple definitions for %s\n", symbol );
		return;
	}

	const char* name = copystring( symbol );
	symbol_t* s = new symbol_t;
	s->segment = currentSegment;
	s->value = value;
	aSymTable[ name ] = s;

	lastSymbol = s;
}


static int LookupSymbol( const char* symbol )
{
	// symbols can only be evaluated on pass 1
	if ( passNumber == 0 )
		return 0;

	// add the file prefix to local symbols to guarantee uniqueness
	// !!! note that this doesn't fucking work, and everything is a global
	char expanded[MAX_LINE_LENGTH];
	if ( symbol[0] == '$' ) {
		sprintf( expanded, "%s_%i", symbol, currentFileIndex );
		symbol = expanded;
	}

	SymTable::const_iterator it = aSymTable.find( symbol );

	if (it == aSymTable.end()) {
		CodeError( "Symbol %s undefined\n", symbol );
		return 0;
	}

	const symbol_t* s = (*it).second;
	return (s->segment->segmentBase + s->value);
}


/*
==============
ExtractLine

Extracts the next line from the given text block.
If a full line isn't parsed, returns NULL
Otherwise returns the updated parse pointer
===============
*/
char *ExtractLine( char *data ) {
/* Goal:
	 Given a string `data', extract one text line into buffer `lineBuffer' that
	 is no longer than MAX_LINE_LENGTH characters long.  Return value is
	 remainder of `data' that isn't part of `lineBuffer'.
 -PH
*/
	/* Hand-optimized by PhaethonH */
	char 	*p, *q;

	currentFileLine++;

	lineParseOffset = 0;
	token[0] = 0;
	*lineBuffer = 0;

	p = q = data;
	if (!*q) {
		return NULL;
	}

	for ( ; !((*p == 0) || (*p == '\n')); p++)  /* nop */ ;

	if ((p - q) >= MAX_LINE_LENGTH) {
		CodeError( "MAX_LINE_LENGTH" );
		return data;
	}

	memcpy( lineBuffer, data, (p - data) );
	lineBuffer[(p - data)] = 0;
	p += (*p == '\n') ? 1 : 0;  /* Skip over final newline. */
	return p;
}


/*
==============
Parse

Parse a token out of linebuffer
==============
*/
qboolean Parse( void ) {
	/* Hand-optimized by PhaethonH */
	const char 	*p, *q;

	/* Because lineParseOffset is only updated just before exit, this makes this code version somewhat harder to debug under a symbolic debugger. */

	*token = 0;  /* Clear token. */

	// skip whitespace
	for (p = lineBuffer + lineParseOffset; *p && (*p <= ' '); p++) /* nop */ ;

	// skip ; comments
	/* die on end-of-string */
	if ((*p == ';') || (*p == 0)) {
		lineParseOffset = p - lineBuffer;
		return qfalse;
	}

	q = p;  /* Mark the start of token. */
	/* Find separator first. */
	for ( ; *p > 32; p++) /* nop */ ;  /* XXX: unsafe assumptions. */
	/* *p now sits on separator.  Mangle other values accordingly. */
	strncpy(token, q, p - q);
	token[p - q] = 0;

	lineParseOffset = p - lineBuffer;

	return qtrue;
}


/*
==============
ParseValue
==============
*/
int	ParseValue( void ) {
	Parse();
	return atoiNoCap( token );
}


/*
==============
ParseExpression
==============
*/
int	ParseExpression(void) {
	/* Hand optimization, PhaethonH */
	int		i, j;
	char	sym[MAX_LINE_LENGTH];
	int		v;

	/* Skip over a leading minus. */
	for ( i = ((token[0] == '-') ? 1 : 0) ; i < MAX_LINE_LENGTH ; i++ ) {
		if ( token[i] == '+' || token[i] == '-' || token[i] == 0 ) {
			break;
		}
	}

	memcpy( sym, token, i );
	sym[i] = 0;

	switch (*sym) {  /* Resolve depending on first character. */
/* Optimizing compilers can convert cases into "calculated jumps".  I think these are faster.  -PH */
		case '-':
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			v = atoiNoCap(sym);
			break;
		default:
			v = LookupSymbol(sym);
			break;
	}

	// parse add / subtract offsets
	while ( token[i] != 0 ) {
		for ( j = i + 1 ; j < MAX_LINE_LENGTH ; j++ ) {
			if ( token[j] == '+' || token[j] == '-' || token[j] == 0 ) {
				break;
			}
		}

		memcpy( sym, token+i+1, j-i-1 );
		sym[j-i-1] = 0;

		switch (token[i]) {
			case '+':
				v += atoiNoCap(sym);
				break;
			case '-':
				v -= atoiNoCap(sym);
				break;
		}

		i = j;
	}

	return v;
}


/*
==============
HackToSegment

BIG HACK: I want to put all 32 bit values in the data
segment so they can be byte swapped, and all char data in the lit
segment, but switch jump tables are emited in the lit segment and
initialized strng variables are put in the data segment.

I can change segments here, but I also need to fixup the
label that was just defined

Note that the lit segment is read-write in the VM, so strings
aren't read only as in some architectures.
==============
*/
void HackToSegment( segmentName_t seg ) {
	if ( currentSegment == &segment[seg] ) {
		return;
	}

	currentSegment = &segment[seg];
	if ( passNumber == 0 ) {
		lastSymbol->segment = currentSegment;
		lastSymbol->value = currentSegment->imageUsed;
	}
}







//#define STAT(L) report("STAT " L "\n");
#define STAT(L)
#define ASM(O) int TryAssemble##O ()


/*
  These clauses were moved out from AssembleLine() to allow reordering of if's.
  An optimizing compiler should reconstruct these back into inline code.
 -PH
*/

	// call instructions reset currentArgOffset
ASM(CALL)
{
	if ( !strncmp( token, "CALL", 4 ) ) {
STAT("CALL");
		EmitByte( &segment[CODESEG], OP_CALL );
		instructionCount++;
		currentArgOffset = 0;
		return 1;
	}
	return 0;
}

	// arg is converted to a reversed store
ASM(ARG)
{
	if ( !strncmp( token, "ARG", 3 ) ) {
STAT("ARG");
		EmitByte( &segment[CODESEG], OP_ARG );
		instructionCount++;
		if ( 8 + currentArgOffset >= 256 ) {
			CodeError( "currentArgOffset >= 256" );
			return 1;
		}
		EmitByte( &segment[CODESEG], 8 + currentArgOffset );
		currentArgOffset += 4;
		return 1;
	}
	return 0;
}

	// ret just leaves something on the op stack
ASM(RET)
{
	if ( !strncmp( token, "RET", 3 ) ) {
STAT("RET");
		EmitByte( &segment[CODESEG], OP_LEAVE );
		instructionCount++;
		EmitInt( &segment[CODESEG], 8 + currentLocals + currentArgs );
		return 1;
	}
	return 0;
}

	// pop is needed to discard the return value of 
	// a function
ASM(POP)
{
	if ( !strncmp( token, "pop", 3 ) ) {
STAT("POP");
		EmitByte( &segment[CODESEG], OP_POP );
		instructionCount++;
		return 1;
	}
	return 0;
}

	// address of a parameter is converted to OP_LOCAL
ASM(ADDRF)
{
	int		v;
	if ( !strncmp( token, "ADDRF", 5 ) ) {
STAT("ADDRF");
		instructionCount++;
		Parse();
		v = ParseExpression();
		v = 16 + currentArgs + currentLocals + v;
		EmitByte( &segment[CODESEG], OP_LOCAL );
		EmitInt( &segment[CODESEG], v );
		return 1;
	}
	return 0;
}

	// address of a local is converted to OP_LOCAL
ASM(ADDRL)
{
	int		v;
	if ( !strncmp( token, "ADDRL", 5 ) ) {
STAT("ADDRL");
		instructionCount++;
		Parse();
		v = ParseExpression();
		v = 8 + currentArgs + v;
		EmitByte( &segment[CODESEG], OP_LOCAL );
		EmitInt( &segment[CODESEG], v );
		return 1;
	}
	return 0;
}

ASM(PROC)
{
	char	name[1024];
	if ( !strcmp( token, "proc" ) ) {
STAT("PROC");
		Parse();					// function name
		strcpy( name, token );

		DefineSymbol( token, instructionCount ); // segment[CODESEG].imageUsed );

		currentLocals = ParseValue();	// locals
		currentLocals = ( currentLocals + 3 ) & ~3;
		currentArgs = ParseValue();		// arg marshalling
		currentArgs = ( currentArgs + 3 ) & ~3;

		if ( 8 + currentLocals + currentArgs >= 32767 ) {
			CodeError( "Locals > 32k in %s\n", name );
		}

		instructionCount++;
		EmitByte( &segment[CODESEG], OP_ENTER );
		EmitInt( &segment[CODESEG], 8 + currentLocals + currentArgs );
		return 1;
	}
	return 0;
}


ASM(ENDPROC)
{
	int		v, v2;
	if ( !strcmp( token, "endproc" ) ) {
STAT("ENDPROC");
		Parse();				// skip the function name
		v = ParseValue();		// locals
		v2 = ParseValue();		// arg marshalling

		// all functions must leave something on the opstack
		instructionCount++;
		EmitByte( &segment[CODESEG], OP_PUSH );

		instructionCount++;
		EmitByte( &segment[CODESEG], OP_LEAVE );
		EmitInt( &segment[CODESEG], 8 + currentLocals + currentArgs );

		return 1;
	}
	return 0;
}


ASM(ADDRESS)
{
	int		v;
	if ( !strcmp( token, "address" ) ) {
STAT("ADDRESS");
		Parse();
		v = ParseExpression();

/* Addresses are 32 bits wide, and therefore go into data segment. */
		HackToSegment( DATASEG );
		EmitInt( currentSegment, v );
		if( passNumber == 1 && token[ 0 ] == '$' ) // crude test for labels
			EmitInt( &segment[ JTRGSEG ], v );
		return 1;
	}
	return 0;
}

ASM(EXPORT)
{
	if ( !strcmp( token, "export" ) ) {
STAT("EXPORT");
		return 1;
	}
	return 0;
}

ASM(IMPORT)
{
	if ( !strcmp( token, "import" ) ) {
STAT("IMPORT");
		return 1;
	}
	return 0;
}

ASM(CODE)
{
	if ( !strcmp( token, "code" ) ) {
STAT("CODE");
		currentSegment = &segment[CODESEG];
		return 1;
	}
	return 0;
}

ASM(BSS)
{
	if ( !strcmp( token, "bss" ) ) {
STAT("BSS");
		currentSegment = &segment[BSSSEG];
		return 1;
	}
	return 0;
}

ASM(DATA)
{
	if ( !strcmp( token, "data" ) ) {
STAT("DATA");
		currentSegment = &segment[DATASEG];
		return 1;
	}
	return 0;
}

ASM(LIT)
{
	if ( !strcmp( token, "lit" ) ) {
STAT("LIT");
		currentSegment = &segment[LITSEG];
		return 1;
	}
	return 0;
}

ASM(LINE)
{
	if ( !strcmp( token, "line" ) ) {
STAT("LINE");
		return 1;
	}
	return 0;
}

ASM(FILE)
{
	if ( !strcmp( token, "file" ) ) {
STAT("FILE");
		return 1;
	}
	return 0;
}

ASM(EQU)
{
	char	name[1024];
	if ( !strcmp( token, "equ" ) ) {
STAT("EQU");
		Parse();
		strcpy( name, token );
		Parse();
		DefineSymbol( name, atoiNoCap(token) );
		return 1;
	}
	return 0;
}

ASM(ALIGN)
{
	int		v;
	if ( !strcmp( token, "align" ) ) {
STAT("ALIGN");
		v = ParseValue();
		currentSegment->imageUsed = (currentSegment->imageUsed + v - 1 ) & ~( v - 1 );
		return 1;
	}
	return 0;
}

ASM(SKIP)
{
	int		v;
	if ( !strcmp( token, "skip" ) ) {
STAT("SKIP");
		v = ParseValue();
		currentSegment->imageUsed += v;
		return 1;
	}
	return 0;
}

ASM(BYTE)
{
	int		i, v, v2;
	if ( !strcmp( token, "byte" ) ) {
STAT("BYTE");
		v = ParseValue();
		v2 = ParseValue();

		if ( v == 1 ) {
/* Character (1-byte) values go into lit(eral) segment. */
			HackToSegment( LITSEG );
		} else if ( v == 4 ) {
/* 32-bit (4-byte) values go into data segment. */
			HackToSegment( DATASEG );
		} else if ( v == 2 ) {
/* and 16-bit (2-byte) values will cause q3asm to barf. */
			CodeError( "16 bit initialized data not supported" );
		}

		// emit little endien
		for ( i = 0 ; i < v ; i++ ) {
			EmitByte( currentSegment, (v2 & 0xFF) ); /* paranoid ANDing  -PH */
			v2 >>= 8;
		}
		return 1;
	}
	return 0;
}

	// code labels are emited as instruction counts, not byte offsets,
	// because the physical size of the code will change with
	// different run time compilers and we want to minimize the
	// size of the required translation table
ASM(LABEL)
{
	if ( !strncmp( token, "LABEL", 5 ) ) {
STAT("LABEL");
		Parse();
		if ( currentSegment == &segment[CODESEG] ) {
			DefineSymbol( token, instructionCount );
		} else {
			DefineSymbol( token, currentSegment->imageUsed );
		}
		return 1;
	}
	return 0;
}


static void AssembleLine()
{
	Parse();
	if ( !token[0] )
		return;

	OpTable::const_iterator it = aOpTable.find( token );

	if (it != aOpTable.end()) {
		int opcode = (*it).second;

		if ( opcode == OP_UNDEF ) {
			CodeError( "Undefined opcode: %s\n", token );
		}

		if ( opcode == OP_IGNORE ) {
			return;		// we ignore most conversions
		}

		// sign extensions need to check next parm
		if ( opcode == OP_SEX8 ) {
			Parse();
			if ( token[0] == '1' ) {
				opcode = OP_SEX8;
			} else if ( token[0] == '2' ) {
				opcode = OP_SEX16;
			} else {
				CodeError( "Bad sign extension: %s\n", token );
				return;
			}
		}

		// check for expression
		Parse();

		if ( token[0] && opcode != OP_CVIF && opcode != OP_CVFI ) {
			// code like this can generate non-dword block copies:
			// auto char buf[2] = " ";
			// we are just going to round up.  This might conceivably
			// be incorrect if other initialized chars follow.
			int expression = ParseExpression();
			if ( opcode == OP_BLOCK_COPY ) {
				expression = ( expression + 3 ) & ~3;
			}
			EmitByte( &segment[CODESEG], opcode );
			EmitInt( &segment[CODESEG], expression );
		} else {
			EmitByte( &segment[CODESEG], opcode );
		}

		instructionCount++;
		return;
	}

/* These should be sorted in sequence of statistical frequency, most frequent first.  -PH

Empirical frequency statistics from FI 2001.01.23:
 109892	STAT ADDRL
  72188	STAT BYTE
  51150	STAT LINE
  50906	STAT ARG
  43704	STAT IMPORT
  34902	STAT LABEL
  32066	STAT ADDRF
  23704	STAT CALL
   7720	STAT POP
   7256	STAT RET
   5198	STAT ALIGN
   3292	STAT EXPORT
   2878	STAT PROC
   2878	STAT ENDPROC
   2812	STAT ADDRESS
    738	STAT SKIP
    374	STAT EQU
    280	STAT CODE
    176	STAT LIT
    102	STAT FILE
    100	STAT BSS
     68	STAT DATA

*/

#undef ASM
#define ASM(O) if (TryAssemble##O ()) return;

	ASM(ADDRL)
	ASM(BYTE)
	ASM(LINE)
	ASM(ARG)
	ASM(IMPORT)
	ASM(LABEL)
	ASM(ADDRF)
	ASM(CALL)
	ASM(POP)
	ASM(RET)
	ASM(ALIGN)
	ASM(EXPORT)
	ASM(PROC)
	ASM(ENDPROC)
	ASM(ADDRESS)
	ASM(SKIP)
	ASM(EQU)
	ASM(CODE)
	ASM(LIT)
	ASM(FILE)
	ASM(BSS)
	ASM(DATA)

	CodeError( "Unknown token: %s\n", token );
}


static void InitTables()
{
	for (int i = 0; aSourceOps[i].name; ++i) {
		aOpTable[ aSourceOps[i].name ] = aSourceOps[i].opcode;
	}
}


static void WriteMapFile()
{
/*
	FILE		*f;
	symbol_t	*s;
	char		imageName[MAX_OS_PATH];
	int			seg;

	strcpy( imageName, outputFilename );
	StripExtension( imageName );
	strcat( imageName, ".map" );

	report( "Writing %s...\n", imageName );

	f = SafeOpenWrite( imageName );
	for ( seg = CODESEG ; seg <= BSSSEG ; seg++ ) {
		for ( s = symbols ; s ; s = s->next ) {
			if ( s->name[0] == '$' ) {
				continue;	// skip locals
			}
			if ( &segment[seg] != s->segment ) {
				continue;
			}
			fprintf( f, "%i %8x %s\n", seg, s->value, s->name );
		}
	}
	fclose( f );
*/
}


/*
===============
WriteVmFile
===============
*/
void WriteVmFile( void ) {
	char	imageName[MAX_OS_PATH];
	vmHeader_t	header;
	FILE	*f;
	int		headerSize;

	report( "%i total errors\n", errorCount );

	strcpy( imageName, outputFilename );
	StripExtension( imageName );
	strcat( imageName, ".qvm" );

	remove( imageName );

	report( "code segment: %7i\n", segment[CODESEG].imageUsed );
	report( "data segment: %7i\n", segment[DATASEG].imageUsed );
	report( "lit  segment: %7i\n", segment[LITSEG].imageUsed );
	report( "bss  segment: %7i\n", segment[BSSSEG].imageUsed );
	report( "instruction count: %i\n", instructionCount );

	if ( errorCount != 0 ) {
		report( "Not writing a file due to errors\n" );
		return;
	}

	header.vmMagic = VM_MAGIC;
	// Don't write the VM_MAGIC_VER2 bits when maintaining 1.32b compatibility.
	// (I know this isn't strictly correct due to padding, but then platforms
	// that pad wouldn't be able to write a correct header anyway).  Note: if
	// vmHeader_t changes, this needs to be adjusted too.
	headerSize = sizeof( header ) - sizeof( header.jtrgLength );

	header.instructionCount = instructionCount;
	header.codeOffset = headerSize;
	header.codeLength = segment[CODESEG].imageUsed;
	header.dataOffset = header.codeOffset + segment[CODESEG].imageUsed;
	header.dataLength = segment[DATASEG].imageUsed;
	header.litLength = segment[LITSEG].imageUsed;
	header.bssLength = segment[BSSSEG].imageUsed;
	header.jtrgLength = segment[JTRGSEG].imageUsed;

	report( "Writing to %s\n", imageName );

	CreatePath( imageName );
	f = SafeOpenWrite( imageName );
	SafeWrite( f, &header, headerSize );
	SafeWrite( f, &segment[CODESEG].image, segment[CODESEG].imageUsed );
	SafeWrite( f, &segment[DATASEG].image, segment[DATASEG].imageUsed );
	SafeWrite( f, &segment[LITSEG].image, segment[LITSEG].imageUsed );
	fclose( f );
}

/*
===============
Assemble
===============
*/
void Assemble( void ) {
	int		i;
	char	filename[MAX_OS_PATH];
	char		*ptr;

	report( "outputFilename: %s\n", outputFilename );

	for ( i = 0 ; i < numAsmFiles ; i++ ) {
		strcpy( filename, asmFileNames[ i ] );
		DefaultExtension( filename, ".asm" );
		LoadFile( filename, (void **)&asmFiles[i] );
	}

	// assemble
	for ( passNumber = 0 ; passNumber < 2 ; passNumber++ ) {
		segment[LITSEG].segmentBase = segment[DATASEG].imageUsed;
		segment[BSSSEG].segmentBase = segment[LITSEG].segmentBase + segment[LITSEG].imageUsed;
		segment[JTRGSEG].segmentBase = segment[BSSSEG].segmentBase + segment[BSSSEG].imageUsed;
		for ( i = 0 ; i < NUM_SEGMENTS ; i++ ) {
			segment[i].imageUsed = 0;
		}
		segment[DATASEG].imageUsed = 4;		// skip the 0 byte, so NULL pointers are fixed up properly
		instructionCount = 0;

		for ( i = 0 ; i < numAsmFiles ; i++ ) {
			currentFileIndex = i;
			currentFileName = asmFileNames[ i ];
			currentFileLine = 0;
			report("pass %i: %s\n", passNumber, currentFileName );
			fflush( NULL );
			ptr = asmFiles[i];
			while ( ptr ) {
				ptr = ExtractLine( ptr );
				AssembleLine();
			}
		}

		// align all the segments
		for ( i = 0 ; i < NUM_SEGMENTS ; i++ ) {
			segment[i].imageUsed = (segment[i].imageUsed + 3) & ~3;
		}
	}

	// reserve the stack in bss
	DefineSymbol( "_stackStart", segment[BSSSEG].imageUsed );
	segment[BSSSEG].imageUsed += stackSize;
	DefineSymbol( "_stackEnd", segment[BSSSEG].imageUsed );

	// write the image
	WriteVmFile();

	// write the map file even if there were errors
	if( options.writeMapFile ) {
		WriteMapFile();
	}
}


/*
=============
ParseOptionFile

=============
*/
void ParseOptionFile( const char *filename ) {
	char		expanded[MAX_OS_PATH];
	char		*text, *text_p;

	strcpy( expanded, filename );
	DefaultExtension( expanded, ".q3asm" );
	LoadFile( expanded, (void **)&text );
	if ( !text ) {
		return;
	}

	text_p = text;

	while( ( text_p = ASM_Parse( text_p ) ) != 0 ) {
		if ( !strcmp( com_token, "-o" ) ) {
			// allow output override in option file
			text_p = ASM_Parse( text_p );
			if ( text_p ) {
				strcpy( outputFilename, com_token );
			}
			continue;
		}

		asmFileNames[ numAsmFiles ] = copystring( com_token );
		numAsmFiles++;
	}
}

/*
==============
main
==============
*/
int main( int argc, char **argv ) {
	int			i;
	double		start, end;

	if ( argc < 2 ) {
		Error("Usage: %s [OPTION]... [FILES]...\n\
Assemble LCC bytecode assembly to Q3VM bytecode.\n\
\n\
    -o OUTPUT      Write assembled output to file OUTPUT.qvm\n\
    -f LISTFILE    Read options and list of files to assemble from LISTFILE\n\
    -v             Verbose compilation report\n\
", argv[0]);
	}

	start = I_FloatTime ();

	// default filename is "q3asm"
	strcpy( outputFilename, "q3asm" );
	numAsmFiles = 0;	

	for ( i = 1 ; i < argc ; i++ ) {
		if ( argv[i][0] != '-' ) {
			break;
		}
		if ( !strcmp( argv[i], "-o" ) ) {
			if ( i == argc - 1 ) {
				Error( "-o must preceed a filename" );
			}
/* Timbo of Tremulous pointed out -o not working; stock ID q3asm folded in the change. Yay. */
			strcpy( outputFilename, argv[ i+1 ] );
			i++;
			continue;
		}

		if ( !strcmp( argv[i], "-f" ) ) {
			if ( i == argc - 1 ) {
				Error( "-f must preceed a filename" );
			}
			ParseOptionFile( argv[ i+1 ] );
			i++;
			continue;
		}

		if( !strcmp( argv[ i ], "-v" ) ) {
/* Verbosity option added by Timbo, 2002.09.14.
By default (no -v option), q3asm remains silent except for critical errors.
Verbosity turns on all messages, error or not.
Motivation: not wanting to scrollback for pages to find asm error.
*/
			options.verbose = qtrue;
			continue;
		}

		if( !strcmp( argv[ i ], "-m" ) ) {
			options.writeMapFile = qtrue;
			continue;
		}

		Error( "Unknown option: %s", argv[i] );
	}

	// the rest of the command line args are asm files
	for ( ; i < argc ; i++ ) {
		asmFileNames[ numAsmFiles ] = copystring( argv[ i ] );
		numAsmFiles++;
	}

	options.verbose = qtrue;

	InitTables();
	Assemble();

	end = I_FloatTime ();
	report ("%5.0f seconds elapsed\n", end-start);

	return errorCount;
}

