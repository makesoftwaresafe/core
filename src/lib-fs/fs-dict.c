/* Copyright (c) 2015-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "str.h"
#include "guid.h"
#include "hex-binary.h"
#include "base64.h"
#include "istream.h"
#include "ostream.h"
#include "settings.h"
#include "dict.h"
#include "fs-api-private.h"

enum fs_dict_value_encoding {
	FS_DICT_VALUE_ENCODING_RAW,
	FS_DICT_VALUE_ENCODING_HEX,
	FS_DICT_VALUE_ENCODING_BASE64
};

struct dict_fs {
	struct fs fs;
	struct dict *dict;
	char *path_prefix;
	enum fs_dict_value_encoding encoding;
};

struct dict_fs_file {
	struct fs_file file;
	pool_t pool;
	const char *key, *value;
	buffer_t *write_buffer;
};

struct dict_fs_iter {
	struct fs_iter iter;
	struct dict_iterate_context *dict_iter;
};

static struct fs *fs_dict_alloc(void)
{
	struct dict_fs *fs;

	fs = i_new(struct dict_fs, 1);
	fs->fs = fs_class_dict;
	return &fs->fs;
}

struct fs_dict_settings {
	pool_t pool;
	const char *fs_dict_value_encoding;
};

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct fs_dict_settings)
static const struct setting_define fs_dict_setting_defines[] = {
	DEF(ENUM, fs_dict_value_encoding),

	SETTING_DEFINE_LIST_END
};
static const struct fs_dict_settings fs_dict_default_settings = {
	.fs_dict_value_encoding = "raw:hex:base64",
};

const struct setting_parser_info fs_dict_setting_parser_info = {
	.name = "fs_dict",

	.defines = fs_dict_setting_defines,
	.defaults = &fs_dict_default_settings,

	.struct_size = sizeof(struct fs_dict_settings),
	.pool_offset1 = 1 + offsetof(struct fs_dict_settings, pool),
};

static int
fs_dict_value_encoding_parse(const char *str,
			     enum fs_dict_value_encoding *encoding_r,
			     const char **error_r)
{
	if (strcmp(str, "raw") == 0)
		*encoding_r = FS_DICT_VALUE_ENCODING_RAW;
	else if (strcmp(str, "hex") == 0)
		*encoding_r = FS_DICT_VALUE_ENCODING_HEX;
	else if (strcmp(str, "base64") == 0)
		*encoding_r = FS_DICT_VALUE_ENCODING_BASE64;
	else {
		*error_r = t_strdup_printf("Unknown value encoding '%s'", str);
		return -1;
	}
	return 0;
}

static int
fs_dict_init(struct fs *_fs, const struct fs_parameters *params ATTR_UNUSED,
	     const char **error_r)
{
	struct dict_fs *fs = container_of(_fs, struct dict_fs, fs);
	struct fs_dict_settings *fs_dict_set;
	int ret;

	if (settings_get(_fs->event, &fs_dict_setting_parser_info, 0,
			 &fs_dict_set, error_r) < 0)
		return -1;
	ret = fs_dict_value_encoding_parse(fs_dict_set->fs_dict_value_encoding,
					   &fs->encoding, error_r);
	settings_free(fs_dict_set);
	if (ret < 0)
		return -1;

	struct event *event = event_create(_fs->event);
	settings_event_add_filter_name(event, "fs_dict");
	ret = dict_init_auto(event, &fs->dict, error_r);
	event_unref(&event);
	if (ret == 0)
		*error_r = "fs_dict { dict { .. } } not set";
	return ret <= 0 ? -1 : 0;
}

static void fs_dict_free(struct fs *_fs)
{
	struct dict_fs *fs = (struct dict_fs *)_fs;

	if (fs->dict != NULL) dict_deinit(&fs->dict);
	i_free(fs);
}

static enum fs_properties fs_dict_get_properties(struct fs *fs ATTR_UNUSED)
{
	return FS_PROPERTY_ITER | FS_PROPERTY_RELIABLEITER;
}

static struct fs_file *fs_dict_file_alloc(void)
{
	struct dict_fs_file *file;
	pool_t pool;

	pool = pool_alloconly_create("fs dict file", 128);
	file = p_new(pool, struct dict_fs_file, 1);
	file->pool = pool;
	return &file->file;
}

static void
fs_dict_file_init(struct fs_file *_file, const char *path,
		  enum fs_open_mode mode, enum fs_open_flags flags ATTR_UNUSED)
{
	struct dict_fs_file *file = (struct dict_fs_file *)_file;
	struct dict_fs *fs = (struct dict_fs *)_file->fs;
	guid_128_t guid;

	i_assert(mode != FS_OPEN_MODE_APPEND); /* not supported */
	i_assert(mode != FS_OPEN_MODE_CREATE); /* not supported */

	if (mode != FS_OPEN_MODE_CREATE_UNIQUE_128)
		file->file.path = p_strdup(file->pool, path);
	else {
		guid_128_generate(guid);
		file->file.path = p_strdup_printf(file->pool, "%s/%s", path,
						  guid_128_to_string(guid));
	}
	file->key = fs->path_prefix == NULL ?
		p_strdup(file->pool, file->file.path) :
		p_strconcat(file->pool, fs->path_prefix, file->file.path, NULL);
}

