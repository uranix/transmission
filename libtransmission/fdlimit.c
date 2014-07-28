/*
 * This file Copyright (C) 2005-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 * $Id$
 */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <sys/time.h> /* getrlimit */
#include <sys/resource.h> /* getrlimit */

#include "transmission.h"
#include "error.h"
#include "fdlimit.h"
#include "file.h"
#include "log.h"
#include "session.h"
#include "torrent.h" /* tr_isTorrent () */

#define dbgmsg(...) \
  do \
    { \
      if (tr_logGetDeepEnabled ()) \
        tr_logAddDeep (__FILE__, __LINE__, NULL, __VA_ARGS__); \
    } \
  while (0)

/***
****
****  Local Files
****
***/

static bool
preallocate_file_sparse (tr_sys_file_t fd, uint64_t length)
{
  const char zero = '\0';
  bool success = 0;

  if (!length)
    success = true;

  if (!success)
    success = tr_sys_file_preallocate (fd, length, TR_SYS_FILE_PREALLOC_SPARSE, NULL);

  if (!success) /* fallback: the old-style seek-and-write */
    {
      /* seek requires signed offset, so length should be in mod range */
      assert (length < 0x7FFFFFFFFFFFFFFFULL);

      success = tr_sys_file_seek (fd, length - 1, TR_SEEK_SET, NULL, NULL) &&
                tr_sys_file_write (fd, &zero, 1, NULL, NULL) &&
                tr_sys_file_truncate (fd, length, NULL);
    }

  return success;
}

static bool
preallocate_file_full (const char * filename, uint64_t length)
{
  bool success = 0;

  tr_sys_file_t fd = tr_sys_file_open (filename, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE, 0666, NULL);
  if (fd != TR_BAD_SYS_FILE)
    {
      success = tr_sys_file_preallocate (fd, length, 0, NULL);

      if (!success) /* if nothing else works, do it the old-fashioned way */
        {
          uint8_t buf[ 4096 ];
          memset (buf, 0, sizeof (buf));
          success = true;
          while (success && (length > 0))
            {
              const uint64_t thisPass = MIN (length, sizeof (buf));
              uint64_t bytes_written;
              success = tr_sys_file_write (fd, buf, thisPass, &bytes_written, NULL) && bytes_written == thisPass;
              length -= thisPass;
            }
        }

      tr_sys_file_close (fd, NULL);
    }

  return success;
}

/*****
******
******
******
*****/

struct tr_cached_file
{
  bool is_writable;
  tr_sys_file_t fd;
  int torrent_id;
  tr_file_index_t file_index;
  time_t used_at;
};

static inline bool
cached_file_is_open (const struct tr_cached_file * o)
{
  assert (o != NULL);

  return o->fd != TR_BAD_SYS_FILE;
}

static void
cached_file_close (struct tr_cached_file * o)
{
  assert (cached_file_is_open (o));

  tr_sys_file_close (o->fd, NULL);
  o->fd = TR_BAD_SYS_FILE;
}

/**
 * returns 0 on success, or an errno value on failure.
 * errno values include ENOENT if the parent folder doesn't exist,
 * plus the errno values set by tr_mkdirp () and tr_sys_file_open ().
 */
static int
cached_file_open (struct tr_cached_file  * o,
                  const char             * filename,
                  bool                     writable,
                  tr_preallocation_mode    allocation,
                  uint64_t                 file_size)
{
  int flags;
  tr_sys_path_info info;
  bool already_existed;
  bool resize_needed;
  tr_error * error = NULL;

  /* create subfolders, if any */
  if (writable)
    {
      char * dir = tr_sys_path_dirname (filename, NULL);
      const int err = tr_mkdirp (dir, 0777) ? errno : 0;
      if (err)
        {
          tr_logAddError (_("Couldn't create \"%1$s\": %2$s"), dir, tr_strerror (err));
          tr_free (dir);
          return err;
        }
      tr_free (dir);
    }

  already_existed = tr_sys_path_get_info (filename, 0, &info, NULL) && info.type == TR_SYS_PATH_IS_FILE;

  if (writable && !already_existed && (allocation == TR_PREALLOCATE_FULL))
    if (preallocate_file_full (filename, file_size))
      tr_logAddDebug ("Preallocated file \"%s\"", filename);

  /* we can't resize the file w/o write permissions */
  resize_needed = already_existed && (file_size < info.size);
  writable |= resize_needed;

  /* open the file */
  flags = writable ? (TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE) : 0;
  flags |= TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL;
  o->fd = tr_sys_file_open (filename, flags, 0666, &error);

  if (o->fd == TR_BAD_SYS_FILE)
    {
      const int err = error->code;
      tr_logAddError (_("Couldn't open \"%1$s\": %2$s"), filename, error->message);
      tr_error_free (error);
      return err;
    }

  /* If the file already exists and it's too large, truncate it.
   * This is a fringe case that happens if a torrent's been updated
   * and one of the updated torrent's files is smaller.
   * http://trac.transmissionbt.com/ticket/2228
   * https://bugs.launchpad.net/ubuntu/+source/transmission/+bug/318249
   */
  if (resize_needed && !tr_sys_file_truncate (o->fd, file_size, &error))
    {
      const int err = error->code;
      tr_logAddError (_("Couldn't truncate \"%1$s\": %2$s"), filename, error->message);
      tr_error_free (error);
      return err;
    }

  if (writable && !already_existed && (allocation == TR_PREALLOCATE_SPARSE))
    preallocate_file_sparse (o->fd, file_size);

  return 0;
}

