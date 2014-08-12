/*
 * Copyright 2013-2014 by Erik Hofman.
 * Copyright 2013-2014 by Adalin B.V.
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
#endif

#include <aax/aax.h>

#include <base/threads.h>
#include <base/logging.h>

#include <api.h>

#include "software/renderer.h"

// Semaphores:
//   http://docs.oracle.com/cd/E19683-01/806-6867/sync-27385/index.html
//
// Windows Threadpool with semaphores:
//   http://msdn.microsoft.com/en-us/library/windows/desktop/ms686946%28v=vs.85%29.aspx


/*
 * The Pool renderer uses a thread worker with one thread tied to every
 * physical CPU core. This wil get the optimum rendering speed since it
 * can utilize the SSE registeres for every core simultaniously without
 * the possibility of choking the CPU caches.
 */

static _renderer_detect_fn _aaxWorkerDetect;
static _renderer_new_handle_fn _aaxWorkerSetup;
static _render_get_info_fn _aaxWorkerInfo;
static _renderer_open_fn _aaxWorkerOpen;
static _renderer_close_fn _aaxWorkerClose;
static _render_process_fn _aaxWorkerProcess;


_aaxRenderer*
_aaxDetectPoolRenderer()
{
   const char *env = getenv("AAX_USE_THREADPOOL");
   _aaxRenderer* rv = NULL;

   if ((!env || _aax_getbool(env)) && (_aaxGetNoCores() > 1))
   {
      rv = calloc(1, sizeof(_aaxRenderer));
      if (rv)
      {
         rv->detect = _aaxWorkerDetect;
         rv->setup = _aaxWorkerSetup;
         rv->info = _aaxWorkerInfo;

         rv->open = _aaxWorkerOpen;
         rv->close = _aaxWorkerClose;
         rv->process = _aaxWorkerProcess;
      }
   }
   return rv;
}

/* -------------------------------------------------------------------------- */

#define _AAX_MAX_NO_WORKERS		8
#define _AAX_MIN_EMITTERS_PER_WORKER	32

typedef struct
{
   _intBuffers *he;

   _aaxSemaphore *worker_start;
   _aaxSemaphore *worker_ready;
   _aaxMutex *mutex;

   int worker_no;
   int no_workers;
   int workers_busy;

   int max_emitters;
   int stage;
   
   struct threat_t thread[_AAX_MAX_NO_WORKERS];
   _aaxRendererData *data;

} _render_t;

static void* _aaxWorkerThread(void*);


static int
_aaxWorkerDetect() {
   return AAX_TRUE;
}

static void*
_aaxWorkerOpen(void* id)
{
   return id;
}

static int
_aaxWorkerClose(void* id)
{
   _render_t *handle = (_render_t*)id;
   int i;

   // signall all the worker-threads to quit
   for (i=0; i<handle->no_workers; i++)
   {
      struct threat_t *thread = &handle->thread[i];

      thread->started = AAX_FALSE;
      _aaxSemaphoreRelease(handle->worker_start);
   }

   // wait until all worker-threads are finished
   for (i=0; i<handle->no_workers; i++)
   {
      struct threat_t *thread = &handle->thread[i];

      _aaxThreadJoin(thread->ptr);
      if (thread->ptr) {
         _aaxThreadDestroy(thread->ptr);
      }
   }
   _aaxSemaphoreDestroy(handle->worker_start);
   _aaxSemaphoreDestroy(handle->worker_ready);
   _aaxMutexDestroy(handle->mutex);

   free(handle);

   return AAX_TRUE;
}

static void*
_aaxWorkerSetup(int dt)
{
   _render_t *handle = calloc(1, sizeof(_render_t));
   if (handle)
   {
      int i, res;

      handle->no_workers = _MIN(_aaxGetNoCores(), _AAX_MAX_NO_WORKERS);

      handle->mutex = _aaxMutexCreate(NULL);
      handle->worker_start = _aaxSemaphoreCreate(0);
      handle->worker_ready = _aaxSemaphoreCreate(0);

      for (i=0; i<handle->no_workers; i++)
      {
         struct threat_t *thread = &handle->thread[i];

         handle->worker_no = i;
         thread->ptr = _aaxThreadCreate();
         res = _aaxThreadStart(thread->ptr, _aaxWorkerThread, handle, dt);
         if (res == 0)
         {
            int q = 100;
            while (q-- && thread->started != AAX_TRUE) {
               msecSleep(1);
            }
         }
         else {
            _AAX_LOG(LOG_WARNING,  "Thread Pool renderer: thread failed");
         }
      }
      handle->worker_no = 0;
   }
   else {
      _AAX_LOG(LOG_WARNING, "Thread Pool renderer: Insufficient memory");
   }

   return (void*)handle;
}

