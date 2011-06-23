 /* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) Carl-Anton Ingmarsson 2011 <ca.ingmarsson@gmail.com>
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
 * Author: Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 */

#include <config.h>

#include <stdlib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#ifdef HAVE_GCRYPT
#include <gcrypt.h>
#endif

#include "gvfsjobmount.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobcloseread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"

#include "gvfsafpserver.h"
#include "gvfsafpconnection.h"

#include "gvfsbackendafp.h"

static const gint16 ENUMERATE_REQ_COUNT      = G_MAXINT16;
static const gint32 ENUMERATE_MAX_REPLY_SIZE = G_MAXINT32;

struct _GVfsBackendAfpClass
{
	GVfsBackendClass parent_class;
};

struct _GVfsBackendAfp
{
	GVfsBackend parent_instance;

	GNetworkAddress    *addr;
  char               *volume;
	char               *user;

  GVfsAfpServer      *server;

  gint32              time_diff;
  guint16             volume_id;
};


G_DEFINE_TYPE (GVfsBackendAfp, g_vfs_backend_afp, G_VFS_TYPE_BACKEND);

static gboolean
is_root (const char *filename)
{
  const char *p;

  p = filename;
  while (*p == '/')
    p++;

  return *p == 0;
}


static GVfsAfpName *
filename_to_afp_pathname (const char *filename)
{
  gsize len;
  char *str;
  gint i;

  while (*filename == '/')
    filename++;
  
  len = strlen (filename);

  str = g_malloc (len);

  for (i = 0; i < len; i++)
  {
    if (filename[i] == '/')
      str[i] = '\0';
    else
      str[i] = filename[i];
  }
  

  return g_vfs_afp_name_new (0x08000103, str, len);
}

typedef struct
{
  gint16 fork_refnum;
  gint64 offset;
} AfpHandle;

static AfpHandle *
afp_handle_new (gint16 fork_refnum)
{
  AfpHandle *afp_handle;

  afp_handle = g_slice_new0 (AfpHandle);
  afp_handle->fork_refnum = fork_refnum;

  return afp_handle;
}

static void
afp_handle_free (AfpHandle *afp_handle)
{
  g_slice_free (AfpHandle, afp_handle);
}

