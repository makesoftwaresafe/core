/* Copyright (c) 2020 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "str.h"
#include "strescape.h"
#include "hmac.h"
#include "array.h"
#include "hash-method.h"
#include "istream.h"
#include "iso8601-date.h"
#include "json-tree.h"
#include "array.h"
#include "base64.h"
#include "str-sanitize.h"
#include "dcrypt.h"
#include "oauth2.h"
#include "oauth2-private.h"
#include "dict.h"

#include <time.h>

static const struct json_tree_node *
get_field_node(const struct json_tree *tree, const char *key)
{
	const struct json_tree_node *root = json_tree_get_root_const(tree);
	const struct json_tree_node *value_node;

	value_node = json_tree_node_get_member(root, key);
	if (value_node == NULL ||
	    json_tree_node_is_object(value_node) ||
	    json_tree_node_is_array(value_node))
		return NULL;

	return value_node;
}

static const char *
get_field(const struct json_tree *tree, const char *key,
	  enum json_type *type_r)
{
	const struct json_tree_node *value_node = get_field_node(tree, key);

	if (value_node == NULL)
		return NULL;
	if (type_r != NULL)
		*type_r = json_tree_node_get_type(value_node);
	return json_tree_node_as_str(value_node);
}

static const char *
get_field_multiple(const struct json_tree *tree, const char *key)
{
	const struct json_tree_node *root = json_tree_get_root_const(tree);
	const struct json_tree_node *value_node;

	value_node = json_tree_node_get_member(root, key);
	if (value_node == NULL || json_tree_node_is_object(value_node))
		return NULL;
	if (!json_tree_node_is_array(value_node))
		return json_tree_node_as_str(value_node);

	const struct json_tree_node *entry_node;

	entry_node = json_tree_node_get_child(value_node);
	string_t *values = t_str_new(64);

	for (; entry_node != NULL;
	     entry_node = json_tree_node_get_next(entry_node)) {
		if (json_tree_node_is_object(entry_node) ||
		    json_tree_node_is_array(entry_node))
		    	continue;

		const char *value_str =	json_tree_node_as_str(entry_node);

		if (str_len(values) > 0)
			str_append_c(values, '\t');
		str_append_tabescaped(values, value_str);
	}
	return str_c(values);
}

static int
get_time_field(const struct json_tree *tree, const char *key,
	       int64_t *value_r)
{
	const struct json_tree_node *value_node = get_field_node(tree, key);

	if (value_node == NULL)
		return 0;

	enum json_type value_type = json_tree_node_get_type(value_node);

	if (value_type == JSON_TYPE_NUMBER) {
		int64_t tstamp;

		if (json_tree_node_get_int64(value_node, &tstamp) < 0 ||
		    tstamp < 0)
			return -1;
		*value_r = tstamp;
		return 1;
	}

	const char *value = json_tree_node_as_str(value_node);
	time_t tvalue;
	int tz_offset ATTR_UNUSED;

	if (iso8601_date_parse((const unsigned char*)value, strlen(value),
			       &tvalue, &tz_offset)) {
		if (tvalue < 0)
			return -1;
		*value_r = tvalue;
		return 1;
	}
	return -1;
}

/* Escapes '/' and '%' in identifier to %hex */
static const char *escape_identifier(const char *identifier)
{
	size_t pos = strcspn(identifier, "/%");

	/* nothing to escape */
	if (identifier[pos] == '\0')
		return identifier;

	size_t len = strlen(identifier);
	string_t *new_id = t_str_new(len);

	str_append_data(new_id, identifier, pos);

	for (size_t i = pos; i < len; i++) {
	        switch (identifier[i]) {
	        case '/':
	                str_append(new_id, "%2f");
	                break;
	        case '%':
	                str_append(new_id, "%25");
	                break;
	        default:
	                str_append_c(new_id, identifier[i]);
	                break;
	        }
	}
	return str_c(new_id);
}

