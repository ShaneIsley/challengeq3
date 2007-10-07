/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2005 Stuart Dalton (badcdev@gmail.com)

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

// Dynamically loads OpenAL

#if USE_OPENAL

#include "qal.h"

#if USE_OPENAL_DLOPEN

#if defined _WIN32
#include <windows.h>
#define OBJTYPE HMODULE
#define OBJLOAD(x) LoadLibrary(x)
#define SYMLOAD(x,y) GetProcAddress(x,y)
#define OBJFREE(x) FreeLibrary(x)

#elif defined __linux__ || defined __FreeBSD__ || defined MACOS_X || defined __sun
#include <dlfcn.h>
#define OBJTYPE void *
#define OBJLOAD(x) dlopen(x, RTLD_LAZY | RTLD_GLOBAL)
#define SYMLOAD(x,y) dlsym(x,y)
#define OBJFREE(x) dlclose(x)
#else

#error "Your platform has no lib loading code or it is disabled"
#endif

#if defined __linux__ || defined __FreeBSD__ || defined MACOS_X
#include <unistd.h>
#include <sys/types.h>
#endif

LPALENABLE qalEnable;
LPALDISABLE qalDisable;
LPALISENABLED qalIsEnabled;
LPALGETSTRING qalGetString;
LPALGETBOOLEANV qalGetBooleanv;
LPALGETINTEGERV qalGetIntegerv;
LPALGETFLOATV qalGetFloatv;
LPALGETDOUBLEV qalGetDoublev;
LPALGETBOOLEAN qalGetBoolean;
LPALGETINTEGER qalGetInteger;
LPALGETFLOAT qalGetFloat;
LPALGETDOUBLE qalGetDouble;
LPALGETERROR qalGetError;
LPALISEXTENSIONPRESENT qalIsExtensionPresent;
LPALGETPROCADDRESS qalGetProcAddress;
LPALGETENUMVALUE qalGetEnumValue;
LPALLISTENERF qalListenerf;
LPALLISTENER3F qalListener3f;
LPALLISTENERFV qalListenerfv;
LPALLISTENERI qalListeneri;
LPALGETLISTENERF qalGetListenerf;
LPALGETLISTENER3F qalGetListener3f;
LPALGETLISTENERFV qalGetListenerfv;
LPALGETLISTENERI qalGetListeneri;
LPALGENSOURCES qalGenSources;
LPALDELETESOURCES qalDeleteSources;
LPALISSOURCE qalIsSource;
LPALSOURCEF qalSourcef;
LPALSOURCE3F qalSource3f;
LPALSOURCEFV qalSourcefv;
LPALSOURCEI qalSourcei;
LPALGETSOURCEF qalGetSourcef;
LPALGETSOURCE3F qalGetSource3f;
LPALGETSOURCEFV qalGetSourcefv;
LPALGETSOURCEI qalGetSourcei;
LPALSOURCEPLAYV qalSourcePlayv;
LPALSOURCESTOPV qalSourceStopv;
LPALSOURCEREWINDV qalSourceRewindv;
LPALSOURCEPAUSEV qalSourcePausev;
LPALSOURCEPLAY qalSourcePlay;
LPALSOURCESTOP qalSourceStop;
LPALSOURCEREWIND qalSourceRewind;
LPALSOURCEPAUSE qalSourcePause;
LPALSOURCEQUEUEBUFFERS qalSourceQueueBuffers;
LPALSOURCEUNQUEUEBUFFERS qalSourceUnqueueBuffers;
LPALGENBUFFERS qalGenBuffers;
LPALDELETEBUFFERS qalDeleteBuffers;
LPALISBUFFER qalIsBuffer;
LPALBUFFERDATA qalBufferData;
LPALGETBUFFERF qalGetBufferf;
LPALGETBUFFERI qalGetBufferi;
LPALDOPPLERFACTOR qalDopplerFactor;
LPALDOPPLERVELOCITY qalDopplerVelocity;
LPALDISTANCEMODEL qalDistanceModel;

