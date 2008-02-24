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

/*****************************************************************************
 * name:		snd_dma.c
 *
 * desc:		main control for any streaming sound output device
 *
 * $Archive: /MissionPack/code/client/snd_dma.c $
 *
 *****************************************************************************/

#include "snd_local.h"
#include "snd_codec.h"
#include "client.h"


static void S_Update_DMA();
static void S_UpdateBackgroundTrack();
static void S_Base_StopBackgroundTrack();

static snd_stream_t* s_backgroundStream = NULL;
static char s_backgroundLoop[MAX_QPATH];


// =======================================================================
// Internal sound data & structures
// =======================================================================

// only begin attenuating sound volumes when outside the FULLVOLUME range
static const float SOUND_FULLVOLUME = 80;

static const float SOUND_MAX_DIST = 1250;
static const float SOUND_ATTENUATE = (1.0f / SOUND_MAX_DIST);

channel_t	s_channels[MAX_CHANNELS];
channel_t	loop_channels[MAX_CHANNELS];
int			numLoopChannels;

static qbool	s_soundStarted;
static qbool	s_soundMuted;

static int			listener_number;
static vec3_t		listener_origin;
static vec3_t		listener_axis[3];

dma_t		dma;
int			s_soundtime;
int			s_paintedtime;

// MAX_SFX may be larger than MAX_SOUNDS because of custom player sounds
#define MAX_SFX 4096
static sfx_t s_knownSfx[MAX_SFX];
static int s_numSfx = 0;

#define SFX_HASH_SIZE 128
static sfx_t* sfxHash[SFX_HASH_SIZE];

static cvar_t* s_show;
static cvar_t* s_mixahead;
static cvar_t* s_mixPreStep;
const cvar_t* s_testsound;

static loopSound_t		loopSounds[MAX_GENTITIES];
static channel_t		*freelist = NULL;

int						s_rawend;
portable_samplepair_t	s_rawsamples[MAX_RAW_SAMPLES];


static void S_Base_SoundInfo()
{
	Com_Printf("----- Sound Info -----\n" );

	if (!s_soundStarted) {
		Com_Printf ("sound system not started\n");
	} else {
		Com_Printf("%5d stereo\n", dma.channels - 1);
		Com_Printf("%5d samples\n", dma.samples);
		Com_Printf("%5d samplebits\n", dma.samplebits);
		Com_Printf("%5d submission_chunk\n", dma.submission_chunk);
		Com_Printf("%5d speed\n", dma.speed);
		Com_Printf("0x%x dma buffer\n", dma.buffer);
		if ( s_backgroundStream ) {
			Com_Printf("Background file: %s\n", s_backgroundLoop );
		} else {
			Com_Printf("No background file.\n" );
		}
	}

	Com_Printf("----------------------\n" );
}


static void S_Base_SoundList()
{
	int total = 0;

	const sfx_t* sfx = s_knownSfx;
	for ( int i = 0; i < s_numSfx; ++i, ++sfx ) {
		int size = sfx->soundLength;
		total += size;
		Com_Printf("%6i [%s] : %s\n", size, sfx->inMemory ? "MEM" : "PGD", sfx->soundName );
	}

	S_DisplayFreeMemory();
}


static void S_ChannelFree( channel_t* v )
{
	v->thesfx = NULL;
	*(channel_t **)v = freelist;
	freelist = (channel_t*)v;
}

static channel_t* S_ChannelMalloc()
{
	channel_t *v;
	if (freelist == NULL) {
		return NULL;
	}
	v = freelist;
	freelist = *(channel_t **)freelist;
	v->allocTime = Com_Milliseconds();
	return v;
}

static void S_ChannelSetup()
{
	channel_t *p, *q;

	Com_Memset( s_channels, 0, sizeof( s_channels ) );

	p = s_channels;
	q = p + MAX_CHANNELS;
	while (--q > p) {
		*(channel_t **)q = q-1;
	}

	*(channel_t **)q = NULL;
	freelist = p + MAX_CHANNELS - 1;
	Com_DPrintf("Channel memory manager started\n");
}



// =======================================================================
// Load a sound
// =======================================================================


// will allocate a new sfx if it isn't found

