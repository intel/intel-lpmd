## Process this file with automake to generate Makefile.in

include $(GLIB_MAKEFILE)

if HAVE_SYSTEMD
systemdsystemunit_DATA = \
	intel_lpmd.service

intel_lpmd.service: intel_lpmd.service.in
	@$(edit) $< >$@

servicedir = $(datadir)/dbus-1/system-services
service_in_files = org.freedesktop.intel_lpmd.service.in
service_DATA = $(service_in_files:.service.in=.service)

$(service_DATA): $(service_in_files) Makefile
	@$(edit) $< >$@
endif

edit = sed \
	-e 's|@bindir[@]|$(bindir)|g' \
	-e 's|@sbindir[@]|$(sbindir)|g' \
	-e 's|@sysconfdir[@]|$(sysconfdir)|g' \
	-e 's|@localstatedir[@]|$(localstatedir)|g'

dbusservicedir = $(DBUS_SYS_DIR)
dbusservice_DATA = org.freedesktop.intel_lpmd.conf

lpmd_configdir = $(lpmd_confdir)
lpmd_config_DATA = \
	  intel_lpmd_config.xml

EXTRA_DIST = \
	intel_lpmd.service.in \
	org.freedesktop.intel_lpmd.service.in \
	$(dbusservice_DATA)

CLEANFILES = intel_lpmd.service org.freedesktop.intel_lpmd.service
