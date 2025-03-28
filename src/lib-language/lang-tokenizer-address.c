/* Copyright (c) 2015-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "buffer.h"
#include "rfc822-parser.h"
#include "lang-tokenizer-private.h"
#include "lang-tokenizer-common.h"
#include "lang-settings.h"

#define IS_DTEXT(c) \
	(rfc822_atext_chars[(int)(unsigned char)(c)] == 2)

enum email_address_parser_state {
	EMAIL_ADDRESS_PARSER_STATE_NONE = 0,
	EMAIL_ADDRESS_PARSER_STATE_LOCALPART,
	EMAIL_ADDRESS_PARSER_STATE_DOMAIN,
	EMAIL_ADDRESS_PARSER_STATE_COMPLETE,
	EMAIL_ADDRESS_PARSER_STATE_SKIP,
};

struct email_address_lang_tokenizer {
	struct lang_tokenizer tokenizer;
	enum email_address_parser_state state;
	string_t *last_word;
	string_t *parent_data; /* Copy of input data between tokens. */
	unsigned int max_length;
	bool search;
};

static int
lang_tokenizer_email_address_create(const struct lang_settings *set,
				    struct event *event ATTR_UNUSED,
				    enum lang_tokenizer_flags flags,
				    struct lang_tokenizer **tokenizer_r,
				    const char **error_r ATTR_UNUSED)
{
	struct email_address_lang_tokenizer *tok;
	tok = i_new(struct email_address_lang_tokenizer, 1);
	tok->tokenizer = *lang_tokenizer_email_address;
	tok->last_word = str_new(default_pool, 128);
	tok->parent_data = str_new(default_pool, 128);
	tok->max_length = set->tokenizer_address_token_maxlen;
	tok->search = HAS_ALL_BITS(flags, LANG_TOKENIZER_FLAG_SEARCH);
	*tokenizer_r = &tok->tokenizer;
	return 0;
}

static void lang_tokenizer_email_address_destroy(struct lang_tokenizer *_tok)
{
	struct email_address_lang_tokenizer *tok =
		(struct email_address_lang_tokenizer *)_tok;

	str_free(&tok->last_word);
	str_free(&tok->parent_data);
	i_free(tok);
}

static bool
lang_tokenizer_address_current_token(struct email_address_lang_tokenizer *tok,
                                     const char **token_r)
{
	const unsigned char *data = tok->last_word->data;
	size_t len = tok->last_word->used;

	tok->tokenizer.skip_parents = TRUE;
	tok->state = EMAIL_ADDRESS_PARSER_STATE_NONE;
	if (str_len(tok->last_word) > tok->max_length) {
		str_truncate(tok->last_word, tok->max_length);
		/* As future proofing, delete partial utf8.
		   IS_DTEXT() does not actually allow utf8 addresses
		   yet though. */
		len = tok->last_word->used;
		lang_tokenizer_delete_trailing_partial_char(data, &len);
		i_assert(len <= tok->max_length);
	}

	if (len > 0)
		lang_tokenizer_delete_trailing_invalid_char(data, &len);
	*token_r = len == 0 ? "" :
		t_strndup(data, len);
	return len > 0;
}

static bool
lang_tokenizer_address_parent_data(struct email_address_lang_tokenizer *tok,
                                   const char **token_r)
{
	if (tok->tokenizer.parent == NULL || str_len(tok->parent_data) == 0)
		return FALSE;

	if (tok->search && tok->state >= EMAIL_ADDRESS_PARSER_STATE_DOMAIN) {
		/* we're searching and we want to find only the full
		   user@domain (not "user" and "domain"). we'll do this by
		   not feeding the last user@domain to parent tokenizer. */
		size_t parent_prefix_len =
			str_len(tok->parent_data) - str_len(tok->last_word);
		i_assert(str_len(tok->parent_data) >= str_len(tok->last_word) &&
			 strcmp(str_c(tok->parent_data) + parent_prefix_len,
				str_c(tok->last_word)) == 0);
		str_truncate(tok->parent_data, parent_prefix_len);
		if (str_len(tok->parent_data) == 0)
			return FALSE;
	}

	*token_r = t_strdup(str_c(tok->parent_data));
	str_truncate(tok->parent_data, 0);
	return TRUE;
}

/* Used to rewind past characters that cannot be the start of a new localpart.
 Returns size that can be skipped. */
static size_t skip_nonlocal_part(const unsigned char *data, size_t size)
{
	size_t skip = 0;

	/* Yes, a dot can start an address. De facto before de jure. */
	while (skip < size && (!IS_ATEXT(data[skip]) && data[skip] != '.'))
		skip++;
	return skip;
}