static sfx_t* S_FindName( const char *name )
{
	int		i;

	if (!name) {
		Com_Error (ERR_FATAL, "S_FindName: NULL\n");
	}
	if (!name[0]) {
		Com_Error (ERR_FATAL, "S_FindName: empty name\n");
	}

	if (strlen(name) >= MAX_QPATH) {
		Com_Error (ERR_FATAL, "Sound name too long: %s", name);
	}

	int hash = Q_FileHash( name, SFX_HASH_SIZE );
	sfx_t* sfx = sfxHash[hash];

	// see if already loaded
	while (sfx) {
		if (!Q_stricmp(sfx->soundName, name) ) {
			return sfx;
		}
		sfx = sfx->next;
	}

	// find a free sfx
	for (i=0 ; i < s_numSfx ; i++) {
		if (!s_knownSfx[i].soundName[0]) {
			break;
		}
	}

	if (i == s_numSfx) {
		if (s_numSfx == MAX_SFX) {
			Com_Error (ERR_FATAL, "S_FindName: out of sfx_t");
		}
		s_numSfx++;
	}

	sfx = &s_knownSfx[i];
	Com_Memset (sfx, 0, sizeof(*sfx));
	strcpy (sfx->soundName, name);

	sfx->next = sfxHash[hash];
	sfxHash[hash] = sfx;

	return sfx;
}


static void S_memoryLoad( sfx_t* sfx )
{
	if ( !S_LoadSound( sfx ) ) {
		//Com_Printf( S_COLOR_YELLOW "WARNING: couldn't load sound: %s\n", sfx->soundName );
		sfx->defaultSound = qtrue;
	}
	sfx->inMemory = qtrue;
}


// creates a default buzz sound if the file can't be loaded

static sfxHandle_t S_Base_RegisterSound( const char* name )
{
	if (!s_soundStarted) {
		return 0;
	}

	if ( strlen( name ) >= MAX_QPATH ) {
		Com_Printf( "Sound name exceeds MAX_QPATH\n" );
		return 0;
	}

	sfx_t* sfx = S_FindName( name );
	if ( sfx->soundData ) {
		if ( sfx->defaultSound ) {
			Com_Printf( S_COLOR_YELLOW "WARNING: could not find %s - using default\n", sfx->soundName );
			return 0;
		}
		return sfx - s_knownSfx;
	}

	sfx->inMemory = qfalse;
	S_memoryLoad( sfx );

	if ( sfx->defaultSound ) {
		Com_Printf( S_COLOR_YELLOW "WARNING: could not find %s - using default\n", sfx->soundName );
		return 0;
	}

	return sfx - s_knownSfx;
}


static void S_Base_BeginRegistration()
{
	s_soundMuted = qfalse;		// we can play again

	SND_setup();

	s_numSfx = 0;
	Com_Memset( s_knownSfx, 0, sizeof( s_knownSfx ) );
	Com_Memset( sfxHash, 0, sizeof(sfx_t *)*SFX_HASH_SIZE );

	S_Base_RegisterSound( "sound/world/buzzer.wav" );
}


///////////////////////////////////////////////////////////////


// used for spatializing s_channels (duh, really?)

static void S_SpatializeOrigin( const vec3_t origin, int master_vol, int *left_vol, int *right_vol )
{
	vec_t		dot;
	vec_t		dist;
	vec_t		lscale, rscale, scale;
	vec3_t		source_vec;
	vec3_t		vec;

	// calculate stereo seperation and distance attenuation
	VectorSubtract( origin, listener_origin, source_vec );

	dist = VectorNormalize( source_vec );
	dist -= SOUND_FULLVOLUME;
	if (dist < 0)
		dist = 0;			// close enough to be at full volume
	dist *= SOUND_ATTENUATE;		// different attenuation levels

	VectorRotate( source_vec, listener_axis, vec );

	dot = -vec[1];

	if (dma.channels == 1)
	{ // no attenuation = no spatialization
		rscale = 1.0;
		lscale = 1.0;
	}
	else
	{
		rscale = 0.5 * (1.0 + dot);
		lscale = 0.5 * (1.0 - dot);
		if ( rscale < 0 ) {
			rscale = 0;
		}
		if ( lscale < 0 ) {
			lscale = 0;
		}
	}

	// add in distance effect
	scale = (1.0 - dist) * rscale;
	*right_vol = (master_vol * scale);
	if (*right_vol < 0)
		*right_vol = 0;

	scale = (1.0 - dist) * lscale;
	*left_vol = (master_vol * scale);
	if (*left_vol < 0)
		*left_vol = 0;
}


// =======================================================================
// Start a sound effect
// =======================================================================

/*
Validates the parms and queues the sound up
if pos is NULL, the sound will be dynamically sourced from the entity

in theory, entchannel 0 (CHAN_AUTO) will not override a playing sound unless it has to
and other channels will automatically override

in reality, this code doesn't actually bother
and EVERYTHING except CHAN_ANNOUNCER is treated as CHAN_AUTO
*/

