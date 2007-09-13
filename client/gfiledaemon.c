#include <config.h>

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "gfiledaemon.h"
#include "gvfsdaemondbus.h"
#include <gvfsdaemonprotocol.h>
#include <gfileinputstreamdaemon.h>
#include <gfileenumeratordaemon.h>
#include <glib/gi18n-lib.h>

static void g_file_daemon_file_iface_init (GFileIface       *iface);

struct _GFileDaemon
{
  GObject parent_instance;

  GQuark match_bus_name;
  char *path;
};

G_DEFINE_TYPE_WITH_CODE (GFileDaemon, g_file_daemon, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						g_file_daemon_file_iface_init))

static void
g_file_daemon_finalize (GObject *object)
{
  GFileDaemon *daemon_file;

  daemon_file = G_FILE_DAEMON (object);

  g_free (daemon_file->path);
  
  if (G_OBJECT_CLASS (g_file_daemon_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_daemon_parent_class)->finalize) (object);
}

static void
g_file_daemon_class_init (GFileDaemonClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_file_daemon_finalize;
}

static void
g_file_daemon_init (GFileDaemon *daemon_file)
{
}

GFile *
g_file_daemon_new (GQuark match_bus_name,
		   const char *path)
{
  GFileDaemon *daemon_file;
  int len;

  daemon_file = g_object_new (G_TYPE_FILE_DAEMON, NULL);
  /* TODO: These should be construct only properties */
  daemon_file->match_bus_name = match_bus_name;
  daemon_file->path = g_strdup (path);

  /* Remove any trailing slashes */
  len = strlen (daemon_file->path);
  while (len > 1 && daemon_file->path[len-1] == '/')
    {
      daemon_file->path[len-1] = 0;
      len--;
    }
  
  return G_FILE (daemon_file);
}

static gboolean
g_file_daemon_is_native (GFile *file)
{
  return FALSE;
}

static char *
g_file_daemon_get_path (GFile *file)
{
  return NULL;
}

static char *
g_file_daemon_get_uri (GFile *file)
{
  /* TODO: implement to-uri */
  return NULL;
}

static char *
g_file_daemon_get_parse_name (GFile *file)
{
  /* TODO: implement to-iri */
  return NULL;
}

static GFile *
g_file_daemon_get_parent (GFile *file)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);
  const char *path;
  GFileDaemon *parent;
  const char *base;
  char *parent_path;
  gsize len;    

  path = daemon_file->path;
  base = strrchr (path, '/');
  if (base == NULL || base == path)
    return NULL;

  while (base > path && *base == '/')
    base--;

  len = (guint) 1 + base - path;
  
  parent_path = g_new (gchar, len + 1);
  g_memmove (parent_path, path, len);
  parent_path[len] = 0;

  parent = g_object_new (G_TYPE_FILE_DAEMON, NULL);
  parent->match_bus_name = daemon_file->match_bus_name;
  parent->path = parent_path;
  
  return G_FILE (parent);
}

static GFile *
g_file_daemon_copy (GFile *file)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);

  return g_file_daemon_new (daemon_file->match_bus_name,
			    daemon_file->path);
}


static GFile *
g_file_daemon_get_child (GFile *file,
			 const char *name)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);
  char *path;
  GFile *child;

  path = g_build_filename (daemon_file->path, name, NULL);
  child = g_file_daemon_new (daemon_file->match_bus_name, path);
  g_free (path);
  
  return child;
}

static DBusMessage *
do_sync_path_call (GFile *file,
		   const char *op,
		   DBusConnection **connection_out,
		   GCancellable *cancellable,
		   GError **error,
		   int first_arg_type,
		   ...)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);
  va_list var_args;
  DBusMessage *message, *reply;

  va_start (var_args, first_arg_type);
  message =
    _g_vfs_impl_daemon_new_path_call_valist (daemon_file->match_bus_name,
					     daemon_file->path,
					     op,
					     first_arg_type, var_args);
  va_end (var_args);

  if (message == NULL)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_NOT_MOUNTED,
		   _("The share was not mounted"));
      return NULL;
    }
  
  reply = _g_vfs_daemon_call_sync (message,
				   connection_out,
				   cancellable, error);
  dbus_message_unref (message);
  
  return reply;
}

