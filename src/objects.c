/*
 * Copyright 2005-2017 by Erik Hofman.
 * Copyright 2009-2017 by Adalin B.V.
 *
 * This file is part of AeonWave
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_ASSERT_H
# include <assert.h>
#endif
#ifdef HAVE_RMALLOC_H
# include <rmalloc.h>
#else
# include <string.h>
#endif
#include <stdio.h>		/* for NULL */
#include <xml.h>

#include <software/gpu/gpu.h>
#include <dsp/filters.h>
#include <dsp/effects.h>
#include "objects.h"
#include "arch.h"
#include "api.h"

void
_aaxSetDefaultInfo(_aaxMixerInfo *info, void *handle)
{
// const char *opencl;
   unsigned int size;

   size = 2*sizeof(vec4f_t); 
   _aax_memcpy(&info->hrtf, &_aaxDefaultHead, size);

   size = _AAX_MAX_SPEAKERS * sizeof(vec4f_t);
   _aax_memcpy(&info->speaker, &_aaxDefaultSpeakersVolume, size);

   info->delay = &info->speaker[_AAX_MAX_SPEAKERS];
   _aax_memcpy(info->delay, &_aaxDefaultSpeakersDelay, size);

   size = _AAX_MAX_SPEAKERS-1;
   do {
      info->router[size] = size;
   } while (size--);

   info->no_tracks = 2;
   info->bitrate = 320;
   info->track = AAX_TRACK_ALL;

   info->pitch = 1.0f;
   info->frequency = 48000.0f;
   info->period_rate = 20.0f;
   info->refresh_rate = 20.0f;
   info->format = AAX_PCM16S;
   info->mode = AAX_MODE_WRITE_STEREO;
   info->max_emitters = _AAX_MAX_MIXER_REGISTERED;
   info->max_registered = 0;
   info->unit_m = 1.0f;

   info->update_rate = 0;
   info->sse_level = _aaxGetSIMDSupportLevel();
   info->no_cores = _aaxGetNoCores();

   info->id = INFO_ID;
   info->backend = handle;

#if 0
   opencl = getenv("AAX_USE_OPENCL");
   if ((!opencl || _aax_getbool(opencl)) && _aaxOpenCLDetect()) {
      info->gpu = _aaxOpenCLCreate();
   }
#endif
}

void
_aaxSetDefault2dFiltersEffects(_aax2dProps *p2d)
{
   unsigned int pos;

   /* Note: skips the volume filter */
   for (pos=DYNAMIC_GAIN_FILTER; pos<MAX_STEREO_FILTER; pos++)
   {
      _aaxSetDefaultFilter2d(&p2d->filter[pos], pos, 0);
      _FILTER_FREE_DATA(p2d, pos);
   }

   /* Note: skips the pitch effect */
   for (pos=REVERB_EFFECT; pos<MAX_STEREO_EFFECT; pos++)
   {
      _aaxSetDefaultEffect2d(&p2d->effect[pos], pos, 0);
      _EFFECT_FREE_DATA(p2d, pos);
   }
}

void
_aaxSetDefault2dProps(_aax2dProps *p2d)
{
   unsigned int size;

   assert (p2d);

   p2d->mutex = _aaxMutexCreate(NULL);
   _aaxSetDefaultFilter2d(&p2d->filter[0], 0, 0); // volume
   _aaxSetDefaultEffect2d(&p2d->effect[0], 0, 0); // pitch
   _aaxSetDefault2dFiltersEffects(p2d);

   /* normalized  directions */
   size = _AAX_MAX_SPEAKERS*sizeof(vec4f_t);
   memset(p2d->speaker, 0, 2*size);

   /* HRTF sample offsets */
   size = 2*sizeof(vec4f_t);
   memset(p2d->hrtf, 0, size);
   memset(p2d->hrtf_prev, 0, size);

   /* HRTF head shadow */
   size = _AAX_MAX_SPEAKERS*sizeof(float);
   memset(p2d->freqfilter_history, 0, size);
   p2d->k = 0.0f;

   /* previous gains */
   size = _AAX_MAX_SPEAKERS*sizeof(float);
   memset(&p2d->prev_gain, 0, size);
   p2d->prev_freq_fact = 0.0f;
   p2d->dist_delay_sec = 0.0f;
   p2d->bufpos3dq = 0.0f;

   p2d->curr_pos_sec = 0.0f;
   p2d->pitch_factor = 1.0f;
#ifdef MIDI
   p2d->note.velocity = 1.0f;		/* MIDI */
   p2d->note.pressure = 1.0f;
#endif

   p2d->final.pitch_lfo = 1.0f;		/* LFO */
   p2d->final.pitch = 1.0f;
   p2d->final.gain_lfo = 1.0f;
   p2d->final.gain = 1.0f;
}