static void S_Base_StartSound( const vec3_t origin, int entityNum, int entchannel, sfxHandle_t sfxHandle )
{
	channel_t	*ch;
	sfx_t		*sfx;
	int i, time;

	if ( !s_soundStarted || s_soundMuted ) {
		return;
	}

	if ( !origin && ( entityNum < 0 || entityNum > MAX_GENTITIES ) ) {
		Com_Error( ERR_DROP, "S_StartSound: bad entitynum %i", entityNum );
	}

	if ( sfxHandle < 0 || sfxHandle >= s_numSfx ) {
		Com_Printf( S_COLOR_YELLOW, "S_StartSound: handle %i out of range\n", sfxHandle );
		return;
	}

	sfx = &s_knownSfx[ sfxHandle ];

	if (sfx->inMemory == qfalse) {
		S_memoryLoad(sfx);
	}

	if ( s_show->integer == 1 ) {
		Com_Printf( "%i : %s\n", s_paintedtime, sfx->soundName );
	}

	time = Com_Milliseconds();

//	Com_Printf("playing %s\n", sfx->soundName);
	// pick a channel to play on

	int allowed = (entityNum == listener_number) ? 8 : 4;

	int inplay = 0;
	ch = s_channels;
	for ( i = 0; i < MAX_CHANNELS ; i++, ch++ ) {
		if (ch->entnum == entityNum && ch->thesfx == sfx) {
			// the WORLD can very legitimately have multiple instances of the same sound at the same time
			// and it's important that they DO play if possible even if simultaneous
			// because a bullet hitting 2ft away and one hitting 50ft away are not the same thing
			if ( (entityNum != ENTITYNUM_WORLD) && (time == ch->allocTime) ) {
//				if (Cvar_VariableValue( "cg_showmiss" )) {
//					Com_Printf("double sound start: %d %s\n", entityNum, sfx->soundName);
//				}
				return;
			}
			inplay++;
		}
	}

	if (inplay > allowed) {
		return;
	}

	sfx->lastTimeUsed = time;

	ch = S_ChannelMalloc();	// entityNum, entchannel);
	if (!ch) {
		// realistically, this will only happen in timedemos,
		// and on ubershitty maps (3W, CA, space) with massive PG spam etc
		int oldest = sfx->lastTimeUsed;
		int chosen = -1;

		//Com_Printf("no channel for %s - ", sfx->soundName);

		ch = s_channels;
		for ( i = 0 ; i < MAX_CHANNELS ; i++, ch++ ) {
			if (ch->entnum != listener_number && ch->entnum == entityNum && ch->allocTime<oldest && ch->entchannel != CHAN_ANNOUNCER) {
				oldest = ch->allocTime;
				chosen = i;
				//Com_Printf("reusing same-entity channel\n");
			}
		}

		if (chosen == -1) {
			ch = s_channels;
			for ( i = 0 ; i < MAX_CHANNELS ; i++, ch++ ) {
				if (ch->entnum != listener_number && ch->allocTime<oldest && ch->entchannel != CHAN_ANNOUNCER) {
					oldest = ch->allocTime;
					chosen = i;
					//Com_Printf("reusing diff-entity channel\n");
				}
			}
		}

		if (chosen == -1) {
			ch = s_channels;
			for ( i = 0 ; i < MAX_CHANNELS ; i++, ch++ ) {
				if ( (ch->entnum == listener_number) && (ch->allocTime < oldest) ) {
					oldest = ch->allocTime;
					chosen = i;
					//Com_Printf("reusing listener channel\n");
				}
			}
		}

		if (chosen == -1) {
			Com_Printf("dropping sound\n");
			return;
		}

		ch = &s_channels[chosen];
		ch->allocTime = sfx->lastTimeUsed;
	}

	if (origin) {
		VectorCopy (origin, ch->origin);
		ch->fixed_origin = qtrue;
	} else {
		ch->fixed_origin = qfalse;
	}

	ch->master_vol = 127;
	ch->entnum = entityNum;
	ch->thesfx = sfx;
	ch->startSample = START_SAMPLE_IMMEDIATE;
	ch->entchannel = entchannel;
	ch->leftvol = ch->master_vol;		// these will get calced at next spatialize
	ch->rightvol = ch->master_vol;		// unless the game isn't running
	ch->doppler = qfalse;
}


static void S_Base_StartLocalSound( sfxHandle_t sfxHandle, int channelNum )
{
	if ( !s_soundStarted || s_soundMuted ) {
		return;
	}

	if ( sfxHandle < 0 || sfxHandle >= s_numSfx ) {
		Com_Printf( S_COLOR_YELLOW, "S_StartLocalSound: handle %i out of range\n", sfxHandle );
		return;
	}

	S_Base_StartSound( NULL, listener_number, channelNum, sfxHandle );
}


