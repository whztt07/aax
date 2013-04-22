/*
 * Copyright 2005-2012 by Erik Hofman.
 * Copyright 2009-2012 by Adalin B.V.
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

#include <string.h>		/* for memset */
#include <errno.h>		/* for ETIMEDOUT */
#include <assert.h>

#include <base/timer.h>		/* for msecSleep, gettimeofday */
#include <base/threads.h>
#include <ringbuffer.h>
#include <arch.h>
#include <api.h>


void
_aaxSoftwareMixerApplyEffects(const void *id, const void *hid, void *drb, const void *props2d)
{
   _aaxDriverBackend *be = (_aaxDriverBackend*)id;
   _oalRingBuffer2dProps *p2d = (_oalRingBuffer2dProps*)props2d;
   _oalRingBuffer *rb = (_oalRingBuffer *)drb;
   _oalRingBufferFreqFilterInfo* freq_filter;
   _oalRingBufferDelayEffectData* delay;
   _oalRingBufferSample *rbd;
   float maxgain, gain = 1.0f;
   int dist_state;

   assert(rb != 0);
   assert(rb->sample != 0);

   rbd = rb->sample;

   maxgain = be->param(hid, DRIVER_MAX_VOLUME);
   gain = _FILTER_GET(p2d, VOLUME_FILTER, AAX_GAIN);

   delay = _EFFECT_GET_DATA(p2d, DELAY_EFFECT);
   freq_filter = _FILTER_GET_DATA(p2d, FREQUENCY_FILTER);
   dist_state = _EFFECT_GET_STATE(p2d, DISTORTION_EFFECT);
   if ((gain > maxgain) || (delay || freq_filter || dist_state))
   {
      int32_t *scratch0 = rbd->scratch[SCRATCH_BUFFER0];
      int32_t *scratch1 = rbd->scratch[SCRATCH_BUFFER1];
      unsigned int bps, no_samples, ddesamps;
      unsigned int track, tracks;
      void* distortion = NULL;

      bps = rbd->bytes_sample;
      ddesamps = rbd->dde_samples;
      no_samples = rbd->no_samples;

      if (dist_state) {
         distortion = &_EFFECT_GET(p2d, DISTORTION_EFFECT, 0);
      }

      tracks = rbd->no_tracks;
      for (track=0; track<tracks; track++)
      {
         int32_t *dptr = rbd->track[track];
         int32_t *ddeptr = dptr - ddesamps;

         /* save the unmodified next effects buffer for later use          */
         /* (scratch buffers have a leading and a trailing effects buffer) */
         DBG_MEMCLR(1, scratch1-ddesamps, no_samples+2*ddesamps, bps);
         _aax_memcpy(scratch1+no_samples, ddeptr+no_samples, ddesamps*bps);

         /* mix the buffer and the delay buffer */
         DBG_MEMCLR(1, scratch0-ddesamps, no_samples+2*ddesamps, bps);
         bufEffectsApply(scratch0, dptr, scratch1, 0, no_samples, no_samples,
                         ddesamps, track, 0, freq_filter, delay, distortion);

         /* copy the unmodified next effects buffer back */
         DBG_MEMCLR(1, dptr-ddesamps, no_samples+ddesamps, bps);
         _aax_memcpy(ddeptr, scratch1+no_samples, ddesamps*bps);

         /* copy the data back from scratch0 to dptr */
         _aax_memcpy(dptr, scratch0, no_samples*bps);

         /*
          * If the requested gain is larger than the maximum capabilities of
          * hardware volume support, adjust the difference here (before the
          * compressor/limiter)
          */
         if (gain > maxgain) {
            _batch_mul_value(dptr, sizeof(int32_t), no_samples, gain/maxgain);
         }
      }
   }
}

