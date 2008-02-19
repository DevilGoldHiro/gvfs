/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* GVFS cdrom audio file system driver
 * 
 * Copyright (C) 2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

/* NOTE: since we link the libcdio libs (GPLv2) into our process space
 * the combined work is GPLv2. This source file, however, is LGPLv2+.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "gvfsbackendcdda.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobenumerate.h"

#define DO_NOT_WANT_PARANOIA_COMPATIBILITY
#include <cdio/paranoia.h>

/* TODO:
 *
 * - GVFS integration
 *   - Need to unmount ourselves when the backing media is removed
 *   - Need to have some way of making our resulting GDaemonMount object (in the client address space)
 *     be associated with the GVolume (probably stems from the HAL volume monitor).. both ways
 *   - g_vfs_backend_set_display_name() needs to work post mount
 *
 * - Metadata
 *   - Use CD-Text to read metadata from the physical disc
 *     - http://en.wikipedia.org/wiki/CD-Text
 *     - libcdio can do this
 *   - Use Musicbrainz to read metadata from the net
 *     - libmusicbrainz appear to be a dead-end: http://musicbrainz.org/doc/libmusicbrainz
 *     - Need to provide some UI for configuring musicbrainz; think proxy, local server, 
 *       lockdown (secure facilities don't want us to randomly connect to the Interwebs)
 *   - Ideally use libjuicer for all this
 *     - however it is currently private to sound-juicer and brings in GTK+, gnome-vfs, gconf...
 *   - Use metadata for file names and display_name of our Mount (using g_vfs_backend_set_display_name())
 *   - Also encode metadata in the WAV header so transcoding to Vorbis or MP3 Just Works(tm)
 *     - This is already done; see create_header() in this file
 *   - see thread on gtk-devel-list for a plan
 *
 * - Scratched discs / error conditions from paranoia
 *   - Need to handle this better... ideally caller passes a flag when opening the file to
 *     specify whether he wants us to try hard to get the hard result (ripping) or whether 
 *     he's fine with some noise (playback)
 *
 * - Sector cache? Might be useful to maintain a cache of previously read sectors
 */

/*--------------------------------------------------------------------------------------------------------------*/

struct _GVfsBackendCdda
{
  GVfsBackend parent_instance;

  char *device_path;
  cdrom_drive_t *drive;
  int num_open_files;
};

G_DEFINE_TYPE (GVfsBackendCdda, g_vfs_backend_cdda, G_VFS_TYPE_BACKEND)

static void
g_vfs_backend_cdda_finalize (GObject *object)
{
  GVfsBackendCdda *cdda_backend = G_VFS_BACKEND_CDDA (object);

  //g_warning ("finalizing %p", object);

  g_free (cdda_backend->device_path);

  if (G_OBJECT_CLASS (g_vfs_backend_cdda_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_cdda_parent_class)->finalize) (object);
}

static void
g_vfs_backend_cdda_init (GVfsBackendCdda *cdda_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (cdda_backend);
  GMountSpec *mount_spec;

  //g_warning ("initing %p", cdda_backend);

  g_vfs_backend_set_display_name (backend, "cdda");
  // TODO: HMM: g_vfs_backend_set_user_visible (backend, FALSE);  

  mount_spec = g_mount_spec_new ("cdda");
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  g_mount_spec_unref (mount_spec);
}

static void
do_mount (GVfsBackend *backend,
	  GVfsJobMount *job,
	  GMountSpec *mount_spec,
	  GMountSource *mount_source,
	  gboolean is_automount)
{
  char *fuse_name;
  char *display_name;
  const char *host;
  GVfsBackendCdda *cdda_backend = G_VFS_BACKEND_CDDA (backend);
  GError *error = NULL;
  GMountSpec *cdda_mount_spec;

  //g_warning ("do_mount %p", cdda_backend);

  host = g_mount_spec_get (mount_spec, "host");
  //g_warning ("host=%s", host);
  if (host == NULL)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", _("No drive specified"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  cdda_backend->device_path = g_strdup_printf ("/dev/%s", host);

  cdda_backend->drive = cdio_cddap_identify (cdda_backend->device_path, 0, NULL);
  if (cdda_backend->drive == NULL)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, 
                   _("Cannot find drive %s"), cdda_backend->device_path);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  if (cdio_cddap_open (cdda_backend->drive) != 0)
    {
      cdio_cddap_close (cdda_backend->drive);
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, 
                   _("Drive %s does not contain audio files"), cdda_backend->device_path);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  /* Translator: %s is the device the disc is inserted into */
  fuse_name = g_strdup_printf (_("Audio Disc on %s"), host);
  display_name = g_strdup_printf (_("Audio Disc"));
  g_vfs_backend_set_stable_name (backend, fuse_name);
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);
  g_free (fuse_name);
  g_vfs_backend_set_icon_name (backend, "media-optical-audio");

  g_vfs_job_succeeded (G_VFS_JOB (job));

  cdda_mount_spec = g_mount_spec_new ("cdda");
  g_mount_spec_set (cdda_mount_spec, "host", host);
  g_vfs_backend_set_mount_spec (backend, cdda_mount_spec);
  g_mount_spec_unref (cdda_mount_spec);

  //g_warning ("mounted %p", cdda_backend);
}

