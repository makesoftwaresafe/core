/* Copyright (c) 2010-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "crc32.h"
#include "ostream-private.h"
#include "settings.h"
#include "ostream-zlib.h"
#include <zlib.h>

#define CHUNK_SIZE (1024*32)
#define ZLIB_OS_CODE 0x03  /* Unix */

struct zlib_ostream {
	struct ostream_private ostream;
	z_stream zs;

	unsigned char gz_header[10];
	unsigned char outbuf[CHUNK_SIZE];
	unsigned int outbuf_offset, outbuf_used;
	unsigned int header_bytes_left;

	uint32_t crc, bytes32;

	bool gz:1;
	bool flushed:1;
};

struct zlib_settings {
	pool_t pool;
	unsigned int compress_gz_level;
	unsigned int compress_deflate_level;
};

static bool zlib_settings_check(void *_set, pool_t pool, const char **error_r);

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct zlib_settings)
static const struct setting_define zlib_setting_defines[] = {
	DEF(UINT, compress_gz_level),
	DEF(UINT, compress_deflate_level),

	SETTING_DEFINE_LIST_END
};
static const struct zlib_settings zlib_default_settings = {
	.compress_gz_level = 6,
	.compress_deflate_level = 6,
};

const struct setting_parser_info zlib_setting_parser_info = {
	.name = "zlib",

	.defines = zlib_setting_defines,
	.defaults = &zlib_default_settings,

	.struct_size = sizeof(struct zlib_settings),
	.pool_offset1 = 1 + offsetof(struct zlib_settings, pool),
#ifndef CONFIG_BINARY
	.check_func = zlib_settings_check,
#endif
};

static bool zlib_settings_check(void *_set, pool_t pool ATTR_UNUSED,
				const char **error_r)
{
	struct zlib_settings *set = _set;

	if (set->compress_gz_level > Z_BEST_COMPRESSION) {
		*error_r = t_strdup_printf(
			"compress_gz_level must be between %d..%d",
			Z_NO_COMPRESSION, Z_BEST_COMPRESSION);
		return FALSE;
	}
	if (set->compress_deflate_level > Z_BEST_COMPRESSION) {
		*error_r = t_strdup_printf(
			"compress_deflate_level must be between %d..%d",
			Z_NO_COMPRESSION, Z_BEST_COMPRESSION);
		return FALSE;
	}
	return TRUE;
}

static void o_stream_zlib_close(struct iostream_private *stream,
				bool close_parent)
{
	struct zlib_ostream *zstream = (struct zlib_ostream *)stream;

	i_assert(zstream->ostream.finished ||
		 zstream->ostream.ostream.stream_errno != 0 ||
		 zstream->ostream.error_handling_disabled);
	(void)deflateEnd(&zstream->zs);
	if (close_parent)
		o_stream_close(zstream->ostream.parent);
}

static int o_stream_zlib_send_gz_header(struct zlib_ostream *zstream)
{
	size_t header_send_offset =
		sizeof(zstream->gz_header) - zstream->header_bytes_left;
	ssize_t ret;

	i_assert(zstream->header_bytes_left <= sizeof(zstream->gz_header));
	ret = o_stream_send(zstream->ostream.parent,
			    zstream->gz_header + header_send_offset,
			    zstream->header_bytes_left);
	if (ret < 0) {
		o_stream_copy_error_from_parent(&zstream->ostream);
		return -1;
	}
	i_assert((size_t)ret <= zstream->header_bytes_left);
	zstream->header_bytes_left -= ret;
	if (zstream->header_bytes_left != 0) {
		o_stream_set_flush_pending(&zstream->ostream.ostream, TRUE);
		return 0;
	}
	return 1;
}

static int o_stream_zlib_lsb_uint32(struct ostream *output, uint32_t num)
{
	unsigned char buf[sizeof(uint32_t)];
	unsigned int i;

	for (i = 0; i < sizeof(buf); i++) {
		buf[i] = num & 0xff;
		num >>= 8;
	}
	if (o_stream_send(output, buf, sizeof(buf)) != sizeof(buf))
		return -1;
	return 0;
}

static int o_stream_zlib_send_gz_trailer(struct zlib_ostream *zstream)
{
	struct ostream *output = zstream->ostream.parent;

	if (!zstream->gz)
		return 0;

	if (o_stream_zlib_lsb_uint32(output, zstream->crc) < 0 ||
	    o_stream_zlib_lsb_uint32(output, zstream->bytes32) < 0) {
		o_stream_copy_error_from_parent(&zstream->ostream);
		return -1;
	}
	return 0;
}

