#
# Copyright (C) Mellanox Technologies Ltd. 2001-2014.  ALL RIGHTS RESERVED.
# Copyright (C) The University of Tennessee and the University of Tennessee Research Foundation. 2016. ALL RIGHTS RESERVED.
#
# See file LICENSE for terms.
#

bin_PROGRAMS      = ucx_tune
AM_CPPFLAGS       = \
    $(BASE_CPPFLAGS) \
    -I$(abs_top_srcdir)/src \
    -I$(abs_top_builddir)/src
ucx_tune_SOURCES  = tune_params.c
ucx_tune_LDADD    = \
    $(abs_top_builddir)/src/uct/libuct.la \
    $(abs_top_builddir)/src/ucp/libucp.la \
    $(abs_top_builddir)/src/ucs/libucs.la \
    $(abs_top_builddir)/src/ucm/libucm.la
