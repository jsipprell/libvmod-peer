AM_CPPFLAGS = @VMOD_INCLUDES@

dist_man_MANS = vmod_peer.3
vmoddir = @VMOD_DIR@
vmod_LTLIBRARIES = libvmod_peer.la
EXTRA_DIST = vmod_peer.vcc
MAINTAINERCLEANFILES = $(dist_man_MANS)
CLEANFILES = $(builddir)/vcc_if.c $(builddir)/vcc_if.h $(builddir)/vmod_peer.rst $(builddir)/vmod_peer.man.rst

libvmod_peer_la_CFLAGS = @CURL_CFLAGS@
libvmod_peer_la_LDFLAGS = -module -export-dynamic -avoid-version
libvmod_peer_la_LIBADD = @CURL_LIBS@
libvmod_peer_la_SOURCES = \
		vcc_if.c \
		vcc_if.h \
		locks.c \
		vmod_peer.h vct.h \
		vmod_peer.c

vcc_if.c vcc_if.h vmod_peer.man.rst: $(srcdir)/vmod_peer.vcc
	@PYTHON@ @VMODTOOL@ $(srcdir)/vmod_peer.vcc

vmod_peer.3: vmod_peer.man.rst
if HAVE_RST2MAN
	$(RST2MAN) $< $@
else
	@echo "=================================================="
	@echo "You need rst2man installed to build vmod_peer docs"
	@echo "=================================================="
	@false
endif