static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  const char *host;
  GError *error = NULL;
  GMountSpec *cdda_mount_spec;

  //g_warning ("try_mount %p", backend);

  /* TODO: Hmm.. apparently we have to set the mount spec in
   * try_mount(); doing it in mount() won't work.. 
   */
  host = g_mount_spec_get (mount_spec, "host");
  //g_warning ("tm host=%s", host);
  if (host == NULL)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", _("No drive specified"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return TRUE;
    }

  cdda_mount_spec = g_mount_spec_new ("cdda");
  g_mount_spec_set (cdda_mount_spec, "host", host);
  g_vfs_backend_set_mount_spec (backend, cdda_mount_spec);
  g_mount_spec_unref (cdda_mount_spec);
  return FALSE;
}


static void
do_unmount (GVfsBackend *backend,
            GVfsJobUnmount *job)
{
  GError *error;
  GVfsBackendCdda *cdda_backend = G_VFS_BACKEND_CDDA (backend);

  if (cdda_backend->num_open_files > 0)
    {
      error = g_error_new (G_IO_ERROR, G_IO_ERROR_BUSY, 
                           ngettext ("File system is busy: %d open file",
                                     "File system is busy: %d open files",
                                     cdda_backend->num_open_files),
                           cdda_backend->num_open_files);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      return;
    }

  if (cdda_backend->drive != NULL)
    {
      //g_warning ("closed drive %p", backend);
      cdio_cddap_close (cdda_backend->drive);
    }
  
  //g_warning ("unmounted %p", backend);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

/* returns -1 if we couldn't map */
static int
get_track_num_from_name (GVfsBackendCdda *cdda_backend, const char *filename)
{
  int n;
  char *basename;

  basename = g_path_get_basename (filename);
  if (sscanf (basename, "Track %d.wav", &n) == 1)
    {
      g_free (basename);
      return n;
    }

  return -1;
}

typedef struct {
  cdrom_paranoia_t *paranoia;

  long size;           /* size of file being read */
  long header_size;    /* size of the header */
  long content_size;   /* size of content after the header */

  long cursor;         /* cursor into the file being read */

  long first_sector;   /* first sector of raw PCM audio data */
  long last_sector;    /* last sector of raw PCM audio data */
  long sector_cursor;  /* sector we're currently at */

  char *header;        /* header payload */

  /* These following two fields are used for caching the last read sector. This
   * is to avoid seeking back if fewer bytes than whole sector is requested.
   */
  long buf_at_sector_num;                     /* the sector that is cached */
  char buf_at_sector[CDIO_CD_FRAMESIZE_RAW];  /* the data of the sector */

} ReadHandle;

static void
free_read_handle (ReadHandle *read_handle)
{
  if (read_handle->paranoia != NULL)
    cdio_paranoia_free (read_handle->paranoia);
  g_free (read_handle->header);
  g_free (read_handle);
}

static char *
create_header (GVfsBackendCdda *cdda_backend, long *header_size, long content_size)
{
  char *artist;
  char *title;
  const char *software;
  size_t artist_len;
  size_t title_len;
  size_t software_len;
  char *header;
  char *ptr;
  int var;

  /* See http://www.saettler.com/RIFFMCI/riffmci.html for the spec.
   *
   * artist -> IART
   * title -> INAM
   * track_number -> ?? (TODO: work with GStreamer people on coordinate with the wavparse plugin)
   *
   * software -> ISFT
   */


  //artist = g_strdup ("Homer Simpson");
  //title = g_strdup ("Simpsons Jail House Rock");

  /* TODO: fill in from metadata */
  artist = NULL;
  title = NULL;
  software = "gvfs-cdda using libcdio " CDIO_VERSION;

  artist_len = 0;
  title_len = 0;

  /* ensure even length and include room for the chunk */
  if (artist != NULL)
    artist_len = 2 * ((strlen (artist) + 2) / 2) + 8;
  if (title != NULL)
    title_len = 2 * ((strlen (title) + 2) / 2) + 8;
  software_len = 2 * ((strlen (software) + 2) / 2) + 8;

  *header_size = 44;
  *header_size += 12; /* for LIST INFO */
  *header_size += artist_len;
  *header_size += title_len;
  *header_size += software_len;
  header = g_new0 (char, *header_size);

  ptr = header;
  memcpy (ptr, "RIFF", 4); ptr += 4;
  var = content_size + *header_size - 8;
  memcpy (ptr, &var, 4); ptr += 4;
  memcpy (ptr, "WAVE", 4); ptr += 4;

  memcpy (ptr, "fmt ", 4); ptr += 4;
  var = 16;
  memcpy (ptr, &var, 4); ptr += 4;
  var = 1;
  memcpy (ptr, &var, 2); ptr += 2;
  var = 2;
  memcpy (ptr, &var, 2); ptr += 2;
  var = 44100;
  memcpy (ptr, &var, 4); ptr += 4;
  var = 44100 * 2 * 2;
  memcpy (ptr, &var, 4); ptr += 4;
  var = 4;
  memcpy (ptr, &var, 2); ptr += 2;
  var = 16;
  memcpy (ptr, &var, 2); ptr += 2;

  memcpy (ptr, "LIST", 4); ptr += 4;
  var = 4 + artist_len + title_len + software_len;
  memcpy (ptr, &var, 4); ptr += 4;
  memcpy (ptr, "INFO", 4); ptr += 4;

  if (artist != NULL)
    {
      memcpy (ptr, "IART", 4);
      var = artist_len - 8;
      memcpy (ptr + 4, &var, 4);
      strncpy (ptr + 8, artist, artist_len); 
      ptr += artist_len;
    }

  if (title != NULL)
    {
      memcpy (ptr, "INAM", 4);
      var = title_len - 8;
      memcpy (ptr + 4, &var, 4);
      strncpy (ptr + 8, title, title_len); 
      ptr += title_len;
    }

  memcpy (ptr, "ISFT", 4);
  var = software_len - 8;
  memcpy (ptr + 4, &var, 4);
  strncpy (ptr + 8, software, software_len); 
  ptr += software_len;

  memcpy (ptr, "data", 4); ptr += 4;
  memcpy (ptr, &content_size, 4); ptr += 4;

  g_free (artist);
  g_free (title);

  return header;
}

static void
do_open_for_read (GVfsBackend *backend,
                  GVfsJobOpenForRead *job,
                  const char *filename)
{
  int track_num;
  GError *error;
  ReadHandle *read_handle;
  GVfsBackendCdda *cdda_backend = G_VFS_BACKEND_CDDA (backend);

  //g_warning ("open_for_read (%s)", filename);

  read_handle = g_new0 (ReadHandle, 1);

  track_num = get_track_num_from_name (cdda_backend, job->filename);
  if (track_num == -1)
    {
      error = g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, 
                           _("No such file %s on drive %s"), job->filename, cdda_backend->device_path);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      free_read_handle (read_handle);
      return;
    }


  read_handle->first_sector = cdio_cddap_track_firstsector (cdda_backend->drive, track_num);
  read_handle->last_sector = cdio_cddap_track_lastsector (cdda_backend->drive, track_num);
  read_handle->sector_cursor = -1;

  read_handle->cursor = 0;
  read_handle->buf_at_sector_num = -1;
  read_handle->content_size  = ((read_handle->last_sector - read_handle->first_sector) + 1) * CDIO_CD_FRAMESIZE_RAW;

  read_handle->header = create_header (cdda_backend, &(read_handle->header_size), read_handle->content_size);
  read_handle->size = read_handle->header_size + read_handle->content_size;

  read_handle->paranoia = cdio_paranoia_init (cdda_backend->drive);
  cdio_paranoia_modeset (read_handle->paranoia, PARANOIA_MODE_DISABLE);

  cdda_backend->num_open_files++;

  g_vfs_job_open_for_read_set_can_seek (job, TRUE);
  g_vfs_job_open_for_read_set_handle (job, GINT_TO_POINTER (read_handle));
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

/* We have to pass in a callback to paranoia_read, even though we don't use it */
static void 
paranoia_callback (long int inpos, paranoia_cb_mode_t function)
{
}


static void
do_read (GVfsBackend *backend,
         GVfsJobRead *job,
         GVfsBackendHandle handle,
         char *buffer,
         gsize bytes_requested)
{
  GVfsBackendCdda *cdda_backend = G_VFS_BACKEND_CDDA (backend);
  ReadHandle *read_handle = (ReadHandle *) handle;
  int bytes_read;
  long skip_bytes;
  char *readbuf;
  long desired_sector;
  int bytes_to_copy;
  long cursor_in_stream;

  //g_warning ("read (%"G_GSSIZE_FORMAT") (@ %ld)", bytes_requested, read_handle->cursor);

  /* header */
  if (read_handle->cursor < read_handle->header_size)
    {
      skip_bytes = read_handle->cursor;
      bytes_read = read_handle->header_size - read_handle->cursor;
      readbuf = read_handle->header + skip_bytes;
      goto read_data_done;
    }

  /* EOF */
  if (read_handle->cursor >= read_handle->size)
    {
      skip_bytes = 0;
      bytes_read = 0;
      readbuf = NULL;
      goto read_data_done;
    }

  cursor_in_stream = read_handle->cursor - read_handle->header_size;

  desired_sector = cursor_in_stream / CDIO_CD_FRAMESIZE_RAW + read_handle->first_sector;

  if (desired_sector == read_handle->buf_at_sector_num)
    {
      /* got it cached */

      /* skip some bytes */
      skip_bytes = cursor_in_stream - (desired_sector - read_handle->first_sector) * CDIO_CD_FRAMESIZE_RAW;
      readbuf = read_handle->buf_at_sector + skip_bytes;
      bytes_read = CDIO_CD_FRAMESIZE_RAW - skip_bytes;

      //g_warning ("read from cache for cursor @ %ld", read_handle->buf_at_sector_num);
    }
  else
    {
      /* first check that we're at the right sector */
      if (desired_sector != read_handle->sector_cursor)
        {
          cdio_paranoia_seek (read_handle->paranoia, desired_sector, SEEK_SET);
          read_handle->sector_cursor = desired_sector;
          //g_warning ("seeking cursor to %ld", read_handle->sector_cursor);
        }
      
      /* skip some bytes */
      skip_bytes = cursor_in_stream - (read_handle->sector_cursor - read_handle->first_sector) * CDIO_CD_FRAMESIZE_RAW;
      //g_warning ("advanced cursor to %ld", read_handle->sector_cursor);
      
      readbuf = (char *) cdio_paranoia_read (read_handle->paranoia, paranoia_callback);

      if (readbuf == NULL)
        {
          int errsv = errno;

          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            g_io_error_from_errno (errsv),
                            /* Translators: paranoia is the name of the cd audio reading library */
                            _("Error from 'paranoia' on drive %s"), cdda_backend->device_path);
          return;
        }

      read_handle->buf_at_sector_num = read_handle->sector_cursor;
      memcpy (read_handle->buf_at_sector, readbuf, CDIO_CD_FRAMESIZE_RAW);

      read_handle->sector_cursor++;

      readbuf += skip_bytes;
      bytes_read = CDIO_CD_FRAMESIZE_RAW - skip_bytes;


    }
  
 read_data_done:

  bytes_to_copy = bytes_read;
  if (bytes_requested < bytes_read)
    bytes_to_copy = bytes_requested;

  read_handle->cursor += bytes_to_copy;
  cursor_in_stream = read_handle->cursor - read_handle->header_size;


  if (bytes_to_copy > 0 && readbuf != NULL)
    memcpy (buffer, readbuf, bytes_to_copy);

  g_vfs_job_read_set_size (job, bytes_to_copy);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_seek_on_read (GVfsBackend *backend,
		 GVfsJobSeekRead *job,
		 GVfsBackendHandle handle,
		 goffset    offset,
		 GSeekType  type)
{
  GVfsBackendCdda *cdda_backend = G_VFS_BACKEND_CDDA (backend);
  ReadHandle *read_handle = (ReadHandle *) handle;
  long new_offset;

  //g_warning ("seek_on_read (%d, %d)", (int)offset, type);

  switch (type)
    {
    default:
    case G_SEEK_SET:
      new_offset = offset;
      break;
    case G_SEEK_CUR:
      new_offset = read_handle->cursor + offset;
      break;
    case G_SEEK_END:
      new_offset = read_handle->size + offset;
      break;
    }

  if (new_offset < 0 || new_offset >= read_handle->size)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			G_IO_ERROR_FAILED,
			_("Error seeking in stream on drive %s"), cdda_backend->device_path);
    }
  else
    {
      read_handle->cursor = new_offset;
      
      g_vfs_job_seek_read_set_offset (job, offset);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  
}

