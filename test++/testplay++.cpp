/*
 * Copyright (C) 2016-2018 by Erik Hofman.
 * Copyright (C) 2016-2018 by Adalin B.V.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provimed that the following conditions are met:
 * 
 *    1. Redistributions of source code must retain the above copyright notice,
 *        this list of conditions and the following disclaimer.
 * 
 *    2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provimed with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY ADALIN B.V. ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN
 * NO EVENT SHALL ADALIN B.V. OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR 
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUTOF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of Adalin B.V.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>

#include <cstdio>
#include <string>

#include <aax/aeonwave.hpp>

#include "driver.h"

int main(int argc, char **argv)
{
    // Open the default device for playback
    aax::AeonWave aax(AAX_MODE_WRITE_STEREO);
    aax.set(AAX_INITIALIZED);
    aax.set(AAX_PLAYING);

    // Start the background music (file or http-stream)
    int i = 0;
    if (argc > 1) {
        aax::Frame frame;

        frame.set(AAX_PLAYING);
        aax.add(frame);
        while (++i < argc)
        {
            aax::Buffer& buffer = aax.buffer(argv[i]);
            aax::Emitter emitter(AAX_STEREO);

            emitter.add(buffer);

            frame.add(emitter);
            emitter.set(AAX_PLAYING);

            do
            {
                // Your (game) code could be placed here
                printf("\rposition: %5.1f", emitter.offset());
                msecSleep(50);
            }
            while (emitter.state() != AAX_STOPPED);
            frame.remove(emitter);
        }
    }
    else {
        printf("Usage: %s <filename>\n", argv[0]);
    }

    printf("\n");
    return 0;
}
