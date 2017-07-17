/*
 * Copyright 2005-2017 by Erik Hofman.
 * Copyright 2009-2017 by Adalin B.V.
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
# include <string.h>
#endif

#include <xml.h>

#include <base/dlsym.h>

#include <api.h>
#include <arch.h>

#include "extension.h"
#include "format.h"
#include "fmt_flac.h"

#define FRAME_SIZE 960
#define MAX_FRAME_SIZE 6*960
#define MAX_PACKET_SIZE (3*1276)

typedef struct
{
   void *id;
   char *artist;
   char *original;
   char *title;
   char *album;
   char *trackno;
   char *date;
   char *genre;
   char *composer;
   char *comments;
   char *copyright;
   char *website;
   char *image;

   int mode;

   char capturing;

   uint8_t no_tracks;
   uint8_t bits_sample;
   int frequency;
   int bitrate;
   int blocksize;
   enum aaxFormat format;
   size_t no_samples;
   size_t max_samples;

   ssize_t flacBufPos;
   size_t flacBufSize;
   int32_t *flacBuffer;
   char *flacptr;

} _driver_t;

static void _flac_metafn(void*, drflac_metadata*);


int
_flac_detect(_fmt_t *fmt, int mode)
{
   int rv = AAX_FALSE;

   /* not required but useful */
   if (mode == 0)
   {
      fmt->id = calloc(1, sizeof(_driver_t));
      if (fmt->id)
      {
         _driver_t *handle = fmt->id;

         handle->mode = mode;
         handle->capturing = (mode == 0) ? 1 : 0;
         handle->blocksize = 4096;
         handle->format = AAX_PCM32S;
         handle->bits_sample = aaxGetBitsPerSample(handle->format);

         rv = AAX_TRUE;
      }             
      else {
         _AAX_FILEDRVLOG("FLAC: Insufficient memory");
      }
   }

   return rv;
}

void*
_flac_open(_fmt_t *fmt, void *buf, size_t *bufsize, size_t fsize)
{
   _driver_t *handle = fmt->id;
   void *rv = NULL;

   assert(bufsize);

   if (handle)
   {
      if (!handle->id)
      {
         char *ptr = 0;

         handle->flacBufPos = 0;
         handle->flacBufSize = MAX_PACKET_SIZE;
         handle->flacptr = _aax_malloc(&ptr, handle->flacBufSize);
         handle->flacBuffer = (int32_t*)ptr;

         memcpy(handle->flacBuffer, buf, *bufsize);
         handle->flacBufPos += *bufsize;

         handle->id = drflac_open_memory_with_metadata(handle->flacBuffer,
                                                       handle->flacBufPos,
                                                       _flac_metafn, handle);
         if (handle->id) {
            rv = buf;
         }
      }
   }
   else
   {
      _AAX_FILEDRVLOG("FLAC: Internal error: handle id equals 0");
   }

   return rv;
}

void
_flac_close(_fmt_t *fmt)
{
   _driver_t *handle = fmt->id;

   if (handle)
   {
      drflac_close(handle->id);
      handle->id = NULL;

      free(handle->flacptr);

      free(handle->trackno);
      free(handle->artist);
      free(handle->title);
      free(handle->album);
      free(handle->date);
      free(handle->genre);
      free(handle->comments);
      free(handle->composer);
      free(handle->copyright);
      free(handle->original);
      free(handle->website);
      free(handle->image);
      free(handle);
   }
}

int
_flac_setup(_fmt_t *fmt, _fmt_type_t pcm_fmt, enum aaxFormat aax_fmt)
{
   return AAX_TRUE;
}

size_t
_flac_fill(_fmt_t *fmt, void_ptr sptr, size_t *bytes)
{
   _driver_t *handle = fmt->id;
   size_t bufpos, bufsize;
   size_t rv = 0;

   bufpos = handle->flacBufPos;
   bufsize = handle->flacBufSize;
   if ((*bytes+bufpos) <= bufsize)
   {
      char *buf = (char *)handle->flacBuffer + bufpos;

      memcpy(buf, sptr, *bytes);
      handle->flacBufPos += *bytes;
      rv = *bytes;
   }

   return rv;
}

size_t
_flac_copy(_fmt_t *fmt, int32_ptr dptr, size_t dptr_offs, size_t *num)
{
   _driver_t *handle = fmt->id;
   unsigned int bits, tracks;
   size_t bytes, bufsize;
   size_t rv = __F_EOF;
   char *buf;

   tracks = handle->no_tracks;
   bits = handle->bits_sample;
   bytes = *num*tracks*bits/8;

   buf = (char*)handle->flacBuffer;
   bufsize = handle->flacBufSize;

   if (bytes > bufsize) {
      bytes = bufsize;
   }

   if (bytes)
   {
      memcpy(buf+handle->flacBufPos, dptr, bytes);
      handle->flacBufPos += bytes;
      rv = bytes;
   }

   return rv;
}

size_t
_flac_cvt_from_intl(_fmt_t *fmt, int32_ptrptr dptr, size_t offset, size_t *num)
{
   _driver_t *handle = fmt->id;
   size_t bytes, bufsize, size = 0;
   unsigned int bits, tracks;
   size_t rv = __F_EOF;
   int32_t *buf;
   int ret;

   tracks = handle->no_tracks;
   bits = handle->bits_sample;
   bytes = *num*tracks*bits/8;

   buf = handle->flacBuffer;
   bufsize = handle->flacBufSize;

   if (bytes > bufsize)
   {
      bytes = bufsize;
      *num = bytes*8/(tracks*bits);
   }

   // Returns the number of samples actually read.
   ret = drflac_read_s32(handle->id, *num, buf);
#if 0
// ret = pflac_decode(handle->id, buf, bytes, handle->pcm, *num, decode_fec);
   if (ret > 0)
   {
      unsigned int framesize = tracks*bits/8;

      *num = size/framesize;
      _batch_cvt24_32_intl(dptr, handle->pcm, offset, tracks, *num);

      handle->no_samples += *num;
      rv = size;
   }
#endif

   return rv;
}

