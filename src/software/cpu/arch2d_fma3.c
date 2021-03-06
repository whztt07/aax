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
#include "software/cpu/arch2d_simd.h"

#ifdef __FMA__

# define CACHE_ADVANCE_FMADD	 (2*16)
# define CACHE_ADVANCE_FF	 (2*32)

FN_PREALIGN void
_batch_fma3_float_avx(float32_ptr dst, const_float32_ptr src, size_t num, float v, float vstep)
{
   int need_step = (fabsf(vstep) <=  LEVEL_96DB) ? 0 : 1;
   float32_ptr s = (float32_ptr)src;
   float32_ptr d = (float32_ptr)dst;
   size_t i, step, dtmp, stmp;

   if (!num || (v <= LEVEL_128DB && vstep <= LEVEL_128DB)) return;

   if (fabsf(v - 1.0f) < LEVEL_96DB && !need_step) {
      _batch_fadd_avx(dst, src, num);
      return;
   }

   // volume change requested
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

   /* work towards a 16-byte aligned dptr (and hence 16-byte aligned sptr) */
   dtmp = (size_t)d & MEMMASK;
   if (dtmp && num)
   {
      i = (MEMALIGN - dtmp)/sizeof(float);
      if (i <= num)
      {
         do {
            *d++ += *s++ * v;
         } while(--i);
      }
   }
   stmp = (size_t)s & MEMMASK;

   step = 8*sizeof(__m256)/sizeof(float);

   i = num/step;
   if (i)
   {
      __m256 ymm0, ymm1, ymm2, ymm3, ymm4, ymm5, ymm6, ymm7;
      __m256 tv = _mm256_set1_ps(v);
      __m256 *sptr = (__m256 *)s;
      __m256 *dptr = (__m256 *)d;

      num -= i*step;
      s += i*step;
      d += i*step;

      if (stmp)
      {
         do
         {
            ymm0 = _mm256_loadu_ps((const float*)sptr++);
            ymm1 = _mm256_loadu_ps((const float*)sptr++);
            ymm2 = _mm256_loadu_ps((const float*)sptr++);
            ymm3 = _mm256_loadu_ps((const float*)sptr++);

            ymm4 = _mm256_loadu_ps((const float*)sptr++);
            ymm5 = _mm256_loadu_ps((const float*)sptr++);
            ymm6 = _mm256_loadu_ps((const float*)sptr++);
            ymm7 = _mm256_loadu_ps((const float*)sptr++);

            ymm0 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+0)), ymm0, tv);
            ymm1 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+1)), ymm1, tv);
            ymm2 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+2)), ymm2, tv);
            ymm3 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+3)), ymm3, tv);
            ymm4 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+4)), ymm4, tv);
            ymm5 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+5)), ymm5, tv);
            ymm6 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+6)), ymm6, tv);
            ymm7 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+7)), ymm7, tv);

            _mm256_store_ps((float*)dptr++, ymm0);
            _mm256_store_ps((float*)dptr++, ymm1);
            _mm256_store_ps((float*)dptr++, ymm2);
            _mm256_store_ps((float*)dptr++, ymm3);
            _mm256_store_ps((float*)dptr++, ymm4);
            _mm256_store_ps((float*)dptr++, ymm5);
            _mm256_store_ps((float*)dptr++, ymm6);
            _mm256_store_ps((float*)dptr++, ymm7);
         }
         while(--i);
      }
      else
      {
         do
         {
            ymm0 = _mm256_load_ps((const float*)sptr++);
            ymm1 = _mm256_load_ps((const float*)sptr++);
            ymm2 = _mm256_load_ps((const float*)sptr++);
            ymm3 = _mm256_load_ps((const float*)sptr++);

            ymm4 = _mm256_load_ps((const float*)sptr++);
            ymm5 = _mm256_load_ps((const float*)sptr++);
            ymm6 = _mm256_load_ps((const float*)sptr++);
            ymm7 = _mm256_load_ps((const float*)sptr++);

            ymm0 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+0)), ymm0, tv);
            ymm1 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+1)), ymm1, tv);
            ymm2 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+2)), ymm2, tv);
            ymm3 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+3)), ymm3, tv);
            ymm4 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+4)), ymm4, tv);
            ymm5 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+5)), ymm5, tv);
            ymm6 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+6)), ymm6, tv);
            ymm7 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+7)), ymm7, tv);

            _mm256_store_ps((float*)dptr++, ymm0);
            _mm256_store_ps((float*)dptr++, ymm1);
            _mm256_store_ps((float*)dptr++, ymm2);
            _mm256_store_ps((float*)dptr++, ymm3);
            _mm256_store_ps((float*)dptr++, ymm4);
            _mm256_store_ps((float*)dptr++, ymm5);
            _mm256_store_ps((float*)dptr++, ymm6);
            _mm256_store_ps((float*)dptr++, ymm7);
         }
         while(--i);
      }
   }

   step = 2*sizeof(__m256)/sizeof(float);
   i = num/step;
   if (i)
   {
      __m256 tv = _mm256_set1_ps(v);
      __m256* sptr = (__m256*)s;
      __m256* dptr = (__m256*)d;
      __m256 ymm0, ymm1;

      num -= i*step;
      s += i*step;
      d += i*step;
      if (sptr)
      {
         do
         {
            ymm0 = _mm256_loadu_ps((const float*)sptr++);
            ymm1 = _mm256_loadu_ps((const float*)sptr++);

            ymm0 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+0)), ymm0, tv);
            ymm1 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+1)), ymm1, tv);

            _mm256_store_ps((float*)dptr++, ymm0);
            _mm256_store_ps((float*)dptr++, ymm1);
         }
         while(--i);
      }
      else
      {
         do
         {
            ymm0 = _mm256_load_ps((const float*)sptr++);
            ymm1 = _mm256_load_ps((const float*)sptr++);

            ymm0 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+0)), ymm0, tv);
            ymm1 =_mm256_fmadd_ps(_mm256_load_ps((const float*)(dptr+1)), ymm1, tv);

            _mm256_store_ps((float*)dptr++, ymm0);
            _mm256_store_ps((float*)dptr++, ymm1);
         }
         while(--i);
      }
      vstep /= step;
   }
   _mm256_zeroall();

   if (num)
   {
      i = num;
      do {
         *d++ += *s++ * v;
      } while(--i);
   }
}

#else
typedef int make_iso_compilers_happy;
#endif /* __FMA__ */