static bool
lang_tokenizer_email_address_too_large(struct email_address_lang_tokenizer *tok,
				       size_t pos)
{
	if (str_len(tok->last_word) + pos <= tok->max_length)
		return FALSE;

	/* The token is too large - skip over it.

	   Truncate the input that was added so far to the token, so all of it
	   gets sent to the parent tokenizer in
	   lang_tokenizer_address_parent_data(). */
	str_truncate(tok->last_word, 0);
	return TRUE;
}

static enum email_address_parser_state
lang_tokenizer_email_address_parse_local(struct email_address_lang_tokenizer *tok,
                                         const unsigned char *data, size_t size,
                                         size_t *skip_r)
{
	size_t pos = 0;
	bool seen_at = FALSE;

	i_assert(size == 0 || data != NULL);

	while (pos < size && (IS_ATEXT(data[pos]) ||
			      data[pos] == '@' || data[pos] == '.')) {
		if (data[pos] == '@')
			seen_at = TRUE;
		pos++;
		if (seen_at)
			break;
	}

	if (lang_tokenizer_email_address_too_large(tok, pos)) {
		*skip_r = 0;
		return EMAIL_ADDRESS_PARSER_STATE_SKIP;
	}

	 /* localpart and @ */
	if (seen_at && (pos > 1 || str_len(tok->last_word) > 0)) {
		str_append_data(tok->last_word, data, pos);
		*skip_r = pos;
		return EMAIL_ADDRESS_PARSER_STATE_DOMAIN;
	}

	/* localpart, @ not included yet */
	if (pos > 0 && (IS_ATEXT(data[pos-1]) || data[pos-1] == '.')) {
		str_append_data(tok->last_word, data, pos);
		*skip_r = pos;
		return  EMAIL_ADDRESS_PARSER_STATE_LOCALPART;
	}
	/* not a localpart. skip past rest of no-good chars. */
	pos += skip_nonlocal_part(data+pos, size - pos);
	*skip_r = pos;
	return EMAIL_ADDRESS_PARSER_STATE_NONE;
}

static bool domain_is_empty(struct email_address_lang_tokenizer *tok)
{
	const char *p, *str = str_c(tok->last_word);

	if ((p = strchr(str, '@')) == NULL)
		return TRUE;
	return p[1] == '\0';
}

static enum email_address_parser_state
lang_tokenizer_email_address_parse_domain(struct email_address_lang_tokenizer *tok,
                                          const unsigned char *data, size_t size,
                                          size_t *skip_r)
{
	size_t pos = 0;

	while (pos < size && (IS_DTEXT(data[pos]) || data[pos] == '.' || data[pos] == '-'))
		pos++;

	if (lang_tokenizer_email_address_too_large(tok, pos)) {
		*skip_r = 0;
		return EMAIL_ADDRESS_PARSER_STATE_SKIP;
	}

	 /* A complete domain name */
	if ((pos > 0 && pos < size) || /* non-atext after atext in this data*/
	    (pos < size && !domain_is_empty(tok))) { /* non-atext after previous atext */
		str_append_data(tok->last_word, data, pos);
		*skip_r = pos;
		return EMAIL_ADDRESS_PARSER_STATE_COMPLETE;
	}
	if (pos == size) { /* All good, but possibly not complete. */
		str_append_data(tok->last_word, data, pos);
		*skip_r = pos;
		return EMAIL_ADDRESS_PARSER_STATE_DOMAIN;
	}
	/* not a domain. skip past no-good chars. */
	pos += skip_nonlocal_part(data + pos, size - pos);
	*skip_r = pos;
	return EMAIL_ADDRESS_PARSER_STATE_NONE;
}

static bool
lang_tokenizer_address_skip(const unsigned char *data, size_t size,
			    size_t *skip_r)
{
	for (size_t pos = 0; pos < size; pos++) {
		if (!(IS_ATEXT(data[pos]) || data[pos] == '.' ||
		      data[pos] == '-') || data[pos] == '@') {
			*skip_r = pos;
			return TRUE;
		}
	}
	*skip_r = size;
	return FALSE;
}

/* Buffer raw data for parent. */
static void
lang_tokenizer_address_update_parent(struct email_address_lang_tokenizer *tok,
                                     const unsigned char *data, size_t size)
{
	if (tok->tokenizer.parent != NULL)
		str_append_data(tok->parent_data, data, size);
}

static void lang_tokenizer_email_address_reset(struct lang_tokenizer *_tok)
{
	struct email_address_lang_tokenizer *tok =
		(struct email_address_lang_tokenizer *)_tok;

	tok->state = EMAIL_ADDRESS_PARSER_STATE_NONE;
	str_truncate(tok->last_word, 0);
	str_truncate(tok->parent_data, 0);
}

