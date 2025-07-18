/* Copyright (c) 2002-2018 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "ioloop.h"
#include "buffer.h"
#include "hash.h"
#include "sha1.h"
#include "hex-binary.h"
#include "str.h"
#include "array.h"
#include "safe-memset.h"
#include "str-sanitize.h"
#include "strescape.h"
#include "dns-lookup.h"
#include "hostpid.h"
#include "settings.h"
#include "master-service.h"
#include "auth-cache.h"
#include "auth-request.h"
#include "auth-request-handler.h"
#include "auth-request-handler-private.h"
#include "auth-client-connection.h"
#include "auth-master-connection.h"
#include "auth-policy.h"
#include "passdb.h"
#include "passdb-blocking.h"
#include "passdb-cache.h"
#include "userdb-blocking.h"
#include "password-scheme.h"
#include "wildcard-match.h"

#include <sys/stat.h>

#define AUTH_DNS_WARN_MSECS 500
#define AUTH_REQUEST_MAX_DELAY_SECS (60*5)
#define CACHED_PASSWORD_SCHEME "SHA1"

struct auth_request_proxy_dns_lookup_ctx {
	struct auth_request *request;
	struct event *event;
	auth_request_proxy_cb_t *callback;
	struct dns_lookup *dns_lookup;
};

struct auth_policy_check_ctx {
	enum {
		AUTH_POLICY_CHECK_TYPE_PLAIN,
		AUTH_POLICY_CHECK_TYPE_LOOKUP,
		AUTH_POLICY_CHECK_TYPE_SUCCESS,
	} type;
	struct auth_request *request;

	buffer_t *success_data;

	verify_plain_callback_t *callback_plain;
	lookup_credentials_callback_t *callback_lookup;
};

unsigned int auth_request_state_count[AUTH_REQUEST_STATE_MAX];

static void
auth_request_userdb_import(struct auth_request *request, const char *args);

static void auth_request_lookup_credentials_policy_continue(
	struct auth_request *request, lookup_credentials_callback_t *callback);
static void auth_request_policy_check_callback(int result, void *context);

#define MAX_LOG_USERNAME_LEN 64
static const char *get_log_prefix(struct auth_request *auth_request)
{
	string_t *str = t_str_new(64);
	const char *ip;

	str_append(str, master_service_get_configured_name(master_service));
	str_append_c(str, '(');

	if (auth_request->fields.user == NULL)
	        str_append(str, "?");
	else
		str_sanitize_append(str, auth_request->fields.user,
				    MAX_LOG_USERNAME_LEN);

	ip = net_ip2addr(&auth_request->fields.remote_ip);
	if (ip[0] != '\0') {
	        str_append_c(str, ',');
	        str_append(str, ip);
	}
	if (auth_request->mech != NULL) {
		str_append(str, ",sasl:");
		str_append(str, t_str_lcase(auth_request->mech->mech_name));
	}
	if (auth_request->fields.requested_login_user != NULL)
	        str_append(str, ",master");
	str_append_c(str, ')');
	if (worker) {
		str_append(str, "<");
		str_append(str, my_pid);
		str_append(str,">");
	}
	if (auth_request->fields.session_id != NULL)
	        str_printfa(str, "<%s>", auth_request->fields.session_id);
	if (worker)
		str_printfa(str, ": request [%u]", auth_request->id);
	str_append(str, ": ");

	return str_c(str);
}

static const char *
auth_request_get_log_prefix_db(struct auth_request *auth_request)
{
	const char *name;

	if (!auth_request->userdb_lookup) {
		i_assert(auth_request->passdb != NULL);
		name = auth_request->passdb->name;
	} else {
		i_assert(auth_request->userdb != NULL);
		name = auth_request->userdb->name;
	}

	return t_strconcat(name, ": ", NULL);
}

static void
auth_request_post_alloc_init(struct auth_request *request,
			     struct event *parent_event)
{
	enum log_type level;

	request->state = AUTH_REQUEST_STATE_NEW;
	auth_request_state_count[AUTH_REQUEST_STATE_NEW]++;
	request->refcount = 1;
	request->last_access = ioloop_time;
	request->session_pid = (pid_t)-1;
	request->set = global_auth_settings;
	request->protocol_set = global_auth_settings;
	request->event = event_create(parent_event);
	auth_request_fields_init(request);

	level = request->set->verbose ? LOG_TYPE_INFO : LOG_TYPE_WARNING;
	event_set_min_log_level(request->event, level);
	event_set_forced_debug(request->event, request->set->debug);
	event_add_category(request->event, &event_category_auth);
	event_set_log_prefix_callback(request->event, TRUE,
				      get_log_prefix, request);
	auth_request_event_set_var_expand(request);

	p_array_init(&request->authdb_event, request->pool, 2);
}

struct auth_request *
auth_request_new(const struct mech_module *mech, struct event *parent_event)
{
	struct auth_request *request;

	request = mech->auth_new();
	request->mech = mech;
	auth_request_post_alloc_init(request, parent_event);

	enum log_type level =
		(request->set->verbose ? LOG_TYPE_INFO : LOG_TYPE_WARNING);
	const char *prefix = t_strconcat(
		t_str_lcase(request->mech->mech_name), ": ", NULL);

	request->mech_event = event_create(request->event);
	event_set_min_log_level(request->mech_event, level);
	event_set_append_log_prefix(request->mech_event, prefix);

	return request;
}

struct auth_request *auth_request_new_dummy(struct event *parent_event)
{
	struct auth_request *request;
	pool_t pool;

	pool = pool_alloconly_create(MEMPOOL_GROWING"auth_request", 1024);
	request = p_new(pool, struct auth_request, 1);
	request->pool = pool;

	auth_request_post_alloc_init(request, parent_event);
	return request;
}

void auth_request_set_state(struct auth_request *request,
			    enum auth_request_state state)
{
	if (request->state == state)
		return;

	i_assert(request->to_penalty == NULL);

	i_assert(auth_request_state_count[request->state] > 0);
	auth_request_state_count[request->state]--;
	auth_request_state_count[state]++;

	request->state = state;
	auth_refresh_proctitle();
}

void auth_request_init(struct auth_request *request)
{
	struct auth *auth;

	auth = auth_request_get_auth(request);
	request->set = auth->protocol_set;
	request->protocol_set = auth->protocol_set;
	request->passdb = auth->passdbs;
	request->userdb = auth->userdbs;
}

struct auth *auth_request_get_auth(struct auth_request *request)
{
	return auth_find_protocol(request->fields.protocol);
}

void auth_request_success(struct auth_request *request,
			  const void *data, size_t data_size)
{
	i_assert(request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);

	/* preserve userdb fields set by mechanisms that don't use a passdb */
	if (request->fields.userdb_reply != NULL)
		auth_fields_snapshot(request->fields.userdb_reply);

	struct auth_policy_check_ctx *ctx;

	ctx = p_new(request->pool, struct auth_policy_check_ctx, 1);
	ctx->request = request;
	ctx->success_data = buffer_create_dynamic(request->pool, data_size);
	buffer_append(ctx->success_data, data, data_size);
	ctx->type = AUTH_POLICY_CHECK_TYPE_SUCCESS;

	if (!request->set->policy_check_after_auth) {
		auth_request_policy_check_callback(0, ctx);
		return;
	}

	/* perform second policy lookup here */
	auth_policy_check(request, request->mech_password,
			  auth_request_policy_check_callback, ctx);
}

struct event_passthrough *
auth_request_finished_event(struct auth_request *request, struct event *event)
{
	struct event_passthrough *e = event_create_passthrough(event);

	if (request->failed) {
		if (request->internal_failure) {
			e->add_str("error", "internal failure");
		} else {
			e->add_str("error", "authentication failed");
		}
	} else if (request->fields.successful) {
		e->add_str("success", "yes");
	}
	if (request->userdb_lookup) {
		return e;
	}
	if (request->policy_penalty > 0)
		e->add_int("policy_penalty", request->policy_penalty);
	if (request->policy_refusal) {
		e->add_str("policy_result", "refused");
	} else if (request->policy_processed && request->policy_penalty > 0) {
		e->add_str("policy_result", "delayed");
		e->add_int("policy_penalty", request->policy_penalty);
	} else if (request->policy_processed) {
		e->add_str("policy_result", "ok");
	}
	return e;
}

void auth_request_log_finished(struct auth_request *request)
{
	if (request->event_finished_sent)
		return;
	request->event_finished_sent = TRUE;
	struct event_passthrough *e =
		auth_request_finished_event(request, request->event)->
		set_name("auth_request_finished");
	e_debug(e->event(), "Auth request finished");
}

static void auth_request_success_continue(struct auth_policy_check_ctx *ctx)
{
	struct auth_request *request = ctx->request;
	i_assert(request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);

	timeout_remove(&request->to_penalty);

	if (request->failed || !request->passdb_success) {
		/* password was valid, but some other check failed. */
		auth_request_fail(request);
		return;
	}
	auth_request_set_auth_successful(request);

	/* log before delay */
	auth_request_log_finished(request);

	if (request->delay_until > ioloop_time) {
		unsigned int delay_secs = request->delay_until - ioloop_time;
		request->to_penalty = timeout_add(delay_secs * 1000,
			auth_request_success_continue, ctx);
		return;
	}

	if (ctx->success_data->used > 0 && !request->fields.final_resp_ok) {
		/* we'll need one more SASL round, since client doesn't support
		   the final SASL response */
		i_assert(!request->final_resp_sent);
		request->final_resp_sent = TRUE;
		auth_request_handler_reply_continue(request,
			ctx->success_data->data, ctx->success_data->used);
		return;
	}

	auth_request_set_state(request, AUTH_REQUEST_STATE_FINISHED);
	auth_request_refresh_last_access(request);
	auth_request_handler_reply(request, AUTH_CLIENT_RESULT_SUCCESS,
		ctx->success_data->data, ctx->success_data->used);
}

void auth_request_fail_with_reply(struct auth_request *request,
				  const void *final_data, size_t final_data_size)
{
	i_assert(request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);

	/* Sending challenge data (final_data_size > 0) as part of the final
	   authentication command response is never an option in SASL, but when
	   the authentication client sets the "final-resp-ok" field it indicates
	   that it will handle the final protocol sequence, avoiding the need to
	   do that here. */
	if (final_data_size > 0 && !request->fields.final_resp_ok) {
		/* Otherwise, we need to send the data as part of a normal
		   challenge and wait for a dummy client response. */
		i_assert(!request->final_resp_sent);
		request->final_resp_sent = TRUE;
		auth_request_handler_reply_continue(request, final_data,
						    final_data_size);
		return;
	}

	auth_request_set_state(request, AUTH_REQUEST_STATE_FINISHED);
	auth_request_refresh_last_access(request);
	auth_request_log_finished(request);
	auth_request_handler_reply(request, AUTH_CLIENT_RESULT_FAILURE,
			           final_data, final_data_size);
}

void auth_request_fail(struct auth_request *request)
{
	auth_request_fail_with_reply(request, "", 0);
}

void auth_request_internal_failure(struct auth_request *request)
{
	request->internal_failure = TRUE;
	auth_request_fail(request);
}

void auth_request_ref(struct auth_request *request)
{
	request->refcount++;
}