// if we are about to perform file access, clear the buffer
// so sound doesn't stutter

static void S_Base_ClearSoundBuffer()
{
	if (!s_soundStarted)
		return;

	// stop looping sounds
	Com_Memset(loopSounds, 0, MAX_GENTITIES*sizeof(loopSound_t));
	Com_Memset(loop_channels, 0, MAX_CHANNELS*sizeof(channel_t));
	numLoopChannels = 0;

	S_ChannelSetup();

	s_rawend = 0;

	SNDDMA_BeginPainting();
	if (dma.buffer) {
		int clear = (dma.samplebits == 8) ? 0x80 : 0x00;
		Com_Memset( dma.buffer, clear, dma.samples * dma.samplebits/8 );
	}
	SNDDMA_Submit();
}


static void S_Base_StopAllSounds()
{
	if ( !s_soundStarted ) {
		return;
	}

	// stop the background music
	S_Base_StopBackgroundTrack();

	S_Base_ClearSoundBuffer();
}


// disables sounds until the next S_BeginRegistration
// this is called when the hunk is cleared and the sounds are no longer valid

static void S_Base_DisableSounds()
{
	S_Base_StopAllSounds();
	s_soundMuted = qtrue;
}


/*
==============================================================

continuous looping sounds are added each frame

==============================================================
*/


static void S_Base_ClearLoopingSounds()
{
	for (int i = 0; i < MAX_GENTITIES; ++i)
		loopSounds[i].active = qfalse;
	numLoopChannels = 0;
}


static qbool S_Base_InitLoopingSound( int entityNum, const vec3_t origin, const vec3_t velocity, sfxHandle_t sfxHandle )
{
	if ( !s_soundStarted || s_soundMuted ) {
		return qfalse;
	}

	if ( sfxHandle < 0 || sfxHandle >= s_numSfx ) {
		Com_Printf( S_COLOR_YELLOW, "S_AddLoopingSound: handle %i out of range\n", sfxHandle );
		return qfalse;
	}

	sfx_t* sfx = &s_knownSfx[ sfxHandle ];

	if (sfx->inMemory == qfalse) {
		S_memoryLoad(sfx);
	}

	if ( !sfx->soundLength ) {
		Com_Error( ERR_DROP, "%s has length 0", sfx->soundName );
	}

	VectorCopy( origin, loopSounds[entityNum].origin );
	VectorCopy( velocity, loopSounds[entityNum].velocity );
	loopSounds[entityNum].sfx = sfx;
	loopSounds[entityNum].active = qtrue;
	loopSounds[entityNum].doppler = qfalse;

	return qtrue;
}


// called during entity generation for a frame
// includes velocity for doppler

static void S_Base_AddLoopingSound( int entityNum, const vec3_t origin, const vec3_t velocity, sfxHandle_t sfxHandle )
{
	if ( !S_Base_InitLoopingSound( entityNum, origin, velocity, sfxHandle ) )
		return;

	loopSounds[entityNum].oldDopplerScale = 1.0;
	loopSounds[entityNum].dopplerScale = 1.0;

	if (s_doppler->integer && VectorLengthSquared(velocity)>0.0) {
		vec3_t	out;
		float	lena, lenb;

		loopSounds[entityNum].doppler = qtrue;
		lena = DistanceSquared(loopSounds[listener_number].origin, loopSounds[entityNum].origin);
		VectorAdd(loopSounds[entityNum].origin, loopSounds[entityNum].velocity, out);
		lenb = DistanceSquared(loopSounds[listener_number].origin, out);
		if ((loopSounds[entityNum].framenum+1) != cls.framecount) {
			loopSounds[entityNum].oldDopplerScale = 1.0;
		} else {
			loopSounds[entityNum].oldDopplerScale = loopSounds[entityNum].dopplerScale;
		}
		loopSounds[entityNum].dopplerScale = lenb/(lena*100);
		if (loopSounds[entityNum].dopplerScale<=1.0) {
			loopSounds[entityNum].doppler = qfalse;			// don't bother doing the math
		} else if (loopSounds[entityNum].dopplerScale>MAX_DOPPLER_SCALE) {
			loopSounds[entityNum].dopplerScale = MAX_DOPPLER_SCALE;
		}
	}

	loopSounds[entityNum].framenum = cls.framecount;
}


