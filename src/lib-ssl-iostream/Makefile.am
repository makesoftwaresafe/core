noinst_LTLIBRARIES = libssl_iostream.la

NOPLUGIN_LDFLAGS =

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-test \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-var-expand \
	-DMODULE_DIR=\""$(moduledir)"\" \
	$(SSL_CFLAGS)

module_LTLIBRARIES = libssl_iostream_openssl.la

libssl_iostream_openssl_la_LDFLAGS = -module -avoid-version
libssl_iostream_openssl_la_LIBADD = $(SSL_LIBS)
libssl_iostream_openssl_la_SOURCES = \
	dovecot-openssl-common.c \
	iostream-openssl.c \
	iostream-openssl-common.c \
	iostream-openssl-context.c \
	istream-openssl.c \
	ostream-openssl.c

libssl_iostream_la_SOURCES = \
	iostream-ssl.c \
	iostream-ssl-context-cache.c \
	iostream-ssl-test.c \
	ssl-settings.c

noinst_HEADERS = \
	dovecot-openssl-common.h

headers = \
	iostream-openssl.h \
	iostream-ssl.h \
	iostream-ssl-private.h \
	iostream-ssl-test.h \
	ssl-settings.h

pkginc_libdir=$(pkgincludedir)
pkginc_lib_HEADERS = $(headers)

test_libs = \
	$(module_LTLIBRARIES) \
	$(noinst_LTLIBRARIES) \
	../lib-settings/libsettings.la \
	../lib-dns/libdns.la \
	../lib-var-expand/libvar_expand.la \
	../lib-test/libtest.la \
	../lib/liblib.la

test_iostream_ssl_SOURCES = test-iostream-ssl.c
test_iostream_ssl_LDADD = $(test_libs) $(SSL_LIBS) $(DLLIB)
test_iostream_ssl_DEPENDENCIES = $(test_libs)

test_programs = \
	test-iostream-ssl

noinst_PROGRAMS = $(test_programs)

check-local:
	for bin in $(test_programs); do \
	  if ! $(RUN_TEST) ./$$bin; then exit 1; fi; \
	done
