/*
 * Copyright 2007-2017 by Erik Hofman.
 * Copyright 2009-2017 by Adalin B.V.
 *
 * This file is part of AeonWave
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
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

#ifdef HAVE_LIBIO_H
#include <libio.h>		/* for NULL */
#endif
#ifdef HAVE_RMALLOC_H
# include <rmalloc.h>
#else
# include <string.h>
# if HAVE_STRINGS_H
#  include <strings.h>
# endif
#endif
#include <math.h>		/* for INFINITY */
#include <assert.h>
#include <xml.h>

#include <base/gmath.h>
#include <base/logging.h>

#include <dsp/filters.h>
#include <dsp/effects.h>

#include "api.h"
#include "devices.h"
#include "arch.h"
#include "ringbuffer.h"

static _intBuffers* get_backends();
static _handle_t* _open_handle(aaxConfig);
static _aaxConfig* _aaxReadConfig(_handle_t*, const char*, int);
static void _aaxContextSetupHRTF(void *, unsigned int);
static void _aaxContextSetupSpeakers(void **, unsigned char *router, unsigned int);
static void _aaxFreeSensor(void *);

static const char* _aax_default_devname;
static char* _default_renderer = "default";

int __release_mode = AAX_FALSE;
_aaxMixerInfo* _info = NULL;
_intBuffers* _backends = NULL;
time_t _tvnow = 0;

AAX_API enum aaxErrorType AAX_APIENTRY
aaxDriverGetErrorNo(aaxConfig config)
{
   _handle_t *handle = get_handle(config, __func__);
   if (!handle) return __aaxErrorSet(AAX_ERROR_NONE, NULL);
   return __aaxDriverErrorSet(handle, AAX_ERROR_NONE, NULL);
}


AAX_API const char* AAX_APIENTRY
aaxDriverGetSetup(const aaxConfig config, enum aaxSetupType type)
{
   _handle_t *handle = get_handle(config, __func__);
   char *rv = NULL;
   if (handle)
   {
      const _aaxDriverBackend *be = handle->backend.ptr;
      switch(type)
      {
      case AAX_VERSION_STRING:
         rv = (char*)be->version;
         break;
      case AAX_VENDOR_STRING:
         rv = (char*)be->vendor;
         break;
      case AAX_DRIVER_STRING:
         if (handle->backend.driver) {
            rv = (char*)be->renderer;
         } else {
            rv = (char*)be->driver;
         }
         break;
      case AAX_RENDERER_STRING:
         rv = be->name(handle->backend.handle, type);
         if (!rv)
         {
            if (handle->backend.driver) {
               rv = (char*)handle->backend.driver;
            } else {
               rv = (char*)be->driver;
            }
         }
         break;
      case AAX_MUSIC_PERFORMER_STRING:
      case AAX_MUSIC_PERFORMER_UPDATE:
      case AAX_MUSIC_GENRE_STRING:
      case AAX_TRACK_TITLE_STRING:
      case AAX_TRACK_TITLE_UPDATE:
      case AAX_TRACK_NUMBER_STRING:
      case AAX_ALBUM_NAME_STRING:
      case AAX_RELEASE_DATE_STRING:
      case AAX_SONG_COMPOSER_STRING:
      case AAX_SONG_COPYRIGHT_STRING:
      case AAX_SONG_COMMENT_STRING:
      case AAX_ORIGINAL_PERFORMER_STRING:
      case AAX_WEBSITE_STRING:
      case AAX_COVER_IMAGE_DATA:
          rv = be->name(handle->backend.handle, type);
          break;
      default:
         _aaxErrorSet(AAX_INVALID_ENUM);
      }
   }
   return (const char*)rv;
}

AAX_API unsigned AAX_APIENTRY
aaxDriverGetCount(enum aaxRenderMode mode)
{
   unsigned rv = 0;
   if (mode < AAX_MODE_WRITE_MAX)
   {
      _intBuffers* backends = get_backends();
      unsigned count = _intBufGetNumNoLock(backends, _AAX_BACKEND);
      unsigned i, m = (mode == AAX_MODE_READ) ? 0 : 1;
      for (i=0; i<count; i++)
      {
         _aaxDriverBackend *be = _aaxGetDriverBackendByPos(backends, i);
         if (be)
         {
            if (!m && be->state(NULL, DRIVER_SUPPORTS_CAPTURE)) rv++;
            else if (m && be->state(NULL, DRIVER_SUPPORTS_PLAYBACK)) rv++;
         }
      }
   }
   else {
      __aaxErrorSet(AAX_INVALID_ENUM, __func__);
   }
   return rv;
}

