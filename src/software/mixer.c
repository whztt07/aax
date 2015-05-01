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

#if HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_RMALLOC_H
# include <rmalloc.h>
#else
# include <string.h>
#endif
#include <errno.h>		/* for ETIMEDOUT */
#include <assert.h>

#include <base/threads.h>
#include <dsp/filters.h>
#include <dsp/effects.h>

#include <api.h>
#include <arch.h>
#include <ringbuffer.h>

#include "cpu/rbuf2d_effects.h"
#include "rbuf_int.h"
#include "audio.h"


void
_aaxSoftwareMixerApplyEffects(const void *id, const void *hid, void *drb, const void *props2d)
{
   _aaxDriverBackend *be = (_aaxDriverBackend*)id;
   _aaxRingBufferDelayEffectData* delay_effect;
   _aaxRingBufferFreqFilterData* freq_filter;
   _aaxRingBuffer *rb = (_aaxRingBuffer *)drb;
   _aax2dProps *p2d = (_aax2dProps*)props2d;
   float maxgain, gain;
   int bps, dist_state;

   assert(rb != 0);

   bps = rb->get_parami(rb, RB_BYTES_SAMPLE);
   assert(bps == sizeof(MIX_T));

   delay_effect = _EFFECT_GET_DATA(p2d, DELAY_EFFECT);
   freq_filter = _FILTER_GET_DATA(p2d, FREQUENCY_FILTER);
   dist_state = _EFFECT_GET_STATE(p2d, DISTORTION_EFFECT);
   if (delay_effect || freq_filter || dist_state)
   {
      _aaxRingBufferData *rbi = rb->handle;
      _aaxRingBufferSample *rbd = rbi->sample;
      MIX_T **scratch = (MIX_T**)rb->get_scratch(rb);
      MIX_T *scratch0 = scratch[SCRATCH_BUFFER0];
      MIX_T *scratch1 = scratch[SCRATCH_BUFFER1];
      void* distortion_effect = NULL;
      size_t no_samples, ddesamps = 0;
      unsigned int track, no_tracks;
      MIX_T **tracks;
      void *env;

      if (dist_state) {
         distortion_effect = &p2d->effect[DISTORTION_EFFECT];
      }

      if (delay_effect)
      {
         float f = rb->get_paramf(rb, RB_FREQUENCY);
         /*
          * can not use drbd->dde_samples since it's 10 times as big for the
          * final mixer to accomodate for reverb
          */
         // ddesamps = rb->get_parami(rb, RB_DDE_SAMPLES);
         ddesamps = (size_t)ceilf(f * DELAY_EFFECTS_TIME);
      }

      env = _FILTER_GET_DATA(p2d, TIMED_GAIN_FILTER);

      no_tracks = rb->get_parami(rb, RB_NO_TRACKS);
      no_samples = rb->get_parami(rb, RB_NO_SAMPLES);
      tracks = (MIX_T**)rbd->track;
      for (track=0; track<no_tracks; track++)
      {
         MIX_T *dptr = (MIX_T*)tracks[track];
         MIX_T *ddeptr = dptr - ddesamps;

         /* save the unmodified next effects buffer for later use          */
         /* (scratch buffers have a leading and a trailing effects buffer) */
         DBG_MEMCLR(1, scratch1-ddesamps, no_samples+2*ddesamps, bps);
         _aax_memcpy(scratch1+no_samples, ddeptr+no_samples, ddesamps*bps);

         /* mix the buffer and the delay buffer */
         DBG_MEMCLR(1, scratch0-ddesamps, no_samples+2*ddesamps, bps);
         rbi->effects(rbi->sample, scratch0, dptr, scratch1,
                      0, no_samples, no_samples, ddesamps, track, 0,
                      freq_filter, delay_effect, distortion_effect, env);

         /* copy the unmodified next effects buffer back */
         DBG_MEMCLR(1, dptr-ddesamps, no_samples+ddesamps, bps);
         _aax_memcpy(ddeptr, scratch1+no_samples, ddesamps*bps);

         /* copy the data back from scratch0 to dptr */
         _aax_memcpy(dptr, scratch0, no_samples*bps);
      }
   }

   /*
    * If the requested gain is larger than the maximum capabilities of
    * hardware volume support, adjust the difference here (before the
    * compressor/limiter)
    */
   maxgain = be->param(hid, DRIVER_MAX_VOLUME);
   gain = _FILTER_GET(p2d, VOLUME_FILTER, AAX_GAIN);
   if (gain > maxgain) {
      rb->data_multiply(rb, 0, 0, gain/maxgain);
   }
}