static void fill_info (GVfsBackendAfp *afp_backend,
                       GFileInfo *info, GVfsAfpReply *reply,
                       gboolean directory, guint16 bitmap)
{
  goffset start_pos;

  if (directory)
  {
    GIcon *icon;
    
    g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_content_type (info, "inode/directory");

    icon = g_themed_icon_new ("folder");
    g_file_info_set_icon (info, icon);
    g_object_unref (icon);
  }
  else
    g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);

  
  start_pos = g_vfs_afp_reply_get_pos (reply);

  if (bitmap & AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT)
  {
    guint16 attributes;

    g_vfs_afp_reply_read_uint16 (reply, &attributes);
    
    if (attributes & AFP_FILEDIR_ATTRIBUTES_BITMAP_INVISIBLE_BIT)
      g_file_info_set_is_hidden (info, TRUE);
  }

  if (bitmap & AFP_FILEDIR_BITMAP_CREATE_DATE_BIT)
  {
    gint32 create_date;

    g_vfs_afp_reply_read_int32 (reply, &create_date);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CREATED,
                                      create_date + afp_backend->time_diff);
  }

  if (bitmap & AFP_FILEDIR_BITMAP_MOD_DATE_BIT)
  {
    gint32 mod_date;

    g_vfs_afp_reply_read_int32 (reply, &mod_date);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                      mod_date + afp_backend->time_diff);
  }

  /* Directory specific attributes */
  if (directory)
  {
    if (bitmap & AFP_DIR_BITMAP_OFFSPRING_COUNT_BIT)
    {
      guint32 offspring_count;

      g_vfs_afp_reply_read_uint32 (reply, &offspring_count);
      g_file_info_set_attribute_uint32 (info, "afp::children-count", offspring_count);
    }
  }
  
  /* File specific attributes */
  else
  {
    if (bitmap & AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT)
    {
      guint64 fork_len;

      g_vfs_afp_reply_read_uint64 (reply, &fork_len);
      g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                        fork_len);
    }
  }
  
  if (bitmap & AFP_FILEDIR_BITMAP_UTF8_NAME_BIT)
  {
    guint16 UTF8Name_offset;
    goffset old_pos;
    GVfsAfpName *afp_name;
    char *utf8_name;

    g_vfs_afp_reply_read_uint16 (reply, &UTF8Name_offset);

    old_pos = g_vfs_afp_reply_get_pos (reply);
    g_vfs_afp_reply_seek (reply, start_pos + UTF8Name_offset, G_SEEK_SET);

    g_vfs_afp_reply_read_afp_name (reply, TRUE, &afp_name);
    utf8_name = g_vfs_afp_name_get_string (afp_name);    
    g_vfs_afp_name_unref (afp_name);

    g_file_info_set_name (info, utf8_name);
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                                      utf8_name);

    /* Set file as hidden if it begins with a dot */
    if (utf8_name[0] == '.')
      g_file_info_set_is_hidden (info, TRUE);

    if (!directory)
    {
      char *content_type;
      GIcon *icon;

      content_type = g_content_type_guess (utf8_name, NULL, 0, NULL);
      g_file_info_set_content_type (info, content_type);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE,
                                        content_type);

      icon = g_content_type_get_icon (content_type);
      g_file_info_set_icon (info, icon);

      g_object_unref (icon);
      g_free (content_type);
    }
    
    g_free (utf8_name);

    g_vfs_afp_reply_seek (reply, old_pos, G_SEEK_SET);
  }
}

static void
seek_on_read_cb (GVfsAfpConnection *afp_connection,
                 GVfsAfpReply      *reply,
                 GError            *error,
                 gpointer           user_data)
{
  GVfsJobSeekRead *job = G_VFS_JOB_SEEK_READ (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  AfpResultCode res_code;
  guint16 file_bitmap;
  GFileInfo *info;
  gsize size;
  
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);
    
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_FAILED, _("Got error from server"));
    return;
  }

  g_vfs_afp_reply_read_uint16 (reply, &file_bitmap);

  info = g_file_info_new ();
  fill_info (afp_backend, info, reply, FALSE, file_bitmap);

  size = g_file_info_get_size (info);
  g_object_unref (info);

  switch (job->seek_type)
  {
    case G_SEEK_CUR:
      afp_handle->offset += job->requested_offset;
      break;
    case G_SEEK_SET:
      afp_handle->offset = job->requested_offset;
      break;
    case G_SEEK_END:
      afp_handle->offset = size + job->requested_offset;
      break;
  }

  if (afp_handle->offset < 0)
    afp_handle->offset = 0;
  else if (afp_handle->offset > size)
    afp_handle->offset = size;

  g_vfs_job_seek_read_set_offset (job, afp_handle->offset);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

  
static gboolean
try_seek_on_read (GVfsBackend *backend,
                  GVfsJobSeekRead *job,
                  GVfsBackendHandle handle,
                  goffset    offset,
                  GSeekType  type)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;

  GVfsAfpCommand *comm;
  guint16 file_bitmap;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_FORK_PARMS);
  /* pad byte */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);

  /* OForkRefNum */
  g_data_output_stream_put_int16 (G_DATA_OUTPUT_STREAM (comm), afp_handle->fork_refnum,
                                  NULL, NULL);

  /* Bitmap */
  file_bitmap = AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT;
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm), file_bitmap,
                                   NULL, NULL);
  
  g_vfs_afp_connection_queue_command (afp_backend->server->conn, comm,
                                      seek_on_read_cb, G_VFS_JOB (job)->cancellable,
                                      job);
  g_object_unref (comm);

  return TRUE;
}