void
_aaxSetDefaultDelayed3dProps(_aaxDelayed3dProps *dp3d)
{
   assert(dp3d);

   /* modelview matrix */
#ifdef ARCH32
   mtx4fSetIdentity(dp3d->matrix.m4);
   mtx4fSetIdentity(dp3d->imatrix.m4);
#else
   mtx4dSetIdentity(dp3d->matrix.m4);
   mtx4dSetIdentity(dp3d->imatrix.m4);
#endif
   _PROP3D_MTX_SET_CHANGED(dp3d);

   /* velocity     */
   mtx4fSetIdentity(dp3d->velocity.m4);
   _PROP3D_SPEED_SET_CHANGED(dp3d);

   /* occlusion */
   memset(dp3d->occlusion.v4, 0, sizeof(vec4f_t));

   /* status */
   dp3d->state3d = 0;
   dp3d->pitch = 1.0f;
   dp3d->gain = 1.0f;
}

_aax3dProps *
_aax3dPropsCreate()
{
   _aax3dProps *rv = NULL;
   char *ptr1, *ptr2;
   size_t offs, size;

   offs = sizeof(_aax3dProps);
   size = sizeof(_aaxDelayed3dProps);
   ptr1 = _aax_calloc(&ptr2, offs, 1, size);
   if (ptr1)
   {
      unsigned int pos;

      rv = (_aax3dProps*)ptr1;
      rv->m_dprops3d = (_aaxDelayed3dProps*)ptr2;

      rv->dprops3d = _aax_aligned_alloc(sizeof(_aaxDelayed3dProps));
      if (rv->dprops3d)
      {
         _SET_INITIAL(rv);

         _aaxSetDefaultDelayed3dProps(rv->dprops3d);
         _aaxSetDefaultDelayed3dProps(rv->m_dprops3d);

         rv->mutex = _aaxMutexCreate(NULL);
         for (pos=0; pos<MAX_3D_FILTER; pos++) {
            _aaxSetDefaultFilter3d(&rv->filter[pos], pos, 0);
         }
         for (pos=0; pos<MAX_3D_EFFECT; pos++) {
            _aaxSetDefaultEffect3d(&rv->effect[pos], pos, 0);
         }
      }
      else
      {
         free(rv);
         rv = NULL;
      }
   }
   return rv;
}

_aaxDelayed3dProps *
_aaxDelayed3dPropsDup(_aaxDelayed3dProps *dp3d)
{
   _aaxDelayed3dProps *rv;

   rv = _aax_aligned_alloc(sizeof(_aaxDelayed3dProps));
   if (rv) {
      _aax_memcpy(rv, dp3d, sizeof(_aaxDelayed3dProps));
   }
   return rv;
}

/* -------------------------------------------------------------------------- */

/* HRTF
 *
 * Left-right time difference (delay):
 * Angle from ahead (azimuth, front = 0deg): (ear-distance)
 *     0 deg =  0.00 ms,                                -- ahead      --
 *    90 deg =  0.64 ms,				-- right/left --
 *   180 deg =  0.00 ms 				-- back       --
 *
 * The outer pinna rim which is important in determining elevation
 * in the vertical plane:
 * Angle from above (0deg = below, 180deg = above): (outer pinna rim)
 *     0 deg = 0.325 ms,                                -- below  --
 *    90 deg = 0.175 ms,                                -- center --
 *   180 deg = 0.100 ms                                 -- above  --
 *
 * The inner pinna ridge which determine front-back directions in the
 * horizontal plane. Front-back distinctions are not uniquely determined
 * by time differences:
 * Angle from right (azimuth, front = 0deg): (inner pinna ridge)
 *     0 deg =  0.080 ms,				-- ahead  --
 *    90 deg =  0.015 ms,				-- center --
 *   135+deg =  0.000 ms				-- back   --
 *
 *        RIGHT   UP        BACK
 * ahead: 0.00ms, 0.175 ms, 0.080 ms
 * left:  0.64ms, 0.175 ms, 0.015 ms
 * right: 0.64ms, 0.175 ms, 0.015 ms
 * back:  0.00ms, 0.175 ms, 0.000 ms
 * up:    0.00ms, 0.100 ms, 0.015 ms
 * down:  0.00ms, 0.325 ms, 0.015 ms
 */
