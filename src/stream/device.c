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

#ifdef HAVE_RMALLOC_H
# include <rmalloc.h>
#else
# include <stdlib.h>
# include <string.h>
# if HAVE_STRINGS_H
#  include <strings.h>
# endif
#endif
#ifdef HAVE_IO_H
#include <io.h>
#endif
#include <ctype.h>		/* toupper */
#include <errno.h>		/* for ETIMEDOUT, errno */
#include <fcntl.h>		/* SEEK_*, O_* */
#include <assert.h>		/* assert */

#include <xml.h>

#include <base/types.h>
#include <base/threads.h>
#include <base/logging.h>

#include <api.h>
#include <arch.h>

#include <software/renderer.h>
#include "device.h"
#include "audio.h"

#define BACKEND_NAME_OLD	"File"
#define BACKEND_NAME		"Audio Files"
#define DEFAULT_RENDERER	AAX_NAME_STR""

#define DEFAULT_OUTPUT_RATE	22050
#define WAVE_HEADER_SIZE	11
#define WAVE_EXT_HEADER_SIZE	17
#ifdef WIN32
# define USE_CAPTURE_THREAD	AAX_FALSE
#else
# define USE_CAPTURE_THREAD	AAX_TRUE
#endif


static _aaxDriverDetect _aaxStreamDriverDetect;
static _aaxDriverNewHandle _aaxStreamDriverNewHandle;
static _aaxDriverGetDevices _aaxStreamDriverGetDevices;
static _aaxDriverGetInterfaces _aaxStreamDriverGetInterfaces;
static _aaxDriverConnect _aaxStreamDriverConnect;
static _aaxDriverDisconnect _aaxStreamDriverDisconnect;
static _aaxDriverSetup _aaxStreamDriverSetup;
static _aaxDriverCaptureCallback _aaxStreamDriverCapture;
static _aaxDriverPlaybackCallback _aaxStreamDriverPlayback;
static _aaxDriverSetPosition _aaxStreamDriverSetPosition;
static _aaxDriverGetName _aaxStreamDriverGetName;
static _aaxDriverRender _aaxStreamDriverRender;
static _aaxDriverState _aaxStreamDriverState;
static _aaxDriverParam _aaxStreamDriverParam;

static char _file_default_renderer[MAX_ID_STRLEN] = DEFAULT_RENDERER;
const _aaxDriverBackend _aaxStreamDriverBackend =
{
   AAX_VERSION_STR,
   DEFAULT_RENDERER,
   AAX_VENDOR_STR,
   (char *)&_file_default_renderer,

   (_aaxDriverRingBufferCreate *)&_aaxRingBufferCreate,
   (_aaxDriverRingBufferDestroy *)&_aaxRingBufferFree,

   (_aaxDriverDetect *)&_aaxStreamDriverDetect,
   (_aaxDriverNewHandle *)&_aaxStreamDriverNewHandle,
   (_aaxDriverGetDevices *)&_aaxStreamDriverGetDevices,
   (_aaxDriverGetInterfaces *)&_aaxStreamDriverGetInterfaces,

   (_aaxDriverGetName *)&_aaxStreamDriverGetName,
   (_aaxDriverRender *)&_aaxStreamDriverRender,
   (_aaxDriverThread *)&_aaxSoftwareMixerThread,

   (_aaxDriverConnect *)&_aaxStreamDriverConnect,
   (_aaxDriverDisconnect *)&_aaxStreamDriverDisconnect,
   (_aaxDriverSetup *)&_aaxStreamDriverSetup,
   (_aaxDriverCaptureCallback *)&_aaxStreamDriverCapture,
   (_aaxDriverPlaybackCallback *)&_aaxStreamDriverPlayback,

   (_aaxDriverPrepare3d *)&_aaxSoftwareDriver3dPrepare,
   (_aaxDriverPostProcess *)&_aaxSoftwareMixerPostProcess,
   (_aaxDriverPrepare *)&_aaxSoftwareMixerApplyEffects,
   (_aaxDriverSetPosition *)&_aaxStreamDriverSetPosition,

   (_aaxDriverState *)&_aaxStreamDriverState,
   (_aaxDriverParam *)&_aaxStreamDriverParam,
   (_aaxDriverLog *)&_aaxStreamDriverLog
};

typedef struct
{
   void *handle;
   char *name;

   char copy_to_buffer; // true if Capture has to copy the data unmodified
   char start_with_fill;
   char end_of_file;

   uint8_t no_channels;
   uint8_t bits_sample;
   enum aaxFormat format;
   int mode;
   float latency;
   float frequency;
   size_t no_samples;
   unsigned int no_bytes;
   float refresh_rate;

   _data_t *dataBuffer;
   size_t dataAvailWrite;

   void *out_header;
   unsigned int out_hdr_size;

   _io_t *io;
   _prot_t *prot;
   _ext_t* ext;

   char *interfaces;
   _aaxRenderer *render;

   struct threat_t thread;
   _aaxMutex *threadbuf_lock;
   _data_t *threadBuffer;

} _driver_t;

static _ext_t* _aaxGetFormat(const char*, enum aaxRenderMode);

#if USE_CAPTURE_THREAD
static void* _aaxStreamDriverReadThread(void*);
#endif
static void* _aaxStreamDriverWriteThread(void*);
static void _aaxStreamDriverWriteChunk(const void*);
static ssize_t _aaxStreamDriverReadChunk(const void*);

static char default_renderer[256];

static int
_aaxStreamDriverDetect(UNUSED(int mode))
{
   snprintf(default_renderer, 255, "%s: %s/AeonWaveOut.wav", BACKEND_NAME, tmpDir());
   return AAX_TRUE;
}