static void
read_ext_cb (GVfsAfpConnection *afp_connection,
             GVfsAfpReply      *reply,
             GError            *error,
             gpointer           user_data)
{
  GVfsJobRead *job = G_VFS_JOB_READ (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  AfpResultCode res_code;
  gsize size;
  char *data;

  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (!(res_code == AFP_RESULT_NO_ERROR || res_code == AFP_RESULT_EOF_ERR
        || res_code == AFP_RESULT_LOCK_ERR))
  {
    g_object_unref (reply);

    if (res_code == AFP_RESULT_ACCESS_DENIED)
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                                _("File is not open for read access"));
    }
    else
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                                _("Got error from server"));
    }
    return;
  }

  size = g_vfs_afp_reply_get_size (reply);

  /* TODO: Read directly into the data buffer */
  g_vfs_afp_reply_get_data (reply, size, (guint8 **)&data);
  memcpy (job->buffer, data, size);

  afp_handle->offset += size;
  g_vfs_job_read_set_size (job, size);

  g_object_unref (reply);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean 
try_read (GVfsBackend *backend,
          GVfsJobRead *job,
          GVfsBackendHandle handle,
          char *buffer,
          gsize bytes_requested)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;
  
  GVfsAfpCommand *comm;
  guint32 req_count;

  comm = g_vfs_afp_command_new (AFP_COMMAND_READ_EXT);
  /* pad byte */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);

  /* OForkRefNum */
  g_data_output_stream_put_int16 (G_DATA_OUTPUT_STREAM (comm),
                                  afp_handle->fork_refnum, NULL, NULL);
  /* Offset */
  g_data_output_stream_put_int64 (G_DATA_OUTPUT_STREAM (comm),
                                  afp_handle->offset, NULL, NULL);
  /* ReqCount */
  req_count = MIN (bytes_requested, G_MAXUINT32);
  g_data_output_stream_put_int64 (G_DATA_OUTPUT_STREAM (comm),
                                  req_count, NULL, NULL);

  g_vfs_afp_connection_queue_command (afp_backend->server->conn, comm,
                                      read_ext_cb, G_VFS_JOB (job)->cancellable,
                                      job);
  g_object_unref (comm);

  return TRUE;
}

static void
close_fork_cb (GVfsAfpConnection *afp_connection,
               GVfsAfpReply      *reply,
               GError            *error,
               gpointer           user_data)
{
  GVfsJobCloseRead *job = G_VFS_JOB_CLOSE_READ (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  AfpResultCode res_code;
  
  if (!reply)
  {
    afp_handle_free (afp_handle);
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  g_object_unref (reply);

  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Got error from server"));
  }
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));

  afp_handle_free (afp_handle);
}

static gboolean
try_close_read (GVfsBackend *backend,
                GVfsJobCloseRead *job,
                GVfsBackendHandle handle)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;

  GVfsAfpCommand *comm;

  comm = g_vfs_afp_command_new (AFP_COMMAND_CLOSE_FORK);
  /* pad byte */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);

  g_data_output_stream_put_int16 (G_DATA_OUTPUT_STREAM (comm), afp_handle->fork_refnum,
                                  NULL, NULL);

  g_vfs_afp_connection_queue_command (afp_backend->server->conn, comm,
                                      close_fork_cb, G_VFS_JOB (job)->cancellable,
                                      job);
  g_object_unref (comm);

  return TRUE;
}

