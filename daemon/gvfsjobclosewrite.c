#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>
#include "gvfswritechannel.h"
#include "gvfsjobclosewrite.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobCloseWrite, g_vfs_job_close_write, G_VFS_TYPE_JOB);

static void run (GVfsJob *job);
static gboolean try (GVfsJob *job);
static void send_reply (GVfsJob *job);

static void
g_vfs_job_close_write_finalize (GObject *object)
{
  GVfsJobCloseWrite *job;

  job = G_VFS_JOB_CLOSE_WRITE (object);
  g_object_unref (job->channel);

  if (G_OBJECT_CLASS (g_vfs_job_close_write_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_close_write_parent_class)->finalize) (object);
}

static void
g_vfs_job_close_write_class_init (GVfsJobCloseWriteClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_close_write_finalize;

  job_class->run = run;
  job_class->try = try;
  job_class->send_reply = send_reply;
}

static void
g_vfs_job_close_write_init (GVfsJobCloseWrite *job)
{
}

GVfsJob *
g_vfs_job_close_write_new (GVfsWriteChannel *channel,
			  GVfsBackendHandle handle,
			  GVfsBackend *backend)
{
  GVfsJobCloseWrite *job;
  
  job = g_object_new (G_VFS_TYPE_JOB_CLOSE_WRITE,
		      NULL);

  job->channel = g_object_ref (channel);
  job->backend = backend;
  job->handle = handle;
  
  return G_VFS_JOB (job);
}

/* Might be called on an i/o thwrite */
static void
send_reply (GVfsJob *job)
{
  GVfsJobCloseWrite *op_job = G_VFS_JOB_CLOSE_WRITE (job);
  
  g_print ("job_close_write send reply\n");

  if (job->failed)
    g_vfs_channel_send_error (G_VFS_CHANNEL (op_job->channel), job->error);
  else
    g_vfs_write_channel_send_closed (op_job->channel);
}

static void
run (GVfsJob *job)
{
  GVfsJobCloseWrite *op_job = G_VFS_JOB_CLOSE_WRITE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  
  class->close_write (op_job->backend,
		      op_job,
		      op_job->handle);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobCloseWrite *op_job = G_VFS_JOB_CLOSE_WRITE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  
  if (class->try_close_write == NULL)
    return FALSE;
  
  return class->try_close_write (op_job->backend,
				 op_job,
				 op_job->handle);
}