/*
 * Copyright (C) 2005-2011 by Erik Hofman.
 * Copyright (C) 2007-2011 by Adalin B.V.
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

#if HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_TIME_H
# include <time.h>             /* for nanosleep */
#endif
#if HAVE_SYS_TIME_H
# include <sys/time.h>         /* for struct timeval */
#endif
#include <errno.h>

#include "timer.h"


#ifdef WIN32
/*
   Implementation as per:
   The Open Group Base Specifications, Issue 6
   IEEE Std 1003.1, 2004 Edition

   The timezone pointer arg is ignored.  Errors are ignored.
*/
int gettimeofday(struct timeval* p, void* tz /* IGNORED */)
{
   union {
      long long ns100; /*time since 1 Jan 1601 in 100ns units */
      FILETIME ft;
   } now;

   GetSystemTimeAsFileTime( &(now.ft) );
   p->tv_usec = (long)((now.ns100 / 10LL) % 1000000LL );
   p->tv_sec = (long)((now.ns100-(116444736000000000LL))/10000000LL);
   return 0;
}

int clock_gettime(int clk_id, struct timespec *p)
{
   union {
      long long ns100; /*time since 1 Jan 1601 in 100ns units */
      FILETIME ft;
   } now;

   GetSystemTimeAsFileTime( &(now.ft) );
   p->tv_nsec = (long)((now.ns100 * 100LL) % 1000000000LL );
   p->tv_sec = (long)((now.ns100-(116444736000000000LL))/10000000LL);
   return 0;
}

int msecSleep(unsigned int dt_ms)
{
   DWORD res = SleepEx((DWORD)dt_ms, 0);
   return (res != 0) ? -1 : 0;
}

unsigned int
getTimerResolution()
{
   _aaxTimer* timer = _aaxTimerCreate();
   double dt;

   _aaxTimerStart(timer);
   SleepEx(1, 0);
   dt = _aaxTimerElapsed(timer);
   _aaxTimerDestroy(timer);

   return 1000*dt;
}

int
setTimerResolution(unsigned int dt_ms) {
   return timeBeginPeriod(dt_ms);
}

int
resetTimerResolution(unsigned int dt_ms) {
   return timeEndPeriod(dt_ms);
}

/** highres timing code */
_aaxTimer*
_aaxTimerCreate()
{
   LARGE_INTEGER timerFreq;
   _aaxTimer *rv = NULL;

   if (QueryPerformanceFrequency(&timerFreq))
   {
      rv = malloc(sizeof(_aaxTimer));
      if (rv)
      {
         DWORD_PTR threadMask = SetThreadAffinityMask(GetCurrentThread(), 0);
         threadMask = SetThreadAffinityMask(GetCurrentThread(), 0);
         QueryPerformanceCounter(&rv->timerCount);
         QueryPerformanceCounter(&rv->timerOverhead);
         SetThreadAffinityMask(GetCurrentThread(), threadMask);

         rv->timerOverhead.QuadPart -= rv->timerCount.QuadPart;

         rv->prevTimerCount.QuadPart = rv->timerCount.QuadPart;
         rv->tfreq = (double)timerFreq.QuadPart;
      }
   }
   return rv;
}

double
_aaxTimerGetFrequency(_aaxTimer *tm)
{
   double rv = 0;
   if (tm) {
      rv = tm->tfreq;
   }
   return rv;
}

void
_aaxTimerStart(_aaxTimer* tm) {
   _aaxTimerElapsed(tm);
}

double
_aaxTimerElapsed(_aaxTimer* tm)
{
   double rv = 0;
   if (tm)
   {
      DWORD_PTR threadMask;

      tm->prevTimerCount.QuadPart = tm->timerCount.QuadPart;
      tm->prevTimerCount.QuadPart += tm->timerOverhead.QuadPart;
   
      threadMask = SetThreadAffinityMask(GetCurrentThread(), 0);
      QueryPerformanceCounter(&tm->timerCount);
      SetThreadAffinityMask(GetCurrentThread(), threadMask);

      rv = (tm->timerCount.QuadPart - tm->prevTimerCount.QuadPart)/tm->tfreq;
   }
   return rv;
}