void auth_request_unref(struct auth_request **_request)
{
	struct auth_request *request = *_request;

	*_request = NULL;
	i_assert(request->refcount > 0);
	if (--request->refcount > 0)
		return;

	i_assert(array_count(&request->authdb_event) == 0);

	if (request->handler_pending_reply)
		auth_request_handler_abort(request);

	event_unref(&request->mech_event);
	event_unref(&request->event);
	auth_request_state_count[request->state]--;
	auth_refresh_proctitle();

	if (request->mech_password != NULL) {
		safe_memset(request->mech_password, 0,
			    strlen(request->mech_password));
	}

	if (request->dns_lookup_ctx != NULL)
		dns_lookup_abort(&request->dns_lookup_ctx->dns_lookup);
	timeout_remove(&request->to_abort);
	timeout_remove(&request->to_penalty);

	if (request->mech != NULL)
		request->mech->auth_free(request);
	else
		pool_unref(&request->pool);
}

bool auth_request_import_master(struct auth_request *request,
				const char *key, const char *value)
{
	pid_t pid;

	i_assert(value != NULL);

	/* master request lookups may set these */
	if (strcmp(key, "session_pid") == 0) {
		if (str_to_pid(value, &pid) == 0)
			request->session_pid = pid;
	} else if (strcmp(key, "request_auth_token") == 0)
		request->request_auth_token = TRUE;
	else
		return FALSE;
	return TRUE;
}

static bool
auth_request_fail_on_nuls(struct auth_request *request,
			  const unsigned char *data, size_t data_size)
{
	if ((request->mech->flags & MECH_SEC_ALLOW_NULS) != 0)
		return FALSE;
	if (memchr(data, '\0', data_size) != NULL) {
		e_debug(request->mech_event, "Unexpected NUL in auth data");
		auth_request_fail(request);
		return TRUE;
	}
	return FALSE;
}

void auth_request_initial(struct auth_request *request)
{
	i_assert(request->state == AUTH_REQUEST_STATE_NEW);

	auth_request_set_state(request, AUTH_REQUEST_STATE_MECH_CONTINUE);

	if (auth_request_fail_on_nuls(request, request->initial_response,
				      request->initial_response_len))
		return;

	request->mech->auth_initial(request, request->initial_response,
				    request->initial_response_len);
}

void auth_request_continue(struct auth_request *request,
			   const unsigned char *data, size_t data_size)
{
	i_assert(request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);

	if (request->final_resp_sent) {
		if (!request->fields.successful) {
			auth_request_fail(request);
			return;
		}
		auth_request_success(request, "", 0);
		return;
	}

	if (auth_request_fail_on_nuls(request, data, data_size))
		return;

	auth_request_refresh_last_access(request);
	request->mech->auth_continue(request, data, data_size);
}

static void
auth_request_save_cache(struct auth_request *request,
			enum passdb_result result)
{
	struct auth_passdb *passdb = request->passdb;
	const char *encoded_password;
	string_t *str;
	struct password_generate_params gen_params = {
		.user = request->fields.user,
		.rounds = 0
	};

	switch (result) {
	case PASSDB_RESULT_USER_UNKNOWN:
	case PASSDB_RESULT_PASSWORD_MISMATCH:
	case PASSDB_RESULT_OK:
	case PASSDB_RESULT_SCHEME_NOT_AVAILABLE:
		/* can be cached */
		break;
	case PASSDB_RESULT_NEXT:
	case PASSDB_RESULT_USER_DISABLED:
	case PASSDB_RESULT_PASS_EXPIRED:
		/* FIXME: we can't cache this now, or cache lookup would
		   return success. */
		return;
	case PASSDB_RESULT_INTERNAL_FAILURE:
		i_unreached();
	}

	if (passdb_cache == NULL || passdb->cache_key == NULL)
		return;

	if (result < 0) {
		/* lookup failed. */
		if (result == PASSDB_RESULT_USER_UNKNOWN) {
			auth_cache_insert(passdb_cache, request,
					  passdb->cache_key, "", FALSE);
		}
		return;
	}

	if (request->passdb_password == NULL &&
	    !auth_fields_exists(request->fields.extra_fields, "nopassword")) {
		/* passdb didn't provide the correct password */
		if (result != PASSDB_RESULT_OK ||
		    request->mech_password == NULL)
			return;

		/* we can still cache valid password lookups though.
		   strdup() it so that mech_password doesn't get
		   cleared too early. */
		if (!password_generate_encoded(request->mech_password,
					       &gen_params,
					       CACHED_PASSWORD_SCHEME,
					       &encoded_password))
			i_unreached();
		request->passdb_password =
			p_strconcat(request->pool, "{"CACHED_PASSWORD_SCHEME"}",
				    encoded_password, NULL);
	}

	/* save all except the currently given password in cache */
	str = t_str_new(256);
	if (request->passdb_password != NULL) {
		if (*request->passdb_password != '{') {
			/* cached passwords must have a known scheme */
			str_append_c(str, '{');
			str_append(str, passdb->passdb->default_pass_scheme);
			str_append_c(str, '}');
		}
		str_append_tabescaped(str, request->passdb_password);
	}

	/* add only those extra fields to cache that were set by this passdb
	   lookup. the CHANGED flag does this, because we snapshotted the
	   extra_fields before the current passdb lookup. */
	auth_fields_append(request->fields.extra_fields, str,
			   AUTH_FIELD_FLAG_CHANGED, AUTH_FIELD_FLAG_CHANGED,
			   TRUE);
	auth_cache_insert(passdb_cache, request, passdb->cache_key, str_c(str),
			  result == PASSDB_RESULT_OK);
}

static bool
auth_request_mechanism_accepted(const char *const *mechs,
				const struct mech_module *mech)
{
	/* no filter specified, anything goes */
	if (mechs == NULL) return TRUE;
	/* request has no mechanism, see if lookup is accepted */
	if (mech == NULL)
		return str_array_icase_find(mechs, "lookup");
	/* check if request mechanism is accepted */
	return str_array_icase_find(mechs, mech->mech_name);
}

/**

Check if username is included in the filter. Logic is that if the username
is not excluded by anything, and is included by something, it will be accepted.
By default, all usernames are included, unless there is a inclusion item, when
username will be excluded if there is no inclusion for it.

Exclusions are denoted with a ! in front of the pattern.
*/
bool auth_request_username_accepted(const char *const *filter,
				    const char *username)
{
	bool have_includes = FALSE;
	bool matched_inc = FALSE;

	for(;*filter != NULL; filter++) {
		/* if filter has ! it means the pattern will be refused */
		bool exclude = (**filter == '!');
		if (!exclude)
			have_includes = TRUE;
		if (wildcard_match(username, (*filter)+(exclude?1:0))) {
			if (exclude) {
				return FALSE;
			} else {
				matched_inc = TRUE;
			}
		}
	}

	return matched_inc || !have_includes;
}

static bool
auth_request_want_skip_passdb(struct auth_request *request,
			      struct auth_passdb *passdb)
{
	/* if mechanism is not supported, skip */
	const char *const *mechs = passdb->mechanisms_filter;
	const char *const *username_filter = passdb->username_filter;
	const char *username;

	username = request->fields.user;

	if (!auth_request_mechanism_accepted(mechs, request->mech)) {
		e_debug(request->event, "skipping passdb: mechanism filtered");
		return TRUE;
	}

	if (passdb->username_filter != NULL &&
	    !auth_request_username_accepted(username_filter, username)) {
		e_debug(request->event, "skipping passdb: username filtered");
		return TRUE;
	}

	/* skip_password_check basically specifies if authentication is
	   finished */
	bool authenticated = request->fields.skip_password_check;

	switch (passdb->skip) {
	case AUTH_PASSDB_SKIP_NEVER:
		return FALSE;
	case AUTH_PASSDB_SKIP_AUTHENTICATED:
		return authenticated;
	case AUTH_PASSDB_SKIP_UNAUTHENTICATED:
		return !authenticated;
	}
	i_unreached();
}

static bool
auth_request_want_skip_userdb(struct auth_request *request,
			      struct auth_userdb *userdb)
{
	switch (userdb->skip) {
	case AUTH_USERDB_SKIP_NEVER:
		return FALSE;
	case AUTH_USERDB_SKIP_FOUND:
		return request->userdb_success;
	case AUTH_USERDB_SKIP_NOTFOUND:
		return !request->userdb_success;
	}
	i_unreached();
}

static const char *
auth_request_cache_result_to_str(enum auth_request_cache_result result)
{
	switch(result) {
	case AUTH_REQUEST_CACHE_NONE:
		return "none";
	case AUTH_REQUEST_CACHE_HIT:
		return "hit";
	case AUTH_REQUEST_CACHE_MISS:
		return "miss";
	default:
		i_unreached();
	}
}

void auth_request_passdb_lookup_begin(struct auth_request *request)
{
	struct event *event;

	i_assert(request->passdb != NULL);
	i_assert(!request->userdb_lookup);

	request->passdb_cache_result = AUTH_REQUEST_CACHE_NONE;

	/* use passdb-specific settings during the passdb lookup */
	request->set = request->passdb->auth_set;

	event = event_create(request->event);
	event_add_str(event, "passdb", request->passdb->name);
	event_add_str(event, "passdb_id", dec2str(request->passdb->passdb->id));
	const char *passdb_driver = request->passdb->passdb->iface.name;
	event_add_str(event, "passdb_driver", passdb_driver);
	settings_event_add_filter_name(event,
		t_strconcat("passdb_", passdb_driver, NULL));
	settings_event_add_list_filter_name(event, "passdb",
					    request->passdb->name);
	event_set_log_prefix_callback(event, FALSE,
		auth_request_get_log_prefix_db, request);

	/* check if we should enable verbose logging here */
	event_set_min_log_level(event, request->passdb->auth_set->verbose ?
				LOG_TYPE_INFO : LOG_TYPE_WARNING);

	e_debug(event_create_passthrough(event)->
			set_name("auth_passdb_request_started")->
			event(),
		"Performing passdb lookup");
	array_push_back(&request->authdb_event, &event);
}

void auth_request_passdb_lookup_end(struct auth_request *request,
				    enum passdb_result result)
{
	i_assert(array_count(&request->authdb_event) > 0);

	/* If client certificates are required, ensure that something
	   checked the certificate, either it was valid due to CA checks
	   or certificate fingerprint checks. */
	if (result == PASSDB_RESULT_OK &&
	    request->set->ssl_require_client_cert &&
	    !request->fields.valid_client_cert) {
		const char *reply = "Client didn't present valid SSL certificate";
		request->failed = TRUE;
		auth_request_set_field(request, "reason", reply, STATIC_PASS_SCHEME);
		result = PASSDB_RESULT_PASSWORD_MISMATCH;
	}

	struct event *event = authdb_event(request);
	struct event_passthrough *e =
		event_create_passthrough(event)->
		set_name("auth_passdb_request_finished")->
		add_str("result", passdb_result_to_string(result));
	if (request->passdb_cache_result != AUTH_REQUEST_CACHE_NONE &&
	    request->set->cache_ttl != 0 && request->set->cache_size != 0) {
		e->add_str("cache", auth_request_cache_result_to_str(
					request->passdb_cache_result));
	}
	e_debug(e->event(), "Finished passdb lookup");
	event_unref(&event);
	array_pop_back(&request->authdb_event);

	/* restore protocol-specific settings */
	request->set = request->protocol_set;
}

