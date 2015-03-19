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

#include <assert.h>
#ifdef HAVE_RMALLOC_H
# include <rmalloc.h>
#else
# include <stdlib.h>
# include <malloc.h>
#endif
#include <math.h>
#ifndef NDEBUG
# include <stdio.h>
#endif

#include <base/types.h>
#include <base/logging.h>

#include <api.h>
#include <arch.h>
#include <ringbuffer.h>

#include "audio.h"
#include "rbuf_int.h"
#include "renderer.h"

#ifndef _DEBUG
# define _DEBUG		0
#endif

static void _aaxRingBufferInitFunctions(_aaxRingBuffer*);
static int _aaxRingBufferClear(_aaxRingBufferData*);

static _aaxFormat_t _aaxRingBufferFormat[AAX_FORMAT_MAX];

_aaxRingBuffer *
_aaxRingBufferAlloc()
{
   _aaxRingBuffer *rb;
   size_t size;

   _AAX_LOG(LOG_DEBUG, __func__);

   size = sizeof(_aaxRingBuffer) + sizeof(_aaxRingBufferData);
   rb = (_aaxRingBuffer *)calloc(1, size);
   if (rb)
   {
      _aaxRingBufferData *rbi;
      _aaxRingBufferSample *rbd;
      char *ptr;

      ptr = (char*)rb;
      rbi = (_aaxRingBufferData*)(ptr + sizeof(_aaxRingBuffer));
      rb->handle = rbi;

      rbd = (_aaxRingBufferSample *)calloc(1, sizeof(_aaxRingBufferSample));
      rbi->sample = rbd;
      if (!rbd)
      {
         free(rb);
         rb = NULL;
      }
   }

   return rb;
}

_aaxRingBuffer *
_aaxRingBufferCreate(float dde, enum aaxRenderMode mode)
{
   _aaxRingBuffer *rb;

   _AAX_LOG(LOG_DEBUG, __func__);

   rb = _aaxRingBufferAlloc();
   if (rb)
   {
      _aaxRingBufferData *rbi = rb->handle;
      _aaxRingBufferSample *rbd = rbi->sample;
      if (rbd)
      {
         float ddesamps;

         /*
          * fill in the defaults
          */
         rbd->ref_counter = 1;

         rbi->playing = 0;
         rbi->stopped = 1;
         rbi->streaming = 0;
         rbi->gain_agc = 1.0f;
         rbi->pitch_norm = 1.0f;
         rbi->volume_min = 0.0f;
         rbi->volume_max = 1.0f;
         rbi->codec = _aaxRingBufferProcessCodec;	// always cvt to 24-bit
         rbi->effects = _aaxRingBufferEffectsApply;
         rbi->mix = _aaxRingBufferProcessMixer;		// uses the above funcs

         rbi->mode = mode;
         rbi->access = RB_RW_MAX;
#ifndef NDEBUG
         rbi->parent = rb;
#endif

         rbd->dde_sec = dde;
         rbd->no_tracks = 1;
         rbd->frequency_hz = 44100.0f;
         rbd->format = AAX_PCM16S;
         rbd->codec = _aaxRingBufferCodecs[rbd->format];
         rbd->bytes_sample = _aaxRingBufferFormat[rbd->format].bits/8;
#if RB_FLOAT_DATA
         rbd->freqfilter = _batch_freqfilter_float;
         rbd->resample = _batch_resample_float;
         rbd->multiply = _batch_fmul_value;
         rbd->add = _batch_fmadd;
#else
         rbd->freqfilter = _batch_freqfilter;
         rbd->resample = _batch_resample;
         rbd->multiply = _batch_imul_value;
         rbd->add = _batch_imadd;
#endif
         rbd->mixmn = _aaxRingBufferMixStereo16;
         switch(rbi->mode)
         {
         case AAX_MODE_WRITE_SPATIAL:
           rbd->mix1n = _aaxRingBufferMixMono16Spatial;
            break;
         case AAX_MODE_WRITE_SURROUND:
            rbd->mix1n = _aaxRingBufferMixMono16Surround;
            break;
         case AAX_MODE_WRITE_HRTF:
            rbd->mix1n = _aaxRingBufferMixMono16HRTF;
            break;
         case AAX_MODE_WRITE_STEREO:
         default:
            rbd->mix1n = _aaxRingBufferMixMono16Stereo;
            break;
         }

         ddesamps = ceilf(dde * rbd->frequency_hz);
         rbd->dde_samples = (size_t)ddesamps;
         rbd->scratch = NULL;

         _aaxRingBufferInitFunctions(rb);
      }
   }

   return rb;
}

void
_aaxRingBufferDestroy(_aaxRingBuffer *rb)
{
   _aaxRingBufferSample *rbd;
   _aaxRingBufferData *rbi;

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(rb != 0);

   rbi = rb->handle;
   assert(rbi->sample != 0);

   rbd = rbi->sample;

   if (rbd && rbd->ref_counter > 0)
   {
      if (--rbd->ref_counter == 0)
      {
         free(rbd->track);
         rbd->track = NULL;

         free(rbd->scratch);
         rbd->scratch = NULL;

         free(rbi->sample);
         rbi->sample = NULL;
      }
      free(rb);
   }
}

void
_aaxRingBufferFree(void *ringbuffer)
{
   _aaxRingBuffer *rb = (_aaxRingBuffer*)ringbuffer;
   rb->destroy(rb);
}

