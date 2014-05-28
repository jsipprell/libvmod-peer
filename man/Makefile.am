# docs for libvmod-peer

dist_man_MANS = vmod_peer.3
MAINTAINERCLEANFILES = $(dist_man_MANS)
EXTRA_DIST = vmod_peer.rst

vmod_peer.3: vmod_peer.rst
if HAVE_RST2MAN
	$(RST2MAN) $< $@
else
	@echo "================================================"
	@echo "You need rst2man install to build vmod_peer docs"
	@echo "================================================"
	@false
endif