void
_aaxTimerDestroy(_aaxTimer* tm)
{
   free(tm);
}
/* end of highres timing code */

#else	/* if defined(WIN32) */

/*
 * dt_ms == 0 is a special case which make the time-slice available for other
 * waiting processes
 */
int msecSleep(unsigned int dt_ms)
{
   static struct timespec s;
   if (dt_ms > 0)
   {
      s.tv_sec = (dt_ms/1000);
      s.tv_nsec = (dt_ms % 1000)*1000000L;
      while(nanosleep(&s,&s)==-1 && errno == EINTR)
         continue;
   }
   else
   {
      s.tv_sec = 0;
      s.tv_nsec = 500000L;
      return nanosleep(&s, 0);
   }
   return 0;
}

unsigned int
getTimerResolution() {
   return 0;
}

int
setTimerResolution(unsigned int dt_ms) {
   return 0;
}

int
resetTimerResolution(unsigned int dt_ms) {
   return 0;
}

/** highres timing code */
static void
__aaxTimerSub(struct timespec *tso,
               struct timespec *ts1, struct timespec *ts2)
{
   double dt1, dt2;

   dt2 = ts2->tv_sec + ts2->tv_nsec/1000000000.0;
   dt1 = ts1->tv_sec + ts1->tv_nsec/1000000000.0;
   dt1 -= dt2;

   dt2 = floor(dt1);
   tso->tv_sec = dt2;
   tso->tv_nsec = rint((dt1-dt2)*1000000000.0);
}

static void
__aaxTimerAdd(struct timespec *tso,
               struct timespec *ts1, struct timespec *ts2)
{
   double dt1, dt2;

   dt2 = ts2->tv_sec + ts2->tv_nsec/1000000000.0;
   dt1 = ts1->tv_sec + ts1->tv_nsec/1000000000.0;
   dt1 -= dt2;

   dt2 = floor(dt1);
   tso->tv_sec = dt2;
   tso->tv_nsec = rint((dt1-dt2)*1000000000.0);
}

_aaxTimer*
_aaxTimerCreate()
{
   _aaxTimer *rv = malloc(sizeof(_aaxTimer));
   if (rv)
   {
      int res = clock_getres(CLOCK_MONOTONIC, &rv->timerCount);
      if (!res)
      {
         time_t sec = rv->timerCount.tv_sec;
         long nsec = rv->timerCount.tv_nsec;

         rv->tfreq = 1.0/(sec + nsec/1000000000.0);

         res = clock_gettime(CLOCK_MONOTONIC, &rv->prevTimerCount);
         res |= clock_gettime(CLOCK_MONOTONIC, &rv->timerCount);
         if (!res) {
            __aaxTimerSub(&rv->timerOverhead,
                           &rv->timerCount, &rv->prevTimerCount);
         }
      }
      
      if (res == -1)
      {
         free(rv);
         rv = NULL;
      }
   }
   return rv;
}

double
_aaxTimerGetFrequency(_aaxTimer *tm)
{
   double rv = 0;
   if (tm) {
      rv = tm->tfreq;
   }
   return rv;
}

void
_aaxTimerStart(_aaxTimer *tm) {
   _aaxTimerElapsed(tm);
}

double
_aaxTimerElapsed(_aaxTimer *tm)
{
   double rv = 0;
   if (tm)
   {
      int res;

      __aaxTimerAdd(&tm->prevTimerCount, &tm->timerCount, &tm->timerOverhead);

      res = clock_gettime(CLOCK_MONOTONIC, &tm->timerCount);
      if (!res)
      {
         double t1, t2;

         t1 = tm->prevTimerCount.tv_sec+tm->prevTimerCount.tv_nsec/1000000000.0;
         t2 = tm->timerCount.tv_sec + tm->timerCount.tv_nsec/1000000000.0;
         rv = (t2-t1);
      }
   }
   return rv;
}

void
_aaxTimerDestroy(_aaxTimer *tm)
{
   free(tm);
}
/* end of highres timing code */

#endif	/* if defined(WIN32) */

