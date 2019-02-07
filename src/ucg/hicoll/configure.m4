#
# Copyright (c) Huawei Technologies Ltd. 2018.  ALL RIGHTS RESERVED.
# See file LICENSE for terms.
#

hicoll_happy="no"

AC_ARG_WITH([hicoll],
            [AS_HELP_STRING([--with-hicoll=(DIR)], [Enable the use of Huawei collectives (default is guess).])],
            [], [with_hicoll=guess])

AS_IF([test "x$with_hicoll" != "xno"],
    [save_CPPFLAGS="$CPPFLAGS"
     save_CFLAGS="$CFLAGS"
     save_LDFLAGS="$LDFLAGS"

     AS_IF([test ! -z "$with_hicoll" -a "x$with_hicoll" != "xyes" -a "x$with_hicoll" != "xguess"],
            [
            ucx_check_hicoll_dir="$with_hicoll"
            AS_IF([test -d "$with_hicoll/lib64"],[libsuff="64"],[libsuff=""])
            ucx_check_hicoll_libdir="$with_hicoll/lib$libsuff"
            CPPFLAGS="-I$with_hicoll/include $save_CPPFLAGS"
            LDFLAGS="-L$ucx_check_hicoll_libdir $save_LDFLAGS"
            ])

        AC_CHECK_HEADERS([hicoll.h],
            [AC_CHECK_LIB([hicoll] , [hicoll_init],
                           [hicoll_happy="yes"],
                           [AC_MSG_WARN([Huawei collectives library not detected. Disable.])
                            hicoll_happy="no"])
            ], [hicoll_happy="no"])

        CFLAGS="$save_CFLAGS"
        CPPFLAGS="$save_CPPFLAGS"
        LDFLAGS="$save_LDFLAGS"

        AS_IF([test "x$hicoll_happy" == "xyes"],
            [
                AC_SUBST(HICOLL_CPPFLAGS, "-I$ucx_check_hicoll_dir/include/ ")
                AC_SUBST(HICOLL_LDFLAGS, "-lhicoll -L$ucx_check_hicoll_dir/lib64")
                ucg_modules+=":hicoll"
            ],
            [
                AS_IF([test "x$with_hicoll" != "xguess"],
                    [AC_MSG_ERROR([Huawei collectives support is requested but hicoll packages can't found])],
                    [AC_MSG_WARN([Huawei collectives were not found])])
            ])
    ],
    [AC_MSG_WARN([Huawei collectives were explicitly disabled])])

AM_CONDITIONAL([HAVE_HICOLL], [test "x$hicoll_happy" != xno])

AC_CONFIG_FILES([src/ucg/hicoll/Makefile])