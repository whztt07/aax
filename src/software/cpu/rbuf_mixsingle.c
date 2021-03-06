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

/*
 * 1:N ringbuffer mixer functions.
 */

#define PURE_STEREO

#if HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef _DEBUG
# define _DEBUG		0
#endif

#include <assert.h>

#include <api.h>
#include <ringbuffer.h>

#include "software/rbuf_int.h"

void
_aaxRingBufferMixMono16Mono(_aaxRingBufferSample *drbd, CONST_MIX_PTRPTR_T sptr, UNUSED(const unsigned char *router), _aax2dProps *ep2d, unsigned char ch, size_t offs, size_t dno_samples, UNUSED(float fs), float gain, float svol, float evol, UNUSED(char ctr))
{
   MIX_T *dptr = (MIX_T*)drbd->track[0] + offs;
   float vstart, vend, vstep;

   vstart = ep2d->prev_gain[0] * svol;
   vend   = gain * evol;
   vstep  = (vend - vstart) / dno_samples;

// DBG_MEMCLR(!offs, drbd->track[t], drbd->no_samples, sizeof(int32_t));
   drbd->add(dptr, sptr[ch]+offs, dno_samples, vstart, vstep);

   ep2d->prev_gain[0] = vend;
}

void
_aaxRingBufferMixMono16Stereo(_aaxRingBufferSample *drbd, CONST_MIX_PTRPTR_T sptr, const unsigned char *router, _aax2dProps *ep2d, unsigned char ch, size_t offs, size_t dno_samples, UNUSED(float fs), float gain, float svol, float evol, UNUSED(char ctr))
{
   unsigned int t;

   _AAX_LOG(LOG_DEBUG, __func__);

   /** Mix */
   for (t=0; t<drbd->no_tracks; t++)
   {
      MIX_T *dptr = (MIX_T*)drbd->track[router[t]] + offs;
      float vstart, vend, vstep;
      float dir_fact;

      /*
       * dir_fact is speaker and source position dependent.
       * If the source is at the speaker's side of the listener it will
       * always be 1.0 (normal volume) for this speaker, but if it's located
       * at the opposite side of the speaker (the listener is between the
       * speaker and the source) it gradually silences until the source is 
       * directly opposite of the speaker, relative to the listener.
       *
       * 0.8776 = cosf(0.5)
       */
#ifdef PURE_STEREO
      dir_fact = _MINMAX(0.5f+ep2d->speaker[t].v4[DIR_RIGHT], 0.0f, 1.0f);
#else
      dir_fact = _MIN(0.8776f + ep2d->speaker[t].v4[DIR_RIGHT], 1.0f);
#endif
      vstart = ep2d->prev_gain[t] * svol;
      vend   = gain * dir_fact * evol;
      vstep  = (vend - vstart) / dno_samples;

//    DBG_MEMCLR(!offs, drbd->track[t], drbd->no_samples, sizeof(int32_t));
      drbd->add(dptr, sptr[ch]+offs, dno_samples, vstart, vstep);

      ep2d->prev_gain[t] = vend;
   }
}