AAX_API aaxConfig AAX_APIENTRY
aaxDriverGetByPos(unsigned pos_req, enum aaxRenderMode mode)
{
   _handle_t *handle = NULL;

   if (mode < AAX_MODE_WRITE_MAX)
   {
      handle = new_handle();
      if (handle)
      {
         unsigned count = _intBufGetNumNoLock(handle->backends, _AAX_BACKEND);
         unsigned i,  m = (mode == AAX_MODE_READ) ? 0 : 1;
         _aaxDriverBackend *be = NULL;
         unsigned p = 0;

         for (i=0; i<count; i++)
         {
            be = _aaxGetDriverBackendByPos(handle->backends, i);
            if (be)
            {
               if ((!m && be->state(NULL, DRIVER_SUPPORTS_CAPTURE)) &&
                      (p++ == pos_req)) {
                  break;
               }
               else if ((m && be->state(NULL, DRIVER_SUPPORTS_PLAYBACK)) &&
                           (p++ == pos_req)) {
                  break;
               }
            }
         }
 
         if (be)
         {
            handle->id = HANDLE_ID;
            handle->backend.ptr = be;
            handle->info->mode = mode;
//          handle->devname[0] = be->driver;
            handle->devname[0] = (char *)_aax_default_devname;

         }
         else
         {
            _aaxErrorSet(AAX_INVALID_PARAMETER);
            aaxDriverDestroy(handle);
            handle = NULL;
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_HANDLE);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_ENUM);
   }
   return (aaxConfig)handle;
}

AAX_API aaxConfig AAX_APIENTRY
aaxDriverGetByName(const char* devname, enum aaxRenderMode mode)
{
   _handle_t *handle = NULL;

   if (mode < AAX_MODE_WRITE_MAX)
   {
      handle = new_handle();
      if (handle)
      {
         char *name = (char *)devname;
         handle->info->mode = mode;

         if (!name)
         {
            _aaxConfig *cfg = _aaxReadConfig(handle, NULL, mode);
            if (cfg->node[0].devname)
            {
               name = _aax_strdup(cfg->node[0].devname);

               free(handle->backend.driver);
               handle->backend.driver = _aax_strdup(cfg->node[0].devname);
            }
            _aaxDriverBackendClearConfigSettings(cfg);
         }

         if ((name != NULL) && strcasecmp(name, "default"))
         {
            char *ptr;

            handle->devname[0] = _aax_strdup(name);

            ptr = strstr(handle->devname[0], " on ");
            if (ptr)
            {
               *ptr = 0;
               handle->devname[1] = ptr+4;	/* 4 = strlen(" on ") */

               if (!strcasecmp(handle->devname[0], "Generic Software") ||
                   !strcasecmp(handle->devname[0], "Generic Hardware"))
               {
                  // strlen("WASAPI") is always less than
                  // strlen("Generic Hardware")
                  sprintf(handle->devname[0], "WASAPI");
                  _aaxConnectorDeviceToDeviceConnector(handle->devname[1]);
               }
            }
            handle->backend.ptr = _aaxGetDriverBackendByName(handle->backends,
                                                            handle->devname[0],
                                                            &handle->be_pos);
         }
         else /* name == NULL */
         {
            const _aaxDriverBackend *be;

            if (mode == AAX_MODE_READ) {
               be = _aaxGetDriverBackendDefaultCapture(handle->backends,
                                                       &handle->be_pos);
            } else {
               be = _aaxGetDriverBackendDefault(handle->backends,
                                                &handle->be_pos);
            }

            handle->backend.ptr = be;
            if (be) { /* be == NULL should never happen */
               handle->devname[0] = _aax_strdup(be->driver);
               handle->devname[1] = be->name(handle->backend.handle, mode?1:0);
            }
         }

         if (name != devname)
         {
            _aax_free(name);
             name = NULL;
         }

         if (handle->backend.ptr == NULL)
         {
            _aaxErrorSet(AAX_INVALID_PARAMETER);
            if (handle->devname[0] != _aax_default_devname)
            {
               free(handle->devname[0]);
               handle->devname[0] = (char *)_aax_default_devname;
            }
            aaxDriverDestroy(handle);
            handle = NULL;
         }
      }
      else
      {
         _aaxErrorSet(AAX_INVALID_HANDLE);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_ENUM);
   }
   return handle;
}

AAX_API int AAX_APIENTRY
aaxDriverGetSupport(const aaxConfig config, enum aaxRenderMode mode)
{
   _handle_t *handle = get_handle(config, __func__);
   int rv = AAX_FALSE;

   if (handle)
   {
      const _aaxDriverBackend *be = handle->backend.ptr;

      switch (mode)
      {
      case AAX_MODE_READ:
         rv = be->state(NULL, DRIVER_SUPPORTS_CAPTURE);
         break;
      case AAX_MODE_WRITE_STEREO:
      case AAX_MODE_WRITE_SURROUND:
      case AAX_MODE_WRITE_HRTF:
         rv = be->state(NULL, DRIVER_SUPPORTS_PLAYBACK);
         break;
      default:
         _aaxErrorSet(AAX_INVALID_ENUM);
      }
   } 
   return rv;
}

