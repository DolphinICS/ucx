DIS_HOME = /opt/DIS
INCLUDE  = -I$(DIS_HOME)/include -I$(DIS_HOME)/include/dis -I $(DIS_HOME)/src/include

module_LTLIBRARIES    = libuct_sci.la

libuct_sci_la_CPPFLAGS = $(BASE_CPPFLAGS) 
libuct_sci_la_CFLAGS   = $(BASE_CFLAGS) $(INCLUDE) -D_REENTRANT
libuct_sci_la_LIBADD   = $(top_builddir)/src/ucs/libucs.la \
                        $(top_builddir)/src/uct/libuct.la
libuct_sci_la_LDFLAGS  = -version-info $(SOVERSION)

noinst_HEADERS = \
	sci_iface.h \
	sci_md.h \
	sci_ep.h 

libuct_sci_la_SOURCES =  \
	sci_iface.c \
	sci_md.c \
	sci_ep.c