static int
oauth2_lookup_hmac_key(const struct oauth2_settings *set, const char *azp,
		       const char *alg, const char *key_id,
		       const buffer_t **hmac_key_r, const char **error_r)
{
	const char *base64_key;
	const char *cache_key_id, *lookup_key;
	int ret;

	cache_key_id = t_strconcat(azp, ".", alg, ".", key_id, NULL);
	if (oauth2_validation_key_cache_lookup_hmac_key(
		set->key_cache, cache_key_id, hmac_key_r) == 0)
		return 0;


	/* do a synchronous dict lookup */
	lookup_key = t_strconcat(DICT_PATH_SHARED, azp, "/", alg, "/", key_id,
				 NULL);
	struct dict_op_settings dict_set = {
		.username = NULL,
	};
	if ((ret = dict_lookup(set->key_dict, &dict_set, pool_datastack_create(),
			       lookup_key, &base64_key, error_r)) < 0) {
		return -1;
	} else if (ret == 0) {
		*error_r = t_strdup_printf("%s key '%s' not found",
					   alg, key_id);
		return -1;
	}

	/* decode key */
	buffer_t *key = t_base64_decode_str(base64_key);
	if (key->used == 0) {
		*error_r = "Invalid base64 encoded key";
		return -1;
	}
	oauth2_validation_key_cache_insert_hmac_key(set->key_cache,
						    cache_key_id, key);
	*hmac_key_r = key;
	return 0;
}

static int
oauth2_validate_hmac(const struct oauth2_settings *set, const char *azp,
		     const char *alg, const char *key_id,
		     const char *const *blobs, const char **error_r)
{
	const struct hash_method *method;

	if (strcmp(alg, "HS256") == 0)
		method = hash_method_lookup("sha256");
	else if (strcmp(alg, "HS384") == 0)
		method = hash_method_lookup("sha384");
	else if (strcmp(alg, "HS512") == 0)
		method = hash_method_lookup("sha512");
	else {
		*error_r = t_strdup_printf("unsupported algorithm '%s'", alg);
		return -1;
	}

	const buffer_t *key;

	if (oauth2_lookup_hmac_key(set, azp, alg, key_id, &key, error_r) < 0)
		return -1;

	struct hmac_context ctx;
	unsigned char digest[method->digest_size];

	hmac_init(&ctx, key->data, key->used, method);
	hmac_update(&ctx, blobs[0], strlen(blobs[0]));
	hmac_update(&ctx, ".", 1);
	hmac_update(&ctx, blobs[1], strlen(blobs[1]));
	hmac_final(&ctx, digest);

	buffer_t *their_digest =
		t_base64url_decode_str(BASE64_DECODE_FLAG_NO_PADDING, blobs[2]);
	if (method->digest_size != their_digest->used ||
	    !mem_equals_timing_safe(digest, their_digest->data,
				    method->digest_size)) {
		*error_r = "Incorrect JWT signature";
		return -1;
	}
	return 0;
}

static int
oauth2_lookup_pubkey(const struct oauth2_settings *set, const char *azp,
		     const char *alg, const char *key_id,
		     struct dcrypt_public_key **key_r, const char **error_r)
{
	const char *key_str;
	const char *cache_key_id, *lookup_key;
	int ret;

	cache_key_id = t_strconcat(azp, ".", alg, ".", key_id, NULL);
	if (oauth2_validation_key_cache_lookup_pubkey(
		set->key_cache, cache_key_id, key_r) == 0)
		return 0;

	/* do a synchronous dict lookup */
	lookup_key = t_strconcat(DICT_PATH_SHARED, azp, "/", alg, "/", key_id,
				 NULL);
	struct dict_op_settings dict_set = {
		.username = NULL,
	};
	if ((ret = dict_lookup(set->key_dict, &dict_set, pool_datastack_create(),
			       lookup_key, &key_str, error_r)) < 0) {
		return -1;
	} else if (ret == 0) {
		*error_r = t_strdup_printf("%s key '%s' not found",
					   alg, key_id);
		return -1;
	}

