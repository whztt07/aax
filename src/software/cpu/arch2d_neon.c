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

#include "software/rbuf_int.h"
#include "arch2d_simd.h"

#ifdef __ARM_NEON__

/*
 * http://gcc.gnu.org/projects/prefetch.html
 *
 * ARMv7 has (from ARM Architecture Reference Manual):
 *  PLD data preload with intent to read
 *  PLDW data preload with intent to write
 *  PLI instruction preload
 *
 * ARMv8 AArch64 has a PRFM instruction with the following hints
 * (from LLVM and binutils code):
 *  PLD (data load), PLI (instruction), PST (data store)
 *  Level l1, l2, l3
 *  KEEP (retained), STRM (streaming)
 *
 * Note: In NEON, the SIMD supports up to 16 operations at the same time.
 */

float
fast_sin_neon(float x)
{
   x *= GMATH_1_PI;
   x = fmodf(x, 2.0f) - 1.0f;
   return -4.0f*(x - x*fabsf(x));
}

void
_batch_get_average_rms_neon(const_float32_ptr s, size_t num, float *rms, float *peak)
{
   double rms_total = 0.0;
   float peak_cur = 0.0f;
   size_t i, step, total;

   *rms = *peak = 0;

   if (!num) return;

   total = num;
   step = 4*sizeof(float32x4_t)/sizeof(float);

   i = num/step;
   if (i)
   {
      float32x4x4_t nfr4;
      union {
         float32x4x4_t x;
         float f[4][4];
      } v;
      int j, k;

      num -= i*step;
      do
      {
         nfr4 = vld4q_f32(s);
         s += 4*4;

         nfr4.val[0] = vmulq_f32(nfr4.val[0], nfr4.val[0]);
         nfr4.val[1] = vmulq_f32(nfr4.val[1], nfr4.val[1]);
         nfr4.val[2] = vmulq_f32(nfr4.val[2], nfr4.val[2]);
         nfr4.val[3] = vmulq_f32(nfr4.val[3], nfr4.val[3]);

         vst4q_f32((float*)v.f, nfr4);

         for (j=0; j<4; ++j)
         {
            for (k=0; k<4; ++k)
            {
               float val = v.f[j][k];

               rms_total += val;
               if (val > peak_cur) peak_cur = val;
            }
         }
      }
      while(--i);
   }

   if (num)
   {
      i = num;
      do
      {
         float samp = *s++;            // rms
         float val = samp*samp;
         rms_total += val;
         if (val > peak_cur) peak_cur = val;
      }
      while (--i);
   }

   *rms = (float)sqrt(rms_total/total);
   *peak = sqrtf(peak_cur);
}

void
_batch_cvtps24_24_neon(void_ptr dst, const_void_ptr src, size_t num)
{
   int32_t *s = (int32_t*)src;
   float *d = (float*)dst;
   size_t i, step;

   assert(s != 0);
   assert(d != 0);

   if (!num) return;

   step = sizeof(int32x4x4_t)/sizeof(int32_t);

   i = num/step;
   num -= i*step;
   if (i)
   {
      int32x4x4_t nir4;
      float32x4x4_t nfr4d;

      do
      {
         nir4 = vld4q_s32(s);
         s += 4*4;

         nfr4d.val[0] = vcvtq_f32_s32(nir4.val[0]);
         nfr4d.val[1] = vcvtq_f32_s32(nir4.val[1]);
         nfr4d.val[2] = vcvtq_f32_s32(nir4.val[2]);
         nfr4d.val[3] = vcvtq_f32_s32(nir4.val[3]);

         vst4q_f32(d, nfr4d);
         d += step;
      }
      while(--i);

      if (num)
      {
         i = num;
         do {
            *d++ = (float)*s++;
         } while (--i);
      }
   }
}

