/*
 * Copyright 2015 by Erik Hofman.
 * Copyright 2015 by Adalin B.V.
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

#ifdef HAVE_UNISTD_H
# include <unistd.h>           /* read, write, close, lseek, access */
#endif
#ifdef HAVE_RMALLOC_H
# include <rmalloc.h>
#else
# include <string.h>
#endif
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>

#include <base/types.h>

#include "audio.h"

int
_socket_open(const char *server, int oflags, ...)
{
   int port = 0;
   int fd = -1;
   va_list ap;

   va_start(ap, oflags);
   if (oflags >= 1) {
      port = va_arg(ap, int);
   }
   va_end(ap);

   if (server && (oflags == O_RDWR) && (port > 0))
   {
      int slen = strlen(server);
      if (slen < 256)
      {
         struct addrinfo hints, *host;
         char sport[16];
         int res;

         snprintf(sport, 15, "%d", port);
         sport[15] = '\0';

         memset(&hints, 0, sizeof hints);
         hints.ai_socktype = SOCK_STREAM;
         res = getaddrinfo(server, (port > 0) ? sport : NULL, &hints, &host);
         if (res == 0)
         {
            fd = socket(host->ai_family, host->ai_socktype, host->ai_protocol);
            if (fd >= 0)
            {
               setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, 0, 0);
               if (connect(fd, host->ai_addr, host->ai_addrlen) < 0)
               {
                  close(fd);
                  fd = -1;
               }
            }
            freeaddrinfo(host);
         }
         else {
            errno = -res;
         }
      }
      else {
         errno = ENAMETOOLONG;
      }
   }
   else {
      errno = EACCES;
   }

   return fd;
}

int
_socket_close(int fd)
{
   return close(fd);
}

ssize_t
_socket_read(int fd, void *buf, size_t size)
{
   return read(fd, buf, size);
}

ssize_t
_socket_write(int fd, const void *buf, size_t size)
{
   return write(fd, buf, size);
}

off_t
_socket_seek(int fd, off_t offs, int whence)
{
   errno = EPERM;
   return (off_t)-1;
}

int
_socket_stat(int fd, void *stat)
{
   errno = EPERM;
   return -1;
}

/* NOTE: modifies url, make sure to strdup it before calling this function */
_protocol_t
_url_split(char *url, char **protocol, char **server, char **path, int *port)
{
   _protocol_t rv;
   char *ptr;

   *protocol = NULL;
   *server = NULL;
   *path = NULL;
   *port = 0;

   ptr = strstr(url, "://");
   if (ptr)
   {
      *protocol = (char*)url;
      *ptr = '\0';
      url = ptr + strlen("://");
   }
   else if (access(url, F_OK) != -1) {
      *path = url;
   }

   if (!*path)
   {
      *server = url;

      ptr = strchr(url, '/');
      if (ptr)
      {
         *ptr++ = '\0';
         *path = ptr;
      }

      ptr = strchr(url, ':');
      if (ptr)
      {
         *ptr++ = '\0';
         *port = atoi(ptr);
      }
   }

   if ((*protocol && !strcasecmp(*protocol, "http")) || *server) {
      rv = PROTOCOL_HTTP;
      if (*port <= 0) *port = 80;
   }
   else if (!*protocol || !strcasecmp(*protocol, "file")) {
      rv = PROTOCOL_FILE;
   } else {
      rv = PROTOCOL_UnSUPPORTED;
   }

   return rv;
}

/* -------------------------------------------------------------------------- */

#define MAX_BUFFER	512

static int
http_get_response_data(_io_t *io, int fd, char *buf, int size)
{
   int i = 0;
   while (i < size)
   {
      if (io->read(fd, buf, 1) != 1)
      {
         i = -i;
         break;
      }
      ++i;
      if (*buf == '\r') continue;       // ignore CR
      if (*buf == '\n') break;          // end of line
      ++buf;
   }
   *buf = '\0';
   return i;
}

int
http_send_request(_io_t *io, int fd, const char *command, const char *server, const char *path, const char *extra, const char *user_agent)
{
   char header[MAX_BUFFER];
   int hlen, rv = 0;

   if (extra && *extra != '\0')
   {
      snprintf(header, MAX_BUFFER,
              "%s /%.256s HTTP/1.0\r\n"
              "User-Agent: %s\r\n"
              "Host: %s\r\n"
              "%s\r\n"
              "\r\n",
              command, path, user_agent, server, extra);
   }
   else
   {
       snprintf(header, MAX_BUFFER,
               "%s /%.256s HTTP/1.0\r\n"
               "User-Agent: %s\r\n"
               "Host: %s\r\n"
               "\r\n",
               command, path, user_agent, server);
   }
   header[MAX_BUFFER-1] = '\0';

   hlen = strlen(header);
   rv = io->write(fd, header, hlen);
   if (rv != hlen) {
      rv = -1;
   }

   return rv;
}

int
http_get_response(_io_t *io, int fd, char *buf, int size)
{
   int res, rv = http_get_response_data(io, fd, buf, size);
   if (rv > 0)
   {
      int res = sscanf(buf, "HTTP/1.%*d %03d", (int*)&rv);
      if (res != 1)
      {
         res =  sscanf(buf, "ICY %03d", (int*)&rv);
         if (res != 1) {
            rv = -1;
         }
      }
   }
#if 0
   if (rv != 200) {
      io->read(fd, buf, size);
   }
#endif
   return rv;
}
