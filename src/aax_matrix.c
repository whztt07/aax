/*
 * Copyright 2007-2013 by Erik Hofman.
 * Copyright 2009-2013 by Adalin B.V.
 * All Rights Reserved.
 *
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Adalin B.V.;
 * the contents of this file may not be disclosed to third parties, copied or
 * duplicated in any form, in whole or in part, without the prior written
 * permission of Adalin B.V.
 */

#include <aax/aax.h>

#include <base/gmath.h>
#include <base/geometry.h>

#include "api.h"

AAX_API int AAX_APIENTRY
aaxMatrixSetIdentityMatrix(aaxMtx4f mtx)
{
   int rv = AAX_FALSE;
   if (mtx)
   {
      mtx4Copy(mtx, aaxIdentityMatrix);
      rv = AAX_TRUE;
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxMatrix64SetIdentityMatrix(aaxMtx4d mtx)
{
   int rv = AAX_FALSE;
   if (mtx)
   {
      mtx4dCopy(mtx, aaxIdentityMatrix64);
      rv = AAX_TRUE;
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxMatrixTranslate(aaxMtx4f mtx, float dx, float dy, float dz)
{
   int rv = AAX_FALSE;
   if (mtx && !detect_nan_vec4(mtx[0]) && !detect_nan_vec4(mtx[1]) &&
              !detect_nan_vec4(mtx[2]) && !detect_nan_vec4(mtx[3]))
   {
      if (!is_nan(dx))
      {
         if (!is_nan(dy))
         {
            if (!is_nan(dz))
            {
               mtx4_t m;

               mtx4Copy(m, mtx);
               mtx4Translate(m, dx, dy, dz);
               mtx4Copy(mtx, m);
               rv = AAX_TRUE;
            }
            else {
               _aaxErrorSet(AAX_INVALID_PARAMETER + 3);
            }
         }
         else {
            _aaxErrorSet(AAX_INVALID_PARAMETER + 2);
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER + 1);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxMatrix64Translate(aaxMtx4d mtx, double dx, double dy, double dz)
{
   int rv = AAX_FALSE;
   if (mtx && !detect_nan_vec4d(mtx[0]) && !detect_nan_vec4d(mtx[1]) &&
              !detect_nan_vec4d(mtx[2]) && !detect_nan_vec4d(mtx[3]))
   {
      if (!is_nan64(dx))
      {
         if (!is_nan64(dy))
         {
            if (!is_nan64(dz))
            {
               mtx4d_t m;

               mtx4dCopy(m, mtx);
               mtx4dTranslate(m, dx, dy, dz);
               mtx4dCopy(mtx, m);
               rv = AAX_TRUE;
            }
            else {
               _aaxErrorSet(AAX_INVALID_PARAMETER + 3);
            }
         }
         else {
            _aaxErrorSet(AAX_INVALID_PARAMETER + 2);
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER + 1);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxMatrixRotate(aaxMtx4f mtx, float angle_rad, float x, float y, float z)
{
   int rv = AAX_FALSE;
   if (mtx && !detect_nan_vec4(mtx[0]) && !detect_nan_vec4(mtx[1]) &&
              !detect_nan_vec4(mtx[2]) && !detect_nan_vec4(mtx[3]))
   {
      if (!is_nan(angle_rad))
      {
         if (!is_nan(x))
         {
            if (!is_nan(y))
            {
               if (!is_nan(z))
               {
                  mtx4_t m;

                  mtx4Copy(m, mtx);
                  mtx4Rotate(m, angle_rad, x, y, z);
                  mtx4Copy(mtx, m);
                  rv = AAX_TRUE;
               }
               else {
                  _aaxErrorSet(AAX_INVALID_PARAMETER + 4);
               }
            }
            else {
               _aaxErrorSet(AAX_INVALID_PARAMETER + 3);
            }
         }
         else {
            _aaxErrorSet(AAX_INVALID_PARAMETER + 2);
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER + 1);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxMatrix64Rotate(aaxMtx4d mtx, double angle_rad, double x, double y, double z)
{
   int rv = AAX_FALSE;
   if (mtx && !detect_nan_vec4d(mtx[0]) && !detect_nan_vec4d(mtx[1]) &&
              !detect_nan_vec4d(mtx[2]) && !detect_nan_vec4d(mtx[3]))
   {
      if (!is_nan64(angle_rad))
      {
         if (!is_nan64(x))
         {
            if (!is_nan64(y))
            {
               if (!is_nan64(z))
               {
                  mtx4d_t m;

                  mtx4dCopy(m, mtx);
                  mtx4dRotate(m, angle_rad, x, y, z);
                  mtx4dCopy(mtx, m);
                  rv = AAX_TRUE;
               }
               else {
                  _aaxErrorSet(AAX_INVALID_PARAMETER + 4);
               }
            }
            else {
               _aaxErrorSet(AAX_INVALID_PARAMETER + 3);
            }
         }
         else {
            _aaxErrorSet(AAX_INVALID_PARAMETER + 2);
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER + 1);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}


AAX_API int AAX_APIENTRY
aaxMatrixMultiply(aaxMtx4f mtx1, aaxMtx4f mtx2)
{
   int rv = AAX_FALSE;
   if (mtx1 && !detect_nan_vec4(mtx1[0]) && !detect_nan_vec4(mtx1[1]) &&
               !detect_nan_vec4(mtx1[2]) && !detect_nan_vec4(mtx1[3]))
   {
      if (mtx2 && !detect_nan_vec4(mtx2[0]) && !detect_nan_vec4(mtx2[1]) &&
                  !detect_nan_vec4(mtx2[2]) && !detect_nan_vec4(mtx2[3]))
      {
         mtx4_t m1, m2, m3;

         mtx4Copy(m1, mtx1);
         mtx4Copy(m2, mtx2);
         mtx4Mul(m3, m1, m2);
         mtx4Copy(mtx1, m3);
         rv = AAX_TRUE;
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER + 1);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxMatrix64Multiply(aaxMtx4d mtx1, aaxMtx4d mtx2)
{
   int rv = AAX_FALSE;
   if (mtx1 && !detect_nan_vec4d(mtx1[0]) && !detect_nan_vec4d(mtx1[1]) &&
               !detect_nan_vec4d(mtx1[2]) && !detect_nan_vec4d(mtx1[3]))
   {
      if (mtx2 && !detect_nan_vec4d(mtx2[0]) && !detect_nan_vec4d(mtx2[1]) &&
                  !detect_nan_vec4d(mtx2[2]) && !detect_nan_vec4d(mtx2[3]))
      {
         mtx4d_t m1, m2, m3;

         mtx4dCopy(m1, mtx1);
         mtx4dCopy(m2, mtx2);
         mtx4dMul(m3, m1, m2);
         mtx4dCopy(mtx1, m3);
         rv = AAX_TRUE;
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER + 1);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxMatrixInverse(aaxMtx4f mtx)
{
   int rv = AAX_FALSE;
   if (mtx)
   {
      if (!detect_nan_vec4(mtx[0]) && !detect_nan_vec4(mtx[1]) &&
          !detect_nan_vec4(mtx[2]) && !detect_nan_vec4(mtx[3]))
      {
         mtx4_t m1, m2;
         mtx4Copy(m1, mtx);
         mtx4InverseSimple(m2, m1);
         mtx4Copy(mtx, m2);
         rv = AAX_TRUE;
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER + 1);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxMatrix64Inverse(aaxMtx4d mtx)
{
   int rv = AAX_FALSE;
   if (mtx)
   {
      if (!detect_nan_vec4d(mtx[0]) && !detect_nan_vec4d(mtx[1]) &&
          !detect_nan_vec4d(mtx[2]) && !detect_nan_vec4d(mtx[3]))
      {
         mtx4d_t m1, m2;
         mtx4dCopy(m1, mtx);
         mtx4dInverseSimple(m2, m1);
         mtx4dCopy(mtx, m2);
         rv = AAX_TRUE;
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER + 1);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxMatrixSetDirection(aaxMtx4f mtx, const aaxVec3f pos, const aaxVec3f at)
{
   int rv = AAX_FALSE;
   if (mtx)
   {
      if (pos && !detect_nan_vec3(pos))
      {
          if (at && !detect_nan_vec3(at))
          {
            mtx4Copy(mtx, aaxIdentityMatrix);
            if (at[0] || at[1] || at[2])
            {
               aaxVec3f up = { 0.0f, 1.0f, 0.0f }; 
               vec3 side, upwd, fwd, back;

               if (fabs(at[0]) < 1e-6f && fabs(at[2]) < 1e-6f)
               {  
                  up[1] = 0.0f;
                  if (at[2] < 0.0f) up[2] = -1.0f;
                  else              up[2] =  1.0f;
               }  

               vec3Copy(upwd, up);
               vec3Copy(fwd, at);
               vec3CrossProduct(side, fwd, upwd);

               vec3Negate(back, fwd);
               vec3Normalize(mtx[0], side);
               vec3Normalize(mtx[1], up);
               vec3Normalize(mtx[2], back);
            }
            vec3Negate(mtx[3], pos);
#if 0
 printf("SetDirection:\n");
 PRINT_MATRIX(mtx);
#endif
            rv = AAX_TRUE;
         }
         else {
            _aaxErrorSet(AAX_INVALID_PARAMETER + 2);
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER + 1);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxMatrixSetOrientation(aaxMtx4f mtx, const aaxVec3f pos, const aaxVec3f at,
                                                          const aaxVec3f up)
{
   int rv = AAX_FALSE;
   if (mtx)
   {
      if (pos && !detect_nan_vec3(pos))
      { 
         if (at && !detect_nan_vec3(at))
         {
            if (up && !detect_nan_vec3(up))
            {
               mtx4Copy(mtx, aaxIdentityMatrix);
               if (at[0] || at[1] || at[2] || up[0] || up[1] || up[2])
               {
                  vec3 side, upwd, fwd, back;

                  vec3Copy(upwd, up);
                  vec3Copy(fwd, at);
                  vec3CrossProduct(side, fwd, upwd);

                  vec3Negate(back, fwd);
                  vec3Normalize(mtx[0], side);
                  vec3Normalize(mtx[1], up);
                  vec3Normalize(mtx[2], back);
               }
               vec3Negate(mtx[3], pos);
               rv = AAX_TRUE;
            }
            else {
               _aaxErrorSet(AAX_INVALID_PARAMETER + 3);
            }
         }
         else {
            _aaxErrorSet(AAX_INVALID_PARAMETER + 2);
         }
      }
      else {
         _aaxErrorSet(AAX_INVALID_PARAMETER + 1);
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxMatrixGetOrientation(aaxMtx4f mtx, aaxVec3f pos, aaxVec3f at, aaxVec3f up)
{
   int rv = AAX_FALSE;
   if (mtx)
   {
      if (pos || at || up)
      {
         if (pos) {
            vec3Copy(pos, mtx[3]);	/* LOCATION */
         }
         if (at) {
            vec3Copy(at, mtx[2]);	/* DIR_UPWD */
         }
         if (up) {
            vec3Copy(up, mtx[1]); /* DIR_BACK */
         }
         rv = AAX_TRUE;
      }
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}

AAX_API int AAX_APIENTRY
aaxMatrix64ToMatrix(aaxMtx4f mtx, aaxMtx4d mtx64)
{
   int rv = AAX_FALSE;
   if (mtx && mtx64 &&
           !detect_nan_vec4d(mtx64[0]) && !detect_nan_vec4d(mtx64[1]) &&
           !detect_nan_vec4d(mtx64[2]) && !detect_nan_vec4d(mtx64[3]))
   {
      int i, j;
      i = 4;
      do
      {
         i--;
         j = 4;
         do
         {
            j--;
            mtx[i][j] = (float)mtx64[i][j];
         }
         while(j);
      }
      while(i);
   }
   else {
      _aaxErrorSet(AAX_INVALID_PARAMETER);
   }
   return rv;
}
 
