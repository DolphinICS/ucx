DIS_HOME = /opt/DIS
INCLUDE  = -I$(DIS_HOME)/include -I$(DIS_HOME)/include/dis -I $(DIS_HOME)/src/include

module_LTLIBRARIES    = libuct_pcie.la

libuct_pcie_la_CPPFLAGS = $(BASE_CPPFLAGS) 
libuct_pcie_la_CFLAGS   = $(BASE_CFLAGS) $(INCLUDE) -D_REENTRANT
libuct_pcie_la_LIBADD   = $(top_builddir)/src/ucs/libucs.la \
                        $(top_builddir)/src/uct/libuct.la \
                        -L$(DIS_HOME)/lib64 -lsisci
libuct_pcie_la_LDFLAGS  = -version-info $(SOVERSION)

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