float _aaxDefaultHead[2][4] = 
{
//     RIGHT     UP        BACK
   { 0.000640f, 0.000125f, 0.000120f, 0.0f },	/* head delay factors */
   { 0.000000f, 0.000225f, 0.000020f, 0.0f }	/* head delay offsets */
};

float _aaxDefaultHRTFVolume[_AAX_MAX_SPEAKERS][4] =
{
   /* left headphone shell (volume)                          --- */
   { 1.00f, 0.00f, 0.00f, 1.0f }, 	 /* left-right           */
   { 0.00f,-1.00f, 0.00f, 1.0f }, 	 /* up-down              */
   { 0.00f, 0.00f, 1.00f, 1.0f }, 	 /* back-front           */
   /* right headphone shell (volume)                         --- */
   {-1.00f, 0.00f, 0.00f, 1.0f }, 	 /* left-right           */
   { 0.00f,-1.00f, 0.00f, 1.0f }, 	 /* up-down              */
   { 0.00f, 0.00f, 1.00f, 1.0f }, 	 /* back-front           */
   /* unused                                                     */
   { 0.00f, 0.00f, 0.00f, 0.0f },
   { 0.00f, 0.00f, 0.000, 0.0f }
};

float _aaxDefaultHRTFDelay[_AAX_MAX_SPEAKERS][4] =
{
   /* left headphone shell (delay)                           --- */
   {-1.00f, 0.00f, 0.00f, 0.0f },        /* left-right           */
   { 0.00f,-1.00f, 0.00f, 0.0f },        /* up-down              */
   { 0.00f, 0.00f, 1.00f, 0.0f },        /* back-front           */
   /* right headphone shell (delay)                          --- */
   { 1.00f, 0.00f, 0.00f, 0.0f },        /* left-right           */
   { 0.00f,-1.00f, 0.00f, 0.0f },        /* up-down              */
   { 0.00f, 0.00f, 1.00f, 0.0f },        /* back-front           */
   /* unused                                                     */
   { 0.00f, 0.00f, 0.00f, 0.0f },        
   { 0.00f, 0.00f, 0.000, 0.0f }
};

float _aaxDefaultSpeakersVolume[_AAX_MAX_SPEAKERS][4] =
{
   { 1.00f, 0.00f, 1.00f, 1.0f },	/* front left speaker    */
   {-1.00f, 0.00f, 1.00f, 1.0f },	/* front right speaker   */
   { 1.00f, 0.00f,-1.00f, 1.0f },	/* rear left speaker     */
   {-1.00f, 0.00f,-1.00f, 1.0f },	/* rear right speaker    */
   { 0.00f, 0.00f, 1.00f, 1.0f },	/* front center speaker  */
   { 0.00f, 0.00f, 1.00f, 1.0f },	/* low frequency emitter */
   { 1.00f, 0.00f, 0.00f, 1.0f },	/* left side speaker     */
   {-1.00f, 0.00f, 0.00f, 1.0f }	/* right side speaker    */
};

float _aaxDefaultSpeakersDelay[_AAX_MAX_SPEAKERS][4] =
{
   { 0.00f, 0.00f, 0.00f, 1.0f },       /* front left speaker    */
   { 0.00f, 0.00f, 0.00f, 1.0f },       /* front right speaker   */
   { 0.00f, 0.00f, 0.00f, 1.0f },       /* rear left speaker     */
   { 0.00f, 0.00f, 0.00f, 1.0f },       /* rear right speaker    */
   { 0.00f, 0.00f, 0.00f, 1.0f },       /* front center speaker  */
   { 0.00f, 0.00f, 0.00f, 1.0f },       /* low frequency emitter */
   { 0.00f, 0.00f, 0.00f, 1.0f },       /* left side speaker     */
   { 0.00f, 0.00f, 0.00f, 1.0f }        /* right side speaker    */
};