void
_aaxSoftwareMixerPostProcess(const void *id, void *d, const void *s)
{
   _oalRingBuffer *rb = (_oalRingBuffer*)d;
   _sensor_t *sensor = (_sensor_t*)s;
   _oalRingBufferReverbData *reverb;
   unsigned int maxavg, maxpeak;
   unsigned int average, peak;
   unsigned int track, tracks;
   _oalRingBufferSample *rbd;
   char parametric, graphic;
   void *ptr = 0;
   float dt;
   char *p;

   assert(rb != 0);
   assert(rb->sample != 0);

   rbd = rb->sample;
   dt = _MINMAX(_oalRingBufferGetParamf(rb, RB_DURATION_SEC)*50.0f, 0.0f, 1.0f);

   reverb = 0;
   parametric = graphic = 0;
   if (sensor)
   {
      reverb = _EFFECT_GET_DATA(sensor->mixer->props2d, REVERB_EFFECT);
      parametric = graphic = (_FILTER_GET_DATA(sensor, EQUALIZER_HF) != NULL);
      parametric &= (_FILTER_GET_DATA(sensor, EQUALIZER_LF) != NULL);
      graphic    &= (_FILTER_GET_DATA(sensor, EQUALIZER_LF) == NULL);

      if (parametric || graphic || reverb)
      {
         unsigned int size = 2*rbd->track_len_bytes;
         if (reverb) size += rbd->dde_samples*rbd->bytes_sample;
         p = 0;
         ptr = _aax_malloc(&p, size);
         // TODO: create only once
      }
   }

   /* set up this way because we always need to apply compression */
   maxavg = maxpeak = 0;
   tracks = rbd->no_tracks;
   for (track=0; track<tracks; track++)
   {
      int32_t *d1 = (int32_t *)rbd->track[track];
      unsigned int dmax = rbd->no_samples;

      if (ptr && reverb)
      {
         unsigned int ds = rbd->dde_samples;
         int32_t *sbuf = (int32_t *)p + ds;
         int32_t *sbuf2 = sbuf + dmax;

         /* level out previous filters and effects */
         average = 0;
         peak = dmax;
         _aaxProcessCompression(d1, &average, &peak);
         bufEffectReflections(d1, sbuf, sbuf2, 0, dmax, ds, track, reverb);
         bufEffectReverb(d1, 0, dmax, ds, track, reverb);
      }

      if (ptr && parametric)
      {
         _oalRingBufferFreqFilterInfo* filter;
         int32_t *d2 = (int32_t *)p;
         int32_t *d3 = d2 + dmax;

         _aax_memcpy(d3, d1, rbd->track_len_bytes);
         filter = _FILTER_GET_DATA(sensor, EQUALIZER_LF);
         bufFilterFrequency(d1, d3, 0, dmax, 0, track, filter, 0);

         filter = _FILTER_GET_DATA(sensor, EQUALIZER_HF);
         bufFilterFrequency(d2, d3, 0, dmax, 0, track, filter, 0);
         _batch_fmadd(d1, d2, dmax, 1.0, 0.0);
      }
      else if (ptr && graphic)
      {
         _oalRingBufferFreqFilterInfo* filter;
         _oalRingBufferEqualizerInfo *eq;
         int32_t *d2 = (int32_t *)p;
         int32_t *d3 = d2 + dmax;
         int b = 6;

         eq = _FILTER_GET_DATA(sensor, EQUALIZER_HF);
         filter = &eq->band[b--];
         _aax_memcpy(d3, d1, rbd->track_len_bytes);
         bufFilterFrequency(d1, d3,  0, dmax, 0, track, filter, 0);
         do
         {
            filter = &eq->band[b--];
            if (filter->lf_gain || filter->hf_gain)
            {
               bufFilterFrequency(d2, d3, 0, dmax, 0, track, filter, 0);
               _batch_fmadd(d1, d2, rbd->no_samples, 1.0f, 0.0f);
            }

            filter = &eq->band[b--];
            if (filter->lf_gain || filter->hf_gain) 
            {
               bufFilterFrequency(d2, d3, 0, dmax, 0, track, filter, 0);
               _batch_fmadd(d1, d2, rbd->no_samples, 1.0f, 0.0f);
            }

            filter = &eq->band[b--];
            if (filter->lf_gain || filter->hf_gain) 
            {
               bufFilterFrequency(d2, d3, 0, dmax, 0, track, filter, 0);
               _batch_fmadd(d1, d2, rbd->no_samples, 1.0f, 0.0f);
            }
         }
         while (b > 0);
      }

      average = 0;
      peak = dmax;
      _aaxProcessCompression(d1, &average, &peak);
      rb->average[track] = ((1.0f-dt)*rb->average[track] + dt*average);
      rb->peak[track] = peak; //((1.0f-dt)*rb->peak[track] + dt*peak);

      if (maxavg < average) maxavg = average;
      if (maxpeak < peak) maxpeak = peak;
   }
   free(ptr);

   rb->average[_AAX_MAX_SPEAKERS] = maxavg;
   rb->peak[_AAX_MAX_SPEAKERS] = maxpeak;
}

