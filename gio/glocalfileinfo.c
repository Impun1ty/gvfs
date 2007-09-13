#include <config.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif

#include <sys/types.h>
#ifdef HAVE_XATTR

#if defined HAVE_SYS_XATTR_H
  #include <sys/xattr.h>
#elif defined HAVE_ATTR_XATTR_H
  #include <attr/xattr.h>
#else
  #error "Neither <sys/xattr.h> nor <attr/xattr.h> is present but extended attribute support is enabled."
#endif /* defined HAVE_SYS_XATTR_H || HAVE_ATTR_XATTR_H */

#endif /* HAVE_XATTR */

#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>

#include "glocalfileinfo.h"
#include "gioerror.h"
#include "gcontenttype.h"
#include "gcontenttypeprivate.h"

char *
_g_local_file_info_create_etag (struct stat *statbuf)
{
  GTimeVal tv;
  
  tv.tv_sec = statbuf->st_mtime;
#if defined (HAVE_STRUCT_STAT_ST_MTIMENSEC)
  tv.tv_usec = statbuf->st_mtimensec / 1000;
#elif defined (HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC)
  tv.tv_usec = statbuf->st_mtim.tv_nsec / 1000;
#else
  tv.tv_usec = 0;
#endif

  return g_strdup_printf ("%ld:%ld", tv.tv_sec, tv.tv_usec);
}

static gchar *
read_link (const gchar *full_name)
{
#ifdef HAVE_READLINK
  gchar *buffer;
  guint size;
  
  size = 256;
  buffer = g_malloc (size);
  
  while (1)
    {
      int read_size;
      
      read_size = readlink (full_name, buffer, size);
      if (read_size < 0)
	{
	  g_free (buffer);
	  return NULL;
	}
      if (read_size < size)
	{
	  buffer[read_size] = 0;
	  return buffer;
	}
      size *= 2;
      buffer = g_realloc (buffer, size);
    }
#else
  return NULL;
#endif
}

/* Get the SELinux security context */
static void
get_selinux_context (const char *path,
		     GFileInfo *info,
		     GFileAttributeMatcher *attribute_matcher,
		     gboolean follow_symlinks)
{
#ifdef HAVE_SELINUX
  char *context;

  if (!g_file_attribute_matcher_matches (attribute_matcher, "selinux:context"))
    return;
  
  if (is_selinux_enabled ())
    {
      if (follow_symlinks)
	{
	  if (lgetfilecon_raw (path, &context) < 0)
	    return;
	}
      else
	{
	  if (getfilecon_raw (path, &context) < 0)
	    return;
	}

      if (context)
	{
	  g_file_info_set_attribute_string (info, "selinux:context", context);
	  freecon(context);
	}
    }
#endif
}

#ifdef HAVE_XATTR

static gboolean
valid_char (char c)
{
  return c >= 32 && c <= 126 && c != '\\';
}

static gboolean
name_is_valid (const char *str)
{
  while (*str)
    {
      if (!valid_char (*str))
	return FALSE;
    }
  return TRUE;
}

static char *
hex_escape_string (const char *str, gboolean *free_return)
{
  int num_invalid, i;
  char *escaped_str, *p;
  unsigned char c;
  static char *hex_digits = "0123456789abcdef";
  int len;

  len = strlen (str);
  
  num_invalid = 0;
  for (i = 0; i < len; i++)
    {
      if (!valid_char (str[i]))
	num_invalid++;
    }

  if (num_invalid == 0)
    {
      *free_return = FALSE;
      return (char *)str;
    }

  escaped_str = g_malloc (len + num_invalid*3 + 1);

  p = escaped_str;
  for (i = 0; i < len; i++)
    {
      if (valid_char (str[i]))
	*p++ = str[i];
      else
	{
	  c = str[i];
	  *p++ = '\\';
	  *p++ = 'x';
	  *p++ = hex_digits[(c >> 4) & 0xf];
	  *p++ = hex_digits[c & 0xf];
	}
    }
  *p++ = 0;

  *free_return = TRUE;
  return escaped_str;
}

static char *
hex_unescape_string (const char *str, int *out_len, gboolean *free_return)
{
  int i;
  char *unescaped_str, *p;
  unsigned char c;
  int len;

  len = strlen (str);
  
  if (strchr (str, '\\') == NULL)
    {
      if (out_len)
	*out_len = len;
      *free_return = FALSE;
      return (char *)str;
    }
  
  unescaped_str = g_malloc (len + 1);

  p = unescaped_str;
  for (i = 0; i < len; i++)
    {
      if (str[i] == '\\' &&
	  str[i+1] == 'x' &&
	  len - i >= 4)
	{
	  c =
	    (g_ascii_xdigit_value (str[i+2]) << 4) |
	    g_ascii_xdigit_value (str[i+3]);
	  *p++ = c;
	  i += 3;
	}
      else
	*p++ = str[i];
    }
  *p++ = 0;

  if (out_len)
    *out_len = p - unescaped_str;
  *free_return = TRUE;
  return unescaped_str;
}