LPALCCREATECONTEXT qalcCreateContext;
LPALCMAKECONTEXTCURRENT qalcMakeContextCurrent;
LPALCPROCESSCONTEXT qalcProcessContext;
LPALCSUSPENDCONTEXT qalcSuspendContext;
LPALCDESTROYCONTEXT qalcDestroyContext;
LPALCGETCURRENTCONTEXT qalcGetCurrentContext;
LPALCGETCONTEXTSDEVICE qalcGetContextsDevice;
LPALCOPENDEVICE qalcOpenDevice;
LPALCCLOSEDEVICE qalcCloseDevice;
LPALCGETERROR qalcGetError;
LPALCISEXTENSIONPRESENT qalcIsExtensionPresent;
LPALCGETPROCADDRESS qalcGetProcAddress;
LPALCGETENUMVALUE qalcGetEnumValue;
LPALCGETSTRING qalcGetString;
LPALCGETINTEGERV qalcGetIntegerv;

static OBJTYPE OpenALLib = NULL;

static qbool alinit_fail = qfalse;


static void* GPA( const char* s )
{
	void* rv = SYMLOAD( OpenALLib, s );
	if (!rv)
	{
		Com_Printf( "Can't load symbol %s\n", s );
		alinit_fail = qtrue;
	}
	else
	{
		Com_DPrintf( "Loaded symbol %s (0x%08X)\n", s, rv );
	}
	return rv;
}


