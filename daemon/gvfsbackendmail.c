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

#include <gmime/gmime.h>
#include <gconf/gconf-client.h>

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

const gchar *charsets[] = { "UTF-8" , NULL };


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
  g_mime_shutdown();
  g_free(backend->maildir);
}

static void
g_vfs_backend_mail_init (GVfsBackendMail *mail_backend)
{
  GConfClient *gclient;
  GError *error = NULL;

  if ((gclient = gconf_client_get_default()))
      mail_backend->maildir = gconf_client_get_string(gclient,"/desktop/gnome/mail/maildir",&error);

  if(!mail_backend->maildir)
  {
    mail_backend->maildir = g_strdup("/home/christian/Mail/Initcrash/INBOX/");
    gconf_client_set_string(gclient,"/desktop/gnome/mail/maildir",mail_backend->maildir,&error);
  }
  g_object_unref(gclient);

  g_print("maildir: %s\n",mail_backend->maildir);

  g_mime_init(NULL);
  g_mime_set_user_charsets(charsets);
  GVfsBackend *backend = G_VFS_BACKEND (mail_backend);


  /* translators: This is the name of the backend */
  g_vfs_backend_set_display_name (backend, _("Mail"));
  g_vfs_backend_set_icon_name (backend, "stock_mail");
  g_vfs_backend_set_user_visible (backend, TRUE);

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
  char *path;
  GFile *file;
  GFileInputStream *stream;
  GError *error;
  GVfsBackendMail *mail_backend = G_VFS_BACKEND_MAIL (backend);

  if (!g_strcmp0(filename,"/"))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_IS_DIRECTORY,
                        _("Can't open directory"));
    }
  else
    {
      path = g_build_filename (mail_backend->maildir, "cur", filename, NULL);
      file = g_file_new_for_path (path);
      g_free (path);
      stream = g_file_read (file,G_VFS_JOB(job)->cancellable,&error);
      g_object_unref (file);

      if (stream)
        {
          g_vfs_job_open_for_read_set_handle (job, stream);
          g_vfs_job_open_for_read_set_can_seek  (job, g_seekable_can_seek (G_SEEKABLE (stream)));
          g_vfs_job_succeeded (G_VFS_JOB (job));
        }
      else
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
        }
    }
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
  char *path, *subject, *from;
  char *filename;
  char **filename_parts;
  FILE *fp;
  GMimeStream *mime_stream;
  GMimeParser *parser;
  GMimeMessage *message;
  GIcon *icon;

  filename = g_file_info_get_name (info);

  path = g_build_filename (maildir, "cur", filename, NULL);
  fp = fopen (path,"r+");
  g_free (path);
  if(!fp) return;

  mime_stream = g_mime_stream_file_new (fp);
  parser = g_mime_parser_new_with_stream(mime_stream);
  g_object_unref(mime_stream);
  message = g_mime_parser_construct_message(parser);
  g_object_unref(parser);

  subject = g_mime_utils_header_decode_phrase(g_mime_message_get_subject(message));
  from = g_mime_utils_header_decode_phrase(g_mime_message_get_sender(message));
  //from = g_mime_utils_header_decode_phrase(g_mime_message_get_header(message,"X-Label"));

  if (subject && g_strcmp0("",subject) && g_utf8_validate(subject, -1, NULL))
    {
      g_file_info_set_attribute_string(info,"standard::display-name",subject);
      g_file_info_set_attribute_string(info,"mail::subject",subject);
    }
  else
    {
      g_file_info_set_attribute_string(info,"standard::display-name",_("no subject"));
      g_file_info_set_attribute_string(info,"mail::subject",from);
    }
  g_free(subject);

  if (from && g_utf8_validate(from, -1, NULL))
    g_file_info_set_attribute_string(info,"mail::from",from);
  g_free(from);

  g_object_unref(message);

  g_file_info_set_content_type (info, "message/rfc822");

  filename_parts = g_strsplit(filename,":2,",2);
  if(g_strrstr(filename_parts[1],"R"))
    icon = g_themed_icon_new ("mail-replied");
  else if(g_strrstr(filename_parts[1],"S"))
    icon = g_themed_icon_new ("mail-read");
  else
    icon = g_themed_icon_new ("mail-unread");
  g_strfreev(filename_parts);
  g_file_info_set_icon (info, icon);
  g_object_unref (icon);
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
  if ((!g_strcmp0(filename,"/")) || (!g_strcmp0(filename,"")))
    {
      GIcon *icon;
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_name (info, "/");
      /* Translators: this is the display name of the backend */
      g_file_info_set_display_name (info, _("Mail"));
      g_file_info_set_content_type (info, "inode/directory");

      icon = g_themed_icon_new ("stock_mail");
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
    }
  else
    {
      GFile *file;
      GFileInfo *local_info;
      char *path;
      GError *error;
      char *basename;

      basename = g_path_get_basename (filename);

      path = g_build_filename (mail_backend->maildir, "cur", basename, NULL);
      file = g_file_new_for_path (path);
      g_free (path);

      error = NULL;
      local_info = g_file_query_info (file,
                                      job->attributes,
                                      job->flags,
                                      G_VFS_JOB (job)->cancellable,
                                      &error);
      g_object_unref (file);

      if (local_info)
        {
          g_file_info_copy_into (local_info, info);
          g_file_info_set_name (info, basename);
          update_file_info(info,job,mail_backend->maildir);
          g_object_unref (local_info);
        }
      g_free (basename);
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