void
_aaxRingBufferMixMono16Surround(_aaxRingBufferSample *drbd, CONST_MIX_PTRPTR_T sptr, const unsigned char *router, _aax2dProps *ep2d, unsigned char ch, size_t offs, size_t dno_samples, UNUSED(float fs), float gain, float svol, float evol, UNUSED(char ctr))
{
   unsigned int t;

   _AAX_LOG(LOG_DEBUG, __func__);

   /** Mix 
    * 1. Copy all channels to LFE and aplly a 40Hz-200Hz (80Hz default)
    *    low pass filter. (It’s not that 100 Hz is really that easy to localize,
    *    but that frequencies a bit above it are.)
    * 2. Apply a high-pass filter with the same cut off frequency to all tracks
    *    but the mains (front left and right) and the LFE.
    *
    * http://www.hometheaterhifi.com/volume_9_3/feature-article-multiple-crossovers-9-2002.html
    * If you want consistent bass response from each channel of your 5.1 system,
    * in our opinion, you're best to set all speakers to "Small", set them all
    * to the same crossover point, and set that point no lower than what you
    * are comfortable throwing away from the LFE channel.  If your main left
    * and right speakers are genuinely full range (be honest now!), then you
    * are better off running them full range as opposed to high-passing them
    * at a ridiculously low frequency.
    *
    *http://www.hometheaterhifi.com/volume_12_2/feature-article-slope-troubles-6-2005.html
    * The up front solution lies in getting receiver and processor manufacturers
    * to start giving us a bass management scheme that caters to "full-range"
    * speakers.  Taking the THX Linkwitz/Riley scheme as a good place to start,
    * all they need to do is provide a choice of high-pass:  2nd order for THX
    * speakers (and other true satellites), and 4th order for all others.  With
    * such a crossover, the main speakers need only be reasonably flat to an
    * octave below the chosen frequency, which, in the case of the common 80 Hz
    * crossover, means being flat to 40 Hz.  Virtually all "full-range" speakers
    * - even most of the smaller bookshelf models - qualify.
    *
    * This is applied in src/software/mixer.c: _aaxSoftwareMixerPostProcess()
    */
   for (t=0; t<drbd->no_tracks; t++)
   {
      MIX_T *dptr = (MIX_T*)drbd->track[router[t]] + offs;
      float vstart, vend, vstep;
      float dir_fact;
      float hrtf_volume[3];
      int i;

      /**
       * horizontal positioning, left-right
       **/
#ifdef USE_SPATIAL_FOR_SURROUND
      dir_fact = ep2d->speaker[t].v4[DIR_RIGHT];
#else
      dir_fact = _MIN(0.8776f + ep2d->speaker[t].v4[DIR_RIGHT], 1.0f);
#endif
      vstart = ep2d->prev_gain[t] * svol;
      vend = gain * evol;
      vstep = (vend - vstart) / dno_samples;

      drbd->add(dptr, sptr[ch]+offs, dno_samples, dir_fact*vstart, vstep);

      ep2d->prev_gain[t] = vend;

      /**
       * vertical positioning
       **/
      dir_fact = ep2d->speaker[t].v4[DIR_UPWD];
      hrtf_volume[DIR_UPWD] = 0.1f + 0.125f*dir_fact;
      gain *= 0.76923f; 		/* 1.0f/0.3f */

      i = DIR_UPWD;			/* skip left-right and back-front */
      do
      {
         ssize_t diff = (ssize_t)ep2d->hrtf[t].v4[i];
         float v_start, v_step;

         if (hrtf_volume[i] <= LEVEL_64DB) { // || (i > 0 && diff == 0)) {
            continue;
         }

         assert(diff < (ssize_t)drbd->dde_samples);
         assert(diff > -(ssize_t)dno_samples);
         diff = _MINMAX(diff, -(ssize_t)dno_samples,(ssize_t)drbd->dde_samples);

         v_start = vstart * hrtf_volume[i];
         v_step = ((vend - vstart) * hrtf_volume[i])/dno_samples;

//       DBG_MEMCLR(!offs, drbd->track[t], drbd->no_samples, sizeof(int32_t));

// TODO: add HF filtered version of sptr[ch] if (t != AAX_TRACK_LFE)
// TODO: or add LF filtered verson of sptr[ch] if (t == AAX_TRACK_LFE)
         drbd->add(dptr, sptr[ch]+offs-diff, dno_samples, v_start, v_step);
      }
      while(0);
   }
}

void
_aaxRingBufferMixMono16Spatial(_aaxRingBufferSample *drbd, CONST_MIX_PTRPTR_T sptr, const unsigned char *router, _aax2dProps *ep2d, unsigned char ch, size_t offs, size_t dno_samples, UNUSED(float fs), float gain, float svol, float evol, UNUSED(char ctr))
{
   unsigned int t;

   _AAX_LOG(LOG_DEBUG, __func__);

   /** Mix */
   for (t=0; t<drbd->no_tracks; t++)
   {
      MIX_T *dptr = (MIX_T*)drbd->track[router[t]] + offs;
      float vstart, vend, vstep;
      float dir_fact;

      dir_fact = ep2d->speaker[t].v4[DIR_RIGHT];
      vstart = ep2d->prev_gain[t] * svol;
      vend   = gain * dir_fact * evol;
      vstep  = (vend - vstart) / dno_samples;

//    DBG_MEMCLR(!offs, drbd->track[t], drbd->no_samples, sizeof(int32_t));
      drbd->add(dptr, sptr[ch]+offs, dno_samples, vstart, vstep);

      ep2d->prev_gain[t] = vend;
   }
}

