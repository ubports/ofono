AC_PREREQ(2.60)
AC_INIT(ofono, 1.21)

AM_INIT_AUTOMAKE([foreign subdir-objects color-tests])
AC_CONFIG_HEADERS(config.h)

m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AM_MAINTAINER_MODE

AC_PREFIX_DEFAULT(/usr/local)

PKG_PROG_PKG_CONFIG

COMPILER_FLAGS

AC_LANG_C

AC_C_RESTRICT

AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_CC_PIE
AC_PROG_INSTALL
AC_PROG_MKDIR_P

m4_define([_LT_AC_TAGCONFIG], [])
m4_ifdef([AC_LIBTOOL_TAGS], [AC_LIBTOOL_TAGS([])])

AC_DISABLE_STATIC
AC_PROG_LIBTOOL

AC_ARG_ENABLE(optimization, AC_HELP_STRING([--disable-optimization],
			[disable code optimization through compiler]), [
	if (test "${enableval}" = "no"); then
		CFLAGS="$CFLAGS -O0"
	fi
])

AC_ARG_ENABLE(debug, AC_HELP_STRING([--enable-debug],
			[enable compiling with debugging information]), [
	if (test "${enableval}" = "yes" &&
				test "${ac_cv_prog_cc_g}" = "yes"); then
		CFLAGS="$CFLAGS -g"
                CPPFLAGS="$CPPFLAGS -DDEBUG"
	fi
])

AC_ARG_ENABLE(pie, AC_HELP_STRING([--enable-pie],
			[enable position independent executables flag]), [
	if (test "${enableval}" = "yes" &&
				test "${ac_cv_prog_cc_pie}" = "yes"); then
		CFLAGS="$CFLAGS -fPIE"
		LDFLAGS="$LDFLAGS -pie"
	fi
])

AC_ARG_ENABLE(threads, AC_HELP_STRING([--enable-threads],
		[enable threading support]), [enable_threads=${enableval}])

AC_CHECK_FUNC(signalfd, dummy=yes,
			AC_MSG_ERROR(signalfd support is required))

AC_CHECK_LIB(dl, dlopen, dummy=yes,
			AC_MSG_ERROR(dynamic linking loader is required))

PKG_CHECK_MODULES(GLIB, glib-2.0 >= 2.32, dummy=yes,
				AC_MSG_ERROR(GLib >= 2.32 is required))
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

PKG_CHECK_MODULES(GOBJECT, gobject-2.0, dummy=yes,
				AC_MSG_ERROR(GObject is required))
GLIB_CFLAGS="$GLIB_CFLAGS $GOBJECT_CFLAGS"
GLIB_LIBS="$GLIB_LIBS $GOBJECT_LIBS"

PKG_CHECK_MODULES(GIO, gio-2.0, dummy=yes,
				AC_MSG_ERROR(GIO is required))
GLIB_CFLAGS="$GLIB_CFLAGS $GIO_CFLAGS"
GLIB_LIBS="$GLIB_LIBS $GIO_LIBS"

if (test "${enable_threads}" = "yes"); then
	AC_DEFINE(NEED_THREADS, 1, [Define if threading support is required])
	PKG_CHECK_MODULES(GTHREAD, gthread-2.0 >= 2.16, dummy=yes,
				AC_MSG_ERROR(GThread >= 2.16 is required))
	GLIB_CFLAGS="$GLIB_CFLAGS $GTHREAD_CFLAGS"
	GLIB_LIBS="$GLIB_LIBS $GTHREAD_LIBS"
fi

PKG_CHECK_MODULES(DBUS, dbus-1 >= 1.4, dummy=yes,
				AC_MSG_ERROR(D-Bus >= 1.4 is required))
AC_SUBST(DBUS_CFLAGS)
AC_SUBST(DBUS_LIBS)

AC_ARG_WITH(dbusconfdir, AC_HELP_STRING([--with-dbusconfdir=PATH],
	[path to D-Bus config directory]), [path_dbusconf=${withval}],
		[path_dbusconf="`$PKG_CONFIG --variable=sysconfdir dbus-1`"])
if (test -z "${path_dbusconf}"); then
	DBUS_CONFDIR="${sysconfdir}/dbus-1/system.d"
else
	DBUS_CONFDIR="${path_dbusconf}/dbus-1/system.d"
fi
AC_SUBST(DBUS_CONFDIR)

AC_ARG_WITH(dbusdatadir, AC_HELP_STRING([--with-dbusdatadir=PATH],
	[path to D-Bus data directory]), [path_dbusdata=${withval}],
		[path_dbusdata="`$PKG_CONFIG --variable=datadir dbus-1`"])