static void *
_aaxStreamDriverNewHandle(enum aaxRenderMode mode)
{
   _driver_t *handle = (_driver_t *)calloc(1, sizeof(_driver_t));
   if (handle)
   {
      handle->mode = mode;
      handle->threadBuffer = _aaxDataCreate(IOBUF_THRESHOLD, 1);
      if (mode == AAX_MODE_READ) {
         handle->dataBuffer = _aaxDataCreate(IOBUF_SIZE, 1);
      }
   }

   return handle;
}

static void *
_aaxStreamDriverConnect(void *config, const void *id, void *xid, const char *device, enum aaxRenderMode mode)
{
   _driver_t *handle = (_driver_t *)id;
   char *renderer = (char *)device;
   char *s = NULL;

   if (!renderer) {
      renderer = default_renderer;
   }

   if (xid || renderer)
   {
      s = default_renderer;
      if (renderer)
      {
         unsigned int devlenold = 5; /* strlen(BACKEND_NAME_OLD":"); */
         unsigned int devlen = 12;   /* strlen(BACKEND_NAME":");     */
         if (!strncasecmp(renderer, BACKEND_NAME":", devlen))
         {
            renderer += devlen;
            while (*renderer == ' ' && *renderer != '\0') renderer++;
         }
         else if (!strncasecmp(renderer, BACKEND_NAME_OLD":", devlenold))
         {
            renderer += devlenold;
            while (*renderer == ' ' && *renderer != '\0') renderer++;
         }

         if (strcasecmp(renderer, "default")) {
            s = _aax_strdup(renderer);
         }
      }
      else if (xid)
      {
         char *ptr = xmlAttributeGetString(xid, "name");
         if (ptr)
         {
            s = _aax_strdup(ptr);
            xmlFree(ptr);
         }
      }

      if (s && (*s == '~'))
      {
         const char *home = userHomeDir();
         if (home)
         {
            size_t hlen = strlen(home);
            size_t slen = strlen(s);
            char *ptr;

            ptr = realloc(s, slen+hlen);
            if (ptr)
            {
               s = ptr+hlen-1;
               memmove(s, ptr, slen+1);
               memcpy(ptr, home, hlen);
               s = ptr;
            }
         }
      }
   }

   if (!handle) {
      id = handle = _aaxStreamDriverNewHandle(mode);
   }

   if (handle)
   {
      handle->handle = config;
      handle->name = s;

      snprintf(_file_default_renderer, MAX_ID_STRLEN, "%s",DEFAULT_RENDERER);

      if (xid)
      {
         float f;
         int i;

         f = (float)xmlNodeGetDouble(xid, "frequency-hz");
         if (f)
         {
            if (f < (float)_AAX_MIN_MIXER_FREQUENCY)
            {
               _AAX_FILEDRVLOG("File: frequency too small.");
               f = (float)_AAX_MIN_MIXER_FREQUENCY;
            }
            else if (f > _AAX_MAX_MIXER_FREQUENCY)
            {
               _AAX_FILEDRVLOG("File: frequency too large.");
               f = (float)_AAX_MAX_MIXER_FREQUENCY;
            }
            handle->frequency = f;
         }

         i = xmlNodeGetInt(xid, "channels");
         if (i)
         {
            if (i < 1)
            {
               _AAX_FILEDRVLOG("File: no. tracks too small.");
               i = 1;
            }
            else if (i > _AAX_MAX_SPEAKERS)
            {
               _AAX_FILEDRVLOG("File: no. tracks too great.");
               i = _AAX_MAX_SPEAKERS;
            }
            handle->no_channels = i;
         }

         i = xmlNodeGetInt(xid, "bits-per-sample");
         if (i)
         {
            if (i != 16)
            {
               _AAX_FILEDRVLOG("File: unsopported bits-per-sample");
               i = 16;
            }
            handle->bits_sample = i;
         }

         if (xmlNodeGetBool(xid, COPY_TO_BUFFER)) {
            handle->copy_to_buffer = 1;
         }
      }
   }

   return handle;
}

static int
_aaxStreamDriverDisconnect(void *id)
{
   _driver_t *handle = (_driver_t *)id;
   int ret = AAX_TRUE;

   if (handle)
   {
      size_t offs, size;
      void *buf = NULL;

      if (handle->thread.started)
      {
         handle->thread.started = AAX_FALSE;

         _aaxSignalTrigger(&handle->thread.signal);
         _aaxThreadJoin(handle->thread.ptr);
      }
      _aaxSignalFree(&handle->thread.signal);
      _aaxMutexDestroy(handle->threadbuf_lock);

      if (handle->thread.ptr) {
         _aaxThreadDestroy(handle->thread.ptr);
      }

      _aaxDataDestroy(handle->dataBuffer);
      if (handle->name && handle->name != default_renderer) {
         free(handle->name);
      }

      if (handle->ext)
      {
         if (handle->ext->update) {
            buf = handle->ext->update(handle->ext, &offs, &size, AAX_TRUE);
         }
         if (buf && (handle && handle->io))
         {
            if (handle->io->protocol == PROTOCOL_DIRECT)
            {
               handle->io->set_param(handle->io, __F_POSITION, 0L);
               ret = handle->io->write(handle->io, buf, size);
            }
         }
         handle->ext->close(handle->ext);
         handle->ext = _ext_free(handle->ext);
      }
      if (handle->io)
      {
         handle->io->close(handle->io);
         handle->io = _io_free(handle->io);
      }
      if (handle->prot) {
         handle->prot = _prot_free(handle->prot);
      }
      if (handle->out_header) {
         free(handle->out_header);
      }

      if (handle->render)
      {
         handle->render->close(handle->render->id);
         free(handle->render);
      }

      _aaxDataDestroy(handle->threadBuffer);
      if (handle->interfaces) {
         free(handle->interfaces);
      }
      free(handle);
   }

   return ret;
}