// http://www.sfu.ca/sonic-studio/handbook/Binaural_Hearing.html
// http://www.cns.nyu.edu/~david/courses/perception/lecturenotes/localization/localization-slides/Slide18.jpg
void
_aaxRingBufferMixMono16HRTF(_aaxRingBufferSample *drbd, CONST_MIX_PTRPTR_T sptr, const unsigned char *router, _aax2dProps *ep2d, unsigned char ch, size_t offs, size_t dno_samples, float fs, float gain, UNUSED(float svol), float evol, char ctr)
{
   unsigned int t;

   _AAX_LOG(LOG_DEBUG, __func__);

   for (t=0; t<drbd->no_tracks; t++)
   {
      MIX_T *track = drbd->track[router[t]];
      float dir_fact[3], hrtf_volume[3];
      const MIX_T *ptr;
      MIX_T *dptr;
      int i;

      /*
       * IID; Interaural Intensitive Difference
       */

      /**
       * vertical positioning
       **/
      dir_fact[DIR_UPWD] = ep2d->speaker[t].v4[DIR_UPWD];
      hrtf_volume[DIR_UPWD] = -0.125f*dir_fact[DIR_UPWD];

      /**
       * horizontal positioning, back-front
       **/
      dir_fact[DIR_BACK] = ep2d->speaker[t].v4[DIR_BACK];
      hrtf_volume[DIR_BACK] = 0.175f + 0.05f*dir_fact[DIR_BACK];

      /**
       * horizontal positioning, left-right
       * WARNING: must be last: dir_fact is used for head shadow filtering
       **/
      dir_fact[DIR_RIGHT] = ep2d->speaker[t].v4[DIR_RIGHT];
      hrtf_volume[DIR_RIGHT] = dir_fact[DIR_RIGHT];

#if 0
 printf("t: %i, lr: %3.2f (%5.4f ms), ud: %3.2f (%5.4f ms), bf: %3.2f (%5.4f ms)\n", t, hrtf_volume[DIR_RIGHT], 1000*ep2d->hrtf[t].v4[0]/44100.0f, hrtf_volume[DIR_UPWD], 1000*ep2d->hrtf[t].v4[1]/44100.0f, hrtf_volume[DIR_BACK], 1000*ep2d->hrtf[t].v4[2]/44100.0f);
#endif
      gain = _MIN(gain, 0.69f);	// compensate for a combined gain of 1.45 below

      dptr = track+offs;
      ptr = sptr[ch]+offs;
      for (i=0; i<3; i++)
      {
         ssize_t diff = (ssize_t)ep2d->hrtf[t].v4[i];
         float v_start, v_end, v_step;

         if (hrtf_volume[i] <= LEVEL_64DB) { // || (i > 0 && diff == 0))
            continue;
         }

         assert(diff < (ssize_t)drbd->dde_samples);
         assert(diff > -(ssize_t)dno_samples);
         diff = _MINMAX(diff, -(ssize_t)dno_samples,(ssize_t)drbd->dde_samples);
 
         v_start = ep2d->prev_gain[3*t+i];
         v_end = gain * hrtf_volume[i] * evol;
         v_step = (v_end - v_start)/dno_samples;

//       DBG_MEMCLR(!offs, drbd->track[t], drbd->no_samples, sizeof(int32_t));
         drbd->add(dptr, ptr-diff, dno_samples, v_start, v_step);

         ep2d->prev_gain[3*t+i] = v_end;
      }

      /* HEAD shadow frequency filter */
      if (dir_fact[DIR_RIGHT] < 0.0f)
      {
         float dirfact;
         float *hist;

         dirfact = dir_fact[DIR_RIGHT];

// http://www.cns.nyu.edu/~david/courses/perception/lecturenotes/localization/localization-slides/Slide18.jpg
         if (!ctr)
         {
            float fc;

            // dirfact = 0.0f: 20kHz, dirfact = -1.0f: 250Hz
            // log10(20000 - 1000) = 4.2787541
            fc = 20000.0f - _log2lin(-4.278754f*dirfact);
            ep2d->k = _aax_movingaverage_compute(fc, fs);
         }

         hist = &ep2d->freqfilter_history[t];
#if RB_FLOAT_DATA
         _batch_movingaverage_float(dptr, dptr, dno_samples, hist, ep2d->k);
#else
         _batch_movingaverage(dptr, dptr, dno_samples, hist, ep2d->k);
#endif
      }
   }
}