static int o_stream_zlib_send_outbuf(struct zlib_ostream *zstream)
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
o_stream_zlib_send_chunk(struct zlib_ostream *zstream,
			 const void *data, size_t size)
{
	z_stream *zs = &zstream->zs;
	int ret, flush;

	i_assert(zstream->outbuf_used == 0);

	flush = zstream->ostream.corked || zstream->gz ?
		Z_NO_FLUSH : Z_SYNC_FLUSH;

	if (zstream->header_bytes_left > 0) {
		if ((ret = o_stream_zlib_send_gz_header(zstream)) <= 0)
			return ret;
	}

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

		ret = deflate(zs, flush);
		switch (ret) {
		case Z_OK:
		case Z_BUF_ERROR:
			break;
		case Z_MEM_ERROR:
			i_fatal_status(FATAL_OUTOFMEM, "zlib: Out of memory");
		case Z_STREAM_ERROR:
			i_assert(zstream->gz);
			i_panic("zlib.write(%s) failed: Can't write more data to .gz after flushing",
				o_stream_get_name(&zstream->ostream.ostream));
		default:
			i_panic("zlib.write(%s) failed with unexpected code %d",
				o_stream_get_name(&zstream->ostream.ostream), ret);
		}
	}
	size -= zs->avail_in;

	zstream->crc = crc32_data_more(zstream->crc, data, size);
	zstream->bytes32 += size;
	zstream->flushed = FALSE;
	return size;
}

static int
o_stream_zlib_send_flush(struct zlib_ostream *zstream, bool final)
{
	z_stream *zs = &zstream->zs;
	size_t len;
	bool done = FALSE;
	int ret, flush;

	i_assert(zs->avail_in == 0);

	if (zstream->flushed) {
		i_assert(zstream->outbuf_used == 0);
		return 1;
	}

	if ((ret = o_stream_flush_parent_if_needed(&zstream->ostream)) <= 0)
		return ret;
	if (zstream->header_bytes_left > 0) {
		if ((ret = o_stream_zlib_send_gz_header(zstream)) <= 0)
			return ret;
	}

	if ((ret = o_stream_zlib_send_outbuf(zstream)) <= 0)
		return ret;

	flush = final ? Z_FINISH :
		(!zstream->gz ? Z_SYNC_FLUSH : Z_NO_FLUSH);

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

		switch (deflate(zs, flush)) {
		case Z_OK:
		case Z_BUF_ERROR:
			break;
		case Z_STREAM_END:
			done = TRUE;
			break;
		case Z_MEM_ERROR:
			i_fatal_status(FATAL_OUTOFMEM, "zlib: Out of memory");
		default:
			i_unreached();
		}
	} while (zs->avail_out != sizeof(zstream->outbuf));

	if (final) {
		if (o_stream_zlib_send_gz_trailer(zstream) < 0)
			return -1;
	}
	if (final)
		zstream->flushed = TRUE;
	i_assert(zstream->outbuf_used == 0);
	return 1;
}

static int o_stream_zlib_flush(struct ostream_private *stream)
{
	struct zlib_ostream *zstream = (struct zlib_ostream *)stream;
	int ret;
	if ((ret = o_stream_zlib_send_flush(zstream, stream->finished)) < 0)
		return -1;
	else if (ret > 0)
		return o_stream_flush_parent(stream);
	return ret;
}

static size_t
o_stream_zlib_get_buffer_used_size(const struct ostream_private *stream)
{
	const struct zlib_ostream *zstream =
		(const struct zlib_ostream *)stream;

	/* outbuf has already compressed data that we're trying to send to the
	   parent stream. We're not including zlib's internal compression
	   buffer size. */
	return (zstream->outbuf_used - zstream->outbuf_offset) +
		o_stream_get_buffer_used_size(stream->parent);
}

static size_t
o_stream_zlib_get_buffer_avail_size(const struct ostream_private *stream)
{
	/* FIXME: not correct - this is counting compressed size, which may be
	   too larger than uncompressed size in some situations. Fixing would
	   require some kind of additional buffering. */
	return o_stream_get_buffer_avail_size(stream->parent);
}

