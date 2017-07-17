/*
 * Copyright (C) 2005-2017 by Erik Hofman.
 * Copyright (C) 2007-2017 by Adalin B.V.
 *
 * This file is part of OpenAL-AeonWave.
 *
 *  OpenAL-AeonWave is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  OpenAL-AeonWave is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenAL-AeonWave.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _OAL_DLSYM_H
#define _OAL_DLSYM_H 1

#if defined(__cplusplus)
extern "C" {
#endif

#ifdef _WIN32
#include <Windows.h>
# define DLL_API		WINAPI
# ifdef STRICT 
#  define DLL_RV		WNDPROC
# else
#  define DLL_RV		FARPROC
# endif
# define TIE_FUNCTION(f)         p##f = (f##_proc)_aaxGetProcAddress(audio, #f)

#else	/* ifdef _WIN32 */
# define DLL_API	
# define DLL_RV			void*
# define TIE_FUNCTION(f)         *(void**)(&p##f) = _aaxGetProcAddress(audio, #f)
#endif

#define DECL_FUNCTION(f)	f##_proc p##f = 0
#define DECL_STATIC_FUNCTION(f)	static f##_proc p##f = 0

#define DECL_VARIABLE(v)	static void* p##v = 0
#define TIE_VARIABLE(v)		p##v = _aaxGetProcAddress(audio, #v)

char *_aaxGetSymError(const char *error);
void *_aaxIsLibraryPresent(const char *name, const char *version);
DLL_RV _aaxGetProcAddress(void *handle, const char *func);
void *_aaxGetGlobalProcAddress(const char *func);

#if defined(__cplusplus)
}  /* extern "C" */
#endif


#endif /* !_OAL_DLSYM_H */
