#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([babeltrace2-ruby-provider], [1.0.0], [bvideau@anl.gov])
AC_CONFIG_SRCDIR([src])
AC_CONFIG_HEADERS([src/config.h])

# build artefacts in separate dir
AC_CONFIG_AUX_DIR([m4])
AC_CONFIG_MACRO_DIR([m4])

# automake should fail on any error
AM_INIT_AUTOMAKE([-Wall -Werror foreign 1.12])
AM_PROG_AR
AM_PROG_CC_C_O

# check for libtool
LT_INIT

# check for programs
AC_LANG([C])
AC_PROG_CC
AC_PROG_CC_STDC

# Check Ruby
PKG_CHECK_MODULES([RUBY], [ruby >= 2.3], [HAVE_RB=yes], [HAVE_RB=no])
if test "x$HAVE_RB" != xyes;
then
  AC_PATH_PROG([RUBY], [ruby])
  if test "x$RUBY" != x;
  then
    RUBYHDRDIR=`$RUBY -e 'puts RbConfig::CONFIG[["rubyhdrdir"]]'`
    RUBYARCHHDRDIR=`$RUBY -e 'puts RbConfig::CONFIG[["rubyarchhdrdir"]]'`
    LIBRUBYARG=`$RUBY -e 'puts RbConfig::CONFIG[["LIBRUBYARG"]]'`
    RUBYLIBDIR=`$RUBY -e 'puts RbConfig::CONFIG[[ RbConfig::CONFIG["libdirname"] ]]'`
    RUBY_CFLAGS="-I$RUBYARCHHDRDIR -I$RUBYHDRDIR"
    RUBY_LIBS="-L$RUBYLIBDIR $LIBRUBYARG -lm"
    AC_SUBST([RUBY_CFLAGS])
    AC_SUBST([RUBY_LIBS])
    HAVE_RB=yes
  else
    HAVE_RB=no
  fi
fi

AS_IF([test "x$HAVE_RB" != "xyes"], [
  AC_MSG_FAILURE([dnl
Ruby >= 2.3 is required.
  ])
])

PKG_CHECK_MODULES([BABELTRACE2], [babeltrace2 >= 2.0])

# Check for glib >= 2.28 with gmodule support
AM_PATH_GLIB_2_0([2.28.0], [],
  AC_MSG_ERROR([glib >= 2.28 is required - download it from ftp://ftp.gtk.org/pub/gtk]),
  [gmodule-no-export]
)



AC_CONFIG_FILES([Makefile
                 src/Makefile])
AC_OUTPUT