static int
_aaxStreamDriverSetup(const void *id, float *refresh_rate, int *fmt,
                    unsigned int *tracks, float *speed, int *bitrate,
                    int registered, float period_rate)
{
   _driver_t *handle = (_driver_t *)id;
   char m = (handle->mode == AAX_MODE_READ) ? 0 : 1;
   char *s, *protname, *server, *path, *extension;
   int res, port, rate, size, rv = AAX_FALSE;
   _protocol_t protocol;
   size_t headerSize;
   float period_ms;

   assert(handle);

   handle->format = *fmt;
   handle->bits_sample = aaxGetBitsPerSample(*fmt);
   handle->frequency = *speed;

   rate = *speed;
   size = *tracks * rate * handle->bits_sample / *refresh_rate;

   period_ms = 1000.0f / period_rate;
   if (period_ms < 4.0f) period_ms = 4.0f;
   period_rate = 1000.0f / period_ms;

   s = strdup(handle->name);
   protocol = _url_split(s, &protname, &server, &path, &extension, &port);
#if 0
 printf("\nname: '%s'\n", handle->name);
 printf("protocol: '%s'\n", protname);
 printf("server: '%s'\n", server);
 printf("path: '%s'\n", path);
 printf("ext: '%s'\n", extension);
 printf("port: %i\n", port);
 printf("timeout period: %4.1f ms\n", period_ms);
 printf("refresh rate: %f\n", *refresh_rate);
 printf("buffer size: %i bytes\n\n", size);
#endif

   handle->io = _io_create(protocol);
   if (!handle->io)
   {
      _aaxStreamDriverLog(id, 0, 0, "Unable to create the protocol");
      return rv;
   }

   handle->io->set_param(handle->io, __F_FLAGS, handle->mode);

   res = AAX_FALSE;
   if (1) // !m)
   {
      switch (protocol)
      {
      case PROTOCOL_HTTP:
         handle->io->set_param(handle->io, __F_NO_BYTES, size);
         handle->io->set_param(handle->io, __F_RATE, *refresh_rate);
         handle->io->set_param(handle->io, __F_PORT, port);
         handle->io->set_param(handle->io, __F_TIMEOUT, (int)period_ms);
         if (handle->io->open(handle->io, server) >= 0)
         {   
            handle->prot = _prot_create(protocol);
            if (handle->prot)
            {
               const char *agent = aaxGetVersionString((aaxConfig)id);
               int rv, num = 10;
               do {
                  rv = handle->prot->connect(handle->prot, handle->io,
                                              server, path, agent);
               } while (rv < 0 && --num);

               if (rv < 0)
               {
                  _aaxStreamDriverLog(id, 0, 0, "Unable to open connection");
                  handle->prot = _prot_free(handle->prot);
                  handle->io->close(handle->io);
                  handle->io = _io_free(handle->io);
               }
               else
               {
                  int fmt = handle->prot->get_param(handle->prot,__F_EXTENSION);
                  if (fmt)
                  {
                     handle->ext = _ext_free(handle->ext);
                     handle->ext = _ext_create(fmt);
                     if (handle->ext)
                     {
                        handle->no_bytes = res;
                        res = AAX_TRUE;
                     }
                  }
                  else if (!handle->ext)
                  {
                     _aaxStreamDriverLog(id, 0, 0, "Unsupported file extension");
                     handle->prot = _prot_free(handle->prot);
                     handle->io->close(handle->io);
                     handle->io = _io_free(handle->io);
                  }
               }
            }
            else {
               _aaxStreamDriverLog(id, 0, 0, "Unknow protocol");
            }
         }
         else {
            _aaxStreamDriverLog(id, 0, 0, "Connection failed");
         }
         break;
      case PROTOCOL_DIRECT:
         handle->io->set_param(handle->io, __F_FLAGS, handle->mode);
         if (handle->io->open(handle->io, path) >= 0)
         {
            handle->ext = _ext_free(handle->ext);
            handle->ext = _aaxGetFormat(handle->name, handle->mode);
            if (handle->ext)
            {
               handle->no_bytes= handle->io->get_param(handle->io,__F_NO_BYTES);
               res = AAX_TRUE;
            }
         }
         else
         {
            if (handle->mode != AAX_MODE_READ) {
               _aaxStreamDriverLog(id, 0, 0, "File already exists");
            } else {
               _aaxStreamDriverLog(id, 0, 0, "File read error");
            }
            handle->io->close(handle->io);
            handle->io = _io_free(handle->io);
         }
         break;
      default:
         _aaxStreamDriverLog(id, 0, 0, "Unknown protocol");
         break;
      }
   }

   if (res)
   {
      int format = _FMT_NONE;
      size_t period_frames;

      if (handle->prot) {
         format = handle->prot->get_param(handle->prot, __F_FMT);
      }
      if (format == _FMT_NONE) {
         format = handle->ext->supported(extension);
      }

      period_frames = (size_t)rintf(rate / period_rate);
      res = handle->ext->setup(handle->ext, handle->mode, &headerSize, rate,
                               *tracks, format, period_frames, *bitrate);
      handle->ext->set_param(handle->ext,__F_COPY_DATA, handle->copy_to_buffer);
      if (handle->io->protocol == PROTOCOL_HTTP) {
         handle->ext->set_param(handle->ext, __F_IS_STREAM, 1);
      }

      if (res && ((handle->io->fds.fd >= 0) || m))
      {
         size_t no_samples = period_frames;
         void *header = NULL;
         void *buf = NULL;

         if (headerSize) {
            header = malloc(headerSize);
         }

         do
         {
            if (!m && header && headerSize)
            {
               int tries = 50; /* 50 miliseconds */
               do
               {
                  res = handle->io->read(handle->io, header, headerSize);
                  if (res > 0 || --tries == 0) break;
                  msecSleep(1);
               }
               while (res == 0);

               if (!res)
               {
                   _aaxStreamDriverLog(id, 0, 0, "Unexpected end-of-file");
                   return AAX_FALSE;
               }
               else if (res < 0)
               {
                  _aaxStreamDriverLog(id, 0, 0, "Timeout");
                  break;
               }

               if (handle->prot)
               {
                  int r;

                  r = handle->prot->process(handle->prot, header, res, res);
                  res -= r;
               }
            }

            headerSize = res;
            buf = handle->ext->open(handle->ext, header, &headerSize,
                                    handle->no_bytes);
            if (m && buf)
            {
               handle->out_header = malloc(headerSize);
               memcpy(handle->out_header, buf, headerSize);
               handle->out_hdr_size = headerSize;
               res = headerSize;
               buf = NULL;
            }
            else {
               res = headerSize;
            }
         }
         while (handle->mode == AAX_MODE_READ && buf && headerSize);

         if (header) {
            free(header);
         }

         if (headerSize && res == (int)headerSize)
         {
            rate = handle->ext->get_param(handle->ext, __F_FREQUENCY);

            handle->frequency = (float)rate;
            handle->format = handle->ext->get_param(handle->ext, __F_FMT);
            handle->no_channels = handle->ext->get_param(handle->ext, __F_TRACKS);

            *fmt = handle->format;
            *speed = handle->frequency;
            *tracks = handle->no_channels;
            *refresh_rate = period_rate;

            handle->refresh_rate = period_rate;
            handle->no_samples = no_samples;
            handle->latency = (float)_MAX(no_samples,(PERIOD_SIZE*8/(handle->no_channels*handle->bits_sample))) / (float)handle->frequency;

            if (handle->mode == AAX_MODE_READ) {
#if USE_CAPTURE_THREAD
               handle->thread.ptr = _aaxThreadCreate();
               handle->thread.signal.mutex = _aaxMutexCreate(handle->thread.signal.mutex);
               _aaxSignalInit(&handle->thread.signal);
               handle->threadbuf_lock = _aaxMutexCreate(handle->threadbuf_lock);
               res = _aaxThreadStart(handle->thread.ptr,
                                     _aaxStreamDriverReadThread, handle, 20);
#else
               res = 0;
#endif
            } else {
               handle->thread.ptr = _aaxThreadCreate();
               handle->thread.signal.mutex = _aaxMutexCreate(handle->thread.signal.mutex);
               _aaxSignalInit(&handle->thread.signal);
               res = _aaxThreadStart(handle->thread.ptr,
                                     _aaxStreamDriverWriteThread, handle, 20);
            }

            if (res == 0)
            {
#if USE_CAPTURE_THREAD
               handle->thread.started = AAX_TRUE;
#endif
               handle->render = _aaxSoftwareInitRenderer(handle->latency,
                                                      handle->mode, registered);
               if (handle->render)
               {
                  const char *rstr = handle->render->info(handle->render->id);
                  snprintf(_file_default_renderer, MAX_ID_STRLEN, "%s %s",
                                                   DEFAULT_RENDERER, rstr);
               }
               rv = AAX_TRUE;
            }
            else {
               _aaxStreamDriverLog(id, 0, 0, "Internal error: thread failed");
            }
         }
         else if (headerSize) {
            _aaxStreamDriverLog(id, 0, 0, "Incorrect header");
         }
         else
         {
            char *l = strdup(_aaxStreamDriverLog(NULL, 0, 0, NULL));
            _aaxStreamDriverLog(id, 0, 0, l);
            free(l);
         }

         if (!rv)
         {
            handle->ext = _ext_free(handle->ext);
            if (handle->prot) {
               handle->prot = _prot_free(handle->prot);
            }
            handle->io->close(handle->io);
            handle->io = _io_free(handle->io);
         }
      }
      free(s);
   }
   else {
//    _aaxStreamDriverLog(id, 0, 0, "Unable to intialize the file format");
   }

   return rv;
}