/*
Spatialize all of the looping (ie ambient) sounds.
All sounds are on the same cycle, so any duplicates can just
sum up the channel multipliers.
*/
static void S_AddLoopSounds()
{
	const int AMBIENT_VOLUME = 96; // 25% quieter than normal sounds

	int			i, j, time;
	int			left_total, right_total, left, right;
	channel_t	*ch;
	loopSound_t	*loop, *loop2;
	static int	loopFrame;


	numLoopChannels = 0;

	time = Com_Milliseconds();

	loopFrame++;
	for ( i = 0 ; i < MAX_GENTITIES ; i++) {
		loop = &loopSounds[i];
		if ( !loop->active || loop->mergeFrame == loopFrame ) {
			continue;	// already merged into an earlier sound
		}

		S_SpatializeOrigin( loop->origin, AMBIENT_VOLUME, &left_total, &right_total );

		loop->sfx->lastTimeUsed = time;

		for (j=(i+1); j< MAX_GENTITIES ; j++) {
			loop2 = &loopSounds[j];
			if ( !loop2->active || loop2->doppler || loop2->sfx != loop->sfx) {
				continue;
			}
			loop2->mergeFrame = loopFrame;

			S_SpatializeOrigin( loop2->origin, AMBIENT_VOLUME, &left, &right );

			loop2->sfx->lastTimeUsed = time;
			left_total += left;
			right_total += right;
		}
		if (left_total == 0 && right_total == 0) {
			continue;		// not audible
		}

		// allocate a channel
		ch = &loop_channels[numLoopChannels];

		if (left_total > 255) {
			left_total = 255;
		}
		if (right_total > 255) {
			right_total = 255;
		}

		ch->master_vol = 127;
		ch->leftvol = left_total;
		ch->rightvol = right_total;
		ch->thesfx = loop->sfx;
		ch->doppler = loop->doppler;
		ch->dopplerScale = loop->dopplerScale;
		ch->oldDopplerScale = loop->oldDopplerScale;
		numLoopChannels++;
		if (numLoopChannels == MAX_CHANNELS) {
			return;
		}
	}
}


///////////////////////////////////////////////////////////////


// music streaming

static void S_Base_RawSamples( int samples, int rate, int width, int s_channels, const byte *data, float volume )
{
	int		i;
	int		src, dst;
	float	scale;
	int		intVolume;

	if ( !s_soundStarted || s_soundMuted ) {
		return;
	}

	intVolume = 256 * volume;

	if ( s_rawend < s_soundtime ) {
		Com_DPrintf( "S_RawSamples: resetting minimum: %i < %i\n", s_rawend, s_soundtime );
		s_rawend = s_soundtime;
	}

	scale = (float)rate / dma.speed;

//Com_Printf ("%i < %i < %i\n", s_soundtime, s_paintedtime, s_rawend);
	if (s_channels == 2 && width == 2)
	{
		if (scale == 1.0)
		{	// optimized case
			for (i=0 ; i<samples ; i++)
			{
				dst = s_rawend&(MAX_RAW_SAMPLES-1);
				s_rawend++;
				s_rawsamples[dst].left = ((short *)data)[i*2] * intVolume;
				s_rawsamples[dst].right = ((short *)data)[i*2+1] * intVolume;
			}
		}
		else
		{
			for (i=0 ; ; i++)
			{
				src = i*scale;
				if (src >= samples)
					break;
				dst = s_rawend&(MAX_RAW_SAMPLES-1);
				s_rawend++;
				s_rawsamples[dst].left = ((short *)data)[src*2] * intVolume;
				s_rawsamples[dst].right = ((short *)data)[src*2+1] * intVolume;
			}
		}
	}
	else if (s_channels == 1 && width == 2)
	{
		for (i=0 ; ; i++)
		{
			src = i*scale;
			if (src >= samples)
				break;
			dst = s_rawend&(MAX_RAW_SAMPLES-1);
			s_rawend++;
			s_rawsamples[dst].left = ((short *)data)[src] * intVolume;
			s_rawsamples[dst].right = ((short *)data)[src] * intVolume;
		}
	}
	else if (s_channels == 2 && width == 1)
	{
		intVolume *= 256;

		for (i=0 ; ; i++)
		{
			src = i*scale;
			if (src >= samples)
				break;
			dst = s_rawend&(MAX_RAW_SAMPLES-1);
			s_rawend++;
			s_rawsamples[dst].left = ((char *)data)[src*2] * intVolume;
			s_rawsamples[dst].right = ((char *)data)[src*2+1] * intVolume;
		}
	}
	else if (s_channels == 1 && width == 1)
	{
		intVolume *= 256;

		for (i=0 ; ; i++)
		{
			src = i*scale;
			if (src >= samples)
				break;
			dst = s_rawend&(MAX_RAW_SAMPLES-1);
			s_rawend++;
			s_rawsamples[dst].left = (((byte *)data)[src]-128) * intVolume;
			s_rawsamples[dst].right = (((byte *)data)[src]-128) * intVolume;
		}
	}

	if ( s_rawend > s_soundtime + MAX_RAW_SAMPLES ) {
		Com_DPrintf( "S_RawSamples: overflowed %i > %i\n", s_rawend, s_soundtime );
	}
}


