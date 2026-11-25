#
# Copyright (C) Dolphin Interconnect Solutions AS, 2025-2026. ALL RIGHTS RESERVED.
# See file LICENSE for terms.
#

sisci_happy="no"

AC_ARG_WITH([sisci],
            [AS_HELP_STRING([--with-sisci=(DIR)],
                            [Enable the pcie transport using SISCI from DIR
                             (default is guess, /opt/DIS).])],
            [], [with_sisci=guess])

AS_IF([test "x$with_sisci" != xno],
      [AS_IF([test "x$with_sisci" = xguess -o "x$with_sisci" = xyes -o "x$with_sisci" = x],
             [sisci_dir=/opt/DIS],
             [sisci_dir=$with_sisci])

       AS_IF([test -d "$sisci_dir/lib64"],
             [sisci_libdir=$sisci_dir/lib64],
             [sisci_libdir=$sisci_dir/lib])

       sisci_cppflags="-I$sisci_dir/include -I$sisci_dir/include/dis"
       sisci_ldflags="-L$sisci_libdir"

       save_CPPFLAGS="$CPPFLAGS"
       save_LDFLAGS="$LDFLAGS"
       save_LIBS="$LIBS"
       CPPFLAGS="$sisci_cppflags $CPPFLAGS"
       LDFLAGS="$sisci_ldflags $LDFLAGS"

       sisci_happy="yes"
       AC_CHECK_HEADER([sisci_api.h], [:], [sisci_happy="no"])

       # sisci_api_macros.h renames the API symbols, so link through the header
       AS_IF([test "x$sisci_happy" = xyes],
             [AC_MSG_CHECKING([for SCIInitialize in -lsisci])
              LIBS="-lsisci $LIBS"
              AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <sisci_api.h>]],
                                              [[sci_error_t error;
                                                SCIInitialize(0, &error);]])],
                             [AC_MSG_RESULT([yes])],
                             [AC_MSG_RESULT([no])
                              sisci_happy="no"])])

       CPPFLAGS="$save_CPPFLAGS"
       LDFLAGS="$save_LDFLAGS"
       LIBS="$save_LIBS"

       AS_IF([test "x$sisci_happy" = xyes],
             [AC_SUBST([SISCI_CPPFLAGS], [$sisci_cppflags])
              AC_SUBST([SISCI_LDFLAGS], [$sisci_ldflags])
              AC_SUBST([SISCI_LIBS], [-lsisci])
              uct_modules="${uct_modules}:pcie"],
             [AS_IF([test "x$with_sisci" != xguess],
                    [AC_MSG_ERROR([SISCI requested but sisci_api.h or libsisci could not be found in $sisci_dir])],
                    [AC_MSG_NOTICE([SISCI not found in $sisci_dir, the pcie transport will not be built])])])
      ])

AM_CONDITIONAL([HAVE_SISCI], [test "x$sisci_happy" = xyes])
AC_CONFIG_FILES([src/uct/pcie/Makefile])
