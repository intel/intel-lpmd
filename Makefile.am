## Process this file with automake to generate Makefile.in

include $(GLIB_MAKEFILE)

SUBDIRS = . data tools

ACLOCAL_AMFLAGS = -I m4

# Global C Flags
AM_CFLAGS = \
	${GLIB_CFLAGS} \
	$(XML_CFLAGS) \
	-DTDRUNDIR=\"$(lpmd_rundir)\" \
	-DTDCONFDIR=\"$(lpmd_confdir)\" \
	$(UPOWER_CFLAGS) \
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
	-I@top_srcdir@/src/include \
	-I@top_srcdir@/src/wlt_proxy/include \
	-DTDLOCALEDIR=\"$(datadir)/locale\" \
	-DGLIB_SUPPORT

intel_lpmd_includedir = @top_srcdir@
intel_lpmd_LDADD = \
	$(GLIB_LIBS) \
	$(LIBNL_LIBS) \
	$(LIBM) \
	-lm \
	$(LIBDL) \
	$(XML_LIBS) \
	$(UPOWER_LIBS) \
	$(libnlgenl30_LIBS) \
	$(SYSTEMD_LIBS)

BUILT_SOURCES = \
	lpmd-resource.c

intel_lpmd_SOURCES = \
	src/lpmd_main.c \
	src/lpmd_proc.c \
	src/lpmd_dbus_server.c \
	src/lpmd_config.c \
	src/lpmd_cpumask.c \
	src/lpmd_helpers.c \
	src/lpmd_hfi.c \
	src/lpmd_irq.c \
	src/lpmd_cgroup.c \
	src/lpmd_socket.c \
	src/lpmd_util.c \
	src/lpmd_wlt.c \
	src/lpmd_misc.c \
	src/lpmd_uevent.c \
	src/lpmd_state_machine.c \
	src/wlt_proxy/wlt_proxy.c \
	src/wlt_proxy/spike_mgmt.c \
	src/wlt_proxy/state_machine.c \
	src/wlt_proxy/state_util.c \
	src/wlt_proxy/state_manager.c \
	lpmd-resource.c

man8_MANS = man/intel_lpmd.8 man/intel_lpmd_control.8
man5_MANS = man/intel_lpmd_config.xml.5

lpmd-resource.c: $(top_srcdir)/lpmd-resource.gresource.xml
	$(AM_V_GEN) glib-compile-resources --generate-source lpmd-resource.gresource.xml

install-data-hook:
	gzip -f -k "$(DESTDIR)$(mandir)/man8/intel_lpmd.8";
	mandb || true

CLEANFILES = $(BUILT_SOURCES)
