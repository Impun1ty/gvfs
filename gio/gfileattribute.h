#ifndef __G_FILE_ATTRIBUTE_H__
#define __G_FILE_ATTRIBUTE_H__

#include <glib-object.h>
#include <gio/giotypes.h>

G_BEGIN_DECLS

typedef enum {
  G_FILE_ATTRIBUTE_TYPE_INVALID = 0,
  G_FILE_ATTRIBUTE_TYPE_STRING,
  G_FILE_ATTRIBUTE_TYPE_BYTE_STRING, /* zero terminated string of non-zero bytes */
  G_FILE_ATTRIBUTE_TYPE_BOOLEAN,
  G_FILE_ATTRIBUTE_TYPE_UINT32,
  G_FILE_ATTRIBUTE_TYPE_INT32,
  G_FILE_ATTRIBUTE_TYPE_UINT64,
  G_FILE_ATTRIBUTE_TYPE_INT64,
  G_FILE_ATTRIBUTE_TYPE_OBJECT
} GFileAttributeType;

typedef enum {
  G_FILE_ATTRIBUTE_FLAGS_NONE = 0,
  G_FILE_ATTRIBUTE_FLAGS_COPY_WITH_FILE = 1 << 0,
  G_FILE_ATTRIBUTE_FLAGS_COPY_WHEN_MOVED = 1 << 1,
} GFileAttributeFlags;

/* Used by g_file_set_attributes_from_info */
typedef enum {
  G_FILE_ATTRIBUTE_STATUS_UNSET = 0,
  G_FILE_ATTRIBUTE_STATUS_SET,
  G_FILE_ATTRIBUTE_STATUS_ERROR_SETTING,
} GFileAttributeStatus;

#define G_FILE_ATTRIBUTE_VALUE_INIT {0}

typedef struct  {
  GFileAttributeType type : 8;
  GFileAttributeStatus status : 8;
  union {
    gboolean boolean;
    gint32 int32;
    guint32 uint32;
    gint64 int64;
    guint64 uint64;
    char *string;
    GQuark quark;
    GObject *obj;
  } u;
} GFileAttributeValue;

typedef struct {
  char *name;
  GFileAttributeType type;
  GFileAttributeFlags flags;
} GFileAttributeInfo;

typedef struct {
  GFileAttributeInfo *infos;
  int n_infos;
} GFileAttributeInfoList;

GFileAttributeValue *g_file_attribute_value_new             (void);
void                 g_file_attribute_value_free            (GFileAttributeValue *attr);
void                 g_file_attribute_value_clear           (GFileAttributeValue *attr);
void                 g_file_attribute_value_set             (GFileAttributeValue *attr,
							     const GFileAttributeValue *new_value);
GFileAttributeValue *g_file_attribute_value_dup             (const GFileAttributeValue *attr);

char *               g_file_attribute_value_as_string       (const GFileAttributeValue *attr);

const char *         g_file_attribute_value_get_string      (const GFileAttributeValue *attr);
const char *         g_file_attribute_value_get_byte_string (const GFileAttributeValue *attr);
gboolean             g_file_attribute_value_get_boolean     (const GFileAttributeValue *attr);
guint32              g_file_attribute_value_get_uint32      (const GFileAttributeValue *attr);
gint32               g_file_attribute_value_get_int32       (const GFileAttributeValue *attr);
guint64              g_file_attribute_value_get_uint64      (const GFileAttributeValue *attr);
gint64               g_file_attribute_value_get_int64       (const GFileAttributeValue *attr);
GObject *            g_file_attribute_value_get_object      (const GFileAttributeValue *attr);

void                 g_file_attribute_value_set_string      (GFileAttributeValue *attr,
							     const char          *value);
void                 g_file_attribute_value_set_byte_string (GFileAttributeValue *attr,
							     const char          *value);
void                 g_file_attribute_value_set_boolean     (GFileAttributeValue *attr,
							     gboolean             value);
void                 g_file_attribute_value_set_uint32      (GFileAttributeValue *attr,
							     guint32              value);
void                 g_file_attribute_value_set_int32       (GFileAttributeValue *attr,
							     gint32               value);
void                 g_file_attribute_value_set_uint64      (GFileAttributeValue *attr,
							     guint64              value);
void                 g_file_attribute_value_set_int64       (GFileAttributeValue *attr,
							     gint64               value);
void                 g_file_attribute_value_set_object      (GFileAttributeValue *attr,
							     GObject             *obj);

GFileAttributeInfoList *  g_file_attribute_info_list_new    (void);
GFileAttributeInfoList *  g_file_attribute_info_list_ref    (GFileAttributeInfoList *list);
void                      g_file_attribute_info_list_unref  (GFileAttributeInfoList *list);
GFileAttributeInfoList *  g_file_attribute_info_list_dup    (GFileAttributeInfoList *list);
const GFileAttributeInfo *g_file_attribute_info_list_lookup (GFileAttributeInfoList *list,
							     const char             *name);
void                      g_file_attribute_info_list_add    (GFileAttributeInfoList *list,
							     const char             *name,
							     GFileAttributeType      type,
							     GFileAttributeFlags     flags);

G_END_DECLS


#endif /* __G_FILE_INFO_H__ */