void
_batch_cvt24_ps24_neon(void_ptr dst, const_void_ptr src, size_t num)
{
   int32_t *d = (int32_t*)dst;
   float *s = (float*)src;
   size_t i, step;

   assert(s != 0);
   assert(d != 0);

   if (!num) return;

   step = sizeof(float32x4x4_t)/sizeof(float32_t);

   i = num/step;
   num -= i*step;
   if (i)
   {
      float32x4x4_t nir4;
      int32x4x4_t nfr4d;

      do
      {
         nir4 = vld4q_f32(s);
         s += 4*4;

         nfr4d.val[0] = vcvtq_s32_f32(nir4.val[0]);
         nfr4d.val[1] = vcvtq_s32_f32(nir4.val[1]);
         nfr4d.val[2] = vcvtq_s32_f32(nir4.val[2]);
         nfr4d.val[3] = vcvtq_s32_f32(nir4.val[3]);

         vst4q_s32(d, nfr4d);
         d += step;
      }
      while(--i);

      if (num)
      {
         i = num;
         do {
            *d++ = (int32_t)*s++;
         } while (--i);
      }
   }
}

static void
_batch_iadd_neon(int32_ptr d, const_int32_ptr src, size_t num)
{
   int32_t *s = (int32_t *)src;
   size_t i, size, step;

   step = sizeof(int32x4x4_t)/sizeof(int32_t);

   i = size = num/step;
   if (i)
   {
      do
      {
         int32x4x4_t nir4d, nir4s;

         nir4s = vld4q_s32(s);
         nir4d = vld4q_s32(d);
         s += 4*4;

         nir4d.val[0] = vaddq_s32(nir4d.val[0], nir4s.val[0]);
         nir4d.val[1] = vaddq_s32(nir4d.val[1], nir4s.val[1]);
         nir4d.val[2] = vaddq_s32(nir4d.val[2], nir4s.val[2]);
         nir4d.val[3] = vaddq_s32(nir4d.val[3], nir4s.val[3]);

         vst4q_s32(d, nir4d);
         d += 4*4;
      }
      while(--i);
   }

   i = num - size*step;
   if (i) {
      do {
         *d++ += *s++;
      } while(--i);
   }
}

void
_batch_imadd_neon(int32_ptr dst, const_int32_ptr src, size_t num, float v, float vstep)
{
   int32_ptr d = (int32_ptr)dst;
   int32_ptr s = (int32_t *)src;
   size_t i, size, step;

   if (!num || (v <= LEVEL_128DB && vstep <= LEVEL_128DB)) return;
   if (fabsf(v - 1.0f) < LEVEL_96DB && vstep <=  LEVEL_96DB) {
      _batch_iadd_neon(dst, src, num);
      return;
   }

   step = sizeof(int32x4x4_t)/sizeof(int32_t);

   vstep *= step;                                 /* 16 samples at a time */
   i = size = num/step;
   if (i)
   {
      float32x4_t tv = vdupq_n_f32(v);
      int32x4x4_t nir4;

      do
      {
         float32x4_t nfr[4];
         int32x4x4_t nnir4;

         nir4 = vld4q_s32(s);
         nfr[0] = vcvtq_f32_s32(nir4.val[0]);
         nfr[1] = vcvtq_f32_s32(nir4.val[1]);
         nfr[2] = vcvtq_f32_s32(nir4.val[2]);
         nfr[3] = vcvtq_f32_s32(nir4.val[3]);

         s += step;

         nfr[0] = vmulq_f32(nfr[0], tv);
         nfr[1] = vmulq_f32(nfr[1], tv);
         nfr[2] = vmulq_f32(nfr[2], tv);
         nfr[3] = vmulq_f32(nfr[3], tv);

         v += vstep;

         nir4 = vld4q_s32(d);
         nnir4.val[0] = vcvtq_s32_f32(nfr[0]);
         nnir4.val[1] = vcvtq_s32_f32(nfr[1]);
         nnir4.val[2] = vcvtq_s32_f32(nfr[2]);
         nnir4.val[3] = vcvtq_s32_f32(nfr[3]);

         d += step;

         nir4.val[0] = vaddq_s32(nir4.val[0], nnir4.val[0]);
         nir4.val[1] = vaddq_s32(nir4.val[1], nnir4.val[1]);
         nir4.val[2] = vaddq_s32(nir4.val[2], nnir4.val[2]);
         nir4.val[3] = vaddq_s32(nir4.val[3], nnir4.val[3]);
         vst4q_s32(d, nir4);

         tv = vdupq_n_f32(v);
      }
      while(--i);
   }

   i = num - size*step;
   if (i) {
      vstep /= 16;
      do {
         *d++ += *s++ * v;
         v += vstep;
      } while(--i);
   }
}

