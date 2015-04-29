/*
 * Copyright 2005-2014 by Erik Hofman.
 * Copyright 2009-2014 by Adalin B.V.
 * All Rights Reserved.
 *
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Adalin B.V.;
 * the contents of this file may not be disclosed to third parties, copied or
 * duplicated in any form, in whole or in part, without the prior written
 * permission of Adalin B.V.
 */

/*
 * 1:N ringbuffer mixer functions.
 */

/*
 * Sources:
 * http://en.wikipedia.org/wiki/Doppler_effect
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef _DEBUG
# define _DEBUG		0
#endif

#include <assert.h>

#include <base/logging.h>
#include <base/threads.h>

#include <dsp/filters.h>
#include <dsp/effects.h>

#include <api.h>

#include <ringbuffer.h>

#include "rbuf_int.h"
#include "audio.h"

/**
 * Mix a single track source buffer into a multi track destination buffer.
 * The result will be a stereo-mixed multitrack recording.
 *
 * When the source buffer has more tracks than the destination buffer the
 * first remaining source track will be mixed with the first destination
 * buffer and so on.
 *
 * @param drbi multi track destination buffer
 * @param srbi single track source buffer
 * @param mode requested mixing mode
 * @param ep2d 3d positioning information structure of the source
 * @param fp2f 3d positioning information structure of the parents frame
 * @param ch channel to use from the source buffer if it is multi-channel
 * #param ctr update-rate counter:
 *     - Rendering to the destination buffer is done every frame at the
 *       interval rate. Updating of 3d properties and the like is done
 *       once every 'ctr' frame updates. so if ctr == 1, updates are
 *       done every frame.
 * @param streaming weather the emitter has one buffer or more
 */
int
_aaxRingBufferMixMono16(_aaxRingBuffer *drb, _aaxRingBuffer *srb, const _aaxMixerInfo *info, _aax2dProps *ep2d, _aax2dProps *fp2d, unsigned char ch, unsigned char ctr, unsigned int streaming, float pos_sec)
{
   _aaxRingBufferData *drbi, *srbi;
   _aaxRingBufferSample *drbd;
   _aaxRingBufferEnvelopeData* env;
   _aaxRingBufferLFOData *lfo;
   CONST_MIX_PTRPTR_T sptr;
   size_t offs, dno_samples;
   float gain, svol, evol;
   float pitch, max, nvel;
   int ret = 0;

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(drb != NULL);
   assert(srb != NULL);

   drbi = drb->handle;
   srbi = srb->handle;
   assert(srbi != 0);
   assert(drbi != 0);
   assert(srbi->sample != 0);
   assert(drbi->sample != 0);
   assert(ep2d != 0);

   drbd = drbi->sample;

   /** Pitch */
   pitch = ep2d->final.pitch; /* Doppler effect */
   pitch *= _EFFECT_GET(ep2d, PITCH_EFFECT, AAX_PITCH);

   env = _EFFECT_GET_DATA(ep2d, TIMED_PITCH_EFFECT);
   lfo = _EFFECT_GET_DATA(ep2d, DYNAMIC_PITCH_EFFECT);
   if (lfo) 
   {
      float pval = lfo->get(lfo, env, NULL, 0, 0)-1.0f;
      if (fp2d) pval *= fp2d->final.pitch_lfo;
      pitch *= NORM_TO_PITCH(pval+1.0f);
   }

   if (fp2d) {
      pitch *= _EFFECT_GET(fp2d, PITCH_EFFECT, AAX_PITCH);
   }

   if (ep2d->note.velocity == 1.0f) {
      nvel = 1.0f;
   } else {
      nvel = powf(ep2d->note.velocity, ep2d->curr_pos_sec);
   }
   pitch *= _aaxRingBufferEnvelopeGet(env, srbi->stopped, nvel);
   pitch *= ep2d->note.pressure;

   max = _EFFECT_GET(ep2d, PITCH_EFFECT, AAX_MAX_PITCH);
   pitch = _MINMAX(pitch, 0.0f, max);

   /** Resample */
   offs = (drbi->mode == AAX_MODE_WRITE_HRTF) ? drbi->sample->dde_samples : 0;
   sptr = drbi->mix(drbi, srbi, ep2d, pitch, &offs, &dno_samples, ctr, streaming);
   if (sptr == NULL || dno_samples == 0)
   {
      if (srbi->playing == 0 && srbi->stopped == 1) {
         return -1;
      } else {
         return 0;
      }
   }

   /** Volume */
   env = _FILTER_GET_DATA(ep2d, TIMED_GAIN_FILTER);
   if (srbi->playing == 0 && srbi->stopped == 1)
   {
      /* the emitter was already flagged as stopped */
      ret = -1;
   }
   else if (!env && srbi->stopped == 1)
   {
      /*
       * Distance delay induced stopping of playback
       * In the event that distance delay is not active dist_delay_sec equals
       * to 0 so detracting duration_sec instantly turns dist_delay_sec < 0.0
       */
      ep2d->dist_delay_sec -= drbi->sample->duration_sec;
      if (ep2d->dist_delay_sec <= 0.0f) {
         ret = -1;
      }
   }

   /* apply envelope filter */
   gain = _aaxRingBufferEnvelopeGet(env, srbi->stopped, nvel);
   gain *= ep2d->note.pressure;
   if (gain < -1e-3f) {
      ret = -1;
   }

   /* 3d: distance and audio-cone related gain */
   gain *= ep2d->final.gain;

   /* apply the parent mixer/audio-frame volume and tremolo-gain */
   max = 1.0f;
   if (fp2d)
   {
      gain *= _FILTER_GET(fp2d, VOLUME_FILTER, AAX_GAIN);
      max *= fp2d->final.gain_lfo;
   }

   /* tremolo and envelope following gain filter */
   lfo = _FILTER_GET_DATA(ep2d, DYNAMIC_GAIN_FILTER);
   if (lfo)
   {
      if (lfo->envelope)
      {
         float g = lfo->get(lfo, env, sptr[ch]+offs, 0, dno_samples);
         if (lfo->inv) g = 1.0f/g;
         gain *= g;
      }
      else {
         max *= lfo->get(lfo, env, NULL, 0, 0);
      }
   }

   /* tremolo was defined */
   if (max != 1.0f) {
      gain *= 1.0f - max/2.0f;
   }

   /* final emitter volume */
   gain *= _FILTER_GET(ep2d, VOLUME_FILTER, AAX_GAIN);

   /** Automatic volume ramping to avoid clicking */
   svol = evol = 1.0f;
   if (!env && !srbi->streaming && (srbi->playing == srbi->stopped))
   {
      svol = (srbi->stopped || offs) ? 1.0f : 0.0f;
      evol = (srbi->stopped) ? 0.0f : 1.0f;
      srbi->playing = !srbi->stopped;
   }

   /* Mix */
   drbd->mix1n(drbd, sptr, info->router, ep2d, ch, offs, dno_samples, gain, svol, evol);

   if (drbi->playing == 0 && drbi->stopped == 1) {
      ret = 0;
   }

   return ret;
}