void auth_request_userdb_lookup_begin(struct auth_request *request)
{
	struct event *event;

	i_assert(request->userdb != NULL);
	i_assert(request->userdb_lookup);

	request->userdb_cache_result = AUTH_REQUEST_CACHE_NONE;

	/* use userdb-specific settings during the userdb lookup */
	request->set = request->userdb->auth_set;

	event = event_create(request->event);
	event_add_str(event, "userdb", request->userdb->name);
	event_add_str(event, "userdb_id", dec2str(request->userdb->userdb->id));
	const char *userdb_driver = request->userdb->userdb->iface->name;
	event_add_str(event, "userdb_driver", userdb_driver);
	settings_event_add_filter_name(event,
		t_strconcat("userdb_", userdb_driver, NULL));
	settings_event_add_list_filter_name(event, "userdb",
					    request->userdb->name);
	event_set_log_prefix_callback(event, FALSE,
		auth_request_get_log_prefix_db, request);

	/* check if we should enable verbose logging here*/
	event_set_min_log_level(event, request->userdb->auth_set->verbose ?
				LOG_TYPE_INFO : LOG_TYPE_WARNING);

	e_debug(event_create_passthrough(event)->
			set_name("auth_userdb_request_started")->
			event(),
		"Performing userdb lookup");
	array_push_back(&request->authdb_event, &event);
}

void auth_request_userdb_lookup_end(struct auth_request *request,
				    enum userdb_result result)
{
	i_assert(array_count(&request->authdb_event) > 0);
	struct event *event = authdb_event(request);
	struct event_passthrough *e =
		event_create_passthrough(event)->
		set_name("auth_userdb_request_finished")->
		add_str("result", userdb_result_to_string(result));
	if (request->userdb_cache_result != AUTH_REQUEST_CACHE_NONE &&
	    request->set->cache_ttl != 0 && request->set->cache_size != 0) {
		e->add_str("cache", auth_request_cache_result_to_str(
					request->userdb_cache_result));
	}
	e_debug(e->event(), "Finished userdb lookup");
	event_unref(&event);
	array_pop_back(&request->authdb_event);

	/* restore protocol-specific settings */
	request->set = request->protocol_set;
}

static unsigned int
auth_request_get_internal_failure_delay(struct auth_request *request)
{
	if (shutting_down)
		return 0;

	unsigned int delay_msecs = request->set->internal_failure_delay;

	/* add 0..50% random delay to avoid thundering herd problems */
	return delay_msecs +
		(delay_msecs < 2 ? 0 : i_rand_limit(delay_msecs / 2));
}

static void auth_request_passdb_internal_failure(struct auth_request *request)
{
	timeout_remove(&request->to_penalty);

	request->passdb_result = PASSDB_RESULT_INTERNAL_FAILURE;
	if (request->wanted_credentials_scheme != NULL) {
		request->private_callback.lookup_credentials(
			request->passdb_result, &uchar_nul, 0, request);
	} else {
		request->private_callback.verify_plain(request->passdb_result,
						       request);
	}
	auth_request_unref(&request);
}

static int
auth_request_fields_var_expand_lookup(const char *field_name, const char **value_r,
				      void *context, const char **error_r)
{
	struct auth_fields *fields = context;

	if (fields != NULL)
		*value_r = auth_fields_find(fields, field_name);
	if (fields == NULL || *value_r == NULL) {
		*error_r = t_strdup_printf("No such field '%s'", field_name);
		return -1;
	}
	return 0;
}

int auth_request_set_passdb_fields(struct auth_request *request,
				   struct auth_fields *fields)
{
	const char *driver_name =
		t_str_replace(request->passdb->passdb->iface.name, '-', '_');
	const struct var_expand_provider fn_table[] = {
		{ .key = driver_name, .func = auth_request_fields_var_expand_lookup },
		{ NULL, NULL }
	};

	return auth_request_set_passdb_fields_ex(request, fields, STATIC_PASS_SCHEME, fn_table);
}

int auth_request_set_passdb_fields_ex(struct auth_request *request,
				      void *context,
				      const char *default_password_scheme,
				      const struct var_expand_provider *fn_table)
{
	struct event *event = event_create(authdb_event(request));
	const struct auth_passdb_post_settings *post_set;
	const char *error;

	struct var_expand_params params = {
		.providers = fn_table,
		.context = context,
	};
	event_set_ptr(event, SETTINGS_EVENT_VAR_EXPAND_PARAMS, &params);

	if (settings_get(event, &auth_passdb_post_setting_parser_info, 0,
			 &post_set, &error) < 0) {
		e_error(event, "%s", error);
		event_unref(&event);
		return -1;
	}
	auth_request_set_strlist(request, &post_set->fields,
				 default_password_scheme);
	settings_free(post_set);
	event_unref(&event);
	return 0;
}

int auth_request_set_userdb_fields(struct auth_request *request,
				   struct auth_fields *fields) {
	const char *driver_name =
		t_str_replace(request->userdb->userdb->iface->name, '-', '_');
	const struct var_expand_provider fn_table[] = {
		{ .key = driver_name, .func = auth_request_fields_var_expand_lookup },
		VAR_EXPAND_TABLE_END
	};

	return auth_request_set_userdb_fields_ex(request, fields, fn_table);
}

int auth_request_set_userdb_fields_ex(struct auth_request *request, void *context,
				      const struct var_expand_provider *fn_table)
{
	struct event *event = event_create(authdb_event(request));
	const struct auth_userdb_post_settings *post_set;
	const char *error;

	const struct var_expand_params params = {
		.providers = fn_table,
		.context = context,
	};
	event_set_ptr(event, SETTINGS_EVENT_VAR_EXPAND_PARAMS, (void *)&params);

	if (settings_get(event, &auth_userdb_post_setting_parser_info, 0,
			 &post_set, &error) < 0) {
		e_error(event, "%s", error);
		event_unref(&event);
		return -1;
	}
	auth_request_set_userdb_strlist(request, &post_set->fields);
	settings_free(post_set);
	event_unref(&event);
	return 0;
}

static int
auth_request_finish_passdb_lookup(enum passdb_result *result,
				  struct auth_request *request,
				  struct auth_passdb **next_passdb_r,
				  bool *master_login_r)
{
	struct auth_passdb *next_passdb;
	enum auth_db_rule result_rule;
	bool passdb_continue = FALSE;

	*master_login_r = FALSE;
	*next_passdb_r = NULL;

	if (request->passdb_password != NULL) {
		safe_memset(request->passdb_password, 0,
			    strlen(request->passdb_password));
	}

	if (request->passdb->set->deny &&
	    *result != PASSDB_RESULT_USER_UNKNOWN) {
		/* deny passdb. we can get through this step only if the
		   lookup returned that user doesn't exist in it. internal
		   errors are fatal here. */
		if (*result != PASSDB_RESULT_INTERNAL_FAILURE) {
			e_info(authdb_event(request),
			       "User found from deny passdb");
			*result = PASSDB_RESULT_USER_DISABLED;
		}
		return 1;
	}
	if (request->failed) {
		/* The passdb didn't fail, but something inside it failed
		   (e.g. allow_nets mismatch). Make sure we'll fail this
		   lookup, but reset the failure so the next passdb can
		   succeed. */
		if (*result == PASSDB_RESULT_OK)
			*result = PASSDB_RESULT_USER_UNKNOWN;
		request->failed = FALSE;
	}

	/* users that exist but can't log in are special. we don't try to match
	   any of the success/failure rules to them. they'll always fail. */
	switch (*result) {
	case PASSDB_RESULT_USER_DISABLED:
		return 1;
	case PASSDB_RESULT_PASS_EXPIRED:
		auth_request_set_field(request, "reason",
					"Password expired", NULL);
		return 1;

	case PASSDB_RESULT_OK:
		result_rule = request->passdb->result_success;
		break;
	case PASSDB_RESULT_INTERNAL_FAILURE:
		result_rule = request->passdb->result_internalfail;
		break;
	case PASSDB_RESULT_NEXT:
		e_debug(authdb_event(request),
			"Not performing authentication (noauthenticate set)");
		result_rule = AUTH_DB_RULE_CONTINUE;
		break;
	case PASSDB_RESULT_SCHEME_NOT_AVAILABLE:
	case PASSDB_RESULT_USER_UNKNOWN:
	case PASSDB_RESULT_PASSWORD_MISMATCH:
	default:
		result_rule = request->passdb->result_failure;
		break;
	}

	switch (result_rule) {
	case AUTH_DB_RULE_RETURN:
		break;
	case AUTH_DB_RULE_RETURN_OK:
		request->passdb_success = TRUE;
		break;
	case AUTH_DB_RULE_RETURN_FAIL:
		request->passdb_success = FALSE;
		break;
	case AUTH_DB_RULE_CONTINUE:
		passdb_continue = TRUE;
		if (*result == PASSDB_RESULT_OK) {
			/* password was successfully verified. don't bother
			   checking it again. */
			auth_request_set_password_verified(request);
		}
		break;
	case AUTH_DB_RULE_CONTINUE_OK:
		passdb_continue = TRUE;
		request->passdb_success = TRUE;
		/* password was successfully verified. don't bother
		   checking it again. */
		auth_request_set_password_verified(request);
		break;
	case AUTH_DB_RULE_CONTINUE_FAIL:
		passdb_continue = TRUE;
		request->passdb_success = FALSE;
		break;
	}
	/* nopassword check is specific to a single passdb and shouldn't leak
	   to the next one. we already added it to cache. */
	auth_fields_remove(request->fields.extra_fields, "nopassword");
	auth_fields_remove(request->fields.extra_fields, "noauthenticate");

	if (request->fields.requested_login_user != NULL &&
	    *result == PASSDB_RESULT_OK) {
		*master_login_r = TRUE;
		/* if the passdb lookup continues, it continues with non-master
		   passdbs for the requested_login_user. */
		next_passdb = auth_request_get_auth(request)->passdbs;
	} else {
		next_passdb = request->passdb->next;
	}

	while (next_passdb != NULL &&
		auth_request_want_skip_passdb(request, next_passdb))
		next_passdb = next_passdb->next;

	if (*result == PASSDB_RESULT_OK || *result == PASSDB_RESULT_NEXT) {
		/* this passdb lookup succeeded, preserve its extra fields */
		auth_fields_snapshot(request->fields.extra_fields);
		request->snapshot_have_userdb_prefetch_set =
			request->userdb_prefetch_set;
		if (request->fields.userdb_reply != NULL)
			auth_fields_snapshot(request->fields.userdb_reply);
	} else {
		/* this passdb lookup failed, remove any extra fields it set */
		auth_fields_rollback(request->fields.extra_fields);
		if (request->fields.userdb_reply != NULL) {
			auth_fields_rollback(request->fields.userdb_reply);
			request->userdb_prefetch_set =
				request->snapshot_have_userdb_prefetch_set;
		}
	}

	if (passdb_continue && next_passdb != NULL) {
		/* try next passdb. */
		request->passdb_password = NULL;

		if (*result == PASSDB_RESULT_USER_UNKNOWN) {
			/* remember that we did at least one successful
			   passdb lookup */
			request->passdbs_seen_user_unknown = TRUE;
		} else if (*result == PASSDB_RESULT_INTERNAL_FAILURE) {
			/* remember that we have had an internal failure. at
			   the end return internal failure if we couldn't
			   successfully login. */
			request->passdbs_seen_internal_failure = TRUE;
		}
		*next_passdb_r = next_passdb;
		return 0;
	} else if (request->passdb_success) {
		/* either this or a previous passdb lookup succeeded. */
		*result = PASSDB_RESULT_OK;
	} else if (request->passdbs_seen_internal_failure) {
		/* last passdb lookup returned internal failure. it may have
		   had the correct password, so return internal failure
		   instead of plain failure. */
		*result = PASSDB_RESULT_INTERNAL_FAILURE;
	} else if (*result == PASSDB_RESULT_NEXT) {
		/* admin forgot to put proper passdb last */
		e_error(authdb_event(request),
			"Last passdb had noauthenticate field, "
			"cannot authenticate user");
		*result = PASSDB_RESULT_INTERNAL_FAILURE;
	}

	i_assert(request->to_penalty == NULL);
	if (*result == PASSDB_RESULT_INTERNAL_FAILURE) {
		unsigned int internal_failure_delay =
			auth_request_get_internal_failure_delay(request);
		if (internal_failure_delay > 0) {
			auth_request_ref(request);
			request->to_penalty = timeout_add(internal_failure_delay,
				auth_request_passdb_internal_failure, request);
			return -1;
		}
	}
	return 1;
}

