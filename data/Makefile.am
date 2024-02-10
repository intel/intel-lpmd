if HAVE_SYSTEMD
systemdsystemunit_DATA = \
	intel_lpmd.service

servicedir = $(datadir)/dbus-1/system-services
service_in_files = org.freedesktop.intel_lpmd.service.in
service_DATA = $(service_in_files:.service.in=.service)
endif

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