if (test -z "${path_dbusdata}"); then
	DBUS_DATADIR="${datadir}/dbus-1/system-services"
else
	DBUS_DATADIR="${path_dbusdata}/dbus-1/system-services"
fi
AC_SUBST(DBUS_DATADIR)

AC_ARG_WITH([systemdunitdir], AC_HELP_STRING([--with-systemdunitdir=DIR],
	[path to systemd service directory]), [path_systemdunit=${withval}],
		[path_systemdunit="`$PKG_CONFIG --variable=systemdsystemunitdir systemd`"])
if (test -n "${path_systemdunit}"); then
	SYSTEMD_UNITDIR="${path_systemdunit}"
	AC_SUBST(SYSTEMD_UNITDIR)
fi
AM_CONDITIONAL(SYSTEMD, test -n "${path_systemdunit}")

AC_ARG_ENABLE(test, AC_HELP_STRING([--enable-test],
		[enable test/example scripts]), [enable_test=${enableval}])
AM_CONDITIONAL(TEST, test "${enable_test}" = "yes")

AC_ARG_ENABLE(tools, AC_HELP_STRING([--enable-tools],
		[enable testing tools]), [enable_tools=${enableval}])
AM_CONDITIONAL(TOOLS, test "${enable_tools}" = "yes")

AC_ARG_ENABLE(dundee, AC_HELP_STRING([--enable-dundee],
		[enable dialup deamon support]), [enable_dundee=${enableval}])
AM_CONDITIONAL(DUNDEE, test "${enable_dundee}" = "yes")

AC_ARG_ENABLE(udev, AC_HELP_STRING([--disable-udev],
			[disable udev modem detection support]),
						[enable_udev=${enableval}])
if (test "${enable_udev}" != "no"); then
	PKG_CHECK_MODULES(UDEV, libudev >= 143, [enable_udev="yes"],
				AC_MSG_ERROR(libudev >= 143 is required))
	UDEV_DATADIR="`$PKG_CONFIG --variable=udevdir udev`"
	if (test -z "${UDEV_DATADIR}"); then
		UDEV_DATADIR="${sysconfdir}/udev/rules.d"
	else
		UDEV_DATADIR="${UDEV_DATADIR}/rules.d"
	fi
	AC_SUBST(UDEV_DATADIR)
fi
AC_SUBST(UDEV_CFLAGS)
AC_SUBST(UDEV_LIBS)
AM_CONDITIONAL(UDEV, test "${enable_udev}" = "yes")

AC_ARG_ENABLE(atmodem, AC_HELP_STRING([--disable-atmodem],
				[disable ETSI AT modem support]),
					[enable_atmodem=${enableval}])
AM_CONDITIONAL(ATMODEM, test "${enable_atmodem}" != "no")

AC_ARG_ENABLE(cdmamodem, AC_HELP_STRING([--disable-cdmamodem],
				[disable CDMA AT modem support]),
					[enable_cdmamodem=${enableval}])
AM_CONDITIONAL(CDMAMODEM, test "${enable_cdmamodem}" != "no")

AC_ARG_ENABLE(phonesim, AC_HELP_STRING([--disable-phonesim],
				[disable Phone simulator support]),
					[enable_phonesim=${enableval}])
AM_CONDITIONAL(PHONESIM, test "${enable_phonesim}" != "no" &&
					test "${enable_atmodem}" != "no")

AC_ARG_ENABLE(isimodem, AC_HELP_STRING([--disable-isimodem],
				[disable PhoNet/ISI modem support]),
					[enable_isimodem=${enableval}])
AM_CONDITIONAL(ISIMODEM, test "${enable_isimodem}" != "no")

AC_ARG_ENABLE(rilmodem, AC_HELP_STRING([--disable-rilmodem],
                                [disable RIL modem support]),
                                        [enable_rilmodem=${enableval}])
AM_CONDITIONAL(RILMODEM, test "${enable_rilmodem}" != "no")

AC_ARG_ENABLE(sailfish-rilmodem, AC_HELP_STRING([--enable-sailfish-rilmodem],
				[enable Sailfish RIL modem]),
				[enable_sailfish_rilmodem=${enableval}],
				[enable_sailfish_rilmodem="no"])
AM_CONDITIONAL(SAILFISH_RILMODEM, test "${enable_sailfish_rilmodem}" != "no")

