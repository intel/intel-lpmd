include $(GLIB_MAKEFILE)

SUBDIRS = . data tools 

ACLOCAL_AMFLAGS =

# Global C Flags
AM_CFLAGS = \
	${DBUS_CFLAGS} \
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
	$(DBUS_LIBS) \
	$(GLIB_LIBS) \
	$(LIBNL_LIBS) \
	$(LIBM) \
        -lm \
        -lcap \
	$(LIBDL) \
	$(XML_LIBS) \
	$(libnlgenl30_LIBS) \
	$(SYSTEMD_LIBS)

BUILT_SOURCES = \
	intel_lpmd_dbus_interface.h	\
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
        src/wlt_proxy/helper.c \
	src/wlt_proxy/wlt_proxy_def.c \
        src/wlt_proxy/spike_mgmt.c \    
        src/wlt_proxy/state_machine.c \
        src/wlt_proxy/util.c \
        src/wlt_proxy/irq.c \	
        src/wlt_proxy/perf_msr.c \
        src/wlt_proxy/cpu_group.c \
	src/weights/power-supply-status.c \
        src/additional/privilege.c \
        #src/visualize/plotting.c \
	#src/wlt_proxy/helper_gaps.c \
	lpmd-resource.c

man8_MANS = man/intel_lpmd.8
man5_MANS = man/intel_lpmd_config.xml.5

intel_lpmd_dbus_interface.h: $(top_srcdir)/src/intel_lpmd_dbus_interface.xml
	$(AM_V_GEN) dbus-binding-tool --prefix=dbus_interface --mode=glib-server --output=$@ $<

lpmd-resource.c: $(top_srcdir)/lpmd-resource.gresource.xml
	$(AM_V_GEN) glib-compile-resources --generate-source lpmd-resource.gresource.xml


CLEANFILES = $(BUILT_SOURCES)
