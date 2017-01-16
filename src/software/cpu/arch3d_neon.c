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

#ifdef __ARM_NEON__
# include <arm_neon.h>
#include "arch3d_simd.h"

void
_vec4Copy_neon(vec4 d, const vec4 v)
{
   const float32_t *src = (const float32_t *)v;
   float32_t *dst = (float32_t *)d;
   float32x4_t nfr1;

   nfr1 = vld1q_f32(src);
   vst1q_f32(dst, nfr1);
}

void
_vec4Mulvec4_neon(vec4 r, const vec4 v1, const vec4 v2)
{
   const float32_t *src1 = (const float32_t *)v1;
   const float32_t *src2 = (const float32_t *)v2;
   float32_t *dst = (float32_t *)r;
   float32x4_t nfr1, nfr2;

   nfr1 = vld1q_f32(src1);
   nfr2 = vld1q_f32(src2);
   nfr1 = vmulq_f32(nfr1, nfr2);
   vst1q_f32(dst, nfr1);
}

void
__vec4Matrix4_neon(vec4 d, const vec4 v, const mtx4 m)
{
   float32x4_t a_line, b_line, r_line;
   const float32_t *a = (const float32_t *)m;
   const float32_t *b = (const float32_t *)v;
   float32_t *r = (float32_t *)d;
   int i;

  /*
   * unroll the first step of the loop to avoid having to initialize
   * r_line to zero
   */
   a_line = vld1q_f32(a);             /* a_line = vec4(column(m, 0)) */
   b_line = vdupq_n_f32(b[0]);          /* b_line = vec4(v[0])         */
   r_line = vmulq_f32(a_line, b_line); /* r_line = a_line * b_line    */
   for (i=1; i<4; i++)
   {
      a_line = vld1q_f32(&a[i*4]);    /* a_line = vec4(column(m, i)) */
      b_line = vdupq_n_f32(b[i]);       /* b_line = vec4(v[i])         */
                                        /* r_line += a_line * b_line   */
      r_line = vaddq_f32(vmulq_f32(a_line, b_line), r_line);
   }
   vst1q_f32(r, r_line);             /* r = r_line                  */
}

void
_pt4Matrix4_neon(vec4 d, const vec4 p, const mtx4 m)
{
   vec4_t v;

   vec4Copy(v, p);
   v[3] = 1.0f;
   __vec4Matrix4_neon(d, v, m);
}

void
_vec4Matrix4_neon(vec4 d, const vec4 vi, const mtx4 m)
{
   vec4_t v;

   vec4Copy(v, vi);
   v[3] = 0.0f;
   __vec4Matrix4_neon(d, v, m);
}


void
_mtx4Mul_neon(mtx4 d, const mtx4 m1, const mtx4 m2)
{
   float32x4_t a_line, b_line, r_line;
   const float32_t *a = (const float32_t *)m1;
   const float32_t *b = (const float32_t *)m2;
   float32_t *r = (float32_t *)d;
   int i;

   for (i=0; i<16; i += 4)
   {
     int j;
     /*
      * unroll the first step of the loop to avoid having to initialize
      * r_line to zero
      */
      a_line = vld1q_f32(a);             /* a_line = vec4(column(a, 0)) */
      b_line = vdupq_n_f32(b[i]);          /* b_line = vec4(b[i][0])      */
      r_line = vmulq_f32(a_line, b_line); /* r_line = a_line * b_line    */
      for (j=1; j<4; j++)
      {
         a_line = vld1q_f32(&a[j*4]);    /* a_line = vec4(column(a, j)) */
         b_line = vdupq_n_f32(b[i+j]);     /* b_line = vec4(b[i][j])      */
                                           /* r_line += a_line * b_line   */
         r_line = vaddq_f32(vmulq_f32(a_line, b_line), r_line);
      }
      vst1q_f32(&r[i], r_line);         /* r[i] = r_line               */
   }
}

#endif /* NEON */

