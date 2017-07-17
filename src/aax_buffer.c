 /*
 * Copyright 2007-2017 by Erik Hofman.
 * Copyright 2009-2017 by Adalin B.V.
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
#ifdef HAVE_LIBIO_H
#include <libio.h>              /* for NULL */
#endif
#include <math.h>		/* for floorf */
#include <assert.h>

#include <xml.h>

#include <3rdparty/pffft.h>

#include <software/audio.h>
#include <software/rbuf_int.h>
#include <stream/device.h>
#include <dsp/common.h>

#include "devices.h"
#include "arch.h"
#include "api.h"

static _aaxRingBuffer* _bufGetRingBuffer(_buffer_t*, _handle_t*);
static _aaxRingBuffer* _bufDestroyRingBuffer(_buffer_t*);
static int _aaxBufferProcessWaveform(aaxBuffer, float, float, float, enum aaxWaveformType, float, enum aaxProcessingType);
static _aaxRingBuffer* _bufSetDataInterleaved(_buffer_t*, _aaxRingBuffer*, const void*, unsigned);
static _aaxRingBuffer* _bufConvertDataToMixerFormat(_buffer_t*, _aaxRingBuffer*);
static void _bufGetDataInterleaved(_aaxRingBuffer*, void*, unsigned int, unsigned int, float);
static void _bufConvertDataToPCM24S(void*, void*, unsigned int, enum aaxFormat);
static void _bufConvertDataFromPCM24S(void*, void*, unsigned int, unsigned int, enum aaxFormat, unsigned int);
static int _bufCreateFromAAXS(_buffer_t*, const void*, float);
static char** _bufCreateAAXS(_buffer_t*, void**, unsigned int);


static unsigned char  _aaxFormatsBPS[AAX_FORMAT_MAX];

AAX_API aaxBuffer AAX_APIENTRY
aaxBufferCreate(aaxConfig config, unsigned int samples, unsigned tracks,
                                   enum aaxFormat format)
{
   _handle_t* handle = (_handle_t*)config;
   unsigned int native_fmt = format & AAX_FORMAT_NATIVE;
   aaxBuffer rv = NULL;