static void
escape_xattr (GFileInfo *info,
	      const char *gio_attr, /* gio attribute name */
	      const char *value, /* Is zero terminated */
	      size_t len /* not including zero termination */)
{
  char *escaped_val;
  gboolean free_escaped_val;
  
  escaped_val = hex_escape_string (value, &free_escaped_val);
  
  g_file_info_set_attribute_string (info, gio_attr, escaped_val);
  
  if (free_escaped_val)
    g_free (escaped_val);
}

static void
get_one_xattr (const char *path,
	       GFileInfo *info,
	       const char *gio_attr,
	       const char *xattr,
	       gboolean follow_symlinks)
{
  char value[64];
  char *value_p;
  ssize_t len;

  if (follow_symlinks)  
    len = getxattr (path, xattr, value, sizeof (value)-1);
  else
    len = lgetxattr (path, xattr,value, sizeof (value)-1);

  value_p = NULL;
  if (len >= 0)
    value_p = value;
  else if (len == -1 && errno == ERANGE)
    {
      if (follow_symlinks)  
	len = getxattr (path, xattr, NULL, 0);
      else
	len = lgetxattr (path, xattr, NULL, 0);

      if (len < 0)
	return;

      value_p = g_malloc (len+1);

      if (follow_symlinks)  
	len = getxattr (path, xattr, value_p, len);
      else
	len = lgetxattr (path, xattr, value_p, len);

      if (len < 0)
	{
	  g_free (value_p);
	  return;
	}
    }
  else
    return;
  
  /* Null terminate */
  value_p[len] = 0;

  escape_xattr (info, gio_attr, value_p, len);
  
  if (value_p != value)
    g_free (value_p);
}

#endif /* defined HAVE_XATTR */

static void
get_xattrs (const char *path,
	    gboolean user,
	    GFileInfo *info,
	    GFileAttributeMatcher *matcher,
	    gboolean follow_symlinks)
{
#ifdef HAVE_XATTR
  gboolean all;
  gsize list_size;
  ssize_t list_res_size;
  size_t len;
  char *list;
  const char *attr, *attr2;

  if (user)
    all = g_file_attribute_matcher_enumerate_namespace (matcher, "xattr");
  else
    all = g_file_attribute_matcher_enumerate_namespace (matcher, "xattr_sys");

  if (all)
    {
      if (follow_symlinks)
	list_res_size = listxattr (path, NULL, 0);
      else
	list_res_size = llistxattr (path, NULL, 0);

      if (list_res_size == -1 ||
	  list_res_size == 0)
	return;

      list_size = list_res_size;
      list = g_malloc (list_size);

    retry:
      
      if (follow_symlinks)
	list_res_size = listxattr (path, list, list_size);
      else
	list_res_size = llistxattr (path, list, list_size);
      
      if (list_res_size == -1 && errno == ERANGE)
	{
	  list_size = list_size * 2;
	  list = g_realloc (list, list_size);
	  goto retry;
	}

      if (list_res_size == -1)
	return;

      attr = list;
      while (list_res_size > 0)
	{
	  if ((user && g_str_has_prefix (attr, "user.")) ||
	      (!user && !g_str_has_prefix (attr, "user.")))
	    {
	      char *escaped_attr, *gio_attr;
	      gboolean free_escaped_attr;
	      
	      if (user)
		{
		  escaped_attr = hex_escape_string (attr + 5, &free_escaped_attr);
		  gio_attr = g_strconcat ("xattr:", escaped_attr, NULL);
		}
	      else
		{
		  escaped_attr = hex_escape_string (attr, &free_escaped_attr);
		  gio_attr = g_strconcat ("xattr_sys:", escaped_attr, NULL);
		}
	      
	      if (free_escaped_attr)
		g_free (escaped_attr);
	      
	      get_one_xattr (path, info, gio_attr, attr, follow_symlinks);
	    }
	      
	  len = strlen (attr) + 1;
	  attr += len;
	  list_res_size -= len;
	}

      g_free (list);
    }
  else
    {
      while ((attr = g_file_attribute_matcher_enumerate_next (matcher)) != NULL)
	{
	  char *unescaped_attribute, *a;
	  gboolean free_unescaped_attribute;

	  attr2 = strchr (attr, ':');
	  if (attr2)
	    {
	      attr2++; /* Skip ':' */
	      unescaped_attribute = hex_unescape_string (attr2, NULL, &free_unescaped_attribute);
	      if (user)
		a = g_strconcat ("user.", unescaped_attribute, NULL);
	      else
		a = unescaped_attribute;
	      
	      get_one_xattr (path, info, attr, a, follow_symlinks);

	      if (user)
		g_free (a);
	      
	      if (free_unescaped_attribute)
		g_free (unescaped_attribute);
	    }
	}
    }
#endif /* defined HAVE_XATTR */
}