AAX_API aaxConfig AAX_APIENTRY
aaxDriverOpen(aaxConfig config)
{
   _handle_t *handle = _open_handle(config);
   const char *env = getenv("AAX_RELEASE_MODE");
   if (env) __release_mode = _aax_getbool(env) ? AAX_TRUE : AAX_FALSE;

   if (handle)
   {
      enum aaxRenderMode mode = handle->info->mode;
      _aaxConfig *cfg = _aaxReadConfig(handle, NULL, mode);
      const _aaxDriverBackend *be = handle->backend.ptr;
      void *xoid, *nid = 0;

      if (cfg)
      {
         if (handle->info->mode == AAX_MODE_READ) {
            xoid = cfg->backend.input;
         } else {
            xoid = cfg->backend.output;
         }
         if (be)
         {
            const char* name = handle->devname[1];
            char *renderer;

            handle->backend.handle = be->connect(handle, nid, xoid, name, mode);

            if (handle->backend.driver != _default_renderer) {
               free(handle->backend.driver);
            }
            renderer = be->name(handle->backend.handle, mode?1:0);
            if (!renderer) renderer = _default_renderer;

            handle->backend.driver = malloc(strlen(be->driver)
                                            +strlen(" on ")+strlen(renderer)+1);
            if (handle->backend.driver)
            {
               sprintf(handle->backend.driver, "%s on %s", be->driver,renderer);
               if (renderer != _default_renderer) {
                  free(renderer);
               }
            }
            else {
                handle->backend.driver = renderer;
            }

            if (_info == NULL) {
               _info =  handle->info;
            }

            if (handle->info->mode == AAX_MODE_WRITE_SURROUND)
            {
               _aaxRingBufferFreqFilterData *freqfilter;
               float k, fc = 80.0f;
               _filter_t *filter;

               /* crossover lowpass filter at 80Hz, 4th orer (24dB/oct) */
               filter = aaxFilterCreate(handle, AAX_FREQUENCY_FILTER);
               aaxFilterSetSlot(filter, 0, AAX_LINEAR, fc, 1.0f, 0.0f, 1.0f);
               aaxFilterSetState(filter, AAX_BESSEL|AAX_24DB_OCT);
               _FILTER_SWAP_SLOT_DATA(handle, SURROUND_CROSSOVER_LP, filter, 0);
               aaxFilterDestroy(filter);

               freqfilter = _FILTER_GET_DATA(handle, SURROUND_CROSSOVER_LP);
               k = _aax_movingaverage_compute(fc, freqfilter->fs);
               freqfilter->k = k;
            }
            else if (handle->info->mode == AAX_MODE_WRITE_HRTF)
            {
               int type = HRTF_HEADSHADOW;
               _filter_t *filter;

               /* head shadow filter at 1kHz, 1st order (6dB/oct) max */
               filter = aaxFilterCreate(handle, AAX_FREQUENCY_FILTER);
               aaxFilterSetSlot(filter, 0, AAX_LINEAR, 1000.0f, 1.0f,0.0f,1.0f);
               aaxFilterSetState(filter, AAX_BESSEL|AAX_6DB_OCT);
               _FILTER_SWAP_SLOT_DATA(handle, type, filter, 0);
               aaxFilterDestroy(filter);
            }
         }
         _aaxDriverBackendClearConfigSettings(cfg);
      }
   }

   if (!handle || !handle->backend.handle)
   {
      aaxDriverDestroy(handle);
      handle = NULL;

      _aaxErrorSet(AAX_INVALID_DEVICE);
   }
   return (aaxConfig)handle;
}

AAX_API aaxConfig AAX_APIENTRY
aaxDriverOpenByName(const char* name, enum aaxRenderMode mode)
{
   aaxConfig config = NULL;
   if (mode < AAX_MODE_WRITE_MAX)
   {
      config = aaxDriverGetByName(name, mode);
      if (config) {
         config = aaxDriverOpen(config);
      } else {
         __aaxErrorSet(AAX_INVALID_PARAMETER, __func__);
      }
   }
   else {
      __aaxErrorSet(AAX_INVALID_ENUM, __func__);
   }
   return config;
}

AAX_API aaxConfig AAX_APIENTRY
aaxDriverOpenDefault(enum aaxRenderMode mode)
{
   return aaxDriverOpenByName(NULL, mode);
}

