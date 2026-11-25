#
# Copyright (C) Dolphin Interconnect Solutions AS, 2025-2026. ALL RIGHTS RESERVED.
# See file LICENSE for terms.
#

if HAVE_SISCI

module_LTLIBRARIES    = libuct_pcie.la

libuct_pcie_la_CPPFLAGS = $(BASE_CPPFLAGS) $(SISCI_CPPFLAGS)
libuct_pcie_la_CFLAGS   = $(BASE_CFLAGS) -D_REENTRANT
libuct_pcie_la_LIBADD   = $(top_builddir)/src/ucs/libucs.la \
                        $(top_builddir)/src/uct/libuct.la \
                        $(SISCI_LIBS)
libuct_pcie_la_LDFLAGS  = $(SISCI_LDFLAGS) -version-info $(SOVERSION)

noinst_HEADERS = \
	pcie_iface.h \
	pcie_md.h \
	pcie_ep.h \
	pcie_sisci_helper.h

libuct_pcie_la_SOURCES =  \
	pcie_iface.c \
	pcie_md.c \
	pcie_ep.c \
	pcie_sisci_helper.c

include $(top_srcdir)/config/module.am

endif
