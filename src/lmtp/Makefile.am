pkglibexecdir = $(libexecdir)/dovecot

pkglibexec_PROGRAMS = lmtp

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-auth-client \
	-I$(top_srcdir)/src/lib-mail \
	-I$(top_srcdir)/src/lib-smtp \
	-I$(top_srcdir)/src/lib-imap \
	-I$(top_srcdir)/src/lib-index \
	-I$(top_srcdir)/src/lib-master \
	-I$(top_srcdir)/src/lib-lda \
	-I$(top_srcdir)/src/lib-ssl-iostream \
	-I$(top_srcdir)/src/lib-var-expand \
	-I$(top_srcdir)/src/lib-storage \
	-I$(top_srcdir)/src/lib-storage/index \
	-I$(top_srcdir)/src/lib-storage/index/raw \
	-DMODULEDIR=\""$(moduledir)"\" \
	$(BINARY_CFLAGS)

lmtp_LDFLAGS = -export-dynamic \
	$(BINARY_LDFLAGS)

lmtp_LDADD = \
	$(LIBDOVECOT_LDA) \
	$(LIBDOVECOT_STORAGE) \
	$(LIBDOVECOT)
lmtp_DEPENDENCIES = \
	$(LIBDOVECOT_LDA) \
	$(LIBDOVECOT_STORAGE_DEPS) \
	$(LIBDOVECOT_DEPS)

lmtp_SOURCES = \
	main.c \
	lmtp-client.c \
	lmtp-commands.c \
	lmtp-recipient.c \
	lmtp-local.c \
	lmtp-proxy.c \
	lmtp-settings.c

noinst_HEADERS = \
	lmtp-local.h \
	lmtp-proxy.h

headers = \
	lmtp-common.h \
	lmtp-commands.h \
	lmtp-recipient.h \
	lmtp-client.h \
	lmtp-settings.h

pkginc_libdir=$(pkgincludedir)
pkginc_lib_HEADERS = $(headers)