static ssize_t
o_stream_zlib_sendv(struct ostream_private *stream,
		    const struct const_iovec *iov, unsigned int iov_count)
{
	struct zlib_ostream *zstream = (struct zlib_ostream *)stream;
	ssize_t ret, bytes = 0;
	unsigned int i;

	if ((ret = o_stream_zlib_send_outbuf(zstream)) <= 0) {
		/* error / we still couldn't flush existing data to
		   parent stream. */
		return ret;
	}

	for (i = 0; i < iov_count; i++) {
		ret = o_stream_zlib_send_chunk(zstream, iov[i].iov_base,
					       iov[i].iov_len);
		if (ret < 0)
			return -1;
		bytes += ret;
		if ((size_t)ret != iov[i].iov_len)
			break;
	}
	stream->ostream.offset += bytes;

	if (!zstream->ostream.corked && i == iov_count) {
		if (o_stream_zlib_send_flush(zstream, FALSE) < 0)
			return -1;
	}
	/* avail_in!=0 check is used to detect errors. if it's non-zero here
	   it simply means we didn't send all the data */
	zstream->zs.avail_in = 0;
	return bytes;
}

static void o_stream_zlib_init_gz_header(struct zlib_ostream *zstream,
					 int level, int strategy)
{
	unsigned char *hdr = zstream->gz_header;

	hdr[0] = 0x1f;
	hdr[1] = 0x8b;
	hdr[2] = Z_DEFLATED;
	hdr[8] = level == 9 ? 2 :
		(strategy >= Z_HUFFMAN_ONLY ||
		 (level != Z_DEFAULT_COMPRESSION && level < 2) ? 4 : 0);
	hdr[9] = ZLIB_OS_CODE;
	i_assert(sizeof(zstream->gz_header) == 10);
}

static struct ostream *
o_stream_create_zlib(struct ostream *output, int level, bool gz)
{
	const int strategy = Z_DEFAULT_STRATEGY;
	struct zlib_ostream *zstream;
	int ret;

	/* accepted range is 0..9 and -1 is default compression */
	i_assert(level >= -1 && level <= 9);

	zstream = i_new(struct zlib_ostream, 1);
	zstream->ostream.sendv = o_stream_zlib_sendv;
	zstream->ostream.flush = o_stream_zlib_flush;
	zstream->ostream.get_buffer_used_size =
		o_stream_zlib_get_buffer_used_size;
	zstream->ostream.get_buffer_avail_size =
		o_stream_zlib_get_buffer_avail_size;
	zstream->ostream.iostream.close = o_stream_zlib_close;
	zstream->crc = 0;
	zstream->gz = gz;
	if (gz)
		zstream->header_bytes_left = sizeof(zstream->gz_header);

	o_stream_zlib_init_gz_header(zstream, level, strategy);
	ret = deflateInit2(&zstream->zs, level, Z_DEFLATED, -15, 8, strategy);
	switch (ret) {
	case Z_OK:
		break;
	case Z_MEM_ERROR:
		i_fatal_status(FATAL_OUTOFMEM, "deflateInit(): Out of memory");
	case Z_VERSION_ERROR:
		i_fatal("Wrong zlib library version (broken compilation)");
	case Z_STREAM_ERROR:
		i_fatal("Invalid compression level %d", level);
	default:
		i_fatal("deflateInit() failed with %d", ret);
	}

	zstream->zs.next_out = zstream->outbuf;
	zstream->zs.avail_out = sizeof(zstream->outbuf);
	o_stream_init_buffering_flush(&zstream->ostream, output);
	return o_stream_create(&zstream->ostream, output,
			       o_stream_get_fd(output));
}

static struct ostream *
o_stream_create_zlib_auto(struct ostream *output, struct event *event, bool gz)
{
	const struct zlib_settings *set;
	const char *error;

	if (settings_get(event, &zlib_setting_parser_info, 0,
			 &set, &error) < 0)
		return o_stream_create_error_str(EIO, "%s", error);
	int level = gz ? set->compress_gz_level : set->compress_deflate_level;
	settings_free(set);
	return o_stream_create_zlib(output, level, gz);
}

struct ostream *o_stream_create_gz_auto(struct ostream *output, struct event *event)
{
	return o_stream_create_zlib_auto(output, event, TRUE);
}

struct ostream *o_stream_create_deflate_auto(struct ostream *output, struct event *event)
{
	return o_stream_create_zlib_auto(output, event, FALSE);
}

struct ostream *o_stream_create_deflate(struct ostream *output, int level)
{
	return o_stream_create_zlib(output, level, FALSE);
}
