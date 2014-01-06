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
# if HAVE_STRINGS_H
#  include <strings.h>   /* strcasecmp */
# endif
#endif
#include <time.h>	/* time */

#include <xml.h>

#include "api.h"
#include "devices.h"
#include "base/logging.h"

#include <stream/file.h>
#include <dmedia/device.h>
#include <alsa/device.h>
#include <oss/device.h>
#ifdef HAVE_WINDOWS_H
# include <windows/wasapi.h>
#endif
#include <arch.h>
#include <ringbuffer.h>
#include <filters/effects.h>

static _intBuffers *_aaxIntDriverGetBackends();

char is_bigendian()
{
   static char __big_endian = 0;
   static char detect = 0;
   if (!detect)
   {
      unsigned int _t = 1;
      __big_endian = (*(char *)&_t == 0);
      detect = 1;
   }
   return __big_endian;
}

_intBuffers *
_aaxGetDriverBackends()
{
   _intBuffers *backends = NULL;
   unsigned int r;

   _AAX_LOG(LOG_WARNING, __FUNCTION__);

   r = _intBufCreate(&backends, _AAX_BACKEND);
   if (r != UINT_MAX)
   {
      _intBuffers *dbe = _aaxIntDriverGetBackends();
      unsigned int i, num;

      num = _intBufGetNumNoLock(dbe, _AAX_BACKEND);
      for (i=0; i<num; i++)
      {
         _intBufferData *dptr;
         _aaxDriverBackend *be;

         dptr = _intBufGetNoLock(dbe, _AAX_BACKEND, i);
         assert (dptr);

         be = _intBufGetDataPtr(dptr);
         if (be)
         {
            assert(be->detect);
#if 0
            printf("Backend #%i: '%s'\n", i, be->driver);
#endif
            if (be->detect(AAX_MODE_WRITE_STEREO) || be->detect(AAX_MODE_READ))
            {
               unsigned int r;

               r = _intBufAddReference(backends, _AAX_BACKEND, dbe, i);
               if (r == UINT_MAX) break;
            }
         }
      }
   }

   return backends;
}

void *
_aaxRemoveDriverBackends(_intBuffers **be)
{
   _intBufErase(be, _AAX_BACKEND, free);
   return 0;
}

_aaxDriverBackend *
_aaxGetDriverBackendByName(_intBuffers *bs, const char *name, unsigned int *pos)
{
   _aaxDriverBackend *be = 0;

   assert(bs != 0);

   if (name)
   {
      _aaxDriverBackend *b;
      unsigned int i, num;

      num = _intBufGetNumNoLock(bs, _AAX_BACKEND);
      for (i=0; i<num; i++)
      {
         _intBufferData *dptr;
         int res;

         dptr = _intBufGetNoLock(bs, _AAX_BACKEND, i);
         b = _intBufGetDataPtr(dptr);

         {
            res = strcasecmp(b->driver, name);
            if (!res)
            {
               *pos = i;
               be = (_aaxDriverBackend *)b;
               break;
            }
         }
      }

      if (i == num)
      {
         be = 0;
         *pos = UINT_MAX;
      }
   }
   else {
      be = _aaxGetDriverBackendDefault(bs, pos);
   }

   return be;
}

_aaxDriverBackend *
_aaxGetDriverBackendDefault(_intBuffers *bs, unsigned int *pos)
{
   _aaxDriverBackend *be = 0;
   int i;

   assert (bs != 0);

   i = _intBufGetNumNoLock(bs, _AAX_BACKEND);
   while (i--)
   {
      _intBufferData *dptr;

      dptr = _intBufGetNoLock(bs, _AAX_BACKEND, i);
      be = _intBufGetDataPtr(dptr);
      if (be->detect(AAX_MODE_WRITE_STEREO) &&
          be->state(NULL, DRIVER_SUPPORTS_PLAYBACK))
      {
         *pos = i;
         break;
      }
   }

   return be;
}

_aaxDriverBackend *
_aaxGetDriverBackendLoopback(unsigned int *pos)
{
   _intBuffers *dbe = _aaxIntDriverGetBackends();
   const char *name = "AeonWave Loopback";
   _aaxDriverBackend *be = 0;
   unsigned int i, num;

   num = _intBufGetNumNoLock(dbe, _AAX_BACKEND);
   for (i=0; i<num; i++)
   {
      _aaxDriverBackend *found_be;
      _intBufferData *dptr;

      dptr = _intBufGetNoLock(dbe, _AAX_BACKEND, i);
      found_be = _intBufGetDataPtr(dptr);
      if (found_be)
      {
         if (!strcasecmp(found_be->driver, name))
         {
            *pos = i;
            be = found_be;
            break;
         }
      }
   }

   return be;
}

