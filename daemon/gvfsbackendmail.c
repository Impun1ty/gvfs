/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
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
 * Author: Alexander Larsson <alexl@redhat.com>
 */


#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <glib/gurifuncs.h>

#include "gvfsbackendmail.h"
#include "gvfsmonitor.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobdelete.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsdaemonprotocol.h"


struct _GVfsBackendMail
{
  GVfsBackend parent_instance;
  char *maildir;
};

G_DEFINE_TYPE (GVfsBackendMail, g_vfs_backend_mail, G_VFS_TYPE_BACKEND)


static void
g_vfs_backend_mail_finalize (GObject *object)
{
  GVfsBackendMail *backend;

  backend = G_VFS_BACKEND_MAIL (object);

  if (G_OBJECT_CLASS (g_vfs_backend_mail_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_mail_parent_class)->finalize) (object);
}

static void
g_vfs_backend_mail_init (GVfsBackendMail *mail_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (mail_backend);

  mail_backend->maildir = "/home/christian/Mail/Initcrash/INBOX/";

  /* translators: This is the name of the backend */
  g_vfs_backend_set_display_name (backend, _("Mail"));
  g_vfs_backend_set_icon_name (backend, "user-mail");
  g_vfs_backend_set_user_visible (backend, FALSE);
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  mount_spec = g_mount_spec_new ("mail");
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  g_mount_spec_unref (mount_spec);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_open_for_read (GVfsBackend *backend,
                  GVfsJobOpenForRead *job,
                  const char *filename)
{
  g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                    G_IO_ERROR_IS_DIRECTORY,
                    _("Can't open directory"));
}

static void
do_read (GVfsBackend *backend,
         GVfsJobRead *job,
         GVfsBackendHandle _handle,
         char *buffer,
         gsize bytes_requested)
{
  GInputStream *stream;
  gssize res;
  GError *error;

  stream = G_INPUT_STREAM (_handle);

  error = NULL;
  res = g_input_stream_read (stream,
                             buffer, bytes_requested,
                             G_VFS_JOB (job)->cancellable,
                             &error);

  if (res != -1)
    {
      g_vfs_job_read_set_size (job, res);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}


static void
do_close_read (GVfsBackend *backend,
               GVfsJobCloseRead *job,
               GVfsBackendHandle _handle)
{
  GInputStream *stream;
  GError *error;

  stream = G_INPUT_STREAM (_handle);

  error = NULL;
  if (g_input_stream_close (stream,
                            G_VFS_JOB (job)->cancellable,
                            &error))
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}


static void
update_file_info (GFileInfo *info, GVfsJob *job, char *maildir)
{
  GFile *file;
  GFileInputStream *stream;
  GDataInputStream *datastream;
  char *path, *name;

  path = g_build_filename (maildir, "cur", g_file_info_get_name(info), NULL);
  file = g_file_new_for_path (path);
  g_free (path);

  if(stream = g_file_read (file,job->cancellable,NULL))
    {
      datastream = g_data_input_stream_new (G_INPUT_STREAM (stream));
      g_object_unref(stream);

      while(name = g_data_input_stream_read_line(datastream,0,0,0))
        {
            if(name == g_strrstr(name,"Subject: "))
              {
                if (g_utf8_validate(name+9, -1, NULL))
                  {
                    g_file_info_set_display_name(info,name+9);
                  }
                else
                  {
                    g_file_info_set_display_name(info,"invalid utf-8");
                  }

                g_free (name);
                break;
              }
            g_free (name);
        }

      g_object_unref(datastream);
    }
  else
    {
      g_file_info_set_display_name(info,"error");
    }

  g_file_info_set_attribute_string(info,"mail::sender","foo@bla.com");
  g_object_unref(file);
}

static void
enumerate_maildir (char *maildir,
                  GVfsJobEnumerate *job)

{
  GFile *file;
  GFileEnumerator *enumerator;
  char *path;

  path = g_build_filename (maildir, "cur", NULL);
  file = g_file_new_for_path (path);
  g_free (path);

  enumerator = g_file_enumerate_children(file,
                                         job->attributes,
                                         job->flags,
                                         G_VFS_JOB (job)->cancellable,
                                         NULL);
  g_object_unref(file);

  if (enumerator)
    {
      GFileInfo *info;
      while ((info = g_file_enumerator_next_file (enumerator,G_VFS_JOB (job)->cancellable,NULL)))
        {
            update_file_info(info, G_VFS_JOB(job), maildir);

            g_vfs_job_enumerate_add_info(job,info);
            g_object_unref (info);
        }
      g_object_unref(enumerator);
    }
}

static void
do_enumerate (GVfsBackend *backend,
              GVfsJobEnumerate *job,
              const char *filename,
              GFileAttributeMatcher *attribute_matcher,
              GFileQueryInfoFlags flags)
{
  GVfsBackendMail *mail_backend;
  mail_backend = G_VFS_BACKEND_MAIL (backend);

  enumerate_maildir(mail_backend->maildir,job);

  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_vfs_job_enumerate_done (job);
}

static void
do_query_info (GVfsBackend *backend,
               GVfsJobQueryInfo *job,
               const char *filename,
               GFileQueryInfoFlags flags,
               GFileInfo *info,
               GFileAttributeMatcher *matcher)
{
  GVfsBackendMail *mail_backend = G_VFS_BACKEND_MAIL (backend);

  /* The mail:/// root */
  if (!g_strcmp0(filename,"/"))
    {
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_name (info, "/");
      /* Translators: this is the display name of the backend */
      g_file_info_set_display_name (info, _("Mail"));
      g_file_info_set_content_type (info, "inode/directory");
    }
  else
    {
      g_file_info_set_name (info, filename+1);
      update_file_info(info, G_VFS_JOB(job), mail_backend->maildir);
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_delete (GVfsBackend *backend,
           GVfsJobDelete *job,
           const char *filename)
{
  g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                    G_IO_ERROR_PERMISSION_DENIED,
                    _("Can't delete mail"));
}

static void
g_vfs_backend_mail_class_init (GVfsBackendMailClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

  gobject_class->finalize = g_vfs_backend_mail_finalize;

  backend_class->mount = do_mount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->read = do_read;
  backend_class->close_read = do_close_read;
  backend_class->query_info = do_query_info;
  backend_class->enumerate = do_enumerate;
  backend_class->delete = do_delete;
}