static int
auth_request_handle_passdb_callback(enum passdb_result *result,
				    struct auth_request *request)
{
	enum passdb_result orig_result = *result;
	struct auth_passdb *next_passdb;
	bool master_login;
	int ret;

	ret = auth_request_finish_passdb_lookup(result, request,
						&next_passdb, &master_login);
	auth_request_passdb_lookup_end(request, orig_result);
	if (master_login)
		auth_request_master_user_login_finish(request);
	if (ret == 0)
		request->passdb = next_passdb;
	return ret;
}

void
auth_request_verify_plain_callback_finish(enum passdb_result result,
					  struct auth_request *request)
{
	int ret;

	if ((ret = auth_request_handle_passdb_callback(&result, request)) == 0) {
		/* try next passdb */
		auth_request_verify_plain(request, request->mech_password,
			request->private_callback.verify_plain);
	} else if (ret > 0) {
		auth_request_ref(request);
		request->passdb_result = result;
		request->private_callback.verify_plain(request->passdb_result,
						       request);
		auth_request_unref(&request);
	}
}

void auth_request_verify_plain_callback(enum passdb_result result,
					struct auth_request *request)
{
	struct auth_passdb *passdb = request->passdb;

	i_assert(request->state == AUTH_REQUEST_STATE_PASSDB);

	auth_request_set_state(request, AUTH_REQUEST_STATE_MECH_CONTINUE);

	if (result == PASSDB_RESULT_OK &&
	    auth_fields_exists(request->fields.extra_fields, "noauthenticate"))
		result = PASSDB_RESULT_NEXT;

	if (result != PASSDB_RESULT_INTERNAL_FAILURE)
		auth_request_save_cache(request, result);
	else {
		/* lookup failed. if we're looking here only because the
		   request was expired in cache, fallback to using cached
		   expired record. */
		const char *cache_key = passdb->cache_key;

		if (passdb_cache_verify_plain(request, cache_key,
					      request->mech_password,
					      &result, TRUE)) {
			e_info(authdb_event(request),
			       "Falling back to expired data from cache");
			return;
		}
	}

	auth_request_verify_plain_callback_finish(result, request);
}

static bool password_has_illegal_chars(const char *password)
{
	for (; *password != '\0'; password++) {
		switch (*password) {
		case '\001':
		case '\t':
		case '\r':
		case '\n':
			/* these characters have a special meaning in internal
			   protocols, make sure the password doesn't
			   accidentally get there unescaped. */
			return TRUE;
		}
	}
	return FALSE;
}

static bool auth_request_is_disabled_master_user(struct auth_request *request)
{
	if (request->fields.requested_login_user == NULL ||
	    request->passdb != NULL)
		return FALSE;

	/* no masterdbs, master logins not supported */
	e_info(request->event,
	       "Attempted master login with no master passdbs "
	       "(trying to log in as user: %s)",
	       request->fields.requested_login_user);
	return TRUE;
}

static void auth_request_policy_penalty_finish(void *context)
{
	struct auth_policy_check_ctx *ctx = context;

	timeout_remove(&ctx->request->to_penalty);

	i_assert(ctx->request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);

	switch(ctx->type) {
	case AUTH_POLICY_CHECK_TYPE_PLAIN:
		ctx->request->handler->verify_plain_continue_callback(
			ctx->request, ctx->callback_plain);
		return;
	case AUTH_POLICY_CHECK_TYPE_LOOKUP:
		auth_request_lookup_credentials_policy_continue(
			ctx->request, ctx->callback_lookup);
		return;
	case AUTH_POLICY_CHECK_TYPE_SUCCESS:
		auth_request_success_continue(ctx);
		return;
	default:
		i_unreached();
	}
}

static void auth_request_policy_check_callback(int result, void *context)
{
	struct auth_policy_check_ctx *ctx = context;

	i_assert(ctx->request->to_penalty == NULL);

	ctx->request->policy_processed = TRUE;
	/* It's possible that multiple policy lookups return a penalty.
	   Sum them all up to the event. */
	ctx->request->policy_penalty += result < 0 ? 0 : result;

	if (ctx->request->set->policy_log_only && result != 0) {
		auth_request_policy_penalty_finish(context);
		return;
	}
	if (result < 0) {
		/* fail it right here and now */
		auth_request_fail(ctx->request);
	} else if (ctx->type != AUTH_POLICY_CHECK_TYPE_SUCCESS && result > 0 &&
		   !ctx->request->fields.no_penalty && !shutting_down) {
		ctx->request->to_penalty = timeout_add(result * 1000,
			auth_request_policy_penalty_finish, context);
	} else {
		auth_request_policy_penalty_finish(context);
	}
}

void auth_request_verify_plain(struct auth_request *request,
				const char *password,
				verify_plain_callback_t *callback)
{
	struct auth_policy_check_ctx *ctx;

	i_assert(request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);

	if (request->mech_password == NULL)
		request->mech_password = p_strdup(request->pool, password);
	else
		i_assert(request->mech_password == password);
	request->user_returned_by_lookup = FALSE;

	if (request->policy_processed ||
	    !request->set->policy_check_before_auth) {
		request->handler->verify_plain_continue_callback(request,
								 callback);
	} else {
		ctx = p_new(request->pool, struct auth_policy_check_ctx, 1);
		ctx->request = request;
		ctx->callback_plain = callback;
		ctx->type = AUTH_POLICY_CHECK_TYPE_PLAIN;
		auth_policy_check(request, request->mech_password,
				  auth_request_policy_check_callback, ctx);
	}
}

void auth_request_default_verify_plain_continue(
	struct auth_request *request, verify_plain_callback_t *callback)
{
	struct auth_passdb *passdb;
	enum passdb_result result;
	const char *cache_key;
	const char *password = request->mech_password;

	i_assert(request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);

	if (auth_request_is_disabled_master_user(request)) {
		callback(PASSDB_RESULT_USER_UNKNOWN, request);
		return;
	}

	if (password_has_illegal_chars(password)) {
		e_info(authdb_event(request),
		       "Attempted login with password having illegal chars");
		callback(PASSDB_RESULT_USER_UNKNOWN, request);
		return;
	}

	passdb = request->passdb;

	while (passdb != NULL && auth_request_want_skip_passdb(request, passdb))
		passdb = passdb->next;

	request->passdb = passdb;

	if (passdb == NULL) {
		e_error(request->event, "All password databases were skipped");
		callback(PASSDB_RESULT_INTERNAL_FAILURE, request);
		return;
	}

	auth_request_passdb_lookup_begin(request);
	request->private_callback.verify_plain = callback;

	cache_key = passdb_cache == NULL ? NULL : passdb->cache_key;
	if (passdb_cache_verify_plain(request, cache_key, password,
				      &result, FALSE)) {
		return;
	}

	auth_request_set_state(request, AUTH_REQUEST_STATE_PASSDB);
	/* In case this request had already done a credentials lookup (is it
	   even possible?), make sure wanted_credentials_scheme is cleared
	   so passdbs don't think we're doing a credentials lookup. */
	request->wanted_credentials_scheme = NULL;

	if (passdb->passdb->iface.verify_plain == NULL) {
		/* we're deinitializing and just want to get rid of this
		   request */
		auth_request_verify_plain_callback(
			PASSDB_RESULT_INTERNAL_FAILURE, request);
	} else if (passdb->passdb->blocking) {
		passdb_blocking_verify_plain(request);
	} else {
		passdb->passdb->iface.verify_plain(
			request, password, auth_request_verify_plain_callback);
	}
}

static void
auth_request_lookup_credentials_finish(enum passdb_result result,
				       const unsigned char *credentials,
				       size_t size,
				       struct auth_request *request)
{
	int ret;

	if ((ret = auth_request_handle_passdb_callback(&result, request)) == 0) {
		/* try next passdb */
		if (request->fields.skip_password_check &&
		    request->fields.delayed_credentials == NULL && size > 0) {
			/* passdb continue* rule after a successful lookup.
			   remember these credentials and use them later on. */
			auth_request_set_delayed_credentials(request,
				credentials, size);
		}
		auth_request_lookup_credentials(request,
			request->wanted_credentials_scheme,
		  	request->private_callback.lookup_credentials);
	} else if (ret > 0) {
		if (request->fields.delayed_credentials != NULL && size == 0) {
			/* we did multiple passdb lookups, but the last one
			   didn't provide any credentials (e.g. just wanted to
			   add some extra fields). so use the first passdb's
			   credentials instead. */
			credentials = request->fields.delayed_credentials;
			size = request->fields.delayed_credentials_size;
		}
		if (request->set->debug_passwords &&
		    result == PASSDB_RESULT_OK) {
			e_debug(authdb_event(request),
				"Credentials: %s",
				binary_to_hex(credentials, size));
		}
		if (result == PASSDB_RESULT_SCHEME_NOT_AVAILABLE &&
		    request->passdbs_seen_user_unknown) {
			/* one of the passdbs accepted the scheme,
			   but the user was unknown there */
			result = PASSDB_RESULT_USER_UNKNOWN;
		}
		request->passdb_result = result;
		request->private_callback.
			lookup_credentials(result, credentials, size, request);
	}
}

void auth_request_lookup_credentials_callback(enum passdb_result result,
					      const unsigned char *credentials,
					      size_t size,
					      struct auth_request *request)
{
	struct auth_passdb *passdb = request->passdb;
	const char *cache_cred, *cache_scheme;

	i_assert(request->state == AUTH_REQUEST_STATE_PASSDB);

	auth_request_set_state(request, AUTH_REQUEST_STATE_MECH_CONTINUE);

	if (result == PASSDB_RESULT_OK &&
	    auth_fields_exists(request->fields.extra_fields, "noauthenticate"))
		result = PASSDB_RESULT_NEXT;