static GFileEnumerator *
g_file_daemon_enumerate_children (GFile      *file,
				  GFileInfoRequestFlags requested,
				  const char *attributes,
				  gboolean follow_symlinks,
				  GCancellable *cancellable,
				  GError **error)
{
  DBusMessage *reply;
  guint32 requested_32;
  dbus_bool_t follow_symlinks_dbus;
  DBusMessageIter iter;
  char *obj_path;
  GFileEnumeratorDaemon *enumerator;
  DBusConnection *connection;

  enumerator = g_file_enumerator_daemon_new ();
  obj_path = g_file_enumerator_daemon_get_object_path (enumerator);
						       
  requested_32 = (guint32)requested;
  if (attributes == NULL)
    attributes = "";
  follow_symlinks_dbus = follow_symlinks;
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_OP_ENUMERATE,
			     &connection, cancellable, error,
			     DBUS_TYPE_STRING, &obj_path,
			     DBUS_TYPE_UINT32, &requested_32,
			     DBUS_TYPE_STRING, &attributes,
			     DBUS_TYPE_BOOLEAN, &follow_symlinks_dbus,
			     0);
  
  g_free (obj_path);

  if (reply == NULL)
    goto error;

  if (!dbus_message_iter_init (reply, &iter) ||
      (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_UINT32))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Invalid return value from Enumerate"));
      goto error;
    }
  
  dbus_message_iter_get_basic (&iter, &requested_32);
  dbus_message_unref (reply);

  g_file_enumerator_daemon_set_sync_connection (enumerator, connection);
  g_file_enumerator_daemon_set_request_flags (enumerator, requested_32);
  
  return G_FILE_ENUMERATOR (enumerator);

 error:
  if (reply)
    dbus_message_unref (reply);
  g_object_unref (enumerator);
  return NULL;
}

static GFileInfo *
g_file_daemon_get_info (GFile                *file,
			GFileInfoRequestFlags requested,
			const char           *attributes,
			gboolean              follow_symlinks,
			GCancellable         *cancellable,
			GError              **error)
{
  DBusMessage *reply;
  guint32 requested_32;
  dbus_bool_t follow_symlinks_dbus;
  DBusMessageIter iter;
  GFileInfo *info;

  requested_32 = (guint32)requested;
  if (attributes == NULL)
    attributes = "";
  follow_symlinks_dbus = follow_symlinks;
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_OP_GET_INFO,
			     NULL, cancellable, error,
			     DBUS_TYPE_UINT32, &requested_32,
			     DBUS_TYPE_STRING, &attributes,
			     DBUS_TYPE_BOOLEAN, &follow_symlinks_dbus,
			     0);
  if (reply == NULL)
    return NULL;

  info = NULL;
  
  if (!dbus_message_iter_init (reply, &iter) ||
      (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_UINT32))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Invalid return value from get_info"));
      goto out;
    }

  dbus_message_iter_get_basic (&iter, &requested_32);
  
  if (!dbus_message_iter_next (&iter))
    goto out;
  
  info = _g_dbus_get_file_info (&iter, requested_32, error);

 out:
  dbus_message_unref (reply);
  return info;
}

typedef struct {
  GFile *file;
  GFileReadCallback read_callback;
  gpointer callback_data;
  gboolean can_seek;
} GetFDData;

static void
read_async_get_fd_cb (int fd,
		      gpointer callback_data)
{
  GetFDData *data = callback_data;
  GFileInputStream *stream;
  GError *error;
  
  if (fd == -1)
    {
      error = NULL;
      g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Didn't get stream file descriptor"));
      data->read_callback (data->file, NULL, data->callback_data, error);
      g_error_free (error);
    }
  else
    {
      stream = g_file_input_stream_daemon_new (fd, data->can_seek);
      data->read_callback (data->file, stream, data->callback_data, NULL);
      g_object_unref (stream);
    }
  g_free (data);
}