/***
****
***/

struct tr_fileset
{
  struct tr_cached_file * begin;
  const struct tr_cached_file * end;
};

static void
fileset_construct (struct tr_fileset * set, int n)
{
  struct tr_cached_file * o;
  const struct tr_cached_file TR_CACHED_FILE_INIT = { false, TR_BAD_SYS_FILE, 0, 0, 0 };

  set->begin = tr_new (struct tr_cached_file, n);
  set->end = set->begin + n;

  for (o=set->begin; o!=set->end; ++o)
    *o = TR_CACHED_FILE_INIT;
}

static void
fileset_close_all (struct tr_fileset * set)
{
  struct tr_cached_file * o;

  if (set != NULL)
    for (o=set->begin; o!=set->end; ++o)
      if (cached_file_is_open (o))
        cached_file_close (o);
}

static void
fileset_destruct (struct tr_fileset * set)
{
  fileset_close_all (set);
  tr_free (set->begin);
  set->end = set->begin = NULL;
}

static void
fileset_close_torrent (struct tr_fileset * set, int torrent_id)
{
  struct tr_cached_file * o;

  if (set != NULL)
    for (o=set->begin; o!=set->end; ++o)
      if ((o->torrent_id == torrent_id) && cached_file_is_open (o))
        cached_file_close (o);
}

static struct tr_cached_file *
fileset_lookup (struct tr_fileset * set, int torrent_id, tr_file_index_t i)
{
  struct tr_cached_file * o;

  if (set != NULL)
    for (o=set->begin; o!=set->end; ++o)
      if ((torrent_id == o->torrent_id) && (i == o->file_index) && cached_file_is_open (o))
        return o;

  return NULL;
}

static struct tr_cached_file *
fileset_get_empty_slot (struct tr_fileset * set)
{
  struct tr_cached_file * cull = NULL;

  if (set->begin != NULL)
    {
      struct tr_cached_file * o;

      /* try to find an unused slot */
      for (o=set->begin; o!=set->end; ++o)
        if (!cached_file_is_open (o))
          return o;

      /* all slots are full... recycle the least recently used */
      for (cull=NULL, o=set->begin; o!=set->end; ++o)
        if (!cull || o->used_at < cull->used_at)
          cull = o;

      cached_file_close (cull);
    }

  return cull;
}

/***
****
****  Startup / Shutdown
****
***/

struct tr_fdInfo
{
  int peerCount;
  struct tr_fileset fileset;
};

static void
ensureSessionFdInfoExists (tr_session * session)
{
  assert (tr_isSession (session));

  if (session->fdInfo == NULL)
    {
      struct rlimit limit;
      struct tr_fdInfo * i;
      const int FILE_CACHE_SIZE = 32;

      /* Create the local file cache */
      i = tr_new0 (struct tr_fdInfo, 1);
      fileset_construct (&i->fileset, FILE_CACHE_SIZE);
      session->fdInfo = i;

      /* set the open-file limit to the largest safe size wrt FD_SETSIZE */
      if (!getrlimit (RLIMIT_NOFILE, &limit))
        {
          const int old_limit = (int) limit.rlim_cur;
          const int new_limit = MIN (limit.rlim_max, FD_SETSIZE);
          if (new_limit != old_limit)
            {
              limit.rlim_cur = new_limit;
              setrlimit (RLIMIT_NOFILE, &limit);
              getrlimit (RLIMIT_NOFILE, &limit);
              tr_logAddInfo ("Changed open file limit from %d to %d", old_limit, (int)limit.rlim_cur);
            }
        }
    }
}

void
tr_fdClose (tr_session * session)
{
  if (session && session->fdInfo)
    {
      struct tr_fdInfo * i = session->fdInfo;
      fileset_destruct (&i->fileset);
      tr_free (i);
      session->fdInfo = NULL;
    }
}

/***
****
***/

static struct tr_fileset*
get_fileset (tr_session * session)
{
  if (!session)
    return NULL;

  ensureSessionFdInfoExists (session);
  return &session->fdInfo->fileset;
}