#ifdef HAVE_XATTR
static void
get_one_xattr_from_fd (int fd,
		       GFileInfo *info,
		       const char *gio_attr,
		       const char *xattr)
{
  char value[64];
  char *value_p;
  ssize_t len;

  len = fgetxattr (fd, xattr, value, sizeof (value)-1);

  value_p = NULL;
  if (len >= 0)
    value_p = value;
  else if (len == -1 && errno == ERANGE)
    {
      len = fgetxattr (fd, xattr, NULL, 0);

      if (len < 0)
	return;

      value_p = g_malloc (len+1);

      len = fgetxattr (fd, xattr, value_p, len);

      if (len < 0)
	{
	  g_free (value_p);
	  return;
	}
    }
  else
    return;
  
  /* Null terminate */
  value_p[len] = 0;

  escape_xattr (info, gio_attr, value_p, len);
  
  if (value_p != value)
    g_free (value_p);
}
#endif /* defined HAVE_XATTR */

static void
get_xattrs_from_fd (int fd,
		    gboolean user,
		    GFileInfo *info,
		    GFileAttributeMatcher *matcher)
{
#ifdef HAVE_XATTR
  gboolean all;
  gsize list_size;
  ssize_t list_res_size;
  size_t len;
  char *list;
  const char *attr, *attr2;

  if (user)
    all = g_file_attribute_matcher_enumerate_namespace (matcher, "xattr");
  else
    all = g_file_attribute_matcher_enumerate_namespace (matcher, "xattr_sys");

  if (all)
    {
      list_res_size = flistxattr (fd, NULL, 0);

      if (list_res_size == -1 ||
	  list_res_size == 0)
	return;

      list_size = list_res_size;
      list = g_malloc (list_size);

    retry:
      
      list_res_size = flistxattr (fd, list, list_size);
      
      if (list_res_size == -1 && errno == ERANGE)
	{
	  list_size = list_size * 2;
	  list = g_realloc (list, list_size);
	  goto retry;
	}

      if (list_res_size == -1)
	return;

      attr = list;
      while (list_res_size > 0)
	{
	  if ((user && g_str_has_prefix (attr, "user.")) ||
	      (!user && !g_str_has_prefix (attr, "user.")))
	    {
	      char *escaped_attr, *gio_attr;
	      gboolean free_escaped_attr;
	      
	      if (user)
		{
		  escaped_attr = hex_escape_string (attr + 5, &free_escaped_attr);
		  gio_attr = g_strconcat ("xattr:", escaped_attr, NULL);
		}
	      else
		{
		  escaped_attr = hex_escape_string (attr, &free_escaped_attr);
		  gio_attr = g_strconcat ("xattr_sys:", escaped_attr, NULL);
		}
	      
	      if (free_escaped_attr)
		g_free (escaped_attr);
	      
	      get_one_xattr_from_fd (fd, info, gio_attr, attr);
	    }
	  
	  len = strlen (attr) + 1;
	  attr += len;
	  list_res_size -= len;
	}

      g_free (list);
    }
  else
    {
      while ((attr = g_file_attribute_matcher_enumerate_next (matcher)) != NULL)
	{
	  char *unescaped_attribute, *a;
	  gboolean free_unescaped_attribute;

	  attr2 = strchr (attr, ':');
	  if (attr2)
	    {
	      attr2++; /* Skip ':' */
	      unescaped_attribute = hex_unescape_string (attr2, NULL, &free_unescaped_attribute);
	      if (user)
		a = g_strconcat ("user.", unescaped_attribute, NULL);
	      else
		a = unescaped_attribute;
	      
	      get_one_xattr_from_fd (fd, info, attr, a);

	      if (user)
		g_free (a);
	      
	      if (free_unescaped_attribute)
		g_free (unescaped_attribute);
	    }
	}
    }
#endif /* defined HAVE_XATTR */
}