static void
do_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  ReadHandle *read_handle = (ReadHandle *) handle;
  GVfsBackendCdda *cdda_backend = G_VFS_BACKEND_CDDA (backend);

  //g_warning ("close ()");

  free_read_handle (read_handle);

  cdda_backend->num_open_files--;
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
set_info_for_track (GVfsBackendCdda *cdda_backend, GFileInfo *info, int track_num)
{
  char *header;
  long first;
  long last;
  long header_size;
  long content_size;
  GIcon *icon;

  first = cdio_cddap_track_firstsector (cdda_backend->drive, track_num);
  last = cdio_cddap_track_lastsector (cdda_backend->drive, track_num);
  content_size = (last - first + 1) * CDIO_CD_FRAMESIZE_RAW;

  header = create_header (cdda_backend, &header_size, content_size);
  g_free (header);

  //g_warning ("size=%ld for track %d", size, track_num);

  g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);
  g_file_info_set_content_type (info, "audio/x-wav");
  g_file_info_set_size (info, header_size + content_size);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE); 

  icon = g_themed_icon_new ("audio-x-generic");
  g_file_info_set_icon (info, icon);
  g_object_unref (icon);

}

static void
do_query_info (GVfsBackend *backend,
	       GVfsJobQueryInfo *job,
	       const char *filename,
	       GFileQueryInfoFlags flags,
	       GFileInfo *info,
	       GFileAttributeMatcher *matcher)
{
  GVfsBackendCdda *cdda_backend = G_VFS_BACKEND_CDDA (backend);
  int track_num;
  GError *error;

  //g_warning ("get_file_info (%s)", filename);

  if (strcmp (filename, "/") == 0)
    {
      GIcon *icon;
      g_file_info_set_display_name (info, _("Audio Disc")); /* TODO: fill in from metadata */
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_content_type (info, "inode/directory");
      g_file_info_set_size (info, 0);
      icon = g_themed_icon_new ("folder");
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
    }
  else
    {
      g_file_info_set_name (info, filename);
      g_file_info_set_display_name (info, filename);
      
      track_num = get_track_num_from_name (cdda_backend, filename);
      if (track_num == -1)
        {
          error = g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("No such file"));
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          return;
        }

      if (track_num > cdda_backend->drive->tracks)
        {
          error = g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("The file does not exist"));
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          return;
        }

      if (! cdio_cddap_track_audiop (cdda_backend->drive, track_num))
        {
          error = g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("The file does not exist or isn't an audio track"));
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          return;
        }

      set_info_for_track (cdda_backend, info, track_num);
    }
  

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_enumerate (GVfsBackend *backend,
              GVfsJobEnumerate *job,
              const char *filename,
              GFileAttributeMatcher *matcher,
              GFileQueryInfoFlags flags)
{
  GVfsBackendCdda *cdda_backend = G_VFS_BACKEND_CDDA (backend);
  GFileInfo *info;
  GList *l;
  int n;

  //g_warning ("enumerate (%s)", filename);

  l = NULL;
  for (n = 1; n <= cdda_backend->drive->tracks; n++)
    {
      char *name;

      /* not audio track */
      if (! cdio_cddap_track_audiop (cdda_backend->drive, n))
        continue;
      
      info = g_file_info_new ();

      name = g_strdup_printf ("Track %d.wav", n);
      g_file_info_set_name (info, name);
      g_file_info_set_display_name (info, name);
      g_free (name);

      set_info_for_track (cdda_backend, info, n);
      
      l = g_list_append (l, info);
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_vfs_job_enumerate_add_infos (job, l);
  g_list_foreach (l, (GFunc) g_object_unref, NULL);
  g_list_free (l);

  g_vfs_job_enumerate_done (job);
}

static void
g_vfs_backend_cdda_class_init (GVfsBackendCddaClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_cdda_finalize;

  backend_class->try_mount = try_mount;
  backend_class->mount = do_mount;
  backend_class->unmount = do_unmount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->read = do_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->close_read = do_close_read;
  backend_class->query_info = do_query_info;
  backend_class->enumerate = do_enumerate;
}