static void
open_for_read_cb (GVfsAfpConnection *afp_connection,
                  GVfsAfpReply      *reply,
                  GError            *error,
                  gpointer           user_data)
{
  GVfsJobOpenForRead *job = G_VFS_JOB_OPEN_FOR_READ (user_data);

  AfpResultCode res_code;
  guint16 file_bitmap;
  gint16 fork_refnum;
  AfpHandle *afp_handle;
  
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);
    
    if (res_code == AFP_RESULT_ACCESS_DENIED)
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                _("Access denied"));
    }
    else if (res_code == AFP_RESULT_OBJECT_NOT_FOUND)
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                _("File doesn't exist"));
    }
    else if (res_code == AFP_RESULT_OBJECT_TYPE_ERR)
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                                _("File is a directory"));
    }
    else if (res_code == AFP_RESULT_TOO_MANY_FILES_OPEN)
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES,
                                _("Too many files open"));
    }
    else
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                                _("Got error from server"));
    }
    return;
  }

  g_vfs_afp_reply_read_uint16 (reply, &file_bitmap);
  g_vfs_afp_reply_read_int16  (reply, &fork_refnum);

  g_object_unref (reply);
  
  afp_handle = afp_handle_new (fork_refnum);
  g_vfs_job_open_for_read_set_handle (job, afp_handle);

  g_vfs_job_open_for_read_set_can_seek (job, TRUE);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_open_for_read (GVfsBackend *backend,
                   GVfsJobOpenForRead *job,
                   const char *filename)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  GVfsAfpCommand *comm;
  guint16 access_mode;
  GVfsAfpName *pathname;
  
  if (is_root (filename))
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_NOT_REGULAR_FILE,
                              _("File is a directory"));
    return TRUE;
  }

  comm = g_vfs_afp_command_new (AFP_COMMAND_OPEN_FORK);
  /* data fork */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);

  /* Volume ID */
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm),
                                   afp_backend->volume_id, NULL, NULL);
  /* Directory ID */
  g_data_output_stream_put_int32 (G_DATA_OUTPUT_STREAM (comm), 2, NULL, NULL);

  /* Bitmap */
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);
  
  /* AccessMode */
  access_mode = AFP_ACCESS_MODE_READ_BIT;
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm), access_mode,
                                   NULL, NULL);

  /* PathType */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), AFP_PATH_TYPE_UTF8_NAME,
                                 NULL, NULL);

  pathname = filename_to_afp_pathname (filename);
  g_vfs_afp_command_put_afp_name (comm, pathname);
  g_vfs_afp_name_unref (pathname);

  g_vfs_afp_connection_queue_command (afp_backend->server->conn, comm,
                                      open_for_read_cb,
                                      G_VFS_JOB (job)->cancellable, job);
  g_object_unref (comm);

  return TRUE;
}


static guint16
create_filedir_bitmap (GFileAttributeMatcher *matcher)
{
  guint16 bitmap;

  bitmap = AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT | AFP_FILEDIR_BITMAP_UTF8_NAME_BIT;

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_TIME_CREATED))
    bitmap |= AFP_FILEDIR_BITMAP_CREATE_DATE_BIT;
  
  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_TIME_MODIFIED))
    bitmap |= AFP_FILEDIR_BITMAP_MOD_DATE_BIT;
  
  return bitmap;
}
static guint16
create_file_bitmap (GFileAttributeMatcher *matcher)
{
  guint16 file_bitmap;
  
  file_bitmap = create_filedir_bitmap (matcher);

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_STANDARD_SIZE))
    file_bitmap |= AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT;
  
  return file_bitmap;
}

static guint16
create_dir_bitmap (GFileAttributeMatcher *matcher)
{
  guint16 dir_bitmap;
  
  dir_bitmap = create_filedir_bitmap (matcher);

  if (g_file_attribute_matcher_matches (matcher, "afp::children-count"))
    dir_bitmap |= AFP_DIR_BITMAP_OFFSPRING_COUNT_BIT;
  
  return dir_bitmap;
}

static void
enumerate_ext2 (GVfsJobEnumerate *job,
                gint32 start_index);