void
_aaxSoftwareMixerPostProcess(const void *id, void *d, const void *s, void *i)
{
   _aaxRingBuffer *rb = (_aaxRingBuffer*)d;
   _sensor_t *sensor = (_sensor_t*)s;
   _aaxMixerInfo *info = (_aaxMixerInfo*)i;
   unsigned char *router = info->router;
   unsigned int track, no_tracks, lfe_track;
   size_t no_samples, track_len_bytes;
   char parametric, graphic, crossover;
   _aaxRingBufferReverbData *reverb;
   _aaxRingBufferSample *rbd;
   _aaxRingBufferData *rbi;
   MIX_T *lfe, **tracks, **scratch;

   assert(rb != 0);
   assert(rb->handle != 0);

   rbi = rb->handle;

   assert(rbi->sample != 0);

   rbi = rb->handle;
   rbd = rbi->sample;

   /* set up this way because we always need to apply compression */
   track_len_bytes = rb->get_parami(rb, RB_TRACKSIZE);
   no_samples = rb->get_parami(rb, RB_NO_SAMPLES);
   no_tracks = rb->get_parami(rb, RB_NO_TRACKS);
   lfe_track = router[AAX_TRACK_LFE];

   reverb = NULL;
   crossover = parametric = graphic = 0;
   if (sensor)
   {
      reverb = _EFFECT_GET_DATA(sensor->mixer->props2d, REVERB_EFFECT);
      parametric = graphic = (_FILTER_GET_DATA(sensor, EQUALIZER_HF) != NULL);
      parametric &= (_FILTER_GET_DATA(sensor, EQUALIZER_LF) != NULL);
      graphic    &= (_FILTER_GET_DATA(sensor, EQUALIZER_LF) == NULL);
      crossover = (_FILTER_GET_DATA(sensor, SURROUND_CROSSOVER) != NULL);
      crossover &= (no_tracks >= lfe_track);
   }

   if (reverb) {
      rb->limit(rb, RB_LIMITER_VALVE);
   }

   tracks = (MIX_T**)rbd->track;
   lfe = tracks[lfe_track];
   scratch = (MIX_T**)rb->get_scratch(rb);
   for (track=0; track<no_tracks; track++)
   {
      if (reverb)
      {
         size_t ds = rb->get_parami(rb, RB_DDE_SAMPLES);

         /* level out previous filters and effects */
         _aaxRingBufferEffectReflections(rbd, tracks[track],
                                         scratch[0], scratch[1], 
                                         0, no_samples, ds, track, reverb);
         _aaxRingBufferEffectReverb(rbd, tracks[track], 0, no_samples,
                                    ds, track, reverb);
      }

      if (parametric)
      {
         _aaxRingBufferFreqFilterData* filter;

         _aax_memcpy(scratch[1], tracks[track], track_len_bytes);
         filter = _FILTER_GET_DATA(sensor, EQUALIZER_LF);
         _aaxRingBufferFilterFrequency(rbd, tracks[track], scratch[1], 0,
                                       no_samples, 0, track, filter, NULL, 0);

         filter = _FILTER_GET_DATA(sensor, EQUALIZER_HF);
         _aaxRingBufferFilterFrequency(rbd, scratch[0], scratch[1], 0,
                                       no_samples, 0, track, filter, NULL, 0);
         rbd->add(tracks[track], scratch[0], no_samples, 1.0f, 0.0f);
      }
      else if (graphic)
      {
         _aaxRingBufferFreqFilterData* filter;
         _aaxRingBufferEqualizerData *eq;
         int b = 6;

         eq = _FILTER_GET_DATA(sensor, EQUALIZER_HF);
         filter = &eq->band[b];
         _aax_memcpy(scratch[1], tracks[track], track_len_bytes);
         _aaxRingBufferFilterFrequency(rbd, tracks[track], scratch[1],
                                     0, no_samples, 0, track, filter, NULL, 0);
         do
         {
            filter = &eq->band[--b];
            if (filter->lf_gain > GMATH_128DB || filter->hf_gain > GMATH_128DB)
            {
               _aaxRingBufferFilterFrequency(rbd, scratch[0], scratch[1],
                                     0, no_samples, 0, track, filter, NULL, 0);
               rbd->add(tracks[track], scratch[0], no_samples, 1.0f, 0.0f);
            }

            filter = &eq->band[--b];
            if (filter->lf_gain > GMATH_128DB || filter->hf_gain > GMATH_128DB)
            {
               _aaxRingBufferFilterFrequency(rbd, scratch[0], scratch[1],
                                     0, no_samples, 0, track, filter, NULL, 0);
               rbd->add(tracks[track], scratch[0], no_samples, 1.0f, 0.0f);
            }

            filter = &eq->band[--b];
            if (filter->lf_gain > GMATH_128DB || filter->hf_gain > GMATH_128DB)
            {
               _aaxRingBufferFilterFrequency(rbd, scratch[0], scratch[1],
                                     0, no_samples, 0, track, filter, NULL, 0);
               rbd->add(tracks[track], scratch[0], no_samples, 1.0f, 0.0f);
            }
         }
         while (b);
      }

      if (crossover && track != lfe_track)
      {
         _aaxRingBufferFreqFilterData* filter;

         rbd->add(lfe, tracks[track], no_samples, 1.0f, 0.0f);

         filter = _FILTER_GET_DATA(sensor, SURROUND_CROSSOVER);
         filter->lf_gain = 0.0f;
         filter->hf_gain = 1.0f;
         filter->hf_gain_prev = 1.0f;
         _aax_memcpy(scratch[1], tracks[track], track_len_bytes);
         _aaxRingBufferFilterFrequency(rbd, tracks[track], scratch[1],
                                     0, no_samples, 0, track, filter, NULL, 0);
      }
   }

   rb->limit(rb, RB_LIMITER_ELECTRONIC);

   // TODO: Surround Sound crossover filtering
   //       1. copy all channels to AAX_TRACK_LFE
   //       2. apply a highpass filter to all channels but the LFE
   //       3. apply limiter
   //       4. apply a lowpass filter to the LFE
   if (crossover)
   {
      _aaxRingBufferFreqFilterData* filter;

      filter = _FILTER_GET_DATA(sensor, SURROUND_CROSSOVER);

      filter->lf_gain = 1.0f;
      filter->hf_gain = 0.0f;
      filter->hf_gain_prev = 1.0f;
      _aaxRingBufferFilterFrequency(rbd, lfe, lfe, 0, no_samples,
                                    0, lfe_track, filter, NULL, 0);
   }
}