	if (result != PASSDB_RESULT_INTERNAL_FAILURE)
		auth_request_save_cache(request, result);
	else {
		/* lookup failed. if we're looking here only because the
		   request was expired in cache, fallback to using cached
		   expired record. */
		const char *cache_key = passdb->cache_key;

		if (passdb_cache_lookup_credentials(request, cache_key,
						    &cache_cred, &cache_scheme,
						    &result, TRUE)) {
			e_info(authdb_event(request),
			       "Falling back to expired data from cache");
			passdb_handle_credentials(
				result, cache_cred, cache_scheme,
				auth_request_lookup_credentials_finish,
				request);
			return;
		}
	}

	auth_request_lookup_credentials_finish(result, credentials, size,
					       request);
}

void auth_request_lookup_credentials(struct auth_request *request,
				     const char *scheme,
				     lookup_credentials_callback_t *callback)
{
	struct auth_policy_check_ctx *ctx;

	i_assert(request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);

	if (request->wanted_credentials_scheme == NULL)
		request->wanted_credentials_scheme =
			p_strdup(request->pool, scheme);
	request->user_returned_by_lookup = FALSE;

	if (request->policy_processed ||
	    !request->set->policy_check_before_auth) {
		auth_request_lookup_credentials_policy_continue(
			request, callback);
	} else {
		ctx = p_new(request->pool, struct auth_policy_check_ctx, 1);
		ctx->request = request;
		ctx->callback_lookup = callback;
		ctx->type = AUTH_POLICY_CHECK_TYPE_LOOKUP;
		auth_policy_check(request, ctx->request->mech_password,
				  auth_request_policy_check_callback, ctx);
	}
}

static void
auth_request_lookup_credentials_policy_continue(
	struct auth_request *request, lookup_credentials_callback_t *callback)
{
	struct auth_passdb *passdb;
	const char *cache_key, *cache_cred, *cache_scheme;
	enum passdb_result result;

	i_assert(request->state == AUTH_REQUEST_STATE_MECH_CONTINUE);
	if (auth_request_is_disabled_master_user(request)) {
		callback(PASSDB_RESULT_USER_UNKNOWN, NULL, 0, request);
		return;
	}
	passdb = request->passdb;
	while (passdb != NULL && auth_request_want_skip_passdb(request, passdb))
		passdb = passdb->next;
	request->passdb = passdb;

	if (passdb == NULL) {
		if (request->passdb_success) {
			/* This is coming from mech that has already validated
			   credentials, so we can just continue as success. */
			result = PASSDB_RESULT_OK;
			request->passdb_result = result;
			callback(result, NULL, 0, request);
			return;
		}
		e_error(request->event, "All password databases were skipped");
		callback(PASSDB_RESULT_INTERNAL_FAILURE, NULL, 0, request);
		return;
	}

	auth_request_passdb_lookup_begin(request);
	request->private_callback.lookup_credentials = callback;

	cache_key = passdb_cache == NULL ? NULL : passdb->cache_key;
	if (cache_key != NULL) {
		if (passdb_cache_lookup_credentials(request, cache_key,
						    &cache_cred, &cache_scheme,
						    &result, FALSE)) {
			request->passdb_cache_result = AUTH_REQUEST_CACHE_HIT;
			passdb_handle_credentials(
				result, cache_cred, cache_scheme,
				auth_request_lookup_credentials_finish,
				request);
			return;
		} else {
			request->passdb_cache_result = AUTH_REQUEST_CACHE_MISS;
		}
	}

	auth_request_set_state(request, AUTH_REQUEST_STATE_PASSDB);

	if (passdb->passdb->iface.lookup_credentials == NULL) {
		/* this passdb doesn't support credentials */
		e_debug(authdb_event(request),
			"passdb doesn't support credential lookups");
		auth_request_lookup_credentials_callback(
					PASSDB_RESULT_SCHEME_NOT_AVAILABLE,
					uchar_empty_ptr, 0, request);
	} else if (passdb->passdb->blocking) {
		passdb_blocking_lookup_credentials(request);
	} else {
		passdb->passdb->iface.lookup_credentials(request,
			auth_request_lookup_credentials_callback);
	}
}

void auth_request_set_credentials(struct auth_request *request,
				  const char *scheme, const char *data,
				  set_credentials_callback_t *callback)
{
	struct auth_passdb *passdb = request->passdb;
	const char *cache_key, *new_credentials;

	cache_key = passdb_cache == NULL ? NULL : passdb->cache_key;
	if (cache_key != NULL)
		auth_cache_remove(passdb_cache, request, cache_key);

	request->private_callback.set_credentials = callback;

	new_credentials = t_strdup_printf("{%s}%s", scheme, data);
	if (passdb->passdb->blocking)
		passdb_blocking_set_credentials(request, new_credentials);
	else if (passdb->passdb->iface.set_credentials != NULL) {
		passdb->passdb->iface.set_credentials(request, new_credentials,
						      callback);
	} else {
		/* this passdb doesn't support credentials update */
		callback(FALSE, request);
	}
}

static void
auth_request_userdb_save_cache(struct auth_request *request,
			       enum userdb_result result)
{
	struct auth_userdb *userdb = request->userdb;
	string_t *str;
	const char *cache_value;

	if (passdb_cache == NULL || userdb->cache_key == NULL)
		return;

	if (result == USERDB_RESULT_USER_UNKNOWN)
		cache_value = "";
	else {
		str = t_str_new(128);
		auth_fields_append(request->fields.userdb_reply, str,
				   AUTH_FIELD_FLAG_CHANGED,
				   AUTH_FIELD_FLAG_CHANGED, FALSE);
		if (request->user_returned_by_lookup) {
			/* username was changed by passdb or userdb */
			if (str_len(str) > 0)
				str_append_c(str, '\t');
			str_append(str, "user=");
			str_append_tabescaped(str, request->fields.user);
		}
		if (str_len(str) == 0) {
			/* no userdb fields. but we can't save an empty string,
			   since that means "user unknown". */
			str_append(str, AUTH_REQUEST_USER_KEY_IGNORE);
		}
		cache_value = str_c(str);
	}
	/* last_success has no meaning with userdb */
	auth_cache_insert(passdb_cache, request, userdb->cache_key,
			  cache_value, FALSE);
}

static bool
auth_request_lookup_user_cache(struct auth_request *request, const char *key,
			       enum userdb_result *result_r, bool use_expired)
{
	const char *value;
	struct auth_cache_node *node;
	bool expired, neg_expired;

	value = auth_cache_lookup(passdb_cache, request, key, &node,
				  &expired, &neg_expired);
	if (value == NULL || (expired && !use_expired)) {
		request->userdb_cache_result = AUTH_REQUEST_CACHE_MISS;
		e_debug(request->event,
			value == NULL ? "%suserdb cache miss" :
			"%suserdb cache expired",
			auth_request_get_log_prefix_db(request));
		return FALSE;
	}
	request->userdb_cache_result = AUTH_REQUEST_CACHE_HIT;
	e_debug(request->event,
		"%suserdb cache hit: %s",
		auth_request_get_log_prefix_db(request), value);

	if (*value == '\0') {
		/* negative cache entry */
		*result_r = USERDB_RESULT_USER_UNKNOWN;
	} else {
		*result_r = USERDB_RESULT_OK;
	}

	/* We want to preserve any userdb fields set by the earlier passdb
	   lookup, so initialize userdb_reply only if it doesn't exist. */
	if (request->fields.userdb_reply == NULL)
		auth_request_init_userdb_reply(request);
	auth_request_userdb_import(request, value);
	return TRUE;
}

static void auth_request_userdb_internal_failure(struct auth_request *request)
{
	timeout_remove(&request->to_penalty);
	request->private_callback.userdb(USERDB_RESULT_INTERNAL_FAILURE,
					 request);
	auth_request_unref(&request);
}

void auth_request_userdb_callback(enum userdb_result result,
				  struct auth_request *request)
{
	struct auth_userdb *userdb = request->userdb;
	struct auth_userdb *next_userdb;
	enum auth_db_rule result_rule;
	bool userdb_continue = FALSE;

	if (!request->userdb_lookup_tempfailed &&
	    result != USERDB_RESULT_INTERNAL_FAILURE &&
	    request->userdb_cache_result != AUTH_REQUEST_CACHE_HIT) {
		/* The userdb lookup itself didn't fail. We want to cache
		   its result, regardless of what is done with it afterwards. */
		auth_request_userdb_save_cache(request, result);
	}

	if (result == USERDB_RESULT_OK) {
		/* this userdb lookup succeeded, preserve its extra fields */
		auth_fields_snapshot(request->fields.userdb_reply);
	} else {
		/* this userdb lookup failed, remove any extra fields
		   it set */
		auth_fields_rollback(request->fields.userdb_reply);
	}

	switch (result) {
	case USERDB_RESULT_OK:
		result_rule = userdb->result_success;
		break;
	case USERDB_RESULT_INTERNAL_FAILURE:
		result_rule = userdb->result_internalfail;
		break;
	case USERDB_RESULT_USER_UNKNOWN:
	default:
		result_rule = userdb->result_failure;
		break;
	}

	switch (result_rule) {
	case AUTH_DB_RULE_RETURN:
		break;
	case AUTH_DB_RULE_RETURN_OK:
		request->userdb_success = TRUE;
		break;
	case AUTH_DB_RULE_RETURN_FAIL:
		request->userdb_success = FALSE;
		break;
	case AUTH_DB_RULE_CONTINUE:
		userdb_continue = TRUE;
		break;
	case AUTH_DB_RULE_CONTINUE_OK:
		userdb_continue = TRUE;
		request->userdb_success = TRUE;
		break;
	case AUTH_DB_RULE_CONTINUE_FAIL:
		userdb_continue = TRUE;
		request->userdb_success = FALSE;
		break;
	}

	auth_request_userdb_lookup_end(request, result);

	next_userdb = userdb->next;
	while (next_userdb != NULL &&
		auth_request_want_skip_userdb(request, next_userdb))
		next_userdb = next_userdb->next;

	if (userdb_continue && next_userdb != NULL) {
		/* try next userdb. */
		if (result == USERDB_RESULT_INTERNAL_FAILURE)
			request->userdbs_seen_internal_failure = TRUE;

		request->user_returned_by_lookup = FALSE;

		request->userdb = next_userdb;
		auth_request_lookup_user(request,
					 request->private_callback.userdb);
		return;
	}

	if (request->userdb_success)
		result = USERDB_RESULT_OK;
	else if (request->userdbs_seen_internal_failure ||
		 result == USERDB_RESULT_INTERNAL_FAILURE) {
		/* one of the userdb lookups failed. the user might have been
		   in there, so this is an internal failure */
		result = USERDB_RESULT_INTERNAL_FAILURE;
	} else if (request->client_pid != 0) {
		/* this was an actual login attempt, the user should
		   have been found. */
		e_error(request->event, "user not found from any userdbs");
		result = USERDB_RESULT_USER_UNKNOWN;
	} else {
		result = USERDB_RESULT_USER_UNKNOWN;
	}

	unsigned int internal_failure_delay = 0;
	if (request->userdb_lookup_tempfailed) {
		/* userdb lookup succeeded, but it either returned tempfail
		   or one of its fields was invalid. Looking up the user from
		   cache isn't probably a good idea. */
	} else if (result == USERDB_RESULT_INTERNAL_FAILURE &&
		   (passdb_cache != NULL && userdb->cache_key != NULL)) {
		/* lookup failed. if we're looking here only because the
		   request was expired in cache, fallback to using cached
		   expired record. */
		const char *cache_key = userdb->cache_key;

		if (auth_request_lookup_user_cache(request, cache_key,
						   &result, TRUE)) {
			e_info(request->event,
			       "%sFalling back to expired data from cache",
				auth_request_get_log_prefix_db(request));
		} else {
			internal_failure_delay =
				auth_request_get_internal_failure_delay(request);
		}
	} else if (result == USERDB_RESULT_INTERNAL_FAILURE) {
		internal_failure_delay =
			auth_request_get_internal_failure_delay(request);
	}

