AC_PREREQ(1.0)

m4_define([lpmd_major_version], [0])
m4_define([lpmd_minor_version], [0.9])
m4_define([lpmd_version],
          [lpmd_major_version.lpmd_minor_version])

AC_INIT([intel_lpmd], [lpmd_version], [], [intel_lpmd])
m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR(build-aux)
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([1.11 foreign no-define subdir-objects])
AM_MAINTAINER_MODE([enable])

GTK_DOC_CHECK([1.11],[--flavour no-tmpl])

AC_ARG_WITH(dbus-sys-dir, AS_HELP_STRING([--with-dbus-sys-dir=DIR], [where D-BUS system.d directory is]))
if test -n "$with_dbus_sys_dir" ; then
    DBUS_SYS_DIR="$with_dbus_sys_dir"
else
    DBUS_SYS_DIR="/etc/dbus-1/system.d"
fi
AC_SUBST(DBUS_SYS_DIR)

# paths
AC_SUBST(lpmd_binary, "$sbindir/$PACKAGE", [Binary executable])
AC_SUBST(lpmd_confdir, "$sysconfdir/$PACKAGE", [Configuration directory])
AC_SUBST(lpmd_rundir, "$localstatedir/run/$PACKAGE", [Runtime state directory])

PKG_PROG_PKG_CONFIG
AC_ARG_WITH([systemdsystemunitdir],
        AS_HELP_STRING([--with-systemdsystemunitdir=DIR], [Directory for systemd service files]),
        [], [with_systemdsystemunitdir=$($PKG_CONFIG --variable=systemdsystemunitdir systemd)])
if test "x$with_systemdsystemunitdir" != xno; then
        AC_SUBST([systemdsystemunitdir], [$with_systemdsystemunitdir])
fi

AM_CONDITIONAL(HAVE_SYSTEMD, [test -n "$with_systemdsystemunitdir" -a "x$with_systemdsystemunitdir" != xno ])

# print configuration
echo
echo "System paths:"
echo "  prefix: $prefix"
echo "  exec_prefix: $exec_prefix"
echo "  systemdunitdir: $with_systemdsystemunitdir"
echo "  lpmd_binary: $lpmd_binary"
echo "  lpmd_confdir: $lpmd_confdir"
echo "  lpmd_rundir: $lpmd_rundir"
echo

GETTEXT_PACKAGE=intel_lpmd
AC_SUBST(GETTEXT_PACKAGE)
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE,"$GETTEXT_PACKAGE", [Gettext package])

GLIB_VERSION_DEFINES="-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_26"

PKG_CHECK_MODULES(GLIB, gio-unix-2.0 >= 2.22 gmodule-2.0 glib-2.0 gobject-2.0)
GLIB_CFLAGS="$GLIB_CFLAGS $GLIB_VERSION_DEFINES"
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

PKG_CHECK_MODULES(XML, libxml-2.0 >= 2.4)

PKG_CHECK_MODULES(libnl30, [libnl-3.0], libnl30=yes, libnl30=no)
PKG_CHECK_MODULES(libnlgenl30, [libnl-genl-3.0], libnlgenl30=yes, libnlgenl30=no)

PKG_CHECK_MODULES([SYSTEMD], [libsystemd], [],
      [PKG_CHECK_MODULES([SYSTEMD], [libsystemd-daemon], [],
       AC_MSG_ERROR([libsystemd support requested but found]))])

PKG_CHECK_MODULES(UPOWER, upower-glib)

AC_PATH_PROG([GDBUS_CODEGEN],[gdbus-codegen])

AC_PROG_CC
AC_PROG_INSTALL
AC_C_CONST
AC_C_INLINE
AC_TYPE_SIZE_T

AC_ARG_ENABLE(werror, AS_HELP_STRING([--disable-werror], [Disable -Werror]))
AS_IF([test "x$enable_werror" != "xno"], [CFLAGS="$CFLAGS -Werror"])

AC_CONFIG_FILES([Makefile
                 data/Makefile
                 tools/Makefile])

AC_ARG_ENABLE(gdbus,
              [AS_HELP_STRING([--disable-gdbus],
                              [Switch DBus backend to glib-dbus. (Default: GDBus)])],
              [],
              [AC_DEFINE([GDBUS], [1], [Enable GDBus support])])

AC_OUTPUT