static void
_batch_fadd_neon(float32_ptr dst, const_float32_ptr src, size_t num)
{
   float32_ptr s = (float32_ptr)src;
   float32_ptr d = (float32_ptr)dst;
   size_t i, step;

   step = sizeof(float32x4x4_t)/sizeof(float);

   i = num/step;
   num -= i*step;
   if (i)
   {
      float32x4x4_t sfr4, dfr4;

      do
      {
         sfr4 = vld4q_f32(s);   // load s
         dfr4 = vld4q_f32(d);   // load d
         s += step;

         dfr4.val[0] = vaddq_f32(dfr4.val[0], sfr4.val[0]);
         dfr4.val[1] = vaddq_f32(dfr4.val[1], sfr4.val[1]);
         dfr4.val[2] = vaddq_f32(dfr4.val[2], sfr4.val[2]);
         dfr4.val[3] = vaddq_f32(dfr4.val[3], sfr4.val[3]);

         vst4q_f32(d, dfr4);    // store d
         d += step;
      }
      while(--i);
   }

   if (num) {
      i = num;
      do {
         *d++ += *s++;
      } while(--i);
   }
}

void
_batch_fmadd_neon(float32_ptr dst, const_float32_ptr src, size_t num, float v, float vstep)
{
   int need_step = (fabsf(vstep) <=  LEVEL_96DB) ? 0 : 1;
   float32_ptr s = (float32_ptr)src;
   float32_ptr d = (float32_ptr)dst;
   size_t i, step;

   if (!num || (v <= LEVEL_128DB && vstep <= LEVEL_128DB)) return;

   if (fabsf(v - 1.0f) < LEVEL_96DB && !need_step) {
      _batch_fadd_neon(dst, src, num);
      return;
   }

   if (need_step)
   {
      i = num;
      do {
         *d++ += *s++ * v;
         v += vstep;
      }
      while (--i);
      return;
   }

   step = sizeof(float32x4x4_t)/sizeof(float);

   i = num/step;
   num -= i*step;
   if (i)
   {
      float32x4x4_t sfr4, dfr4;
      float32x4_t tv;

      tv = vdupq_n_f32(v);
      do
      {
         sfr4 = vld4q_f32(s);   // load s
         dfr4 = vld4q_f32(d);   // load d

         s += step;

         dfr4.val[0] = vmlaq_f32(dfr4.val[0], sfr4.val[0], tv);
         dfr4.val[1] = vmlaq_f32(dfr4.val[1], sfr4.val[1], tv);
         dfr4.val[2] = vmlaq_f32(dfr4.val[2], sfr4.val[2], tv);
         dfr4.val[3] = vmlaq_f32(dfr4.val[3], sfr4.val[3], tv);

         vst4q_f32(d, dfr4);    // store d
         d += step;
      }
      while(--i);
   }

   if (num) {
      i = num;
      do {
         *d++ += *s++ * v;
      } while(--i);
   }
}

void
_batch_cvt24_16_neon(void_ptr dst, const_void_ptr src, size_t num)
{
// int32x4_t  vshlq_n_s32(int32x4_t a, __constrange(0,31) int b);
// int32x4_t *sptr = (int32x4_t*)src;
   int16_t* s = (int16_t*)src;
   int32_t *d = (int32_t*)dst;
   size_t i, step;

   if (!num) return;

   step = sizeof(int32x4x2_t)/sizeof(int32_t);
   
   i = num/step;
   num -= i*step;
   if (i)
   {
      int32x4_t nfr0, nfr1, nfr2, nfr3;
      int32x4x2_t nfr2x0, nfr2x1;
      int16x4x4_t nfr2x4;

      do
      {
         nfr2x4 = vld4_s16(s);

         /* widen 16-bit to 32-bit */
         nfr0 = vmovl_s16(nfr2x4.val[0]);
         nfr1 = vmovl_s16(nfr2x4.val[1]);
         nfr2 = vmovl_s16(nfr2x4.val[2]);
         nfr3 = vmovl_s16(nfr2x4.val[3]);

         s += step;

         /* shift from 16-bit to 24-bit */
         nfr2x0.val[0] = vshlq_n_s32(nfr0, 8);
         nfr2x0.val[1] = vshlq_n_s32(nfr1, 8);
         nfr2x1.val[0] = vshlq_n_s32(nfr2, 8);
         nfr2x1.val[1] = vshlq_n_s32(nfr3, 8);

         vst2q_s32(d, nfr2x0);
         vst2q_s32(d+step/2, nfr2x1);
         d += step;

      } while (--i);
   }

   if (num)
   {
      i = num;
      do {
         *d++ = *s++ >> 8;
      } while (--i);
   }
}