static size_t
_aaxStreamDriverPlayback(const void *id, void *src, UNUSED(float pitch), float gain, UNUSED(char batched))
{
   _driver_t *handle = (_driver_t *)id;  
   _aaxRingBuffer *rb = (_aaxRingBuffer *)src;
   size_t no_samples, offs, outbuf_size, scratchsize;
   unsigned int rb_bps, file_bps, file_tracks;
   unsigned char *scratch, *databuf;
   int32_t** sbuf;
   ssize_t res;

   assert(rb);
   assert(id != 0);

   /*
    * Opening the  file for playback is delayed until actual playback to
    * prevent setting the file size to zero just bij running aaxinfo -d
    * which always opens a file in playback mode.
    */
   if (handle->io->fds.fd < 0) {
      handle->io->open(handle->io, handle->name);
   }

   if (handle->out_header)
   {
      if (handle->io->fds.fd >= 0)
      {
         res = handle->io->write(handle->io, handle->out_header,
                                            handle->out_hdr_size);
         if ((size_t)res == handle->out_hdr_size)
         {
            free(handle->out_header);
            handle->out_header = NULL;
         }
         else {
            return -1;
         }
      }
      else {
         return -1;
      }
   }

   offs = rb->get_parami(rb, RB_OFFSET_SAMPLES);
   no_samples = rb->get_parami(rb, RB_NO_SAMPLES) - offs;
   rb_bps = rb->get_parami(rb, RB_BYTES_SAMPLE);

   file_bps = handle->ext->get_param(handle->ext, __F_BITS_PER_SAMPLE)/8;
   file_tracks = handle->ext->get_param(handle->ext, __F_TRACKS);
   assert(file_tracks == handle->no_channels);

   _aaxMutexLock(handle->thread.signal.mutex);

   outbuf_size = file_tracks * no_samples*_MAX(file_bps, rb_bps);
   if ((handle->dataBuffer == 0) || (handle->dataBuffer->size < 2*outbuf_size))
   {
      _aaxDataDestroy(handle->dataBuffer);
      handle->dataBuffer = _aaxDataCreate(2*outbuf_size, 1);
      if (handle->dataBuffer == 0) return -1;
   }
   databuf = handle->dataBuffer->data;
   scratch = databuf + outbuf_size;
   scratchsize = handle->dataBuffer->size/2;

   assert(outbuf_size <= handle->dataBuffer->size);

   // NOTE: Need RB_RW in case it is used as a slaved file-backend
   //       See _aaxSoftwareMixerPlay

   sbuf = (int32_t**)rb->get_tracks_ptr(rb, RB_RW);
   if (fabsf(gain - 1.0f) > LEVEL_32DB)
   {
      unsigned int t;
      for (t=0; t<file_tracks; t++) {
         _batch_imul_value(sbuf[t]+offs, sizeof(int32_t), no_samples, gain);
      }
   }
   res = handle->ext->cvt_to_intl(handle->ext, databuf, (const int32_t**)sbuf,
                                  offs, &no_samples, scratch, scratchsize);
   rb->release_tracks_ptr(rb);
   handle->dataAvailWrite = res;

   _aaxMutexUnLock(handle->thread.signal.mutex);

   if (batched) {
      _aaxStreamDriverWriteChunk(id);
   } else {
      _aaxSignalTrigger(&handle->thread.signal);
   }

   return (res >= 0) ? (res-res) : -1; // (res - no_samples);
}