///////////////////////////////////////////////////////////////


// let the sound system know where an entity currently is

static void S_Base_UpdateEntityPosition( int entityNum, const vec3_t origin )
{
	if ( entityNum < 0 || entityNum > MAX_GENTITIES ) {
		Com_Error( ERR_DROP, "S_UpdateEntityPosition: bad entitynum %i", entityNum );
	}
	VectorCopy( origin, loopSounds[entityNum].origin );
}


// change the volumes of all the playing sounds for changes in their positions

static void S_Base_Respatialize( int entityNum, const vec3_t head, const vec3_t axis[3], int inwater )
{
	int			i;
	channel_t	*ch;
	vec3_t		origin;

	if ( !s_soundStarted || s_soundMuted ) {
		return;
	}

	listener_number = entityNum;
	VectorCopy(head, listener_origin);
	VectorCopy(axis[0], listener_axis[0]);
	VectorCopy(axis[1], listener_axis[1]);
	VectorCopy(axis[2], listener_axis[2]);

	// update spatialization for dynamic sounds
	ch = s_channels;
	for ( i = 0 ; i < MAX_CHANNELS ; i++, ch++ ) {
		if ( !ch->thesfx ) {
			continue;
		}
		// anything coming from the view entity will always be full volume
		if (ch->entnum == listener_number) {
			ch->leftvol = ch->master_vol;
			ch->rightvol = ch->master_vol;
		} else {
			if (ch->fixed_origin) {
				VectorCopy( ch->origin, origin );
			} else {
				VectorCopy( loopSounds[ ch->entnum ].origin, origin );
			}
			S_SpatializeOrigin( origin, ch->master_vol, &ch->leftvol, &ch->rightvol );
		}
	}

	S_AddLoopSounds();
}


// returns true if any new sounds were started since the last mix
// and clears out any expired sounds

static qbool S_ScanChannelStarts()
{
	channel_t		*ch;
	int				i;
	qbool		newSamples = qfalse;

	ch = s_channels;

	for (i=0; i<MAX_CHANNELS ; i++, ch++) {
		if ( !ch->thesfx ) {
			continue;
		}

		// if this channel was just started this frame,
		// set the sample count so it begins mixing
		// into the very first sample
		if ( ch->startSample == START_SAMPLE_IMMEDIATE ) {
			ch->startSample = s_paintedtime;
			newSamples = qtrue;
			continue;
		}

		// if it is completely finished by now, clear it
		if ( ch->startSample + ch->thesfx->soundLength <= s_paintedtime ) {
			S_ChannelFree(ch);
		}
	}

	return newSamples;
}


// called once each time through the main loop

static void S_Base_Update()
{
	int			i;
	int			total;
	channel_t	*ch;

	if ( !s_soundStarted || s_soundMuted ) {
//		Com_DPrintf ("not started or muted\n");
		return;
	}

	//
	// debugging output
	//
	if ( s_show->integer == 2 ) {
		total = 0;
		ch = s_channels;
		for (i=0 ; i<MAX_CHANNELS; i++, ch++) {
			if (ch->thesfx && (ch->leftvol || ch->rightvol) ) {
				Com_Printf ("%f %f %s\n", ch->leftvol, ch->rightvol, ch->thesfx->soundName);
				total++;
			}
		}
		Com_Printf ("----(%i)---- painted: %i\n", total, s_paintedtime);
	}

	// add raw data from streamed samples
	S_UpdateBackgroundTrack();

	// mix some sound
	S_Update_DMA();
}