void
_batch_cvt16_24_neon(void_ptr dst, const_void_ptr sptr, size_t num)
{
   // int32x4_t  vshrq_n_s32(int32x4_t a, __constrange(1,32) int b);
   int32_t *s = (int32_t*)sptr;
   int16_t* d = (int16_t*)dst;
   size_t i, step;

   if (!num) return;

   step = sizeof(int32x4x2_t)/sizeof(int32_t);

   i = num/step;
   num -= i*step;
   if (i)
   {
      int32x4_t nfr0, nfr1, nfr2, nfr3;
      int32x4x2_t nfr2x0, nfr2x1;
      int16x4x4_t nfr2x4;

      do
      {
         nfr2x0 = vld2q_s32(s);
         nfr2x1 = vld2q_s32(s+step/2);

         /* shift from 24-bit to 16-bit */
         nfr0 = vshrq_n_s32(nfr2x0.val[0], 8);
         nfr1 = vshrq_n_s32(nfr2x0.val[1], 8);
         nfr2 = vshrq_n_s32(nfr2x1.val[0], 8);
         nfr3 = vshrq_n_s32(nfr2x1.val[1], 8);

         s += step;

         /* extract lower part */
         nfr2x4.val[0] = vmovn_s32(nfr0);
         nfr2x4.val[1] = vmovn_s32(nfr1);
         nfr2x4.val[2] = vmovn_s32(nfr2);
         nfr2x4.val[3] = vmovn_s32(nfr3);

         vst4_s16(d, nfr2x4);
         d += step;

      } while (--i);
   }

   if (num)
   {
      i = num;
      do {
         *d++ = *s++ >> 8;
      } while (--i);
   }
}

void
_batch_cvt16_intl_24_neon(void_ptr dst, const_int32_ptrptr src,
                                size_t offset, unsigned int tracks,
                                size_t num)
{
   int16_t* d = (int16_t*)dst;
   size_t i, step;
   int32_t *s1, *s2;

   if (!num) return;

   s1 = (int32_t *)src[0] + offset;
   s2 = (int32_t *)src[1] + offset;

   step = sizeof(int32x4x2_t)/sizeof(int32_t);

   i = num/step;
   num -= i*step;
   if (i)
   {
      int32x4x2_t nfr2x0, nfr2x1, nfr2x2, nfr2x3;
      int32x4_t nfr0, nfr1, nfr2, nfr3;
      int16x4x4_t nfr2x4;

      do
      {
         nfr2x0 = vld2q_s32(s1);
         nfr2x1 = vld2q_s32(s2);

         /* interleave s1 and s2 */
         nfr2x2 = vzipq_s32(nfr2x0.val[0], nfr2x1.val[0]);
         nfr2x3 = vzipq_s32(nfr2x0.val[1], nfr2x1.val[1]);

         s1 += step;

         /* shift from 24-bit to 16-bit */
         nfr0 = vshrq_n_s32(nfr2x2.val[0], 8);
         nfr1 = vshrq_n_s32(nfr2x2.val[1], 8);
         nfr2 = vshrq_n_s32(nfr2x3.val[0], 8);
         nfr3 = vshrq_n_s32(nfr2x3.val[1], 8);

         s2 += step;

         /* extract lower part */
         nfr2x4.val[0] = vmovn_s32(nfr0);
         nfr2x4.val[1] = vmovn_s32(nfr1);
         nfr2x4.val[2] = vmovn_s32(nfr2);
         nfr2x4.val[3] = vmovn_s32(nfr3);

         vst4_s16(d, nfr2x4);
         d += step;

      } while (--i);
   }

   if (num)
   {
      i = num;
      do
      {
         *d++ = *s1++ >> 8;
         *d++ = *s2++ >> 8;
      }
      while (--i);
   }
}