   if ((native_fmt < AAX_FORMAT_MAX) && (samples*tracks > 0))
   {
      _buffer_t* buf = calloc(1, sizeof(_buffer_t));
      if (buf)
      {
         int blocksize;

         switch(native_fmt)
         {
         case AAX_IMA4_ADPCM:
            blocksize = DEFAULT_IMA4_BLOCKSIZE;
            break;
         default:
            blocksize = 1;
         }

         buf->id = BUFFER_ID;
         buf->ref_counter = 1;

         buf->no_tracks = tracks;
         buf->no_samples = samples;
         buf->blocksize = blocksize;
         buf->mipmap = AAX_FALSE;
         buf->pos = 0;
         buf->format = format;
         buf->frequency = 0.0f;
         buf->info = VALID_LITE_HANDLE(handle) ? &handle->info : &_info;
         buf->handle = handle;
         buf->ringbuffer = _bufGetRingBuffer(buf, handle);
         buf->to_mixer = AAX_FALSE;
         rv = (aaxBuffer)buf;
      }
      if (buf == NULL) {
         _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
      }
   }
   else
   {
      if (native_fmt >= AAX_FORMAT_MAX) {
         _aaxErrorSet(AAX_INVALID_ENUM);
      } else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxBufferSetSetup(aaxBuffer buffer, enum aaxSetupType type, unsigned int setup)
{
   _buffer_t* handle = get_buffer(buffer, __func__);
   unsigned int tmp;
   int rv = AAX_FALSE;
   if (handle)
   {
      _aaxRingBuffer* rb = _bufGetRingBuffer(handle, NULL);
      switch(type)
      {
      case AAX_FREQUENCY:
         if ((setup >= 1000) && (setup <= 96000))
         {
            if (rb && !handle->frequency) {
               rb->set_paramf(rb, RB_FREQUENCY, (float)setup);
            }
            handle->frequency = (float)setup;
            rv = AAX_TRUE;
         }
         else _aaxErrorSet(AAX_INVALID_PARAMETER);
         break;
      case AAX_TRACKS:
         if ((setup > 0) && (setup <= _AAX_MAX_SPEAKERS))
         {
            if (rb)
            {
               rv = rb->set_parami(rb, RB_NO_TRACKS, setup);
               if (!rv) {
                  _aaxErrorSet(AAX_INVALID_PARAMETER);
               }
            }
            handle->no_tracks = setup;
            rv = AAX_TRUE;
         }
         break;
      case AAX_FORMAT:
      {
         enum aaxFormat native_fmt = setup & AAX_FORMAT_NATIVE;
         if (native_fmt < AAX_FORMAT_MAX)
         {
            handle->format = setup;
            switch(native_fmt)
            {
            case AAX_IMA4_ADPCM:
               handle->blocksize = DEFAULT_IMA4_BLOCKSIZE;
               break;
            default:
               handle->blocksize = 1;
               break;
            }
            rv = AAX_TRUE;
         }
         else _aaxErrorSet(AAX_INVALID_PARAMETER);
         break;
      }
      case AAX_TRACKSIZE:
         if (rb)
         {
            rv = rb->set_parami(rb, RB_TRACKSIZE, setup);
            if (!rv) {
               _aaxErrorSet(AAX_INVALID_PARAMETER);
            }
         }
         tmp = handle->no_tracks * aaxGetBitsPerSample(handle->format);
         handle->no_samples = setup*8/tmp;
         rv = AAX_TRUE;
         break;
      case AAX_LOOP_START:
         if (setup < handle->no_samples)
         {
            if (rb)
            {
               int looping = (setup < handle->loop_end) ? AAX_TRUE : AAX_FALSE;
               rb->set_parami(rb, RB_LOOPPOINT_START, setup);
               rb->set_parami(rb, RB_LOOPING, looping);
            }
            handle->loop_start = setup;
            rv = AAX_TRUE;
         }
         else _aaxErrorSet(AAX_INVALID_PARAMETER);
         break;
      case AAX_LOOP_END:
         if (setup < handle->no_samples)
         {
            if (rb)
            {
               int looping = (handle->loop_start < setup) ? AAX_TRUE : AAX_FALSE;
               rb->set_parami(rb, RB_LOOPPOINT_END, setup);
               rb->set_parami(rb, RB_LOOPING, looping);
            }
            handle->loop_end = setup;
            rv = AAX_TRUE;
         }
         else _aaxErrorSet(AAX_INVALID_PARAMETER);
         break;
      case AAX_BLOCK_ALIGNMENT:
         if (setup > 1)
         {
            if (handle->format == AAX_IMA4_ADPCM)
            {
               handle->blocksize = setup;
               rv = AAX_TRUE;
            }
         }
         else if (handle->format != AAX_IMA4_ADPCM)
         {
            handle->blocksize = setup;
            rv = AAX_TRUE;
         }
         break;
      case AAX_POSITION:
         if (setup <= handle->no_samples)
         {
            handle->pos = setup;
            rv = AAX_TRUE;
         }
         else  _aaxErrorSet(AAX_INVALID_PARAMETER);
         break;
      default:
         _aaxErrorSet(AAX_INVALID_ENUM);
      }
   }
   return rv;
}

AAX_API unsigned int AAX_APIENTRY
aaxBufferGetSetup(const aaxBuffer buffer, enum aaxSetupType type)
{
   _buffer_t* handle = get_buffer(buffer, __func__);
   unsigned int rv = AAX_FALSE;
   if (handle)
   {
      _aaxRingBuffer* rb = _bufGetRingBuffer(handle, NULL);
      switch(type)
      {
      case AAX_FREQUENCY:
         rv = (unsigned int)handle->frequency;
         break;
      case AAX_TRACKS:
         rv = handle->no_tracks;
         break;
      case AAX_FORMAT:
         rv = handle->format;
         break;
      case AAX_TRACKSIZE:
         if (handle->frequency)
         {
            float fact = 1.0f;
            if (rb) {
               fact = handle->frequency / rb->get_paramf(rb, RB_FREQUENCY);
            }
            rv = handle->no_samples - handle->pos;
            rv *= (unsigned int)(fact*aaxGetBitsPerSample(handle->format));
            rv /= 8;
         }
         else _aaxErrorSet(AAX_INVALID_STATE);
         break;
      case AAX_NO_SAMPLES:
         if (handle->frequency)
         {
            float fact = 1.0f;
            if (rb) {
               fact = handle->frequency / rb->get_paramf(rb, RB_FREQUENCY);
            }
            rv = (unsigned int)(fact*(handle->no_samples - handle->pos));
         }
         else _aaxErrorSet(AAX_INVALID_STATE);
         break;
      case AAX_LOOP_START:
         rv = handle->loop_start;
         break;
      case AAX_LOOP_END:
         rv = handle->loop_end;
         break;
      case AAX_BLOCK_ALIGNMENT:
         rv = handle->blocksize;
         break;
      case AAX_POSITION:
         rv = handle->pos;
         break;
      default:
         _aaxErrorSet(AAX_INVALID_ENUM);
      }
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxBufferSetData(aaxBuffer buffer, const void* d)
{
   _buffer_t* handle = get_buffer(buffer, __func__);
   int rv = AAX_FALSE;
   if (handle && (handle->format & AAX_SPECIAL) && d)
   {				/* the data in *d isn't actual raw sound data */
      unsigned int format = handle->format;
      switch(format)
      {
      case AAX_AAXS16S:
      case AAX_AAXS24S:
         rv = _bufCreateFromAAXS(handle, d, 0);
         break;
      default:					/* should never happen */
         break;
      }
   }
   else if (handle)
   {
      _aaxRingBuffer *rb = _bufGetRingBuffer(handle, NULL);
      if (rb)
      {
         unsigned int tracks, no_samples, buf_samples;

         rb->init(rb, AAX_FALSE);
         tracks = rb->get_parami(rb, RB_NO_TRACKS);
         no_samples = rb->get_parami(rb, RB_NO_SAMPLES);

         buf_samples = tracks*no_samples;
         if (d && (buf_samples > 0))
         {
            unsigned blocksize =  handle->blocksize;
            unsigned int format = handle->format;
            void *data = (void*)d, *ptr = NULL;
            unsigned int native_fmt;
            char fmt_bps, *m = 0;
            const char *env;

				/* do we need to convert to native format? */
            native_fmt = format & AAX_FORMAT_NATIVE;
            if (format & ~AAX_FORMAT_NATIVE)
            {
               fmt_bps = _aaxFormatsBPS[native_fmt];
               ptr = (void**)_aax_malloc(&m, buf_samples*fmt_bps);
               if (!ptr)
               {
                  _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
                  return rv;
               }

               _aax_memcpy(m, data, buf_samples*fmt_bps);
               data = m;
					/* first convert to native endianness */
               if ( ((format & AAX_FORMAT_LE) && is_bigendian()) ||
                    ((format & AAX_FORMAT_BE) && !is_bigendian()) )
               {
                  switch (native_fmt)
                  {
                  case AAX_PCM16S:
                     _batch_endianswap16(data, buf_samples);
                     break;
                  case AAX_PCM24S:
                  case AAX_PCM32S:
                  case AAX_FLOAT:
                     _batch_endianswap32(data, buf_samples);
                     break;
                  case AAX_DOUBLE:
                     _batch_endianswap64(data, buf_samples);
                     break;
                  default:
                     break;
                  }
               }
					/* then convert to proper signedness */
               if (format & AAX_FORMAT_UNSIGNED)
               {
                  switch (native_fmt)
                  {  
                  case AAX_PCM8S:
                     _batch_cvt8u_8s(data, buf_samples);
                     break;
                  case AAX_PCM16S:
                     _batch_cvt16u_16s(data, buf_samples);
                     break;
                  case AAX_PCM24S:
                     _batch_cvt24u_24s(data, buf_samples);
                     break;
                  case AAX_PCM32S:
                     _batch_cvt32u_32s(data, buf_samples);
                     break;
                  default:
                     break;
                  }
               }
            }

            /* explicit request not to convert */
            env = getenv("AAX_USE_MIXER_FMT");
            if (env && !_aax_getbool(env)) {
               handle->to_mixer = AAX_FALSE;
            }

            /* sound is not mono or larger than 4Mb, do not convert */
            else if (tracks != 1 || rb->get_parami(rb, RB_TRACKSIZE) > (4*1024)) {
                handle->to_mixer = AAX_FALSE;
            }

            /* more than 500Mb free memory is available, convert */
            else if (_aax_get_free_memory() > (500*1024*1024)) {
               handle->to_mixer = AAX_TRUE;
            }
            rb = _bufSetDataInterleaved(handle, rb, data, blocksize);
            handle->ringbuffer = rb;

            rv = AAX_TRUE;
            _aax_free(ptr);
         }
         else {
            _aaxErrorSet(AAX_INVALID_PARAMETER);
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_STATE);
      }
   }
   return rv;
}

AAX_API int AAX_APIENTRY aaxBufferProcessWaveform(aaxBuffer buffer, float rate, enum aaxWaveformType wtype, float ratio, enum aaxProcessingType ptype)
{
   return _aaxBufferProcessWaveform(buffer, rate, 1.0f, rate, wtype, ratio, ptype);
}

AAX_API void** AAX_APIENTRY
aaxBufferGetData(const aaxBuffer buffer)
{
   _buffer_t* handle = get_buffer(buffer, __func__);
   void** data = NULL;
   if (handle && handle->frequency)
   {
      unsigned int buf_samples, no_samples, tracks;
      unsigned int native_fmt, rb_format, pos;
      enum aaxFormat user_format;
      _aaxRingBuffer *rb;
      char *ptr, bps;
      float fact;

      rb = _bufGetRingBuffer(handle, NULL);
      fact = handle->frequency / rb->get_paramf(rb, RB_FREQUENCY);
      pos = (unsigned int)(fact*handle->pos);

      no_samples = (unsigned int)(fact*rb->get_parami(rb, RB_NO_SAMPLES) - pos);
      bps = rb->get_parami(rb, RB_BYTES_SAMPLE);
      tracks = rb->get_parami(rb, RB_NO_TRACKS);
      buf_samples = tracks*no_samples;

      ptr = (char*)sizeof(void*);
      data = (void**)_aax_malloc(&ptr, no_samples*tracks*bps);
      if (data == NULL) 
      {
         _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
         return data;
      }

      _bufGetDataInterleaved(rb, ptr, no_samples, tracks, fact);
      *data = (void*)(ptr + pos*tracks*bps);

      user_format = handle->format;
      if (user_format == AAX_AAXS16S || user_format == AAX_AAXS24S) {
         user_format = AAX_FLOAT;
      }
      native_fmt = user_format & AAX_FORMAT_NATIVE;
      rb_format = rb->get_parami(rb, RB_FORMAT);
      if (rb_format != native_fmt)
      {
         if (rb_format != AAX_PCM24S) 	/* first convert to signed 24-bit */
         {
            void **ndata;
            char *ptr;

            ptr = (char*)sizeof(void*);
            ndata = (void**)_aax_malloc(&ptr, buf_samples*sizeof(int32_t));
            if (ndata)
            {
               *ndata = (void*)ptr;
               _bufConvertDataToPCM24S(*ndata, *data, buf_samples, rb_format);
               free(data);
               data = ndata;
            }
         } 

         if (native_fmt != AAX_PCM24S)	/* then convert to requested format */
         {
            int new_bps = aaxGetBytesPerSample(native_fmt);
            int block_smp = IMA4_BLOCKSIZE_TO_SMP(handle->blocksize);
            int new_samples = ((no_samples/block_smp)+1)*block_smp;
            void** ndata;
            char *ptr;

            ptr = (char*)sizeof(void*);
            ndata = (void**)_aax_malloc(&ptr, tracks*new_samples*new_bps);
            if (ndata)
            {
               *ndata = (void*)ptr;
               _bufConvertDataFromPCM24S(*ndata, *data, tracks, no_samples,
                                         native_fmt, handle->blocksize);
               free(data);
               data = ndata;
            }
         }
      } /* rb_format != native_fmt */
 
			/* do we need to convert to non-native format? */
      if (user_format & ~AAX_FORMAT_NATIVE)
      {
					/* do we need to change signedness? */
         if (user_format & AAX_FORMAT_UNSIGNED)
         {
            int signed_format = user_format & ~(AAX_FORMAT_LE | AAX_FORMAT_BE);
            switch (signed_format)
            {
            case AAX_PCM8U:
               _batch_cvt8s_8u(*data, buf_samples);
               break;
            case AAX_PCM16U:
               _batch_cvt16s_16u(*data, buf_samples);
               break;
            case AAX_PCM24U:
               _batch_cvt24s_24u(*data, buf_samples);
               break;
            case AAX_PCM32U:
               _batch_cvt32s_32u(*data, buf_samples);
               break;
            default:
               break;
            }
         }
					/* convert to requested endianness */
         if ( ((user_format & AAX_FORMAT_LE) && is_bigendian()) ||
              ((user_format & AAX_FORMAT_BE) && !is_bigendian()) )
         {
            switch (native_fmt)
            {
            case AAX_PCM16S:
               _batch_endianswap16(*data, buf_samples);
               break;
            case AAX_PCM24S:
            case AAX_PCM32S:
            case AAX_FLOAT:
               _batch_endianswap32(*data, buf_samples);
               break;
            case AAX_DOUBLE:
               _batch_endianswap64(*data, buf_samples);
               break;
            default:
               break;
            }
         }
      }

      if (handle->format == AAX_AAXS16S || handle->format == AAX_AAXS24S)
      {
         data = (void**)_bufCreateAAXS(handle, data, buf_samples*sizeof(float));
      }
   }
   else if (handle) {	/* handle->frequency is not set */
      _aaxErrorSet(AAX_INVALID_STATE);
   }

   return data;
}

AAX_API int AAX_APIENTRY
aaxBufferDestroy(aaxBuffer buffer)
{
   _buffer_t* handle = get_buffer(buffer, __func__);
   int rv = AAX_FALSE;
   if (handle) {
     rv = free_buffer(handle);
   }
   return rv;
}

/**
 * This creates a buffer from an audio file indicated by an URL
 */
AAX_API aaxBuffer AAX_APIENTRY
aaxBufferReadFromStream(aaxConfig config, const char *url)
{
   const _aaxDriverBackend *stream = &_aaxStreamDriverBackend;
   _handle_t *handle = (_handle_t*)config;
   aaxBuffer rv = NULL;

   if (handle && stream)
   {
      static const char *xcfg = "<?xml?><_ctb8>1</_ctb8>";
      void *id = stream->new_handle(AAX_MODE_READ);
      void *xid = xmlInitBuffer(xcfg, strlen(xcfg));

      // xid makes the stream return sound data in file format when capturing
      id = stream->connect(config, id, xid, url, AAX_MODE_READ);
      xmlClose(xid);

      if (id)
      {
         _aaxMixerInfo* info = handle->info;
         float refrate = info->refresh_rate;
         float periodrate = info->period_rate;
         unsigned tracks = info->no_tracks;
         float freq = info->frequency;
         int brate = info->bitrate;
         int fmt = info->format;
         int res;

         res = stream->setup(id, &refrate, &fmt, &tracks, &freq, &brate,
                                 AAX_FALSE, periodrate);
         if (res)
         {
            size_t no_samples, no_bytes, blocksize, datasize, bits;
            char *ptr, *ptr2;

            bits = aaxGetBitsPerSample(fmt);
            blocksize = stream->param(id, DRIVER_BLOCK_SIZE);
            no_samples = stream->param(id, DRIVER_MAX_SAMPLES);
            no_bytes = no_samples*bits/8;

            no_bytes = ((no_bytes/blocksize)+1)*blocksize;
            datasize =  SIZE_ALIGNED(tracks*no_bytes);

            ptr2 = (char*)(2 * sizeof(void*));
            ptr = _aax_calloc(&ptr2, 2, sizeof(void*) + datasize);
            if (ptr)
            {
               void **dst = (void **)ptr;
               ssize_t res, offset = 0;
               ssize_t offs_packets = 0;

               dst[0] = ptr2;
               ptr2 += datasize;
               dst[1] = ptr2;

               // capture now returns native file format instead of PCM24S
               // in batched capturing mode
               do
               {
                  ssize_t offs = offs_packets;
                  size_t packets = no_samples;

                  assert(packets <= datasize*8/(tracks*bits));

                  res = stream->capture(id, dst, &offs, &packets,
                                            dst[1], datasize, 1.0f, AAX_TRUE);
                  offs_packets += packets;
                  if (res > 0) {
                     offset += res*8/(bits*tracks);
                  }
               }
               while (res);

               // get the actual number of samples
               no_samples = stream->param(id, DRIVER_MAX_SAMPLES);
               rv = aaxBufferCreate(config, no_samples, tracks, fmt);
               if (rv)
               {
                   aaxBufferSetSetup(rv, AAX_FREQUENCY, freq);
                   aaxBufferSetSetup(rv, AAX_BLOCK_ALIGNMENT, blocksize);
                   aaxBufferSetData(rv, dst[0]);
#if 0
_aaxFileDriverWrite("/tmp/test.wav", AAX_OVERWRITE, dst[0], no_samples, freq, tracks, fmt);
#endif
               }
               free(ptr);
            }
         }
         stream->disconnect(id);
      }
   }

   return rv;
}

AAX_API int AAX_APIENTRY
aaxBufferWriteToFile(aaxBuffer buffer, const char *file, enum aaxProcessingType type)
{
   int rv = AAX_FALSE;
   if (aaxIsValid(buffer, AAX_BUFFER))
   {
      enum aaxFormat format = aaxBufferGetSetup(buffer, AAX_FORMAT);
      unsigned int samples = aaxBufferGetSetup(buffer, AAX_NO_SAMPLES);
      unsigned int freq = aaxBufferGetSetup(buffer, AAX_FREQUENCY);
      char tracks = aaxBufferGetSetup(buffer, AAX_TRACKS);
#if 0
      _buffer_t* handle = get_buffer(buffer, __func__);
      _aaxRingBuffer* rb = _bufGetRingBuffer(handle, NULL);
      _aaxRingBufferSample *rbd = rb->sample;
      const int32_t **data;

      data = (const int32_t**)rbd->track;
      rv = _aaxFileDriverWrite(file, type, data, samples, freq, tracks, format);
#else
      void **data = aaxBufferGetData(buffer);
      _buffer_t *handle = (_buffer_t*)buffer;

      format = handle->format;
      freq = handle->frequency;
      _aaxFileDriverWrite(file, type, *data, samples, freq, tracks, format);
      free(data);
      rv = AAX_TRUE;
#endif
   }
   return rv;
}

/* -------------------------------------------------------------------------- */

static unsigned char  _aaxFormatsBPS[AAX_FORMAT_MAX] =
{
  1,    /* 8-bit          */
  2,    /* 16-bit         */
  4,    /* 24-bit         */
  4,    /* 32-bit         */
  4,    /* 32-bit floats  */
  8,    /* 64-bit doubles */
  1,    /* mu-law         */
  1,    /* a-law          */
  1     /* IMA4-ADPCM     */
};

_buffer_t*
get_buffer(aaxBuffer buffer, const char *func)
{
   _buffer_t *handle = (_buffer_t *)buffer;
   _buffer_t* rv  = NULL;

   if (handle && handle->id == BUFFER_ID) {
      rv = handle;
   }
   else if (handle && handle->id == FADEDBAD) {
      __aaxErrorSet(AAX_DESTROYED_HANDLE, func);
   }
   else {
      __aaxErrorSet(AAX_INVALID_HANDLE, func);
   }

   return rv;
}

int
free_buffer(_buffer_t* handle)
{
   int rv = AAX_FALSE;
   if (handle)
   {
      if (--handle->ref_counter == 0)
      {
         handle->ringbuffer = _bufDestroyRingBuffer(handle);

         /* safeguard against using already destroyed handles */
         handle->id = FADEDBAD;
         free(handle);
      }
      rv = AAX_TRUE;
   }
   return rv;
}

static _aaxRingBuffer*
_bufGetRingBuffer(_buffer_t* buf, _handle_t *handle)
{
   _aaxRingBuffer *rb = buf->ringbuffer;
   if (!rb && (VALID_LITE_HANDLE(handle) ||
               (buf->info && *buf->info &&
                VALID_LITE_HANDLE((_handle_t*)((*buf->info)->backend)))
              )
      )
   {
      enum aaxRenderMode mode = handle->info->mode;
      const _aaxDriverBackend *be;

      if VALID_LITE_HANDLE(handle) {
         be = handle->backend.ptr;
      } else {
         be = ((_handle_t*)((*buf->info)->backend))->backend.ptr;
      }
 
      rb = be->get_ringbuffer(0.0f, mode);
      if (rb)
      {
         /* initialize the ringbuffer in native format only */
         rb->set_format(rb, buf->format & AAX_FORMAT_NATIVE, AAX_FALSE);
         rb->set_parami(rb, RB_NO_TRACKS, buf->no_tracks);
         rb->set_parami(rb, RB_NO_SAMPLES, buf->no_samples);
         rb->set_parami(rb, RB_LOOPPOINT_START, buf->loop_start);
         rb->set_parami(rb, RB_LOOPPOINT_END, buf->loop_end);
         rb->set_paramf(rb, RB_FREQUENCY, buf->frequency);
//       rb->set_paramf(rb, RB_BLOCKSIZE, buf->blocksize);
         /* Postpone until aaxBufferSetData gets called
          * rb->init(rb, AAX_FALSE);
         */
      }
   }
   return rb;
}

static _aaxRingBuffer*
_bufDestroyRingBuffer(_buffer_t* buf)
{
   _aaxRingBuffer *rb = buf->ringbuffer;
   if (rb && (buf->info && *buf->info &&
              VALID_LITE_HANDLE((_handle_t*)((*buf->info)->backend)))
      )
   {
      _handle_t *handle = (_handle_t*)(*buf->info)->backend;
      const _aaxDriverBackend *be = handle->backend.ptr;

      be->destroy_ringbuffer(rb);
      rb = NULL;
   }
   return rb;
}

static int
_bufCreateFromAAXS(_buffer_t* handle, const void* d, float freq)
{
   int rv = AAX_FALSE;
   void *xid;

   assert(d);

   xid = xmlInitBuffer(d, strlen(d));
   if (xid)
   {
      void *xsid = xmlNodeGet(xid, "/sound");
      if (xsid)
      {
         unsigned int i, num = xmlNodeGetNum(xsid, "waveform");
         void *xwid = xmlMarkId(xsid);
//       _aaxRingBuffer* rb;

         if (!freq) freq = xmlAttributeGetDouble(xsid, "frequency");
         /* for backwards combatibility, remove with version 3.0 */
         if (!freq) freq = xmlAttributeGetDouble(xsid, "freq_hz");
         if (!freq) freq = 1000.0f;

         for (i=0; i<num; i++)
         {
            if (xmlNodeGetPos(xsid, xwid, "waveform", i) != 0)
            {
               enum aaxProcessingType ptype = AAX_OVERWRITE;
               enum aaxWaveformType wtype = AAX_SINE_WAVE;
               float pitch, ratio, staticity;

               ratio = xmlNodeGetDouble(xwid, "ratio");
               pitch = xmlNodeGetDouble(xwid, "pitch");
               staticity = xmlNodeGetDouble(xwid, "staticity");

               if (!xmlAttributeCompareString(xwid, "src", "brownian-noise")) 
               {
                   wtype = AAX_BROWNIAN_NOISE;
               }
               else if (!xmlAttributeCompareString(xwid, "src", "white-noise"))
               {
                   wtype = AAX_WHITE_NOISE;
               } 
               else if (!xmlAttributeCompareString(xwid, "src", "pink-noise")) 
               {
                   wtype = AAX_PINK_NOISE;
               }
               else if (!xmlAttributeCompareString(xwid, "src", "square"))
               {
                  wtype = AAX_SQUARE_WAVE;
               }
               else if (!xmlAttributeCompareString(xwid, "src", "triangle"))
               {
                   wtype = AAX_TRIANGLE_WAVE;
               }
               else if (!xmlAttributeCompareString(xwid, "src", "sawtooth"))
               {
                   wtype = AAX_SAWTOOTH_WAVE;
               }
               else if (!xmlAttributeCompareString(xwid, "src", "impulse"))
               {
                   wtype = AAX_IMPULSE_WAVE;
               }
               else	// !xmlAttributeCompareString(xwid, "src", "sine")
               {
                  wtype = AAX_SINE_WAVE;
               }

               if (!xmlNodeCompareString(xwid, "processing", "add"))
               {
                  ptype = AAX_ADD;
                  if (!ratio) ratio = 1.0f;
                  if (!pitch) pitch = 1.0f;
               }
               else if (!xmlNodeCompareString(xwid, "processing", "mix"))
               {
                  ptype = AAX_MIX;
                  if (!ratio) ratio = 0.5f;
                  if (!pitch) pitch = 1.0f;
               }
               else if (!xmlNodeCompareString(xwid, "processing", "modulate"))
               {
                  ptype = AAX_RINGMODULATE;
                  if (!ratio) ratio = 1.0f;
                  if (!pitch) pitch = 1.0f;
               }
               else  // !xmlNodeCompareString(xwid, "processing", "overwrite")
               {
                  ptype = AAX_OVERWRITE;
                  if (!ratio) ratio = 1.0f;
                  if (!pitch) pitch = 1.0f;
               }

               rv = _aaxBufferProcessWaveform(handle, freq, pitch, staticity, wtype, ratio, ptype);
               if (rv == AAX_FALSE) break;
            }
         }
         xmlFree(xwid);
         xmlFree(xsid);

//       rb = _bufGetRingBuffer(handle, NULL);
//       rb->limit(rb, RB_LIMITER_ELECTRONIC);
      }
      else {
         _aaxErrorSet(AAX_INVALID_STATE);
      }
      xmlClose(xid);
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }

   return rv;
}

#define N	4096
#define NMAX	(N/2)
static char**
_bufCreateAAXS(_buffer_t *handle, void **data, unsigned int dlen)
{
   PFFFT_Setup *fft;
   float *output, *ptr;
   float *power;
   char **rv;
   int i;

   rv = data;
   output = _aax_aligned_alloc(2*N*sizeof(float));

   fft = pffft_new_setup(N, PFFFT_COMPLEX);
   pffft_transform_ordered(fft, (float*)*data, output, 0, PFFFT_FORWARD);
   pffft_destroy_setup(fft);

   ptr = output;
   power = _aax_aligned_alloc(NMAX*sizeof(float));
   for (i=0; i<NMAX; ++i)
   {
      float Re = *ptr++;
      float Im = *ptr++;
      power[i] = Re*Re + Im*Im;
   }
   _aax_aligned_free(output);

   /* anlyze the frequencies present in the power spectrum */
   _aaxRingBuffer* rb = _bufGetRingBuffer(handle, NULL);
   float fs2 = 0.5f*rb->get_paramf(rb, RB_FREQUENCY);

   int n = 0;
   float max = 0;
   for (i=1; i<NMAX-1; ++i)
   {
      if (power[i] > max) {
         max = power[i]; n = i;
      }
   }

   for (i=n; i<NMAX-1; i += n)
   {
      float q = power[i]/max;
      if (q >= LEVEL_96DB) {
         printf("% 6.0f Hz: %f\n", fs2*i/N, q);
      }
   }

   _aax_aligned_free(power);

   return rv;
}

static int
_aaxBufferProcessWaveform(aaxBuffer buffer, float freq, float pitch, float staticity, enum aaxWaveformType wtype, float ratio, enum aaxProcessingType ptype)
{
   _buffer_t* handle = get_buffer(buffer, __func__);
   int rv = AAX_FALSE;

   if (wtype > AAX_LAST_WAVEFORM) {
      _aaxErrorSet(AAX_INVALID_PARAMETER + 3);
   } else if (ratio > 1.0f || ratio < -1.0f) {
      _aaxErrorSet(AAX_INVALID_PARAMETER + 4);
   } else if (ptype >= AAX_PROCESSING_MAX) {
      _aaxErrorSet(AAX_INVALID_PARAMETER + 5);
   }
   else if (handle && handle->info && (*handle->info && ((*handle->info)->id == INFO_ID)))
   {
      _aaxRingBuffer* rb = _bufGetRingBuffer(handle, NULL);
      float phase = (ratio < 0.0f) ? GMATH_PI : 0.0f;
      float f, samps_period, fs, fw, fs_mixer, rate;
      unsigned int no_samples, i, bit = 1;
      unsigned skip;

      rate = freq * pitch;
      ratio = fabsf(ratio);
      fw = FNMINMAX(rate, 1.0f, 22050.0f);
      skip = (unsigned char)(1.0f + 99.0f*_MINMAX(staticity, 0.0f, 1.0f));

      fs = rb->get_paramf(rb, RB_FREQUENCY);
      no_samples = rb->get_parami(rb, RB_NO_SAMPLES);
      samps_period = fs/fw;

      if (rb->get_state(rb, RB_IS_VALID) == AAX_FALSE)
      {
          no_samples = floorf(no_samples/samps_period)*samps_period;
          rb->set_parami(rb, RB_NO_SAMPLES, no_samples);
          rb->init(rb, AAX_FALSE);
       }
       f = (float)no_samples/(float)samps_period;
       f = fw*ceilf(f)/f;

      switch (ptype)
      {
      case AAX_OVERWRITE:
         rb->set_state(rb, RB_CLEARED);
         break;
      case AAX_MIX:
      {
         float ratio_orig = FNMINMAX(1.0f-ratio, 0.0f, 1.0f);

         ratio = 2.0f*(1.0f - ratio_orig);
         if (wtype & AAX_SINE_WAVE) ratio /= 2;
         if (wtype & AAX_SQUARE_WAVE) ratio /= 2;
         if (wtype & AAX_TRIANGLE_WAVE) ratio /= 2;
         if (wtype & AAX_SAWTOOTH_WAVE) ratio /= 2;
         if (wtype & AAX_IMPULSE_WAVE) ratio /= 2;
         if (wtype & AAX_WHITE_NOISE) ratio /= 2;
         if (wtype & AAX_PINK_NOISE) ratio /= 2;
         if (wtype & AAX_BROWNIAN_NOISE) ratio /= 2;

         rb->data_multiply(rb, 0, 0, ratio_orig);
         break;
      }
      case AAX_RINGMODULATE:
         ratio = -ratio;
         break;
      case AAX_ADD:
      default:
         break;
      }

      rv = AAX_TRUE;
      if (handle->info && *handle->info) {
         fs_mixer = (*handle->info)->frequency;
      } else {
         fs_mixer = 0.0f;
      }

      for (i=0; i<AAX_MAX_WAVE; i++)
      {
         float dc = 1.0; /* duty cicle for noise */
         switch (wtype & bit)
         {
         case AAX_SINE_WAVE:
         case AAX_SQUARE_WAVE:
         case AAX_TRIANGLE_WAVE:
         case AAX_SAWTOOTH_WAVE:
         case AAX_IMPULSE_WAVE:
            rv = rb->data_mix_waveform(rb, wtype & bit, f, ratio, phase);
            break;
         case AAX_WHITE_NOISE:
         case AAX_PINK_NOISE:
         case AAX_BROWNIAN_NOISE:
            rv = rb->data_mix_noise(rb, wtype & bit, fs_mixer, pitch, ratio, dc, skip);
            break;
         default:
            break;
         }
         bit <<= 1;
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   return rv;
}

/*
 * Convert the buffer to 24-bit
 */
static void
_aaxMuLaw2Linear(int32_t*ndata, uint8_t* data, unsigned int i)
{
   do {
      *ndata++ = _mulaw2linear(*data++) << 8;
   } while (--i);
}

static void
_aaxALaw2Linear(int32_t*ndata, uint8_t* data, unsigned int i)
{  
   do {
      *ndata++ = _alaw2linear(*data++) << 8;
   } while (--i);
}

static void
_bufConvertDataToPCM24S(void *ndata, void *data, unsigned int samples, enum aaxFormat format)
{
   if (ndata)
   {
      int native_fmt = format & AAX_FORMAT_NATIVE;

      if (format != native_fmt)
      {
                                /* first convert to requested endianness */
         if ( ((format & AAX_FORMAT_LE) && is_bigendian()) ||
              ((format & AAX_FORMAT_BE) && !is_bigendian()) )
         {
            switch (native_fmt)
            {
            case AAX_PCM16S:
               _batch_endianswap16(data, samples);
               break;
            case AAX_PCM24S:
            case AAX_PCM32S:
            case AAX_FLOAT:
               _batch_endianswap32(data, samples);
               break;
            case AAX_DOUBLE:
               _batch_endianswap64(data, samples);
               break;
            default:
               break;
            }
         }
                                /* then convert to proper signedness */
         if (format & AAX_FORMAT_UNSIGNED)
         {
            switch (native_fmt)
            {
            case AAX_PCM8S:
               _batch_cvt8u_8s(data, samples);
               break;
            case AAX_PCM16S:
               _batch_cvt16u_16s(data, samples);
               break;
            case AAX_PCM24S:
               _batch_cvt24u_24s(data, samples);
               break;
            case AAX_PCM32S:
               _batch_cvt32u_32s(data, samples);
               break;
            default:
               break;
            }
         }
      }

      switch(native_fmt)
      {
      case AAX_PCM8S:
         _batch_cvt24_8(ndata, data, samples);
         break;
      case AAX_IMA4_ADPCM:
         /* the ringbuffer uses AAX_PCM16S internally for AAX_IMA4_ADPCM */
      case AAX_PCM16S:
         _batch_cvt24_16(ndata, data, samples);
         break;
      case AAX_PCM32S:
         _batch_cvt24_32(ndata, data, samples);
         break;
      case AAX_FLOAT:
        _batch_cvt24_ps(ndata, data, samples);
         break;
      case AAX_DOUBLE:
         _batch_cvt24_pd(ndata, data, samples);
         break;
      case AAX_MULAW:
         _aaxMuLaw2Linear(ndata, data, samples);
         break;
      case AAX_ALAW:
         _aaxALaw2Linear(ndata, data, samples);
         break;
      default:
         break;
      }
   } /* ndata */
}

static _aaxRingBuffer*
_bufConvertDataToMixerFormat(_buffer_t *buf, _aaxRingBuffer *rb)
{
   _aaxRingBuffer *nrb = rb;
   unsigned int no_samples = rb->get_parami(rb, RB_NO_SAMPLES);
   unsigned int tracks = rb->get_parami(rb, RB_NO_TRACKS);
   _handle_t *handle = buf->handle;

   if (handle)
   {
      enum aaxRenderMode mode = handle->info->mode;
      const _aaxDriverBackend *be;

      if VALID_LITE_HANDLE(handle) {
         be = handle->backend.ptr;
      } else {
         be = ((_handle_t*)((*buf->info)->backend))->backend.ptr;
      }

      nrb = be->get_ringbuffer(0.0f, mode);
      if (nrb)
      {
         int32_t **src, **dst;
         unsigned int t, fmt;

         nrb->set_format(nrb, AAX_PCM24S, AAX_FALSE);
         nrb->set_parami(nrb, RB_NO_TRACKS, buf->no_tracks);
         nrb->set_parami(nrb, RB_NO_SAMPLES, buf->no_samples);
         nrb->set_parami(nrb, RB_LOOPPOINT_START, buf->loop_start);
         nrb->set_parami(nrb, RB_LOOPPOINT_END, buf->loop_end);
         nrb->set_paramf(nrb, RB_FREQUENCY, buf->frequency);
         nrb->init(nrb, AAX_FALSE);

         fmt = rb->get_parami(rb, RB_FORMAT);
         src = (int32_t**)rb->get_tracks_ptr(rb, RB_READ);
         dst = (int32_t**)nrb->get_tracks_ptr(nrb, RB_WRITE);
         for (t=0; t<tracks; ++t) {
            _bufConvertDataToPCM24S(dst[t], src[t], no_samples, fmt);
         }
         nrb->release_tracks_ptr(nrb);
         be->destroy_ringbuffer(rb);
      }
      else {
         nrb = rb;
      }
   }

   return nrb;
}

static void
_aaxLinear2MuLaw(uint8_t* ndata, int32_t* data, unsigned int i)
{
   do {
      *ndata++ = _linear2mulaw(*data++ >> 8);
   } while (--i);
}

static void
_aaxLinear2ALaw(uint8_t* ndata, int32_t* data, unsigned int i)
{
   do {
      *ndata++ = _linear2alaw(*data++ >> 8);
   } while (--i);
}

/*
 * Incompatible with MS-IMA which specifies a different way of interleaving.
 */
static void
_aaxLinear2IMA4(uint8_t* ndata, int32_t* data, unsigned int samples, unsigned block_smp, unsigned tracks)
{
   unsigned int i, no_blocks, blocksize;
   int16_t sample = 0;
   uint8_t index = 0;

   no_blocks = samples/block_smp;
   blocksize = IMA4_SMP_TO_BLOCKSIZE(block_smp);

   for(i=0; i<no_blocks; i++)
   {
      _pcm_cvt_lin_to_ima4_block(ndata, data, block_smp, &sample, &index, tracks);
      ndata += blocksize*tracks;
      data += block_smp*tracks;
   }

   if (no_blocks*block_smp < samples)
   {
      unsigned int rest = (no_blocks+1)*block_smp - samples;

      samples = block_smp - rest;
      _pcm_cvt_lin_to_ima4_block(ndata, data, samples, &sample, &index, tracks);

      ndata += IMA4_SMP_TO_BLOCKSIZE(samples);
      memset(ndata, 0, rest/2);
   }
}

static void
_bufConvertDataFromPCM24S(void *ndata, void *data, unsigned int tracks, unsigned int no_samples, enum aaxFormat format, unsigned int blocksize)
{
   if (ndata)
   {
      int native_fmt = format & AAX_FORMAT_NATIVE;
      unsigned int samples = tracks*no_samples;

      if (format != native_fmt)
      {
                                /* first convert to requested endianness */
         if ( ((format & AAX_FORMAT_LE) && is_bigendian()) ||
              ((format & AAX_FORMAT_BE) && !is_bigendian()) )
         {
            switch (native_fmt)
            {
            case AAX_PCM16S:
               _batch_endianswap16(data, samples);
               break;
            case AAX_PCM24S:
            case AAX_PCM32S:
            case AAX_FLOAT:
               _batch_endianswap32(data, samples);
               break;
            case AAX_DOUBLE:
               _batch_endianswap64(data, samples);
               break;
            default:
               break;
            }
         }
                                /* then convert to proper signedness */
         if (format & AAX_FORMAT_UNSIGNED)
         {
            switch (native_fmt)
            {
            case AAX_PCM8S:
               _batch_cvt8u_8s(data, samples);
               break;
            case AAX_PCM16S:
               _batch_cvt16u_16s(data, samples);
               break;
            case AAX_PCM24S:
               _batch_cvt24u_24s(data, samples);
               break;
            case AAX_PCM32S:
               _batch_cvt32u_32s(data, samples);
               break;
            default:
               break;
            }
         }
      }

      switch(native_fmt)
      {
      case AAX_PCM8S:
         _batch_cvt8_24(ndata, data, samples);
         break;
      case AAX_PCM16S:
         _batch_cvt16_24(ndata, data, samples);
         break;
      case AAX_PCM32S:
         _batch_cvt32_24(ndata, data, samples);
         break;
      case AAX_FLOAT:
         _batch_cvtps_24(ndata, data, samples);
         break;
      case AAX_DOUBLE:
         _batch_cvtpd_24(ndata, data, samples);
         break;
      case AAX_MULAW:
         _aaxLinear2MuLaw(ndata, data, samples);
         break;
      case AAX_ALAW:
         _aaxLinear2ALaw(ndata, data, samples);
         break;
      case AAX_IMA4_ADPCM:
      {
         int block_smp = IMA4_BLOCKSIZE_TO_SMP(blocksize);
         unsigned t;
         for (t=0; t<tracks; t++)
         {
            uint8_t *dst = (uint8_t *)ndata + t*blocksize;
            int32_t *src = (int32_t *)data + t;
            _aaxLinear2IMA4(dst, src, no_samples, block_smp, tracks);
         }
         break;
      }
      default:
         break;
      }
   } /* ndata */
}

/*
 * Convert 4-bit IMA to 16-bit PCM
 */
static void
_aaxRingBufferIMA4ToPCM16(int32_t **__restrict dst, const void *__restrict src, int tracks, int blocksize, unsigned int no_samples)
{
   unsigned int i, blocks, block_smp;
   int16_t *d[_AAX_MAX_SPEAKERS];
   uint8_t *s = (uint8_t *)src;
   int t;

   if (tracks > _AAX_MAX_SPEAKERS)
      return;

   /* copy buffer pointers */
   for(t=0; t<tracks; t++) {
      d[t] = (int16_t*)dst[t];
   }

   block_smp = IMA4_BLOCKSIZE_TO_SMP(blocksize);
   blocks = no_samples / block_smp;
   i = blocks-1;
   do
   {
      for (t=0; t<tracks; t++)
      {
         _sw_bufcpy_ima_adpcm(d[t], s, block_smp);
         d[t] += block_smp;
         s += blocksize;
      }
   }
   while (--i);

   no_samples -= blocks*block_smp;
   if (no_samples)
   {
      int t;
      for (t=0; t<tracks; t++)
      {
         _sw_bufcpy_ima_adpcm(d[t], s, no_samples);
         s += blocksize;
      }
   }
}

_aaxRingBuffer*
_bufSetDataInterleaved(_buffer_t *buf, _aaxRingBuffer *rb, const void *dbuf, unsigned blocksize)
{
   unsigned int rb_format, bps, no_samples, no_tracks, tracksize;
   _aaxRingBuffer *rv = rb;
   const void* data;
   int32_t **tracks;

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(rb != 0);
   assert(dbuf != 0);

   data = dbuf;

   rb->set_state(rb, RB_CLEARED);

   rb_format = rb->get_parami(rb, RB_FORMAT);
   bps = rb->get_parami(rb, RB_BYTES_SAMPLE);
   no_samples = rb->get_parami(rb, RB_NO_SAMPLES);
   no_tracks = rb->get_parami(rb, RB_NO_TRACKS);
   tracksize = no_samples * bps;

   switch (rb_format)
   {
   case AAX_IMA4_ADPCM:
      tracks = (int32_t**)rb->get_tracks_ptr(rb, RB_WRITE);
      _aaxRingBufferIMA4ToPCM16(tracks, data, no_tracks, blocksize, no_samples);
      rb->release_tracks_ptr(rb);
      if (buf->to_mixer) rv = _bufConvertDataToMixerFormat(buf, rb);
      break;
   case AAX_PCM32S:
      tracks = (int32_t**)rb->get_tracks_ptr(rb, RB_WRITE);
      _batch_cvt24_32_intl(tracks, data, 0, no_tracks, no_samples);
      rb->release_tracks_ptr(rb);
      break; 
   case AAX_FLOAT:
      tracks = (int32_t**)rb->get_tracks_ptr(rb, RB_WRITE);
      _batch_cvt24_ps_intl(tracks, data, 0, no_tracks, no_samples);
      rb->release_tracks_ptr(rb);
      break;
   case AAX_DOUBLE:
      tracks = (int32_t**)rb->get_tracks_ptr(rb, RB_WRITE);
      _batch_cvt24_pd_intl(tracks, data, 0, no_tracks, no_samples);
      rb->release_tracks_ptr(rb);
      break;
   default:
      tracks = (int32_t**)rb->get_tracks_ptr(rb, RB_WRITE);
      if (no_tracks == 1) {
         _aax_memcpy(tracks[0], data, tracksize);
      }
      else /* stereo */
      {
         unsigned int frame_size = no_tracks * bps;
         unsigned int t;

         for (t=0; t<no_tracks; t++)
         {
            char *sptr, *dptr;
            unsigned int i;

            sptr = (char *)data + t*bps;
            dptr = (char *)tracks[t];
            i = no_samples;
            do
            {
               memcpy(dptr, sptr, bps);
               sptr += frame_size;
               dptr += bps;
            }
            while (--i);
         }
      }
      rb->release_tracks_ptr(rb);
      if (buf->to_mixer) rv = _bufConvertDataToMixerFormat(buf, rb);
   } /* switch */

   return rv;
}


void
_bufGetDataInterleaved(_aaxRingBuffer *rb, void* data, unsigned int samples, unsigned int channels, float fact)
{
   unsigned int fmt, bps, no_samples, t, no_tracks;
   void **ptr, **tracks;

   _AAX_LOG(LOG_DEBUG, __func__);

   assert(rb != 0);
   assert(data != 0);

   fmt = rb->get_parami(rb, RB_FORMAT);
   bps = rb->get_parami(rb, RB_BYTES_SAMPLE);
   no_samples = rb->get_parami(rb, RB_NO_SAMPLES);
   no_tracks = rb->get_parami(rb, RB_NO_TRACKS);
   if (no_tracks > channels) no_tracks = channels;

   assert(samples >= (unsigned int)(fact*no_samples));

   // do not alter the data, we convert to and from float24 when required
   tracks = (void**)rb->get_tracks_ptr(rb, RB_NONE);

   fact = 1.0f/fact;
   ptr = (void**)tracks;
   if (fact != 1.0f)
   {
      unsigned int size = samples*bps;
      char *p;
      
      if (fmt == AAX_PCM24S)
      {
         p = (char*)(no_tracks*sizeof(void*));
         tracks = (void**)_aax_malloc(&p, no_tracks*(sizeof(void*) + size));
         for (t=0; t<no_tracks; t++)
         {
            tracks[t] = p;
#if RB_FLOAT_DATA
            if (rb->get_parami(rb, RB_IS_MIXER_BUFFER) == AAX_FALSE) {
               _batch_cvtps24_24(ptr[t], ptr[t], no_samples);
            }
            _batch_resample_float(tracks[t], ptr[t], 0, samples, 0, fact);
            _batch_cvt24_ps24(tracks[t], tracks[t], samples);
#else
            _batch_resample(tracks[t], ptr[t], 0, samples, 0, fact);
#endif
            p += size;
         }
      }
      else
      {
         size_t scratch_size, len;
         MIX_T **scratch;
         char *sptr;

         scratch_size = 2*sizeof(MIX_T*);
         sptr = (char*)scratch_size;
        
         scratch_size += SIZE_ALIGNED(samples*sizeof(MIX_T));
         len = SIZE_ALIGNED(no_samples*sizeof(MIX_T));
         scratch_size += len;

         scratch = (MIX_T**)_aax_malloc(&sptr, scratch_size);
         scratch[0] = (MIX_T*)sptr;
         scratch[1] = (MIX_T*)(sptr + len);

         p = (char*)(no_tracks*sizeof(void*));
         tracks = (void**)_aax_malloc(&p, no_tracks*(sizeof(void*) + size));
         for (t=0; t<no_tracks; t++)
         {
            tracks[t] = p;
            _bufConvertDataToPCM24S(scratch[0], ptr[t], no_samples, fmt);

#if RB_FLOAT_DATA
            _batch_cvtps24_24(scratch[0], scratch[0], no_samples);
            _batch_resample_float(scratch[1], scratch[0], 0, samples, 0, fact);
            _batch_cvt24_ps24(scratch[1], scratch[1], samples);
#else
            _batch_resample(scratch[1], scratch[0], 0, samples, 0, fact);
#endif
            _bufConvertDataFromPCM24S(tracks[t], scratch[1], 1, samples, fmt,1);
            p += size;
         }
         free(scratch);
      }
   }

   if (no_tracks == 1) {
      _aax_memcpy(data, tracks[0], samples*bps);
   }
   else
   {
      for (t=0; t<no_tracks; t++)
      {
         uint8_t *s = (uint8_t*)tracks[t];
         uint8_t *d = (uint8_t *)data + t*bps;
         unsigned int i =  samples;
         do
         {
            memcpy(d, s, bps);
            d += no_tracks*bps;
            s += bps;
         }
         while (--i);
      }
   }
   rb->release_tracks_ptr(rb);

   if (ptr != tracks) free(tracks);
}