static unsigned int
_aaxGetSetMonoSources(unsigned int max, int num)
{
   static unsigned int _max_sources = _AAX_MAX_SOURCES_AVAIL;
   static unsigned int _sources = _AAX_MAX_SOURCES_AVAIL;
   unsigned int abs_num = abs(num);
   unsigned int ret = _sources;

   if (max)
   {
      if (max > _AAX_MAX_SOURCES_AVAIL) max = _AAX_MAX_SOURCES_AVAIL;
      _max_sources = max;
      _sources = max;
      ret = max;
   }

   if (abs_num && (abs_num < _AAX_MAX_MIXER_REGISTERED))
   {
      unsigned int _src = _sources - num;
      if ((_sources >= (unsigned int)num) && (_src < _max_sources))
      {
         _sources = _src;
         ret = abs_num;
      }
   }

   return ret;
}

unsigned int
_aaxGetNoEmitters() {
   int rv = _aaxGetSetMonoSources(0, 0);
   if (rv > _AAX_MAX_MIXER_REGISTERED) rv = _AAX_MAX_MIXER_REGISTERED;
   return rv;
}

unsigned int
_aaxSetNoEmitters(unsigned int max) {
   return _aaxGetSetMonoSources(max, 0);
}

unsigned int
_aaxIncreaseEmitterCounter() {
   return _aaxGetSetMonoSources(0, 1);
}

unsigned int
_aaxDecreaseEmitterCounter() {
   return _aaxGetSetMonoSources(0, -1);
}

static void
_aaxSetSlotFromAAXSOld(const char *xid, int (*setSlotFn)(void*, unsigned, int, aaxVec4f), void *id)
{
   unsigned int s, snum = xmlNodeGetNum(xid, "slot");
   void *xsid;

   xsid = xmlMarkId(xid);
   if (!xsid) return;

   for (s=0; s<snum; s++)
   {
      if (xmlNodeGetPos(xid, xsid, "slot", s) != 0)
      {
         if (xmlNodeGetPos(xid, xsid, "slot", s) != 0)
         {
            enum aaxType type = AAX_LINEAR;
            aaxVec4f params;
            unsigned int slen;
            long int n = s;
            char src[65];

            if (xmlAttributeExists(xsid, "n")) {
               n = xmlAttributeGetInt(xsid, "n");
            }

            params[0] = xmlNodeGetDouble(xsid, "p0");
            params[1] = xmlNodeGetDouble(xsid, "p1");
            params[2] = xmlNodeGetDouble(xsid, "p2");
            params[3] = xmlNodeGetDouble(xsid, "p3");

            slen = xmlAttributeCopyString(xsid, "type", src, 64);
            if (slen)
            {
               src[slen] = 0;
               type = aaxGetTypeByName(src);
            }
            setSlotFn(id, n, type, params);
         }
      }
   }
   xmlFree(xsid);
}


static int
_aaxSetSlotFromAAXS(const char *xid, int (*setParamFn)(void*, int, int, float), void *id, float freq)
{
   unsigned int s, snum = xmlNodeGetNum(xid, "slot");
   int rv = AAX_FALSE;
   void *xsid;

   xsid = xmlMarkId(xid);
   if (!xsid) return rv;

   for (s=0; s<snum; s++)
   {
      if (xmlNodeGetPos(xid, xsid, "slot", s) != 0)
      {
         unsigned int p, pnum = xmlNodeGetNum(xsid, "param");
         if (pnum)
         {
            enum aaxType type = AAX_LINEAR;
            unsigned int slen;
            long int sn = s;
            char src[65];
            void *xpid;

            if (xmlAttributeExists(xsid, "n")) {
               sn = xmlAttributeGetInt(xsid, "n");
            }

            slen = xmlAttributeCopyString(xsid, "type", src, 64);
            if (slen)
            {
               src[slen] = 0; 
               type = aaxGetTypeByName(src);
            }

            xpid = xmlMarkId(xsid);
            if (xpid)
            {
               for (p=0; p<pnum; p++)
               {  
                  if (xmlNodeGetPos(xsid, xpid, "param", p) != 0)
                  {
                     int slotnum[_MAX_FE_SLOTS] = { 0x00, 0x10, 0x20, 0x30 };
                     double value = xmlGetDouble(xpid);
                     long int pn = p;

                     if (xmlAttributeExists(xpid, "n")) {
                        pn = xmlAttributeGetInt(xpid, "n");
                     }

                     if (freq != 0.0f)
                     {
                        float pitch, adjust;

                        pitch = xmlAttributeGetDouble(xpid, "pitch");
                        if (pitch != 0.0f) {
                           value = pitch*freq;
                        }
 
                        adjust = xmlAttributeGetDouble(xpid, "auto");
                        if (adjust == 0.0f) {
                           adjust = xmlAttributeGetDouble(xpid, "auto-adjust");
                        }
                        if (adjust != 0.0f) {
                           value = _MAX(value - adjust*_lin2log(freq), 0.1f);
                        }
                     }

                     slen = xmlAttributeCopyString(xpid, "type", src, 64);
                     if (slen)
                     {
                        src[slen] = 0;
                        type = aaxGetTypeByName(src);
                     }
            
                     pn |= slotnum[sn];
                     setParamFn(id, pn, type, value);
                  }
               }
               xmlFree(xpid);
            }
            rv = AAX_TRUE;
         }
      }
   }
   xmlFree(xsid);

   return rv;
}

