/usr/lib/vala/gen-introspect --namespace="Vfs" `pkg-config --cflags dbus-glib-1` `pkg-config ` -I. -I../common  gvfsbackend.h gvfsdaemon.h ../common/gmountspec.h | sed 's/GVfsBackend/VfsBackend/g'| sed 's/VfsBackendPrivate[*]/gpointer/g' | sed 's/GMountSpec/MountSpec/g' | sed 's/GVfsDaemon/VfsDaemon/g' > vfsbackend/vfsbackend.gi

vapigen --pkg dbus-glib-1 --pkg gio-2.0 --library vfsbackend vfsbackend/vfsbackend.gi