if (test "${enable_sailfish_rilmodem}" = "yes"); then
	PKG_CHECK_MODULES(GRILIO, libgrilio >= 1.0.25, dummy=yes,
				AC_MSG_ERROR(libgrilio >= 1.0.25 is required))
	PKG_CHECK_MODULES(GLIBUTIL, libglibutil >= 1.0.30, dummy=yes,
				AC_MSG_ERROR(libglibutil >= 1.0.30 is required))
	PKG_CHECK_MODULES(LIBMCE, libmce-glib >= 1.0.5, dummy=yes,
				AC_MSG_ERROR(libmce-glib >= 1.0.5 is required))
	CFLAGS="$CFLAGS $GRILIO_CFLAGS $LIBMCE_CFLAGS"
	LIBS="$LIBS $GRILIO_LIBS $LIBMCE_LIBS"
	enable_sailfish_manager=yes
	need_glibutil=yes
fi

AC_ARG_ENABLE(sailfish-manager,
		AC_HELP_STRING([--enable-sailfish-manager],
			[enable Sailfish OS modem manager plugin]),
					[enable_sailfish_manager=${enableval}])
AM_CONDITIONAL(SAILFISH_MANAGER, test "${enable_sailfish_manager}" = "yes")

if (test "${enable_sailfish_manager}" = "yes"); then
	PKG_CHECK_MODULES(DBUS_GLIB, dbus-glib-1, dummy=yes,
			AC_MSG_ERROR(dbus-glib is required by unit tests))
	AC_SUBST(DBUS_GLIB_CFLAGS)
	AC_SUBST(DBUS_GLIB_LIBS)
fi