static void
_aaxRingBufferInitTracks(_aaxRingBufferData *rbi)
{
   _aaxRingBufferSample *rbd;

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(rbi != NULL);
   assert(rbi->parent == (char*)rbi-sizeof(_aaxRingBuffer));

   rbd = rbi->sample;

   /*
    * When moving playing registered sensors from one output devcie
    * to another rbd->track != NULL.  calculate it's new size, free
    * the previous buffer and create a new one with the proper size
    */
   if (1) // !rbd->track)
   {
      unsigned int tracks;
      size_t no_samples, tracksize, dde_bytes;
      char *ptr, *ptr2;
      char bps;

      bps = rbd->bytes_sample;
      no_samples = rbd->no_samples_avail;
      dde_bytes = TEST_FOR_TRUE(rbd->dde_sec) ? (rbd->dde_samples * bps) : 0;
      dde_bytes = SIZETO16(dde_bytes);

      /*
       * Create one buffer that can hold the data for all channels.
       * The first bytes are reserved for the track pointers
       */
      tracksize = dde_bytes + (no_samples + MEMMASK) * bps;
#if BYTE_ALIGN
      /* 16-byte align every buffer */
      tracksize = SIZETO16(tracksize);

      tracks = rbd->no_tracks;
      ptr2 = (char*)(tracks * sizeof(void*));
      ptr = _aax_calloc(&ptr2, tracks, sizeof(void*) + tracksize);
#else
      ptr = ptr2 = calloc(tracks, sizeof(void*) + tracksize);
#endif
      if (ptr)
      {
         size_t i;

         rbd->track_len_bytes = no_samples * bps;
         rbd->duration_sec = (float)no_samples / rbd->frequency_hz;

         rbd->loop_start_sec = 0.0f;
         rbd->loop_end_sec = rbd->duration_sec;

         free(rbd->track);
         rbd->track = (void **)ptr;
         for (i=0; i<rbd->no_tracks; i++)
         {
            rbd->track[i] = ptr2 + dde_bytes;
            ptr2 += tracksize;
         }
      }
   }
}

void
_aaxRingBufferInit(_aaxRingBuffer *rb, char add_scratchbuf)
{
   _aaxRingBufferSample *rbd;
   _aaxRingBufferData *rbi;

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(rb != NULL);

   rbi = rb->handle;
   assert(rbi != NULL);
   assert(rbi->parent == rb);

   rbd = rbi->sample;
   _aaxRingBufferInitTracks(rbi);

   if (add_scratchbuf && rbd->scratch == NULL)
   {
      size_t i, tracks, no_samples, tracksize, dde_bytes;
      char *ptr, *ptr2;
      char bps;

      bps = rbd->bytes_sample;
      no_samples = rbd->no_samples_avail;
      dde_bytes = rbd->dde_samples*bps;
      dde_bytes = SIZETO16(dde_bytes);

      tracksize = 2*dde_bytes + no_samples*bps;
      tracks = MAX_SCRATCH_BUFFERS;

#if BYTE_ALIGN
      /* 16-byte align every buffer */
      tracksize = SIZETO16(tracksize);

      ptr2 = (char*)(tracks * sizeof(void*));
      ptr = _aax_calloc(&ptr2, tracks, sizeof(void*) + tracksize);
#else
      ptr = ptr2 = calloc(tracks, sizeof(void*) + tracksize);
#endif
      if (ptr)
      {
         rbd->scratch = (void **)ptr;
         for (i=0; i<tracks; i++)
         {
            rbd->scratch[i] = ptr2 + dde_bytes;
            ptr2 += tracksize;
         }
      }
   }
}


_aaxRingBuffer *
_aaxRingBufferReference(_aaxRingBuffer *ringbuffer)
{
   _aaxRingBuffer *rb;

   assert(ringbuffer != 0);

   rb = malloc(sizeof(_aaxRingBuffer) + sizeof(_aaxRingBufferData));
   if (rb)
   {
      _aaxRingBufferData *rbi;
      char *ptr = (char*)rb;

      rbi = (_aaxRingBufferData*)(ptr + sizeof(_aaxRingBuffer));
      rb->handle = rbi;

      memcpy(rbi, ringbuffer->handle, sizeof(_aaxRingBufferData));

      rbi->sample->ref_counter++;
      // rbi->looping = 0;
      rbi->playing = 0;
      rbi->stopped = 1;
      rbi->streaming = 0;
      rbi->elapsed_sec = 0.0f;
      rbi->curr_pos_sec = 0.0f;
      rbi->curr_sample = 0;
// TODO: Is this really what we want? Looks suspiceous to me
      rbi->sample->scratch = NULL;

#ifndef NDEBUG
      rbi->parent = rb;
#endif

      _aaxRingBufferInitFunctions(rb);
   }

   return rb;
}

