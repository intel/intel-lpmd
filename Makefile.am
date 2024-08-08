## Process this file with automake to generate Makefile.in

include $(GLIB_MAKEFILE)

SUBDIRS = . data tools

ACLOCAL_AMFLAGS =

# Global C Flags
AM_CFLAGS = \
	${GLIB_CFLAGS} \
	$(XML_CFLAGS) \
	-DTDRUNDIR=\"$(lpmd_rundir)\" \
	-DTDCONFDIR=\"$(lpmd_confdir)\" \
	$(CFLAGS) \
	$(libnl30_CFLAGS)\
	$(libnlgenl30_CFLAGS) \
	$(SYSTEMD_CFLAGS) \
	-I src

EXTRA_DIST=Makefile.glib \
	intel_lpmd.pc.in

# Programs to build
sbin_PROGRAMS = intel_lpmd

intel_lpmd_CPPFLAGS = \
	-I@top_srcdir@/src \
	-DTDLOCALEDIR=\"$(datadir)/locale\" \
	-DGLIB_SUPPORT

intel_lpmd_includedir = @top_srcdir@
intel_lpmd_LDADD = \
	$(GLIB_LIBS) \
	$(LIBNL_LIBS) \
	$(LIBM) \
	$(LIBDL) \
	$(XML_LIBS) \
	$(libnlgenl30_LIBS) \
	$(SYSTEMD_LIBS)

BUILT_SOURCES = \
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

man8_MANS = man/intel_lpmd.8
man5_MANS = man/intel_lpmd_config.xml.5

lpmd-resource.c: $(top_srcdir)/lpmd-resource.gresource.xml
	$(AM_V_GEN) glib-compile-resources --generate-source lpmd-resource.gresource.xml


CLEANFILES = $(BUILT_SOURCES)