#ifdef HAVE_XATTR
static gboolean
set_xattr (char *filename,
	   const char *escaped_attribute,
	   const GFileAttributeValue *attr_value,
	   GError **error)
{
  char *attribute, *value;
  gboolean free_attribute, free_value;
  int val_len, res, errsv;
  gboolean is_user;
  char *a;

  if (attr_value->type != G_FILE_ATTRIBUTE_TYPE_STRING)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   _("Invalid attribute type (string expected)"));
      return FALSE;
    }

  if (!name_is_valid (escaped_attribute))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   _("Invalid extended attribute name"));
      return FALSE;
    }

  if (g_str_has_prefix (escaped_attribute, "xattr:"))
    {
      escaped_attribute += 6;
      is_user = TRUE;
    }
  else
    {
      g_assert (g_str_has_prefix (escaped_attribute, "xattr_sys:"));
      escaped_attribute += 10;
      is_user = FALSE;
    }
  
  attribute = hex_unescape_string (escaped_attribute, NULL, &free_attribute);
  value = hex_unescape_string (attr_value->u.string, &val_len, &free_value);


  if (is_user)
    a = g_strconcat ("user.", attribute, NULL);
  else
    a = attribute;
  
  res = setxattr (filename, a, value, val_len, 0);
  errsv = errno;
  
  if (is_user)
    g_free (a);
  
  if (free_attribute)
    g_free (attribute);
  
  if (free_value)
    g_free (value);

  if (res == -1)
    {
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errsv),
		   _("Error setting extended attribute '%s': %s"),
		   escaped_attribute, g_strerror (errno));
      return FALSE;
    }
  
  return TRUE;
}

#endif


void
_g_local_file_info_get_parent_info (const char             *dir,
				    GFileAttributeMatcher  *attribute_matcher,
				    GLocalParentFileInfo   *parent_info)
{
  struct stat statbuf;
  int res;
  
  parent_info->writable = FALSE;
  parent_info->is_sticky = FALSE;

  if (g_file_attribute_matcher_matches (attribute_matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME) ||
      g_file_attribute_matcher_matches (attribute_matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE))
    {
      parent_info->writable = (g_access (dir, W_OK) == 0);
      
      if (parent_info->writable)
	{
	  res = g_stat (dir, &statbuf);

	  /*
	   * The sticky bit (S_ISVTX) on a directory means that a file in that directory can be
	   * renamed or deleted only by the owner of the file, by the owner of the directory, and
	   * by a privileged process.
	   */
	  if (res == 0)
	    {
	      parent_info->is_sticky = (statbuf.st_mode & S_ISVTX) != 0;
	      parent_info->owner = statbuf.st_uid;
	    }
	}
    }
}

static void
get_access_rights (GFileAttributeMatcher *attribute_matcher,
		   GFileInfo *info,
		   const gchar *path,
		   struct stat *statbuf,
		   GLocalParentFileInfo *parent_info)
{
  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_ACCESS_CAN_READ))
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ,
				       g_access (path, R_OK) == 0);
  
  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
				       g_access (path, W_OK) == 0);
  
  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
				       g_access (path, X_OK) == 0);


  if (parent_info)
    {
      gboolean writable;

      writable = FALSE;
      if (parent_info->writable)
	{
	  if (parent_info->is_sticky)
	    {
	      uid_t uid = geteuid ();

	      if (uid == statbuf->st_uid ||
		  uid == parent_info->owner ||
		  uid == 0)
		writable = TRUE;
	    }
	  else
	    writable = TRUE;
	}

      if (g_file_attribute_matcher_matches (attribute_matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME))
	g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME,
					   writable);
      
      if (g_file_attribute_matcher_matches (attribute_matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE))
	g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE,
					   writable);
    }
}

static void
set_info_from_stat (GFileInfo *info, struct stat *statbuf,
		    GFileAttributeMatcher *attribute_matcher)
{
  GFileType file_type;

  file_type = G_FILE_TYPE_UNKNOWN;

  if (S_ISREG (statbuf->st_mode))
    file_type = G_FILE_TYPE_REGULAR;
  else if (S_ISDIR (statbuf->st_mode))
    file_type = G_FILE_TYPE_DIRECTORY;
  else if (S_ISCHR (statbuf->st_mode) ||
	   S_ISBLK (statbuf->st_mode) ||
	   S_ISFIFO (statbuf->st_mode)
#ifdef S_ISSOCK
	   || S_ISSOCK (statbuf->st_mode)
#endif
	   )
    file_type = G_FILE_TYPE_SPECIAL;
#ifdef S_ISLNK
  else if (S_ISLNK (statbuf->st_mode))
    file_type = G_FILE_TYPE_SYMBOLIC_LINK;
#endif

  g_file_info_set_file_type (info, file_type);
  g_file_info_set_size (info, statbuf->st_size);

  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_DEVICE, statbuf->st_dev);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_INODE, statbuf->st_ino);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE, statbuf->st_mode);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_NLINK, statbuf->st_nlink);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID, statbuf->st_uid);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID, statbuf->st_uid);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_RDEV, statbuf->st_rdev);