_aaxRingBuffer *
_aaxRingBufferDuplicate(_aaxRingBuffer *ringbuffer, char copy, char dde)
{
   _aaxRingBuffer *srb = ringbuffer;
   _aaxRingBufferData *srbi;
   _aaxRingBuffer *drb;

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(ringbuffer != 0);

   srbi = srb->handle;
   drb = _aaxRingBufferAlloc();
   if (drb)
   {
      _aaxRingBufferSample *srbd, *drbd;
      _aaxRingBufferData *drbi;
      char add_scratchbuf = AAX_FALSE;
      void *ptr;

      srbd = srbi->sample;

      drbi = drb->handle;
      drbd = drbi->sample;

      _aax_memcpy(drbi, srbi, sizeof(_aaxRingBufferData));
      drbi->access = RB_RW_MAX;
      drbi->sample = drbd;
#ifndef NDEBUG
      drbi->parent = drb;
#endif

      ptr = drbd->track;
      _aax_memcpy(drbd, srbd, sizeof(_aaxRingBufferSample));
      drbd->track = ptr;
      drbd->scratch = NULL;
      if (!dde)
      {
         drbd->dde_sec = 0.0f;
         drbd->dde_samples = 4*CUBIC_SAMPS;
      }

      if (srbd->scratch)
      {
         if (!copy)
         {
            drbd->scratch = srbd->scratch;
            srbd->scratch = NULL;
         }
         else {
            add_scratchbuf = AAX_TRUE;
         }
      }

      _aaxRingBufferInitFunctions(drb);
      _aaxRingBufferInit(drb, add_scratchbuf);

      if (copy || dde)
      {
         size_t t, ds, tracksize;

         tracksize = copy ? srb->get_parami(srb, RB_NO_SAMPLES) : 0;
         tracksize *= srb->get_parami(srb, RB_BYTES_SAMPLE);
         ds = dde ? srbd->dde_samples : 0;
         for (t=0; t<drbd->no_tracks; t++)
         {
            char *s, *d;

            s = (char *)srbd->track[t];
            d = (char *)drbd->track[t];
            _aax_memcpy(d-ds, s-ds, tracksize+ds);
         }
      }
   }

   return drb;
}

int32_t**
_aaxRingBufferGetTracksPtr(_aaxRingBuffer *rb, enum _aaxRingBufferMode mode)
{
   _aaxRingBufferData *rbi;
   _aaxRingBufferSample *rbd;
   int32_t **rv = NULL;

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(rb != NULL);

   rbi = rb->handle;
   assert(rbi != 0);
   assert(rbi->sample != 0);
   assert(rbi->parent == rb);
   assert(rbi->access == RB_RW_MAX);

   rbd = rbi->sample;
   if (rbd)
   {
      rbi->access = mode;
#if RB_FLOAT_DATA
      if (rbd->mixer_fmt && (rbi->access & RB_READ))
      {
         _aaxRingBufferSample *rbd = rbi->sample;
         unsigned int track, no_tracks = rbd->no_tracks;
         size_t no_samples = rbd->no_samples;
         void **tracks = rbd->track;
         for (track=0; track<no_tracks; track++) {
            _batch_cvt24_ps24(tracks[track], tracks[track], no_samples);
         }
      }
#endif
      rv  = (int32_t**)rbd->track;
   }
   return rv;
}

int
_aaxRingBufferReleaseTracksPtr(_aaxRingBuffer *rb)
{
   _aaxRingBufferData *rbi;
#if RB_FLOAT_DATA
   _aaxRingBufferSample *rbd;
#endif

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(rb != NULL);

   rbi = rb->handle;
   assert(rbi != 0);
   assert(rbi->sample != 0);
   assert(rbi->parent == rb);
   assert(rbi->access != RB_RW_MAX);

#if RB_FLOAT_DATA
   rbd = rbi->sample;
   if (rbd->mixer_fmt && (rbi->access & RB_WRITE))
   {
      _aaxRingBufferSample *rbd = rbi->sample;
      unsigned int track, no_tracks = rbd->no_tracks;
      size_t no_samples = rbd->no_samples;
      void **tracks = rbd->track;
      for (track=0; track<no_tracks; track++) {
         _batch_cvtps24_24(tracks[track], tracks[track], no_samples);
      }
   }
#endif
   rbi->access = RB_RW_MAX;
   return AAX_TRUE;
}

void**
_aaxRingBufferGetScratchBufferPtr(_aaxRingBuffer *rb)
{
   _aaxRingBufferData *rbi;
   _aaxRingBufferSample *rbd;
   void **rv = NULL;

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(rb != NULL);

   rbi = rb->handle;
   assert(rbi != 0);
   assert(rbi->sample != 0);
   assert(rbi->parent == rb);

   rbd = rbi->sample;
   if (rbd) {
      rv  = (void**)rbd->scratch;
   }
   return rv;
}

void
_aaxRingBufferSetState(_aaxRingBuffer *rb, enum _aaxRingBufferState state)
{
   _aaxRingBufferData *rbi;

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(rb != NULL);

   rbi = rb->handle;
   assert(rbi != 0);
   assert(rbi->sample != 0);
   assert(rbi->parent == rb);

   switch (state)
   {
   case RB_CLEARED:
      _aaxRingBufferClear(rbi);

      rbi->elapsed_sec = 0.0f;
      rbi->pitch_norm = 1.0f;
      rbi->curr_pos_sec = 0.0f;
      rbi->curr_sample = 0;

      rbi->playing = 0;
      rbi->stopped = 1;
      rbi->looping = 0;
      rbi->streaming = 0;
      break;
   case RB_REWINDED:
      rbi->curr_pos_sec = 0.0f;
      rbi->curr_sample = 0;
      break;
   case RB_FORWARDED:
      rbi->curr_pos_sec = rbi->sample->duration_sec;
      rbi->curr_sample = rbi->sample->no_samples;
      break;
   case RB_STARTED:
//    rbi->playing = 1;
      rbi->stopped = 0;
      rbi->streaming = 0;
      break;
   case RB_STOPPED:
//    rbi->playing = 0;
      rbi->stopped = 1;
      rbi->streaming = 0;
      break;
   case RB_STARTED_STREAMING:
//    rbi->playing = 1;
      rbi->stopped = 0;
      rbi->streaming = 1;
      break;
   default:
     break;
   }
}