static void S_GetSoundtime()
{
	int		samplepos;
	static	int		buffers;
	static	int		oldsamplepos;
	int		fullsamples;

	fullsamples = dma.samples / dma.channels;

	if ( CL_VideoRecording() )
	{
		s_soundtime += (int)ceil( dma.speed / cl_aviFrameRate->value );
		return;
	}

	// it is possible to miscount buffers if it has wrapped twice between
	// calls to S_Update.  Oh well.
	samplepos = SNDDMA_GetDMAPos();
	if (samplepos < oldsamplepos)
	{
		buffers++;					// buffer wrapped

		if (s_paintedtime > 0x40000000)
		{	// time to chop things off to avoid 32 bit limits
			buffers = 0;
			s_paintedtime = fullsamples;
			S_Base_StopAllSounds ();
		}
	}
	oldsamplepos = samplepos;

	s_soundtime = buffers*fullsamples + samplepos/dma.channels;

#if 0
// check to make sure that we haven't overshot
	if (s_paintedtime < s_soundtime)
	{
		Com_DPrintf ("S_GetSoundtime : overflow\n");
		s_paintedtime = s_soundtime;
	}
#endif

	if ( dma.submission_chunk < 256 ) {
		s_paintedtime = s_soundtime + s_mixPreStep->value * dma.speed;
	} else {
		s_paintedtime = s_soundtime + dma.submission_chunk;
	}
}


static void S_Update_DMA()
{
	unsigned		endtime;
	int				samps;
	static			float	lastTime = 0.0f;
	float			ma, op;
	float			thisTime, sane;
	static			int ot = -1;

	if ( !s_soundStarted || s_soundMuted ) {
		return;
	}

	thisTime = Com_Milliseconds();

	// Updates s_soundtime
	S_GetSoundtime();

	if (s_soundtime == ot) {
		return;
	}
	ot = s_soundtime;

	// clear any sound effects that end before the current time,
	// and start any new sounds
	S_ScanChannelStarts();

	sane = thisTime - lastTime;
	if (sane<11) {
		sane = 11;			// 85hz
	}

	ma = s_mixahead->value * dma.speed;
	op = s_mixPreStep->value + sane*dma.speed*0.01;

	if (op < ma) {
		ma = op;
	}

	// mix ahead of current position
	endtime = s_soundtime + ma;

	// mix to an even submission block size
	endtime = (endtime + dma.submission_chunk-1)
		& ~(dma.submission_chunk-1);

	// never mix more than the complete buffer
	samps = dma.samples >> (dma.channels-1);
	if (endtime - s_soundtime > samps)
		endtime = s_soundtime + samps;

	SNDDMA_BeginPainting();

	S_PaintChannels( endtime );

	SNDDMA_Submit();

	lastTime = thisTime;
}



/*
===============================================================================

background music functions

===============================================================================
*/


static void S_Base_StopBackgroundTrack()
{
	if(!s_backgroundStream)
		return;
	S_CodecCloseStream(s_backgroundStream);
	s_backgroundStream = NULL;
	s_rawend = 0;
}


static void S_Base_StartBackgroundTrack( const char *intro, const char *loop )
{
	if ( !intro ) {
		intro = "";
	}
	if ( !loop || !loop[0] ) {
		loop = intro;
	}
	Com_DPrintf( "S_StartBackgroundTrack( %s, %s )\n", intro, loop );

	if ( !intro[0] ) {
		return;
	}

	if( !loop ) {
		s_backgroundLoop[0] = 0;
	} else {
		Q_strncpyz( s_backgroundLoop, loop, sizeof( s_backgroundLoop ) );
	}

	// close the background track, but DON'T reset s_rawend
	// if restarting the same background track
	if (s_backgroundStream)
	{
		S_CodecCloseStream(s_backgroundStream);
		s_backgroundStream = NULL;
	}

	// Open stream
	s_backgroundStream = S_CodecOpenStream(intro);
	if (!s_backgroundStream) {
		Com_Printf( S_COLOR_YELLOW "WARNING: couldn't open music file %s\n", intro );
		return;
	}

	if (s_backgroundStream->info.channels != 2 || s_backgroundStream->info.rate != 22050) {
		Com_Printf(S_COLOR_YELLOW "WARNING: music file %s is not 22k stereo\n", intro );
	}
}


