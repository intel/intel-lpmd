SUBDIRS = data

# Programs to build
sbin_PROGRAMS = \
	intel_lpmd \
	intel_lpmd_control

intel_lpmd_CPPFLAGS = \
	$(DBUS_CFLAGS) \
	$(GLIB_CFLAGS) \
	$(LIBDL) \
	$(LIBM) \
	$(LIBNL_CFLAGS) \
	$(SYSTEMD_CFLAGS) \
	$(XML_CFLAGS) \
	$(libnl30_CFLAGS)\
	$(libnlgenl30_CFLAGS) \
	-DGLIB_SUPPORT \
	-DTDCONFDIR=\"$(lpmd_confdir)\" \
	-DTDLOCALEDIR=\"$(datadir)/locale\" \
	-DTDRUNDIR=\"$(lpmd_rundir)\" \
	-I$(top_srcdir)/src

intel_lpmd_CFLAGS = \
	-I$(top_srcdir)/src

intel_lpmd_LDADD = \
	$(DBUS_LIBS) \
	$(GLIB_LIBS) \
	$(LIBNL_LIBS) \
	$(LIBM) \
	$(LIBDL) \
	$(XML_LIBS) \
	$(libnlgenl30_LIBS) \
	$(SYSTEMD_LIBS)

intel_lpmd_control_CFLAGS = \
	$(DBUS_GLIB_CFLAGS)

intel_lpmd_control_LDADD = \
	$(DBUS_GLIB_LIBS)

intel_lpmd_control_SOURCES = \
	tools/intel_lpmd_control.c

BUILT_SOURCES = \
	intel_lpmd_dbus_interface.h \
	lpmd-resource.c

intel_lpmd_SOURCES = \
	src/lpmd_main.c \
	src/lpmd_proc.c \
	src/lpmd_dbus_server.c \
	src/lpmd_config.c \
	src/lpmd_cpu.c \
	src/lpmd_helpers.c \
	src/lpmd_hfi.c \
	src/lpmd_irq.c \
	src/lpmd_socket.c \
	src/lpmd_util.c	\
	lpmd-resource.c

man_MANS = \
	man/intel_lpmd.8 \
	man/intel_lpmd_config.xml.5

intel_lpmd_dbus_interface.h: $(top_srcdir)/src/intel_lpmd_dbus_interface.xml
	$(AM_V_GEN) dbus-binding-tool --prefix=dbus_interface --mode=glib-server --output=$@ $<

lpmd-resource.c: $(top_srcdir)/lpmd-resource.gresource.xml
	$(AM_V_GEN) glib-compile-resources --generate-source lpmd-resource.gresource.xml


CLEANFILES = $(BUILT_SOURCES)