static ssize_t
_aaxStreamDriverCapture(const void *id, void **tracks, ssize_t *offset, size_t *frames, UNUSED(void *scratch), UNUSED(size_t scratchSize), float gain, char batched)
{
   _driver_t *handle = (_driver_t *)id;
   ssize_t offs = *offset;
   ssize_t bytes = 0;
   int num = 3;

   assert(*frames);

   *offset = 0;
   if (handle->io->fds.fd >= 0 && frames && tracks)
   {
      int32_t **sbuf = (int32_t**)tracks;
      int file_tracks = handle->ext->get_param(handle->ext, __F_TRACKS);
      unsigned char *data;
      ssize_t res, no_samples;
      size_t samples;

      no_samples = (ssize_t)*frames;
      *frames = 0;

      data = NULL;
      bytes = 0;
      samples = no_samples;
      res = __F_NEED_MORE;	// for handle->start_with_fill == AAX_TRUE
      do
      {
         // handle->start_with_fill == AAX_TRUE if the previous session was
         // a call to  handle->ext->fill() and it returned __F_NEED_MORE
         if (!handle->start_with_fill)
         {
            if (!data)
            {
               // copy or convert data from ext's internal buffer to tracks[]
               // this allows ext or fmt to convert it to a supported format.
               if (handle->copy_to_buffer)
               {
                  do
                  {
                     res = handle->ext->copy(handle->ext, sbuf[0], offs, &samples);
                     offs += samples;
                     no_samples -= samples;
                     *frames += samples;
                     if (res > 0) bytes += res;
                  }
                  while (res > 0);
               }
               else
               {
                  res = handle->ext->cvt_from_intl(handle->ext, sbuf, offs, &samples);
                  offs += samples;
                  no_samples -= samples;
                  *frames += samples;
                  if (res > 0) bytes += res;
               }
            }
            else	/* convert data still in the buffer */
            {
               size_t avail = handle->dataBuffer->avail;
               res = handle->ext->fill(handle->ext, data, &avail);
               _aaxDataMove(handle->dataBuffer, NULL, avail);
            }
         } /* handle->start_with_fill */
         handle->start_with_fill = AAX_FALSE;

         if (res == __F_EOF) {
            handle->end_of_file = AAX_TRUE;
            bytes = __F_EOF;
            break;
         }

         if (handle->end_of_file && res == __F_NEED_MORE)
         {
            handle->start_with_fill = AAX_TRUE;
            bytes = __F_EOF;
            break;
         }

         /* res holds the number of bytes that are actually converted */
         /* or (-3) __F_PROCESS if the next chunk can be processed         */
         /* or (-2) __F_NEED_MORE if fmt->fill requires more data          */
         /* or (-1) __F_EOF if an error occured, or end of file            */
         if (res == __F_PROCESS)
         {
            data = NULL;
            samples = no_samples;
         }
         else if (res == __F_NEED_MORE || no_samples > 0)
         {
            ssize_t avail;

            if (batched) {
               _aaxStreamDriverReadChunk(id);
            }
            else {
#if USE_CAPTURE_THREAD
//             _aaxSignalTrigger(&handle->thread.signal);
#else
               _aaxStreamDriverReadChunk(id);
#endif
            }

#if USE_CAPTURE_THREAD
            _aaxMutexLock(handle->threadbuf_lock);
#endif
            avail = handle->threadBuffer->avail;
            if (avail > 0) {
               _aaxDataMoveData(handle->threadBuffer, handle->dataBuffer,avail);
            }
#if USE_CAPTURE_THREAD
            _aaxMutexUnLock(handle->threadbuf_lock);
#endif

            data = handle->dataBuffer->data; // needed above
         }
      }
      while (no_samples > 0 && --num);

      handle->frequency = (float)handle->ext->get_param(handle->ext, __F_FREQUENCY);

      if (!handle->copy_to_buffer && bytes > 0)
      {
         /* gain is netagive for auto-gain mode */
         gain = fabsf(gain);
         if (fabsf(gain - 1.0f) > 0.05f)
         {
            int t;
            for (t=0; t<file_tracks; t++) {
               _batch_imul_value(sbuf[t] + *offset, sizeof(int32_t), *frames, gain);
            }
         }
      }
      *offset = _MINMAX(IOBUF_THRESHOLD-(ssize_t)handle->threadBuffer->avail, -1, 1);
   }

   return bytes;
}