void
_batch_ema_iir_float_neon(float32_ptr d, const_float32_ptr sptr, size_t num, float *hist, float a1)
{
   if (num)
   {
      float32_ptr s = (float32_ptr)sptr;
      size_t i = num;
      float smp;

      smp = *hist;
      do
      {
         smp += a1*(*s++ - smp);
         *d++ = smp;
      }
      while (--i);
      *hist = smp;
   }
}

void
_batch_freqfilter_neon(int32_ptr dptr, const_int32_ptr sptr, int t, size_t num, void *flt)
{
   _aaxRingBufferFreqFilterData *filter = (_aaxRingBufferFreqFilterData*)flt;
   const_int32_ptr s = sptr;

   if (num)
   {
      float32x4_t c, h;
      float32x2_t h2;
      float k, *cptr, *hist;
      int stages;

      cptr = filter->coeff;
      hist = filter->freqfilter_history[t];
      stages = filter->no_stages;
      if (!stages) stages++;

      if (filter->state == AAX_BESSEL) {
         k = filter->k * (filter->high_gain - filter->low_gain);
      } else {
         k = filter->k * filter->high_gain;
      }

      do
      {
         int32_ptr d = dptr;
         size_t i = num;

         c = vld1q_f32(cptr);

         h2 = vld1_f32(hist);
         h = vcombine_f32(h2, h2);

         do
         {
            float32x4_t pz;
            float32x2_t half;
            float smp, tmp;

            smp = *s++ * k;

            pz = vmulq_f32(c, h); // poles and zeros
            half = vget_low_f32(pz);
            tmp = vget_lane_f32(half, 0) + vget_lane_f32(half, 1);

            hist[1] = hist[0];
            hist[0] = smp + tmp;
            h2 = vld1_f32(hist);
            h = vcombine_f32(h2, h2);

            half = vget_high_f32(pz);
            tmp = vget_lane_f32(half, 0) + vget_lane_f32(half, 1);
            *d++ = smp + tmp;
         }
         while (--i);

         hist += 2;
         cptr += 4;
         k = 1.0f;
         s = dptr;
      }
      while (--stages);
   }
}

void
_batch_freqfilter_float_neon(float32_ptr dptr, const_float32_ptr sptr, int t, size_t num, void *flt)
{
   _aaxRingBufferFreqFilterData *filter = (_aaxRingBufferFreqFilterData*)flt;
   const_float32_ptr s = sptr;

   if (num)
   {
      float32x4_t c, h;
      float32x2_t h2;
      float k, *cptr, *hist;
      int stages;

      cptr = filter->coeff;
      hist = filter->freqfilter_history[t];
      stages = filter->no_stages;
      if (!stages) stages++;

      if (filter->state == AAX_BESSEL) {
         k = filter->k * (filter->high_gain - filter->low_gain);
      } else {
         k = filter->k * filter->high_gain;
      }

      do
      {
         float32_ptr d = dptr;
         size_t i = num;

         c = vld1q_f32(cptr);

         h2 = vld1_f32(hist);
         h = vcombine_f32(h2, h2);

         do
         {
            float32x4_t pz;
            float32x2_t half;
            float smp, tmp;

            smp = *s++ * k;

            pz = vmulq_f32(c, h); // poles and zeros
            half = vget_low_f32(pz);
            tmp = vget_lane_f32(half, 0) + vget_lane_f32(half, 1);

            hist[1] = hist[0];
            hist[0] = smp + tmp;
            h2 = vld1_f32(hist);
            h = vcombine_f32(h2, h2);

            half = vget_high_f32(pz);
            tmp = vget_lane_f32(half, 0) + vget_lane_f32(half, 1);
            *d++ = smp + tmp;
         }
         while (--i);

         hist += 2;
         cptr += 4;
         k = 1.0f;
         s = dptr;
      }
      while (--stages);
   }
}

