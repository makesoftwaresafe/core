noinst_LTLIBRARIES = libstorage_raw.la

AM_CPPFLAGS = \
	-I$(top_srcdir)/src/lib \
	-I$(top_srcdir)/src/lib-settings \
	-I$(top_srcdir)/src/lib-master \
	-I$(top_srcdir)/src/lib-mail \
	-I$(top_srcdir)/src/lib-imap \
	-I$(top_srcdir)/src/lib-index \
	-I$(top_srcdir)/src/lib-storage \
	-I$(top_srcdir)/src/lib-storage/index \
	-I$(top_srcdir)/src/lib-var-expand

libstorage_raw_la_SOURCES = \
	raw-mail.c \
	raw-sync.c \
	raw-storage.c

headers = \
	raw-storage.h \
	raw-sync.h

pkginc_libdir=$(pkgincludedir)
pkginc_lib_HEADERS = $(headers)
