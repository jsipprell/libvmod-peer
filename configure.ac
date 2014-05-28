AC_PREREQ([2.63])
AC_COPYRIGHT([Copyright (c) 2014 Jesse Sipprell
All Rights Reserved])
builtin(include, m4/compat.m4)dnl
AC_INIT([libvmod-peer], [1.0])
AC_CONFIG_MACRO_DIR(m4)
AC_CONFIG_AUX_DIR(build)
AC_CONFIG_SRCDIR([src/vmod_peer.vcc])
AM_CONFIG_HEADER([config.h])

AC_CANONICAL_TARGET
AC_LANG(C)

AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_SILENT_RULES([yes])
AC_GNU_SOURCE

dnl Pthreads (necessary to test for rwlock capability)
AX_PTHREAD
AS_VAR_IF([ax_pthread_ok],[yes],[],
  [PTHREAD_CC=gcc])
AC_PROG_CC([$PTHREAD_CC])
LT_INIT([disable-static])

AC_PROG_INSTALL
AC_PROG_LIBTOOL
AC_PROG_MAKE_SET
AC_CHECK_TARGET_TOOL([DIRNAME],[dirname],[])

AC_ARG_ENABLE([debug],
  [AS_HELP_STRING([--enable-debug],
                  [turn on full symbol generation, disable optimization and out debug messages])],
  [CFLAGS="-ggdb3 -Wall"
   CPPFLAGS="$CPPFLAGS${CPPFLAGS:+ }-DDEBUG"])
AC_ARG_ENABLE([devel],
  [AC_HELP_STRING([--enable-devel],
                 [enable all warnings, pedantic mode and full debug symbol generation])],
  [CFLAGS="-ggdb3 -Wall -pedantic"
  CPPFLAGS="$CPPFLAGS${CPPFLAGS:+ }-DDEBUG"])

AS_VAR_IF([ax_pthread_ok],[yes],[
  CFLAGS="$CFLAGS${CFLAGS+ }$PTHREAD_CFLAGS"
  LIBS="$PTHREAD_LIBS${PTHREAD_LIBS+ }$LIBS"])

# Check for rst utilities
AC_CHECK_PROGS(RST2MAN, [rst2man rst2man.py], "no")
AS_VAR_IF([RST2MAN],[no],
  [AC_MSG_WARN([rst2man not found - not building man pages])])
AM_CONDITIONAL(HAVE_RST2MAN, [test x"$RST2MAN" != x"no"])

# Check for pkg-config
PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES([CURL],[libcurl])

# Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS([sys/stdlib.h])

# Check for python
AC_CHECK_PROGS(PYTHON, [python3 python3.1 python3.2 python2.7 python2.6 python2.5 python2 python],
  [AC_MSG_ERROR([Python is needed to build this vmod, please install python.])])

# Varnish includes & vmod dir
AC_ARG_WITH(varnish,
  [AS_HELP_STRING([--with-varnish@<:@=DIR@:>@],[Location of Varnish 4 pkg-config (.pc) file])],
  [AS_IF([test -f "$with_varnish"],
    [AS_VAR_IF([DIRNAME],[],[],[with_varnish="`$DIRNAME \"$with_varnish\"`"])])
   AS_IF([test -d "$with_varnish"],,[with_varnish=no])])

AS_CASE($with_varnish,
  [yes],[],
  [no],[],
  [''],[],
  [PKG_CONFIG_PATH="${with_varnish}${PKG_CONFIG_PATH+$PATH_SEPARATOR}${PKG_CONFIG_PATH}"
   export PKG_CONFIG_PATH])
PKG_CHECK_EXISTS([varnishapi >= 4.0],,
  [AC_MSG_ERROR([Varnish 4.0 or better is needed to build this vmod.])])
VARNISH_VMOD_INCLUDES
VARNISH_VMOD_DIR
VARNISH_VMODTOOL

# check if curl supports ms timeout settings
save_CFLAGS="${CFLAGS}"
save_LIBS="${LIBS}"
CFLAGS="${CFLAGS} ${CURL_CFLAGS}"
LIBS="${LIBS} ${CURL_LIBS}"
AC_CACHE_CHECK([for curl ms timeout settings],
  [ac_cv_curl_ms_timeout],
  [AC_RUN_IFELSE(
    [AC_LANG_PROGRAM([[
#include <stdio.h>
#include <curl/curl.h>
    ]],[[
CURL *curl_handle;
curl_handle = curl_easy_init();
curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT_MS, 10);
curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT_MS, 10);
return (0);
    ]])],
    [ac_cv_curl_ms_timeout=yes],
    [ac_cv_curl_ms_timeout=no])
  ])
if test "$ac_cv_curl_ms_timeout" = yes; then
        AC_DEFINE([CURL_TIMEOUTMS_WORKS], [1], [Define if curl supports ms timeout])
fi
CFLAGS="${save_CFLAGS}"
LIBS="${save_LIBS}"

# pthread read/write locks
AC_CHECK_FUNC(pthread_rwlock_init,
  [AC_DEFINE([HAVE_PTHREAD_RWLOCKS], [1],
    [Define if your POSIX threads library supports read/write locks])])
AC_CHECK_FUNCS([pthread_rwlockattr_setpshared])

AC_OUTPUT([Makefile src/Makefile])