	i_assert(request->to_penalty == NULL);
	if (internal_failure_delay > 0) {
		auth_request_ref(request);
		request->to_penalty = timeout_add(internal_failure_delay,
			auth_request_userdb_internal_failure, request);
	} else {
		request->private_callback.userdb(result, request);
	}
}

void auth_request_lookup_user(struct auth_request *request,
			      userdb_callback_t *callback)
{
	struct auth_userdb *userdb = request->userdb;
	const char *cache_key;

	request->private_callback.userdb = callback;
	request->user_returned_by_lookup = FALSE;
	request->userdb_lookup = TRUE;
	request->userdb_cache_result = AUTH_REQUEST_CACHE_NONE;
	if (request->fields.userdb_reply == NULL)
		auth_request_init_userdb_reply(request);

	auth_request_userdb_lookup_begin(request);

	/* (for now) auth_cache is shared between passdb and userdb */
	cache_key = passdb_cache == NULL ? NULL : userdb->cache_key;
	if (cache_key != NULL) {
		enum userdb_result result;

		if (auth_request_lookup_user_cache(request, cache_key,
						   &result, FALSE)) {
			request->userdb_cache_result = AUTH_REQUEST_CACHE_HIT;
			auth_request_userdb_callback(result, request);
			return;
		} else {
			request->userdb_cache_result = AUTH_REQUEST_CACHE_MISS;
		}
	}

	if (userdb->userdb->iface->lookup == NULL) {
		/* we are deinitializing */
		auth_request_userdb_callback(USERDB_RESULT_INTERNAL_FAILURE,
					     request);
	} else if (userdb->userdb->blocking) {
		userdb_blocking_lookup(request);
	} else {
		userdb->userdb->iface->lookup(
			request, auth_request_userdb_callback);
	}
}

static void
auth_request_validate_networks(struct auth_request *request,
			       const char *name, const char *networks,
			       const struct ip_addr *remote_ip)
{
	const char *const *net;
	struct ip_addr net_ip;
	unsigned int bits;
	bool found = FALSE;

	for (net = t_strsplit_spaces(networks, ", "); *net != NULL; net++) {
		e_debug(authdb_event(request),
			"%s: Matching for network %s", name, *net);

		if (strcmp(*net, "local") == 0) {
			if (remote_ip->family == 0) {
				found = TRUE;
				break;
			}
		} else if (net_parse_range(*net, &net_ip, &bits) < 0) {
			e_info(authdb_event(request),
			       "%s: Invalid network '%s'", name, *net);
		} else if (remote_ip->family != 0 &&
			   net_is_in_network(remote_ip, &net_ip, bits)) {
			found = TRUE;
			break;
		}
	}

	if (found)
		;
	else if (remote_ip->family == 0) {
		e_info(authdb_event(request), "%s check failed: "
		       "Remote IP not known and 'local' missing", name);
	} else {
		e_info(authdb_event(request),
		       "%s check failed: IP %s not in allowed networks",
		       name, net_ip2addr(remote_ip));
	}
	if (!found)
		request->failed = TRUE;
}

static void
auth_request_validate_client_fp(struct auth_request *request, const char *name,
				const char *fp)
{
	const char *client_cert_fp = request->fields.ssl_client_cert_fp;
	const char *client_pubkey_fp = request->fields.ssl_client_cert_pubkey_fp;
	bool valid;

	/* Can't be valid if the connection is not TLS secured, proxied does not
	   count. */
	if (request->fields.conn_secured != AUTH_REQUEST_CONN_SECURED_TLS)
		valid = FALSE;
	/* check that the fingerprint isn't empty */
	else if (*fp == '\0') {
		e_info(authdb_event(request), "%s check failed: value was empty",
		       name);
		request->failed = TRUE;
		return;
	} else if (strcmp(name, "check_client_fp") == 0) {
		valid = strcmp(client_cert_fp, fp) == 0 ||
		        strcmp(client_pubkey_fp, fp) == 0;
	} else if (strcmp(name, "check_client_cert_fp") == 0)
		valid = strcmp(client_cert_fp, fp) == 0;
	else if (strcmp(name, "check_client_pubkey_fp") == 0)
		valid = strcmp(client_pubkey_fp, fp) == 0;
	else
		i_unreached();

	if (!valid) {
		e_info(authdb_event(request),
		       "%s check failed: %s does not match client certificate",
		       name, fp);
		request->failed = TRUE;
	} else {
		e_debug(authdb_event(request),
			"%s check success: %s matches client certificate",
			name, fp);
		(void)auth_request_import(request, "valid-client-cert", "yes");
	}
}

static void
auth_request_set_password(struct auth_request *request, const char *value,
			  const char *default_scheme, bool noscheme)
{
	if (request->passdb_password != NULL) {
		e_error(authdb_event(request),
			"Multiple password values not supported");
		return;
	}

	/* if the password starts with '{' it most likely contains
	   also '}'. check it anyway to make sure, because we
	   assert-crash later if it doesn't exist. this could happen
	   if plaintext passwords are used. */
	if (*value == '{' && !noscheme && strchr(value, '}') != NULL)
		request->passdb_password = p_strdup(request->pool, value);
	else {
		i_assert(default_scheme != NULL);
		request->passdb_password =
			p_strdup_printf(request->pool, "{%s}%s",
					default_scheme, value);
	}
}

static const char *
get_updated_username(const char *old_username,
		     const char *name, const char *value)
{
	const char *p;

	if (strcmp(name, "user") == 0) {
		/* replace the whole username */
		return value;
	}

	p = strchr(old_username, '@');
	if (strcmp(name, "username") == 0) {
		if (strchr(value, '@') != NULL)
			return value;

		/* preserve the current @domain */
		return t_strconcat(value, p, NULL);
	}

	if (strcmp(name, "domain") == 0) {
		if (p == NULL) {
			/* add the domain */
			return t_strconcat(old_username, "@", value, NULL);
		} else {
			/* replace the existing domain */
			p = t_strdup_until(old_username, p + 1);
			return t_strconcat(p, value, NULL);
		}
	}
	return NULL;
}

static bool
auth_request_try_update_username(struct auth_request *request,
				 const char *name, const char *value)
{
	const char *new_value;

	new_value = get_updated_username(request->fields.user, name, value);
	if (new_value == NULL)
		return FALSE;
	if (new_value[0] == '\0') {
		e_error(authdb_event(request),
			"username attempted to be changed to empty");
		request->failed = TRUE;
		return TRUE;
	}

	if (strcmp(request->fields.user, new_value) != 0) {
		e_debug(authdb_event(request),
			"username changed %s -> %s",
			request->fields.user, new_value);
		auth_request_set_username_forced(request, new_value);
	}
	request->user_returned_by_lookup = TRUE;
	return TRUE;
}

static void
auth_request_passdb_import(struct auth_request *request, const char *args,
			   const char *key_prefix, const char *default_scheme)
{
	const char *const *arg, *field;

	for (arg = t_strsplit(args, "\t"); *arg != NULL; arg++) {
		field = t_strconcat(key_prefix, *arg, NULL);
		auth_request_set_field_keyvalue(request, field, default_scheme);
	}
}

void auth_request_set_field(struct auth_request *request,
			    const char *name, const char *value,
			    const char *default_scheme)
{
	const char *suffix;
	size_t name_len = strlen(name);

	i_assert(*name != '\0');
	i_assert(value != NULL);

	/* Allow passdb to be NULL if it has already succeeded,
	   this happens mostly with mechs that already know the user
	   account is valid. */
	i_assert(request->passdb != NULL || request->passdb_success);

	if (name_len > 8 && strcmp(name+name_len-8, ":default") == 0) {
		/* set this field only if it hasn't been set before */
		name = t_strndup(name, name_len-8);
		if (auth_fields_exists(request->fields.extra_fields, name))
			return;
	} else if (name_len > 7 && strcmp(name+name_len-7, ":remove") == 0) {
		/* remove this field entirely */
		name = t_strndup(name, name_len-7);
		auth_fields_remove(request->fields.extra_fields, name);
		return;
	}

	if (strcmp(name, "password") == 0) {
		auth_request_set_password(request, value,
					  default_scheme, FALSE);
		return;
	}
	if (strcmp(name, "password_noscheme") == 0) {
		auth_request_set_password(request, value, default_scheme, TRUE);
		return;
	}

	if (auth_request_try_update_username(request, name, value)) {
		/* don't change the original value so it gets saved correctly
		   to cache. */
	} else if (strcmp(name, "login_user") == 0) {
		auth_request_set_login_username_forced(request, value);
	} else if (strcmp(name, "allow_nets") == 0) {
		auth_request_validate_networks(request, name, value,
					       &request->fields.remote_ip);
	} else if (strcmp(name, "check_client_fp") == 0 ||
		   strcmp(name, "check_client_cert_fp") == 0 ||
		   strcmp(name, "check_client_pubkey_fp") == 0) {
		auth_request_validate_client_fp(request, name, value);
	} else if (strcmp(name, "fail") == 0) {
		request->failed = TRUE;
	} else if (strcmp(name, "nodelay") == 0) {
		/* don't delay replying to client of the failure */
		request->failure_nodelay = TRUE;
	 	auth_fields_add(request->fields.extra_fields, name, value, 0);
	 	return;
	} else if (strcmp(name, "delay_until") == 0) {
		time_t timestamp;
		unsigned int extra_secs = 0;
		const char *p;

		p = strchr(value, '+');
		if (p != NULL) {
			value = t_strdup_until(value, p++);
			if (str_to_uint(p, &extra_secs) < 0) {
				e_error(authdb_event(request),
					"Invalid delay_until randomness number '%s'", p);
				request->failed = TRUE;
			} else {
				extra_secs = i_rand_limit(extra_secs);
			}
		}
		if (str_to_time(value, &timestamp) < 0) {
			e_error(authdb_event(request),
				"Invalid delay_until timestamp '%s'", value);
			request->failed = TRUE;
		} else if (timestamp <= ioloop_time) {
			/* no more delays */
		} else if (timestamp - ioloop_time > AUTH_REQUEST_MAX_DELAY_SECS) {
			e_error(authdb_event(request),
				"delay_until timestamp %s is too much in the future, failing", value);
			request->failed = TRUE;
		} else {
			/* add randomness, but not too much of it */
			timestamp += extra_secs;
			if (timestamp - ioloop_time > AUTH_REQUEST_MAX_DELAY_SECS)
				timestamp = ioloop_time + AUTH_REQUEST_MAX_DELAY_SECS;
			request->delay_until = timestamp;
		}
	} else if (strcmp(name, "allow_real_nets") == 0) {
		auth_request_validate_networks(request, name, value,
					       &request->fields.real_remote_ip);
	} else if (str_begins(name, "userdb_", &suffix)) {
		/* for prefetch userdb */
		request->userdb_prefetch_set = TRUE;
		if (request->fields.userdb_reply == NULL)
			auth_request_init_userdb_reply(request);
		if (strcmp(name, "userdb_userdb_import") == 0) {
			/* we can't put the whole userdb_userdb_import
			   value to extra_cache_fields or it doesn't work
			   properly. so handle this explicitly. */
			auth_request_passdb_import(request, value,
						   "userdb_", default_scheme);
			return;
		}
		auth_request_set_userdb_field(request, suffix, value);
	} else if (strcmp(name, "noauthenticate") == 0) {
		/* add "nopassword" also so that passdbs won't try to verify
		   the password. */
		auth_fields_add(request->fields.extra_fields, name, value, 0);
		auth_fields_add(request->fields.extra_fields,
				"nopassword", NULL, 0);
	} else if (strcmp(name, "nopassword") == 0) {
		/* NULL password - anything goes */
		request->passdb_password = NULL;
		auth_fields_add(request->fields.extra_fields, name, value, 0);
		return;
	} else if (strcmp(name, "passdb_import") == 0) {
		auth_request_passdb_import(request, value, "", default_scheme);
		return;
	} else {
		/* these fields are returned to client */
		auth_fields_add(request->fields.extra_fields, name, value, 0);
		return;
	}

	/* add the field unconditionally to extra_fields. this is required if
	   a) auth cache is used, b) if we're a worker and we'll need to send
	   this to the main auth process that can store it in the cache,
	   c) for easily checking :default fields' existence. */
	auth_fields_add(request->fields.extra_fields, name, value,
			AUTH_FIELD_FLAG_HIDDEN);
}