void*
_aaxSoftwareMixerThread(void* config)
{
   _handle_t *handle = (_handle_t *)config;
   _intBufferData *dptr_sensor;
   const _aaxDriverBackend *be;
   _aaxRingBuffer *dest_rb;
   _aaxAudioFrame *smixer;
   int state, tracks;
   float delay_sec;
   int res;

   if (!handle || !handle->sensors || !handle->backend.ptr
       || !handle->info->no_tracks) {
      return NULL;
   }

   be = handle->backend.ptr;
   delay_sec = 1.0f/handle->info->period_rate;

   tracks = 2;
   smixer = NULL;
   dest_rb = be->get_ringbuffer(REVERB_EFFECTS_TIME, handle->info->mode);
   if (dest_rb)
   {
      dptr_sensor = _intBufGet(handle->sensors, _AAX_SENSOR, 0);
      if (dptr_sensor)
      {
         _aaxMixerInfo* info;
         _sensor_t* sensor;

         sensor = _intBufGetDataPtr(dptr_sensor);
         smixer = sensor->mixer;
         info = smixer->info;

         tracks = info->no_tracks;
         dest_rb->set_parami(dest_rb, RB_NO_TRACKS, tracks);
         dest_rb->set_format(dest_rb, AAX_PCM24S, AAX_TRUE);
         dest_rb->set_paramf(dest_rb, RB_FREQUENCY, info->frequency);
         dest_rb->set_paramf(dest_rb, RB_DURATION_SEC, delay_sec);
         dest_rb->init(dest_rb, AAX_TRUE);
         dest_rb->set_state(dest_rb, RB_STARTED);

         handle->ringbuffer = dest_rb;
         _intBufReleaseData(dptr_sensor, _AAX_SENSOR);
      }
   }

   dest_rb = handle->ringbuffer;
   if (!dest_rb) {
      return NULL;
   }

   /* get real duration, it might have been altered for better performance */
   delay_sec = dest_rb->get_paramf(dest_rb, RB_DURATION_SEC);

   be->state(handle->backend.handle, DRIVER_PAUSE);
   state = AAX_SUSPENDED;

   _aaxMutexLock(handle->thread.signal.mutex);
   do
   {
      if TEST_FOR_FALSE(handle->thread.started) {
         break;
      }

      if (state != handle->state)
      {
         if (_IS_PAUSED(handle) || (!_IS_PLAYING(handle) && _IS_STANDBY(handle))) {
            be->state(handle->backend.handle, DRIVER_PAUSE);
         }
         else if (_IS_PLAYING(handle) || _IS_STANDBY(handle)) {
            be->state(handle->backend.handle, DRIVER_RESUME);
         }
         state = handle->state;
      }

      /* do all the mixing */
      _aaxSoftwareMixerThreadUpdate(handle, handle->ringbuffer);

      if (handle->finished) {
         _aaxSemaphoreRelease(handle->finished);
      }
      res = _aaxSignalWaitTimed(&handle->thread.signal, delay_sec);
   }
   while (res == AAX_TIMEOUT || res == AAX_TRUE);

   _aaxMutexUnLock(handle->thread.signal.mutex);

   dptr_sensor = _intBufGetNoLock(handle->sensors, _AAX_SENSOR, 0);
   if (dptr_sensor)
   {
      be->destroy_ringbuffer(handle->ringbuffer);
      handle->ringbuffer = NULL;
   }

   return handle;
}