#if defined (HAVE_STRUCT_STAT_BLKSIZE)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_BLOCK_SIZE, statbuf->st_blksize);
#endif
#if defined (HAVE_STRUCT_STAT_BLOCKS)
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_BLOCKS, statbuf->st_blocks);
#endif
  
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, statbuf->st_mtime);
#if defined (HAVE_STRUCT_STAT_ST_MTIMENSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, statbuf->st_mtimensec / 1000);
#elif defined (HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, statbuf->st_mtim.tv_nsec / 1000);
#endif
  
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS, statbuf->st_atime);
#if defined (HAVE_STRUCT_STAT_ST_ATIMENSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC, statbuf->st_atimensec / 1000);
#elif defined (HAVE_STRUCT_STAT_ST_ATIM_TV_NSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC, statbuf->st_atim.tv_nsec / 1000);
#endif
  
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CHANGED, statbuf->st_ctime);
#if defined (HAVE_STRUCT_STAT_ST_CTIMENSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC, statbuf->st_ctimensec / 1000);
#elif defined (HAVE_STRUCT_STAT_ST_CTIM_TV_NSEC)
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC, statbuf->st_ctim.tv_nsec / 1000);
#endif

  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_ETAG_VALUE))
    {
      char *etag = _g_local_file_info_create_etag (statbuf);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE, etag);
      g_free (etag);
    }

}

GFileInfo *
_g_local_file_info_get (const char *basename,
			const char *path,
			GFileAttributeMatcher *attribute_matcher,
			GFileGetInfoFlags flags,
			GLocalParentFileInfo *parent_info,
			GError **error)
{
  GFileInfo *info;
  struct stat statbuf;
  struct stat statbuf2;
  int res;
  gboolean is_symlink, symlink_broken;

  info = g_file_info_new ();

  /* Make sure we don't set any unwanted attributes */
  g_file_info_set_attribute_mask (info, attribute_matcher);
  
  g_file_info_set_name (info, basename);

  /* Avoid stat in trivial case */
  if (attribute_matcher == NULL)
    return info;

  res = g_lstat (path, &statbuf);
  if (res == -1)
    {
      g_object_unref (info);
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error stating file '%s': %s"),
		   path, g_strerror (errno));
      return NULL;
    }
  
#ifdef S_ISLNK
  is_symlink = S_ISLNK (statbuf.st_mode);
#else
  is_symlink = FALSE;
#endif
  symlink_broken = FALSE;
  
  if (is_symlink)
    {
      g_file_info_set_is_symlink (info, TRUE);

      /* Unless NOFOLLOW was set we default to following symlinks */
      if (!(flags & G_FILE_GET_INFO_NOFOLLOW_SYMLINKS))
	{
	  res = stat (path, &statbuf2);

	    /* Report broken links as symlinks */
	  if (res != -1)
	    {
	      statbuf = statbuf2;
	      symlink_broken = TRUE;
	    }
	}
    }

  set_info_from_stat (info, &statbuf, attribute_matcher);
  
  if (basename != NULL && basename[0] == '.')
    g_file_info_set_is_hidden (info, TRUE);

  if (basename != NULL && basename[strlen (basename) -1] == '~')
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STD_IS_BACKUP, TRUE);

  if (is_symlink &&
      g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_STD_SYMLINK_TARGET))
    {
      char *link = read_link (path);
      g_file_info_set_symlink_target (info, link);
      g_free (link);
    }

  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_STD_DISPLAY_NAME))
    {
      char *display_name = g_filename_display_basename (path);
      
      if (strstr (display_name, "\357\277\275") != NULL)
	{
	  char *p = display_name;
	  display_name = g_strconcat (display_name, _(" (invalid encoding)"), NULL);
	  g_free (p);
	}
      g_file_info_set_display_name (info, display_name);
      g_free (display_name);
    }
  
  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_STD_EDIT_NAME))
    {
      char *edit_name = g_filename_display_basename (path);
      g_file_info_set_edit_name (info, edit_name);
      g_free (edit_name);
    }

  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_STD_CONTENT_TYPE))
    {
      /* TODO: Add windows specific code */

      if (is_symlink &&
	  (symlink_broken || (flags & G_FILE_GET_INFO_NOFOLLOW_SYMLINKS)))
	g_file_info_set_content_type (info, "inode/symlink");
      else if (S_ISDIR(statbuf.st_mode))
	g_file_info_set_content_type (info, "inode/directory");
      else if (S_ISCHR(statbuf.st_mode))
	g_file_info_set_content_type (info, "inode/chardevice");
      else if (S_ISBLK(statbuf.st_mode))
	g_file_info_set_content_type (info, "inode/blockdevice");
      else if (S_ISFIFO(statbuf.st_mode))
	g_file_info_set_content_type (info, "inode/fifo");
#ifdef S_ISSOCK
      else if (S_ISSOCK(statbuf.st_mode))
	g_file_info_set_content_type (info, "inode/socket");
#endif
      else
	{
	  char *content_type;
	  
	  content_type = g_content_type_guess (basename, NULL, 0);

#ifndef G_OS_WIN32
	  if (g_content_type_is_unknown (content_type) && path != NULL)
	    {
	      guchar sniff_buffer[4096];
	      gsize sniff_length;
	      int fd;
	      
	      sniff_length = _g_unix_content_type_get_sniff_len ();
	      if (sniff_length > 4096)
		sniff_length = 4096;

	      fd = open (path, O_RDONLY);
	      if (fd != -1)
		{
		  ssize_t res;

		  res = read (fd, sniff_buffer, sniff_length);
		  close (fd);
		  if (res != -1)
		    {
		      g_free (content_type);
		      content_type = g_content_type_guess (basename, sniff_buffer, sniff_length);
		    }
		  
		}
	    }
#endif

	  g_file_info_set_content_type (info, content_type);
	  g_free (content_type);
	}
            
    }
  
  if (g_file_attribute_matcher_matches (attribute_matcher,
					G_FILE_ATTRIBUTE_STD_ICON))
    {
      /* TODO */
    }

  get_access_rights (attribute_matcher, info, path, &statbuf, parent_info);
  
  get_selinux_context (path, info, attribute_matcher, (flags & G_FILE_GET_INFO_NOFOLLOW_SYMLINKS) == 0);
  get_xattrs (path, TRUE, info, attribute_matcher, (flags & G_FILE_GET_INFO_NOFOLLOW_SYMLINKS) == 0);
  get_xattrs (path, FALSE, info, attribute_matcher, (flags & G_FILE_GET_INFO_NOFOLLOW_SYMLINKS) == 0);
  
  g_file_info_unset_attribute_mask (info);

  return info;
}