static char *
_aaxStreamDriverGetName(const void *id, int type)
{
   _driver_t *handle = (_driver_t *)id;
   char *ret = NULL;

   if (handle)
   {
      assert ((int)AAX_MUSIC_PERFORMER_STRING > (int)AAX_MODE_WRITE_MAX);

      switch (type)
      {
      case AAX_MODE_READ:
      case AAX_MODE_WRITE_STEREO:
      case AAX_MODE_WRITE_SPATIAL:
      case AAX_MODE_WRITE_SURROUND:
      case AAX_MODE_WRITE_HRTF:
         if (handle->name) {
            ret = _aax_strdup(handle->name);
         } else {
            ret = _aax_strdup("default");
         }
         break;
      default:
         if (handle->ext)
         {
            switch (type)
            {
            case AAX_MUSIC_PERFORMER_STRING:
               if (handle->prot) {
                  ret = handle->prot->name(handle->prot, __F_ARTIST);
               }
               if (!ret) {
                  ret = handle->ext->name(handle->ext, __F_ARTIST);
               }
               break;
            case AAX_MUSIC_PERFORMER_UPDATE:
               if (handle->prot) {
                  ret = handle->prot->name(handle->prot, __F_ARTIST|__F_NAME_CHANGED);
               }
               if (!ret) {
                  ret = handle->ext->name(handle->ext, __F_ARTIST|__F_NAME_CHANGED);           }
               break;
            case AAX_TRACK_TITLE_STRING:
               if (handle->prot) {
                  ret = handle->prot->name(handle->prot, __F_TITLE);
               }
               if (!ret) {
                  ret = handle->ext->name(handle->ext, __F_TITLE);
               }
               break;
            case AAX_TRACK_TITLE_UPDATE:
               if (handle->prot) {
                  ret = handle->prot->name(handle->prot, __F_TITLE|__F_NAME_CHANGED);
               }
               if (!ret) {
                  ret = handle->ext->name(handle->ext, __F_TITLE|__F_NAME_CHANGED);
               }
               break;
            case AAX_MUSIC_GENRE_STRING:
               if (handle->prot) {
                  ret = handle->prot->name(handle->prot, __F_GENRE);
               }
               if (!ret) {
                  ret = handle->ext->name(handle->ext, __F_GENRE);
               }
               break;
            case AAX_TRACK_NUMBER_STRING:
               if (handle->prot) {
                  ret = handle->prot->name(handle->prot, __F_TRACKNO);
               }
               if (!ret) {
                  ret = handle->ext->name(handle->ext, __F_TRACKNO);
               }
               break;
            case AAX_ALBUM_NAME_STRING:
               if (handle->prot) {
                  ret = handle->prot->name(handle->prot, __F_ALBUM);
               }
               if (!ret) {
                  ret = handle->ext->name(handle->ext, __F_ALBUM);
               }
               break;
            case AAX_RELEASE_DATE_STRING:
               if (handle->prot) {
                  ret = handle->prot->name(handle->prot, __F_DATE);;
               }
               if (!ret) {
                  ret = handle->ext->name(handle->ext, __F_DATE);
               }
               break;
            case AAX_SONG_COMPOSER_STRING:
               if (handle->prot) {
                  ret = handle->prot->name(handle->prot, __F_COMPOSER);
               }
               if (!ret) {
                  ret = handle->ext->name(handle->ext, __F_COMPOSER);
               }
               break;
            case AAX_SONG_COPYRIGHT_STRING:
               if (handle->prot) {
                  ret = handle->prot->name(handle->prot, __F_COPYRIGHT);
               }
               if (!ret) {
                  ret = handle->ext->name(handle->ext, __F_COPYRIGHT);
               }
               break;
            case AAX_SONG_COMMENT_STRING:
               if (handle->prot) {
                  ret = handle->prot->name(handle->prot, __F_COMMENT);
               }
               if (!ret) {
                  ret = handle->ext->name(handle->ext, __F_COMMENT);
               }
               break;
            case AAX_ORIGINAL_PERFORMER_STRING:
               if (handle->prot) {
                  ret = handle->prot->name(handle->prot, __F_ORIGINAL);
               }
               if (!ret) {
                   ret = handle->ext->name(handle->ext, __F_ORIGINAL);
               }
               break;
            case AAX_WEBSITE_STRING:
               if (handle->prot) {
                  ret = handle->prot->name(handle->prot, __F_WEBSITE);
               }
               if (!ret) {
                   ret = handle->ext->name(handle->ext, __F_WEBSITE);
               }
               break;
            case AAX_COVER_IMAGE_DATA:
               ret = handle->ext->name(handle->ext, __F_IMAGE);
               break;
            default:
               break;
            }
         }
      }
   }

   return ret;
}

_aaxRenderer*
_aaxStreamDriverRender(const void* config)
{
   _driver_t *handle = (_driver_t *)config;
   return handle->render;
}