static void fs_dict_file_deinit(struct fs_file *_file)
{
	struct dict_fs_file *file = (struct dict_fs_file *)_file;

	i_assert(_file->output == NULL);

	fs_file_free(_file);
	pool_unref(&file->pool);
}

static bool fs_dict_prefetch(struct fs_file *_file ATTR_UNUSED,
			     uoff_t length ATTR_UNUSED)
{
	/* once async dict_lookup() is implemented, we want to start it here */
	return TRUE;
}

static int fs_dict_lookup(struct dict_fs_file *file)
{
	struct dict_fs *fs = (struct dict_fs *)file->file.fs;
	const char *error;
	int ret;

	if (file->value != NULL)
		return 0;

	struct dict_op_settings set = {
		.username = file->file.fs->username,
	};
	ret = dict_lookup(fs->dict, &set, file->pool, file->key, &file->value, &error);
	if (ret > 0)
		return 0;
	else if (ret < 0) {
		fs_set_error(file->file.event, EIO,
			     "dict_lookup(%s) failed: %s", file->key, error);
		return -1;
	} else {
		fs_set_error(file->file.event, ENOENT,
			     "Dict key %s doesn't exist", file->key);
		return -1;
	}
}

static struct istream *
fs_dict_read_stream(struct fs_file *_file, size_t max_buffer_size ATTR_UNUSED)
{
	struct dict_fs_file *file = (struct dict_fs_file *)_file;
	struct istream *input;

	if (fs_dict_lookup(file) < 0)
		input = i_stream_create_error_str(errno, "%s", fs_file_last_error(_file));
	else
		input = i_stream_create_from_data(file->value, strlen(file->value));
	i_stream_set_name(input, file->key);
	return input;
}

static void fs_dict_write_stream(struct fs_file *_file)
{
	struct dict_fs_file *file = (struct dict_fs_file *)_file;

	i_assert(_file->output == NULL);

	file->write_buffer = buffer_create_dynamic(file->pool, 128);
	_file->output = o_stream_create_buffer(file->write_buffer);
	o_stream_set_name(_file->output, file->key);
}

static void fs_dict_write_rename_if_needed(struct dict_fs_file *file)
{
	struct dict_fs *fs = (struct dict_fs *)file->file.fs;
	const char *new_fname;

	new_fname = fs_metadata_find(&file->file.metadata, FS_METADATA_WRITE_FNAME);
	if (new_fname == NULL)
		return;

	file->file.path = p_strdup(file->pool, new_fname);
	file->key = fs->path_prefix == NULL ? p_strdup(file->pool, new_fname) :
		p_strconcat(file->pool, fs->path_prefix, new_fname, NULL);
}

static int fs_dict_write_stream_finish(struct fs_file *_file, bool success)
{
	struct dict_fs_file *file = (struct dict_fs_file *)_file;
	struct dict_fs *fs = (struct dict_fs *)_file->fs;
	struct dict_transaction_context *trans;
	const char *error;

	o_stream_destroy(&_file->output);
	if (!success)
		return -1;

	struct dict_op_settings set = {
		.username = _file->fs->username,
	};
	fs_dict_write_rename_if_needed(file);
	trans = dict_transaction_begin(fs->dict, &set);
	switch (fs->encoding) {
	case FS_DICT_VALUE_ENCODING_RAW:
		dict_set(trans, file->key, str_c(file->write_buffer));
		break;
	case FS_DICT_VALUE_ENCODING_HEX: {
		string_t *hex = t_str_new(file->write_buffer->used * 2 + 1);
		binary_to_hex_append(hex, file->write_buffer->data,
				     file->write_buffer->used);
		dict_set(trans, file->key, str_c(hex));
		break;
	}
	case FS_DICT_VALUE_ENCODING_BASE64: {
		const size_t base64_size =
			MAX_BASE64_ENCODED_SIZE(file->write_buffer->used);
		string_t *base64 = t_str_new(base64_size);
		base64_encode(file->write_buffer->data,
			      file->write_buffer->used, base64);
		dict_set(trans, file->key, str_c(base64));
	}
	}
	if (dict_transaction_commit(&trans, &error) < 0) {
		fs_set_error(_file->event, EIO,
			     "Dict transaction commit failed: %s", error);
		return -1;
	}
	return 1;
}