static int
lang_tokenizer_email_address_next(struct lang_tokenizer *_tok,
                                  const unsigned char *data, size_t size,
				  size_t *skip_r, const char **token_r,
				  const char **error_r ATTR_UNUSED)
{
	struct email_address_lang_tokenizer *tok =
		(struct email_address_lang_tokenizer *)_tok;
	size_t pos = 0, local_skip;
	bool finished;

	if (tok->tokenizer.skip_parents == TRUE)
		tok->tokenizer.skip_parents = FALSE;

	if (tok->state == EMAIL_ADDRESS_PARSER_STATE_COMPLETE) {
		*skip_r = pos;
		if (lang_tokenizer_address_current_token(tok, token_r))
			return 1;
	}

	/* end of data, output lingering tokens. first the parents data, then
	   possibly our token, if complete enough */
	if (size == 0) {
		if (tok->state == EMAIL_ADDRESS_PARSER_STATE_DOMAIN &&
		    domain_is_empty(tok)) {
			/* user@ without domain - reset state */
			str_truncate(tok->last_word, 0);
			tok->state = EMAIL_ADDRESS_PARSER_STATE_NONE;
		}

		if (lang_tokenizer_address_parent_data(tok, token_r))
			return 1;

		if (tok->state == EMAIL_ADDRESS_PARSER_STATE_DOMAIN) {
			if (lang_tokenizer_address_current_token(tok, token_r))
				return 1;
		}
		tok->state = EMAIL_ADDRESS_PARSER_STATE_NONE;
	}

	/* 1) regular input data OR
	   2) circle around to return completed address */
	while(pos < size || tok->state == EMAIL_ADDRESS_PARSER_STATE_COMPLETE) {

		switch (tok->state) {
		case EMAIL_ADDRESS_PARSER_STATE_NONE:
			/* no part of address found yet. remove possible
			   earlier data */
			str_truncate(tok->last_word, 0);
                        if (lang_tokenizer_address_parent_data(tok, token_r)) {
                               *skip_r = pos;
                               return 1;
                        }

			/* fall through */
		case EMAIL_ADDRESS_PARSER_STATE_LOCALPART:
			/* last_word is empty or has the beginnings of a valid
			   local-part, but no '@' found yet. continue parsing
			   the beginning of data to see if it contains a full
			   local-part@ */
			tok->state =
				lang_tokenizer_email_address_parse_local(tok,
				                                        data + pos,
				                                        size - pos,
				                                        &local_skip);
			lang_tokenizer_address_update_parent(tok, data+pos,
			                                    local_skip);
			pos += local_skip;

			break;
		case EMAIL_ADDRESS_PARSER_STATE_DOMAIN:
			/* last_word has a local-part@ and maybe the beginning
			   of a domain. continue parsing the beginning of data
			   to see if it contains a valid domain. */

			tok->state =
				lang_tokenizer_email_address_parse_domain(tok,
				                                        data + pos,
				                                        size - pos,
				                                        &local_skip);
			lang_tokenizer_address_update_parent(tok, data+pos,
			                                    local_skip);
			pos += local_skip;

			break;
		case EMAIL_ADDRESS_PARSER_STATE_COMPLETE:
			*skip_r = pos;
			if (lang_tokenizer_address_parent_data(tok, token_r))
				return 1;
			if (lang_tokenizer_address_current_token(tok, token_r))
				return 1;
			break;
		case EMAIL_ADDRESS_PARSER_STATE_SKIP:
			/* The current token is too large to determine if it's
			   an email address or not. The address-tokenizer is
			   simply skipping over it, but the input is being
			   passed to the parent tokenizer. */
			*skip_r = pos;
			if (lang_tokenizer_address_parent_data(tok, token_r))
				return 1;

			finished = lang_tokenizer_address_skip(data + pos,
							      size - pos,
							      &local_skip);
			lang_tokenizer_address_update_parent(tok, data+pos,
							    local_skip);
			pos += local_skip;
			if (finished) {
				*skip_r = pos;
				if (lang_tokenizer_address_parent_data(tok, token_r)) {
					tok->state = EMAIL_ADDRESS_PARSER_STATE_NONE;
					return 1;
				}
				tok->state = EMAIL_ADDRESS_PARSER_STATE_NONE;
			}
			break;
		default:
			i_unreached();
		}

	}
	*skip_r = pos;
	return 0;
}

static const struct lang_tokenizer_vfuncs email_address_tokenizer_vfuncs = {
	lang_tokenizer_email_address_create,
	lang_tokenizer_email_address_destroy,
	lang_tokenizer_email_address_reset,
	lang_tokenizer_email_address_next
};

static const struct lang_tokenizer lang_tokenizer_email_address_real = {
	.name = "email-address",
	.v = &email_address_tokenizer_vfuncs,
	.stream_to_parents = TRUE,
};
const struct lang_tokenizer *lang_tokenizer_email_address =
	&lang_tokenizer_email_address_real;