void auth_request_set_null_field(struct auth_request *request, const char *name)
{
	if (str_begins_with(name, "userdb_")) {
		/* make sure userdb prefetch is used even if all the fields
		   were returned as NULL. */
		request->userdb_prefetch_set = TRUE;
	}
}

void auth_request_set_field_keyvalue(struct auth_request *request,
				     const char *field,
				     const char *default_scheme)
{
	const char *key, *value;

	value = strchr(field, '=');
	if (value == NULL) {
		key = field;
		value = "";
	} else {
		key = t_strdup_until(field, value);
		value++;
	}
	auth_request_set_field(request, key, value, default_scheme);
}

void auth_request_set_fields(struct auth_request *request,
			     const char *const *fields,
			     const char *default_scheme)
{
	for (; *fields != NULL; fields++) {
		if (**fields == '\0')
			continue;
		auth_request_set_field_keyvalue(request, *fields,
						default_scheme);
	}
}

void auth_request_set_strlist(struct auth_request *request,
			      const ARRAY_TYPE(const_string) *strlist,
			      const char *default_scheme)
{
	if (!array_is_created(strlist))
		return;

	unsigned int i, count;
	const char *const *fields = array_get(strlist, &count);
	i_assert(count % 2 == 0);
	for (i = 0; i < count; i += 2) {
		auth_request_set_field(request, fields[i], fields[i + 1],
				       default_scheme);
	}
}

static void
auth_request_set_uidgid_file(struct auth_request *request,
			     const char *path_template)
{
	string_t *path;
	struct stat st;
	const char *error;

	path = t_str_new(256);
	if (auth_request_var_expand(path, path_template, request,
				    NULL, &error) < 0) {
		e_error(authdb_event(request),
			"Failed to expand uidgid_file=%s: %s",
			path_template, error);
		request->userdb_lookup_tempfailed = TRUE;
	} else if (stat(str_c(path), &st) < 0) {
		e_error(authdb_event(request),
			"stat(%s) failed: %m", str_c(path));
		request->userdb_lookup_tempfailed = TRUE;
	} else {
		auth_fields_add(request->fields.userdb_reply,
				"uid", dec2str(st.st_uid), 0);
		auth_fields_add(request->fields.userdb_reply,
				"gid", dec2str(st.st_gid), 0);
	}
}

static void
auth_request_userdb_import(struct auth_request *request, const char *args)
{
	const char *key, *value, *const *arg;

	for (arg = t_strsplit(args, "\t"); *arg != NULL; arg++) {
		value = strchr(*arg, '=');
		if (value == NULL) {
			key = *arg;
			value = "";
		} else {
			key = t_strdup_until(*arg, value);
			value++;
		}
		auth_request_set_userdb_field(request, key, value);
	}
}

void auth_request_set_userdb_field(struct auth_request *request,
				   const char *name, const char *value)
{
	size_t name_len = strlen(name);
	uid_t uid;
	gid_t gid;

	i_assert(value != NULL);

	if (name_len > 8 && strcmp(name+name_len-8, ":default") == 0) {
		/* set this field only if it hasn't been set before */
		name = t_strndup(name, name_len-8);
		if (auth_fields_exists(request->fields.userdb_reply, name))
			return;
	} else if (name_len > 7 && strcmp(name+name_len-7, ":remove") == 0) {
		/* remove this field entirely */
		name = t_strndup(name, name_len-7);
		auth_fields_remove(request->fields.userdb_reply, name);
		return;
	}

	if (strcmp(name, "uid") == 0) {
		uid = userdb_parse_uid(request, value);
		if (uid == (uid_t)-1) {
			request->userdb_lookup_tempfailed = TRUE;
			return;
		}
		value = dec2str(uid);
	} else if (strcmp(name, "gid") == 0) {
		gid = userdb_parse_gid(request, value);
		if (gid == (gid_t)-1) {
			request->userdb_lookup_tempfailed = TRUE;
			return;
		}
		value = dec2str(gid);
	} else if (strcmp(name, "tempfail") == 0) {
		request->userdb_lookup_tempfailed = TRUE;
		return;
	} else if (auth_request_try_update_username(request, name, value)) {
		return;
	} else if (strcmp(name, "uidgid_file") == 0) {
		auth_request_set_uidgid_file(request, value);
		return;
	} else if (strcmp(name, "userdb_import") == 0) {
		auth_request_userdb_import(request, value);
		return;
	} else if (strcmp(name, "system_user") == 0) {
		/* FIXME: the system_user is for backwards compatibility */
		static bool warned = FALSE;
		if (!warned) {
			e_warning(authdb_event(request),
				  "Replace system_user with system_groups_user");
			warned = TRUE;
		}
		name = "system_groups_user";
	} else if (strcmp(name, AUTH_REQUEST_USER_KEY_IGNORE) == 0) {
		return;
	}

	auth_fields_add(request->fields.userdb_reply, name, value, 0);
}

void auth_request_set_userdb_field_values(struct auth_request *request,
					  const char *name,
					  const char *const *values)
{
	if (*values == NULL)
		return;

	if (strcmp(name, "gid") == 0) {
		/* convert gids to comma separated list */
		string_t *value;
		gid_t gid;

		value = t_str_new(128);
		for (; *values != NULL; values++) {
			gid = userdb_parse_gid(request, *values);
			if (gid == (gid_t)-1) {
				request->userdb_lookup_tempfailed = TRUE;
				return;
			}

			if (str_len(value) > 0)
				str_append_c(value, ',');
			str_append(value, dec2str(gid));
		}
		auth_fields_add(request->fields.userdb_reply, name,
				str_c(value), 0);
	} else {
		/* add only one */
		if (values[1] != NULL) {
			e_warning(authdb_event(request),
				  "Multiple values found for '%s', "
				  "using value '%s'", name, *values);
		}
		auth_request_set_userdb_field(request, name, *values);
	}
}

void auth_request_set_userdb_strlist(struct auth_request *request,
				     const ARRAY_TYPE(const_string) *strlist)
{
	if (!array_is_created(strlist))
		return;

	unsigned int i, count;
	const char *const *fields = array_get(strlist, &count);
	i_assert(count % 2 == 0);
	for (i = 0; i < count; i += 2) {
		auth_request_set_userdb_field(request, fields[i],
					      fields[i + 1]);
	}
}

static bool auth_request_proxy_is_self(struct auth_request *request)
{
	const char *port = NULL;

	/* check if the port is the same */
	port = auth_fields_find(request->fields.extra_fields, "port");
	if (port != NULL && !str_uint_equals(port, request->fields.local_port))
		return FALSE;
	/* don't check destuser. in some systems destuser is intentionally
	   changed to proxied connections, but that shouldn't affect the
	   proxying decision.

	   it's unlikely any systems would actually want to proxy a connection
	   to itself only to change the username, since it can already be done
	   without proxying by changing the "user" field. */
	return TRUE;
}

static bool
auth_request_proxy_ip_is_self(struct auth_request *request,
			      const struct ip_addr *ip)
{
	unsigned int i;

	if (net_ip_compare(ip, &request->fields.real_local_ip))
		return TRUE;

	for (i = 0; request->set->proxy_self_ips[i].family != 0; i++) {
		if (net_ip_compare(ip, &request->set->proxy_self_ips[i]))
			return TRUE;
	}
	return FALSE;
}

static void
auth_request_proxy_finish_ip(struct auth_request *request,
			     bool proxy_host_is_self)
{
	const struct auth_request_fields *fields = &request->fields;
	const char *host = auth_fields_find(request->fields.extra_fields, "host");
	const char *hostip = auth_fields_find(request->fields.extra_fields, "hostip");
	struct ip_addr ip1, ip2;

	/* This same check is repeated in login-common, since host might be set
	   after authentication by some other process. */
	if (host != NULL && hostip != NULL &&
	    net_addr2ip(host, &ip1) == 0) {
		/* hostip is already checked to be valid */
		if (net_addr2ip(hostip, &ip2) == 0 &&
		    !net_ip_compare(&ip1, &ip2)) {
			e_error(request->event,
				"host and hostip are both IPs, but they don't match");
			request->internal_failure = TRUE;
			auth_request_proxy_finish_failure(request);
			return;
		}
	}

	if (!auth_fields_exists(fields->extra_fields, "proxy_maybe")) {
		/* proxying */
	} else if (!proxy_host_is_self ||
		   !auth_request_proxy_is_self(request)) {
		/* proxy destination isn't ourself - proxy */
		auth_fields_remove(fields->extra_fields, "proxy_maybe");
		auth_fields_add(fields->extra_fields, "proxy", NULL, 0);
	} else {
		auth_request_proxy_finish_failure(request);
	}
}

static void
auth_request_proxy_dns_callback(const struct dns_lookup_result *result,
				struct auth_request_proxy_dns_lookup_ctx *ctx)
{
	/* We ended up here because dns_lookup_abort() was used */
	if (result->ret == EAI_CANCELED)
		return;
	struct auth_request *request = ctx->request;
	const char *host;
	unsigned int i;
	bool proxy_host_is_self;

	request->dns_lookup_ctx = NULL;
	ctx->dns_lookup = NULL;

	host = auth_fields_find(request->fields.extra_fields, "host");
	i_assert(host != NULL);

	if (result->ret != 0) {
		e_error(ctx->event, "DNS lookup for %s failed: %s",
			host, result->error);
		request->internal_failure = TRUE;
		auth_request_proxy_finish_failure(request);
	} else {
		if (result->msecs > AUTH_DNS_WARN_MSECS) {
			e_warning(ctx->event,
				  "DNS lookup for %s took %u.%03u s",
				  host, result->msecs/1000,
				  result->msecs % 1000);
		}
		auth_fields_add(request->fields.extra_fields, "hostip",
				net_ip2addr(&result->ips[0]), 0);
		proxy_host_is_self = FALSE;
		for (i = 0; i < result->ips_count; i++) {
			if (auth_request_proxy_ip_is_self(request,
							  &result->ips[i])) {
				proxy_host_is_self = TRUE;
				break;
			}
		}
		auth_request_proxy_finish_ip(request, proxy_host_is_self);
	}
	bool res = result->ret == 0 && !request->internal_failure;
	if (ctx->callback != NULL)
		ctx->callback(res, request);
	event_unref(&ctx->event);
	auth_request_unref(&request);
}