size_t
_flac_cvt_to_intl(_fmt_t *fmt, void_ptr dptr, const_int32_ptrptr sptr, size_t offs, size_t *num, void_ptr scratch, size_t scratchlen)
{
   int res = 0;
   return res;
}

char*
_flac_name(_fmt_t *fmt, enum _aaxStreamParam param)
{
   return NULL;
}

off_t
_flac_get(_fmt_t *fmt, int type)
{
   _driver_t *handle = fmt->id;
   off_t rv = 0;

   switch(type)
   {
   case __F_FMT:
      rv = handle->format;
      break;
   case __F_TRACKS:
      rv = handle->no_tracks;
      break;
   case __F_FREQ:
      rv = handle->frequency;
      break;
   case __F_BITS:
      rv = handle->bits_sample;
      break;
   case __F_BLOCK:
      rv = handle->blocksize;
      break;
   case __F_SAMPLES:
      rv = handle->max_samples;
      break;
   default:
      if (type & __F_NAME_CHANGED)
      {
         switch (type & ~__F_NAME_CHANGED)
         {
         default:
            break;
         }
      }
      break;
   }
   return rv;
}

off_t
_flac_set(_fmt_t *fmt, int type, off_t value)
{
   _driver_t *handle = fmt->id;
   off_t rv = 0;

   switch(type)
   {
   case __F_FREQ:
      handle->frequency = value;
      break;
   case __F_RATE:
      handle->bitrate = value;
      break;
   case __F_TRACKS:
      handle->no_tracks = value;
      break;
   case __F_SAMPLES:
      handle->no_samples = value;
      handle->max_samples = value;
      break;
   case __F_BITS:
      handle->bits_sample = value;
      break;
   case __F_IS_STREAM:
      break;
   case __F_POSITION:
      break;
   default:
      break;
   }
   return rv;
}

/* -------------------------------------------------------------------------- */
#define COMMENT_SIZE	1024

static void
_flac_metafn(void *userData, drflac_metadata *metaData)
{
   _driver_t *handle = (_driver_t *)userData;

   switch(metaData->type)
   {
   case DRFLAC_METADATA_BLOCK_TYPE_STREAMINFO:
      handle->blocksize = metaData->data.streaminfo.maxBlockSize;
      handle->frequency = metaData->data.streaminfo.sampleRate;
      handle->no_tracks = metaData->data.streaminfo.channels;
      handle->bits_sample = metaData->data.streaminfo.bitsPerSample;
      handle->max_samples = metaData->data.streaminfo.totalSampleCount;
      break;
   case DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT:
   {
	// https://xiph.org/vorbis/doc/v-comment.html
      drflac_vorbis_comment_iterator it;
      char s[COMMENT_SIZE+1];
      const char *ptr;
      uint32_t slen;
#if 0
      ptr = metaData->data.vorbis_comment.vendor;
      slen = metaData->data.vorbis_comment.vendorLength;
      snprintf(s, _MIN(slen+1, COMMENT_SIZE), "%s\0", ptr);
#endif
      while ((ptr = drflac_next_vorbis_comment(&it, &slen)) != NULL)
      {
         snprintf(s, _MIN(slen+1, COMMENT_SIZE), "%s\0", ptr);
         if (!STRCMP(s, "TITLE")) {
            handle->title = strdup(s+strlen("TITLE="));
         } else if (!STRCMP(s, "ALBUM")) {
            handle->album = strdup(s+strlen("ALBUM="));
         } else if (!STRCMP(s, "TRACKNUMBER")) {
            handle->trackno = strdup(s+strlen("TRACKNUMBER="));
         } else if (!STRCMP(s, "ARTIST")) {
            handle->artist = strdup(s+strlen("ARTIST="));
         } else if (!STRCMP(s, "PERFORMER")) {
            handle->artist = strdup(s+strlen("PERFORMER="));
         } else if (!STRCMP(s, "COPYRIGHT")) {
            handle->copyright = strdup(s+strlen("COPYRIGHT="));
         } else if (!STRCMP(s, "GENRE")) {
            handle->genre = strdup(s+strlen("GENRE="));
         } else if (!STRCMP(s, "DATE")) {
            handle->date = strdup(s+strlen("DATE="));
         } else if (!STRCMP(s, "ORGANIZATION")) {
            handle->composer= strdup(s+strlen("ORGANIZATION="));
         } else if (!STRCMP(s, "DESCRIPTION")) {
            handle->comments = strdup(s+strlen("DESCRIPTION="));
         } else if (!STRCMP(s, "CONTACT")) {
            handle->website =  strdup(s+strlen("CONTACT="));
         }
      }
      break;
   }
   case DRFLAC_METADATA_BLOCK_TYPE_APPLICATION:
   case DRFLAC_METADATA_BLOCK_TYPE_SEEKTABLE:
   case DRFLAC_METADATA_BLOCK_TYPE_CUESHEET:
   case DRFLAC_METADATA_BLOCK_TYPE_PICTURE:
   case DRFLAC_METADATA_BLOCK_TYPE_PADDING:
   default:
      break;
   }
}