AC_ARG_ENABLE(add-remove-context, AC_HELP_STRING([--disable-add-remove-context],
		[don't allow to add or remove connection context over D-Bus]), [
	if (test "${enableval}" = "no"); then
		CFLAGS="$CFLAGS -DDISABLE_ADD_REMOVE_CONTEXT"
	fi
])

AC_ARG_ENABLE(test-coverage,
	AC_HELP_STRING([--enable-test-coverage], [enable test code coverage]),
				[enable_test_coverage=${enableval}],
					[enable_test_coverage="no"])
AM_CONDITIONAL(TEST_COVERAGE, test "${enable_test_coverage}" != "no")

AC_ARG_ENABLE(qmimodem, AC_HELP_STRING([--disable-qmimodem],
				[disable Qualcomm QMI modem support]),
					[enable_qmimodem=${enableval}])
AM_CONDITIONAL(QMIMODEM, test "${enable_qmimodem}" != "no")

AC_ARG_ENABLE(bluetooth, AC_HELP_STRING([--disable-bluetooth],
				[disable BlueZ 4 and BlueZ 5 plugins support]),
					[enable_bluetooth=${enableval}])

AC_ARG_ENABLE(bluez4, AC_HELP_STRING([--enable-bluez4],
				[enable BlueZ 4 plugins support prior to BlueZ 5]),
					[enable_bluez4=${enableval}])

if (test "${enable_bluez4}" = "yes"); then
	PKG_CHECK_MODULES(BLUEZ, bluez >= 4.99 bluez < 5, dummy=yes,
			  AC_MSG_ERROR(Bluetooth library >= 4.99 and < 5 is required))
AC_SUBST(BLUEZ_CFLAGS)
AC_SUBST(BLUEZ_LIBS)
fi

AM_CONDITIONAL(BLUEZ4, test "${enable_bluetooth}" != "no" && test "${enable_bluez4}" = "yes")
AM_CONDITIONAL(BLUETOOTH, test "${enable_bluetooth}" != "no")

AC_ARG_ENABLE(sailfish-bt, AC_HELP_STRING([--enable-sailfish-bt],
			[enable Sailfish OS Bluetooth plugin]),
					[enable_sailfish_bt=${enableval}])
AM_CONDITIONAL(SAILFISH_BT, test "${enable_sailfish_bt}" = "yes")

AC_ARG_ENABLE(sailfish-provision, AC_HELP_STRING([--enable-sailfish-provision],
			[enable Sailfish OS provisioning plugin]),
				[enable_sailfish_provision=${enableval}])
AM_CONDITIONAL(SAILFISH_PROVISION, test "${enable_sailfish_provision=$}" = "yes")

AC_ARG_ENABLE(nettime, AC_HELP_STRING([--disable-nettime],
                                [disable Nettime plugin]),
                                        [enable_nettime=${enableval}])
if (test "${enable_nettime}" != "no"); then
	AC_SEARCH_LIBS([clock_gettime], [rt])
fi
AM_CONDITIONAL(NETTIME, test "${enable_nettime}" != "no")

AC_ARG_WITH([provisiondb], AC_HELP_STRING([--with-provisiondb=FILE],
	[location of provision database]), [path_provisiondb=${withval}])

AC_ARG_ENABLE(provision, AC_HELP_STRING([--disable-provision],
				[disable provisioning support]),
					[enable_provision=${enableval}])
if (test "${enable_provision}" != "no"); then
	if (test -n "${path_provisiondb}"); then
		AC_DEFINE_UNQUOTED(PROVIDER_DATABASE, "${path_provisiondb}",
						[Mobile provider database])
	else
		AC_MSG_CHECKING([for mobile-broadband-provider-info])
		PKG_CHECK_EXISTS(mobile-broadband-provider-info,
			_PKG_CONFIG(PROVIDER_DATABASE, [variable=database],
					[mobile-broadband-provider-info])
			AC_DEFINE_UNQUOTED(PROVIDER_DATABASE,
						"$pkg_cv_PROVIDER_DATABASE",
						[Mobile provider database])
			AC_MSG_RESULT([yes]),
			AC_MSG_ERROR(Mobile broadband provider database is required))
	fi
fi
AM_CONDITIONAL(PROVISION, test "${enable_provision}" != "no")

AC_ARG_ENABLE(upower, AC_HELP_STRING([--disable-upower],
			[disable UPower plugin]),
					[enable_upower=${enableval}])
AM_CONDITIONAL(UPOWER, test "${enable_power}" != "no")

AC_ARG_ENABLE(datafiles, AC_HELP_STRING([--disable-datafiles],
			[do not install configuration and data files]),
					[enable_datafiles=${enableval}])
AM_CONDITIONAL(DATAFILES, test "${enable_datafiles}" != "no")

AC_ARG_ENABLE(sailfish-pushforwarder, AC_HELP_STRING([--enable-sailfish-pushforwarder],
			[enable Sailfish OS push forwarder plugin]),
			[enable_sailfish_pushforwarder=${enableval}],
			[enable_sailfish_pushforwarder="no"])
AM_CONDITIONAL(SAILFISH_PUSHFORWARDER, test "${enable_sailfish_pushforwarder}" != "no")
if (test "${enable_sailfish_pushforwarder}" != "no"); then
	PKG_CHECK_MODULES(GLIBUTIL, libglibutil >= 1.0.15, dummy=yes,
			AC_MSG_ERROR(libglibutil >= 1.0.15 is required))
	PKG_CHECK_MODULES(WSPCODEC, libwspcodec >= 2.0, dummy=yes,
			AC_MSG_ERROR(WSP decoder is required))
	CFLAGS="$CFLAGS $WSPCODEC_CFLAGS"
	LIBS="$LIBS $WSPCODEC_LIBS"
	need_glibutil=yes
fi

AC_ARG_ENABLE(sailfish-debuglog, AC_HELP_STRING([--enable-sailfish-debuglog],
			[enable Sailfish OS debug log plugin]),
			[enable_sailfish_debuglog=${enableval}],
			[enable_sailfish_debuglog="no"])
AM_CONDITIONAL(SAILFISH_DEBUGLOG, test "${enable_sailfish_debuglog}" != "no")
if (test "${enable_sailfish_debuglog}" = "yes"); then
	PKG_CHECK_MODULES(DBUSLOG, libdbuslogserver-dbus, dummy=yes,
				AC_MSG_ERROR(libdbuslogserver-dbus is required))
	CFLAGS="$CFLAGS $DBUSLOG_CFLAGS"
	LIBS="$LIBS $DBUSLOG_LIBS"
fi

if (test "${need_glibutil}" = "yes"); then
	CFLAGS="$CFLAGS $GLIBUTIL_CFLAGS"
	LIBS="$LIBS $GLIBUTIL_LIBS"
fi

if (test "${prefix}" = "NONE"); then
	dnl no prefix and no localstatedir, so default to /var
	if (test "$localstatedir" = '${prefix}/var'); then
		AC_SUBST([localstatedir], ['/var'])
	fi

	prefix="${ac_default_prefix}"
fi

if (test "$localstatedir" = '${prefix}/var'); then
	storagedir="${prefix}/var/lib/ofono"
else
	storagedir="${localstatedir}/lib/ofono"
fi
AC_DEFINE_UNQUOTED(DEFAULT_STORAGEDIR, "${storagedir}",
			[Directory for the storage files])

if (test "$sysconfdir" = '${prefix}/etc'); then
	configdir="${prefix}/etc/ofono"
else
	configdir="${sysconfdir}/ofono"
fi
AC_DEFINE_UNQUOTED(CONFIGDIR, "${configdir}",
			[Directory for the configuration files])

AC_OUTPUT(Makefile include/version.h src/ofono.service ofono.pc \
		dundee/dundee.service)
