/*
 * Copyright 2007-2011 by Erik Hofman.
 * Copyright 2009-2011 by Adalin B.V.
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

#ifdef HAVE_LIBIO_H
#include <libio.h>              /* for NULL */
#endif
#ifdef HAVE_VALUES_H
#include <values.h>             /* for MAXFLOAT */
#endif
#include <math.h>		/* for fabs */
#include <string.h>		/* for calloc */
#include <assert.h>

#include <base/gmath.h>

#include <arch.h>
#include <ringbuffer.h>
#include <stdio.h>

#include "devices.h"
#include "api.h"

static void removeEmitterBufferByPos(void *, unsigned int);


aaxEmitter
aaxEmitterCreate()
{
   aaxEmitter rv = NULL;
   unsigned long size;
   void *ptr1;
   char* ptr2;

   size = sizeof(_emitter_t) + sizeof(_aaxEmitter);
   ptr2 = (char*)size;

   size += sizeof(_oalRingBuffer2dProps);
   ptr1 = _aax_calloc(&ptr2, 1, size);
   if (ptr1)
   {
      _emitter_t* handle = (_emitter_t*)ptr1;
      _aaxEmitter* src;

      src = (_aaxEmitter*)((char*)ptr1 + sizeof(_emitter_t));
      handle->source = src;
      _SET_INITIAL(src);
      src->pos = -1;

      assert(((long int)ptr2 & 0xF) == 0);
      src->props2d = (_oalRingBuffer2dProps*)ptr2;
      _aaxSetDefault2dProps(src->props2d);

      /* unfortunatelly postponing the allocation of the 3d data info buffer
       * is not possible since it prevents setting 3d position and orientation
       * before the emitter is set to 3d mode
       */
      if (src->props3d == NULL)
      {
         unsigned int size = sizeof(_oalRingBuffer3dProps);
         ptr2 = (char*)0;
         ptr1 = _aax_calloc(&ptr2, 1, size);
         if (ptr1)
         {
            src->props3d_ptr = ptr1;
            src->props3d = (_oalRingBuffer3dProps*)ptr2;
            _aaxSetDefault3dProps(src->props3d);
         }
         else
         {
            _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
            free(handle);
            src = NULL;
         }
      }

      if (src)
      {
         _intBufCreate(&src->buffers, _AAX_EMITTER_BUFFER);
         if (src->buffers)
         {
            handle->id = EMITTER_ID;
            handle->pos = UINT_MAX;
            handle->looping = AAX_FALSE;
            _SET_INITIAL(src);

            rv = (aaxEmitter)handle;
         }
         else
         {
            _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
            free(handle->source->props3d_ptr);
            free(handle);
         }
      }
   }
   else {
      _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
   }
   return rv;
}