_aaxDriverBackend *
_aaxGetDriverBackendDefaultCapture(_intBuffers *bs, unsigned int *pos)
{
   _aaxDriverBackend *be = 0;
   int i;

   assert (bs != 0);

   i = _intBufGetNumNoLock(bs, _AAX_BACKEND);
   while(i--)
   {
      _intBufferData *dptr;

      dptr = _intBufGetNoLock(bs, _AAX_BACKEND, i);
      be = _intBufGetDataPtr(dptr);
      if (be->detect(AAX_MODE_READ) &&
          be->state(NULL, DRIVER_SUPPORTS_CAPTURE))
      {
         *pos = i;
         break;
      }
   }

   return be;
}

_aaxDriverBackend *
_aaxGetDriverBackendByPos(_intBuffers *bs, unsigned int pos)
{
   _aaxDriverBackend *be = 0;
   unsigned int num;

   assert(bs != 0);

   num = _intBufGetNumNoLock(bs, _AAX_BACKEND);
   if (num > 0 && pos < num)
   {
      _intBufferData *dptr;
      dptr = _intBufGetNoLock(bs, _AAX_BACKEND, num-pos-1);
      be = _intBufGetDataPtr(dptr);
   }
   return be;
}

const char *
_aaxGetDriverBackendName(const _aaxDriverBackend *be)
{
   return be->driver;
}

long
_aaxDriverBackendSetConfigSettings(_intBuffers *bs, char** devname, _aaxConfig *config)
{
   _intBuffers *dbe = _aaxIntDriverGetBackends();
   time_t rv = time(NULL);
   _aaxDriverBackend *be;
   unsigned int i, num;

   memset(config, 0, sizeof(_aaxConfig));
   if (dbe)
   {
      unsigned int pos;

      num = _intBufGetNumNoLock(dbe, _AAX_BACKEND);
      config->no_backends = num;
      for (i=0; i<num; i++)
      {
         _intBufferData *dptr;
         dptr = _intBufGetNoLock(dbe, _AAX_BACKEND, i);
         be = _intBufGetDataPtr(dptr);
         if (be)
         {
            if (!strcasecmp(devname[0], be->driver))
            {
               config->backend.driver = _aax_strdup(be->driver);
               config->backend.input = 0;
               config->backend.output = 0;
            }
         }
      }

      be = _aaxGetDriverBackendDefault(bs, &pos);
      if (be)
      {
         config->node[0].devname = _aax_strdup(be->driver);
         config->node[0].frequency = 48000;
      }
      else
      {
         config->node[0].devname = _aax_strdup("unknown");
         config->node[0].frequency = 44100;
      }
      config->node[0].setup = _aax_strdup("stereo");
      config->no_nodes = 1;
      config->node[0].interval = 66;
      config->node[0].update = 50;
      config->node[0].hrtf = 0;
      config->node[0].no_speakers = 2;
   }

   return (long)rv; 
}

static char*
_aaxDriverDetectConfigRenderer(char *xid, char **devname, char *l, char *cl)
{
   unsigned int size;
   char curlevel = *cl;
   char level = 0;
   char rr[255];

   *rr = '\0';
   size = xmlNodeCopyString(xid, "renderer", (char*)&rr, 255);
   if (size)
   {
      if (devname[1])
      {
         if (strlen(devname[1]) < size) size = strlen(devname[1]);

         if (!strcasecmp(rr, "default") && level < 2) level = 1;
         if (!strncasecmp(devname[1], rr, size))
         {
            char *dptr = strstr(devname[1], ": ");
            char *rrptr = strstr(rr, ": ");
            if ((dptr-devname[1]) == (rrptr-rr)) level = 3;
            else level = 2;
         }
         else {
            xid = 0;
         }
      }			/* no renderer is requested and default is defined */
      else if (!strcasecmp(rr, "default")) level = 3;
   }			/* no renderer defined but default is requested */
   else if (!devname[1] || !strcasecmp(devname[1], "default")) level = 2;

   *l = level;
   *cl = curlevel;

   return xid;
}