static void
read_async_cb (DBusMessage *reply,
	       DBusConnection *connection,
	       GError *io_error,
	       GCancellable *cancellable,
	       gpointer op_callback,
	       gpointer op_callback_data,
	       gpointer callback_data)
{
  GFileReadCallback read_callback = op_callback;
  GFile *file = callback_data;
  GError *error;
  guint32 fd_id;
  dbus_bool_t can_seek;
  GetFDData *get_fd_data;
  
  if (io_error != NULL)
    {
      read_callback (file, NULL, op_callback_data, io_error);
    }
  else
    {
      if (!dbus_message_get_args (reply, NULL,
				  DBUS_TYPE_UINT32, &fd_id,
				  DBUS_TYPE_BOOLEAN, &can_seek,
				  DBUS_TYPE_INVALID))
	{
	  error = NULL;
	  g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       _("Invalid return value from open"));
	  read_callback (file, NULL, op_callback_data, error);
	  g_error_free (error);
	}

      get_fd_data = g_new0 (GetFDData, 1);
      get_fd_data->file = file;
      get_fd_data->read_callback = read_callback;
      get_fd_data->callback_data = op_callback_data;
      get_fd_data->can_seek = can_seek;
      
      _g_dbus_connection_get_fd_async (connection, fd_id,
				       read_async_get_fd_cb, get_fd_data);
    }
}

static void
g_file_daemon_read_async (GFile *file,
			  int io_priority,
			  GFileReadCallback callback,
			  gpointer callback_data,
			  GCancellable *cancellable)
{
  GFileDaemon *daemon_file = G_FILE_DAEMON (file);
  DBusMessage *message;
  
  message =
    _g_vfs_impl_daemon_new_path_call (daemon_file->match_bus_name,
				      daemon_file->path,
				      G_VFS_DBUS_OP_OPEN_FOR_READ,
				      0);
  /* TODO: handle nonmounted => message == NULL */
  _g_vfs_daemon_call_async (message,
			    callback, callback_data,
			    read_async_cb, file,
			    cancellable);
  dbus_message_unref (message);
}

static GFileInputStream *
g_file_daemon_read (GFile *file,
		    GCancellable *cancellable,
		    GError **error)
{
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;

  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_OP_OPEN_FOR_READ,
			     &connection, cancellable, error,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Invalid return value from open"));
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_file_input_stream_daemon_new (fd, can_seek);
}

static GFileOutputStream *
g_file_daemon_append_to (GFile *file,
			 GCancellable *cancellable,
			 GError **error)
{
  /* TODO: implement */
  return NULL;
}

static GFileOutputStream *
g_file_daemon_create (GFile *file,
		      GCancellable *cancellable,
		      GError **error)
{
  /* TODO: implement */
  return NULL;
}

static GFileOutputStream *
g_file_daemon_replace (GFile *file,
		       time_t mtime,
		       gboolean  make_backup,
		       GCancellable *cancellable,
		       GError **error)
{
  /* TODO: implement */
  return NULL;
}

static void
g_file_daemon_file_iface_init (GFileIface *iface)
{
  iface->copy = g_file_daemon_copy;
  iface->is_native = g_file_daemon_is_native;
  iface->get_path = g_file_daemon_get_path;
  iface->get_uri = g_file_daemon_get_uri;
  iface->get_parse_name = g_file_daemon_get_parse_name;
  iface->get_parent = g_file_daemon_get_parent;
  iface->get_child = g_file_daemon_get_child;
  iface->enumerate_children = g_file_daemon_enumerate_children;
  iface->get_info = g_file_daemon_get_info;
  iface->read = g_file_daemon_read;
  iface->append_to = g_file_daemon_append_to;
  iface->create = g_file_daemon_create;
  iface->replace = g_file_daemon_replace;
  iface->read_async = g_file_daemon_read_async;
}