static void S_UpdateBackgroundTrack()
{
	int		bufferSamples;
	int		fileSamples;
	byte	raw[30000];		// just enough to fit in a mac stack frame
	int		fileBytes;
	int		r;
	static	float	musicVolume = 0.5f;

	if(!s_backgroundStream) {
		return;
	}

	// graeme see if this is OK
	musicVolume = (musicVolume + (s_musicVolume->value * 2))/4.0f;

	// don't bother playing anything if musicvolume is 0
	if ( musicVolume <= 0 ) {
		return;
	}

	// see how many samples should be copied into the raw buffer
	if ( s_rawend < s_soundtime ) {
		s_rawend = s_soundtime;
	}

	while ( s_rawend < s_soundtime + MAX_RAW_SAMPLES ) {
		bufferSamples = MAX_RAW_SAMPLES - (s_rawend - s_soundtime);

		// decide how much data needs to be read from the file
		fileSamples = bufferSamples * s_backgroundStream->info.rate / dma.speed;

		// our max buffer size
		fileBytes = fileSamples * (s_backgroundStream->info.width * s_backgroundStream->info.channels);
		if ( fileBytes > sizeof(raw) ) {
			fileBytes = sizeof(raw);
			fileSamples = fileBytes / (s_backgroundStream->info.width * s_backgroundStream->info.channels);
		}

		// Read
		r = S_CodecReadStream(s_backgroundStream, fileBytes, raw);
		if(r < fileBytes)
		{
			fileBytes = r;
			fileSamples = r / (s_backgroundStream->info.width * s_backgroundStream->info.channels);
		}

		if(r > 0)
		{
			// add to raw buffer
			S_Base_RawSamples( fileSamples, s_backgroundStream->info.rate,
				s_backgroundStream->info.width, s_backgroundStream->info.channels, raw, musicVolume );
		}
		else
		{
			// loop
			if(s_backgroundLoop[0])
			{
				S_CodecCloseStream(s_backgroundStream);
				s_backgroundStream = NULL;
				S_Base_StartBackgroundTrack( s_backgroundLoop, s_backgroundLoop );
				if(!s_backgroundStream)
					return;
			}
			else
			{
				S_Base_StopBackgroundTrack();
				return;
			}
		}

	}
}


/*
======================
S_FreeOldestSound
======================
*/

void S_FreeOldestSound( void ) {
	int	i, oldest, used;
	sfx_t	*sfx;
	sndBuffer	*buffer, *nbuffer;

	oldest = Com_Milliseconds();
	used = 0;

	for (i=1 ; i < s_numSfx ; i++) {
		sfx = &s_knownSfx[i];
		if (sfx->inMemory && sfx->lastTimeUsed<oldest) {
			used = i;
			oldest = sfx->lastTimeUsed;
		}
	}

	sfx = &s_knownSfx[used];

	Com_DPrintf("S_FreeOldestSound: freeing sound %s\n", sfx->soundName);

	buffer = sfx->soundData;
	while(buffer != NULL) {
		nbuffer = buffer->next;
		SND_free(buffer);
		buffer = nbuffer;
	}

	sfx->inMemory = qfalse;
	sfx->soundData = NULL;
}


static void S_Base_Shutdown()
{
	if ( !s_soundStarted ) {
		return;
	}

	SNDDMA_Shutdown();

	s_soundStarted = qfalse;

	Cmd_RemoveCommand("s_info");
}


qbool S_Base_Init( soundInterface_t *si )
{
	Com_Memset( si, 0, sizeof(*si) );

	s_mixahead = Cvar_Get( "s_mixahead", "0.2", CVAR_ARCHIVE );
	s_mixPreStep = Cvar_Get( "s_mixPreStep", "0.05", CVAR_ARCHIVE );
	s_show = Cvar_Get( "s_show", "0", CVAR_CHEAT );
	s_testsound = Cvar_Get( "s_testsound", "0", CVAR_CHEAT );

	if (!SNDDMA_Init())
		return qfalse;

	s_soundStarted = qtrue;
	s_soundMuted = qtrue;
	s_numSfx = 0;

	Com_Memset( sfxHash, 0, sizeof(sfx_t *)*SFX_HASH_SIZE );

	s_soundtime = 0;
	s_paintedtime = 0;

	si->Shutdown = S_Base_Shutdown;
	si->StartSound = S_Base_StartSound;
	si->StartLocalSound = S_Base_StartLocalSound;
	si->StartBackgroundTrack = S_Base_StartBackgroundTrack;
	si->StopBackgroundTrack = S_Base_StopBackgroundTrack;
	si->RawSamples = S_Base_RawSamples;
	si->StopAllSounds = S_Base_StopAllSounds;
	si->ClearLoopingSounds = S_Base_ClearLoopingSounds;
	si->AddLoopingSound = S_Base_AddLoopingSound;
	si->Respatialize = S_Base_Respatialize;
	si->UpdateEntityPosition = S_Base_UpdateEntityPosition;
	si->Update = S_Base_Update;
	si->DisableSounds = S_Base_DisableSounds;
	si->BeginRegistration = S_Base_BeginRegistration;
	si->RegisterSound = S_Base_RegisterSound;
	si->ClearSoundBuffer = S_Base_ClearSoundBuffer;
	si->SoundInfo = S_Base_SoundInfo;
	si->SoundList = S_Base_SoundList;

	S_Base_StopAllSounds();

	return qtrue;
}