int
_aaxRingBufferGetState(_aaxRingBuffer *rb, enum _aaxRingBufferState state)
{
// _aaxRingBufferSample *rbd;
   _aaxRingBufferData *rbi;
   int rv = 0;

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(rb != NULL);

   rbi = rb->handle;
   assert(rbi != 0);
   assert(rbi->sample != 0);
   assert(rbi->parent == rb);

// rbd = rbi->sample;

   switch (state)
   {
   case RB_IS_VALID:
      if (rb && rbi->sample && rbi->sample->track) {
         rv = -1;
      }
      break;
   default:
      break;
   }
   return rv;
}

int
_aaxRingBufferSetParamf(_aaxRingBuffer *rb, enum _aaxRingBufferParam param, float fval)
{
   _aaxRingBufferSample *rbd;
   _aaxRingBufferData *rbi;
   int rv = AAX_TRUE;

   assert(rb != NULL);

   rbi = rb->handle;
   assert(rbi != NULL);
   assert(rbi->sample != 0);
   assert(rbi->parent == rb);

   rbd = rbi->sample;
   switch(param)
   {
   case RB_VOLUME:
      rbi->volume_norm = fval;
      break;
   case RB_VOLUME_MIN:
      rbi->volume_min = fval;
      break;
   case RB_VOLUME_MAX:
      rbi->volume_max = fval;
      break;
   case RB_AGC_VALUE:
      rbi->gain_agc = fval;
      break;
   case RB_FREQUENCY:
      rbd->frequency_hz = fval;
      rbd->duration_sec = (float)rbd->no_samples / rbd->frequency_hz;
      rbd->dde_samples = (size_t)ceilf(rbd->dde_sec * rbd->frequency_hz);
      break;
   case RB_DURATION_SEC:
   {
      int val;

      rbd->duration_sec = fval;
      fval *= rbd->frequency_hz;
      val = rintf(fval);

      // same code as for _aaxRingBufferSetParami with RB_NO_SAMPLES
      if (rbd->track == NULL)
      {
         rbd->no_samples_avail = val;
         rbd->no_samples = val;
         rv = AAX_TRUE;
      }
      else if (val <= rbd->no_samples_avail)
      {
         /**
          * Note:
          * Sensors rely in the fact that resizing to a smaller bufferr-size
          * does not alter the actual buffer size, so it can overrun if required
          */
         rbd->no_samples = val;
         rv = AAX_TRUE;
      }
      else if (val > rbd->no_samples_avail)
      {
         rbd->no_samples_avail = val;
         rbd->no_samples = val;
         _aaxRingBufferInitTracks(rbi);
         rv = AAX_TRUE;
      }
#ifndef NDEBUG
      else if (rbd->track == NULL) {
         printf("Unable to set no. tracks when rbd->track != NULL");
      } else {
         printf("%s: Unknown error\n", __func__);
      }
#endif
      break;
   }
   case RB_LOOPPOINT_START:
      if (fval < rbd->loop_end_sec)
      {
         rbd->loop_start_sec = fval;
//       _aaxRingBufferAddLooping(rb);
      }
      break;
   case RB_LOOPPOINT_END:
      if ((rbd->loop_start_sec < fval) && (fval <= rbd->duration_sec))
      {
         rbd->loop_end_sec = fval;
//       _aaxRingBufferAddLooping(rb);
      }
      break;
   case RB_OFFSET_SEC:
      if (fval > rbd->duration_sec) {
         fval = rbd->duration_sec;
      }
      rbi->curr_pos_sec = fval;
      rbi->curr_sample = rintf(fval*rbd->frequency_hz);
      break;
   default:
      if ((param >= RB_PEAK_VALUE) &&
          (param <= RB_PEAK_VALUE_MAX))
      {
         rbi->peak[param-RB_PEAK_VALUE] = fval;
      }
      else if ((param >= RB_AVERAGE_VALUE) &&
               (param <= RB_AVERAGE_VALUE_MAX))
      {
         rbi->average[param-RB_AVERAGE_VALUE] = fval;
      }
#ifndef NDEBUG
      else {
         printf("UNKNOWN PARAMETER %x at line %i\n", param, __LINE__);
      }
#endif
      break;
   }

   return rv;
}