static int
_aaxStreamDriverState(UNUSED(const void *id), enum _aaxDriverState state)
{
   int rv = AAX_FALSE;
   switch(state)
   {
   case DRIVER_AVAILABLE:
   case DRIVER_PAUSE:
   case DRIVER_RESUME:
   case DRIVER_SUPPORTS_PLAYBACK:
   case DRIVER_SUPPORTS_CAPTURE:
      rv = AAX_TRUE;
      break;
   case DRIVER_SHARED_MIXER:
   case DRIVER_NEED_REINIT:
   default:
      break;
   }
   return rv;
}

static float
_aaxStreamDriverParam(const void *id, enum _aaxDriverParam param)
{
   _driver_t *handle = (_driver_t *)id;
   float rv = 0.0f;
   if (handle)
   {
      switch(param)
      {
      /* float */
      case DRIVER_LATENCY:
         rv = handle->latency;
         break;
      case DRIVER_MAX_VOLUME:
         rv = 1.0f;
         break;
      case DRIVER_MIN_VOLUME:
         rv = 0.0f;
         break;
      case DRIVER_VOLUME:
         rv = 1.0f;
         break;
      case DRIVER_FREQUENCY:
         rv = (float)handle->frequency;
         break;

      /* int */
      case DRIVER_MIN_FREQUENCY:
         rv = 8000.0f;
         break;
      case DRIVER_MAX_FREQUENCY:
         rv = _AAX_MAX_MIXER_FREQUENCY;
         break;
      case DRIVER_MIN_TRACKS:
         rv = 1.0f;
         break;
      case DRIVER_MAX_TRACKS:
         rv = (float)_AAX_MAX_SPEAKERS;
         break;
      case DRIVER_BLOCK_SIZE:
         rv = (float)handle->ext->get_param(handle->ext, __F_BLOCK_SIZE);
         break;
      case DRIVER_MIN_PERIODS:
      case DRIVER_MAX_PERIODS:
         rv = 1.0f;
         break;
      case DRIVER_MAX_SAMPLES:
         rv = (float)handle->ext->get_param(handle->ext, __F_NO_SAMPLES);
         break;
      case DRIVER_SAMPLE_DELAY:
         rv = (float)handle->no_samples;
         break;

      /* boolean */
      case DRIVER_SEEKABLE_SUPPORT:
         if (handle->ext->get_param(handle->ext, __F_POSITION) &&
             handle->io->get_param(handle->io, __F_POSITION) != -1)
         {
             rv = (float)AAX_TRUE;
         }
         break;
      case DRIVER_TIMER_MODE:
      case DRIVER_BATCHED_MODE:
         rv = (float)AAX_TRUE;
         break;
      case DRIVER_SHARED_MODE:
      default:
         break;
      }
   }
   return rv;
}

static int
_aaxStreamDriverSetPosition(const void *id, off_t samples)
{
   _driver_t *handle = (_driver_t *)id;
   int rv = 0;

   if (handle->ext)
   {
      off_t bytes = handle->ext->set_param(handle->ext, __F_POSITION, samples);
      rv = handle->io->set_param(handle->io, __F_POSITION, bytes);
   }
   return rv;
}

static char *
_aaxStreamDriverGetDevices(UNUSED(const void *id), int mode)
{
   static const char *rd[2] = { BACKEND_NAME"\0\0", BACKEND_NAME"\0\0" };
   return (char *)rd[mode];
}

static char *
_aaxStreamDriverGetInterfaces(const void *id, UNUSED(const char *devname), int mode)
{
   _driver_t *handle = (_driver_t *)id;
   char *rv = NULL;

   if (handle && !handle->interfaces)
   {
      char interfaces[2048];
      size_t buflen;
      char *ptr;
      int i = 0;

      ptr = interfaces;
      buflen = 2048;

      for (i=0; i<_EXT_MAX; i++)
      {
         _ext_t* ext = _ext_create(i);
         if (ext)
         {
            if (ext->detect(ext, mode))
            {
               char *ifs;
               size_t len;

               ifs = ext->interfaces(i, mode);
               len = ifs ? strlen(ifs) : 0;
               if (ifs && len)
               {
                  snprintf(ptr, buflen, "%s ", ifs);
                  buflen -= len+1;
                  ptr += len+1;
               }
            }
            _ext_free(ext);
         }
      }
      
      if (ptr != interfaces)
      {
         *(ptr-1) = '\0';
         ptr++;
         rv = handle->interfaces = malloc(ptr-interfaces);
         if (rv) {
            memcpy(rv, interfaces, ptr-interfaces);
         }
      }
   }
   else if (handle) {
      rv = handle->interfaces;
   }

   return rv;
}

char *
_aaxStreamDriverLog(const void *id, UNUSED(int prio), UNUSED(int type), const char *str)
{
   _driver_t *handle = (_driver_t *)id;
   static char _errstr[256];

   if (str)
   {
      if (handle && handle->io && handle->io->protocol == PROTOCOL_HTTP) {
          snprintf(_errstr, 256, "HTTP: %s\n", str);
      } else {
         snprintf(_errstr, 256, "Stream: %s\n", str);
      }
      _errstr[255] = '\0';  /* always null terminated */

      if (handle) {
         __aaxDriverErrorSet(handle->handle, AAX_BACKEND_ERROR, (char*)&_errstr);
      }
      _AAX_SYSLOG(_errstr);
   }

   return (char*)&_errstr;
}

/*-------------------------------------------------------------------------- */