static int
auth_request_proxy_host_lookup(struct auth_request *request,
			       const char *host,
			       auth_request_proxy_cb_t *callback)
{
	struct auth *auth = auth_default_protocol();
	struct event *proxy_event;
	struct auth_request_proxy_dns_lookup_ctx *ctx;
	const char *value;
	unsigned int secs;

	proxy_event = event_create(request->event);
	event_set_append_log_prefix(proxy_event, "proxy: ");

	/* need to do dns lookup for the host */
	value = auth_fields_find(request->fields.extra_fields, "proxy_timeout");
	if (value != NULL) {
		if (str_to_uint(value, &secs) < 0) {
			e_error(proxy_event, "Invalid proxy_timeout value: %s",
				value);
		}
	}

	ctx = p_new(request->pool, struct auth_request_proxy_dns_lookup_ctx, 1);
	ctx->request = request;
	ctx->event = proxy_event;
	auth_request_ref(request);
	request->dns_lookup_ctx = ctx;

	if (dns_client_lookup(auth->dns_client, host, proxy_event,
			      auth_request_proxy_dns_callback, ctx,
			      &ctx->dns_lookup) < 0) {
		/* failed early */
		return -1;
	}
	ctx->callback = callback;
	return 0;
}

int auth_request_proxy_finish(struct auth_request *request,
			      auth_request_proxy_cb_t *callback)
{
	const char *host, *hostip;
	struct ip_addr ip;
	bool proxy_host_is_self;

	if (request->auth_only)
		return 1;
	if (!auth_fields_exists(request->fields.extra_fields, "proxy") &&
	    !auth_fields_exists(request->fields.extra_fields, "proxy_maybe"))
		return 1;

	host = auth_fields_find(request->fields.extra_fields, "host");
	if (host == NULL) {
		/* director can set the host. give it access to lip and lport
		   so it can also perform proxy_maybe internally */
		proxy_host_is_self = FALSE;
		if (request->fields.local_ip.family != 0) {
			auth_fields_add(request->fields.extra_fields, "lip",
				net_ip2addr(&request->fields.local_ip), 0);
		}
		if (request->fields.local_port != 0) {
			auth_fields_add(request->fields.extra_fields, "lport",
				dec2str(request->fields.local_port), 0);
		}
	} else if (net_addr2ip(host, &ip) == 0) {
		proxy_host_is_self =
			auth_request_proxy_ip_is_self(request, &ip);
	} else {
		hostip = auth_fields_find(request->fields.extra_fields,
					  "hostip");
		if (hostip != NULL && net_addr2ip(hostip, &ip) < 0) {
			e_error(request->event, "proxy: "
				"Invalid hostip in passdb: %s", hostip);
			return -1;
		}
		if (hostip == NULL) {
			/* asynchronous host lookup */
			return auth_request_proxy_host_lookup(request, host,
							      callback);
		}
		proxy_host_is_self =
			auth_request_proxy_ip_is_self(request, &ip);
	}

	auth_request_proxy_finish_ip(request, proxy_host_is_self);
	if (request->internal_failure)
		return -1;
	return 1;
}

void auth_request_proxy_finish_failure(struct auth_request *request)
{
	/* drop all proxying fields */
	auth_fields_remove(request->fields.extra_fields, "proxy");
	auth_fields_remove(request->fields.extra_fields, "proxy_maybe");
	auth_fields_remove(request->fields.extra_fields, "host");
	auth_fields_remove(request->fields.extra_fields, "hostip");
	auth_fields_remove(request->fields.extra_fields, "port");
	auth_fields_remove(request->fields.extra_fields, "destuser");
}

static void
log_password_failure(struct event *event, const char *plain_password,
		     const char *crypted_password, const char *scheme,
		     const struct password_generate_params *params)
{
	static bool scheme_ok = FALSE;
	string_t *str = t_str_new(256);
	const char *working_scheme;

	str_printfa(str, "%s(%s) != '%s'", scheme,
		    plain_password, crypted_password);

	if (!scheme_ok) {
		/* perhaps the scheme is wrong - see if we can find
		   a working one */
		working_scheme = password_scheme_detect(
			plain_password, crypted_password, params);
		if (working_scheme != NULL) {
			str_printfa(str, ", try %s scheme instead",
				    working_scheme);
		}
	}

	e_debug(event, "%s", str_c(str));
}

static void
auth_request_append_password(struct auth_request *request, string_t *str)
{
	const char *p, *log_type = request->set->verbose_passwords;
	unsigned int max_len = 1024;

	if (request->mech_password == NULL)
		return;

	p = strchr(log_type, ':');
	if (p != NULL) {
		if (str_to_uint(p+1, &max_len) < 0)
			i_unreached();
		log_type = t_strdup_until(log_type, p);
	}

	if (strcmp(log_type, "plain") == 0) {
		str_printfa(str, "(given password: %s)",
			    t_strndup(request->mech_password, max_len));
	} else if (strcmp(log_type, "sha1") == 0) {
		unsigned char sha1[SHA1_RESULTLEN];

		sha1_get_digest(request->mech_password,
				strlen(request->mech_password), sha1);
		str_printfa(str, "(SHA1 of given password: %s)",
			    t_strndup(binary_to_hex(sha1, sizeof(sha1)),
				      max_len));
	} else {
		i_unreached();
	}
}

void auth_request_log_password_mismatch(struct auth_request *request,
					struct event *event)
{
	auth_request_log_login_failure(request, event,
				       AUTH_LOG_MSG_PASSWORD_MISMATCH);
}

void auth_request_log_unknown_user(struct auth_request *request,
				   struct event *event)
{
	auth_request_log_login_failure(request, event, "unknown user");
}

void auth_request_log_login_failure(struct auth_request *request,
				    struct event *event, const char *message)
{
	string_t *str;

	if (strcmp(request->set->verbose_passwords, "no") == 0) {
		e_info(event, "%s", message);
		return;
	}

	/* make sure this gets logged */
	enum log_type orig_level = event_get_min_log_level(event);
	event_set_min_log_level(event, LOG_TYPE_INFO);

	str = t_str_new(128);
	str_append(str, message);
	str_append(str, " ");

	auth_request_append_password(request, str);

	if (request->userdb_lookup) {
		if (request->userdb->next != NULL)
			str_append(str, " - trying the next userdb");
	} else {
		if (request->passdb->next != NULL)
			str_append(str, " - trying the next passdb");
	}
	e_info(event, "%s", str_c(str));
	event_set_min_log_level(event, orig_level);
}

void auth_request_db_log_password_mismatch(struct auth_request *request)
{
	auth_request_log_login_failure(request, authdb_event(request),
				       AUTH_LOG_MSG_PASSWORD_MISMATCH);
}

void auth_request_db_log_unknown_user(struct auth_request *request)
{
	auth_request_log_login_failure(request, authdb_event(request),
				       "unknown user");
}

void auth_request_db_log_login_failure(struct auth_request *request,
				       const char *message)
{
	auth_request_log_login_failure(request, authdb_event(request),
				       message);
}

enum passdb_result
auth_request_password_verify(struct auth_request *request,
			     struct event *event,
			     const char *plain_password,
			     const char *crypted_password,
			     const char *scheme)
{
	return auth_request_password_verify_log(request, event, plain_password,
						crypted_password, scheme, TRUE);
}

enum passdb_result
auth_request_password_verify_log(struct auth_request *request,
				 struct event *event,
				 const char *plain_password,
				 const char *crypted_password,
				 const char *scheme,
				 bool log_password_mismatch)
{
	enum passdb_result result;
	const unsigned char *raw_password;
	size_t raw_password_size;
	const char *error;
	int ret;
	struct password_generate_params gen_params = {
		.user = request->fields.original_username,
		.rounds = 0
	};

	if (request->fields.skip_password_check) {
		/* passdb continue* rule after a successful authentication */
		return PASSDB_RESULT_OK;
	}

	if (request->passdb->set->deny) {
		/* this is a deny database, we don't care about the password */
		return PASSDB_RESULT_PASSWORD_MISMATCH;
	}

	if (auth_fields_exists(request->fields.extra_fields, "nopassword")) {
		e_debug(event, "Allowing any password");
		return PASSDB_RESULT_OK;
	}

	ret = password_decode(crypted_password, scheme,
			      &raw_password, &raw_password_size, &error);
	if (ret <= 0) {
		if (ret < 0) {
			e_error(event,
				"Password data is not valid for scheme %s: %s",
				scheme, error);
		} else {
			e_error(event, "Unknown scheme %s", scheme);
			return PASSDB_RESULT_SCHEME_NOT_AVAILABLE;
		}
		return PASSDB_RESULT_INTERNAL_FAILURE;
	}

	/* Use original_username since it may be important for some
	   password schemes (eg. digest-md5). Otherwise the username is used
	   only for logging purposes. */
	ret = password_verify(plain_password, &gen_params,
			      scheme, raw_password, raw_password_size, &error);
	if (ret < 0) {
		const char *password_str = request->set->debug_passwords ?
			t_strdup_printf(" '%s'", crypted_password) : "";
		e_error(event, "Invalid password%s in passdb: %s",
			password_str, error);
		result = PASSDB_RESULT_INTERNAL_FAILURE;
	} else if (ret == 0) {
		if (log_password_mismatch)
			auth_request_log_password_mismatch(request, event);
		result = PASSDB_RESULT_PASSWORD_MISMATCH;
	} else {
		result = PASSDB_RESULT_OK;
	}
	if (ret <= 0 && request->set->debug_passwords) T_BEGIN {
		log_password_failure(event, plain_password, crypted_password,
				     scheme, &gen_params);
	} T_END;
	return result;
}

enum passdb_result
auth_request_db_password_verify(struct auth_request *request,
				const char *plain_password,
				const char *crypted_password,
				const char *scheme)
{
	return auth_request_password_verify_log(
		request, authdb_event(request),
		plain_password, crypted_password, scheme, TRUE);
}

enum passdb_result
auth_request_db_password_verify_log(struct auth_request *request,
				    const char *plain_password,
				    const char *crypted_password,
				    const char *scheme,
				    bool log_password_mismatch)
{
	return auth_request_password_verify_log(
		request, authdb_event(request),
		plain_password, crypted_password, scheme,
		log_password_mismatch);
}

enum passdb_result auth_request_password_missing(struct auth_request *request)
{
	if (request->fields.skip_password_check) {
		/* This passdb wasn't used for authentication */
		return PASSDB_RESULT_OK;
	}
	e_info(authdb_event(request),
	       "No password returned (and no nopassword)");
	return PASSDB_RESULT_PASSWORD_MISMATCH;
}

void auth_request_refresh_last_access(struct auth_request *request)
{
	request->last_access = ioloop_time;
	if (request->to_abort != NULL)
		timeout_reset(request->to_abort);
}
