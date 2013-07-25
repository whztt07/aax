/*
 * Copyright 2012 by Erik Hofman.
 * Copyright 2012 by Adalin B.V.
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

#include <ringbuffer.h>
#include <objects.h>
#include <api.h>

/**
 * ssv -> sensor sound velocity
 * sdf -> sensor doppler factor
 * pos
 */
char
_aaxEmittersProcess(_oalRingBuffer *dest_rb, const _aaxMixerInfo *info,
                    float ssv, float sdf, _oalRingBuffer2dProps *fp2d,
                    _oalRingBuffer3dProps *fp3d,
                    _intBuffers *e2d, _intBuffers *e3d,
                    const _aaxDriverBackend* be, void *be_handle)
{
   unsigned int num, stage;
   _intBuffers *he = e3d;
   char rv = AAX_FALSE;
   float dt;

   num = 0;
   stage = 2;
   dt = _oalRingBufferGetParamf(dest_rb, RB_DURATION_SEC);
   do
   {
      unsigned int i, no_emitters;

      no_emitters = _intBufGetMaxNum(he, _AAX_EMITTER);
      num += no_emitters;

      for (i=0; i<no_emitters; i++)
      {
         _intBufferData *dptr_src;
         _emitter_t *emitter;
         _aaxEmitter *src;

         dptr_src = _intBufGet(he, _AAX_EMITTER, i);
         if (!dptr_src) continue;

         dest_rb->curr_pos_sec = 0.0f;

         emitter = _intBufGetDataPtr(dptr_src);
         src = emitter->source;
         if (_IS_PLAYING(src->dprops3d))
         {
            _intBufferData *dptr_sbuf;
            unsigned int nbuf;
            int streaming;

            rv = AAX_TRUE;

            nbuf = _intBufGetNum(src->buffers, _AAX_EMITTER_BUFFER);
            assert(nbuf > 0);

            streaming = (nbuf > 1);
            dptr_sbuf = _intBufGet(src->buffers, _AAX_EMITTER_BUFFER, src->pos);
            if (dptr_sbuf)
            {
               _embuffer_t *embuf = _intBufGetDataPtr(dptr_sbuf);
               _oalRingBuffer *src_rb = embuf->ringbuffer;
               unsigned int res = 0;
               do
               {
                  if (_IS_STOPPED(src->dprops3d)) {
                     _oalRingBufferStop(src_rb);
                  }
                  else if (_oalRingBufferGetParami(src_rb, RB_IS_PLAYING) == 0)
                  {
                     if (streaming) {
                        _oalRingBufferStartStreaming(src_rb);
                     } else {
                        _oalRingBufferStart(src_rb);
                     }
                  }

                  --src->update_ctr;
                   
                  /* 3d mixing */
                  if (stage == 2)
                  {
                     _oalRingBuffer2dProps *ep2d;

                     assert(_IS_POSITIONAL(src->dprops3d));

                     if (!src->update_ctr) {
                        be->prepare3d(src, info, ssv, sdf, fp2d->pos, fp3d);
                     }

                     res = AAX_FALSE;
                     ep2d = src->props2d;
                     if (src->curr_pos_sec >= ep2d->dist_delay_sec) {
                        res = be->mix3d(be_handle, dest_rb, src_rb, ep2d,
                                       fp2d, emitter->track, src->update_ctr,
                                       nbuf, info->mode);
                     }
                  }
                  else
                  {
                     assert(!_IS_POSITIONAL(src->dprops3d));
                     res = be->mix2d(be_handle, dest_rb, src_rb, src->props2d,
                                           fp2d, 1.0, 1.0, src->update_ctr,
                                           nbuf);
                  }

                  if (!src->update_ctr) {
                     src->update_ctr = src->update_rate;
                  }

                  src->curr_pos_sec += dt;

                  /*
                   * The current buffer of the source has finished playing.
                   * Decide what to do next.
                   */
                  if (res)
                  {
                     if (streaming)
                     {
                        /* is there another buffer ready to play? */
                        if (++src->pos == nbuf)
                        {
                           /*
                            * The last buffer was processed, return to the
                            * first buffer or stop? 
                            */
                           if TEST_FOR_TRUE(emitter->looping) {
                              src->pos = 0;
                           }
                           else
                           {
                              _SET_STOPPED(src->dprops3d);
                              _SET_PROCESSED(src->dprops3d);
                              break;
                           }
                        }

                        res &= _oalRingBufferGetParami(dest_rb, RB_IS_PLAYING);
                        if (res)
                        {
                           _intBufReleaseData(dptr_sbuf,_AAX_EMITTER_BUFFER);
                           dptr_sbuf = _intBufGet(src->buffers,
                                              _AAX_EMITTER_BUFFER, src->pos);
                           embuf = _intBufGetDataPtr(dptr_sbuf);
                           src_rb = embuf->ringbuffer;
                        }
                     }
                     else /* !streaming */
                     {
                        _SET_PROCESSED(src->dprops3d);
                        break;
                     }
                  }
               }
               while (res);
               _intBufReleaseData(dptr_sbuf, _AAX_EMITTER_BUFFER);
            }
            _intBufReleaseNum(src->buffers, _AAX_EMITTER_BUFFER);
            _oalRingBufferStart(dest_rb);
         }
         _intBufReleaseData(dptr_src, _AAX_EMITTER);
      }
      _intBufReleaseNum(he, _AAX_EMITTER);

      /*
       * stage == 2 is 3d positional audio
       * stage == 1 is stereo audio
       */
      if (stage == 2) {
         he = e2d;	/* switch to stereo */
      }
   }
   while (--stage); /* process 3d positional and stereo emitters */

   return rv;
}


