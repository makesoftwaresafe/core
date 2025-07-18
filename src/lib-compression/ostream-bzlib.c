/* Copyright (c) 2010-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"

#ifdef HAVE_BZLIB

#include "ostream-private.h"
#include "settings.h"
#include "ostream-zlib.h"
#include <bzlib.h>

#define CHUNK_SIZE (1024*64)

struct bzlib_ostream {
	struct ostream_private ostream;
	bz_stream zs;

	char outbuf[CHUNK_SIZE];
	unsigned int outbuf_offset, outbuf_used;

	bool flushed:1;
};

struct bzlib_settings {
	pool_t pool;
	unsigned int compress_bz2_block_size_100k;
};

static bool bzlib_settings_check(void *_set, pool_t pool, const char **error_r);

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct bzlib_settings)
static const struct setting_define bzlib_setting_defines[] = {
	DEF(UINT, compress_bz2_block_size_100k),

	SETTING_DEFINE_LIST_END
};
static const struct bzlib_settings bzlib_default_settings = {
	.compress_bz2_block_size_100k = 9,
};

const struct setting_parser_info bzlib_setting_parser_info = {
	.name = "bzlib",

	.defines = bzlib_setting_defines,
	.defaults = &bzlib_default_settings,

	.struct_size = sizeof(struct bzlib_settings),
	.pool_offset1 = 1 + offsetof(struct bzlib_settings, pool),
#ifndef CONFIG_BINARY
	.check_func = bzlib_settings_check,
#endif
};

static bool bzlib_settings_check(void *_set, pool_t pool ATTR_UNUSED,
				 const char **error_r)
{
	struct bzlib_settings *set = _set;

	if (set->compress_bz2_block_size_100k < 1 ||
	    set->compress_bz2_block_size_100k > 9) {
		*error_r = "compress_bz2_block_size_100k must be between 1..9";
		return FALSE;
	}
	return TRUE;
}

static void o_stream_bzlib_close(struct iostream_private *stream,
				 bool close_parent)
{
	struct bzlib_ostream *zstream = (struct bzlib_ostream *)stream;

	(void)BZ2_bzCompressEnd(&zstream->zs);
	if (close_parent)
		o_stream_close(zstream->ostream.parent);
}

static int o_stream_zlib_send_outbuf(struct bzlib_ostream *zstream)
{
	ssize_t ret;
	size_t size;

	if (zstream->outbuf_used == 0)
		return 1;

	size = zstream->outbuf_used - zstream->outbuf_offset;
	i_assert(size > 0);
	ret = o_stream_send(zstream->ostream.parent,
			    zstream->outbuf + zstream->outbuf_offset, size);
	if (ret < 0) {
		o_stream_copy_error_from_parent(&zstream->ostream);
		return -1;
	}
	if ((size_t)ret != size) {
		zstream->outbuf_offset += ret;
		/* We couldn't send everything to parent stream, but we
		   accepted all the input already. Set the ostream's flush
		   pending so when there's more space in the parent stream
		   we'll continue sending the rest of the data. */
		o_stream_set_flush_pending(&zstream->ostream.ostream, TRUE);
		return 0;
	}
	zstream->outbuf_offset = 0;
	zstream->outbuf_used = 0;
	return 1;
}

static ssize_t
o_stream_bzlib_send_chunk(struct bzlib_ostream *zstream,
			  const void *data, size_t size)
{
	bz_stream *zs = &zstream->zs;
	int ret;

	i_assert(zstream->outbuf_used == 0);

	zs->next_in = (void *)data;
	zs->avail_in = size;
	while (zs->avail_in > 0) {
		if (zs->avail_out == 0) {
			/* previous block was compressed. send it and start
			   compression for a new block. */
			zs->next_out = zstream->outbuf;
			zs->avail_out = sizeof(zstream->outbuf);

			zstream->outbuf_used = sizeof(zstream->outbuf);
			if ((ret = o_stream_zlib_send_outbuf(zstream)) < 0)
				return -1;
			if (ret == 0) {
				/* parent stream's buffer full */
				break;
			}
		}

		switch ((ret = BZ2_bzCompress(zs, BZ_RUN))) {
		case BZ_RUN_OK:
			break;
		case BZ_MEM_ERROR:
			i_fatal_status(FATAL_OUTOFMEM, "bzip2.write(%s): Out of memory",
				       o_stream_get_name(&zstream->ostream.ostream));
		default:
			i_fatal("BZ2_bzCompress() failed with %d", ret);
		}
	}
	size -= zs->avail_in;

	zstream->flushed = FALSE;
	return size;
}