static void
enumerate_ext2_cb (GVfsAfpConnection *afp_connection,
                   GVfsAfpReply      *reply,
                   GError            *error,
                   gpointer           user_data)
{
  GVfsJobEnumerate *job = G_VFS_JOB_ENUMERATE (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);

  AfpResultCode res_code;
  guint16 file_bitmap;
  guint16  dir_bitmap;
  gint16 count, i;

  gint start_index;
  
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code == AFP_RESULT_OBJECT_NOT_FOUND)
  {
    g_vfs_job_succeeded (G_VFS_JOB (job));
    g_vfs_job_enumerate_done (job);
    return;
  }
  else if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_FAILED, _("Enumeration of files failed"));
    return;
  }

  g_vfs_afp_reply_read_uint16 (reply, &file_bitmap);
  g_vfs_afp_reply_read_uint16 (reply, &dir_bitmap);

  g_vfs_afp_reply_read_int16 (reply, &count);
  for (i = 0; i < count; i++)
  {
    goffset start_pos;
    guint16 struct_length;
    guint8 FileDir;
    
    gboolean directory;
    guint16 bitmap;
    GFileInfo *info;

    start_pos = g_vfs_afp_reply_get_pos (reply);
    
    g_vfs_afp_reply_read_uint16 (reply, &struct_length);
    g_vfs_afp_reply_read_byte (reply, &FileDir);
    /* pad byte */
    g_vfs_afp_reply_read_byte (reply, NULL);

    directory = (FileDir & 0x80); 
    bitmap =  directory ? dir_bitmap : file_bitmap;
    
    info = g_file_info_new ();
    fill_info (afp_backend, info, reply, directory, bitmap);
    g_vfs_job_enumerate_add_info (job, info);
    g_object_unref (info);

    g_vfs_afp_reply_seek (reply, start_pos + struct_length, G_SEEK_SET);
  }

  start_index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (job),
                                                    "start-index"));
  start_index += count;
  g_object_set_data (G_OBJECT (job), "start-index",
                     GINT_TO_POINTER (start_index));

  enumerate_ext2 (job, start_index);
}

static void
enumerate_ext2 (GVfsJobEnumerate *job,
                gint32 start_index)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);
  GVfsAfpConnection *conn = afp_backend->server->conn;
  const char *filename = job->filename;
  GFileAttributeMatcher *matcher = job->attribute_matcher;
  
  GVfsAfpCommand *comm;
  guint16 file_bitmap, dir_bitmap;
  GVfsAfpName *pathname;

  comm = g_vfs_afp_command_new (AFP_COMMAND_ENUMERATE_EXT2);
  /* pad byte */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);

  /* Volume ID */
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm),
                                   afp_backend->volume_id, NULL, NULL);

  /* Directory ID 2 == / */
  g_data_output_stream_put_uint32 (G_DATA_OUTPUT_STREAM (comm), 2, NULL, NULL);

  /* File Bitmap */
  file_bitmap = create_file_bitmap (matcher);
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm),  file_bitmap,
                                   NULL, NULL);
  /* Dir Bitmap */
  dir_bitmap = create_dir_bitmap (matcher);
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm),  dir_bitmap,
                                   NULL, NULL);

  /* Req Count */
  g_data_output_stream_put_int16 (G_DATA_OUTPUT_STREAM (comm),  ENUMERATE_REQ_COUNT,
                                  NULL, NULL);
  /* StartIndex */
  g_data_output_stream_put_int32 (G_DATA_OUTPUT_STREAM (comm),  start_index,
                                  NULL, NULL);
  /* MaxReplySize */
  g_data_output_stream_put_int32 (G_DATA_OUTPUT_STREAM (comm),
                                  ENUMERATE_MAX_REPLY_SIZE, NULL, NULL);


  /* PathType */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), AFP_PATH_TYPE_UTF8_NAME,
                                 NULL, NULL);

  pathname = filename_to_afp_pathname (filename);
  g_vfs_afp_command_put_afp_name (comm, pathname);
  g_vfs_afp_name_unref (pathname);

  g_vfs_afp_connection_queue_command (conn, comm, enumerate_ext2_cb,
                                      G_VFS_JOB (job)->cancellable, job);
  g_object_unref (comm);
}

