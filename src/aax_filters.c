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

#include <math.h>
#include <assert.h>

#include <aax.h>

#include "api.h"

#define WRITEFN		0
#define EPS		1e-5
#define READFN		!WRITEFN

typedef struct {
  enum aaxFilterType type;
  int pos;
} _flt_cvt_tbl_t;
typedef struct {
   vec4 min;
   vec4 max;
} _flt_minmax_tbl_t;
typedef float (*cvtfn_t)(float);

static const _flt_cvt_tbl_t _flt_cvt_tbl[AAX_FILTER_MAX];
static const _flt_minmax_tbl_t _flt_minmax_tbl[AAX_FILTER_MAX];
static cvtfn_t get_cvtfn(enum aaxFilterType, int, int, char);

aaxFilter
aaxFilterCreate(aaxConfig config, enum aaxFilterType type)
{
   _handle_t *handle = get_handle(config);
   aaxFilter rv = NULL;
   if (handle)
   {
      unsigned int size = sizeof(_filter_t);
     _filter_t* flt;

      switch (type)
      {
      case AAX_TIMED_GAIN_FILTER:		/* three slots */
         size += (_MAX_ENVELOPE_STAGES/2)*sizeof(_oalRingBufferFilterInfo);
         break;
      case AAX_EQUALIZER:			/* two slots */
      case AAX_GRAPHIC_EQUALIZER:
         size += EQUALIZER_MAX*sizeof(_oalRingBufferFilterInfo);
         break;
      default:					/* one slot */
         size += sizeof(_oalRingBufferFilterInfo);
         break;
      }

      flt = calloc(1, size);
      if (flt)
      {
         char *ptr;
         int i;

         flt->id = FILTER_ID;
         flt->state = AAX_FALSE;
         flt->info = handle->info;

         ptr = (char*)flt + sizeof(_filter_t);
         flt->slot[0] = (_oalRingBufferFilterInfo*)ptr;
         flt->pos = _flt_cvt_tbl[type].pos;
         flt->type = type;

         size = sizeof(_oalRingBufferFilterInfo);
         switch (type)
         {
         case AAX_EQUALIZER:
         case AAX_GRAPHIC_EQUALIZER:
            flt->slot[1] = (_oalRingBufferFilterInfo*)(ptr + size);
            memcpy(flt->slot[1], &_aaxDefault2dProps.filter[flt->pos], size);
            /* break not needed */
         case AAX_VOLUME_FILTER:
         case AAX_TREMOLO_FILTER:
         case AAX_FREQUENCY_FILTER:
            memcpy(flt->slot[0], &_aaxDefault2dProps.filter[flt->pos], size);
            break;
         case AAX_TIMED_GAIN_FILTER:
            for (i=0; i<_MAX_ENVELOPE_STAGES/2; i++)
            {
               flt->slot[i] = (_oalRingBufferFilterInfo*)(ptr + i*size);
               memcpy(flt->slot[i], &_aaxDefault2dProps.filter[flt->pos], size);
            }
            break;
         case AAX_ANGULAR_FILTER:
         case AAX_DISTANCE_FILTER:
            memcpy(flt->slot[0],&_aaxDefault3dProps.filter[flt->pos], size);
            break;
         default:
            _aaxErrorSet(AAX_INVALID_ENUM);
            free(flt);
            flt = NULL;
         }
      }
      else {
         _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
      }
      rv = (aaxFilter)flt;
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   return rv;
}

int
aaxFilterDestroy(aaxFilter f)
{
   _filter_t* filter = get_filter(f);
   int rv = AAX_FALSE;
   if (filter)
   {
      /*
       * If a filter is applied to an emitter, the scenery or an audioframe
       * filter->slot[0]->data will be copied and set to NULL. If not applied
       * we need to free the located memory ourselves.
       */
      switch (filter->type)
      {
      case AAX_EQUALIZER:
      case AAX_GRAPHIC_EQUALIZER:
         filter->slot[1]->data = NULL;
         /* break not needed */
      case AAX_TIMED_GAIN_FILTER:
      case AAX_TREMOLO_FILTER:
      case AAX_FREQUENCY_FILTER:
         free(filter->slot[0]->data);
         filter->slot[0]->data = NULL;
         break;
      default:
         break;
      }
      free(filter);
      rv = AAX_TRUE;
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   return rv;
}

aaxFilter
aaxFilterSetSlot(aaxFilter f, unsigned slot, int ptype, float p1, float p2, float p3, float p4)
{
   aaxVec4f v;
   v[0] = p1, v[1] = p2; v[2] = p3, v[3] = p4;
   return aaxFilterSetSlotParams(f, slot, ptype, v);
}

aaxFilter
aaxFilterSetSlotParams(aaxFilter f, unsigned slot, int ptype, aaxVec4f p)
{
   _filter_t* filter = get_filter(f);
   aaxFilter rv = AAX_FALSE;
   if (filter && p)
   {
      if ((slot < _MAX_SLOTS) && filter->slot[slot])
      {
         int i, type = filter->type;
         for (i=0; i<4; i++)
         {
            if (!is_nan(p[i]))
            {
               float min = _flt_minmax_tbl[type].min[i];
               float max = _flt_minmax_tbl[type].max[i];
               cvtfn_t cvtfn = get_cvtfn(filter->type, ptype, WRITEFN, i);
               filter->slot[slot]->param[i] = _MINMAX(cvtfn(p[i]), min, max);
            }
         }
         if TEST_FOR_TRUE(filter->state) {
            rv = aaxFilterSetState(filter, AAX_TRUE);
         } else {
            rv = filter;
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   return rv;
}

int
aaxFilterSetParam(const aaxFilter f, int param, int ptype, float value)
{
   _filter_t* filter = get_filter(f);
   int rv = AAX_FALSE;
   if (filter && !is_nan(value))
   {
      unsigned slot = param >> 4;
      if ((slot < _MAX_SLOTS) && filter->slot[slot])
      {
         param &= 0xF;
         if ((param >= 0) && (param < 4))
         {
            cvtfn_t cvtfn = get_cvtfn(filter->type, ptype, WRITEFN, param);
            filter->slot[slot]->param[param] = cvtfn(value);
            rv = AAX_TRUE;
         }
         else {
            _aaxErrorSet(AAX_INVALID_PARAMETER + 1);
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   return rv;
}

aaxFilter
aaxFilterSetState(aaxFilter f, int state)
{
   _filter_t* filter = get_filter(f);
   aaxFilter rv = NULL;
   if (filter && filter->info)
   {
      switch(filter->type)
      {
      case AAX_GRAPHIC_EQUALIZER:
#if !ENABLE_LITE
         if EBF_VALID(filter)
         {
            if TEST_FOR_TRUE(state)
            {
               _oalRingBufferEqualizerInfo *eq = filter->slot[0]->data;
               if (eq == NULL)
               {
                  eq = calloc(1, sizeof(_oalRingBufferEqualizerInfo));
                  filter->slot[EQUALIZER_LF]->data = eq;
               }

               if (eq)
               {
                  float gain_hf=filter->slot[EQUALIZER_HF]->param[AAX_EQ_BAND3];
                  float gain_lf=filter->slot[EQUALIZER_HF]->param[AAX_EQ_BAND2];
                  int s = EQUALIZER_HF, b = AAX_EQ_BAND2;
                  float fband = logf(22.05f)/7.0f;

                  eq = filter->slot[0]->data;
                  do
                  {
                     _oalRingBufferFreqFilterInfo *flt;
                     float *cptr, fc, k, gain;
                     int pos = s*4+b;

                     flt = &eq->band[pos];
                     cptr = flt->coeff;

                     /* gain_lf can never get below 0.001f */
                     gain = (pos == 7) ? gain_lf : gain_hf/gain_lf;
                     fc = expf((float)pos*fband)*100.0f;
                     k = 1.0f;

                     iir_compute_coefs(fc, filter->info->frequency, cptr, &k);
                     flt->lf_gain = (pos == 7) ? gain_hf : 1.0f;
                     flt->hf_gain = gain;
                     flt->k = k;

                     if (--b < 0)
                     {
                        b += 4;
                        s--;
                     }

                     gain_hf = gain_lf;
                     gain_lf = filter->slot[s]->param[b];
                  }
                  while (s && b);
               }
               else _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
            }
            else {
               filter->slot[0]->data = NULL;
            }
         }
#endif
         break;
      case AAX_EQUALIZER:
#if !ENABLE_LITE
         if EBF_VALID(filter)
         {
            if TEST_FOR_TRUE(state)
            {
               _oalRingBufferFreqFilterInfo *flt = filter->slot[0]->data;
               if (flt == NULL)
               {
                  char *ptr;
                  flt=calloc(EQUALIZER_MAX,sizeof(_oalRingBufferFreqFilterInfo));
                  filter->slot[EQUALIZER_LF]->data = flt;

                  ptr = (char*)flt + sizeof(_oalRingBufferFreqFilterInfo);
                  flt = (_oalRingBufferFreqFilterInfo*)ptr;
                  filter->slot[EQUALIZER_HF]->data = flt;
               }

               if (flt)
               {
                  float *cptr, fc, k;

                  /* LF frequency setup */
                  flt = filter->slot[EQUALIZER_LF]->data;
                  fc = filter->slot[EQUALIZER_LF]->param[AAX_CUTOFF_FREQUENCY];
                  cptr = flt->coeff;
                  k = 1.0f;

                  iir_compute_coefs(fc, filter->info->frequency, cptr, &k);
                  flt->lf_gain = filter->slot[EQUALIZER_LF]->param[AAX_LF_GAIN];
                  flt->hf_gain = filter->slot[EQUALIZER_LF]->param[AAX_HF_GAIN];
                  flt->k = k;

                  /* HF frequency setup */
                  flt = filter->slot[EQUALIZER_HF]->data;
                  fc = filter->slot[EQUALIZER_HF]->param[AAX_CUTOFF_FREQUENCY];
                  cptr = flt->coeff;
                  k = 1.0f;

                  iir_compute_coefs(fc, filter->info->frequency, cptr, &k);
                  flt->lf_gain = filter->slot[EQUALIZER_HF]->param[AAX_LF_GAIN];
                  flt->hf_gain = filter->slot[EQUALIZER_HF]->param[AAX_HF_GAIN];
                  flt->k = k;
               }
               else _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
            }
            else {
               filter->slot[0]->data = NULL;
            }
         }
#endif
         break;
      case AAX_TIMED_GAIN_FILTER:
#if !ENABLE_LITE
         if EBF_VALID(filter)
         {
            if TEST_FOR_TRUE(state)
            {
               _oalRingBufferEnvelopeInfo* env = filter->slot[0]->data;
               if (env == NULL)
               {
                  env =  calloc(1, sizeof(_oalRingBufferEnvelopeInfo));
                  filter->slot[0]->data = env;
               }

               if (env)
               {
                  float nextval = filter->slot[0]->param[AAX_LEVEL0];
                  float refresh = filter->info->refresh_rate;
                  float timestep = 1.0f / refresh;
                  int i;

                  env->value = nextval;
                  env->max_stages = _MAX_ENVELOPE_STAGES;
                  for (i=0; i<_MAX_ENVELOPE_STAGES/2; i++)
                  {
                     float dt, value = nextval;
                     uint32_t max_pos;

                     max_pos = (uint32_t)-1;
                     dt = filter->slot[i]->param[AAX_TIME0];
                     if (dt != MAXFLOAT)
                     {
                        if (dt < timestep && dt > EPS) dt = timestep;
                        max_pos = rintf(dt * refresh);
                     }
                     if (max_pos == 0)
                     {
                        env->max_stages = 2*i;
                        break;
                     }

                     nextval = filter->slot[i]->param[AAX_LEVEL1];
                     if (nextval == 0.0f) nextval = -1e-2;
                     env->step[2*i] = (nextval - value)/max_pos;
                     env->max_pos[2*i] = max_pos;

                     /* prevent a core dump for accessing an illegal slot */
                     if (i == (_MAX_ENVELOPE_STAGES/2)-1) break;
                 
                     max_pos = (uint32_t)-1;
                     dt = filter->slot[i]->param[AAX_TIME1];
                     if (dt != MAXFLOAT)
                     {
                        if (dt < timestep && dt > EPS) dt = timestep;
                        max_pos = rintf(dt * refresh);
                     } 
                     if (max_pos == 0)
                     {
                        env->max_stages = 2*i+1;
                        break;
                     }

                     value = nextval;
                     nextval = filter->slot[i+1]->param[AAX_LEVEL0];
                     if (nextval == 0.0f) nextval = -1e-2;
                     env->step[2*i+1] = (nextval - value)/max_pos;
                     env->max_pos[2*i+1] = max_pos;
                  }
               }
               else _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
            }
            else {
               filter->slot[0]->data = NULL;
            }
         }
#endif
         break;
      case AAX_TREMOLO_FILTER:
#if !ENABLE_LITE
         if EBF_VALID(filter)
         {
            switch (state)
            {
            case AAX_TRIANGLE_WAVE:
            case AAX_SINE_WAVE:
            case AAX_SQUARE_WAVE:
            case AAX_SAWTOOTH_WAVE:
            {
               _oalRingBufferLFOInfo* lfo = filter->slot[0]->data;
               if (lfo == NULL)
               {
                  lfo = malloc(sizeof(_oalRingBufferLFOInfo));
                  filter->slot[0]->data = lfo;
               }

               if (lfo)
               {
                  float depth = filter->slot[0]->param[AAX_LFO_DEPTH];

                  lfo->step = 2.0f*depth;
                  lfo->step *= filter->slot[0]->param[AAX_LFO_FREQUENCY];
                  lfo->step /= filter->info->refresh_rate;
                  lfo->value = 1.0f - depth;
                  lfo->min = 1.0f - depth;
                  lfo->max = 1.0f;
                  if (depth > 0.01f)
                  {
                     switch (state)
                     {
                     case AAX_TRIANGLE_WAVE:
                        lfo->get = _oalRingBufferLFOGetTriangle;
                        break;
                     case AAX_SINE_WAVE:
                        lfo->get = _oalRingBufferLFOGetSine;
                        break;
                     case AAX_SQUARE_WAVE:
                        lfo->get = _oalRingBufferLFOGetSquare;
                        break;
                     case AAX_SAWTOOTH_WAVE:
                        lfo->get = _oalRingBufferLFOGetSawtooth;
                        lfo->step *= 0.5f;
                        break;
                     default:
                        break;
                     }
                  } else {
                     lfo->get = _oalRingBufferLFOGetFixedValue;
                  }
               }
               else _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
               break;
            }
            case AAX_FALSE:
               filter->slot[0]->data = NULL;
               break;
            default:
               _aaxErrorSet(AAX_INVALID_PARAMETER);
               break;
            }
         }
#endif
         break;
      case AAX_FREQUENCY_FILTER:
         if TEST_FOR_TRUE(state)
         {
            _oalRingBufferFreqFilterInfo *flt = filter->slot[0]->data;
            if (flt == NULL)
            {
               flt = calloc(1, sizeof(_oalRingBufferFreqFilterInfo));
               filter->slot[0]->data = flt;
            }

            if (flt)
            {
               float fc = filter->slot[0]->param[AAX_CUTOFF_FREQUENCY];
               float *cptr = flt->coeff;
               float frequency = 48000.0f; 
               float k = 1.0f;

               if (filter->info) {
                  frequency = filter->info->frequency;
               }
               iir_compute_coefs(fc, frequency, cptr, &k);
               flt->lf_gain = filter->slot[0]->param[AAX_LF_GAIN];
               flt->hf_gain = filter->slot[0]->param[AAX_HF_GAIN];
               flt->k = k;
            }
            else _aaxErrorSet(AAX_INSUFFICIENT_RESOURCES);
         }
         else {
            filter->slot[0]->data = NULL;
         }
         break;
      case AAX_DISTANCE_FILTER:
         if (state < AAX_AL_DISTANCE_MODEL_MAX)
         {
            int pos = 0;
            if ((state >= AAX_AL_INVERSE_DISTANCE)
                && (state < AAX_AL_DISTANCE_MODEL_MAX))
            {
               pos = state - AAX_AL_INVERSE_DISTANCE;
               filter->slot[0]->data = _oalRingBufferALDistanceFunc[pos];
            }
            else if ((state & ~0x80) < AAX_DISTANCE_MODEL_MAX)
            {
               pos = state & ~0x80;
               if (state & 0x80) {	/* distance delay enabled */
                  filter->slot[0]->param[AAX_ROLLOFF_FACTOR] *= -1;
               }
               filter->slot[0]->data = _oalRingBufferDistanceFunc[pos];
            }
            else _aaxErrorSet(AAX_INVALID_PARAMETER);
         }
         else _aaxErrorSet(AAX_INVALID_PARAMETER);
         break;
      default:
         _aaxErrorSet(AAX_INVALID_STATE);
      }
      rv = filter;
   }
   else if (!filter) {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   } else { /* !filter->info */
      _aaxErrorSet(AAX_INVALID_STATE);
   }
   return rv;
}

float
aaxFilterGetParam(const aaxFilter f, int param, int ptype)
{
   _filter_t* filter = get_filter(f);
   float rv = 0.0f;
   if (filter)
   {
      int slot = param >> 4;
      if ((slot < _MAX_SLOTS) && filter->slot[slot])
      {
         param &= 0xF;
         if ((param >= 0) && (param < 4))
         {
            cvtfn_t cvtfn = get_cvtfn(filter->type, ptype, READFN, param);
            rv = cvtfn(filter->slot[slot]->param[param]);
         }
         else {
            _aaxErrorSet(AAX_INVALID_PARAMETER + 1);
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   return rv;
}

aaxFilter
aaxFilterGetSlot(const aaxFilter f, unsigned slot, int ptype, float* p1, float* p2, float* p3, float* p4)
{
   aaxVec4f v;
   aaxFilter rv = aaxEffectGetSlotParams(f, slot, ptype, v);
   if(p1) *p1 = v[0];
   if(p2) *p2 = v[1];
   if(p3) *p3 = v[2];
   if(p4) *p4 = v[3];
   return rv;
}

aaxFilter
aaxFilterGetSlotParams(const aaxFilter f, unsigned slot, int ptype, aaxVec4f p)
{
   aaxFilter rv = AAX_FALSE;
   _filter_t* filter = get_filter(f);
   if (filter && p)
   {
      if ((slot < _MAX_SLOTS) && filter->slot[slot])
      {
         int i;
         for (i=0; i<4; i++)
         {
            cvtfn_t cvtfn = get_cvtfn(filter->type, ptype, READFN, i);
            p[i] = cvtfn(filter->slot[slot]->param[i]);
         }
         rv = filter;
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_HANDLE);
   }
   return rv;
}

/* -------------------------------------------------------------------------- */

static const _flt_cvt_tbl_t _flt_cvt_tbl[AAX_FILTER_MAX] = 
{
  { AAX_FILTER_NONE,        MAX_STEREO_FILTER },
  { AAX_EQUALIZER,          FREQUENCY_FILTER },
  { AAX_VOLUME_FILTER,      VOLUME_FILTER },
  { AAX_TREMOLO_FILTER,     TREMOLO_FILTER },
  { AAX_TIMED_GAIN_FILTER,  TIMED_GAIN_FILTER },
  { AAX_ANGULAR_FILTER,     ANGULAR_FILTER },
  { AAX_DISTANCE_FILTER,    DISTANCE_FILTER },
  { AAX_FREQUENCY_FILTER,   FREQUENCY_FILTER },
  { AAX_GRAPHIC_EQUALIZER,  FREQUENCY_FILTER }
};

/* see above for the proper sequence */
static const _flt_minmax_tbl_t _flt_minmax_tbl[AAX_FILTER_MAX] =
{   /* min[4] */	  /* max[4] */
  /* AAX_FILTER_NONE      */
  { {  0.0f,  0.0f, 0.0f, 0.0f }, {     0.0f,     0.0f,  0.0f,     0.0f } },
  /* AAX_EQUALIZER        */
  { { 10.0f,  0.0f, 0.0f, 0.0f }, { 22050.0f,    10.0f, 10.0f,     0.0f } },
  /* AAX_VOLUME_FILTER    */
  { {  0.0f,  0.0f, 0.0f, 0.0f }, {     1.0f,     1.0f, 10.0f,     0.0f } },
  /* AAX_TREMOLO_FILTER   */
  { {  0.0f, 0.01f, 0.0f, 0.0f }, {     0.0f,    10.0f,  1.0f,     1.0f } },
  /* AAX_TIMED_GAIN_FILTER */
  { {  0.0f,  0.0f, 0.0f, 0.0f }, {     4.0f, MAXFLOAT,  4.0f, MAXFLOAT } },
  /* AAX_ANGULAR_FILTER   */
  { { -1.0f, -1.0f, 0.0f, 0.0f }, {     1.0f,     1.0f,  1.0f,     0.0f } },
  /* AAX_DISTANCE_FILTER  */
  { {  0.0f,  0.0f, 0.0f, 0.0f }, { MAXFLOAT, MAXFLOAT,  1.0f,     0.0f } },
  /* AAX_FREQUENCY_FILTER */
  { { 10.0f,  0.0f, 0.0f, 0.0f }, { 22050.0f,    10.0f, 10.0f,     0.0f } },
  /* AAX_GRAPHIC_EQUALIZER */
  { {0.001f,0.001f,0.001f,0.001f }, {   1.0f,     1.0f,  1.0f,     1.0f } }
};

/* internal use only, used by aaxdefs.h */
aaxFilter
aaxFilterApply(aaxFilterFn fn, void *handle, aaxFilter f)
{
   if (f)
   {
      if (!fn(handle, f))
      {
         aaxFilterDestroy(f);
         f = NULL;
      }
   }
   return f;
}

float
aaxFilterApplyParam(const aaxFilter f, int s, int p, int ptype)
{
   float rv = 0.0f;
   if ((p >= 0) && (p < 4))
   {
      _filter_t* filter = get_filter(f);
      if (filter)
      {
         cvtfn_t cvtfn = get_cvtfn(filter->type, ptype, READFN, p);
         rv = cvtfn(filter->slot[0]->param[p]);
         free(filter);
      }
   }
   return rv;
}

static float _lin(float v) { return v; }
static float _lin2db(float v) { return 20.0f*log(v); }
static float _db2lin(float v) { return _MINMAX(pow(10.0f,v/20.0f),0.0f,10.0f); }
static float _rad2deg(float v) { return v*GMATH_RAD_TO_DEG; }
static float _deg2rad(float v) { return fmod(v, 360.0f)*GMATH_DEG_TO_RAD; }
static float _cos_deg2rad_2(float v) { return cos(_deg2rad(v)/2); }
static float _2acos_rad2deg(float v) { return 2*acos(_rad2deg(v)); }
static float _cos_2(float v) { return cos(v/2); }
static float _2acos(float v) { return 2*acos(v); }

_filter_t*
new_filter_handle(_aaxMixerInfo* info, enum aaxFilterType type, _oalRingBuffer2dProps* p2d, _oalRingBuffer3dProps* p3d)
{
   _filter_t* rv = NULL;
   if (type < AAX_FILTER_MAX)
   {
      unsigned int size = sizeof(_filter_t);

      switch (type)
      {
      case AAX_TIMED_GAIN_FILTER:		/* three slots */
         size += (_MAX_ENVELOPE_STAGES/2)*sizeof(_oalRingBufferFilterInfo);
         break;
      case AAX_EQUALIZER:                       /* two slots */
      case AAX_GRAPHIC_EQUALIZER:
         size += EQUALIZER_MAX*sizeof(_oalRingBufferFilterInfo);
         break;
      default:					/* one slot */
         size += sizeof(_oalRingBufferFilterInfo);
         break;
      }

      rv = calloc(1, size);
      if (rv)
      {
         char *ptr = (char*)rv + sizeof(_filter_t);

         rv->id = FILTER_ID;
         rv->info = info;
         rv->slot[0] = (_oalRingBufferFilterInfo*)ptr;
         rv->pos = _flt_cvt_tbl[type].pos;
         rv->type = type;

         size = sizeof(_oalRingBufferFilterInfo);
         switch (type)
         {
         case AAX_EQUALIZER:
         case AAX_GRAPHIC_EQUALIZER:
            rv->slot[1] = (_oalRingBufferFilterInfo*)(ptr + size);
            memcpy(rv->slot[1], &p2d->filter[rv->pos], size);
            rv->slot[1]->data = NULL;
            /* break not needed */
         case AAX_VOLUME_FILTER:
         case AAX_TREMOLO_FILTER:
         case AAX_FREQUENCY_FILTER:
            memcpy(rv->slot[0], &p2d->filter[rv->pos], size);
            rv->slot[0]->data = NULL;
            break;
         case AAX_TIMED_GAIN_FILTER:
         {
            _oalRingBufferEnvelopeInfo *env;
            unsigned int no_steps;
            float dt, value;
            int i, stages;

            env = (_oalRingBufferEnvelopeInfo*)p2d->filter[rv->pos].data;
            memcpy(rv->slot[0], &p2d->filter[rv->pos], size);
            rv->slot[0]->data = NULL;

            i = 0;
            if (env->max_pos[1] > env->max_pos[0]) i = 1;
            dt = p2d->filter[rv->pos].param[2*i+1] / env->max_pos[i];

            no_steps = env->max_pos[1];
            value = p2d->filter[rv->pos].param[AAX_LEVEL1];
            value += env->step[1] * no_steps;

            stages = _MIN(1+env->max_stages/2, _MAX_ENVELOPE_STAGES/2);
            for (i=1; i<stages; i++)
            {
               _oalRingBufferFilterInfo* slot;

               slot = (_oalRingBufferFilterInfo*)(ptr + i*size);
               rv->slot[i] = slot;

               no_steps = env->max_pos[2*i];
               slot->param[0] = value;
               slot->param[1] = no_steps * dt;

               value += env->step[2*i] * no_steps;
               no_steps = env->max_pos[2*i+1];
               slot->param[2] = value;
               slot->param[3] = no_steps * dt;

               value += env->step[2*i+1] * no_steps;
            }
            break;
         }
         case AAX_ANGULAR_FILTER:
         case AAX_DISTANCE_FILTER:
            memcpy(rv->slot[0], &p3d->filter[rv->pos], size);
            break;
         default:
            break;
         }
      }
   }
   return rv;
}

_filter_t*
get_filter(aaxFilter f)
{
   _filter_t* rv = (_filter_t*)f;
   if (rv && rv->id == FILTER_ID) {
      return rv;
   }
   return NULL;
}

static cvtfn_t
get_cvtfn(enum aaxFilterType type, int ptype, int mode, char param)
{
   cvtfn_t rv = _lin;
   switch (type)
   {
   case AAX_TIMED_GAIN_FILTER:
   case AAX_VOLUME_FILTER:
      if (ptype == AAX_LOGARITHMIC)
      {
         if (mode == WRITEFN) {
            rv = _lin2db;
         } else {
            rv = _db2lin;
         }
      }
      break;
   case AAX_FREQUENCY_FILTER:
      if (param > 0)
      {
         if (ptype == AAX_LOGARITHMIC)
         {
            if (mode == WRITEFN) {
               rv = _lin2db;
            } else {
               rv = _db2lin;
            }
         }
      }
      break;
   case AAX_ANGULAR_FILTER:
      if (param < 2)
      {
         if (ptype == AAX_DEGREES)
         {
            if (mode == WRITEFN) {
               rv = _cos_deg2rad_2;
            } else {
               rv = _2acos_rad2deg;
            }
         }
         else 
         {
            if (mode == WRITEFN) {
               rv = _cos_2;
            } else {
               rv = _2acos;
            }
         }
      }
      break;
   default:
      break;
   }
   return rv;
}