aaxFilter
_aaxGetFilterFromAAXS(aaxConfig config, const char *xid, float freq)
{
   aaxFilter rv = NULL;
   char src[65];
   int slen;

   slen = xmlAttributeCopyString(xid, "type", src, 64);
   if (slen)
   {
      int state = AAX_CONSTANT_VALUE;
      enum aaxFilterType ftype;
      aaxFilter flt;

      src[slen] = 0;
      ftype = aaxFilterGetByName(config, src);
      flt = aaxFilterCreate(config, ftype);
      if (flt)
      {
         if (!_aaxSetSlotFromAAXS(xid, aaxFilterSetParam, flt, freq)) {
            _aaxSetSlotFromAAXSOld(xid, aaxFilterSetSlotParams, flt);
         }

         if (ftype == AAX_TIMED_GAIN_FILTER)
         {
            if (xmlAttributeExists(xid, "repeat"))
            {
               if (!xmlAttributeCompareString(xid, "repeat", "inf") ||
                   !xmlAttributeCompareString(xid, "repeat", "max")) {
                  state = AAX_REPEAT-1;
               }
               else
               {
                  state = xmlAttributeGetInt(xid, "repeat");
                  if (state < 0) state = 0;
                  else if (state >= AAX_REPEAT) state = AAX_REPEAT-1;
               }
               state |= AAX_REPEAT;
            }
            else if (xmlAttributeExists(xid, "release-factor"))
            {
               state = (10.0f*xmlAttributeGetDouble(xid, "release-factor"));
               if (state < 10) state = 10;
               state |= AAX_RELEASE_FACTOR;
            }
         }
         else
         {
            slen = xmlAttributeCopyString(xid, "src", src, 64);
            if (slen)
            {
               src[slen] = 0;
               if (ftype == AAX_DISTANCE_FILTER) {
                  state = aaxGetDistanceModelByName(src);
               } else if (ftype == AAX_FREQUENCY_FILTER) {
                  state = aaxGetFrequencyFilterTypeByName(src);
               } else {
                  state = aaxGetWaveformTypeByName(src);
               }
            }
         }

         if (xmlAttributeExists(xid, "stereo") &&
             xmlAttributeGetBool(xid, "stereo")) {
            state |= AAX_LFO_STEREO;
         }

         aaxFilterSetState(flt, state);
         rv = flt;
      }
   }

   return rv;
}

aaxEffect
_aaxGetEffectFromAAXS(aaxConfig config, const char *xid, float freq)
{
   aaxEffect rv = NULL;
   char src[65];
   int slen;

   slen = xmlAttributeCopyString(xid, "type", src, 64);
   if (slen)
   {
      enum aaxWaveformType state = AAX_CONSTANT_VALUE;
      enum aaxEffectType etype;
      aaxEffect eff;

      src[slen] = 0;
      etype = aaxEffectGetByName(config, src);
      eff = aaxEffectCreate(config, etype);
      if (eff)
      {
         if (!_aaxSetSlotFromAAXS(xid, aaxEffectSetParam, eff, freq)) {
            _aaxSetSlotFromAAXSOld(xid, aaxEffectSetSlotParams, eff);
         }

         slen = xmlAttributeCopyString(xid, "src", src, 64);
         if (slen)
         {
            src[slen] = 0;
            state = aaxGetWaveformTypeByName(src);
         }

         if (xmlAttributeExists(xid, "stereo") &&
             xmlAttributeGetBool(xid, "stereo")) {
            state |= AAX_LFO_STEREO;
         }

         aaxEffectSetState(eff, state);
         rv = eff;
      }
   }

   return rv;
}