static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *matcher,
               GFileQueryInfoFlags flags)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  if (afp_backend->server->version >= AFP_VERSION_3_1)
  {
    g_object_set_data (G_OBJECT (job), "start-index",
                       GINT_TO_POINTER (1));
    enumerate_ext2 (job, 1);
  }

  else
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Enumeration not supported for AFP_VERSION_3_0 yet");
  }

  return TRUE;
}

static void
get_filedir_parms_cb (GVfsAfpConnection *afp_connection,
                      GVfsAfpReply      *reply,
                      GError            *error,
                      gpointer           user_data)
{
  GVfsJobQueryInfo *job = G_VFS_JOB_QUERY_INFO (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);

  AfpResultCode res_code;
  guint16 file_bitmap, dir_bitmap, bitmap;
  guint8 FileDir;
  gboolean directory;
  
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code == AFP_RESULT_OBJECT_NOT_FOUND)
  {
    g_object_unref (reply);
    g_vfs_job_failed_literal (G_VFS_JOB (job),  G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("File doesn't exist"));
    return;
  }

  else if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_FAILED, _("Retrieval of file/directory parameters failed"));
    return;
  }

  g_vfs_afp_reply_read_uint16 (reply, &file_bitmap);
  g_vfs_afp_reply_read_uint16 (reply, &dir_bitmap);

  g_vfs_afp_reply_read_byte (reply, &FileDir);
  /* Pad Byte */
  g_vfs_afp_reply_read_byte (reply, NULL);
  
  directory = (FileDir & 0x80); 
  bitmap =  directory ? dir_bitmap : file_bitmap;

  fill_info (afp_backend, job->file_info, reply, directory, bitmap);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