void
_batch_fmul_value_neon(void* data, unsigned bps, size_t num, float f)
{
   if (!num || fabsf(f - 1.0f) < LEVEL_96DB) return;

   if (f <= LEVEL_128DB) {
      memset(data, 0, num*bps);
   }
   else if (num)
   {
      size_t i = num;

      switch (bps)
      {
      case 4:
      {
         float32_ptr s = (float32_ptr)data;
         float32_ptr d = (float32_ptr)data;
         size_t i, step;

         step = sizeof(float32x4x4_t)/sizeof(float);

         i = num/step;
         num -= i*step;
         if (i)
         {
            float32x4_t sfact = vdupq_n_f32(f);
            float32x4x4_t sfr4;

            do
            {
               sfr4 = vld4q_f32(s);   // load s
               s += step;

               sfr4.val[0] = vmulq_f32(sfr4.val[0], sfact);
               sfr4.val[1] = vmulq_f32(sfr4.val[1], sfact);
               sfr4.val[2] = vmulq_f32(sfr4.val[2], sfact);
               sfr4.val[3] = vmulq_f32(sfr4.val[3], sfact);

               vst4q_f32(d, sfr4);    // store d
               d += step;
            }
            while(--i);
         }

         if (num) {
            i = num;
            do {
               *d++ *= *s++;
            } while(--i);
         }
         break;
      }
      case 8:
      {
         double *d = (double*)data;
         do {
            *d++ *= f;
         }
         while (--i);
         break;
      }
      default:
         break;
      }
   }
}

#if !RB_FLOAT_DATA
static inline void
_aaxBufResampleDecimate_neon(int32_ptr d, const_int32_ptr s, size_t dmin, size_t dmax, float smu, float freq_factor)
{
   int32_ptr sptr = (int32_ptr)s;
   int32_ptr dptr = d;
   int32_t samp, dsamp;
   size_t i;

   assert(s != 0);
   assert(d != 0);
   assert(dmin < dmax);
   assert(freq_factor >= 1.0f);
   assert(0.0f <= smu && smu < 1.0f);

   dptr += dmin;

   samp = *sptr++;              // n+(step-1)
   dsamp = *sptr - samp;        // (n+1) - n


   i=dmax-dmin;
   if (i)
   {
      do
      {
         size_t step;

         *dptr++ = samp + (int32_t)(dsamp * smu);

         smu += freq_factor;
         step = (size_t)floorf(smu);

         smu -= step;
         sptr += step-1;
         samp = *sptr++;
         dsamp = *sptr - samp;
      }
      while (--i);
   }
}

#if 0
static inline void
_aaxBufResampleNearest_neon(int32_ptr d, const_int32_ptr s, size_t dmin, size_t dmax, float smu, float freq_factor)
{
   if (freq_factor == 1.0f) {
      _aax_memcpy(d+dmin, s, (dmax-dmin)*sizeof(int32_t));
   }
   else
   {
      int32_ptr sptr = (int32_ptr)s;
      int32_ptr dptr = d;
      size_t i;

      assert(s != 0);
      assert(d != 0);
      assert(dmin < dmax);
      assert(0.95f <= freq_factor && freq_factor <= 1.05f);
      assert(0.0f <= smu && smu < 1.0f);

      dptr += dmin;

      i = dmax-dmin;
      if (i)
      {
         do
         {
            *dptr++ = *sptr;

            smu += freq_factor;
            if (smu > 0.5f)
            {
               sptr++;
               smu -= 1.0f;
            }
         }
         while (--i);
      }
   }
}
#endif

static inline void
_aaxBufResampleLinear_neon(int32_ptr d, const_int32_ptr s, size_t dmin, size_t dmax, float smu, float freq_factor)
{
   int32_ptr sptr = (int32_ptr)s;
   int32_ptr dptr = d;
   int32_t samp, dsamp;
   size_t i;

   assert(s != 0);
   assert(d != 0);
   assert(dmin < dmax);
   assert(freq_factor < 1.0f);
   assert(0.0f <= smu && smu < 1.0f);

   dptr += dmin;

   samp = *sptr++;              // n
   dsamp = *sptr - samp;        // (n+1) - n

   i = dmax-dmin;
   if (i)
   {
      do
      {
         *dptr++ = samp + (int32_t)(dsamp * smu);

         smu += freq_factor;
         if (smu >= 1.0)
         {
            smu -= 1.0;
            samp = *sptr++;
            dsamp = *sptr - samp;
         }
      }
      while (--i);
   }
}

