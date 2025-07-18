/* Copyright (c) 2009-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "istream.h"
#include "ostream-private.h"
#include "iostream-openssl.h"

struct ssl_ostream {
	struct ostream_private ostream;
	struct ssl_iostream *ssl_io;
	buffer_t *buffer;

	bool shutdown:1;
};

static void
o_stream_ssl_close(struct iostream_private *stream, bool close_parent)
{
	struct ssl_ostream *sstream = (struct ssl_ostream *)stream;

	if (close_parent)
		o_stream_close(sstream->ssl_io->plain_output);
}

static void o_stream_ssl_destroy(struct iostream_private *stream)
{
	struct ssl_ostream *sstream = (struct ssl_ostream *)stream;
	struct istream *ssl_input = sstream->ssl_io->ssl_input;

	openssl_iostream_shutdown(sstream->ssl_io);
	sstream->ssl_io->ssl_output = NULL;
	i_stream_unref(&ssl_input);
	ssl_iostream_unref(&sstream->ssl_io);
	buffer_free(&sstream->buffer);
}

static size_t get_buffer_avail_size(const struct ssl_ostream *sstream)
{
	if (sstream->ostream.max_buffer_size == 0) {
		if (sstream->buffer == NULL)
			return 0;
		/* we're requested to use whatever space is available in
		   the buffer */
		return buffer_get_writable_size(sstream->buffer) - sstream->buffer->used;
	} else {
		size_t buffer_used = (sstream->buffer == NULL ? 0 :
				      sstream->buffer->used);
		return sstream->ostream.max_buffer_size > buffer_used ?
			sstream->ostream.max_buffer_size - buffer_used : 0;
	}
}

static size_t
o_stream_ssl_buffer(struct ssl_ostream *sstream, const struct const_iovec *iov,
		    unsigned int iov_count)
{
	size_t avail, size, bytes_sent = 0;

	if (sstream->buffer == NULL)
		sstream->buffer = buffer_create_dynamic(default_pool,
			I_MIN(IO_BLOCK_SIZE, sstream->ostream.max_buffer_size));

	avail = get_buffer_avail_size(sstream);
	if (avail > 0)
		o_stream_set_flush_pending(sstream->ssl_io->plain_output, TRUE);

	for (unsigned int i = 0; i < iov_count; i++) {
		size = I_MIN(iov[i].iov_len, avail);
		buffer_append(sstream->buffer, iov[i].iov_base, size);
		bytes_sent += size;
		avail -= size;

		if (size != iov[i].iov_len)
			break;
	}

	sstream->ostream.ostream.offset += bytes_sent;
	return bytes_sent;
}

static int o_stream_ssl_flush_buffer(struct ssl_ostream *sstream)
{
	struct ssl_iostream *ssl_io = sstream->ssl_io;
	size_t pos = 0;
	int ret = 1;

	i_assert(!sstream->shutdown);

	while (pos < sstream->buffer->used) {
		/* we're writing plaintext data to OpenSSL, which it encrypts
		   and writes to bio_int's buffer. ssl_iostream_bio_sync()
		   reads it from there and adds to plain_output stream. */
		ret = SSL_write(ssl_io->ssl,
				CONST_PTR_OFFSET(sstream->buffer->data, pos),
				sstream->buffer->used - pos);
		if (ret <= 0) {
			ret = openssl_iostream_handle_error(
				ssl_io, ret, OPENSSL_IOSTREAM_SYNC_TYPE_WRITE,
				"SSL_write");
			if (ret < 0) {
				io_stream_set_error(
					&sstream->ostream.iostream,
					"%s", ssl_io->last_error);
				sstream->ostream.ostream.stream_errno = errno;
				break;
			}
			if (ret == 0)
				break;
		} else {
			pos += ret;
			ret = openssl_iostream_bio_sync(
				ssl_io, OPENSSL_IOSTREAM_SYNC_TYPE_WRITE);
			if (ret < 0) {
				i_assert(ssl_io->plain_stream_errstr != NULL &&
					 ssl_io->plain_stream_errno != 0);
				io_stream_set_error(
					&sstream->ostream.iostream,
					"%s", ssl_io->plain_stream_errstr);
				sstream->ostream.ostream.stream_errno =
					ssl_io->plain_stream_errno;
				break;
			}
		}
	}
	buffer_delete(sstream->buffer, 0, pos);
	return ret <= 0 ? ret : 1;
}