static _ext_t*
_aaxGetFormat(const char *url, enum aaxRenderMode mode)
{
   char *s, *protocol, *server, *path, *extension;
   _ext_t *rv = NULL;
   int port, res;

   if (!url) return rv;

   extension = NULL;
   s = strdup(url);
   res = _url_split(s, &protocol, &server, &path, &extension, &port);
   switch (res)
   {
   case PROTOCOL_HTTP:
      if (path) extension = strrchr(path, '.');
      if (!extension) extension = ".mp3";
      break;
   case PROTOCOL_DIRECT:
      if (path) extension = strrchr(path, '.');
      break;
   default:
      break;
   }

   if (extension++)
   {
      int i;
      for (i=0; i<_EXT_MAX; i++)
      {
         rv = _ext_create(i);
         if (rv && rv->detect(rv, mode) && rv->supported(extension)) {
            break;
         }
         rv = _ext_free(rv);
      }
   }
   free(s);

   return rv;
}

static void
_aaxStreamDriverWriteChunk(const void *id)
{
   _driver_t *handle = (_driver_t*)id;
   ssize_t buffer_avail, avail;
   unsigned char *databuf;

   databuf = handle->dataBuffer->data;
   avail = handle->dataAvailWrite;

   buffer_avail = avail;
   if (handle->io)
   {
      do
      {
         size_t usize =_aaxDataAdd(handle->threadBuffer, databuf, buffer_avail);
         buffer_avail -= usize;
         databuf += usize;

         if (handle->threadBuffer->avail >= PERIOD_SIZE)
         {
            unsigned char *tbdata = handle->threadBuffer->data;
            size_t tbavail = handle->threadBuffer->avail;
            size_t wsize = (tbavail/PERIOD_SIZE)*PERIOD_SIZE;
            ssize_t res;

            res = handle->io->write(handle->io, tbdata, wsize);
            if (res > 0)
            {
               _aaxDataMove(handle->threadBuffer, NULL, res);

               if (handle->ext->update)
               {
                  size_t spos = 0;
                  void *buf = handle->ext->update(handle->ext, &spos, &usize,
                                                  AAX_FALSE);
                  if (buf)
                  {
                     off_t floc= handle->io->get_param(handle->io,__F_POSITION);
                     handle->io->set_param(handle->io, __F_POSITION, 0L);
                     res = handle->io->write(handle->io, buf, usize);
                     handle->io->set_param(handle->io, __F_POSITION, floc);
                  }
               }
            }
            else
            {
               _AAX_FILEDRVLOG(strerror(errno));
               break;
            }
         }
         else {
            break;
         }
      }
      while (buffer_avail >= 0);
   }
   handle->dataAvailWrite = 0;

}  

static void*
_aaxStreamDriverWriteThread(void *id)
{
   _driver_t *handle = (_driver_t*)id;

   _aaxMutexLock(handle->thread.signal.mutex);

   _aaxThreadSetPriority(handle->thread.ptr, AAX_LOW_PRIORITY);
   do
   {
      _aaxSignalWait(&handle->thread.signal);

      if TEST_FOR_FALSE(handle->thread.started) {
         break;
      }

      if (!handle->dataAvailWrite) {
         continue;
      }
   
      _aaxStreamDriverWriteChunk(id);
   }
   while(handle->thread.started);

   if (handle->dataAvailWrite) {
      _aaxStreamDriverWriteChunk(id);
   }

   _aaxMutexUnLock(handle->thread.signal.mutex);

   return handle; 
}

static ssize_t
_aaxStreamDriverReadChunk(const void *id)
{
   char buffer[PERIOD_SIZE];
   _driver_t *handle = (_driver_t*)id;
   size_t size, avail;
   ssize_t res;

#if USE_CAPTURE_THREAD
   _aaxMutexLock(handle->threadbuf_lock);
#endif
   avail = handle->threadBuffer->avail;
   size = _MIN(handle->threadBuffer->size - avail, PERIOD_SIZE);
#if USE_CAPTURE_THREAD
   _aaxMutexUnLock(handle->threadbuf_lock);
#endif

   if (!size) {
      return 0;
   }

   // read may block until enough data is available but others will only
   // remove data from the threadBuffer so size may only increase.
   res = handle->io->read(handle->io, buffer, size);
   if (res > 0)
   {
      unsigned char *data;

#if USE_CAPTURE_THREAD
      _aaxMutexLock(handle->threadbuf_lock);
#endif
      data = handle->threadBuffer->data;

      avail = handle->threadBuffer->avail;
      assert(avail+res <= handle->threadBuffer->size);

      memcpy(data+avail, buffer, res);
      avail += res;

      if (handle->prot)
      {
         int slen = handle->prot->process(handle->prot, data, res, avail);
         avail -= slen;
      }

      handle->threadBuffer->avail = avail;
#if USE_CAPTURE_THREAD
      _aaxMutexUnLock(handle->threadbuf_lock);
#endif
   }
   else if (res == -1) {
      handle->end_of_file = AAX_TRUE;
   }

   return res;
}

#if USE_CAPTURE_THREAD
static void*
_aaxStreamDriverReadThread(void *id)
{
   _driver_t *handle = (_driver_t*)id;
   ssize_t res;

   // wait for our first job
   _aaxMutexLock(handle->thread.signal.mutex);

   /* read (clear) all bytes already sent from the server */
   if (handle->io->protocol != PROTOCOL_DIRECT)
   {
      do
      {
         handle->threadBuffer->avail = 0;
         res = _aaxStreamDriverReadChunk(id);
      }
      while (res > IOBUF_THRESHOLD);
   }

   if (!handle->copy_to_buffer) {
      res = _aaxStreamDriverReadChunk(id);
   }

   do
   {
      _aaxSignalWaitTimed(&handle->thread.signal, 1.0f/handle->refresh_rate);
      res = _aaxStreamDriverReadChunk(id);
   }
   while(res >= 0 && handle->thread.started);

   _aaxMutexUnLock(handle->thread.signal.mutex);

   return handle;
}
#endif