static inline void
_aaxBufResampleCubic_neon(int32_ptr d, const_int32_ptr s, size_t dmin, size_t dmax, float smu, float freq_factor)
{
   float y0, y1, y2, y3, a0, a1, a2;
   int32_ptr sptr = (int32_ptr)s;
   int32_ptr dptr = d;
   size_t i;

   assert(s != 0);
   assert(d != 0);
   assert(dmin < dmax);
   assert(0.0f <= smu && smu < 1.0f);
   assert(0.0f < freq_factor && freq_factor <= 1.0f);

   dptr += dmin;

   y0 = (float)*sptr++;
   y1 = (float)*sptr++;
   y2 = (float)*sptr++;
   y3 = (float)*sptr++;

   a0 = y3 - y2 - y0 + y1;
   a1 = y0 - y1 - a0;
   a2 = y2 - y0;

   i = dmax-dmin;
   if (i)
   {
      do
      {
         float smu2, ftmp;

         smu2 = smu*smu;
         ftmp = (a0*smu*smu2 + a1*smu2 + a2*smu + y1);
         *dptr++ = (int32_t)ftmp;

         smu += freq_factor;
         if (smu >= 1.0)
         {
            smu--;
            a0 += y0;
            y0 = y1;
            y1 = y2;
            y2 = y3;
            y3 = (float)*sptr++;
            a0 = -a0 + y3;                      /* a0 = y3 - y2 - y0 + y1; */
            a1 = y0 - y1 - a0;
            a2 = y2 - y0;
         }
      }
      while (--i);
   }
}

void
_batch_resample_neon(int32_ptr d, const_int32_ptr s, size_t dmin, size_t dmax, float smu, float fact)
{
   assert(fact > 0.0f);

   if (fact < CUBIC_TRESHOLD) {
      _aaxBufResampleCubic_neon(d, s, dmin, dmax, smu, fact);
   }
   else if (fact < 1.0f) {
      _aaxBufResampleLinear_neon(d, s, dmin, dmax, smu, fact);
   }
   else if (fact > 1.0f) {
      _aaxBufResampleDecimate_neon(d, s, dmin, dmax, smu, fact);
   } else {
//    _aaxBufResampleNearest_neon(d, s, dmin, dmax, smu, fact);
      _aax_memcpy(d+dmin, s, (dmax-dmin)*sizeof(MIX_T));
   }
}
#else

static inline void
_aaxBufResampleDecimate_float_neon(float32_ptr dptr, const_float32_ptr sptr, size_t dmin, size_t dmax, float smu, float freq_factor)
{
   float32_ptr s = (float32_ptr)sptr;
   float32_ptr d = dptr;
   float samp, dsamp;
   size_t i;

   assert(s != 0);
   assert(d != 0);
   assert(dmin < dmax);
   assert(freq_factor >= 1.0f);
   assert(0.0f <= smu && smu < 1.0f);

   d += dmin;

   samp = *s++;                 // n+(step-1)
   dsamp = *s - samp;           // (n+1) - n

   i = dmax-dmin;
   if (i)
   {
      if (freq_factor == 2.0f)
      {
         do {
            *d++ = (*s + *(s+1))*0.5f;
            s += 2;
         }
         while (--i);
      }
      else
      {
         do
         {  
            size_t step;
            
            *d++ = samp + (dsamp * smu);
    
            smu += freq_factor;
            step = (size_t)floorf(smu);
     
            smu -= step;
            s += step-1;
            samp = *s++; 
            dsamp = *s - samp;
         }
         while (--i);
      }
   }
}