get_vol_parms_cb (GVfsAfpConnection *afp_connection,
                  GVfsAfpReply      *reply,
                  GError            *error,
                  gpointer           user_data)
{
  GVfsJobQueryInfo *job = G_VFS_JOB_QUERY_INFO (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);

  AfpResultCode res_code;
  GFileInfo *info;
  guint16 vol_bitmap;
  
  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_FAILED, _("Retrieval of volume parameters failed"));
    return;
  }

  info = job->file_info;

  g_vfs_afp_reply_read_uint16 (reply, &vol_bitmap);
  
  if (vol_bitmap & AFP_VOLUME_BITMAP_CREATE_DATE_BIT)
  {
    gint32 create_date;

    g_vfs_afp_reply_read_int32 (reply, &create_date);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CREATED,
                                      create_date + afp_backend->time_diff);
  }

  if (vol_bitmap & AFP_VOLUME_BITMAP_MOD_DATE_BIT)
  {
    gint32 mod_date;

    g_vfs_afp_reply_read_int32 (reply, &mod_date);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CREATED,
                                      mod_date + afp_backend->time_diff);
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  if (is_root (filename))
  {
    GIcon *icon;
    guint16 vol_bitmap = 0;

    g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_name (info, "/");
    g_file_info_set_display_name (info, g_vfs_backend_get_display_name (backend));
    g_file_info_set_content_type (info, "inode/directory");
    icon = g_vfs_backend_get_icon (backend);
    if (icon != NULL)
      g_file_info_set_icon (info, icon);

    if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_TIME_CREATED))
        vol_bitmap |= AFP_VOLUME_BITMAP_CREATE_DATE_BIT;

    if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_TIME_MODIFIED))
      vol_bitmap |= AFP_VOLUME_BITMAP_MOD_DATE_BIT;


    if (vol_bitmap != 0)
    {
      GVfsAfpCommand *comm;
      
      comm = g_vfs_afp_command_new (AFP_COMMAND_GET_VOL_PARMS);
      /* pad byte */
      g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);
      /* Volume ID */
      g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm),
                                       afp_backend->volume_id, NULL, NULL);

      g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm), vol_bitmap, NULL, NULL);

      g_vfs_afp_connection_queue_command (afp_backend->server->conn, comm,
                                          get_vol_parms_cb, G_VFS_JOB (job)->cancellable,
                                          job);
      g_object_unref (comm);
      return TRUE;
    }

    g_vfs_job_succeeded (G_VFS_JOB (job));
  }
  
  else {
    GVfsAfpCommand *comm;

    guint16 file_bitmap, dir_bitmap;
    GVfsAfpName *pathname;
    
    comm = g_vfs_afp_command_new (AFP_COMMAND_GET_FILE_DIR_PARMS);
    /* pad byte */
    g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);
    /* Volume ID */
    g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm), afp_backend->volume_id, NULL, NULL);
    /* Directory ID */
    g_data_output_stream_put_uint32 (G_DATA_OUTPUT_STREAM (comm), 2, NULL, NULL);

    file_bitmap = create_file_bitmap (matcher);
    g_data_output_stream_put_int16 (G_DATA_OUTPUT_STREAM (comm), file_bitmap,
                                    NULL, NULL);

    dir_bitmap = create_dir_bitmap (matcher);
    g_data_output_stream_put_int16 (G_DATA_OUTPUT_STREAM (comm), dir_bitmap,
                                    NULL, NULL);

    /* PathType */
    g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), AFP_PATH_TYPE_UTF8_NAME,
                                   NULL, NULL);

    pathname = filename_to_afp_pathname (filename);
    g_vfs_afp_command_put_afp_name (comm, pathname);
    g_vfs_afp_name_unref (pathname);

    g_vfs_afp_connection_queue_command (afp_backend->server->conn, comm,
                                        get_filedir_parms_cb,
                                        G_VFS_JOB (job)->cancellable, job);
  }

  return TRUE;
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  gboolean res;
  GError *err = NULL;

  GVfsAfpCommand *comm;
  guint16 vol_bitmap;
  
  GVfsAfpReply *reply;
  AfpResultCode res_code;
  gint32 server_time;
  
  GMountSpec *afp_mount_spec;
  char       *server_name;
  char       *display_name;

  afp_backend->server = g_vfs_afp_server_new (afp_backend->addr);

  res = g_vfs_afp_server_login (afp_backend->server, afp_backend->user, mount_source,
                                G_VFS_JOB (job)->cancellable, &err);
  if (!res)
    goto error;

  /* Get Server Parameters */
  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_SRVR_PARMS);
  /* pad byte */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);

  res = g_vfs_afp_connection_send_command_sync (afp_backend->server->conn,
                                                comm, G_VFS_JOB (job)->cancellable,
                                                &err);
  g_object_unref (comm);
  if (!res)
    goto error;

  reply = g_vfs_afp_connection_read_reply_sync (afp_backend->server->conn,
                                                G_VFS_JOB (job)->cancellable, &err);
  if (!reply)
    goto error;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);
    goto error;
  }

  /* server time */
  g_vfs_afp_reply_read_int32 (reply, &server_time);
  afp_backend->time_diff = (g_get_real_time () / G_USEC_PER_SEC) - server_time;

  g_object_unref (reply);


  /* Open Volume */
  comm = g_vfs_afp_command_new (AFP_COMMAND_OPEN_VOL);
  /* pad byte */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);
  
  /* Volume Bitmap */
  vol_bitmap = AFP_VOLUME_BITMAP_VOL_ID_BIT;
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm), vol_bitmap,
                                   NULL, NULL);

  /* VolumeName */
  g_vfs_afp_command_put_pascal (comm, afp_backend->volume);

  /* TODO: password? */

  res = g_vfs_afp_connection_send_command_sync (afp_backend->server->conn,
                                                comm, G_VFS_JOB (job)->cancellable,
                                                &err);
  g_object_unref (comm);
  if (!res)
    goto error;

  reply = g_vfs_afp_connection_read_reply_sync (afp_backend->server->conn,
                                                G_VFS_JOB (job)->cancellable, &err);
  if (!reply)
    goto error;

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);
    goto generic_error;
  }
  
  /* Volume Bitmap */
  g_vfs_afp_reply_read_uint16 (reply, NULL);

  /* Volume ID */
  g_vfs_afp_reply_read_uint16 (reply, &afp_backend->volume_id);
  
  g_object_unref (reply);
  
  /* set mount info */
  afp_mount_spec = g_mount_spec_new ("afp-volume");
  g_mount_spec_set (afp_mount_spec, "host",
                    g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)));
  g_mount_spec_set (afp_mount_spec, "volume", afp_backend->volume);
  if (afp_backend->user)
    g_mount_spec_set (afp_mount_spec, "user", afp_backend->user);

  g_vfs_backend_set_mount_spec (backend, afp_mount_spec);
  g_mount_spec_unref (afp_mount_spec);

  if (afp_backend->server->utf8_server_name)
    server_name = afp_backend->server->utf8_server_name;
  else
    server_name = afp_backend->server->server_name;
  
  if (afp_backend->user)
    display_name = g_strdup_printf (_("AFP volume %s for %s on %s"), 
                                    afp_backend->volume, afp_backend->user,
                                    server_name);
  else
    display_name = g_strdup_printf (_("AFP volume %s on %s"),
                                    afp_backend->volume, server_name);
  
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);

  g_vfs_backend_set_icon_name (backend, "folder-remote-afp");
  g_vfs_backend_set_user_visible (backend, TRUE);

  g_vfs_job_succeeded (G_VFS_JOB (job));
  return;