GFileInfo *
_g_local_file_info_get_from_fd (int fd,
				char *attributes,
				GError **error)
{
  struct stat stat_buf;
  GFileAttributeMatcher *matcher;
  GFileInfo *info;
  
  if (fstat (fd, &stat_buf) == -1)
    {
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error stating file descriptor: %s"),
		   g_strerror (errno));
      return NULL;
    }

  info = g_file_info_new ();

  matcher = g_file_attribute_matcher_new (attributes);

  /* Make sure we don't set any unwanted attributes */
  g_file_info_set_attribute_mask (info, matcher);
  
  set_info_from_stat (info, &stat_buf, matcher);
  
#ifdef HAVE_SELINUX
  if (g_file_attribute_matcher_matches (matcher, "selinux:context") &&
      is_selinux_enabled ())
    {
      char *context;
      if (fgetfilecon_raw (fd, &context) >= 0)
	{
	  g_file_info_set_attribute_string (info, "selinux:context", context);
	  freecon(context);
	}
    }
#endif

  get_xattrs_from_fd (fd, TRUE, info, matcher);
  get_xattrs_from_fd (fd, FALSE, info, matcher);
  
  g_file_attribute_matcher_unref (matcher);

  g_file_info_unset_attribute_mask (info);
  
  return info;
}

static gboolean
get_uint32 (const GFileAttributeValue *value,
	    guint32 *val_out,
	    GError **error)
{
  if (value->type != G_FILE_ATTRIBUTE_TYPE_UINT32)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   _("Invalid attribute type (uint32 expected)"));
      return FALSE;
    }

  *val_out = value->u.uint32;
  
  return TRUE;
}

static gboolean
get_uint64 (const GFileAttributeValue *value,
	    guint64 *val_out,
	    GError **error)
{
  if (value->type != G_FILE_ATTRIBUTE_TYPE_UINT64)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   _("Invalid attribute type (uint64 expected)"));
      return FALSE;
    }

  *val_out = value->u.uint64;
  
  return TRUE;
}

#if defined(HAVE_SYMLINK)
static gboolean
get_byte_string (const GFileAttributeValue *value,
		 const char **val_out,
		 GError **error)
{
  if (value->type != G_FILE_ATTRIBUTE_TYPE_BYTE_STRING)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   _("Invalid attribute type (byte string expected)"));
      return FALSE;
    }

  *val_out = value->u.string;
  
  return TRUE;
}
#endif

static gboolean
set_unix_mode (char *filename,
	       const GFileAttributeValue *value,
	       GError **error)
{
  guint32 val;
  
  if (!get_uint32 (value, &val, error))
    return FALSE;
  
  if (g_chmod (filename, val) == -1)
    {
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error setting permissions: %s"),
		   g_strerror (errno));
      return FALSE;
    }
  return TRUE;
}

#ifdef HAVE_CHOWN
static gboolean
set_unix_uid_gid (char *filename,
		  const GFileAttributeValue *uid_value,
		  const GFileAttributeValue *gid_value,
		  GFileGetInfoFlags flags,
		  GError **error)
{
  int res;
  guint32 val;
  uid_t uid;
  gid_t gid;
  
  if (uid_value)
    {
      if (!get_uint32 (uid_value, &val, error))
	return FALSE;
      uid = val;
    }
  else
    uid = -1;
  
  if (gid_value)
    {
      if (!get_uint32 (gid_value, &val, error))
	return FALSE;
      gid = val;
    }
  else
    gid = -1;
  
  if (flags & G_FILE_GET_INFO_NOFOLLOW_SYMLINKS)
    res = lchown (filename, uid, gid);
  else
    res = chown (filename, uid, gid);
  
  if (res == -1)
    {
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error setting owner: %s"),
		   g_strerror (errno));
	  return FALSE;
    }
  return TRUE;
}
#endif