int
_aaxRingBufferSetParami(_aaxRingBuffer *rb, enum _aaxRingBufferParam param, unsigned int val)
{
   _aaxRingBufferSample *rbd;
   _aaxRingBufferData *rbi;
   int rv = AAX_FALSE;

   assert(rb != NULL);

   rbi = rb->handle;
   assert(rbi != NULL);
   assert(rbi->sample != 0);
   assert(rbi->parent == rb);

   rbd = rbi->sample;
   switch(param)
   {
   case RB_IS_MIXER_BUFFER:
#if RB_FLOAT_DATA
      if (rbd->mixer_fmt  != val)
      {
         _aaxRingBufferSample *rbd = rbi->sample;
         unsigned int track, no_tracks = rbd->no_tracks;
         size_t no_samples = rbd->no_samples;
         void **tracks = rbd->track;
         if (tracks)
         {
            for (track=0; track<no_tracks; track++)
            {
               if (val) {
                  _batch_cvtps24_24(tracks[track], tracks[track], no_samples);
               } else {
                  _batch_cvt24_ps24(tracks[track], tracks[track], no_samples);
               }
            }
         }
      }
#endif
      rbd->mixer_fmt = (val != 0) ? AAX_TRUE : AAX_FALSE;
      break;
   case RB_BYTES_SAMPLE:
      if (rbd->track == NULL) {
         rbd->bytes_sample = val;
         rv = AAX_TRUE;
      }
#ifndef NDEBUG
      else printf("Unable set bytes/sample when rbd->track == NULL\n");
      break;
#endif
   case RB_NO_TRACKS:
      if ((rbd->track == NULL) || (val <= rbd->no_tracks)) {
         rbd->no_tracks = val;
         rv = AAX_TRUE;
      }
#ifndef NDEBUG
      else printf("Unable set the no. tracks rbd->track == NULL\n");
#endif
      break;
   case RB_LOOPING:
      rbi->looping = val ? AAX_TRUE : AAX_FALSE;
      rbi->loop_max = (val > AAX_TRUE) ? val : 0;
#if 0
      if (loops) {
         _aaxRingBufferAddLooping(rb);
      }
#endif
      rv = AAX_TRUE;
      break;
   case RB_LOOPPOINT_START:
   {
      float fval = val/rbd->frequency_hz;
      if (fval < rbd->loop_end_sec)
      {
         rbd->loop_start_sec = fval;
//       _aaxRingBufferAddLooping(rb);
         rv = AAX_TRUE;
      }
      break;
   }
   case RB_LOOPPOINT_END:
   {
      float fval = val/rbd->frequency_hz;
      if ((rbd->loop_start_sec < fval) && (val <= rbd->no_samples))
      {
         rbd->loop_end_sec = fval;
//       _aaxRingBufferAddLooping(rb);
         rv = AAX_TRUE;
      }
      break;
   }
   case RB_OFFSET_SAMPLES:
      if (val > rbd->no_samples) {
         val = rbd->no_samples;
      }
      rbi->curr_sample = val;
      rbi->curr_pos_sec = (float)val / rbd->frequency_hz;
      rv = AAX_TRUE;
      break;
   case RB_TRACKSIZE:
      val /= rbd->bytes_sample;
      /* no break needed */
   case RB_NO_SAMPLES:
      if (rbd->track == NULL)
      {
         rbd->no_samples_avail = val;
         rbd->no_samples = val;
         rbd->duration_sec = (float)val / rbd->frequency_hz;
         rv = AAX_TRUE;
      }
      else if (val <= rbd->no_samples_avail)
      {
         /**
          * Note:
          * Sensors rely in the fact that resizing to a smaller bufferr-size
          * does not alter the actual buffer size, so it can overrun if required
          */
         rbd->no_samples = val;
         rbd->duration_sec = (float)val / rbd->frequency_hz;
         rv = AAX_TRUE;
      }
      else if (val > rbd->no_samples_avail)
      {
         rbd->no_samples_avail = val;
         rbd->no_samples = val;
         _aaxRingBufferInitTracks(rbi);
         rv = AAX_TRUE;
      }
#ifndef NDEBUG
      else if (rbd->track == NULL) {
         printf("Unable to set no. tracks when rbd->track != NULL");
      } else {
         printf("%s: Unknown error\n", __func__);
      }
#endif
      break;
   case RB_STATE:
      _aaxRingBufferSetState(rb, val);
      rv = AAX_TRUE;
      break;
   case RB_FORMAT:
   default:
      if ((param >= RB_PEAK_VALUE) &&
          (param <= RB_PEAK_VALUE_MAX))
      {
         rv = rbi->peak[param-RB_PEAK_VALUE];
      }
      else if ((param >= RB_AVERAGE_VALUE) &&
               (param <= RB_AVERAGE_VALUE_MAX))
      {
         rv = rbi->average[param-RB_AVERAGE_VALUE];
      }
#ifndef NDEBUG
      else {
         printf("UNKNOWN PARAMETER %x at line %i\n", param, __LINE__);
      }
#endif
      break;
   }

   return rv;
}

