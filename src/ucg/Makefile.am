#
# Copyright (C) Mellanox Technologies Ltd. 2001-2014.  ALL RIGHTS RESERVED.
# Copyright (c) UT-Battelle, LLC. 2017. ALL RIGHTS RESERVED.
# Copyright (c) Huawei Technologies Ltd. 2018.  ALL RIGHTS RESERVED.
# See file LICENSE for terms.
#

lib_LTLIBRARIES     = libucg.la

libucg_la_CFLAGS   = $(BASE_CFLAGS)
libucg_la_LIBS     =
libucg_la_CPPFLAGS = $(BASE_CPPFLAGS)
libucg_la_LDFLAGS  = -ldl -version-info $(SOVERSION)
libucg_la_LIBADD   = ../ucs/libucs.la ../uct/libuct.la ../ucp/libucp.la
libucg_ladir       = $(includedir)/ucg

nobase_dist_libucg_la_HEADERS = \
	api/ucg_def.h \
	api/ucg_version.h \
	api/ucg.h

noinst_HEADERS = \
	core/ops.h \
	core/types.h \
	core/group.h \
	topology/topo.h

libucg_la_SOURCES = \
	core/ops.c \
	core/group.c \
	core/version.c \
	topology/recursive.c \
	topology/neighbor.c \
	topology/tree.c \
	topology/topo.c