void*
_aaxSoftwareMixerThread(void* config)
{
   _handle_t *handle = (_handle_t *)config;
   _intBufferData *dptr_sensor;
   const _aaxDriverBackend *be;
   _oalRingBuffer *dest_rb;
   _aaxAudioFrame *mixer;
   _aaxTimer *timer;
   int state, tracks;
   float delay_sec;

   if (!handle || !handle->sensors || !handle->backend.ptr
       || !handle->info->no_tracks) {
      return NULL;
   }

   be = handle->backend.ptr;
   delay_sec = 1.0f/handle->info->refresh_rate;

   tracks = 2;
   mixer = NULL;
   dest_rb = _oalRingBufferCreate(REVERB_EFFECTS_TIME);
   if (dest_rb)
   {
      dptr_sensor = _intBufGet(handle->sensors, _AAX_SENSOR, 0);
      if (dptr_sensor)
      {
//       _oalRingBuffer *nrb;
         _aaxMixerInfo* info;
         _sensor_t* sensor;

         sensor = _intBufGetDataPtr(dptr_sensor);
         mixer = sensor->mixer;
         info = mixer->info;

         tracks = info->no_tracks;
         _oalRingBufferSetParami(dest_rb, RB_NO_TRACKS, tracks);
         _oalRingBufferSetFormat(dest_rb, be->codecs, AAX_PCM24S);
         _oalRingBufferSetParamf(dest_rb, RB_FREQUENCY, info->frequency);
         _oalRingBufferSetParamf(dest_rb, RB_DURATION_SEC, delay_sec);
         _oalRingBufferInit(dest_rb, AAX_TRUE);
         _oalRingBufferStart(dest_rb);

         handle->ringbuffer = dest_rb;
//       nrb = _oalRingBufferDuplicate(dest_rb, AAX_FALSE, AAX_FALSE);
//       _intBufAddData(mixer->ringbuffers, _AAX_RINGBUFFER, nrb);
         _intBufReleaseData(dptr_sensor, _AAX_SENSOR);
      }
   }

   dest_rb = handle->ringbuffer;
   if (!dest_rb) {
      return NULL;
   }

   /* get real duration, it might have been altered for better performance */
   delay_sec = _oalRingBufferGetParamf(dest_rb, RB_DURATION_SEC);

   be->state(handle->backend.handle, DRIVER_PAUSE);
   state = AAX_SUSPENDED;

   timer = _aaxTimerCreate();
   _aaxTimerStartRepeatable(timer, delay_sec);

   _aaxMutexLock(handle->thread.mutex);
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
      _aaxSoftwareMixerThreadUpdate(handle, dest_rb);
   }
   while (_aaxTimerWait(timer, handle->thread.mutex) == AAX_TIMEOUT);

   _aaxTimerDestroy(timer);
   _aaxMutexUnLock(handle->thread.mutex);

   dptr_sensor = _intBufGetNoLock(handle->sensors, _AAX_SENSOR, 0);
   if (dptr_sensor)
   {
      _oalRingBufferStop(handle->ringbuffer);
      _oalRingBufferDelete(handle->ringbuffer);
      handle->ringbuffer = NULL;
   }

   return handle;
}