	/* try to load key */
	struct dcrypt_public_key *pubkey;
	const char *error;

	if (!dcrypt_key_load_public(&pubkey, key_str, &error)) {
		*error_r = t_strdup_printf("Cannot load key: %s", error);
		return -1;
	}

	/* cache key */
	oauth2_validation_key_cache_insert_pubkey(set->key_cache, cache_key_id,
						  pubkey);
	*key_r = pubkey;
	return 0;
}

static int
oauth2_validate_rsa_ecdsa(const struct oauth2_settings *set,
			  const char *azp, const char *alg, const char *key_id,
			  const char *const *blobs, const char **error_r)
{
	const char *method;
	enum dcrypt_padding padding;
	enum dcrypt_signature_format sig_format;

	if (!dcrypt_is_initialized()) {
		*error_r = "No crypto library loaded";
		return -1;
	}

	if (str_begins_with(alg, "RS")) {
		padding = DCRYPT_PADDING_RSA_PKCS1;
		sig_format = DCRYPT_SIGNATURE_FORMAT_DSS;
	} else if (str_begins_with(alg, "PS")) {
		padding = DCRYPT_PADDING_RSA_PKCS1_PSS;
		sig_format = DCRYPT_SIGNATURE_FORMAT_DSS;
	} else if (str_begins_with(alg, "ES")) {
		padding = DCRYPT_PADDING_DEFAULT;
		sig_format = DCRYPT_SIGNATURE_FORMAT_X962;
	} else {
		/* this should be checked by caller */
		i_unreached();
	}

	if (strcmp(alg+2, "256") == 0 ||
	    strcmp(alg+2, "256K") == 0) {
		method = "sha256";
	} else if (strcmp(alg+2, "384") == 0) {
		method = "sha384";
	} else if (strcmp(alg+2, "512") == 0) {
		method = "sha512";
	} else {
		*error_r = t_strdup_printf("Unsupported algorithm '%s'", alg);
		return -1;
	}

	buffer_t *signature =
		t_base64url_decode_str(BASE64_DECODE_FLAG_NO_PADDING, blobs[2]);

	struct dcrypt_public_key *pubkey;
	if (oauth2_lookup_pubkey(set, azp, alg, key_id, &pubkey, error_r) < 0)
		return -1;

	/* data to verify */
	const char *data = t_strconcat(blobs[0], ".", blobs[1], NULL);

	/* verify signature */
	bool valid;
	if (!dcrypt_verify(pubkey, method, sig_format, data, strlen(data),
			   signature->data, signature->used, &valid, padding,
			   error_r)) {
		valid = FALSE;
	} else if (!valid) {
		*error_r = "Bad signature";
	}

	return valid ? 0 : -1;
}

static int
oauth2_validate_signature(const struct oauth2_settings *set, const char *azp,
			  const char *alg, const char *key_id,
			  const char *const *blobs, const char **error_r)
{
	if (str_begins_with(alg, "HS")) {
		return oauth2_validate_hmac(set, azp, alg, key_id, blobs,
					    error_r);
	} else if (str_begins_with(alg, "RS") ||
		   str_begins_with(alg, "PS") ||
		   str_begins_with(alg, "ES")) {
		return oauth2_validate_rsa_ecdsa(set, azp, alg, key_id, blobs,
						 error_r);
	}

	*error_r = t_strdup_printf("Unsupported algorithm '%s'", alg);
	return -1;
}

struct jwt_node {
	const char *prefix;
	const struct json_tree_node *root;
	bool array:1;
};

static void
oauth2_jwt_copy_fields(ARRAY_TYPE(oauth2_field) *fields,
		       struct json_tree *tree)
{
	pool_t pool = array_get_pool(fields);
	ARRAY(struct jwt_node) nodes;

	t_array_init(&nodes, 1);

	struct jwt_node *root = array_append_space(&nodes);

	root->prefix = "";
	root->root = json_tree_get_root_const(tree);