void
_aaxDriverBackendReadConfigSettings(void *xid, char **devname, _aaxConfig *config, const char *path, int m)
{
   void *xcid = xmlNodeGet(xid, "/configuration");

   if (xcid != NULL && config != NULL)
   {
      unsigned int n, num;
      void *xoid;
      float v;

      v = (float)xmlNodeGetDouble(xcid, "version");
      if ((v < AAX_MIN_CONFIG_VERSION) || (v > AAX_MAX_CONFIG_VERSION))
      {
         xmlFree(xcid);
         fprintf(stderr, "AAX: incompattible configuration file version, skipping.\n");
         return;
      }

      xoid = xmlMarkId(xcid);
      num = xmlNodeGetNum(xoid, "output");      /* global output sections */
      config->no_nodes = num;
      for (n=0; n<num; n++)
      {
         unsigned int be, be_num;
         char *dev;

         xmlNodeGetPos(xcid, xoid, "output", n);

         if (n < _AAX_MAX_SLAVES)
         {
            char driver_name[255];
            unsigned int i;
            char curlevel;
            char *setup;
            void *xbid;
            float f;

            dev = xmlNodeGetString(xoid, "device");
            if (dev)
            {
               free(config->node[n].devname);
               config->node[n].devname = _aax_strdup(dev);
               xmlFree(dev);
            }

#if 0
            setup = getenv("AAX_TUBE_COMPRESSOR");
            if (setup) {
               i = _aax_getbool(setup);
            } else {
               i = xmlNodeGetBool(xoid, "tube-compressor");
            }
            if (i) {
               
               _aaxProcessCompression = bufCompressValve;
            } else {
               _aaxProcessCompression = bufCompressElectronic;
            }
#endif

            setup = xmlNodeGetString(xoid, "setup");
            if (setup)
            {
               free(config->node[n].setup);
               config->node[n].setup = _aax_strdup(setup);
               xmlFree(setup);
            }

            config->node[n].hrtf = xmlNodeCopy(xoid, "head");

            f = (float)xmlNodeGetDouble(xoid, "frequency-hz");
            if (f) config->node[n].frequency = f;

            f = (float)xmlNodeGetDouble(xoid, "interval-hz");
            if (f) config->node[n].interval = f;

            f = (float)xmlNodeGetDouble(xoid, "update-hz");
            if (f) config->node[n].update = f;

            i = xmlNodeGetInt(xoid, "max-emitters");
            if (i) config->node[n].no_emitters = i;


            /*
             * find a mathcing backend
             * level == 0, not fout
             * level == 1, defaul device found
             * level == 2, requested device found
             * level == 3, requested device found with requested output port
             */
            curlevel = 0;
            xbid = xmlMarkId(xcid);
            be_num = xmlNodeGetNum(xbid, "backend");
            snprintf((char*)driver_name, 255, "%s", config->backend.driver);
            for (be=0; be<be_num; be++)
            {
               char level;

               xmlNodeGetPos(xcid, xbid, "backend", be);
               if (xmlNodeCompareString(xbid, "name", driver_name)) {
                  continue;
               }

               if (m)
               {
                  char *output = xmlNodeCopy(xbid, "output");
                  level = 0;

                  if (output)
                  {
                     _aaxDriverDetectConfigRenderer(output, devname,
                                                      (char *)&level,
                                                      (char *)&curlevel);
                     if (level > curlevel)
                     {
                        unsigned int q, i, l, index = -1;
                        char *ptr, *tmp;
                        char rr[255];
                        void *xsid;

                        curlevel = level;

                        xmlNodeCopyString(output, "renderer", (char*)&rr, 255);
                        ptr = config->backend.driver;

                        tmp = strstr(ptr, " on ");
                        if (tmp) q = tmp-ptr;
                        else q = strlen(ptr);

                        l = ++q + strlen(rr) + strlen(" on \0");
                        config->backend.driver = malloc(l);
                        snprintf(config->backend.driver, q, "%s", ptr);
                        strcat(config->backend.driver, " on ");
                        strcat(config->backend.driver, rr);
                        free(ptr);

                        xmlFree(config->backend.output);
                        config->backend.output = output;

                        /* setup speakers */
                        xsid = xmlMarkId(output);

                        i = xmlNodeGetInt(xsid, "channels");
                        if (i > _AAX_MAX_SPEAKERS) i = _AAX_MAX_SPEAKERS;
                        config->node[n].no_speakers = i;

                        i = xmlNodeGetNum(xsid, "speaker");
                        if (i > _AAX_MAX_SPEAKERS) i = _AAX_MAX_SPEAKERS;

                        ptr = xmlNodeGetString(xsid, "setup");
                        if (ptr)
                        {
                           free(config->node[0].setup);
                           config->node[0].setup = _aax_strdup(ptr);
                           xmlFree(ptr);
                        }

                        for (q=0; q<i; q++)
                        {
                           char attrib[10];
                           void *ptr;

                           if (xmlAttributeCopyString(output, "n", (char*)&attrib, 9))
                           {
                              char *pe = (char *)&attrib + index;
                              index = strtol(attrib, &pe, 10);
                           }
                           else index++;
                           if (index >= _AAX_MAX_SPEAKERS) {
                              index = _AAX_MAX_SPEAKERS;
                           }

                           xmlFree(config->node[n].speaker[index]);
                           ptr = xmlNodeCopyPos(output, xsid, "speaker", q);
                           config->node[n].speaker[index] = ptr;
                        }
                        xmlFree(xsid);
                     }
                     else {
                        xmlFree(output);
                     }
                  }
               }
               else /* m == AAAX_MODE_READ */
               {
                  char *input = xmlNodeCopy(xbid, "input");

                  level = 0;
                  if (input)
                  {
                     _aaxDriverDetectConfigRenderer(input, devname,
                                                      (char *)&level,
                                                      (char *)&curlevel);
                     if (level >= curlevel)
                     {
                        unsigned int q, l;
                        char *ptr, *tmp;
                        char rr[255];
 
                        curlevel = level;

                        xmlNodeCopyString(input, "renderer", (char*)&rr, 255);
                        ptr = config->backend.driver;

                        tmp = strstr(ptr, " on ");
                        if (tmp) q = tmp-ptr;
                        else q = strlen(ptr);

                        l = ++q + strlen(rr) + strlen(" on \0");
                        config->backend.driver = malloc(l);
                        snprintf(config->backend.driver, q, "%s", ptr);
                        strcat(config->backend.driver, " on ");
                        strcat(config->backend.driver, rr);
                        free(ptr);

                        xmlFree(config->backend.input);
                        config->backend.input = input;
                     }
                     else {
                        xmlFree(input);
                     }
                  }
               }
            }
            xmlFree(xbid);
         }
      }
      xmlFree(xoid);
      xmlFree(xcid);
   }
}