unsigned int
_aaxSoftwareMixerSignalFrames(void *frames, float refresh_rate)
{
   _intBuffers *hf = (_intBuffers*)frames;
   unsigned int num = 0;

   if (hf)
   {
      unsigned int i;
#if USE_CONDITION
		/* 80%: 0.8*1000000000L */
      uint32_t dt_ns = (uint32_t)(800000000.0f/refresh_rate);
      struct timeval tv;
      struct timespec ts;

      gettimeofday(&tv, NULL);
      ts.tv_sec = tv.tv_sec + (dt_ns % 1000000000L);
      ts.tv_nsec = tv.tv_usec*1000 + (dt_ns / 1000000000L);
      if (ts.tv_nsec > 1000000000L)
      {
         ts.tv_sec++;
         ts.tv_nsec -= 1000000000L;
      }
#endif

      num = _intBufGetMaxNum(hf, _AAX_FRAME);
      for (i=0; i<num; i++)
      {
         _intBufferData *dptr = _intBufGetNoLock(hf, _AAX_FRAME, i);
         if (dptr)
         {
            _frame_t* frame = _intBufGetDataPtr(dptr);
            _aaxAudioFrame* mixer = frame->submix;

            if TEST_FOR_TRUE(mixer->capturing)
            {
               unsigned int nbuf;
               nbuf = _intBufGetNumNoLock(mixer->ringbuffers, _AAX_RINGBUFFER);
#if USE_CONDITION
if (nbuf > 2) printf("nuf: %i\n", nbuf);
               if (frame->thread.condition) {
                  _aaxConditionSignal(frame->thread.condition);
               }
#else
               if (nbuf < 2 && frame->thread.condition)  {
                  _aaxConditionSignal(frame->thread.condition);
               }
#endif
            }
//          _intBufReleaseData(dptr, _AAX_FRAME);
         }
      }
      _intBufReleaseNum(hf, _AAX_FRAME);

      /* give the remainder of the threads time slice to other threads */
//    msecSleep(2);

      
#if USE_CONDITION
      /* Wait for the worker threads to finish before continuing */
      num = _intBufGetMaxNumNoLock(hf, _AAX_FRAME);
      for (i=0; i<num; i++)
      {
         _intBufferData *dptr = _intBufGetNoLock(hf, _AAX_FRAME, i);
         if (dptr)
         {
            _frame_t* frame = _intBufGetDataPtr(dptr);
            _aaxAudioFrame* mixer = frame->submix;
            if (mixer->frame_ready) {		// REGISTERED_FRAME;
               _aaxConditionWaitTimed(mixer->frame_ready, NULL, &ts);
            }
//          _intBufReleaseData(dptr, _AAX_FRAME);
         }
      }
#endif
   }
   return num;
}

/*-------------------------------------------------------------------------- */

unsigned int
_aaxSoftwareMixerMixFrames(void *dest, _intBuffers *hf)
{
   _oalRingBuffer *dest_rb = (_oalRingBuffer *)dest;
   unsigned int i, num = 0;
   if (hf)
   {
      num = _intBufGetMaxNum(hf, _AAX_FRAME);
      for (i=0; i<num; i++)
      {
         _intBufferData *dptr = _intBufGet(hf, _AAX_FRAME, i);
         if (dptr)
         {
            _frame_t* frame = _intBufGetDataPtr(dptr);
            _aaxAudioFrame *mixer = frame->submix;

            /*
             * mixer->thread  = -1: mixer
             * mixer->thread  =  1: threaded frame
             * mixer->thread  =  0: non threaded frame, call update ourselves.
             */
//          if (!mixer->thread)
//          {
// printf("non threaded frame\n");
//          }
//          else
            {
#if USE_CONDITION
               mixer->capturing = 2;
#else
//             unsigned int dt = 1.5f*1000.0f/mixer->info->refresh_rate; // ms
               unsigned int dt = 5000;
               int p = 0;

               /*
                * Can't call aaxAudioFrameWaitForBuffer because of a dead-lock
                */
               while ((mixer->capturing == 1) && (++p < dt)) // 3ms is enough
               {
                  _intBufReleaseData(dptr, _AAX_FRAME);

#ifdef _WIN32
                  SwitchToThread();
#else
                  msecSleep(1);	 /* special case, see Sleep(0) for windows */
#endif

                  dptr = _intBufGet(hf, _AAX_FRAME, i);
                  if (!dptr) break;

                  frame = _intBufGetDataPtr(dptr);
               }
#endif
            } /* mixer->thread */

            if (dptr && mixer->capturing > 1)
            {
                _handle_t *handle = frame->handle;
                const _aaxDriverBackend *be = handle->backend.ptr;
                void *be_handle = handle->backend.handle;
                _oalRingBuffer2dProps *p2d = mixer->props2d;

                _aaxAudioFrameMix(dest_rb, mixer->ringbuffers,
                                  p2d, be, be_handle);
                mixer->capturing = 1;
            }

            /*
             * dptr could be zero if it was removed while waiting for a new
             * buffer
             */
            if (dptr) _intBufReleaseData(dptr, _AAX_FRAME);
         }
      }
      _intBufReleaseNum(hf, _AAX_FRAME);
   }
   return num;
}