float
_aaxRingBufferGetParamf(const _aaxRingBuffer *rb, enum _aaxRingBufferParam param)
{
// _aaxRingBufferSample *rbd = rbi->sample;
   _aaxRingBufferData *rbi;
   float rv = AAX_NONE;

   assert(rb != NULL);

   rbi = rb->handle;
   assert(rbi != NULL);
   assert(rbi->parent == rb);

   switch(param)
   {
   case RB_VOLUME:
      rv = rbi->volume_norm;
      break;
   case RB_VOLUME_MIN:
      rv = rbi->volume_min;
      break;
   case RB_VOLUME_MAX:
      rv = rbi->volume_max;
      break;
   case RB_AGC_VALUE:
      rv = rbi->gain_agc;
      break;
   case RB_FREQUENCY:
      rv = rbi->sample->frequency_hz;
      break;
   case RB_DURATION_SEC:
      rv = rbi->sample->duration_sec;
      break;
   case RB_OFFSET_SEC:
      rv = rbi->curr_pos_sec;
      break;
   default:
      if ((param >= RB_PEAK_VALUE) &&
          (param <= RB_PEAK_VALUE_MAX))
      {
         rv = rbi->peak[param-RB_PEAK_VALUE];
      }
      else if ((param >= RB_AVERAGE_VALUE) &&
               (param <= RB_AVERAGE_VALUE_MAX))
      {
         rv = rbi->average[param-RB_AVERAGE_VALUE];
      }
#ifndef NDEBUG
      else {
         printf("UNKNOWN PARAMETER %x at line %x\n", param, __LINE__);
      }
#endif
      break;
   }

   return rv;
}

unsigned int
_aaxRingBufferGetParami(const _aaxRingBuffer *rb, enum _aaxRingBufferParam param)
{
   _aaxRingBufferSample *rbd;
   _aaxRingBufferData *rbi;
   size_t rv = -1;

   assert(rb != NULL);

   rbi = rb->handle;
   assert(rbi != NULL);
   assert(rbi->sample != 0);
   assert(rbi->parent == rb);

   rbd = rbi->sample;
   switch(param)
   {
   case RB_NO_TRACKS:
      rv = rbi->sample->no_tracks;
      break;
   case RB_NO_SAMPLES:
      rv = rbi->sample->no_samples;
      break;
   case RB_NO_SAMPLES_AVAIL:
      rv = rbi->sample->no_samples_avail;
      break;
   case RB_TRACKSIZE:
      rv = rbi->sample->track_len_bytes;
      break;
   case RB_BYTES_SAMPLE:
      rv = rbi->sample->bytes_sample;
      break;
   case RB_INTERNAL_FORMAT:
      rv = _aaxRingBufferFormat[rbd->format].format;
      break;
   case RB_LOOPING:
      rv = rbi->loop_max ? rbi->loop_max : rbi->looping;
      break;
   case RB_FORMAT:
      rv = rbd->format;
      break;
   case RB_LOOPPOINT_START:
      rv = (size_t)(rbd->loop_start_sec * rbd->frequency_hz);
      break;
   case RB_LOOPPOINT_END:
      rv = (size_t)(rbd->loop_end_sec * rbd->frequency_hz);
      break;
   case RB_OFFSET_SAMPLES:
      rv = rbi->curr_sample;
      break;
   case RB_DDE_SAMPLES:
      rv = rbd->dde_samples;
      break;
   case RB_IS_PLAYING:
      rv = (rbi->playing == 0 && rbi->stopped == 1) ? AAX_FALSE : AAX_TRUE;
      break;
   case RB_IS_MIXER_BUFFER:
      rv = (rbd->mixer_fmt != AAX_FALSE) ? AAX_TRUE : AAX_FALSE;
      break;
   default:
      if ((param >= RB_PEAK_VALUE) &&
          (param <= RB_PEAK_VALUE_MAX))
      {
         rv = rbi->peak[param-RB_PEAK_VALUE];
      }
      else if ((param >= RB_AVERAGE_VALUE) &&
               (param <= RB_AVERAGE_VALUE_MAX))
      {
         rv = rbi->average[param-RB_AVERAGE_VALUE];
      }
#ifndef NDEBUG
      else {
         printf("UNKNOWN PARAMETER %x at line %i\n", param, __LINE__);
      }
#endif
      break;
   }

   return rv;
}

int
_aaxRingBufferSetFormat(_aaxRingBuffer *rb, enum aaxFormat format, int mixer)
{
   _aaxRingBufferSample *rbd;
   _aaxRingBufferData *rbi;
   int rv = AAX_TRUE;

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(rb != NULL);

   rbi = rb->handle;
   assert(rbi != 0);
   assert(rbi->sample != 0);
   assert(format < AAX_FORMAT_MAX);
   assert(rbi->parent == rb);

   rbd = rbi->sample;
   rbd->mixer_fmt = mixer;
   if (rbd->track == NULL)
   {
      rbd->format = format;
      rbd->codec = _aaxRingBufferCodecs[rbd->format];
      rbd->bytes_sample = _aaxRingBufferFormat[rbd->format].bits/8;
   }
#ifndef NDEBUG
   else printf("%s: Can't set value when rbd->track != NULL\n", __func__);
#endif

   return rv;
}

int
_aaxRingBufferDataMixWaveform(_aaxRingBuffer *rb, enum aaxWaveformType type, float f, float ratio, float phase)
{
   _aaxRingBufferData *rbi;
   unsigned int tracks;
   unsigned char bps;
   size_t no_samples;
   int rv = AAX_FALSE;
   void *data;

   bps = rb->get_parami(rb, RB_BYTES_SAMPLE);
   tracks = rb->get_parami(rb, RB_NO_TRACKS);
   no_samples = rb->get_parami(rb, RB_NO_SAMPLES);

   f = rb->get_paramf(rb, RB_FREQUENCY)/f;

   rbi = rb->handle;
   data = rbi->sample->track;
   switch (type)
   {
   case AAX_SINE_WAVE:
      _bufferMixSineWave(data, f, bps, no_samples, tracks, ratio, phase);
      rv = AAX_TRUE;
      break;
   case AAX_SQUARE_WAVE:
      _bufferMixSquareWave(data, f, bps, no_samples, tracks, ratio, phase);
      rv = AAX_TRUE;
      break;
   case AAX_TRIANGLE_WAVE:
      _bufferMixTriangleWave(data, f, bps, no_samples, tracks, ratio, phase);
      rv = AAX_TRUE;
      break;
   case AAX_SAWTOOTH_WAVE:
      _bufferMixSawtooth(data, f, bps, no_samples, tracks, ratio, phase);
      rv = AAX_TRUE;
      break;
   case AAX_IMPULSE_WAVE:
      _bufferMixImpulse(data, f, bps, no_samples, tracks, ratio, phase);
      rv = AAX_TRUE;
      break;
   default:
      break;
   }
   return rv;
}

