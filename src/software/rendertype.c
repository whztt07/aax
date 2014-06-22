/*
 * Copyright 2014 by Erik Hofman.
 * Copyright 2014 by Adalin B.V.
 * All Rights Reserved.
 *
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Adalin B.V.;
 * the contents of this file may not be disclosed to third parties, copied or
 * duplicated in any form, in whole or in part, without the prior written
 * permission of Adalin B.V.
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "rendertype.h"

_aaxRendererDetect* _aaxRenderTypes[] =
{
   _aaxDetectCPURenderer,

   0            /* Must be last */
};