void
tr_fdFileClose (tr_session * s, const tr_torrent * tor, tr_file_index_t i)
{
  struct tr_cached_file * o;

  if ((o = fileset_lookup (get_fileset (s), tr_torrentId (tor), i)))
    {
      /* flush writable files so that their mtimes will be
       * up-to-date when this function returns to the caller... */
      if (o->is_writable)
        tr_sys_file_flush (o->fd, NULL);

      cached_file_close (o);
    }
}

tr_sys_file_t
tr_fdFileGetCached (tr_session * s, int torrent_id, tr_file_index_t i, bool writable)
{
  struct tr_cached_file * o = fileset_lookup (get_fileset (s), torrent_id, i);

  if (!o || (writable && !o->is_writable))
    return TR_BAD_SYS_FILE;

  o->used_at = tr_time ();
  return o->fd;
}

bool
tr_fdFileGetCachedMTime (tr_session * s, int torrent_id, tr_file_index_t i, time_t * mtime)
{
  bool success;
  tr_sys_path_info info;
  struct tr_cached_file * o = fileset_lookup (get_fileset (s), torrent_id, i);

  if ((success = (o != NULL) && tr_sys_file_get_info (o->fd, &info, NULL)))
    *mtime = info.last_modified_at;

  return success;
}

void
tr_fdTorrentClose (tr_session * session, int torrent_id)
{
  assert (tr_sessionIsLocked (session));

  fileset_close_torrent (get_fileset (session), torrent_id);
}

/* returns an fd on success, or a TR_BAD_SYS_FILE on failure and sets errno */
tr_sys_file_t
tr_fdFileCheckout (tr_session             * session,
                   int                      torrent_id,
                   tr_file_index_t          i,
                   const char             * filename,
                   bool                     writable,
                   tr_preallocation_mode    allocation,
                   uint64_t                 file_size)
{
  struct tr_fileset * set = get_fileset (session);
  struct tr_cached_file * o = fileset_lookup (set, torrent_id, i);

  if (o && writable && !o->is_writable)
    cached_file_close (o); /* close it so we can reopen in rw mode */
  else if (!o)
    o = fileset_get_empty_slot (set);

  if (!cached_file_is_open (o))
    {
      const int err = cached_file_open (o, filename, writable, allocation, file_size);
      if (err)
        {
          errno = err;
          return TR_BAD_SYS_FILE;
        }

      dbgmsg ("opened '%s' writable %c", filename, writable?'y':'n');
      o->is_writable = writable;
    }

  dbgmsg ("checking out '%s'", filename);
  o->torrent_id = torrent_id;
  o->file_index = i;
  o->used_at = tr_time ();
  return o->fd;
}

/***
****
****  Sockets
****
***/

int
tr_fdSocketCreate (tr_session * session, int domain, int type)
{
  int s = -1;
  struct tr_fdInfo * gFd;
  assert (tr_isSession (session));

  ensureSessionFdInfoExists (session);
  gFd = session->fdInfo;

  if (gFd->peerCount < session->peerLimit)
    if ((s = socket (domain, type, 0)) < 0)
      if (sockerrno != EAFNOSUPPORT)
        tr_logAddError (_("Couldn't create socket: %s"), tr_strerror (sockerrno));

  if (s > -1)
    ++gFd->peerCount;

  assert (gFd->peerCount >= 0);

  if (s >= 0)
    {
      static bool buf_logged = false;
      if (!buf_logged)
        {
          int i;
          socklen_t size = sizeof (int);
          buf_logged = true;
          getsockopt (s, SOL_SOCKET, SO_SNDBUF, &i, &size);
          tr_logAddDebug ("SO_SNDBUF size is %d", i);
          getsockopt (s, SOL_SOCKET, SO_RCVBUF, &i, &size);
          tr_logAddDebug ("SO_RCVBUF size is %d", i);
        }
    }

  return s;
}

int
tr_fdSocketAccept (tr_session * s, int sockfd, tr_address * addr, tr_port * port)
{
  int fd;
  unsigned int len;
  struct tr_fdInfo * gFd;
  struct sockaddr_storage sock;

  assert (tr_isSession (s));
  assert (addr);
  assert (port);

  ensureSessionFdInfoExists (s);
  gFd = s->fdInfo;

  len = sizeof (struct sockaddr_storage);
  fd = accept (sockfd, (struct sockaddr *) &sock, &len);

  if (fd >= 0)
    {
      if ((gFd->peerCount < s->peerLimit)
          && tr_address_from_sockaddr_storage (addr, port, &sock))
        {
          ++gFd->peerCount;
        }
        else
        {
          tr_netCloseSocket (fd);
          fd = -1;
        }
    }

  return fd;
}

void
tr_fdSocketClose (tr_session * session, int fd)
{
  assert (tr_isSession (session));

  if (session->fdInfo != NULL)
    {
      struct tr_fdInfo * gFd = session->fdInfo;

      if (fd >= 0)
        {
          tr_netCloseSocket (fd);
          --gFd->peerCount;
        }

      assert (gFd->peerCount >= 0);
    }
}