static int fs_dict_stat(struct fs_file *_file, struct stat *st_r)
{
	struct dict_fs_file *file = (struct dict_fs_file *)_file;

	i_zero(st_r);

	if (fs_dict_lookup(file) < 0)
		return -1;
	st_r->st_size = strlen(file->value);
	return 0;
}

static int fs_dict_delete(struct fs_file *_file)
{
	struct dict_fs_file *file = (struct dict_fs_file *)_file;
	struct dict_fs *fs = (struct dict_fs *)_file->fs;
	struct dict_transaction_context *trans;
	const char *error;

	struct dict_op_settings set = {
		.username = fs->fs.username,
	};
	trans = dict_transaction_begin(fs->dict, &set);
	dict_unset(trans, file->key);
	if (dict_transaction_commit(&trans, &error) < 0) {
		fs_set_error(_file->event, EIO,
			     "Dict transaction commit failed: %s", error);
		return -1;
	}
	return 0;
}

static struct fs_iter *fs_dict_iter_alloc(void)
{
	struct dict_fs_iter *iter = i_new(struct dict_fs_iter, 1);
	return &iter->iter;
}

static void
fs_dict_iter_init(struct fs_iter *_iter, const char *path,
		  enum fs_iter_flags flags ATTR_UNUSED)
{
	struct dict_fs_iter *iter = (struct dict_fs_iter *)_iter;
	struct dict_fs *fs = (struct dict_fs *)_iter->fs;

	if (fs->path_prefix != NULL)
		path = t_strconcat(fs->path_prefix, path, NULL);

	struct dict_op_settings set = {
		.username = iter->iter.fs->username,
	};
	iter->dict_iter = dict_iterate_init(fs->dict, &set, path, 0);
}

static const char *fs_dict_iter_next(struct fs_iter *_iter)
{
	struct dict_fs_iter *iter = (struct dict_fs_iter *)_iter;
	const char *key, *value;

	if (!dict_iterate(iter->dict_iter, &key, &value))
		return NULL;
	return key;
}

static int fs_dict_iter_deinit(struct fs_iter *_iter)
{
	struct dict_fs_iter *iter = (struct dict_fs_iter *)_iter;
	const char *error;
	int ret;

	ret = dict_iterate_deinit(&iter->dict_iter, &error);
	if (ret < 0)
		fs_set_error(_iter->event, EIO,
			     "Dict iteration failed: %s", error);
	return ret;
}

const struct fs fs_class_dict = {
	.name = "dict",
	.v = {
		.alloc = fs_dict_alloc,
		.init = fs_dict_init,
		.deinit = NULL,
		.free = fs_dict_free,
		.get_properties = fs_dict_get_properties,
		.file_alloc = fs_dict_file_alloc,
		.file_init = fs_dict_file_init,
		.file_deinit = fs_dict_file_deinit,
		.file_close = NULL,
		.get_path = NULL,
		.set_async_callback = NULL,
		.wait_async = NULL,
		.set_metadata = fs_default_set_metadata,
		.get_metadata = NULL,
		.prefetch = fs_dict_prefetch,
		.read = NULL,
		.read_stream = fs_dict_read_stream,
		.write = NULL,
		.write_stream = fs_dict_write_stream,
		.write_stream_finish = fs_dict_write_stream_finish,
		.lock = NULL,
		.unlock = NULL,
		.exists = NULL,
		.stat = fs_dict_stat,
		.copy = fs_default_copy,
		.rename = NULL,
		.delete_file = fs_dict_delete,
		.iter_alloc = fs_dict_iter_alloc,
		.iter_init = fs_dict_iter_init,
		.iter_next = fs_dict_iter_next,
		.iter_deinit = fs_dict_iter_deinit,
		.switch_ioloop = NULL,
		.get_nlinks = NULL
	}
};