/**
 * ssv:     sensor velocity vector
 * de:      sensor doppler factor
 * fp2dpos: parent frame p2d->pos
 * fp3d:    parent frame dp3d->props3d
 */
void
_aaxEmitterPrepare3d(_aaxEmitter *src,  const _aaxMixerInfo* info, float ssv, float sdf, vec4_t *fp2dpos, _oalRingBuffer3dProps* fp3d)
{
   _oalRingBufferPitchShiftFunc* dopplerfn;
   _oalRingBufferDelayed3dProps *edp3d;
   _oalRingBuffer3dProps *ep3d;
   _oalRingBuffer2dProps *ep2d;
   _oalRingBufferDistFunc* distfn;

   _AAX_LOG(LOG_DEBUG, __FUNCTION__);

   assert(src);
   assert(info);

   edp3d = src->dprops3d;
   ep3d = edp3d->props3d;
   ep2d = src->props2d;

   edp3d->pitch = _EFFECT_GET(ep2d, PITCH_EFFECT, AAX_PITCH);
   edp3d->gain = _FILTER_GET(ep2d, VOLUME_FILTER, AAX_GAIN);
   if (_PROP3D_DISTQUEUE_IS_DEFINED(ep3d))
   {
      if (!src->p3dq) {
         _intBufCreate(&src->p3dq, _AAX_DELAYED3D);
      }

      if (src->p3dq)
      {
         _oalRingBufferDelayed3dProps *sdp3d = NULL;
         _intBufferData *buf;
         float pos;

         _intBufAddData(src->p3dq, _AAX_DELAYED3D, edp3d);
         if (src->curr_pos_sec <= ep2d->dist_delay_sec) {
            return;
         }

         pos = ep2d->bufpos;
         ep2d->bufpos += edp3d->buf_step;
         if (pos <= 0.0f) return;

         do
         {
            buf = _intBufPopData(src->p3dq, _AAX_DELAYED3D);
            if (buf)
            {
               sdp3d = _intBufGetDataPtr(buf);
               free(buf);
            }
            --ep2d->bufpos;
         }
         while (ep2d->bufpos > 1.0f);

         if (!sdp3d) {                  // TODO: get from buffer cache
            sdp3d = _oalRingBufferDelayed3dPropsDup(edp3d);
         }

         src->dprops3d = sdp3d;
         edp3d = src->dprops3d;
         ep3d = edp3d->props3d;
      }
   }

   distfn = _FILTER_GET_DATA(edp3d, DISTANCE_FILTER);
   dopplerfn = _EFFECT_GET_DATA(edp3d, VELOCITY_EFFECT);
   assert(dopplerfn);
   assert(distfn);

   if (_PROP3D_MTXSPEED_HAS_CHANGED(ep3d) || _PROP3D_MTXSPEED_HAS_CHANGED(fp3d))
   {
      vec4_t epos;
      float gain, pitch;
      float dist, esv, ss;
      float min, max;

      /* align the emitter with the parent frame.
       * (compensate for the parents direction offset)
       */
      mtx4Mul(ep3d->m_matrix, fp3d->m_matrix, ep3d->matrix);
      dist = vec3Normalize(epos, ep3d->m_matrix[LOCATION]);
#if 0
 printf("# emitter parent:\t\t\t\temitter:\n");
 PRINT_MATRICES(fp3d->matrix, ep3d->matrix);
 printf("# modified emitter\n");
 PRINT_MATRIX(ep3d->m_matrix);
 printf("# dist: %f\n", dist);
#endif

      /* calculate the sound velocity inbetween the emitter and the sensor */
      esv = _EFFECT_GET(edp3d, VELOCITY_EFFECT, AAX_SOUND_VELOCITY);
      ss = (esv+ssv) / 2.0f;

      /*
       * Doppler
       */
      pitch = edp3d->pitch;
      if (dist > 1.0f)
      {
         float ve, df;
         vec4_t ev;

         /* align velocity vectors with the modified emitter position
          * relative to the sensor
          */
         vec4Matrix4(ep3d->m_velocity, ep3d->velocity, ep3d->m_matrix);
         ve = vec3DotProduct(ep3d->m_velocity, epos);
         df = dopplerfn(0.0f, ve, ss/sdf);

         pitch *= df;
         edp3d->buf_step = df;
      }
      ep2d->final.pitch = pitch;

      /*
       * Distance queues for every speaker (volume)
       */
      gain = edp3d->gain;
      if (_PROP3D_MTX_HAS_CHANGED(ep3d) || _PROP3D_MTX_HAS_CHANGED(fp3d))
      {
         float dist_fact, cone_volume = 1.0f;
         float refdist, maxdist, rolloff;
         unsigned int i;

         _PROP3D_MTX_CLEAR_CHANGED(ep3d);

         refdist = _FILTER_GETD3D(src, DISTANCE_FILTER, AAX_REF_DISTANCE);
         maxdist = _FILTER_GETD3D(src, DISTANCE_FILTER, AAX_MAX_DISTANCE);
         rolloff = _FILTER_GETD3D(src, DISTANCE_FILTER, AAX_ROLLOFF_FACTOR);
         dist_fact = _MIN(dist/refdist, 1.0f);

         switch (info->mode)
         {
         case AAX_MODE_WRITE_HRTF:
         {
            unsigned int t, tracks = info->no_tracks;
            for (t=0; t<tracks; t++)
            {
               for (i=0; i<3; i++)
               {
                  float dp = vec3DotProduct(fp2dpos[3*t+i], epos);
                  float offs, fact;

                  ep2d->pos[3*t+i][0] = dp * dist_fact;  /* -1 .. +1 */

                  dp = 0.5f+dp/2.0f;  /* 0 .. +1 */
                  if (i == DIR_BACK) dp *= dp;
                  if (i == DIR_UPWD) dp = 0.25f*(5.0f*dp - dp*dp);

                  offs = info->hrtf[HRTF_OFFSET][i];
                  fact = info->hrtf[HRTF_FACTOR][i];
                  ep2d->hrtf[t][i] = info->hrtf[HRTF_OFFSET][i];
                  ep2d->hrtf[t][i] = _MAX(offs+dp*fact, 0.0f);
               }
            }
            break;
         }
         case AAX_MODE_WRITE_SPATIAL:
            for (i=0; i<info->no_tracks; i++)
            {
               float dp = vec3DotProduct(fp2dpos[i], epos);
               ep2d->pos[i][0] = 0.5f + dp * dist_fact;
            }
            break;
         case AAX_MODE_WRITE_SURROUND:
         {
            unsigned int t, tracks = info->no_tracks;
            for (t=0; t<tracks; t++)
            {
               for (i=1; i<3; i++) /* skip left-right */
               {
                  float dp = vec3DotProduct(fp2dpos[3*t+i], epos);
                  float offs, fact;

                  dp = 0.5f+dp/2.0f;  /* 0 .. +1 */
                  offs = info->hrtf[HRTF_OFFSET][i];
                  fact = info->hrtf[HRTF_FACTOR][i];
                  ep2d->hrtf[t][i] = info->hrtf[HRTF_OFFSET][i];
                  ep2d->hrtf[t][i] = _MAX(offs+dp*fact, 0.0f);
               }
            }
            /* break not needed */
         }
         default: /* AAX_MODE_WRITE_STEREO */
            for (i=0; i<info->no_tracks; i++)
            {
               vec4Mulvec4(ep2d->pos[i], fp2dpos[i], epos);
               vec4ScalarMul(ep2d->pos[i], dist_fact);
            }
         }

         gain *= distfn(dist, refdist, maxdist, rolloff, ss, 1.0f);

         /*
          * audio cone recalculaion
          */
         if (_PROP3D_CONE_IS_DEFINED(ep3d))
         {
            float inner_vec, tmp = -ep3d->m_matrix[DIR_BACK][2];

            inner_vec = _FILTER_GETD3D(src, ANGULAR_FILTER, AAX_INNER_ANGLE);
            if (tmp < inner_vec)
            {
               float outer_vec, outer_gain;
               outer_vec = _FILTER_GETD3D(src, ANGULAR_FILTER, AAX_OUTER_ANGLE);
               outer_gain = _FILTER_GETD3D(src, ANGULAR_FILTER, AAX_OUTER_GAIN);
               if (outer_vec < tmp)
               {
                  tmp -= inner_vec;
                  tmp *= (outer_gain - 1.0f);
                  tmp /= (outer_vec - inner_vec);
                  cone_volume = (1.0f + tmp);
               } else {
                  cone_volume = outer_gain;
               }
            }
         }
         gain *= cone_volume;
      }
      min = _FILTER_GET2D(src, VOLUME_FILTER, AAX_MIN_GAIN);
      max = _FILTER_GET2D(src, VOLUME_FILTER, AAX_MAX_GAIN);
      ep2d->final.gain = _MINMAX(gain, min, max);
   }
}