static int o_stream_ssl_flush(struct ostream_private *stream)
{
	struct ssl_ostream *sstream = (struct ssl_ostream *)stream;
	struct ssl_iostream *ssl_io = sstream->ssl_io;
	struct ostream *plain_output = ssl_io->plain_output;
	int ret = 1;

	if (!ssl_io->handshaked) {
		if ((ret = ssl_iostream_handshake(ssl_io)) < 0) {
			/* handshake failed */
			i_assert(errno != 0);
			io_stream_set_error(&stream->iostream,
					    "%s", ssl_io->last_error);
			stream->ostream.stream_errno = errno;
			return ret;
		}
	}
	if (ret > 0 &&
	    openssl_iostream_bio_sync(
		ssl_io, OPENSSL_IOSTREAM_SYNC_TYPE_HANDSHAKE) < 0) {
		i_assert(ssl_io->plain_stream_errno != 0 &&
			 ssl_io->plain_stream_errstr != NULL);
		io_stream_set_error(&stream->iostream,
				    "%s", ssl_io->plain_stream_errstr);
		stream->ostream.stream_errno = ssl_io->plain_stream_errno;
		return -1;
	}

	if (ret > 0 && sstream->buffer != NULL && sstream->buffer->used > 0) {
		/* we can try to send some of our buffered data */
		ret = o_stream_ssl_flush_buffer(sstream);
	}

	/* Stream is finished; shutdown the SSL write direction once our buffer
	   is empty. */
	if (stream->finished && !sstream->shutdown && ret >= 0 &&
	    (sstream->buffer == NULL || sstream->buffer->used == 0)) {
		sstream->shutdown = TRUE;
		if (SSL_shutdown(ssl_io->ssl) < 0) {
			io_stream_set_error(
				&sstream->ostream.iostream, "%s",
				t_strdup_printf("SSL_shutdown() failed: %s",
						openssl_iostream_error()));
			sstream->ostream.ostream.stream_errno = EIO;
			ret = -1;
		}
	}

	if (ret == 0 && ssl_io->want_read) {
		/* we need to read more data until we can continue. */
		o_stream_set_flush_pending(plain_output, FALSE);
		ssl_io->ostream_flush_waiting_input = TRUE;
		ret = 1;
	}

	if (ret <= 0)
		return ret;

	/* return 1 only when the output buffer is empty, which is what the
	   caller expects. */
	return o_stream_get_buffer_used_size(plain_output) == 0 ? 1 : 0;
}

static ssize_t
o_stream_ssl_sendv(struct ostream_private *stream,
		   const struct const_iovec *iov, unsigned int iov_count)
{
	struct ssl_ostream *sstream = (struct ssl_ostream *)stream;

	i_assert(!sstream->shutdown);

	size_t bytes_sent = o_stream_ssl_buffer(sstream, iov, iov_count);
	if (sstream->ssl_io->handshaked &&
	    sstream->buffer->used == bytes_sent) {
		/* buffer was empty before calling this. try to write it
		   immediately. */
		if (o_stream_ssl_flush_buffer(sstream) < 0)
			return -1;
	}
	return bytes_sent;
}

static void o_stream_ssl_switch_ioloop_to(struct ostream_private *stream,
					  struct ioloop *ioloop)
{
	struct ssl_ostream *sstream = (struct ssl_ostream *)stream;

	o_stream_switch_ioloop_to(sstream->ssl_io->plain_output, ioloop);
}

static size_t
o_stream_ssl_get_buffer_used_size(const struct ostream_private *stream)
{
	const struct ssl_ostream *sstream = (const struct ssl_ostream *)stream;
	BIO *bio = SSL_get_wbio(sstream->ssl_io->ssl);
	size_t wbuf_avail = BIO_ctrl_get_write_guarantee(bio);
	size_t wbuf_total_size = BIO_get_write_buf_size(bio, 0);
	size_t buffer_used = (sstream->buffer == NULL ? 0 :
			      sstream->buffer->used);
	i_assert(wbuf_avail <= wbuf_total_size);
	return buffer_used + (wbuf_total_size - wbuf_avail) +
		o_stream_get_buffer_used_size(sstream->ssl_io->plain_output);
}

static size_t
o_stream_ssl_get_buffer_avail_size(const struct ostream_private *stream)
{
	const struct ssl_ostream *sstream = (const struct ssl_ostream *)stream;

	return get_buffer_avail_size(sstream);
}

static void
o_stream_ssl_flush_pending(struct ostream_private *_stream, bool set)
{
	struct ssl_ostream *sstream = (struct ssl_ostream *)_stream;

	o_stream_set_flush_pending(sstream->ssl_io->plain_output, set);
}

static void o_stream_ssl_set_max_buffer_size(struct iostream_private *_stream,
					     size_t max_size)
{
	struct ssl_ostream *sstream = (struct ssl_ostream *)_stream;

	sstream->ostream.max_buffer_size = max_size;
	o_stream_set_max_buffer_size(sstream->ssl_io->plain_output, max_size);
}

struct ostream *openssl_o_stream_create_ssl(struct ssl_iostream *ssl_io)
{
	struct ssl_ostream *sstream;

	ssl_io->refcount++;

	/* When ostream is destroyed, it's flushed. With iostream-ssl the
	   flushing requires both istream and ostream to be available. The
	   istream is referenced here to make sure it's not destroyed before
	   the ostream. */
	i_assert(ssl_io->ssl_input != NULL);
	i_stream_ref(ssl_io->ssl_input);

	sstream = i_new(struct ssl_ostream, 1);
	sstream->ssl_io = ssl_io;
	sstream->ostream.max_buffer_size =
		ssl_io->plain_output->real_stream->max_buffer_size;
	sstream->ostream.iostream.close = o_stream_ssl_close;
	sstream->ostream.iostream.destroy = o_stream_ssl_destroy;
	sstream->ostream.sendv = o_stream_ssl_sendv;
	sstream->ostream.flush = o_stream_ssl_flush;
	sstream->ostream.switch_ioloop_to = o_stream_ssl_switch_ioloop_to;

	sstream->ostream.get_buffer_used_size =
		o_stream_ssl_get_buffer_used_size;
	sstream->ostream.get_buffer_avail_size =
		o_stream_ssl_get_buffer_avail_size;
	sstream->ostream.flush_pending = o_stream_ssl_flush_pending;
	sstream->ostream.iostream.set_max_buffer_size =
		o_stream_ssl_set_max_buffer_size;

	o_stream_init_buffering_flush(&sstream->ostream, ssl_io->plain_output);

	return o_stream_create(&sstream->ostream, NULL,
			       o_stream_get_fd(ssl_io->plain_output));
}