qbool QAL_Init( const char* libname )
{
	if (OpenALLib)
		return qtrue;

	Com_Printf( "Loading \"%s\"...\n", libname);
	if (!(OpenALLib = OBJLOAD(libname)))
	{
#ifdef _WIN32
		return qfalse;
#else
		char fn[1024];
		getcwd(fn, sizeof(fn));
		strncat(fn, "/", sizeof(fn));
		strncat(fn, libname, sizeof(fn));

		if( (OpenALLib = OBJLOAD(fn)) == 0 )
		{
			return qfalse;
		}
#endif
	}

	alinit_fail = qfalse;

	qalEnable = (LPALENABLE)GPA("alEnable");
	qalDisable = (LPALDISABLE)GPA("alDisable");
	qalIsEnabled = (LPALISENABLED)GPA("alIsEnabled");
	qalGetString = (LPALGETSTRING)GPA("alGetString");
	qalGetBooleanv = (LPALGETBOOLEANV)GPA("alGetBooleanv");
	qalGetIntegerv = (LPALGETINTEGERV)GPA("alGetIntegerv");
	qalGetFloatv = (LPALGETFLOATV)GPA("alGetFloatv");
	qalGetDoublev = (LPALGETDOUBLEV)GPA("alGetDoublev");
	qalGetBoolean = (LPALGETBOOLEAN)GPA("alGetBoolean");
	qalGetInteger = (LPALGETINTEGER)GPA("alGetInteger");
	qalGetFloat = (LPALGETFLOAT)GPA("alGetFloat");
	qalGetDouble = (LPALGETDOUBLE)GPA("alGetDouble");
	qalGetError = (LPALGETERROR)GPA("alGetError");
	qalIsExtensionPresent = (LPALISEXTENSIONPRESENT)GPA("alIsExtensionPresent");
	qalGetProcAddress = (LPALGETPROCADDRESS)GPA("alGetProcAddress");
	qalGetEnumValue = (LPALGETENUMVALUE)GPA("alGetEnumValue");
	qalListenerf = (LPALLISTENERF)GPA("alListenerf");
	qalListener3f = (LPALLISTENER3F)GPA("alListener3f");
	qalListenerfv = (LPALLISTENERFV)GPA("alListenerfv");
	qalListeneri = (LPALLISTENERI)GPA("alListeneri");
	qalGetListenerf = (LPALGETLISTENERF)GPA("alGetListenerf");
	qalGetListener3f = (LPALGETLISTENER3F)GPA("alGetListener3f");
	qalGetListenerfv = (LPALGETLISTENERFV)GPA("alGetListenerfv");
	qalGetListeneri = (LPALGETLISTENERI)GPA("alGetListeneri");
	qalGenSources = (LPALGENSOURCES)GPA("alGenSources");
	qalDeleteSources = (LPALDELETESOURCES)GPA("alDeleteSources");
	qalIsSource = (LPALISSOURCE)GPA("alIsSource");
	qalSourcef = (LPALSOURCEF)GPA("alSourcef");
	qalSource3f = (LPALSOURCE3F)GPA("alSource3f");
	qalSourcefv = (LPALSOURCEFV)GPA("alSourcefv");
	qalSourcei = (LPALSOURCEI)GPA("alSourcei");
	qalGetSourcef = (LPALGETSOURCEF)GPA("alGetSourcef");
	qalGetSource3f = (LPALGETSOURCE3F)GPA("alGetSource3f");
	qalGetSourcefv = (LPALGETSOURCEFV)GPA("alGetSourcefv");
	qalGetSourcei = (LPALGETSOURCEI)GPA("alGetSourcei");
	qalSourcePlayv = (LPALSOURCEPLAYV)GPA("alSourcePlayv");
	qalSourceStopv = (LPALSOURCESTOPV)GPA("alSourceStopv");
	qalSourceRewindv = (LPALSOURCEREWINDV)GPA("alSourceRewindv");
	qalSourcePausev = (LPALSOURCEPAUSEV)GPA("alSourcePausev");
	qalSourcePlay = (LPALSOURCEPLAY)GPA("alSourcePlay");
	qalSourceStop = (LPALSOURCESTOP)GPA("alSourceStop");
	qalSourceRewind = (LPALSOURCEREWIND)GPA("alSourceRewind");
	qalSourcePause = (LPALSOURCEPAUSE)GPA("alSourcePause");
	qalSourceQueueBuffers = (LPALSOURCEQUEUEBUFFERS)GPA("alSourceQueueBuffers");
	qalSourceUnqueueBuffers = (LPALSOURCEUNQUEUEBUFFERS)GPA("alSourceUnqueueBuffers");
	qalGenBuffers = (LPALGENBUFFERS)GPA("alGenBuffers");
	qalDeleteBuffers = (LPALDELETEBUFFERS)GPA("alDeleteBuffers");
	qalIsBuffer = (LPALISBUFFER)GPA("alIsBuffer");
	qalBufferData = (LPALBUFFERDATA)GPA("alBufferData");
	qalGetBufferf = (LPALGETBUFFERF)GPA("alGetBufferf");
	qalGetBufferi = (LPALGETBUFFERI)GPA("alGetBufferi");
	qalDopplerFactor = (LPALDOPPLERFACTOR)GPA("alDopplerFactor");
	qalDopplerVelocity = (LPALDOPPLERVELOCITY)GPA("alDopplerVelocity");
	qalDistanceModel = (LPALDISTANCEMODEL)GPA("alDistanceModel");

	qalcCreateContext = (LPALCCREATECONTEXT)GPA("alcCreateContext");
	qalcMakeContextCurrent = (LPALCMAKECONTEXTCURRENT)GPA("alcMakeContextCurrent");
	qalcProcessContext = (LPALCPROCESSCONTEXT)GPA("alcProcessContext");
	qalcSuspendContext = (LPALCSUSPENDCONTEXT)GPA("alcSuspendContext");
	qalcDestroyContext = (LPALCDESTROYCONTEXT)GPA("alcDestroyContext");
	qalcGetCurrentContext = (LPALCGETCURRENTCONTEXT)GPA("alcGetCurrentContext");
	qalcGetContextsDevice = (LPALCGETCONTEXTSDEVICE)GPA("alcGetContextsDevice");
	qalcOpenDevice = (LPALCOPENDEVICE)GPA("alcOpenDevice");
	qalcCloseDevice = (LPALCCLOSEDEVICE)GPA("alcCloseDevice");
	qalcGetError = (LPALCGETERROR)GPA("alcGetError");
	qalcIsExtensionPresent = (LPALCISEXTENSIONPRESENT)GPA("alcIsExtensionPresent");
	qalcGetProcAddress = (LPALCGETPROCADDRESS)GPA("alcGetProcAddress");
	qalcGetEnumValue = (LPALCGETENUMVALUE)GPA("alcGetEnumValue");
	qalcGetString = (LPALCGETSTRING)GPA("alcGetString");
	qalcGetIntegerv = (LPALCGETINTEGERV)GPA("alcGetIntegerv");

	if(alinit_fail)
	{
		QAL_Shutdown();
		Com_Printf( " One or more symbols not found\n");
		return qfalse;
	}

	return qtrue;
}