#ifdef HAVE_SYMLINK
static gboolean
set_symlink (char *filename,
	     const GFileAttributeValue *value,
	     GError **error)
{
  const char *val;
  struct stat statbuf;
  
  if (!get_byte_string (value, &val, error))
    return FALSE;
  
  if (val == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   _("symlink must be non-NULL"));
      return FALSE;
    }
  
  if (g_lstat (filename, &statbuf))
    {
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error setting symlink: %s"),
		   g_strerror (errno));
      return FALSE;
    }
  
  if (!S_ISLNK (statbuf.st_mode))
    {
      g_set_error (error, G_IO_ERROR,
		   G_IO_ERROR_NOT_SYMBOLIC_LINK,
		   _("Error setting symlink: file is not a symlink"));
      return FALSE;
    }
  
  if (g_unlink (filename))
    {
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error setting symlink: %s"),
		   g_strerror (errno));
      return FALSE;
    }
  
  if (symlink (filename, val) != 0)
    {
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error setting symlink: %s"),
		   g_strerror (errno));
      return FALSE;
    }
  
  return TRUE;
}
#endif

static int
lazy_stat (char *filename, struct stat *statbuf, gboolean *called_stat)
{
  int res;

  if (*called_stat)
    return 0;
  
  res = g_stat (filename, statbuf);
  
  if (res == 0)
    *called_stat = TRUE;
  
  return res;
}


#ifdef HAVE_UTIMES
static gboolean
set_mtime_atime (char *filename,
		 const GFileAttributeValue *mtime_value,
		 const GFileAttributeValue *mtime_usec_value,
		 const GFileAttributeValue *atime_value,
		 const GFileAttributeValue *atime_usec_value,
		 GError **error)
{
  int res;
  guint64 val;
  guint32 val_usec;
  struct stat statbuf;
  gboolean got_stat = FALSE;
  struct timeval times[2] = { {0, 0}, {0, 0} };

  /* ATIME */
  if (atime_value)
    {
      if (!get_uint64 (atime_value, &val, error))
	return FALSE;
      times[0].tv_sec = val;
    }
  else
    {
      if (lazy_stat (filename, &statbuf, &got_stat) == 0)
	{
	  times[0].tv_sec = statbuf.st_mtime;
#if defined (HAVE_STRUCT_STAT_ST_ATIMENSEC)
	  times[0].tv_usec = statbuf.st_atimensec / 1000;
#elif defined (HAVE_STRUCT_STAT_ST_ATIM_TV_NSEC)
	  times[0].tv_usec = statbuf.st_atim.tv_nsec / 1000;
#endif
	}
    }
  
  if (atime_usec_value)
    {
      if (!get_uint32 (atime_usec_value, &val_usec, error))
	return FALSE;
      times[0].tv_usec = val_usec;
    }

  /* MTIME */
  if (mtime_value)
    {
      if (!get_uint64 (mtime_value, &val, error))
	return FALSE;
      times[1].tv_sec = val;
    }
  else
    {
      if (lazy_stat (filename, &statbuf, &got_stat) == 0)
	{
	  times[1].tv_sec = statbuf.st_mtime;
#if defined (HAVE_STRUCT_STAT_ST_MTIMENSEC)
	  times[1].tv_usec = statbuf.st_mtimensec / 1000;
#elif defined (HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC)
	  times[1].tv_usec = statbuf.st_mtim.tv_nsec / 1000;
#endif
	}
    }
  
  if (mtime_usec_value)
    {
      if (!get_uint32 (mtime_usec_value, &val_usec, error))
	return FALSE;
      times[1].tv_usec = val_usec;
    }
  
  res = utimes(filename, times);
  if (res == -1)
    {
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error setting owner: %s"),
		   g_strerror (errno));
	  return FALSE;
    }
  return TRUE;
}
#endif

gboolean
_g_local_file_info_set_attribute (char *filename,
				  const char *attribute,
				  const GFileAttributeValue *value,
				  GFileGetInfoFlags flags,
				  GCancellable *cancellable,
				  GError **error)
{
  if (strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_MODE) == 0)
    return set_unix_mode (filename, value, error);
  
#ifdef HAVE_CHOWN
  else if (strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_UID) == 0)
    return set_unix_uid_gid (filename, value, NULL, flags, error);
  else if (strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_GID) == 0)
    return set_unix_uid_gid (filename, NULL, value, flags, error);
#endif
  