/*-------------------------------------------------------------------------- */

int
_aaxSoftwareMixerPlay(void* rb, const void* devices, const void* ringbuffers, const void* frames, void* props2d, char capturing, const void* sensor, const void* backend, const void* be_handle, const void* fbackend, const void* fbe_handle, char batched)
{
   const _aaxDriverBackend* be = (const _aaxDriverBackend*)backend;
   const _aaxDriverBackend* fbe = (const _aaxDriverBackend*)fbackend;
   _aax2dProps *p2d = (_aax2dProps*)props2d;
   _aaxRingBuffer *dest_rb = (_aaxRingBuffer *)rb;
   float gain;
   int res;

   /** play back all mixed audio */
   gain = _FILTER_GET(p2d, VOLUME_FILTER, AAX_GAIN);
   if TEST_FOR_TRUE(capturing)
   {
      _intBuffers *mixer_ringbuffers = (_intBuffers*)ringbuffers;
      _aaxRingBuffer *new_rb;

      new_rb = dest_rb->duplicate(dest_rb, AAX_TRUE, AAX_FALSE);

      dest_rb->set_state(new_rb, RB_REWINDED);
      _intBufAddData(mixer_ringbuffers, _AAX_RINGBUFFER, new_rb);

      dest_rb = new_rb;
   }

   // NOTE: File backend must be first, it's the only backend that
   //       converts the buffer back to floats when done!
   if (fbe)	/* slaved file-out backend */
   {
      fbe->play(fbe_handle, dest_rb, 1.0f, gain, batched);
   }
   res = be->play(be_handle, dest_rb, 1.0f, gain, batched);

   return res;
}