static char*
_aaxWorkerInfo(void *id)
{
   _render_t *handle = id;
   static char info[32] = "";

   if (handle && strlen(info) == 0) {
      snprintf(info, 32, "using %i cores", handle->no_workers);
   }

   return info;
}

/*
 * Wait for a worker thread to become ready.
 */
static int
_aaxWorkerProcess(struct _aaxRenderer_t *renderer, _aaxRendererData *data)
{
   _render_t *handle = renderer->id;
   _intBuffers *he = data->e3d;
   unsigned int stage;
   int rv = AAX_FALSE;

   stage = 2;
   do
   {
      unsigned int no_emitters, max_emitters;

      max_emitters = _intBufGetMaxNum(he, _AAX_EMITTER);
      no_emitters = _intBufGetNumNoLock(he, _AAX_EMITTER);
      if (no_emitters)
      {
         int num;

         handle->he = he;
         handle->stage = stage;
         handle->max_emitters = max_emitters;
         handle->data = data;

         // wake up the worker threads
         num = no_emitters/_AAX_MIN_EMITTERS_PER_WORKER;
         num = _MINMAX(num, 1, handle->no_workers);
         do {
            _aaxSemaphoreRelease(handle->worker_start);
         }
         while (--num);

         // Wait until al worker threads are finished
         _aaxSemaphoreWait(handle->worker_ready);

         rv = AAX_TRUE;
      }
      _intBufReleaseNum(he, _AAX_EMITTER);

      /*
       * stage == 2 is 3d positional audio
       * stage == 1 is stereo audio
       */
      if (stage == 2) {
         he = data->e2d;	/* switch to stereo */
      }
   }
   while (--stage); /* process 3d positional and stereo emitters */

   return rv;
}


/* ------------------------------------------------------------------------- */

static void*
_aaxWorkerThread(void *id)
{
   _render_t *handle = (_render_t*)id;
   struct threat_t *thread;
   _aaxRendererData *data;
   int worker_no;

   worker_no = handle->worker_no;
   thread = &handle->thread[worker_no];

   _aaxThreadSetAffinity(thread->ptr, worker_no % _aaxGetNoCores());
   thread->started = AAX_TRUE;

   do
   {
      _aaxSemaphoreWait(handle->worker_start);
      data = handle->data;
      if (data && !data->drb) {
         _aaxSemaphoreRelease(handle->worker_ready);
      }
   }
   while (data && !data->drb && TEST_FOR_TRUE(thread->started));
      
   if TEST_FOR_TRUE(thread->started)
   {
      _aaxRingBuffer *drb;

      drb = data->drb->duplicate(data->drb, AAX_TRUE, AAX_FALSE);
      drb->set_state(drb, RB_STARTED);

      do
      {
         int pos = _aaxAtomicIntSub(&handle->max_emitters, 1);

         /*
          * It might be possible that other threads aleady processed
          * all emitters which causes pos to turn negative here.
          */
         if (pos >= 0)
         {
            _aaxAtomicIntAdd(&handle->workers_busy, 1);
            do
            {
               _intBufferData *dptr_src;

               dptr_src = _intBufGet(handle->he, _AAX_EMITTER, pos);
               if (dptr_src)
               {
                  // _aaxProcessEmitter calls
                  // _intBufReleaseData(dptr_src, _AAX_EMITTER);
                  data->mix_emitter(drb, data, dptr_src, handle->stage);
               }
               pos = _aaxAtomicIntSub(&handle->max_emitters, 1);
            }
            while(pos >= 0);

            /* mix our own ringbuffer with that of the mixer */
            _aaxMutexLock(handle->mutex);
            data->drb->data_mix(data->drb, drb, NULL);
            _aaxMutexUnLock(handle->mutex);

            /* clear our own ringbuffer for future use */
            drb->data_clear(drb);
            drb->set_state(drb, RB_REWINDED);

            /* if we're the last active worker trigger the signal */
            if (_aaxAtomicIntSub(&handle->workers_busy, 1) == 0) {
                _aaxSemaphoreRelease(handle->worker_ready);
            }
         }

         _aaxSemaphoreWait(handle->worker_start);
      }
      while TEST_FOR_TRUE(thread->started);

      drb->destroy(drb);
   }

   return handle;
}