#ifdef HAVE_SYMLINK
  else if (strcmp (attribute, G_FILE_ATTRIBUTE_STD_SYMLINK_TARGET) == 0)
    return set_symlink (filename, value, error);
#endif

#ifdef HAVE_UTIMES
  else if (strcmp (attribute, G_FILE_ATTRIBUTE_TIME_MODIFIED) == 0)
    return set_mtime_atime (filename, value, NULL, NULL, NULL, error);
  else if (strcmp (attribute, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC) == 0)
    return set_mtime_atime (filename, NULL, value, NULL, NULL, error);
  else if (strcmp (attribute, G_FILE_ATTRIBUTE_TIME_ACCESS) == 0)
    return set_mtime_atime (filename, NULL, NULL, value, NULL, error);
  else if (strcmp (attribute, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC) == 0)
    return set_mtime_atime (filename, NULL, NULL, NULL, value, error);
#endif

  else if (g_str_has_prefix (attribute, "xattr:") == 0)
    return set_xattr (filename, attribute, value, error);
  else if (g_str_has_prefix (attribute, "xattr_sys:") == 0)
    return set_xattr (filename, attribute, value, error);
  
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
	       _("Setting attribute %s not supported"), attribute);
  return FALSE;
}

gboolean
_g_local_file_info_set_attributes  (char                       *filename,
				    GFileInfo                  *info,
				    GFileGetInfoFlags           flags,
				    GCancellable               *cancellable,
				    GError                    **error)
{
  GFileAttributeValue *value, *uid, *gid;
  GFileAttributeValue *mtime, *mtime_usec, *atime, *atime_usec;
  GFileAttributeStatus status;
  gboolean res;
  
  /* Handles setting multiple specified data in a single set, and takes care
     of ordering restrictions when setting attributes */

  res = TRUE;

  /* Set symlink first, since this recreates the file */
#ifdef HAVE_SYMLINK
  value = g_file_info_get_attribute (info, G_FILE_ATTRIBUTE_STD_SYMLINK_TARGET);
  if (value)
    {
      if (!set_symlink (filename, value, error))
	{
	  value->status = G_FILE_ATTRIBUTE_STATUS_ERROR_SETTING;
	  res = FALSE;
	  /* Don't set error multiple times */
	  error = NULL;
	}
      else
	value->status = G_FILE_ATTRIBUTE_STATUS_SET;
	
    }
#endif

#ifdef HAVE_CHOWN
  /* Group uid and gid setting into one call
   * Change ownership before permissions, since ownership changes can
     change permissions (e.g. setuid)
   */
  uid = g_file_info_get_attribute (info, G_FILE_ATTRIBUTE_UNIX_UID);
  gid = g_file_info_get_attribute (info, G_FILE_ATTRIBUTE_UNIX_GID);
  
  if (uid || gid)
    {
      if (!set_unix_uid_gid (filename, uid, gid, flags, error))
	{
	  status = G_FILE_ATTRIBUTE_STATUS_ERROR_SETTING;
	  res = FALSE;
	  /* Don't set error multiple times */
	  error = NULL;
	}
      else
	status = G_FILE_ATTRIBUTE_STATUS_SET;
      if (uid)
	uid->status = status;
      if (gid)
	gid->status = status;
    }
#endif
  
  value = g_file_info_get_attribute (info, G_FILE_ATTRIBUTE_UNIX_MODE);
  if (value)
    {
      if (!set_unix_mode (filename, value, error))
	{
	  value->status = G_FILE_ATTRIBUTE_STATUS_ERROR_SETTING;
	  res = FALSE;
	  /* Don't set error multiple times */
	  error = NULL;
	}
      else
	value->status = G_FILE_ATTRIBUTE_STATUS_SET;
	
    }

#ifdef HAVE_UTIMES
  /* Group all time settings into one call
   * Change times as the last thing to avoid it changing due to metadata changes
   */
  
  mtime = g_file_info_get_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  mtime_usec = g_file_info_get_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
  atime = g_file_info_get_attribute (info, G_FILE_ATTRIBUTE_TIME_ACCESS);
  atime_usec = g_file_info_get_attribute (info, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC);

  if (mtime || mtime_usec || atime || atime_usec)
    {
      if (!set_mtime_atime (filename, mtime, mtime_usec, atime, atime_usec, error))
	{
	  status = G_FILE_ATTRIBUTE_STATUS_ERROR_SETTING;
	  res = FALSE;
	  /* Don't set error multiple times */
	  error = NULL;
	}
      else
	status = G_FILE_ATTRIBUTE_STATUS_SET;
      
      if (mtime)
	mtime->status = status;
      if (mtime_usec)
	mtime_usec->status = status;
      if (atime)
	atime->status = status;
      if (atime_usec)
	atime_usec->status = status;
    }
#endif

  /* xattrs are handled by default callback */

  return res;
}