int
_aaxSoftwareMixerPlayFrame(void* rb, const void* devices, const void* ringbuffers, const void* frames, void* props2d, const void* props3d, char capturing, const void* sensor, const void* backend, const void* be_handle)
{
   const _aaxDriverBackend* be = (const _aaxDriverBackend*)backend;
   _oalRingBuffer2dProps *p2d = (_oalRingBuffer2dProps*)props2d;
   _oalRingBuffer *dest_rb = (_oalRingBuffer *)rb;
   float gain;
   int res;

   if (devices) {
      _aaxSensorsProcess(dest_rb, devices, props2d);
   }

   if (frames)
   {
      _intBuffers *mixer_frames = (_intBuffers*)frames;
      _aaxSoftwareMixerMixFrames(dest_rb, mixer_frames);
   }
   be->effects(be, be_handle, dest_rb, props2d);
   be->postprocess(be_handle, dest_rb, sensor);

   /** play back all mixed audio */
   gain = _FILTER_GET(p2d, VOLUME_FILTER, AAX_GAIN);
   res = be->play(be_handle, dest_rb, 1.0, gain);

   if TEST_FOR_TRUE(capturing)
   {
      _intBuffers *mixer_ringbuffers = (_intBuffers*)ringbuffers;
      _oalRingBuffer *new_rb;

      new_rb = _oalRingBufferDuplicate(dest_rb, AAX_TRUE, AAX_FALSE);

      _oalRingBufferForward(new_rb);
      _intBufAddData(mixer_ringbuffers, _AAX_RINGBUFFER, new_rb);
   }

   _oalRingBufferClear(dest_rb);
   _oalRingBufferStart(dest_rb);

   return res;
}