void
_aaxDriverBackendClearConfigSettings(_aaxConfig *config)
{
   unsigned int i;
   for (i=0; i<config->no_nodes; i++)
   {
      int q;
      free(config->node[i].setup);
      free(config->node[i].devname);
      xmlFree(config->node[i].hrtf);
      for (q=0; q<config->node[i].no_speakers; q++) {
         xmlFree(config->node[i].speaker[q]);
      }
   }

   free(config->backend.driver);
   xmlFree(config->backend.input);
   xmlFree(config->backend.output);

   free(config);
}

/* -------------------------------------------------------------------------- */

static _intBuffers *_aaxIntBackends = NULL;

static void
_aaxIntDriverRemoveBackends()
{
   if (_aaxIntBackends)
   {
      _intBufErase(&_aaxIntBackends, _AAX_BACKEND, 0);
      _aaxIntBackends = NULL;
   }
}

static _intBuffers *
_aaxIntDriverGetBackends()
{
   if (_aaxIntBackends == NULL)
   {
      unsigned int r = _intBufCreate(&_aaxIntBackends, _AAX_BACKEND);
      if (r == UINT_MAX) return NULL;

      _intBufAddData(_aaxIntBackends, _AAX_BACKEND, &_aaxNoneDriverBackend);
      _intBufAddData(_aaxIntBackends, _AAX_BACKEND, &_aaxFileDriverBackend);
      _intBufAddData(_aaxIntBackends, _AAX_BACKEND, &_aaxLoopbackDriverBackend);
      _intBufAddData(_aaxIntBackends, _AAX_BACKEND, &_aaxOSSDriverBackend);
      _intBufAddData(_aaxIntBackends, _AAX_BACKEND, &_aaxALSADriverBackend);
#ifdef HAVE_WINDOWS_H
      _intBufAddData(_aaxIntBackends, _AAX_BACKEND, &_aaxWASAPIDriverBackend);
#endif
      _intBufAddData(_aaxIntBackends, _AAX_BACKEND, &_aaxDMediaDriverBackend);

      atexit(_aaxIntDriverRemoveBackends);
   }

   return _aaxIntBackends;
}

