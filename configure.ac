# Init and options
AC_INIT([aweather],[0.9.0],[andy753421@gmail.com])
AM_INIT_AUTOMAKE([-Wall -Werror -Wno-portability foreign subdir-objects])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
#AM_SILENT_RULES([yes])

# Check for required programs
AC_PROG_CC
LT_PROG_RC
LT_INIT
DOLT
PKG_PROG_PKG_CONFIG

# Check for required packages
PKG_CHECK_MODULES(GLIB,  glib-2.0)
PKG_CHECK_MODULES(GRITS, grits >= 0.8.1)

# Check for gpsd support
AC_ARG_ENABLE([gps],
	AS_HELP_STRING([--enable-gps], [Build with gpsd support]))
AS_IF([test "$enable_gps" = "yes"],
	[PKG_CHECK_MODULES(GPSD, libgps >= 3.0)])
AM_CONDITIONAL([HAVE_GPSD], test "x$enable_gps" = "xyes")

# Define odd RSL install location
AC_CHECK_LIB(rsl, RSL_wsr88d_to_radar, RSL_LIBS="-lrsl")
AM_CONDITIONAL(HAVE_RSL, test "$RSL_LIBS" != "")
AC_SUBST(RSL_LIBS)

# Test for windowing system
case "${host}" in
	*mingw32*) SYS="WIN" ;;
	*cygwin*)  SYS="WIN" ;;
	*apple*)   SYS="MAC" ;;
	*)         SYS="X11" ;;
esac
AM_CONDITIONAL([SYS_WIN], test "$SYS" = "WIN")
AM_CONDITIONAL([SYS_MAC], test "$SYS" = "MAC")
AM_CONDITIONAL([SYS_X11], test "$SYS" = "X11")

# Check for Mac OX
if test "$SYS" = "MAC"; then
	PKG_CHECK_MODULES(MAC, gtk-mac-integration)
fi

# Check for relative build
AC_ARG_ENABLE(relative, [AS_HELP_STRING([--enable-relative],
	[enable runtime search paths @<:@default=no@:>@])],
	[DOTS=".."; ac_default_prefix="/"])
AC_SUBST([DOTS])

# Output
AC_CONFIG_FILES([
	Makefile
	src/Makefile
	src/resource.rc
	src/plugins/Makefile
	data/Makefile
	data/main.ui
	docs/Makefile
])
AC_OUTPUT

# Print status messages
if test "$RSL_LIBS" = ""; then
	echo " ** Warning, RSL not found, radar plugin disabled"
fi
