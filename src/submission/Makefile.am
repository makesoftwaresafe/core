pkglibexecdir = $(libexecdir)/dovecot

pkglibexec_PROGRAMS = submission

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-auth \
	-I$(top_srcdir)/src/lib-mail \
	-I$(top_srcdir)/src/lib-ssl-iostream \
	-I$(top_srcdir)/src/lib-imap \
	-I$(top_srcdir)/src/lib-imap-storage \
	-I$(top_srcdir)/src/lib-imap-urlauth \
	-I$(top_srcdir)/src/lib-index \
	-I$(top_srcdir)/src/lib-master \
	-I$(top_srcdir)/src/lib-login \
	-I$(top_srcdir)/src/lib-var-expand \
	-I$(top_srcdir)/src/lib-storage \
	-I$(top_srcdir)/src/lib-storage/index \
	-I$(top_srcdir)/src/lib-storage/index/raw \
	-I$(top_srcdir)/src/lib-smtp

urlauth_libs = \
	$(top_builddir)/src/lib-imap-urlauth/libimap-urlauth.la

submission_LDFLAGS = -export-dynamic

submission_LDADD = \
	$(urlauth_libs) \
	$(LIBDOVECOT_STORAGE) \
	$(LIBDOVECOT) \
	$(MODULE_LIBS)
submission_DEPENDENCIES = \
	$(urlauth_libs) \
	$(LIBDOVECOT_STORAGE_DEPS) \
	$(LIBDOVECOT_DEPS)

submission_SOURCES = \
	main.c \
	submission-backend.c \
	submission-backend-relay.c \
	submission-recipient.c \
	submission-client.c \
	submission-commands.c \
	submission-settings.c

headers = \
	submission-common.h \
	submission-backend.h \
	submission-backend-relay.h \
	submission-commands.h \
	submission-recipient.h \
	submission-client.h \
	submission-settings.h

pkginc_libdir=$(pkgincludedir)
pkginc_lib_HEADERS = $(headers)