	while (array_count(&nodes) > 0) {
		const struct jwt_node *subroot = array_front(&nodes);
		const struct json_tree_node *tnode = subroot->root;

		while (tnode != NULL) {
			const struct json_node *jnode =
				json_tree_node_get(tnode);

			if (!json_node_is_singular(jnode)) {
				root = array_append_space(&nodes);
				root->root = json_tree_node_get_child(tnode);
				root->array = json_node_is_array(jnode);
				if (jnode->name == NULL)
					root->prefix = subroot->prefix;
				else if (*subroot->prefix != '\0')
					root->prefix = t_strconcat(subroot->prefix, jnode->name, "_", NULL);
				else
					root->prefix = t_strconcat(jnode->name, "_", NULL);
			} else {
				struct oauth2_field *field;
				const char *name;

				if (subroot->array) {
					name = strrchr(subroot->prefix, '_');
					if (name != NULL)
						name = t_strdup_until(subroot->prefix, name);
					else
						name = subroot->prefix;
					array_foreach_modifiable(fields, field) {
						if (strcmp(field->name, name) == 0)
							break;
					}
					if (field == NULL || field->name == NULL) {
						field = array_append_space(fields);
						field->name = p_strdup(pool, name);
					}
				} else {
					field = array_append_space(fields);
					field->name = p_strconcat(pool, subroot->prefix, jnode->name, NULL);
				}

				const char *value = str_tabescape(json_node_as_str(jnode));

				if (field->value != NULL) {
					field->value = p_strconcat(pool, field->value, "\t", value, NULL);
				} else {
					field->value = p_strdup(pool, value);
				}
			}
			tnode = json_tree_node_get_next(tnode);
		}
		array_pop_front(&nodes);
	}
}

static int
oauth2_jwt_header_process(struct json_tree *tree, const char **alg_r,
			  const char **kid_r, const char **error_r)
{
	const char *alg = get_field(tree, "alg", NULL);
	const char *kid = get_field(tree, "kid", NULL);

	if (alg == NULL) {
		*error_r = "Cannot find 'alg' field";
		return -1;
	}

	/* These are lost when tree is deinitialized.
	   Make sure algorithm is uppercased. */
	*alg_r = t_str_ucase(alg);
	*kid_r = t_strdup(kid);
	return 0;
}

static bool check_scope(const char *req, const char *got)
{
	const char *const *scope_req = t_strsplit_spaces(req, " ,");
	const char *const *scope_got = t_strsplit_spaces(got, " ,");

	for (; *scope_req != NULL; scope_req++)
		if (!str_array_icase_find(scope_got, *scope_req))
			return FALSE;
	return TRUE;
}