void QAL_Shutdown( void )
{
	if(OpenALLib)
	{
		OBJFREE(OpenALLib);
		OpenALLib = NULL;
	}

	qalEnable = NULL;
	qalDisable = NULL;
	qalIsEnabled = NULL;
	qalGetString = NULL;
	qalGetBooleanv = NULL;
	qalGetIntegerv = NULL;
	qalGetFloatv = NULL;
	qalGetDoublev = NULL;
	qalGetBoolean = NULL;
	qalGetInteger = NULL;
	qalGetFloat = NULL;
	qalGetDouble = NULL;
	qalGetError = NULL;
	qalIsExtensionPresent = NULL;
	qalGetProcAddress = NULL;
	qalGetEnumValue = NULL;
	qalListenerf = NULL;
	qalListener3f = NULL;
	qalListenerfv = NULL;
	qalListeneri = NULL;
	qalGetListenerf = NULL;
	qalGetListener3f = NULL;
	qalGetListenerfv = NULL;
	qalGetListeneri = NULL;
	qalGenSources = NULL;
	qalDeleteSources = NULL;
	qalIsSource = NULL;
	qalSourcef = NULL;
	qalSource3f = NULL;
	qalSourcefv = NULL;
	qalSourcei = NULL;
	qalGetSourcef = NULL;
	qalGetSource3f = NULL;
	qalGetSourcefv = NULL;
	qalGetSourcei = NULL;
	qalSourcePlayv = NULL;
	qalSourceStopv = NULL;
	qalSourceRewindv = NULL;
	qalSourcePausev = NULL;
	qalSourcePlay = NULL;
	qalSourceStop = NULL;
	qalSourceRewind = NULL;
	qalSourcePause = NULL;
	qalSourceQueueBuffers = NULL;
	qalSourceUnqueueBuffers = NULL;
	qalGenBuffers = NULL;
	qalDeleteBuffers = NULL;
	qalIsBuffer = NULL;
	qalBufferData = NULL;
	qalGetBufferf = NULL;
	qalGetBufferi = NULL;
	qalDopplerFactor = NULL;
	qalDopplerVelocity = NULL;
	qalDistanceModel = NULL;

	qalcCreateContext = NULL;
	qalcMakeContextCurrent = NULL;
	qalcProcessContext = NULL;
	qalcSuspendContext = NULL;
	qalcDestroyContext = NULL;
	qalcGetCurrentContext = NULL;
	qalcGetContextsDevice = NULL;
	qalcOpenDevice = NULL;
	qalcCloseDevice = NULL;
	qalcGetError = NULL;
	qalcIsExtensionPresent = NULL;
	qalcGetProcAddress = NULL;
	qalcGetEnumValue = NULL;
	qalcGetString = NULL;
	qalcGetIntegerv = NULL;
}
#else
qbool QAL_Init(const char *libname)
{
	return qtrue;
}
void QAL_Shutdown( void )
{
}
#endif
#endif