int
aaxEmitterDestroy(aaxEmitter emitter)
{
   _emitter_t *handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      if (!handle->handle)
      {
         _oalRingBufferDelayEffectData* effect;
         _aaxEmitter *src = handle->source;

         _SET_PROCESSED(src);
         _intBufErase(&src->buffers, _AAX_EMITTER_BUFFER,
                      removeEmitterBufferByPos, src);

         free(_FILTER_GET2D_DATA(src, FREQUENCY_FILTER));
         free(_FILTER_GET2D_DATA(src, DYNAMIC_GAIN_FILTER));
         free(_FILTER_GET2D_DATA(src, TIMED_GAIN_FILTER));
         free(_EFFECT_GET2D_DATA(src, DYNAMIC_PITCH_EFFECT));
         free(_EFFECT_GET2D_DATA(src, TIMED_PITCH_EFFECT));

         effect = _EFFECT_GET2D_DATA(src, DELAY_EFFECT);
         if (effect) free(effect->history_ptr);
         free(effect);

         free(src->props3d_ptr);

         /* safeguard against using already destroyed handles */
         handle->id = 0xdeadbeef;
         free(handle);
         handle = 0;
         rv = AAX_TRUE;
      }
      else
      {
         put_emitter(emitter);
         _aaxErrorSet(AAX_INVALID_STATE);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   return rv;
}

int
aaxEmitterAddBuffer(aaxEmitter emitter, aaxBuffer buf)
{
   _emitter_t* handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      _buffer_t* buffer = get_buffer(buf);
      if (buffer && _oalRingBufferIsValid(buffer->ringbuffer))
      {
         const _aaxEmitter *src = handle->source;
         if (_intBufGetNumNoLock(src->buffers, _AAX_EMITTER_BUFFER) == 0) {
            handle->track = 0;
         }

         if (handle->track < _oalRingBufferGetNoTracks(buffer->ringbuffer))
         {
            _embuffer_t* embuf = malloc(sizeof(_embuffer_t));
            if (embuf)
            {
               embuf->ringbuffer = _oalRingBufferReference(buffer->ringbuffer);
               embuf->id = EMBUFFER_ID;
               embuf->buffer = buffer;
               buffer->ref_counter++;

               _intBufAddData(src->buffers, _AAX_EMITTER_BUFFER, embuf);

               rv = AAX_TRUE;
            }
            else {
               _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
            }
         }
         else {
            _aaxErrorSet(AAX_INVALID_STATE);
         }
      }
      else if (buffer) {
         _aaxErrorSet(AAX_INVALID_STATE);
      } else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

int
aaxEmitterRemoveBuffer(aaxEmitter emitter)
{
   _emitter_t* handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      _aaxEmitter *src = handle->source;
      if (!_IS_PLAYING(src) || src->pos > 0)
      {
         _intBufferData *dptr;
         unsigned int num;

         num = _intBufGetNum(src->buffers, _AAX_EMITTER_BUFFER);
         if (num > 0)
         {
            dptr = _intBufGet(src->buffers, _AAX_EMITTER_BUFFER, 0);
            if (dptr)
            {
               void **ptr;
               ptr = _intBufShiftIndex(src->buffers, _AAX_EMITTER_BUFFER, 0, 1);
               if (ptr)
               {
                  _embuffer_t *embuf = ptr[0];
                  if (embuf)
                  {
                     assert(embuf->id == EMBUFFER_ID);

                     free_buffer(embuf->buffer);
                     _oalRingBufferDelete(embuf->ringbuffer);
                     embuf->id = 0xdeadbeef;
                     free(embuf);
                  }
                  free(ptr);
                  ptr = NULL;

                  if (src->pos > 0) {
                     src->pos--;
                  }
                  rv = AAX_TRUE;
               }
               else
               {
                  _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
                  _intBufReleaseData(dptr, _AAX_EMITTER_BUFFER);
               }
            }
         }
         else {
            _aaxErrorSet(AAX_INVALID_REFERENCE);
         }
         _intBufReleaseNum(src->buffers, _AAX_EMITTER_BUFFER);
      }
      else {
         _aaxErrorSet(AAX_INVALID_STATE);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

const aaxBuffer
aaxEmitterGetBufferByPos(const aaxEmitter emitter, unsigned int pos, int copy)
{
   _emitter_t* handle = get_emitter(emitter);
   aaxBuffer rv = NULL;
   if (handle)
   {
      const _aaxEmitter *src = handle->source;
      _intBufferData *dptr;
      dptr = _intBufGet(src->buffers, _AAX_EMITTER_BUFFER, pos);
      if (dptr)
      {
         _embuffer_t *embuf = _intBufGetDataPtr(dptr);
         _buffer_t* buf = embuf->buffer;

         assert(embuf->id == EMBUFFER_ID);
         if (copy) buf->ref_counter++;
         rv = embuf->buffer;
         _intBufReleaseData(dptr, _AAX_EMITTER_BUFFER);
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

unsigned int
aaxEmitterGetNoBuffers(const aaxEmitter emitter, enum aaxState state)
{
   _emitter_t* handle = get_emitter(emitter);
   unsigned rv = 0;
   if (handle)
   {
      const _aaxEmitter *src = handle->source;
      switch (state)
      {
      case AAX_PROCESSED:
         if (_IS_PROCESSED(src)) {
            rv = _intBufGetNumNoLock(src->buffers, _AAX_EMITTER_BUFFER);
         } else if (src->pos > 0) {
            rv = src->pos;
         }
         break;
      case AAX_PLAYING:
         if (_IS_PLAYING(src)) {
            rv = _intBufGetNumNoLock(src->buffers, _AAX_EMITTER_BUFFER);
            rv -= src->pos;
         }
         break;
      case AAX_MAXIMUM:
         rv = _intBufGetNumNoLock(src->buffers, _AAX_EMITTER_BUFFER);
         break;
      default:
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

int
aaxEmitterSetState(aaxEmitter emitter, enum aaxState state)
{
   _emitter_t* handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      _aaxEmitter *src = handle->source;
      switch (state)
      {
      case AAX_PLAYING:
         if (!_IS_PLAYING(src))
         {
            unsigned int num;
            num = _intBufGetNumNoLock(src->buffers, _AAX_EMITTER_BUFFER);
            if (num)
            {
               src->pos = 0;
               _SET_PLAYING(src);
            }
         }
         else if (_IS_PAUSED(src)) {
            _TAS_PAUSED(src, AAX_FALSE);
         }
         rv = AAX_TRUE;
         break;
      case AAX_STOPPED:
         if (_IS_PLAYING(src))
         {
            if (!_PROP_DISTDELAY_IS_DEFINED(src->props3d))
            {
               _SET_PROCESSED(src);
               src->pos = -1;
            }
            else {
               _SET_STOPPED(src);
            }
         }
         rv = AAX_TRUE;
         break;
      case AAX_PROCESSED:
         if (_IS_PLAYING(src))
         {
            _SET_PROCESSED(src);
            src->pos = -1;
         }
         rv = AAX_TRUE;
         break;
      case AAX_SUSPENDED:
         if (_IS_PLAYING(src)) {
            _SET_PAUSED(src);
         }
         rv = AAX_TRUE;
         break;
      case AAX_INITIALIZED:	/* or rewind */
      {
         const _intBufferData* dptr;

         src->pos = 0;
         dptr = _intBufGet(src->buffers, _AAX_EMITTER_BUFFER, 0);
         if (dptr)
         {
            _oalRingBufferEnvelopeInfo* env;

            _embuffer_t *embuf = _intBufGetDataPtr(dptr);
            _oalRingBufferRewind(embuf->ringbuffer);
            _intBufReleaseData(dptr, _AAX_EMITTER_BUFFER);

            env = _FILTER_GET2D_DATA(src, TIMED_GAIN_FILTER);
            if (env)
            {
               env->value = _FILTER_GET(src->props2d, TIMED_GAIN_FILTER, 0);
               env->stage =  env->pos = 0;
            }

            env = _EFFECT_GET2D_DATA(src, TIMED_PITCH_EFFECT);
            if (env)
            {
               env->value = _EFFECT_GET(src->props2d, TIMED_PITCH_EFFECT, 0);
               env->stage =  env->pos = 0;
            }
         }
         rv = AAX_TRUE;
         break;
      }
      default:
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

int
aaxEmitterSetFilter(aaxEmitter emitter, aaxFilter f)
{
   _emitter_t* handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      _filter_t* filter = get_filter(f);
      if (filter)
      {
         _aaxEmitter *src = handle->source;
         int type = filter->pos;
         switch (filter->type)
         {
         case AAX_TIMED_GAIN_FILTER:
            _PROP_DISTDELAY_SET_DEFINED(src->props3d);
            /* break not needed */
         case AAX_FREQUENCY_FILTER:
         case AAX_VOLUME_FILTER:
         case AAX_DYNAMIC_GAIN_FILTER:
         {
            _oalRingBuffer2dProps *p2d = src->props2d;
            _FILTER_SET(p2d, type, 0, _FILTER_GET_SLOT(filter, 0, 0));
            _FILTER_SET(p2d, type, 1, _FILTER_GET_SLOT(filter, 0, 1));
            _FILTER_SET(p2d, type, 2, _FILTER_GET_SLOT(filter, 0, 2));
            _FILTER_SET(p2d, type, 3, _FILTER_GET_SLOT(filter, 0, 3));
            _FILTER_SET_STATE(p2d, type, _FILTER_GET_STATE(filter));
            _FILTER_SWAP_SLOT_DATA(p2d, type, filter, 0);
            rv = AAX_TRUE;
            break;
         }      
         case AAX_DISTANCE_FILTER:
         {
            _oalRingBuffer3dProps *p3d = src->props3d;
            _FILTER_SET(p3d, type, 0, _FILTER_GET_SLOT(filter, 0, 0));
            _FILTER_SET(p3d, type, 1, _FILTER_GET_SLOT(filter, 0, 1));
            _FILTER_SET(p3d, type, 2, _FILTER_GET_SLOT(filter, 0, 2));
            _FILTER_SET_STATE(p3d, type, _FILTER_GET_STATE(filter));
            _FILTER_SWAP_SLOT_DATA(p3d, type, filter, 0);
            rv = AAX_TRUE;
            break;
         }
         case AAX_ANGULAR_FILTER:
         {
            _oalRingBuffer3dProps *p3d = src->props3d;
            float inner_vec = _FILTER_GET_SLOT(filter, 0, 0);
            float outer_vec = _FILTER_GET_SLOT(filter, 0, 1);
            float outer_gain = _FILTER_GET_SLOT(filter, 0, 2);

            if ((inner_vec >= 0.995f) || (outer_gain >= 0.99f)) {
               _PROP_CONE_CLEAR_DEFINED(p3d);
            } else {
               _PROP_CONE_SET_DEFINED(p3d);
            }
            _FILTER_SET(p3d, type, 0, inner_vec);
            _FILTER_SET(p3d, type, 1, outer_vec);
            _FILTER_SET(p3d, type, 2, outer_gain);
            _FILTER_SET_STATE(p3d, type, _FILTER_GET_STATE(filter));
            _FILTER_SWAP_SLOT_DATA(p3d, type, filter, 0);
            rv = AAX_TRUE;
            break;
         }
         default:
            _aaxErrorSet(AAX_INVALID_ENUM);
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

const aaxFilter
aaxEmitterGetFilter(const aaxEmitter emitter, enum aaxFilterType type)
{
   _emitter_t* handle = get_emitter(emitter);
   aaxFilter rv = AAX_FALSE;
   if (handle)
   {
      switch(type)
      {
      case AAX_FREQUENCY_FILTER:
      case AAX_VOLUME_FILTER:
      case AAX_TIMED_GAIN_FILTER:
      case AAX_DYNAMIC_GAIN_FILTER:
      case AAX_DISTANCE_FILTER:
      case AAX_ANGULAR_FILTER:
      {
         _aaxEmitter *src = handle->source;
         _handle_t *cfg = (_handle_t*)handle->handle;
         _aaxMixerInfo* info = (cfg) ? cfg->info : NULL;
         rv = new_filter_handle(info, type, src->props2d, src->props3d);
         break;
      }
      default:
         _aaxErrorSet(AAX_INVALID_ENUM);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

int
aaxEmitterSetEffect(aaxEmitter emitter, aaxEffect e)
{
   _emitter_t* handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      _filter_t* effect = get_effect(e);
      if (effect)
      {
         _aaxEmitter *src = handle->source;
         int type = effect->pos;
         switch (effect->type)
         {
         case AAX_PITCH_EFFECT:
         case AAX_TIMED_PITCH_EFFECT:
            _PROP_PITCH_SET_CHANGED(src->props3d);
            /* break not needed */
         case AAX_DISTORTION_EFFECT:
         {
            _oalRingBuffer2dProps *p2d = src->props2d;
            _EFFECT_SET(p2d, type, 0, _EFFECT_GET_SLOT(effect, 0, 0));
            _EFFECT_SET(p2d, type, 1, _EFFECT_GET_SLOT(effect, 0, 1));
            _EFFECT_SET(p2d, type, 2, _EFFECT_GET_SLOT(effect, 0, 2));
            _EFFECT_SET(p2d, type, 3, _EFFECT_GET_SLOT(effect, 0, 3));
            _EFFECT_SET_STATE(p2d, type, _EFFECT_GET_STATE(effect));
            _EFFECT_SWAP_SLOT_DATA(p2d, type, effect, 0);
            rv = AAX_TRUE;
            break;
         }
         case AAX_FLANGING_EFFECT:
         case AAX_PHASING_EFFECT:
         case AAX_CHORUS_EFFECT:
         {
            _oalRingBuffer2dProps *p2d = src->props2d;
            _EFFECT_SET(p2d, type, 0, _EFFECT_GET_SLOT(effect, 0, 0));
            _EFFECT_SET(p2d, type, 1, _EFFECT_GET_SLOT(effect, 0, 1));
            _EFFECT_SET(p2d, type, 2, _EFFECT_GET_SLOT(effect, 0, 2));
            _EFFECT_SET(p2d, type, 3, _EFFECT_GET_SLOT(effect, 0, 3));
            _EFFECT_SET_STATE(p2d, type, _EFFECT_GET_STATE(effect));
            _EFFECT_SWAP_SLOT_DATA(p2d, type, effect, 0);
            if (_intBufGetNumNoLock(src->buffers, _AAX_EMITTER_BUFFER) > 1)
            {
               _oalRingBufferDelayEffectData* data;
               data = _EFFECT_GET2D_DATA(src, DELAY_EFFECT);
               if (data && !data->history_ptr)
               {
                  unsigned int tracks = effect->info->no_tracks;
                  float frequency = effect->info->frequency;
                  _oalRingBufferCreateHistoryBuffer(&data->history_ptr,
                                                    data->delay_history,
                                                    frequency, tracks);
               }
            }
            rv = AAX_TRUE;
            break;
         }
         case AAX_DYNAMIC_PITCH_EFFECT:
         {
            _oalRingBuffer2dProps *p2d = src->props2d;
            _oalRingBufferLFOInfo *lfo;

            _EFFECT_SET(p2d, type, 0, _EFFECT_GET_SLOT(effect, 0, 0));
            _EFFECT_SET(p2d, type, 1, _EFFECT_GET_SLOT(effect, 0, 1));
            _EFFECT_SET(p2d, type, 2, _EFFECT_GET_SLOT(effect, 0, 2));
            _EFFECT_SET_STATE(p2d, type, _EFFECT_GET_STATE(effect));
            _EFFECT_SWAP_SLOT_DATA(p2d, type, effect, 0);

            lfo = _EFFECT_GET_DATA(p2d, DYNAMIC_PITCH_EFFECT);
            if (lfo) /* enabled */
            {
               float lfo_val = _EFFECT_GET_SLOT(effect, 0, AAX_LFO_FREQUENCY);
               _PROP_DYNAMIC_PITCH_SET_DEFINED(src->props3d);
		/*
		 * The vibrato effect is not gradual like tremolo but is
		 * adjusted every update and stays constant which requires
		 * the fastest update rate when the LFO is fater than 1Hz.
		 */
               if ((lfo_val > 1.0f) && (src->update_rate < 4*lfo_val)) {
                  src->update_rate = 1;
               }
            }
            else
            { 
               _PROP_DYNAMIC_PITCH_CLEAR_DEFINED(src->props3d);
               src->update_rate = 0;
            }
            rv = AAX_TRUE;
            break;
         }
         case AAX_VELOCITY_EFFECT:
         {
            _oalRingBuffer3dProps *p3d = src->props3d;
            _EFFECT_SET(p3d, type, 0, _EFFECT_GET_SLOT(effect, 0, 0));
            _EFFECT_SET(p3d, type, 1, _EFFECT_GET_SLOT(effect, 0, 1));
            _EFFECT_SET(p3d, type, 2, _EFFECT_GET_SLOT(effect, 0, 2));
            _EFFECT_SET_STATE(p3d, type, _EFFECT_GET_STATE(effect));
            _EFFECT_SWAP_SLOT_DATA(p3d,  type, effect, 0);
            rv = AAX_TRUE;
            break;
         }
         default:
            _aaxErrorSet(AAX_INVALID_ENUM);
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

const aaxEffect
aaxEmitterGetEffect(const aaxEmitter emitter, enum aaxEffectType type)
{
   _emitter_t* handle = get_emitter(emitter);
   aaxEffect rv = AAX_FALSE;
   if (handle)
   {
      switch(type)
      {
      case AAX_PITCH_EFFECT:
      case AAX_TIMED_PITCH_EFFECT:
      case AAX_DYNAMIC_PITCH_EFFECT:
      case AAX_DISTORTION_EFFECT:
      case AAX_PHASING_EFFECT:
      case AAX_CHORUS_EFFECT:
      case AAX_FLANGING_EFFECT:
      case AAX_VELOCITY_EFFECT:
      {
         _aaxEmitter *src = handle->source;
         _handle_t *cfg = (_handle_t*)handle->handle;
         _aaxMixerInfo* info = (cfg) ? cfg->info : NULL;
         rv = new_effect_handle(info, type, src->props2d, src->props3d);
         break;
      }
      default:
         _aaxErrorSet(AAX_INVALID_ENUM);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

int
aaxEmitterSetMode(aaxEmitter emitter, enum aaxModeType type, int mode)
{
   _emitter_t* handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      _aaxEmitter *src = handle->source;
      switch(type)
      {
      case AAX_POSITION:
      {
         int m = (mode > AAX_MODE_NONE) ? AAX_TRUE : AAX_FALSE;
         _TAS_POSITIONAL(src, m);
         if TEST_FOR_TRUE(m)
         {
            m = (mode == AAX_RELATIVE) ? AAX_TRUE : AAX_FALSE;
            _TAS_RELATIVE(src, m);
            if TEST_FOR_TRUE(m) {
               src->props3d->matrix[LOCATION][3] = 0.0f;
            } else {
               src->props3d->matrix[LOCATION][3] = 1.0f;
            }
         }
         rv = AAX_TRUE;
         break;
      }
      case AAX_LOOPING:
      {
         _intBufferData *dptr =_intBufGet(src->buffers, _AAX_EMITTER_BUFFER, 0);
         if (dptr)
         {
            _embuffer_t *embuf = _intBufGetDataPtr(dptr);
            _oalRingBufferSetLooping(embuf->ringbuffer, mode);
            _intBufReleaseData(dptr, _AAX_EMITTER_BUFFER);
         }
         handle->looping = mode;
         rv = AAX_TRUE;
         break;
      }
      case AAX_BUFFER_TRACK:
      {
         _intBufferData *dptr =_intBufGet(src->buffers, _AAX_EMITTER_BUFFER, 0);
         if (dptr)
         {
            _embuffer_t *embuf = _intBufGetDataPtr(dptr);
            if (mode < _oalRingBufferGetNoTracks(embuf->buffer->ringbuffer))
            {
               handle->track = mode;
               rv = AAX_TRUE;
            }
            _intBufReleaseData(dptr, _AAX_EMITTER_BUFFER);
         }
         break;
      }
      default:
         _aaxErrorSet(AAX_INVALID_ENUM);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

int
aaxEmitterSetMatrix(aaxEmitter emitter, aaxMtx4f mtx)
{
   _emitter_t *handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      if (mtx && !detect_nan_vec4(mtx[0]) && !detect_nan_vec4(mtx[1]) &&
                 !detect_nan_vec4(mtx[2]) && !detect_nan_vec4(mtx[3]))
      {
         _aaxEmitter *src = handle->source;
         mtx4Copy(src->props3d->matrix, mtx);
         if (_IS_RELATIVE(src)) {
            src->props3d->matrix[LOCATION][3] = 0.0f;
         } else {
            src->props3d->matrix[LOCATION][3] = 1.0f;
         }
         _PROP_MTX_SET_CHANGED(src->props3d);
         rv = AAX_TRUE;
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

int
aaxEmitterSetVelocity(aaxEmitter emitter, const aaxVec3f velocity)
{
   _emitter_t* handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      if (velocity && !detect_nan_vec3(velocity))
      {
         _aaxEmitter *src = handle->source;
         vec3Copy(src->props3d->velocity, velocity);
         rv = AAX_TRUE;
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

int
aaxEmitterGetMatrix(const aaxEmitter emitter, aaxMtx4f mtx)
{
   _emitter_t *handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      if (mtx)
      {
         _aaxEmitter *src = handle->source;
         mtx4Copy(mtx, src->props3d->matrix);
         rv = AAX_TRUE;
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

int
aaxEmitterSetOffset(aaxEmitter emitter, unsigned long offs, enum aaxType type)
{
   _emitter_t* handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      _aaxEmitter *src = handle->source;
      _intBufferData *dptr;

      _intBufGetNum(src->buffers, _AAX_EMITTER_BUFFER);
      dptr = _intBufGet(src->buffers, _AAX_EMITTER_BUFFER, 0);
      if (dptr)
      {
         _embuffer_t *embuf = _intBufGetDataPtr(dptr);
         _oalRingBuffer *rb = embuf->ringbuffer;
         float duration, fpos = (float)offs*1e-6f;
         unsigned int samples, pos = 0;

         switch (type)
         {
         case AAX_BYTES:   
            offs /= _oalRingBufferGetBytesPerSample(rb);
         case AAX_FRAMES:
         case AAX_SAMPLES:
            samples = _oalRingBufferGetNoSamples(rb);
            while (offs > samples)
            {
               pos++;
               offs -= samples;
               _intBufReleaseData(dptr, _AAX_EMITTER_BUFFER);

               dptr = _intBufGet(src->buffers, _AAX_EMITTER_BUFFER, pos);
               if (!dptr) break;

               embuf = _intBufGetDataPtr(dptr);
               rb = embuf->ringbuffer;
               samples = _oalRingBufferGetNoSamples(rb);
            }
            if (dptr)
            {
               handle->pos = pos;
               _oalRingBufferSetOffsetSamples(rb, offs);
               rv = AAX_TRUE;
            }
            else _aaxErrorSet(AAX_INVALID_PARAMETER);
            break;
         case AAX_MICROSECONDS:
            duration = _oalRingBufferGetDuration(rb);
            while (fpos > duration)
            {
               pos++;
               fpos -= duration;
               _intBufReleaseData(dptr, _AAX_EMITTER_BUFFER);

               dptr = _intBufGet(src->buffers, _AAX_EMITTER_BUFFER, pos);
               if (!dptr) break;

               embuf = _intBufGetDataPtr(dptr);
               rb = embuf->ringbuffer;
               duration = _oalRingBufferGetDuration(rb);
            }
            if (dptr)
            {
               handle->pos = pos;
               _oalRingBufferSetOffsetSec(rb, fpos);
               rv = AAX_TRUE;
            }
            else _aaxErrorSet(AAX_INVALID_PARAMETER);
            break;
          default:
            _aaxErrorSet(AAX_INVALID_ENUM);
         }
         _intBufReleaseData(dptr, _AAX_EMITTER_BUFFER);
         
      }
      else {
         _aaxErrorSet(AAX_INVALID_REFERENCE);
      }
      _intBufReleaseNum(src->buffers, _AAX_EMITTER_BUFFER);
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

int
aaxEmitterSetOffsetSec(aaxEmitter emitter, float offs)
{
   _emitter_t* handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      if (!is_nan(offs))
      {
         _aaxEmitter *src = handle->source;
         _intBufferData *dptr;

         _intBufGetNum(src->buffers, _AAX_EMITTER_BUFFER);
         dptr = _intBufGet(src->buffers, _AAX_EMITTER_BUFFER, 0);
         if (dptr)
         {
            _embuffer_t *embuf = _intBufGetDataPtr(dptr);
            _oalRingBuffer *rb = embuf->ringbuffer;
            unsigned int pos = 0;
            float duration;

            duration = _oalRingBufferGetDuration(rb);
            while (offs > duration)
            {
               pos++;
               offs -= duration;
               _intBufReleaseData(dptr, _AAX_EMITTER_BUFFER);

               dptr = _intBufGet(src->buffers, _AAX_EMITTER_BUFFER, pos);
               if (!dptr) break;

               embuf = _intBufGetDataPtr(dptr);
               rb = embuf->ringbuffer;
               duration = _oalRingBufferGetDuration(rb);
            }
            if (dptr)
            {
               handle->pos = pos;
               _oalRingBufferSetOffsetSec(rb, offs);
               rv = AAX_TRUE;
            }
            else {
               _aaxErrorSet(AAX_INVALID_PARAMETER);
            }
            _intBufReleaseData(dptr, _AAX_EMITTER_BUFFER);
         }
         else {
            _aaxErrorSet(AAX_INVALID_REFERENCE);
         }
         _intBufReleaseNum(src->buffers, _AAX_EMITTER_BUFFER);
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

int
aaxEmitterGetMode(const aaxEmitter emitter, enum aaxModeType type)
{
   _emitter_t* handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      const _aaxEmitter *src = handle->source;
      switch(type)
      {
      case AAX_POSITION:
         if (_IS_POSITIONAL(src))
         {
            if (_IS_RELATIVE(src)) {
               rv = AAX_RELATIVE;
            } else {
               rv = AAX_ABSOLUTE;
            }
         } else {
            rv = AAX_MODE_NONE;
         }
         break;
      case AAX_LOOPING:
      {
         _intBufferData *dptr =_intBufGet(src->buffers, _AAX_EMITTER_BUFFER, 0);
         if (dptr)
         {
            _embuffer_t *embuf = _intBufGetDataPtr(dptr);
            rv = _oalRingBufferGetLooping(embuf->ringbuffer);
            _intBufReleaseData(dptr, _AAX_EMITTER_BUFFER);
         }
         break;
      }
      case AAX_BUFFER_TRACK:
         rv = handle->track;
         break;
      default:
         _aaxErrorSet(AAX_INVALID_ENUM);
         break;
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

int
aaxEmitterGetVelocity(const aaxEmitter emitter, aaxVec3f velocity)
{
   _emitter_t* handle = get_emitter(emitter);
   int rv = AAX_FALSE;
   if (handle)
   {
      if (velocity)
      {
         const _aaxEmitter *src = handle->source;
         vec3Copy(velocity, src->props3d->velocity);
         rv = AAX_TRUE;
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

int
aaxEmitterGetState(const aaxEmitter emitter)
{
   _emitter_t* handle = get_emitter(emitter);
   enum aaxState ret = AAX_STATE_NONE;
   if (handle)
   {
      _handle_t *thread = get_valid_handle(handle->handle);
      if (thread)
      {
         const _aaxEmitter *src = handle->source;
         if (_IS_PLAYING(src)) ret = AAX_PLAYING;
         else if (_IS_PROCESSED(src)) ret = AAX_PROCESSED;
         else if (_IS_STOPPED(src)) ret = AAX_STOPPED;
         else if (_IS_PAUSED(src)) ret = AAX_SUSPENDED;
         else ret = AAX_INITIALIZED;
       }
       else ret = AAX_INITIALIZED;
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return ret;
}

unsigned long
aaxEmitterGetOffset(const aaxEmitter emitter, enum aaxType type)
{
   _emitter_t* handle = get_emitter(emitter);
   unsigned long rv = 0;
   if (handle)
   {
      const _aaxEmitter *src = handle->source;
      _intBufferData *dptr;
      dptr = _intBufGet(src->buffers, _AAX_EMITTER_BUFFER, 0);
      if (dptr)
      {
         _embuffer_t *embuf = _intBufGetDataPtr(dptr);
         unsigned int i;

         switch (type)
         {
         case AAX_BYTES:
         case AAX_FRAMES:
         case AAX_SAMPLES:
            _intBufGetNum(src->buffers, _AAX_EMITTER_BUFFER);
            for (i=0; i<handle->pos; i++)
            {
               rv += _oalRingBufferGetNoSamples(embuf->ringbuffer);
               _intBufReleaseData(dptr, _AAX_EMITTER_BUFFER);

               dptr = _intBufGet(src->buffers, _AAX_EMITTER_BUFFER, i);
               embuf = _intBufGetDataPtr(dptr);
            }
            _intBufReleaseNum(src->buffers, _AAX_EMITTER_BUFFER);

            rv += _oalRingBufferGetOffsetSamples(embuf->ringbuffer);
            if (type == AAX_BYTES) {
               rv *= _oalRingBufferGetBytesPerSample(embuf->ringbuffer);
            }
            break;
         case AAX_MICROSECONDS:
            rv = src->curr_pos_sec * 1e6f;
            break;
         default:
            _aaxErrorSet(AAX_INVALID_ENUM);
         }
         _intBufReleaseData(dptr, _AAX_EMITTER_BUFFER);
      }
      else {
         _aaxErrorSet(AAX_INVALID_REFERENCE);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

float
aaxEmitterGetOffsetSec(const aaxEmitter emitter)
{
   _emitter_t* handle = get_emitter(emitter);
   float rv = 0.0f;
   if (handle)
   {
      const _aaxEmitter *src = handle->source;
      rv = src->curr_pos_sec;
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   put_emitter(handle);
   return rv;
}

/* -------------------------------------------------------------------------- */

_emitter_t*
get_emitter_unregistered(aaxEmitter em)
{
   _emitter_t *emitter = (_emitter_t *)em;
   _emitter_t *rv = NULL;

   if (emitter && emitter->id == EMITTER_ID && !emitter->handle) {
      rv = emitter;
   }
   return rv;
}

_emitter_t*
get_emitter(aaxEmitter em)
{
   _emitter_t *emitter = (_emitter_t *)em;
   _emitter_t *rv = NULL;

   if (emitter && emitter->id == EMITTER_ID)
   {
      _handle_t *handle = emitter->handle;
      if (handle && handle->id == HANDLE_ID)
      {
         _intBufferData *dptr = _intBufGet(handle->sensors, _AAX_SENSOR, 0);
         if (dptr)
         {
            _sensor_t* sensor = _intBufGetDataPtr(dptr);
            _aaxAudioFrame* mixer = sensor->mixer;
            _intBufferData *dptr_src;
            _intBuffers *he;

            if (!_IS_POSITIONAL(emitter->source)) {
               he = mixer->emitters_2d;
            } else {
               he = mixer->emitters_3d;
            }
            dptr_src = _intBufGet(he, _AAX_EMITTER, emitter->pos);
            emitter = _intBufGetDataPtr(dptr_src);
            _intBufReleaseData(dptr, _AAX_SENSOR);
         }
      }
      else if (handle && handle->id == AUDIOFRAME_ID)
      {
         _frame_t *handle = (_frame_t*)emitter->handle;
         _intBufferData *dptr_src;
         _intBuffers *he;

         if (!_IS_POSITIONAL(emitter->source)) {
            he = handle->submix->emitters_2d;
         } else {
            he = handle->submix->emitters_3d;
         }
         dptr_src = _intBufGet(he, _AAX_EMITTER, emitter->pos);
         emitter = _intBufGetDataPtr(dptr_src);
      }
      rv = emitter;
   }
   return rv;
}

void
put_emitter(aaxEmitter em)
{
   _emitter_t *emitter = (_emitter_t *)em;

   if (emitter && emitter->id == EMITTER_ID)
   {
      _handle_t *handle = emitter->handle;
      if (handle && handle->id == HANDLE_ID)
      {
          _intBufferData *dptr =_intBufGet(handle->sensors, _AAX_SENSOR, 0);
         if (dptr)
         {
            _sensor_t* sensor = _intBufGetDataPtr(dptr);
            _aaxAudioFrame* mixer = sensor->mixer;
            _intBuffers *he;

            if (!_IS_POSITIONAL(emitter->source)) {
               he = mixer->emitters_2d;
            } else {
               he = mixer->emitters_3d;
            }
            _intBufRelease(he, _AAX_EMITTER, emitter->pos);
            _intBufReleaseData(dptr, _AAX_SENSOR);
         }
      }
      else if (handle && handle->id == AUDIOFRAME_ID)
      {
         _frame_t *handle = (_frame_t*)emitter->handle;
         _intBuffers *he;

         if (!_IS_POSITIONAL(emitter->source)) {
            he = handle->submix->emitters_2d;
         } else {
            he = handle->submix->emitters_3d;
         }
         _intBufRelease(he, _AAX_EMITTER, emitter->pos);
      }
   }
}

static void
removeEmitterBufferByPos(void *source, unsigned int pos)
{
   _aaxEmitter *src = (_aaxEmitter *)source;
   _embuffer_t *embuf;
   
   embuf = _intBufRemove(src->buffers, _AAX_EMITTER_BUFFER, pos, AAX_FALSE);
   if (embuf)
   {
      free_buffer(embuf->buffer);
      _oalRingBufferDelete(embuf->ringbuffer);
      embuf->id = 0xdeadbeef;
      free(embuf);
      embuf = NULL;
   }
}

