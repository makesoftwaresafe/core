noinst_LTLIBRARIES = libstorage_maildir.la

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

libstorage_maildir_la_SOURCES = \
	maildir-copy.c \
	maildir-filename.c \
	maildir-filename-flags.c \
	maildir-keywords.c \
	maildir-mail.c \
	maildir-save.c \
	maildir-settings.c \
	maildir-storage.c \
	maildir-sync.c \
	maildir-sync-index.c \
	maildir-uidlist.c \
	maildir-util.c

headers = \
	maildir-filename.h \
	maildir-filename-flags.h \
	maildir-keywords.h \
	maildir-storage.h \
	maildir-settings.h \
	maildir-sync.h \
	maildir-uidlist.h

pkginc_libdir=$(pkgincludedir)
pkginc_lib_HEADERS = $(headers)
