pkglibexecdir = $(libexecdir)/dovecot

pkglibexec_PROGRAMS = dovecot-lda

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-mail \
	-I$(top_srcdir)/src/lib-smtp \
	-I$(top_srcdir)/src/lib-imap \
	-I$(top_srcdir)/src/lib-index \
	-I$(top_srcdir)/src/lib-master \
	-I$(top_srcdir)/src/lib-smtp \
	-I$(top_srcdir)/src/lib-lda \
	-I$(top_srcdir)/src/lib-var-expand \
	-I$(top_srcdir)/src/lib-storage \
	-I$(top_srcdir)/src/lib-storage/index \
	-I$(top_srcdir)/src/lib-storage/index/raw \
	$(BINARY_CFLAGS)

dovecot_lda_LDFLAGS = -export-dynamic

dovecot_lda_LDADD = \
	$(LIBDOVECOT_LDA) \
	$(LIBDOVECOT_STORAGE) \
	$(LIBDOVECOT) \
	$(BINARY_LDFLAGS)

dovecot_lda_DEPENDENCIES = \
	$(LIBDOVECOT_LDA) \
	$(LIBDOVECOT_STORAGE_DEPS) \
	$(LIBDOVECOT_DEPS)

dovecot_lda_SOURCES = \
	main.c

install-exec-local:
	rm -f $(DESTDIR)$(pkglibexecdir)/deliver
	$(LN_S) dovecot-lda $(DESTDIR)$(pkglibexecdir)/deliver