static int
oauth2_jwt_body_process(const struct oauth2_settings *set,
			const char *alg, const char *kid,
			ARRAY_TYPE(oauth2_field) *fields,
			struct json_tree *tree,
			const char *const *blobs, const char **error_r)
{
	const char *sub = get_field(tree, "sub", NULL);
	int64_t t0 = time(NULL);
	/* default IAT and NBF to now */
	int64_t iat, nbf, exp;
	int tz_offset ATTR_UNUSED;
	int ret;

	if (sub == NULL) {
		*error_r = "Missing 'sub' field";
		return -1;
	}

	if ((ret = get_time_field(tree, "exp", &exp)) < 1) {
		*error_r = t_strdup_printf("%s 'exp' field",
				ret == 0 ? "Missing" : "Malformed");
		return -1;
	}

	if ((ret = get_time_field(tree, "nbf", &nbf)) < 0) {
		*error_r = "Malformed 'nbf' field";
		return -1;
	} else if (ret == 0 || nbf == 0)
		nbf = t0;

	if ((ret = get_time_field(tree, "iat", &iat)) < 0) {
		*error_r = "Malformed 'iat' field";
		return -1;
	} else if (ret == 0 || iat == 0)
		iat = t0;

	if (nbf > t0) {
		*error_r = "Token is not valid yet";
		return -1;
	}
	if (iat > t0) {
		*error_r = "Token is issued in future";
		return -1;
	}
	if (exp < t0) {
		*error_r = "Token has expired";
		return -1;
	}

	/* ensure token dates are not conflicting */
	if (exp < iat ||
	    exp < nbf) {
		*error_r = "Token time values are conflicting";
		return -1;
	}

	const char *iss = get_field(tree, "iss", NULL);

	if (set->issuers != NULL && *set->issuers != NULL) {
		if (iss == NULL) {
			*error_r = "Token is missing 'iss' field";
			return -1;
		}
		if (!str_array_find(set->issuers, iss)) {
			*error_r = t_strdup_printf("Issuer '%s' is not allowed",
						   str_sanitize_utf8(iss, 128));
			return -1;
		}
	}

	const char *aud = get_field_multiple(tree, "aud");

	/* if there is client_id configured, then aud should be present */
	if (set->client_id != NULL && *set->client_id != '\0') {
		if (aud == NULL) {
			*error_r = "client_id set but aud is missing";
			return -1;

		}
		const char *const *auds = t_strsplit_tabescaped(aud);
		if (!str_array_find(auds, set->client_id)) {
			*error_r = "client_id not found in aud field";
			return -1;
		}
	}

	const char *got_scope = get_field(tree, "scope", NULL);
	const char *req_scope = set->scope;

	if (req_scope != NULL && *req_scope != '\0') {
		if (got_scope == NULL) {
			*error_r = "scope set but not found in token";
			return -1;
		}

		if (!check_scope(req_scope, got_scope)) {
			*error_r = t_strdup_printf(
				"configured scope '%s' missing from token scope '%s'",
				req_scope, got_scope);
			return -1;
		}
	}

	/* see if there is azp */
	const char *azp = get_field(tree, "azp", NULL);
	if (azp == NULL)
		azp = "default";
	else
		azp = escape_identifier(azp);

	if (oauth2_validate_signature(set, azp, alg, kid, blobs, error_r) < 0)
		return -1;

	oauth2_jwt_copy_fields(fields, tree);
	return 0;
}

int oauth2_try_parse_jwt(const struct oauth2_settings *set,
			 const char *token, ARRAY_TYPE(oauth2_field) *fields,
			 bool *is_jwt_r, const char **error_r)
{
	const char *const *blobs = t_strsplit(token, ".");
	int ret;

	i_assert(set->key_dict != NULL);

	/* we don't know if it's JWT token yet */
	*is_jwt_r = FALSE;

	if (str_array_length(blobs) != 3) {
		*error_r = "Not a JWT token";
		return -1;
	}

	/* attempt to decode header */
	buffer_t *header =
		t_base64url_decode_str(BASE64_DECODE_FLAG_NO_PADDING, blobs[0]);

	if (header->used == 0) {
		*error_r = "Not a JWT token";
		return -1;
	}

	struct json_tree *header_tree;
	if (oauth2_json_tree_build(header, &header_tree, error_r) < 0)
		return -1;

	const char *alg, *kid;
	ret = oauth2_jwt_header_process(header_tree, &alg, &kid, error_r);
	json_tree_unref(&header_tree);
	if (ret < 0)
		return -1;

	/* it is now assumed to be a JWT token */
	*is_jwt_r = TRUE;

	if (kid == NULL)
		kid = "default";
	else if (*kid == '\0') {
		*error_r = "'kid' field is empty";
		return -1;
	} else {
		kid = escape_identifier(kid);
	}

	/* parse body */
	struct json_tree *body_tree;
	buffer_t *body =
		t_base64url_decode_str(BASE64_DECODE_FLAG_NO_PADDING, blobs[1]);
	if (oauth2_json_tree_build(body, &body_tree, error_r) == -1)
		return -1;
	ret = oauth2_jwt_body_process(set, alg, kid, fields, body_tree, blobs,
				      error_r);
	json_tree_unref(&body_tree);

	return ret;
}