AAX_API int AAX_APIENTRY
aaxDriverDestroy(aaxConfig config)
{
   _handle_t *handle = get_handle(config, __func__);
   int rv = AAX_FALSE;
   aaxEffect effect;

   aaxMixerSetState(handle, AAX_STOPPED);
   aaxSensorSetState(handle, AAX_STOPPED);
   aaxDriverClose(handle);

   // This assigns a new effect without any effect data to the mixer
   // and returns the already assigned effect, possibly with a data
   // section. The data gets destroyed when de returned effect gets
   // destroyed.
   effect = aaxEffectCreate(config, AAX_REVERB_EFFECT);
   aaxMixerSetEffect(config, effect);
   aaxEffectDestroy(effect);

   effect = aaxEffectCreate(config, AAX_CONVOLUTION_EFFECT);
   aaxMixerSetEffect(config, effect);
   aaxEffectDestroy(effect);

   if (handle && !handle->handle)
   {
      assert(handle->backends != NULL);

      _aaxSignalFree(&handle->buffer_ready);

      handle->info->id = FADEDBAD;
      if (_info == handle->info) {
         _info = NULL;
      }

      _intBufErase(&handle->sensors, _AAX_SENSOR, _aaxFreeSensor);

      if (handle->buffer) {
         aaxBufferDestroy(handle->buffer);
      }

      if (handle->devname[0] != _aax_default_devname)
      {
         free(handle->devname[0]);
         handle->devname[0] = (char *)_aax_default_devname;
      }

      if (handle->backend.driver != _default_renderer) {
         free(handle->backend.driver);
      }

      if (handle->ringbuffer) {
         _aaxRingBufferFree(handle->ringbuffer);
      }

      if (handle->timer) {
         _aaxTimerDestroy(handle->timer);
      }

      /* safeguard against using already destroyed handles */
      handle->id = FADEDBAD;
      free(handle);

      _aaxRemoveDriverBackends(&_backends);
      rv = AAX_TRUE;
   }
   else if (handle) {
       _aaxErrorSet(AAX_INVALID_STATE);
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxDriverClose(aaxConfig config)
{
   _handle_t *handle = get_handle(config, __func__);
   int rv = AAX_FALSE;

   if (handle)
   {
      const _aaxDriverBackend *be = handle->backend.ptr;

      if (aaxMixerGetState(config) != AAX_STOPPED) {
         aaxMixerSetState(config, AAX_STOPPED);
      }
      if (be && handle->backend.handle) {
         be->disconnect(handle->backend.handle);
      }
      handle->backend.handle = NULL;
      rv = AAX_TRUE;
   }
   else {
      _aaxErrorSet(AAX_ERROR_NONE);
   }
   return rv;
}

AAX_API unsigned AAX_APIENTRY
aaxDriverGetDeviceCount(const aaxConfig config, enum aaxRenderMode mode)
{
   _handle_t *handle = get_handle(config, __func__);
   unsigned int num = 0;

   if (handle)
   {
      if (mode < AAX_MODE_WRITE_MAX)
      {
         const _aaxDriverBackend *be = handle->backend.ptr;
         void* be_handle = handle->backend.handle;
         char *ptr;

         ptr = be->get_devices(be_handle, mode);
         if (ptr)
         {
            while (*ptr)
            {
               ptr += strlen(ptr)+1;
               num++;
            }
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_ENUM);
      }
   }
   return num;
}

AAX_API const char* AAX_APIENTRY
aaxDriverGetDeviceNameByPos(const aaxConfig config, unsigned pos, enum aaxRenderMode mode)
{
   _handle_t *handle = get_handle(config, __func__);
   char *rv = NULL;

   if (handle)
   {
      if (mode < AAX_MODE_WRITE_MAX)
      {
         const _aaxDriverBackend *be = handle->backend.ptr;
         void* be_handle = handle->backend.handle;
         unsigned int num = 0;
         char *ptr;

         if (handle->be_pos != pos)
         {
            aaxDriverClose(handle);
            handle->be_pos = pos;
            be_handle = NULL;
         }

         if (!be_handle)
         {
            char *renderer;

            be_handle = be->new_handle(mode);
            handle->backend.handle = be_handle;

            if (handle->backend.driver != _default_renderer) {
               free(handle->backend.driver);
            }
            renderer = be->name(handle->backend.handle, mode?1:0);
            handle->backend.driver = renderer ? renderer : _default_renderer;
         }

         ptr = be->get_devices(be_handle, mode);
         if (ptr)
         {
            while(*ptr && (pos != num))
            {
               ptr += strlen(ptr)+1;
               num++;
            }
            if (ptr) {
               rv = ptr;
            }
             else {
               _aaxErrorSet(AAX_INVALID_PARAMETER);
            }
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_ENUM);
      }
   }
   return rv;
}

AAX_API unsigned AAX_APIENTRY
aaxDriverGetInterfaceCount(const aaxConfig config, const char* devname, enum aaxRenderMode mode)
{
   _handle_t *handle = get_handle(config, __func__);
   unsigned int num = 0;

   if (handle)
   {
      if (mode < AAX_MODE_WRITE_MAX)
      {
         const _aaxDriverBackend *be = handle->backend.ptr;
         void* be_handle = handle->backend.handle;
         char *ptr;

         ptr = be->get_interfaces(be_handle, devname, mode);
         if (ptr)
         {
            while(*ptr)
            {
               ptr += strlen(ptr)+1;
               num++;
            }
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_ENUM);
      }
   }
   return num;
}

AAX_API const char* AAX_APIENTRY
aaxDriverGetInterfaceNameByPos(const aaxConfig config, const char* devname, unsigned pos, enum aaxRenderMode mode)
{
   _handle_t *handle = get_handle(config, __func__);
   char *rv = NULL;

   if (handle)
   {
      if (mode < AAX_MODE_WRITE_MAX)
      {
         const _aaxDriverBackend *be = handle->backend.ptr;
         void* be_handle = handle->backend.handle;
         unsigned int num = 0;
         char *ptr;

         ptr = be->get_interfaces(be_handle, devname, mode);
         if (*ptr)
         {
            while(*ptr && (pos != num))
            {
               ptr += strlen(ptr)+1;
               num++;
            }
            if (ptr) {
               rv = ptr;
            } else {
               _aaxErrorSet(AAX_INVALID_PARAMETER);
            }
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_ENUM);
      }
   }
   return rv;
}

/* -------------------------------------------------------------------------- */

static const char* _aax_default_devname = "None";

void
_aaxDriverFree(void *handle)
{
   aaxDriverDestroy(handle);
}

static _intBuffers*
get_backends()
{
   if (_backends == NULL) {
      _backends = _aaxGetDriverBackends();
   }
   return _backends;
}

_handle_t*
new_handle()
{
   unsigned long size;
   _handle_t *rv = NULL;
   void *ptr1;
   char *ptr2;

   size = sizeof(_handle_t);
   ptr2 = (char*)size;
   size += sizeof(_aaxMixerInfo);

   ptr1 = _aax_calloc(&ptr2, 1, size);
   if (ptr1)
   {
      _handle_t* handle = (_handle_t*)ptr1;

      handle->id = HANDLE_ID;
      handle->backends = get_backends();

      handle->mixer_pos = UINT_MAX;
      handle->be_pos = UINT_MAX;
      _SET_PROCESSED(handle);

      handle->info = (_aaxMixerInfo*)ptr2;
      _aaxSetDefaultInfo(handle->info, handle);

      handle->timer = _aaxTimerCreate();

      rv = handle;
   }

   return rv;
}

_handle_t*
get_handle(aaxConfig config, const char *func)
{
   _handle_t *handle = (_handle_t *)config;
   _handle_t *rv = NULL;

   if (handle && handle->id == HANDLE_ID) {
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

_handle_t*
get_valid_handle(aaxConfig config, const char *func)
{
   _handle_t *handle = (_handle_t *)config;
   _handle_t *rv = NULL;

   if (handle)
   {
      if (handle->id == HANDLE_ID)
      {
         if (handle->valid & ~HANDLE_ID) {
            rv = handle;
         }
      }
      else if (handle->id == AUDIOFRAME_ID) {
         rv = handle;
      }
      else if (handle->id == FADEDBAD) {
         __aaxErrorSet(AAX_DESTROYED_HANDLE, func);
      }
   }
   else {
      __aaxErrorSet(AAX_INVALID_HANDLE, func);
   }

   return rv;
}

_handle_t*
get_write_handle(aaxConfig config, const char *func)
{
   _handle_t *handle = (_handle_t *)config;
   _handle_t *rv = NULL;

   if (handle && handle->id == HANDLE_ID)
   {
      assert(handle->info);
      if (handle->info->mode != AAX_MODE_READ) {
         rv = handle;
      }
   }
   else if (handle && handle->id == FADEDBAD) {
      __aaxErrorSet(AAX_DESTROYED_HANDLE, func);
   }
   else {
      __aaxErrorSet(AAX_INVALID_HANDLE, func);
   }

   return rv;
}

_handle_t*
get_read_handle(aaxConfig config, const char *func)
{
   _handle_t *handle = (_handle_t *)config;
   _handle_t *rv = NULL;

   if (handle && handle->id == HANDLE_ID)
   {
      assert(handle->info);
      if (handle->info->mode == AAX_MODE_READ) {
         rv = handle;
      }
   }
   else if (handle && handle->id == FADEDBAD) {
      __aaxErrorSet(AAX_DESTROYED_HANDLE, func);
   }
   else {
      __aaxErrorSet(AAX_INVALID_HANDLE, func);
   }

   return rv;
}

_handle_t *
get_driver_handle(aaxConfig c)
{
   _frame_t* frame = (_frame_t*)c;
   _handle_t* rv = NULL;

   if (frame)
   {
      if (frame->id == HANDLE_ID) {
         rv = (_handle_t*)frame;
      } else if (frame->id == AUDIOFRAME_ID) {
         rv = frame->submix->info->backend;
      } else if (frame->id == EMITTER_ID) {
          rv = get_driver_handle( ((_emitter_t*)c)->handle );
      } else if (frame->id == BUFFER_ID) {
          rv = ((_buffer_t*)c)->handle;
      }
   }
   return rv;
}

static _handle_t*
_open_handle(aaxConfig config)
{
   _handle_t *handle = (_handle_t *)config;

   if (handle && handle->id == HANDLE_ID)
   {
      assert (handle->backend.ptr != NULL);

      if (handle->sensors == NULL)
      {
         unsigned int res = _intBufCreate(&handle->sensors, _AAX_SENSOR);
         if (res != UINT_MAX)
         {
            unsigned long size;
            char* ptr2;
            void* ptr1;

            size = sizeof(_sensor_t) + sizeof(_aaxAudioFrame);
            ptr2 = (char*)size;

            size += sizeof(_aax2dProps);
            ptr1 = _aax_calloc(&ptr2, 1, size);

            if (ptr1)
            {
               _sensor_t* sensor = ptr1;
               _aaxAudioFrame* smixer;

               sensor->filter = handle->filter;
               _aaxSetDefaultEqualizer(handle->filter);

               size = sizeof(_sensor_t);
               smixer = (_aaxAudioFrame*)((char*)sensor + size);

               sensor->mixer = smixer;
               sensor->mixer->info = handle->info;

               assert(((long int)ptr2 & MEMMASK) == 0);

               smixer->props2d = (_aax2dProps*)ptr2;
               _aaxSetDefault2dProps(smixer->props2d);
               _EFFECT_SET2D(smixer,PITCH_EFFECT,AAX_PITCH,handle->info->pitch);

               smixer->props3d = _aax3dPropsCreate();
               if (smixer->props3d)
               {
                  smixer->props3d->dprops3d->velocity.m4[VELOCITY][3] = 0.0f;
                  _EFFECT_SETD3D_DATA(smixer, VELOCITY_EFFECT,
                                      *(void**)&_aaxRingBufferDopplerFn[0]);
                  _FILTER_SETD3D_DATA(smixer, DISTANCE_FILTER,
                                      *(void**)&_aaxRingBufferDistanceFn[AAX_EXPONENTIAL_DISTANCE]);
               }

               res = _intBufCreate(&smixer->emitters_3d, _AAX_EMITTER);
               if (res != UINT_MAX) {
                  res = _intBufCreate(&smixer->emitters_2d, _AAX_EMITTER);
               }
               if (res != UINT_MAX) {
                  res = _intBufCreate(&smixer->play_ringbuffers, _AAX_RINGBUFFER);
               }
               if (res != UINT_MAX)
               {
                  res = _intBufAddData(handle->sensors,_AAX_SENSOR, sensor);
                  if (res != UINT_MAX)
                  {
                     unsigned int num;

                     sensor->count = 1;
                     num = _aaxGetNoEmitters();
                     sensor->mixer->info->max_emitters = num;
                     num = _AAX_MAX_MIXER_REGISTERED;
                     sensor->mixer->info->max_registered = num;

                     _PROP_PITCH_SET_CHANGED(smixer->props3d);
                     _PROP_MTX_SET_CHANGED(smixer->props3d);

                     _aaxSignalInit(&handle->buffer_ready);

                     return handle;
                  }
                  _intBufErase(&smixer->play_ringbuffers, _AAX_RINGBUFFER, free);
               }
               /* creating the sensor failed */
               free(ptr1);
            }
            _intBufErase(&handle->sensors, _AAX_SENSOR, _aaxFreeSensor);
         }
      }
   }
   return NULL;
}

_aaxConfig*
_aaxReadConfig(_handle_t *handle, const char *devname, int mode)
{
   _aaxConfig* config = calloc(1, sizeof(_aaxConfig));

   if (config)
   {
      _intBufferData *dptr;
      long tract_now;
      char *path, *name;
      void *xid, *be;
      float fq, iv;

      /* read the default setup */
      tract_now = _aaxDriverBackendSetConfigSettings(handle->backends,
                                                     handle->devname, config);
      if (!_tvnow) _tvnow = tract_now;

      /* read the system wide configuration file */
      path = systemConfigFile(NULL);
      if (path)
      {
         xid = xmlOpen(path);
         if (xid != NULL)
         {
            if (xmlNodeTest(xid, "/configuration"))
            {
               int m = (mode > 0) ? 1 : 0;
               _aaxDriverBackendReadConfigSettings(xid, handle->devname, config,
                                                   path, m);
            }
            else {
               _AAX_SYSLOG("Invalid system configuration file.");
            }
            xmlClose(xid);
         }
         free(path);
      }

      /* read the user configurstion file */
      path = userConfigFile();
      if (path)
      {
         xid = xmlOpen(path);
         if (xid != NULL)
         {
            if (xmlNodeTest(xid, "/configuration"))
            {
               int m = (mode > 0) ? 1 : 0;
               _aaxDriverBackendReadConfigSettings(xid, handle->devname,
                                                   config, path, m);
            }
            else {
               _AAX_SYSLOG("Invalid user configuration file.");
            }
            xmlClose(xid);
         }
         free(path);
      }

      if (1)
      {
         char *ptr;

         name = handle->devname[0];
         if (name == _aax_default_devname)
         {
            if (devname) {
               name = strdup((char *)devname);
            }
            else if (config->node[0].devname) {
               name = strdup(config->node[0].devname);
            }

            handle->devname[0] = name;
            ptr = strstr(name, " on ");
            if (ptr)
            {
               *ptr = 0;
               handle->devname[1] = ptr+4;		/* 4 = strlen(" on ") */
            }
         }

         be = _aaxGetDriverBackendByName(handle->backends, name,
                                         &handle->be_pos);
         if (be || (handle->devname[0] != _aax_default_devname)) {
            handle->backend.ptr = be;
         }

         free(handle->backend.driver);
         handle->backend.driver = _aax_strdup(config->backend.driver);

         if (config->node[0].no_emitters)
         {
            unsigned int emitters = config->node[0].no_emitters;
            unsigned int system_max = _aaxGetNoEmitters();

            handle->info->max_emitters = _MINMAX(emitters, 4, system_max);
            _aaxSetNoEmitters(handle->info->max_emitters);
         }
         else {
            handle->info->max_emitters = _aaxGetNoEmitters();
         }

         ptr = config->node[0].setup;
         if (ptr && handle->info->mode == AAX_MODE_WRITE_STEREO)
         {
            if (!strcasecmp(ptr, "surround")) {
               handle->info->mode = AAX_MODE_WRITE_SURROUND;
            } else if (!strcasecmp(ptr, "hrtf")) {
               handle->info->mode = AAX_MODE_WRITE_HRTF;
            } else if (!strcasecmp(ptr, "spatial")) {
               handle->info->mode = AAX_MODE_WRITE_SPATIAL;
            } else if (!strcasecmp(ptr, "stereo")) {
               handle->info->mode = AAX_MODE_WRITE_STEREO;
            }
         }
         else if (ptr && handle->info->mode == AAX_MODE_READ)
         {
            if (!strcasecmp(ptr, "mix")) {
               handle->info->track = AAX_TRACK_MIX;
            } else if (!strcasecmp(ptr, "left")) {
               handle->info->track = AAX_TRACK_LEFT;
            } else if (!strcasecmp(ptr, "right")) {
               handle->info->track = AAX_TRACK_RIGHT;
            } else {
               handle->info->track = AAX_TRACK_ALL;
            }
         }

         if (config->node[0].no_speakers > 0) {
            handle->info->no_tracks = config->node[0].no_speakers;
         }

         if (config->node[0].bitrate >= 64 && config->node[0].bitrate <= 320) {
            handle->info->bitrate = config->node[0].bitrate;
         }

         fq = config->node[0].frequency;
         iv = config->node[0].interval;

         do
         {
            if (fq < _AAX_MIN_MIXER_FREQUENCY) {
               fq = _AAX_MIN_MIXER_FREQUENCY;
            }
            if (fq > _AAX_MAX_MIXER_FREQUENCY) {
               fq = _AAX_MAX_MIXER_FREQUENCY;
            }
            if (iv < _AAX_MIN_MIXER_REFRESH_RATE) {
               iv = _AAX_MIN_MIXER_REFRESH_RATE;
            }
            if (iv > _AAX_MAX_MIXER_REFRESH_RATE) {
               iv = _AAX_MAX_MIXER_REFRESH_RATE;
            }
            iv = fq / INTERVAL(fq / iv);
            handle->info->period_rate = iv;
            handle->info->refresh_rate = iv;
            handle->info->frequency = fq;
            if (config->node[0].update) {
               handle->info->update_rate = (uint8_t)rintf(iv/config->node[0].update);
            } else {
               handle->info->update_rate = (uint8_t)rintf(iv/50);
            }
            if (handle->info->update_rate < 1) {
               handle->info->update_rate = 1;
            }

            handle->valid = HANDLE_ID;
         } 
         while(0);

         if (handle->sensors)
         {
            dptr = _intBufGet(handle->sensors, _AAX_SENSOR, 0);
            if (dptr)
            {
               _sensor_t* sensor = _intBufGetDataPtr(dptr);
               _aaxMixerInfo* info = sensor->mixer->info;
               unsigned int size;

               size = _AAX_MAX_SPEAKERS * sizeof(vec4f_t);
               if (handle->info->mode == AAX_MODE_WRITE_HRTF)
               {
                  handle->info->no_tracks = 2;
                  _aax_memcpy(&info->speaker,&_aaxContextDefaultHRTFVolume, size);
                  _aax_memcpy(info->delay, &_aaxContextDefaultHRTFDelay, size);

                  /*
                   * By mulitplying the delays with the sample frequency the
                   * delays in seconds get converted into sample offsets.
                   */
                  _aaxContextSetupHRTF(config->node[0].hrtf, 0);
                  vec4fFill(info->hrtf[0].v4, _aaxContextDefaultHead[0]);
                  vec4fScalarMul(&info->hrtf[0], fq);

                  vec4fFill(info->hrtf[1].v4, _aaxContextDefaultHead[1]);
                  vec4fScalarMul(&info->hrtf[1], fq);
               }
               else
               {
                  unsigned int t;

                  _aaxContextSetupSpeakers(config->node[0].speaker,
                                           info->router, info->no_tracks);
                  for (t=0; t<handle->info->no_tracks; t++)
                  {
                     vec3f_t sv;
                     vec3fFill(sv.v3, _aaxContextDefaultSpeakersVolume[t]);
                     float gain = vec3fNormalize((vec3f_ptr)&info->speaker[t], &sv);
                     info->speaker[t].v4[3] = 1.0f/gain;
                  }
                  _aax_memcpy(info->delay, &_aaxContextDefaultSpeakersDelay, size);
               }
               _intBufReleaseData(dptr, _AAX_SENSOR);
            }

            do
            {
               char buffer[1024], *buf = (char *)&buffer;
               _intBuffers* backends = get_backends();
               unsigned int i, count;

               for (i=0; i<config->no_nodes; i++)
               {
                  snprintf(buf,1024,"config file; settings:");
                  _AAX_SYSLOG(buf);

                  snprintf(buf,1024,"  output[%i]: '%s'\n", i, config->node[i].devname);
                 _AAX_SYSLOG(buf);

                  snprintf(buf,1024,"  setup: %s\n", (handle->info->mode == AAX_MODE_READ) ? "capture" : config->node[i].setup);
                  _AAX_SYSLOG(buf);

                  snprintf(buf,1024,"  frequency: %5.1f, interval: %5.1f\n",
                           handle->info->frequency, handle->info->refresh_rate);
                  _AAX_SYSLOG(buf);
               }

               count = _intBufGetNumNoLock(backends, _AAX_BACKEND);
               for (i=0; i<count; i++)
               {
                  _aaxDriverBackend *be = _aaxGetDriverBackendByPos(backends, i);
                  if (be)
                  {
                     snprintf(buf,1024,"  backend[%i]: '%s'\n", i, be->driver);
                     _AAX_SYSLOG(buf);
                  }
               }
            }
            while (0);
         }
      }
      else
      {
         free(config);
         config = NULL;
      }
   }

   return config;
}

static void
_aaxContextSetupHRTF(void *xid, UNUSED(unsigned int n))
{
   if (xid)
   {
      float f = (float)xmlNodeGetDouble(xid, "gain");
      _aaxContextDefaultHead[HRTF_FACTOR][GAIN] = f;

      f = (float)xmlNodeGetDouble(xid, "side-delay-sec");
      _aaxContextDefaultHead[HRTF_FACTOR][DIR_RIGHT] = f;

      f = (float)xmlNodeGetDouble(xid, "side-offset-sec");
      _aaxContextDefaultHead[HRTF_OFFSET][DIR_RIGHT] = f;

      f = (float)xmlNodeGetDouble(xid, "up-delay-sec");
      _aaxContextDefaultHead[HRTF_FACTOR][DIR_UPWD] = f;

      f = (float)xmlNodeGetDouble(xid, "up-offset-sec");
      _aaxContextDefaultHead[HRTF_OFFSET][DIR_UPWD] = f;

      f = (float)xmlNodeGetDouble(xid, "forward-delay-sec");
      _aaxContextDefaultHead[HRTF_FACTOR][DIR_BACK] = f;

      f = (float)xmlNodeGetDouble(xid, "forward-offset-sec");
      _aaxContextDefaultHead[HRTF_OFFSET][DIR_BACK] = f;
   }
}

static void
_aaxContextSetupSpeakers(void **speaker, unsigned char *router, unsigned int n)
{
   unsigned int i;

   for (i=0; i<n; i++)
   {
      void *xsid = speaker[i];

      if (xsid)
      {
         unsigned int channel;
         vec3f_t v;
         float f;

         channel = xmlNodeGetInt(xsid, "channel");
         if (channel >= n) channel = n-1;

         router[i] = channel;

         f = (float)xmlNodeGetDouble(xsid, "volume-norm");
         if (f) {
            _aaxContextDefaultSpeakersVolume[channel][GAIN] = f;
         } else {
            _aaxContextDefaultSpeakersVolume[channel][GAIN] = 1.0f;
         }

         v.v3[0] = -(float)xmlNodeGetDouble(xsid, "pos-x");
         v.v3[1] = -(float)xmlNodeGetDouble(xsid, "pos-y");
         v.v3[2] = (float)xmlNodeGetDouble(xsid, "pos-z");
         vec3fFill(_aaxContextDefaultSpeakersVolume[channel], v.v3);
      }
   }
}

enum aaxErrorType
__aaxDriverErrorSet(aaxConfig config, enum aaxErrorType err, const char* fnname)
{
   _handle_t *handle = get_driver_handle(config);
   enum aaxErrorType rv = AAX_INVALID_HANDLE;

   __aaxErrorSet(err, fnname);
   if (handle)
   {
      rv = handle->error;
      handle->error = err;
   }

   return rv;
}

static void
_aaxFreeSensor(void *ssr)
{
   _sensor_t *sensor = (_sensor_t*)ssr;
   _aaxRingBufferDelayEffectData* effect;
   _aaxAudioFrame* smixer = sensor->mixer;

   /* frees both EQUALIZER_LF and EQUALIZER_HF */
   free(sensor->filter[EQUALIZER_LF].data);

   /* frees both HRTF_HEADSHADOW and SURROUND_CROSSOVER_LP **/
   free(sensor->filter[HRTF_HEADSHADOW].data);

   free(_FILTER_GET2D_DATA(smixer, FREQUENCY_FILTER));
   free(_FILTER_GET2D_DATA(smixer, DYNAMIC_GAIN_FILTER));
   free(_FILTER_GET2D_DATA(smixer, TIMED_GAIN_FILTER));
   free(_EFFECT_GET2D_DATA(smixer, DYNAMIC_PITCH_EFFECT));
   free(_EFFECT_GET2D_DATA(smixer, REVERB_EFFECT));
   free(_EFFECT_GET3D_DATA(smixer, CONVOLUTION_EFFECT));

   effect = _EFFECT_GET2D_DATA(smixer, DELAY_EFFECT);
   if (effect) free(effect->history_ptr);
   free(effect);

   _intBufErase(&smixer->p3dq, _AAX_DELAYED3D, _aax_aligned_free);
   _aax_aligned_free(smixer->props3d->dprops3d);
   free(smixer->props3d);

   if (smixer->ringbuffer) {
      _aaxRingBufferFree(smixer->ringbuffer);
   }
   _intBufErase(&smixer->frames, _AAX_FRAME, _aaxAudioFrameFree);
   _intBufErase(&smixer->devices, _AAX_DEVICE, _aaxDriverFree);
   _intBufErase(&smixer->emitters_2d, _AAX_EMITTER, free);
   _intBufErase(&smixer->emitters_3d, _AAX_EMITTER, free);
   _intBufErase(&smixer->play_ringbuffers, _AAX_RINGBUFFER,
                                           _aaxRingBufferFree);
   _intBufErase(&smixer->frame_ringbuffers, _AAX_RINGBUFFER,
                                            _aaxRingBufferFree);
   free(sensor);
}
