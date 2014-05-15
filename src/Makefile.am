AM_CPPFLAGS = -I$(VARNISHSRC)/include -I$(VARNISHSRC)

vmoddir = $(VMODDIR)
vmod_LTLIBRARIES = libvmod_peer.la

libvmod_peer_la_CFLAGS = @CURL_CFLAGS@
libvmod_peer_la_LDFLAGS = -module -export-dynamic -avoid-version
libvmod_peer_la_LIBADD = @CURL_LIBS@
libvmod_peer_la_SOURCES = \
		vcc_if.c \
		vcc_if.h \
		vmod_peer.h \
		vmod_peer.c

vcc_if.c vcc_if.h: $(top_builddir)/build/vmod.py $(srcdir)/vmod_peer.vcc
	@PYTHON@ $(top_builddir)/build/vmod.py $(srcdir)/vmod_peer.vcc

EXTRA_DIST = vmod_peer.vcc

CLEANFILES = $(builddir)/vcc_if.c $(builddir)/vcc_if.h
