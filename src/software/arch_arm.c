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

#if HAVE_UNISTD_H
# include <unistd.h>    /* sysconf */
#endif
#if defined(__MINGW32__)
# include <mm_malloc.h>
#endif
#if HAVE_CPU_FEATURES_H
#include <machine/cpu-features.h>
#endif

#include <base/types.h>
#include <base/logging.h>
#include <base/geometry.h>

#include <arch.h>

#include "rbuf_int.h"
#include "cpu/arch2d_simd.h"
#include "cpu/arch3d_simd.h"

#if defined(__arm__) || defined(_M_ARM)

#define MAX_CPUINFO	1024

enum {
    AAX_NO_SIMD      = 0,
    AAX_ARCH_VFP     = 0x00000001,
    AAX_ARCH_VFPV3   = 0x00000002,
    AAX_ARCH_VFPV4   = 0x00000004,
    AAX_ARCH_NEON    = 0x00000008
};

enum {
   AAX_SIMD_VFP = 1,
   AAX_SIMD_VFPV3,
   AAX_SIMD_VFPV4,
   AAX_SIMD_NEON,
   AAX_SIMD_VFPV4_NEON,
   AAX_SIMD_MAX
};

static uint32_t _aax_arch_capabilities = AAX_NO_SIMD;
static const char *_aaxArchSIMDSupportString[AAX_SIMD_MAX] =
{
   SIMD_PREFIX"",
   SIMD_PREFIX"VFP",
   SIMD_PREFIX"VFPv3",
   SIMD_PREFIX"VFPv4",
   SIMD_PREFIX"NEON",
   SIMD_PREFIX"VFPv4/NEON"
};

char
_aaxArchDetectNeon()
{
  static char res = 0;
   static int8_t init = -1;
   if (init)
   {
      char *env = getenv("AAX_NO_SIMD_SUPPORT");

      init = 0;
      if (!_aax_getbool(env))
      {
# if HAVE_CPU_FEATURES_H
         if (android_getCpuFamily() == ANDROID_CPU_FAMILY_ARM)
         {
            uint64_t features = android_getCpuFeatures();
            if ( (features & ANDROID_CPU_ARM_FEATURE_ARMv7)
                  && (features & ANDROID_CPU_ARM_FEATURE_NEON) )
            {
               res = 1;
            }
         }

# elif defined(__linux__)
// Reading /proc/self/auxv doesn't work reliably on Android.
// cat /proc/cpuinfo | grep "^Features"
// Features        : swp half thumb fastmult vfp edsp thumbee neon vfpv3 tls
         FILE *fp = fopen("/proc/cpuinfo", "r");
         if (fp)
         {
            char cpuinfo[MAX_CPUINFO];
            int rv;

            memset(cpuinfo, 0, MAX_CPUINFO);
            rv = fread(cpuinfo, 1, MAX_CPUINFO, fp);
            fclose(fp);

            if (rv > 0 && rv < MAX_CPUINFO)
            {
               char *features, *ptr;

               cpuinfo[MAX_CPUINFO-1] = '\0';
               features = strstr(cpuinfo, "Features");
               if (features)
               {
                  ptr = strchr(features, '\n');
                  if (ptr) *ptr = '\0';

                  ptr = strstr(features, " neon");
                  if (ptr && (*(ptr+5) == ' ' || *(ptr+5) == '\0'))
                  {
                     res = 1;
                  }
               }
            }
         }
      }
# endif
   }
   if (res) _aax_arch_capabilities |= AAX_ARCH_NEON;
   return res;
}

const char *
_aaxGetSIMDSupportString()
{
   uint32_t level = AAX_NO_SIMD;

   level = _aaxGetVFPNeonLevel();
   if (_aax_arch_capabilities & AAX_ARCH_NEON)
   {
      vec4Add = _vec4Add_neon;
      vec4Sub = _vec4Sub_neon;
      vec4Copy = _vec4Copy_neon;
      vec4Devide = _vec4Devide_neon;
      vec4Mulvec4 = _vec4Mulvec4_neon;
      vec4Matrix4 = _vec4Matrix4_neon;
      pt4Matrix4 = _pt4Matrix4_neon;

      mtx4Mul = _mtx4Mul_neon;

      _batch_imadd = _batch_imadd_neon;
      _batch_fmadd = _batch_fmadd_neon;
      _batch_cvt24_16 = _batch_cvt24_16_neon;
      _batch_cvt16_24 = _batch_cvt16_24_neon;
//    _batch_cvt16_intl_24 = _batch_cvt16_inl_24_neon;
      _batch_freqfilter = _batch_freqfilter_neon;

# if RB_FLOAT_DATA
      _batch_cvtps24_24 = _batch_cvtps24_24_neon;
      _batch_cvt24_ps24 = _batch_cvt24_ps24_neon;
      _batch_resample_float = _batch_resample_float_neon;
# else
      _batch_resample = _batch_resample_neon;
# endif
   }

   return _aaxArchSIMDSupportString[level];
}

/* -------------------------------------------------------------------------- */

unsigned int
_aaxGetNoCores()
{
   unsigned int cores = 1;

# if defined(__MACH__)
   sysctlbyname("hw.physicalcpu", &cores, sizeof(cores), NULL, 0);

# elif defined(freebsd) || defined(bsdi) || defined(__darwin__) || defined(openbsd)
   size_t len = sizeof(cores);
   int mib[4];

   mib[0] = CTL_HW;
   mib[1] = HW_AVAILCPU;
   sysctl(mib, 2, &cores, &len, NULL, 0);
   if(cores < 1)
   {
      mib[1] = HW_NCPU;
      sysctl(mib, 2, &cores, &len, NULL, 0);
   }
# elif defined(__linux__) || (__linux) || (defined (__SVR4) && defined (__sun))
   // also for AIX
   cores = sysconf( _SC_NPROCESSORS_ONLN );

# elif defined(IRIX)
   cores = sysconf( _SC_NPROC_ONLN );

# elif defined(HPUX)
   cores = mpctl(MPC_GETNUMSPUS, NULL, NULL);

# elif defined( WIN32 )
   SYSTEM_INFO sysinfo;

   GetSystemInfo( &sysinfo );
   cores = sysinfo.dwNumberOfProcessors;
# endif
   return (cores > 0) ? cores : 1;
}

#endif // defined(__arm__) || defined(_M_ARM)