static int o_stream_bzlib_send_flush(struct bzlib_ostream *zstream, bool final)
{
	bz_stream *zs = &zstream->zs;
	size_t len;
	bool done = FALSE;
	int ret;

	i_assert(zs->avail_in == 0);

	if (zstream->flushed) {
		i_assert(zstream->outbuf_used == 0);
		return 1;
	}

	if ((ret = o_stream_flush_parent_if_needed(&zstream->ostream)) <= 0)
		return ret;
	if ((ret = o_stream_zlib_send_outbuf(zstream)) <= 0)
		return ret;

	/* do not attempt to finish the stream early */
	if (!final)
		return 1;

	i_assert(zstream->outbuf_used == 0);
	do {
		len = sizeof(zstream->outbuf) - zs->avail_out;
		if (len != 0) {
			zs->next_out = zstream->outbuf;
			zs->avail_out = sizeof(zstream->outbuf);

			zstream->outbuf_used = len;
			if ((ret = o_stream_zlib_send_outbuf(zstream)) <= 0)
				return ret;
			if (done)
				break;
		}

		ret = BZ2_bzCompress(zs, BZ_FINISH);
		switch (ret) {
		case BZ_RUN_OK:
		case BZ_FLUSH_OK:
		case BZ_STREAM_END:
			done = TRUE;
			break;
		case BZ_FINISH_OK:
			break;
                case BZ_MEM_ERROR:
                        i_fatal_status(FATAL_OUTOFMEM, "bzip2.write(%s): Out of memory",
                                       o_stream_get_name(&zstream->ostream.ostream));
                default:
                        i_fatal("BZ2_bzCompress() failed with %d", ret);
		}
	} while (zs->avail_out != sizeof(zstream->outbuf));

	if (final)
		zstream->flushed = TRUE;
	i_assert(zstream->outbuf_used == 0);
	return 1;
}

static int o_stream_bzlib_flush(struct ostream_private *stream)
{
	struct bzlib_ostream *zstream = (struct bzlib_ostream *)stream;
	int ret;
	if ((ret = o_stream_bzlib_send_flush(zstream, stream->finished)) < 0)
		return -1;
	else if (ret > 0)
		return o_stream_flush_parent(stream);
	return ret;
}

static size_t
o_stream_bzlib_get_buffer_used_size(const struct ostream_private *stream)
{
	const struct bzlib_ostream *zstream =
		(const struct bzlib_ostream *)stream;

	/* outbuf has already compressed data that we're trying to send to the
	   parent stream. We're not including bzlib's internal compression
	   buffer size. */
	return (zstream->outbuf_used - zstream->outbuf_offset) +
		o_stream_get_buffer_used_size(stream->parent);
}

static size_t
o_stream_bzlib_get_buffer_avail_size(const struct ostream_private *stream)
{
	/* FIXME: not correct - this is counting compressed size, which may be
	   too larger than uncompressed size in some situations. Fixing would
	   require some kind of additional buffering. */
	return o_stream_get_buffer_avail_size(stream->parent);
}

static ssize_t
o_stream_bzlib_sendv(struct ostream_private *stream,
		    const struct const_iovec *iov, unsigned int iov_count)
{
	struct bzlib_ostream *zstream = (struct bzlib_ostream *)stream;
	ssize_t ret, bytes = 0;
	unsigned int i;

	if ((ret = o_stream_zlib_send_outbuf(zstream)) <= 0) {
		/* error / we still couldn't flush existing data to
		   parent stream. */
		return ret;
	}

	for (i = 0; i < iov_count; i++) {
		ret = o_stream_bzlib_send_chunk(zstream, iov[i].iov_base,
						iov[i].iov_len);
		if (ret < 0)
			return -1;
		bytes += ret;
		if ((size_t)ret != iov[i].iov_len)
			break;
	}
	stream->ostream.offset += bytes;

	/* avail_in!=0 check is used to detect errors. if it's non-zero here
	   it simply means we didn't send all the data */
	zstream->zs.avail_in = 0;
	return bytes;
}

static struct ostream *o_stream_create_bz2(struct ostream *output, int level)
{
	struct bzlib_ostream *zstream;
	int ret;

	i_assert(level >= 1 && level <= 9);

	zstream = i_new(struct bzlib_ostream, 1);
	zstream->ostream.sendv = o_stream_bzlib_sendv;
	zstream->ostream.flush = o_stream_bzlib_flush;
	zstream->ostream.get_buffer_used_size =
		o_stream_bzlib_get_buffer_used_size;
	zstream->ostream.get_buffer_avail_size =
		o_stream_bzlib_get_buffer_avail_size;
	zstream->ostream.iostream.close = o_stream_bzlib_close;

	ret = BZ2_bzCompressInit(&zstream->zs, level, 0, 0);
	switch (ret) {
	case BZ_OK:
		break;
	case BZ_MEM_ERROR:
		i_fatal_status(FATAL_OUTOFMEM,
			       "bzlib: Out of memory");
	case BZ_CONFIG_ERROR:
		i_fatal("Wrong bzlib library version (broken compilation)");
	case BZ_PARAM_ERROR:
		i_fatal("bzlib: Invalid parameters");
	default:
		i_fatal("BZ2_bzCompressInit() failed with %d", ret);
	}

	zstream->zs.next_out = zstream->outbuf;
	zstream->zs.avail_out = sizeof(zstream->outbuf);
	o_stream_init_buffering_flush(&zstream->ostream, output);
	return o_stream_create(&zstream->ostream, output,
			       o_stream_get_fd(output));
}

struct ostream *
o_stream_create_bz2_auto(struct ostream *output, struct event *event)
{
	const struct bzlib_settings *set;
	const char *error;

	if (settings_get(event, &bzlib_setting_parser_info, 0,
			 &set, &error) < 0)
		return o_stream_create_error_str(EIO, "%s", error);
	int block_size = set->compress_bz2_block_size_100k;
	settings_free(set);
	return o_stream_create_bz2(output, block_size);
}

#endif