int
_aaxRingBufferDataMixNoise(_aaxRingBuffer *rb, enum aaxWaveformType type, float f, float rate, float ratio, float dc, char skip)
{
   _aaxRingBufferData *rbi;
   void *data, *scratch;
   unsigned int tracks;
   unsigned char bps;
   size_t no_samples;
   int rv = AAX_FALSE;
   float pitch;

   pitch = _MINMAX(rate, 0.01f, 1.0f);

   bps = rb->get_parami(rb, RB_BYTES_SAMPLE);
   tracks = rb->get_parami(rb, RB_NO_TRACKS);
   no_samples = rb->get_parami(rb, RB_NO_SAMPLES);

   rbi = rb->handle;
   data = rbi->sample->track;
   scratch = rbi->sample->scratch;
   switch (type)
   {
   case AAX_WHITE_NOISE:
      _bufferMixWhiteNoise(data, scratch, no_samples, bps, tracks, pitch, ratio, dc, skip);
      rv = AAX_TRUE;
      break;
   case AAX_PINK_NOISE:
      if (f) 
      {
         _bufferMixPinkNoise(data, scratch, no_samples, bps, tracks, pitch, ratio, f, dc, skip);
         rv = AAX_TRUE;
      }
      break;
   case AAX_BROWNIAN_NOISE:
     if (f)
     {
         _bufferMixBrownianNoise(data, scratch, no_samples, bps, tracks, pitch, ratio, f, dc, skip);
         rv = AAX_TRUE;
      }
      break;
   default:
      break;
   }

   return rv;
}

int
_aaxRingBufferDataMultiply(_aaxRingBuffer *rb, size_t offs, size_t no_samples, float ratio_orig)
{
   _aaxRingBufferData *rbi = rb->handle;
   _aaxRingBufferSample *rbd = rbi->sample;
   size_t t, tracks;
   unsigned char bps;
   MIX_T *data;

   bps = rb->get_parami(rb, RB_BYTES_SAMPLE);
   tracks = rb->get_parami(rb, RB_NO_TRACKS);
   if (!no_samples)
   {
      no_samples = rb->get_parami(rb, RB_NO_SAMPLES);
      offs = 0;
   }

   for (t=0; t<tracks; t++)
   {
      data = rbd->track[t];
      rbd->multiply(data+offs, bps, no_samples, ratio_orig);
   }
   return AAX_TRUE;
}

int
_aaxRingBufferDataMixData(_aaxRingBuffer *drb, _aaxRingBuffer *srb, _aaxRingBufferLFOData *lfo)
{
   _aaxRingBufferData *srbi, *drbi;
   _aaxRingBufferSample *drbd;
   unsigned char track, tracks;
   size_t dno_samples;
   float g = 1.0f;

   dno_samples =  drb->get_parami(drb, RB_NO_SAMPLES);
   tracks =  drb->get_parami(drb, RB_NO_TRACKS);

   srbi = srb->handle;
   if (lfo && lfo->envelope)
   {
       g = 0.0f;
       for (track=0; track<tracks; track++)
       {
           MIX_T *sptr = srbi->sample->track[track];
           float gain =  lfo->get(lfo, NULL, sptr, track, dno_samples);

           if (lfo->inv) gain = 1.0f/g;
           g += gain;
       }
       g /= tracks;
   }

   drbi = drb->handle;
   drbd = drbi->sample;
   for (track=0; track<tracks; track++)
   {
      void *sptr = srbi->sample->track[track];
      void *dptr = drbd->track[track];
      float gstep = 0.0f;

      drbd->add(dptr, sptr, dno_samples, g, gstep);
   }
   return AAX_TRUE;
}

int
_aaxRingBufferDataClear(_aaxRingBuffer *rb)
{
   _aaxRingBufferData *rbi = rb->handle;
   return _aaxRingBufferClear(rbi);
}

void
_aaxRingBufferCopyDelyEffectsData(_aaxRingBuffer *drb, const _aaxRingBuffer *srb)
{
   _aaxRingBufferData *drbi, *srbi;
   _aaxRingBufferSample *srbd, *drbd;

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(drb);
   assert(srb);

   drbi = drb->handle;
   srbi = srb->handle;
   assert(srbi);
   assert(drbi);
   assert(srbi->parent == srb);
   assert(drbi->parent == drb);

   srbd = srbi->sample;
   drbd = drbi->sample;

   if (srbd->bytes_sample == drbd->bytes_sample)
   {
      size_t t, tracks, ds, bps;

      ds = srbd->dde_samples;
      if (ds > drbd->dde_samples) {
         ds = drbd->dde_samples;
      }

      tracks = srbd->no_tracks;
      if (tracks > drbd->no_tracks) {
         tracks = drbd->no_tracks;
      }

      bps = srbd->bytes_sample;
      for (t=0; t<tracks; t++)
      {
         MIX_T *sptr = srbd->track[t];
         MIX_T *dptr = drbd->track[t];
         _aax_memcpy(dptr-ds, sptr-ds, ds*bps);
      }
   }
}