error:
  g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
  return;

generic_error:
  g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED,
                    _("Couldn't mount AFP volume %s on %s"), afp_backend->volume,
                    afp_backend->server->server_name);
  return;
}
  
static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
	GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
	
	const char *host, *volume, *portstr, *user;
	guint16 port = 548;
	
	host = g_mount_spec_get (mount_spec, "host");
	if (host == NULL)
		{
			g_vfs_job_failed (G_VFS_JOB (job),
			                  G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			                  _("No hostname specified"));
			return TRUE;
    }

  volume = g_mount_spec_get (mount_spec, "volume");
  if (volume == NULL)
  {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      _("No volume specified"));
    return TRUE;
  }
  afp_backend->volume = g_strdup (volume);
  
	portstr = g_mount_spec_get (mount_spec, "port");
	if (portstr != NULL)
		{
			port = atoi (portstr);
		}

	afp_backend->addr = G_NETWORK_ADDRESS (g_network_address_new (host, port));
	
	user = g_mount_spec_get (mount_spec, "user");
	afp_backend->user = g_strdup (user);
	
	return FALSE;
}

static void
g_vfs_backend_afp_init (GVfsBackendAfp *object)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (object);
  
  afp_backend->volume = NULL;
  afp_backend->user = NULL;

  afp_backend->addr = NULL;
}

static void
g_vfs_backend_afp_finalize (GObject *object)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (object);

  g_free (afp_backend->volume);
  g_free (afp_backend->user);

  if (afp_backend->addr)
    g_object_unref (afp_backend->addr);
  
	G_OBJECT_CLASS (g_vfs_backend_afp_parent_class)->finalize (object);
}

static void
g_vfs_backend_afp_class_init (GVfsBackendAfpClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

	object_class->finalize = g_vfs_backend_afp_finalize;

	backend_class->try_mount = try_mount;
  backend_class->mount = do_mount;
  backend_class->try_query_info = try_query_info;
  backend_class->try_enumerate = try_enumerate;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_close_read = try_close_read;
  backend_class->try_read = try_read;
  backend_class->try_seek_on_read = try_seek_on_read;
}

void
g_vfs_afp_daemon_init (void)
{
  g_set_application_name (_("Apple Filing Protocol Service"));

#ifdef HAVE_GCRYPT
  gcry_check_version (NULL);
  gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
#endif
}