int
_aaxSoftwareMixerThreadUpdate(void *config, void *dest)
{
   _handle_t *handle = (_handle_t *)config;
   const _aaxDriverBackend* be;
   _intBufferData *dptr_sensor;
   int res = 0;

   assert(handle);
   assert(handle->sensors);
   assert(handle->backend.ptr);
   assert(handle->info->no_tracks);

   be = handle->backend.ptr;
   dptr_sensor = _intBufGetNoLock(handle->sensors, _AAX_SENSOR, 0);
   if (dptr_sensor && (_IS_PLAYING(handle) || _IS_STANDBY(handle)))
   {
      void* be_handle = handle->backend.handle;
      _aaxAudioFrame *mixer = NULL;

      if (_IS_PLAYING(handle))
      {
         dptr_sensor = _intBufGetNoLock(handle->sensors, _AAX_SENSOR, 0);
         if (dptr_sensor)
         {
            _sensor_t *sensor = _intBufGetDataPtr(dptr_sensor);
            mixer = sensor->mixer;

            if (handle->info->mode == AAX_MODE_READ)
            {
               float gain, dt = 1.0f / mixer->info->refresh_rate;
               void *rv, *rb = dest; // mixer->ringbuffer;

               gain = _FILTER_GET(mixer->props2d, VOLUME_FILTER, AAX_GAIN);
               rv = _aaxSensorCapture(rb, be, be_handle, &dt,
                                               mixer->curr_pos_sec, gain);
               if (dt == 0.0f)
               {
                  _SET_STOPPED(handle);
                  _SET_PROCESSED(handle);
               }

               dptr_sensor = _intBufGet(handle->sensors, _AAX_SENSOR, 0);
               if (dptr_sensor)
               {
                  if (rb != rv)
                  {
                     _intBufAddData(mixer->ringbuffers, _AAX_RINGBUFFER, rb);
                     handle->ringbuffer = rv;
                  }
                  mixer->curr_pos_sec += dt;
                  _intBufReleaseData(dptr_sensor, _AAX_SENSOR);
               }
            }
            else if (mixer->emitters_3d || mixer->emitters_2d || mixer->frames)
            {
               _oalRingBuffer2dProps sp2d;
               _oalRingBuffer3dProps sp3d;
               float gain;

               /* copying here prevents locking the listener the whole time */
               /* it's used for just one time-frame anyhow                  */
               dptr_sensor = _intBufGet(handle->sensors, _AAX_SENSOR, 0);
               memcpy(&sp3d, mixer->props3d, sizeof(_oalRingBuffer3dProps));
               memcpy(&sp2d, mixer->props2d, sizeof(_oalRingBuffer2dProps));
               memcpy(&sp2d.pos, handle->info->speaker,
                                  _AAX_MAX_SPEAKERS*sizeof(vec4_t));
               memcpy(&sp2d.hrtf, handle->info->hrtf, 2*sizeof(vec4_t));
               _intBufReleaseData(dptr_sensor, _AAX_SENSOR);

#if THREADED_FRAMES
               /** signal threaded frames to update (if necessary) */
               /* thread == -1: mixer; attached frames are threads */
               /* thread >=  0: frame; call updates manually       */
               if (mixer->thread < 0) {
                  _aaxSoftwareMixerSignalFrames(mixer->frames,
                                                mixer->info->refresh_rate);
               }

               /* main mixer */
               _aaxEmittersProcess(dest, handle->info, &sp2d, &sp3d, NULL, NULL,
                                         mixer->emitters_2d, mixer->emitters_3d,
                                         be, be_handle);
 
               res = _aaxSoftwareMixerPlayFrame(dest, mixer->devices,
                                                mixer->ringbuffers,
                                                mixer->frames,
                                                &sp2d, &sp3d, mixer->capturing,
                                                sensor, be, be_handle);

#else
               _aaxAudioFrameProcess(dest, sensor, mixer, &sp2d, &sp3d,
                                      NULL, NULL, &sp2d, &sp3d, be, be_handle);

               /** play back all mixed audio */
               gain = _FILTER_GET(mixer->props2d, VOLUME_FILTER, AAX_GAIN);
               res = be->play(be_handle, dest, 1.0, gain);
               if TEST_FOR_TRUE(mixer->capturing)
               {
                  _intBuffers *mixer_ringbuffers;
                  _oalRingBuffer *new_rb;

                  mixer_ringbuffers = (_intBuffers*)mixer->ringbuffers;
                  new_rb = _oalRingBufferDuplicate(dest, AAX_TRUE, AAX_FALSE);

                  _oalRingBufferForward(new_rb);
                  _intBufAddData(mixer_ringbuffers, _AAX_RINGBUFFER, new_rb);
               }

               _oalRingBufferClear(dest);
               _oalRingBufferStart(dest);
#endif
            }
         }
      }
      else /* if (_IS_STANDBY(handle) */
      {
         if (handle->info->mode != AAX_MODE_READ)
         {
            dptr_sensor = _intBufGet(handle->sensors, _AAX_SENSOR, 0);
            if (dptr_sensor)
            {
               _sensor_t *sensor = _intBufGetDataPtr(dptr_sensor);
               mixer = sensor->mixer;

               if (mixer->emitters_3d || mixer->emitters_2d) {
                  _aaxNoneDriverProcessFrame(mixer);
               }
               _intBufReleaseData(dptr_sensor, _AAX_SENSOR);
            }
         }
      }
   }

   return res;
}