int
_aaxSoftwareMixerThreadUpdate(void *config, void *drb)
{
   _aaxRingBuffer *rb = (_aaxRingBuffer*)drb;
   _handle_t *handle = (_handle_t *)config;
   const _aaxDriverBackend *be, *fbe = NULL;
   _intBufferData *dptr_sensor;
   char batched;
   int res = 0;

   assert(handle);
   assert(handle->sensors);
   assert(handle->backend.ptr);
   assert(handle->info->no_tracks);

   _aaxTimerStart(handle->timer);

   batched = handle->finished ? AAX_TRUE : AAX_FALSE;

   be = handle->backend.ptr;
   if (handle->file.driver && _IS_PLAYING((_handle_t*)handle->file.driver)) {
      fbe = handle->file.ptr;
   }

   dptr_sensor = _intBufGetNoLock(handle->sensors, _AAX_SENSOR, 0);
   if (dptr_sensor && (_IS_PLAYING(handle) || _IS_STANDBY(handle)))
   {
      void* be_handle = handle->backend.handle;
      void* fbe_handle = handle->file.handle;
      _aaxAudioFrame *smixer = NULL;

      if (handle->info->mode == AAX_MODE_READ)
      {
         _aaxSensorsProcessSensor(handle, rb, NULL, 0, 0);
      }
      else if (_IS_PLAYING(handle))
      {
         dptr_sensor = _intBufGetNoLock(handle->sensors, _AAX_SENSOR, 0);
         if (dptr_sensor)
         {
            _sensor_t *sensor = _intBufGetDataPtr(dptr_sensor);

            smixer = sensor->mixer;
            if (smixer->emitters_3d || smixer->emitters_2d || smixer->frames)
            {
               _aaxDelayed3dProps *sdp3d, *sdp3d_m;
               _aax2dProps sp2d;
               char fprocess = AAX_TRUE;
               size_t size;
               float ssv = 343.3f;
               float sdf = 1.0f;

               size = sizeof(_aaxDelayed3dProps);
               sdp3d = _aax_aligned_alloc16(size);
               sdp3d_m = _aax_aligned_alloc16(size);

               /**
                * copying here prevents locking the listener the whole time
                * it's used for just one time-frame anyhow
                */
               dptr_sensor = _intBufGet(handle->sensors, _AAX_SENSOR, 0);
               if (dptr_sensor)
               {
                  _aaxAudioFrameProcessDelayQueue(smixer);
                  ssv=_EFFECT_GETD3D(smixer,VELOCITY_EFFECT,AAX_SOUND_VELOCITY);
                  sdf=_EFFECT_GETD3D(smixer,VELOCITY_EFFECT,AAX_DOPPLER_FACTOR);

                  _aax_memcpy(&sp2d, smixer->props2d,
                                     sizeof(_aax2dProps));
                  _aax_memcpy(sdp3d, smixer->props3d->dprops3d,
                                      sizeof(_aaxDelayed3dProps));
                  sdp3d_m->state3d = sdp3d->state3d;
                  sdp3d_m->pitch = sdp3d->pitch;
                  sdp3d_m->gain = sdp3d->gain;
                  _PROP_CLEAR(smixer->props3d);
                  _intBufReleaseData(dptr_sensor, _AAX_SENSOR);
               }

               /* read-only data */
               _aax_memcpy(&sp2d.speaker, handle->info->speaker,
                                          2*_AAX_MAX_SPEAKERS*sizeof(vec4_t));
               _aax_memcpy(&sp2d.hrtf, handle->info->hrtf, 2*sizeof(vec4_t));

               /* update the modified properties */
               mtx4Copy(sdp3d_m->matrix, sdp3d->matrix);
               mtx4Mul(sdp3d_m->velocity, sdp3d->matrix, sdp3d->velocity);
#if 0
 if (_PROP3D_MTXSPEED_HAS_CHANGED(sdp3d_m)) {
 printf("matrix:\t\t\t\tvelocity\n");
 PRINT_MATRICES(sdp3d->matrix, sdp3d->velocity);
 printf("modified velocity\n");
 PRINT_MATRIX(sdp3d_m->velocity);
 }
#endif
               /* clear the buffer for use by the subframe */
               rb->set_state(rb, RB_CLEARED);
               rb->set_state(rb, RB_STARTED);

               /* process emitters and registered sensors */
               res = _aaxAudioFrameProcess(rb, sensor, smixer, ssv, sdf,
                                           NULL, NULL, &sp2d, sdp3d, sdp3d_m,
                                           be, be_handle, fprocess, batched);
               /*
                * if the final mixer actually did render something,
                * mix the data.
                */
               res = _aaxSoftwareMixerPlay(rb, smixer->devices,
                                           smixer->play_ringbuffers,
                                           smixer->frames, &sp2d,
                                           smixer->capturing, sensor,
                                           be, be_handle, fbe, fbe_handle,
                                           batched);
               _aax_aligned_free(sdp3d);
               _aax_aligned_free(sdp3d_m);

               if (handle->file.driver)
               {
                  _handle_t *fhandle = (_handle_t*)handle->file.driver;
                  if (_IS_PLAYING(fhandle))
                  {
                     _intBufferData *dptr;

                     dptr = _intBufGet(fhandle->sensors, _AAX_SENSOR, 0);
                     if (dptr)
                     {
                        _sensor_t* sensor = _intBufGetDataPtr(dptr);
                        _aaxAudioFrame *fmixer = sensor->mixer;
                        float dt = smixer->info->period_rate;

                        fmixer->curr_pos_sec += 1.0f/dt;
                        fmixer->curr_sample += res;
                        _intBufReleaseData(dptr, _AAX_SENSOR);
                     }
                  }
               }

               if (smixer->capturing) {
                  _aaxSignalTrigger(&handle->buffer_ready);
               }
            }
         }
      }
      else /* if (_IS_STANDBY(handle) */
      {
         if (handle->info->mode != AAX_MODE_READ)
         {
            if (smixer->emitters_3d || smixer->emitters_2d || smixer->frames)
            {
               dptr_sensor = _intBufGet(handle->sensors, _AAX_SENSOR, 0);
               if (dptr_sensor)
               {
                  _aaxNoneDriverProcessFrame(smixer);
                  _intBufReleaseData(dptr_sensor, _AAX_SENSOR);
               }
            }
         }
      }
   }

   handle->elapsed = _aaxTimerElapsed(handle->timer);
   _aaxTimerStop(handle->timer);

   return res;
}