#if 0
static inline void
_aaxBufResampleNearest_float_neon(float32_ptr d, const_float32_ptr s, size_t dmin, size_t dmax, float smu, float freq_factor)
{
   if (freq_factor == 1.0f) {
      _aax_memcpy(d+dmin, s, (dmax-dmin)*sizeof(float));
   }
   else
   {
      float32_ptr sptr = (float32_ptr)s;
      float32_ptr dptr = d;
      size_t i;

      assert(s != 0);
      assert(d != 0);
      assert(dmin < dmax);
      assert(0.95f <= freq_factor && freq_factor <= 1.05f);
      assert(0.0f <= smu && smu < 1.0f);

      dptr += dmin;

      i = dmax-dmin;
      if (i)
      {
         do
         {
            *dptr++ = *sptr;

            smu += freq_factor;
            if (smu > 0.5f)
            {
               sptr++;
               smu -= 1.0f;
            }
         }
         while (--i);
      }
   }
}
#endif

static inline void
_aaxBufResampleLinear_float_neon(float32_ptr d, const_float32_ptr s, size_t dmin, size_t dmax, float smu, float freq_factor)
{
   float32_ptr sptr = (float32_ptr)s;
   float32_ptr dptr = d;
   float samp, dsamp;
   size_t i;

   assert(s != 0);
   assert(d != 0);
   assert(dmin < dmax);
   assert(freq_factor < 1.0f);
   assert(0.0f <= smu && smu < 1.0f);

   dptr += dmin;

   samp = *sptr++;              // n
   dsamp = *sptr - samp;        // (n+1) - n

   i = dmax-dmin;
   if (i)
   {
      do
      {
         *dptr++ = samp + (dsamp * smu);

         smu += freq_factor;
         if (smu >= 1.0)
         {
            smu -= 1.0;
            samp = *sptr++;
            dsamp = *sptr - samp;
         }
      }
      while (--i);
   }
}

static inline void
_aaxBufResampleCubic_float_neon(float32_ptr d, const_float32_ptr s, size_t dmin, size_t dmax, float smu, float freq_factor)
{
   float y0, y1, y2, y3, a0, a1, a2;
   float32_ptr sptr = (float32_ptr)s;
   float32_ptr dptr = d;
   size_t i;

   assert(s != 0);
   assert(d != 0);
   assert(dmin < dmax);
   assert(0.0f <= smu && smu < 1.0f);
   assert(0.0f < freq_factor && freq_factor <= 1.0f);

   dptr += dmin;

   y0 = *sptr++;
   y1 = *sptr++;
   y2 = *sptr++;
   y3 = *sptr++;

   a0 = y3 - y2 - y0 + y1;
   a1 = y0 - y1 - a0;
   a2 = y2 - y0;

   i = dmax-dmin;
   if (i)
   {
      do
      {
         float smu2, ftmp;

         smu2 = smu*smu;
         ftmp = (a0*smu*smu2 + a1*smu2 + a2*smu + y1);
         *dptr++ = ftmp;

         smu += freq_factor;
         if (smu >= 1.0)
         {
            smu--;
            a0 += y0;
            y0 = y1;
            y1 = y2;
            y2 = y3;
            y3 = *sptr++;
            a0 = -a0 + y3;                      /* a0 = y3 - y2 - y0 + y1; */
            a1 = y0 - y1 - a0;
            a2 = y2 - y0;
         }
      }
      while (--i);
   }
}

void
_batch_resample_float_neon(float32_ptr d, const_float32_ptr s, size_t dmin, size_t dmax, float smu, float fact)
{
   assert(fact > 0.0f);

   if (fact < CUBIC_TRESHOLD) {
      _aaxBufResampleCubic_float_neon(d, s, dmin, dmax, smu, fact);
   }
   else if (fact < 1.0f) {
      _aaxBufResampleLinear_float_neon(d, s, dmin, dmax, smu, fact);
   }
   else if (fact > 1.0f) {
      _aaxBufResampleDecimate_float_neon(d, s, dmin, dmax, smu, fact);
   } else {
//    _aaxBufResampleNearest_float_neon(d, s, dmin, dmax, smu, fact);
      _aax_memcpy(d+dmin, s, (dmax-dmin)*sizeof(MIX_T));
   }
}
#endif // RB_FLOAT_DATA

#endif /* NEON */