void
_aaxRingBufferDataLimiter(_aaxRingBuffer *rb, enum _aaxLimiterType type)
{
   static const float _val[RB_LIMITER_MAX][2] = 
   {
      { 0.5f, 0.0f },		// Electronic
      { 0.9f, 0.0f }, 		// Digital
      { 0.2f, 0.9f }		// Valve
   };

   _aaxRingBufferData *rbi = rb->handle;
   _aaxRingBufferSample *rbd = rbi->sample;
   unsigned int track, no_tracks = rbd->no_tracks;
   size_t no_samples = rbd->no_samples;
   size_t maxpeak, maxrms;
   size_t peak, rms;
   float dt, rms_rr, avg;
   MIX_T **tracks;

   dt = GMATH_E1 * rbd->duration_sec;
   rms_rr = _MINMAX(dt/0.3f, 0.0f, 1.0f);       // 300 ms average

   maxrms = maxpeak = 0;
   tracks = (MIX_T**)rbd->track;
   for (track=0; track<no_tracks; track++)
   {
      rms = 0;
      peak = no_samples;
      _aaxRingBufferLimiter(tracks[track], &rms, &peak, _val[type][0], _val[type][1]); 

      avg = rbi->average[track];
      avg = (rms_rr*avg + (1.0f-rms_rr)*rms);
      rbi->average[track] = avg;
      rbi->peak[track] = peak;

      if (maxrms < rms) maxrms = rms;
      if (maxpeak < peak) maxpeak = peak;
   }

   rbi->average[_AAX_MAX_SPEAKERS] = maxrms;
   rbi->peak[_AAX_MAX_SPEAKERS] = maxpeak;
}


/* -------------------------------------------------------------------------- */

static _aaxFormat_t _aaxRingBufferFormat[AAX_FORMAT_MAX] =
{
  {  8, AAX_PCM8S },	/* 8-bit  */
  { 16, AAX_PCM16S },	/* 16-bit */
  { 32, AAX_PCM24S },	/* 24-bit */
  { 32, AAX_PCM24S },	/* 32-bit gets converted to 24-bit */
  { 32, AAX_PCM24S },	/* float gets converted to 24-bit  */
  { 32, AAX_PCM24S },	/* double gets converted to 24-bit */
  {  8, AAX_MULAW },	/* mu-law  */
  {  8, AAX_ALAW },	/* a-law */
  { 16, AAX_PCM16S }	/* IMA4-ADPCM gets converted to 16-bit */
};

_aaxRingBufferMixStereoFn _aaxRingBufferMixMulti16;
_aaxRingBufferMixMonoFn _aaxRingBufferMixMono16;

float _linear(float v, float f) { return v*f; }
float _compress(float v, float f) { return powf(f, 1.0f-v); }


static int
_aaxRingBufferClear(_aaxRingBufferData *rbi)
{
   _aaxRingBufferSample *rbd;
   unsigned int i;

   assert(rbi->parent == (char*)rbi-sizeof(_aaxRingBuffer));

   rbd = rbi->sample;
   for (i=0; i<rbd->no_tracks; i++) {
      memset((void *)rbd->track[i], 0, rbd->track_len_bytes);
   }

   return AAX_TRUE;
}

static void
_aaxRingBufferInitFunctions(_aaxRingBuffer *rb)
{
   rb->init = _aaxRingBufferInit;
   rb->reference = _aaxRingBufferReference;
   rb->duplicate = _aaxRingBufferDuplicate;
   rb->destroy = _aaxRingBufferDestroy;

   rb->set_state = _aaxRingBufferSetState;
   rb->get_state = _aaxRingBufferGetState;
   rb->set_format = _aaxRingBufferSetFormat;

   rb->set_paramf = _aaxRingBufferSetParamf;
   rb->set_parami = _aaxRingBufferSetParami;
   rb->get_paramf = _aaxRingBufferGetParamf;
   rb->get_parami = _aaxRingBufferGetParami;

   rb->get_tracks_ptr = _aaxRingBufferGetTracksPtr;
   rb->release_tracks_ptr = _aaxRingBufferReleaseTracksPtr;

   /* protected */
   rb->data_clear = _aaxRingBufferDataClear;
   rb->data_multiply = _aaxRingBufferDataMultiply;
   rb->data_mix_waveform = _aaxRingBufferDataMixWaveform;
   rb->data_mix_noise = _aaxRingBufferDataMixNoise;
   rb->data_mix = _aaxRingBufferDataMixData;
   rb->limit = _aaxRingBufferDataLimiter;

   /* private */
   rb->mix2d = _aaxRingBufferMixMulti16;
   rb->mix3d = _aaxRingBufferMixMono16;
   rb->get_scratch = _aaxRingBufferGetScratchBufferPtr;
   rb->copy_effectsdata = _aaxRingBufferCopyDelyEffectsData;
}
