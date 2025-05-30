/* Copyright (c) 2002-2018 Dovecot authors, see the included COPYING file */

#include "login-common.h"
#include "array.h"
#include "md5.h"
#include "sasl-server.h"
#include "str.h"
#include "buffer.h"
#include "hex-binary.h"
#include "ioloop.h"
#include "istream.h"
#include "strfuncs.h"
#include "strescape.h"
#include "str-sanitize.h"
#include "anvil-client.h"
#include "auth-client.h"
#include "iostream-ssl.h"
#include "master-interface.h"
#include "login-client.h"
#include "client-common.h"

#include <unistd.h>

#define ERR_TOO_MANY_USERIP_CONNECTIONS \
	"Maximum number of connections from user+IP exceeded " \
	"(mail_max_userip_connections=%u)"

struct anvil_request {
	struct client *client;
	unsigned int auth_pid;
	unsigned char cookie[LOGIN_REQUEST_COOKIE_SIZE];
};

static bool
sasl_server_filter_mech(struct client *client, struct auth_mech_desc *mech,
			bool advertize)
{
	/* Allow plugins to filter and amend available mechanisms. */
	if (client->v.sasl_filter_mech != NULL &&
	    !client->v.sasl_filter_mech(client, mech))
		return FALSE;
	/* Disable anonymous mechanisms unless the protocol explicitly
	   allows anonymous login when configured. */
	if ((mech->flags & MECH_SEC_ANONYMOUS) != 0 &&
	    !login_binary->anonymous_login_acceptable)
		return FALSE;
	/* Don't advertize private mechanisms. */
	if (advertize && (mech->flags & MECH_SEC_PRIVATE) != 0)
		return FALSE;
	/* Only advertize this mechanism if either:
	   a) transport is secured
	   b) auth mechanism isn't plaintext
	   c) we allow insecure authentication

	   We don't completely disable mechanisms for which none of these points
	   apply, because we want to return a specific warning to the client
	   when such a mechanism is used.
	*/
	if (advertize && !client->connection_secured &&
	    !client->set->auth_allow_cleartext &&
	    (mech->flags & MECH_SEC_PLAINTEXT) != 0)
		return FALSE;
	/* Disable mechanisms that require channel binding when there is no TLS
	   layer (yet). */
	if (client->ssl_iostream == NULL &&
	    (mech->flags & MECH_SEC_CHANNEL_BINDING) != 0)
		return FALSE;

	return TRUE;
}

const struct auth_mech_desc *
sasl_server_get_advertised_mechs(struct client *client, unsigned int *count_r)
{
	const struct auth_mech_desc *mech;
	struct auth_mech_desc *ret_mech;
	unsigned int i, j, count;

	mech = auth_client_get_available_mechs(auth_client, &count);
	if (count == 0 || (!client->connection_secured &&
			   strcmp(client->ssl_server_set->ssl, "required") == 0)) {
		*count_r = 0;
		return NULL;
	}

	ret_mech = t_new(struct auth_mech_desc, count);
	for (i = j = 0; i < count; i++) {
		struct auth_mech_desc fmech = mech[i];

		if (!sasl_server_filter_mech(client, &fmech, TRUE))
			continue;

		ret_mech[j++] = fmech;
	}
	*count_r = j;
	return ret_mech;
}

const struct auth_mech_desc *
sasl_server_find_available_mech(struct client *client, const char *name)
{
	const struct auth_mech_desc *mech;
	struct auth_mech_desc fmech;

	mech = auth_client_find_mech(auth_client, name);
	if (mech == NULL)
		return NULL;

	fmech = *mech;
	if (!sasl_server_filter_mech(client, &fmech, FALSE))
		return NULL;
	if (memcmp(&fmech, mech, sizeof(fmech)) != 0) {
		struct auth_mech_desc *nmech = t_new(struct auth_mech_desc, 1);

		*nmech = fmech;
		mech = nmech;
	}
	return mech;
}

static enum auth_request_flags
client_get_auth_flags(struct client *client)
{
        enum auth_request_flags auth_flags = 0;

	if (client->ssl_iostream != NULL &&
	    ssl_iostream_has_valid_client_cert(client->ssl_iostream))
		auth_flags |= AUTH_REQUEST_FLAG_VALID_CLIENT_CERT;
	if (client->connection_tls_secured)
		auth_flags |= AUTH_REQUEST_FLAG_CONN_SECURED_TLS;
	if (client->connection_secured)
		auth_flags |= AUTH_REQUEST_FLAG_CONN_SECURED;
	if (login_binary->sasl_support_final_reply)
		auth_flags |= AUTH_REQUEST_FLAG_SUPPORT_FINAL_RESP;
	return auth_flags;
}

static void ATTR_NULL(3, 4)
call_client_callback(struct client *client, enum sasl_server_reply reply,
		     const char *data, const char *const *args)
{
	sasl_server_callback_t *sasl_callback;

	i_assert(reply != SASL_SERVER_REPLY_CONTINUE);

	sasl_callback = client->sasl_callback;
	client->sasl_callback = NULL;

	sasl_callback(client, reply, data, args);
	/* NOTE: client may be destroyed now */
}

static void
login_callback(const struct login_reply *reply, void *context)
{
	struct client *client = context;
	enum sasl_server_reply sasl_reply = SASL_SERVER_REPLY_MASTER_FAILED;
	const char *data = NULL;

	client->master_tag = 0;
	client->authenticating = FALSE;
	if (reply != NULL) {
		switch (reply->status) {
		case LOGIN_REPLY_STATUS_OK:
			sasl_reply = SASL_SERVER_REPLY_SUCCESS;
			data = client->auth_success_data;
			break;
		case LOGIN_REPLY_STATUS_INTERNAL_ERROR:
			sasl_reply = SASL_SERVER_REPLY_MASTER_FAILED;
			break;
		}
		client->mail_pid = reply->mail_pid;
	} else {
		auth_client_send_cancel(auth_client, client->master_auth_id);
	}
	call_client_callback(client, sasl_reply, data, NULL);
}

static int master_send_request(struct anvil_request *anvil_request)
{
	struct client *client = anvil_request->client;
	struct login_client_request_params params;
	struct login_request req;
	const unsigned char *data;
	size_t size;
	buffer_t *buf;
	const char *session_id = client_get_session_id(client);
	int fd;
	bool close_fd;

	if (client_get_plaintext_fd(client, &fd, &close_fd) < 0)
		return -1;

	i_zero(&req);
	req.auth_pid = anvil_request->auth_pid;
	req.auth_id = client->master_auth_id;
	req.local_ip = client->local_ip;
	req.remote_ip = client->ip;
	req.local_port = client->local_port;
	req.remote_port = client->remote_port;
	req.client_pid = getpid();
	if (client->multiplex_output != NULL)
		req.flags |= LOGIN_REQUEST_FLAG_MULTIPLEX_OUTPUT;
	if (client->ssl_iostream != NULL &&
	    ssl_iostream_get_compression(client->ssl_iostream) != NULL)
		req.flags |= LOGIN_REQUEST_FLAG_TLS_COMPRESSION;
	if (client->end_client_tls_secured)
		req.flags |= LOGIN_REQUEST_FLAG_END_CLIENT_SECURED_TLS;
	if (HAS_ALL_BITS(client->auth_flags, SASL_SERVER_AUTH_FLAG_IMPLICIT))
		req.flags |= LOGIN_REQUEST_FLAG_IMPLICIT;
	memcpy(req.cookie, anvil_request->cookie, sizeof(req.cookie));

	buf = t_buffer_create(256);
	/* session ID */
	buffer_append(buf, session_id, strlen(session_id)+1);
	/* protocol specific data (e.g. IMAP tag) */
	buffer_append(buf, client->master_data_prefix,
		      client->master_data_prefix_len);
	/* buffered client input */
	data = i_stream_get_data(client->input, &size);
	buffer_append(buf, data, size);
	req.data_size = buf->used;
	i_stream_skip(client->input, size);

	client->auth_finished = ioloop_timeval;

	i_zero(&params);
	params.client_fd = fd;
	params.socket_path = client->postlogin_socket_path;
	params.request = req;
	params.data = buf->data;
	login_client_request(login_client_list, &params, login_callback,
			     client, &client->master_tag);
	if (close_fd)
		i_close_fd(&fd);
	return 0;
}

static void ATTR_NULL(1)
anvil_lookup_callback(const struct anvil_reply *reply,
		      struct anvil_request *req)
{
	struct client *client = req->client;
	const struct login_settings *set = client->set;
	const char *errmsg;
	unsigned int conn_count;
	enum sasl_server_reply sasl_reply = SASL_SERVER_REPLY_SUCCESS;

	client->anvil_query = NULL;
	client->anvil_request = NULL;

	conn_count = 0;
	if (reply == NULL)
		;
	else if (reply->error != NULL) {
		e_error(client->event, "mail_max_userip_connections lookup failed - skipping: %s",
			reply->error);
	} else if (str_to_uint(reply->reply, &conn_count) < 0)
		i_fatal("Received invalid reply from anvil: %s", reply->reply);

	/* reply=NULL if we didn't need to do anvil lookup,
	   reply->error != NULL if the anvil lookup failed.
	   Allow failed anvil lookups in. */
	if (reply == NULL || reply->error != NULL ||
	    conn_count < set->mail_max_userip_connections) {
		if (master_send_request(req) < 0)
			sasl_reply = SASL_SERVER_REPLY_MASTER_FAILED;
		errmsg = NULL; /* client will see internal error */
	} else {
		sasl_reply = SASL_SERVER_REPLY_MASTER_FAILED_LIMIT;
		errmsg = t_strdup_printf(ERR_TOO_MANY_USERIP_CONNECTIONS,
					 set->mail_max_userip_connections);
	}
	if (sasl_reply != SASL_SERVER_REPLY_SUCCESS) {
		client->authenticating = FALSE;
		auth_client_send_cancel(auth_client, client->master_auth_id);
		call_client_callback(client, sasl_reply, errmsg, NULL);
	}
	i_free(req);
}

static void
anvil_check_too_many_connections(struct client *client)
{
	struct anvil_request *req;
	const char *query;
	buffer_t buf;

	req = i_new(struct anvil_request, 1);
	req->client = client;
	req->auth_pid = client->auth_server_pid;

	buffer_create_from_data(&buf, req->cookie, sizeof(req->cookie));
	if (strlen(client->auth_conn_cookie) == LOGIN_REQUEST_COOKIE_SIZE*2)
		(void)hex_to_binary(client->auth_conn_cookie, &buf);

	if (client->virtual_user == NULL ||
	    client->set->mail_max_userip_connections == 0) {
		anvil_lookup_callback(NULL, req);
		return;
	}

	query = t_strconcat("LOOKUP\t",
			    str_tabescape(client->virtual_user), "\t",
			    login_binary->protocol, "\t",
			    net_ip2addr(&client->ip), NULL);
	client->anvil_request = req;
	client->anvil_query =
		anvil_client_query(anvil, query,
				   ANVIL_DEFAULT_LOOKUP_TIMEOUT_MSECS,
				   anvil_lookup_callback, req);
}

static bool
sasl_server_check_login(struct client *client)
{
	if (client->v.sasl_check_login != NULL &&
	    !client->v.sasl_check_login(client))
		return FALSE;
	if (client->auth_anonymous &&
	    !login_binary->anonymous_login_acceptable) {
		sasl_server_auth_failed(client,
			"Anonymous login denied",
			AUTH_CLIENT_FAIL_CODE_ANONYMOUS_DENIED);
		return FALSE;
	}
	return TRUE;
}

static bool
args_parse_user(struct client *client, const char *key, const char *value)
{
	if (strcmp(key, "user") == 0) {
		i_free(client->virtual_user);
		i_free_and_null(client->virtual_user_orig);
		i_free_and_null(client->virtual_auth_user);
		client->virtual_user = i_strdup(value);
		event_add_str(client->event, "user", client->virtual_user);
	} else if (strcmp(key, "original_user") == 0) {
		i_free(client->virtual_user_orig);
		client->virtual_user_orig = i_strdup(value);
	} else if (strcmp(key, "auth_user") == 0) {
		i_free(client->virtual_auth_user);
		client->virtual_auth_user = i_strdup(value);
	} else {
		return FALSE;
	}
	return TRUE;
}

static int
sasl_server_channel_binding(const char *type, void *context,
			    const buffer_t **data_r, const char **error_r)
{
	struct client *client = context;

	return ssl_iostream_get_channel_binding(client->ssl_iostream,
						type, data_r, error_r);
}

static void
sasl_server_auth_success_finish(struct client *client, bool nologin,
				const char *data, const char *const *args)
{
	if (nologin) {
		client->authenticating = FALSE;
		call_client_callback(client, SASL_SERVER_REPLY_SUCCESS,
				     data, args);
	} else if (!sasl_server_check_login(client)) {
		i_assert(!client->authenticating);
	} else {
		client->auth_success_data =
			p_strdup(client->preproxy_pool, data);
		anvil_check_too_many_connections(client);
	}
}

static void
authenticate_callback(struct auth_client_request *request,
		      enum auth_request_status status, const char *data_base64,
		      const char *const *args, void *context)
{
	struct client *client = context;
	bool nologin;
	unsigned int i;

	if (!client->authenticating) {
		/* client aborted */
		i_assert(status < 0);
		return;
	}
	client->auth_client_continue_pending = FALSE;

	i_assert(client->auth_request == request);
	switch (status) {
	case AUTH_REQUEST_STATUS_CONTINUE:
		/* continue */
		client->sasl_callback(client, SASL_SERVER_REPLY_CONTINUE,
				      data_base64, NULL);
		break;
	case AUTH_REQUEST_STATUS_OK:
		client->auth_request = NULL;
		client->master_auth_id = auth_client_request_get_id(request);
		client->auth_server_pid =
			auth_client_request_get_server_pid(request);
		i_free(client->auth_conn_cookie);
		client->auth_conn_cookie =
			i_strdup(auth_client_request_get_cookie(request));

		client->auth_successes++;
		client->auth_passdb_args = p_strarray_dup(client->pool, args);
		client->postlogin_socket_path = NULL;

		nologin = FALSE;
		for (i = 0; args[i] != NULL; i++) {
			const char *key, *value;
			t_split_key_value_eq(args[i], &key, &value);

			if (args_parse_user(client, key, value))
				continue;

			if (strcmp(key, "postlogin_socket") == 0) {
				client->postlogin_socket_path =
					p_strdup(client->pool, value);
			} else if (strcmp(key, "nologin") == 0 ||
				   strcmp(key, "proxy") == 0) {
				/* user can't login */
				nologin = TRUE;
			} else if (strcmp(key, "anonymous") == 0) {
				client->auth_anonymous = TRUE;
			} else if (str_begins(args[i], "event_", &key)) {
				event_add_str(client->event_auth, key, value);
			}
		}

		sasl_server_auth_success_finish(client, nologin,
						data_base64, args);
		break;
	case AUTH_REQUEST_STATUS_INTERNAL_FAIL:
		client->auth_process_comm_fail = TRUE;
		/* fall through */
	case AUTH_REQUEST_STATUS_FAIL:
	case AUTH_REQUEST_STATUS_ABORT:
		client->auth_request = NULL;

		if (args != NULL) {
			/* parse our username if it's there */
			for (i = 0; args[i] != NULL; i++) {
				const char *key, *value;
				t_split_key_value_eq(args[i], &key, &value);
				if (args_parse_user(client, key, value))
					continue;
			}
		}

		client->authenticating = FALSE;
		call_client_callback(client, SASL_SERVER_REPLY_AUTH_FAILED,
				     NULL, args);
		break;
	}
}

static bool
get_cert_username(struct client *client, const char **username_r,
		  const char **error_r)
{
	/* this was proxied connection, so we use the name here */
	if (client->client_cert_common_name != NULL) {
		*username_r = client->client_cert_common_name;
		return TRUE;
	}

	/* no SSL */
	if (client->ssl_iostream == NULL) {
		*username_r = NULL;
		return TRUE;
	}

	/* no client certificate */
	if (!ssl_iostream_has_valid_client_cert(client->ssl_iostream)) {
		*username_r = NULL;
		return TRUE;
	}

	/* get peer name */
	const char *username = ssl_iostream_get_peer_username(client->ssl_iostream);

	/* if we wanted peer name, but it was not there, fail */
	if (client->set->auth_ssl_username_from_cert &&
	    (username == NULL || *username == '\0')) {
		if (client->set->auth_ssl_require_client_cert) {
			*error_r = "Missing username in certificate";
			return FALSE;
		}
	}

	*username_r = username;
	return TRUE;
}

int sasl_server_auth_request_info_fill(struct client *client,
				       struct auth_request_info *info_r,
				       const char **client_error_r)
{
	const char *error;

	i_zero(info_r);
	info_r->protocol = login_binary->protocol;
	info_r->session_id = client_get_session_id(client);

	if (!get_cert_username(client, &info_r->cert_username, &error)) {
		e_error(client->event,
			"Cannot get username from certificate: %s", error);
		*client_error_r = "Unable to validate certificate";
		return -1;
	}

	if (client->ssl_iostream != NULL) {
		unsigned char hash[MD5_RESULTLEN];
		info_r->ssl_cipher = ssl_iostream_get_cipher(client->ssl_iostream,
							 &info_r->ssl_cipher_bits);
		info_r->ssl_pfs = ssl_iostream_get_pfs(client->ssl_iostream);
		info_r->ssl_protocol =
			ssl_iostream_get_protocol_name(client->ssl_iostream);
		const char *ja3 = ssl_iostream_get_ja3(client->ssl_iostream);
		/* See https://github.com/salesforce/ja3#how-it-works for reason
		   why md5 is used. */
		if (ja3 != NULL) {
			md5_get_digest(ja3, strlen(ja3), hash);
			info_r->ssl_ja3_hash = binary_to_hex(hash, sizeof(hash));
		}

		if (*client->ssl_set->ssl_peer_certificate_fingerprint_hash != '\0') {
			int ret = ssl_iostream_get_peer_cert_fingerprint(
				client->ssl_iostream, &info_r->ssl_client_cert_fp,
				&info_r->ssl_client_cert_pubkey_fp,
				&error);
			if (ret < 0) {
				e_error(client->event,
					"Cannot get client certificate fingerprints: %s",
					error);
				*client_error_r = "Unable to validate certificate";
				return -1;
			}
		}
	}
	info_r->flags = client_get_auth_flags(client);
	info_r->local_ip = client->local_ip;
	info_r->remote_ip = client->ip;
	info_r->local_port = client->local_port;
	info_r->local_name = client->local_name;
	info_r->remote_port = client->remote_port;
	info_r->real_local_ip = client->real_local_ip;
	info_r->real_remote_ip = client->real_remote_ip;
	info_r->real_local_port = client->real_local_port;
	info_r->real_remote_port = client->real_remote_port;
	if (client->client_id != NULL)
		info_r->client_id = str_c(client->client_id);
	if (array_is_created(&client->forward_fields)) {
		array_append_zero(&client->forward_fields);
		array_pop_back(&client->forward_fields);
		info_r->forward_fields = array_front(&client->forward_fields);
	}
	return 0;
}

void sasl_server_auth_begin(struct client *client, const char *mech_name,
			    enum sasl_server_auth_flags flags,
			    const char *initial_resp_base64,
			    sasl_server_callback_t *callback)
{
	struct auth_request_info info;
	const struct auth_mech_desc *mech;
	bool private = HAS_ALL_BITS(flags, SASL_SERVER_AUTH_FLAG_PRIVATE);
	const char *client_error;

	i_assert(auth_client_is_connected(auth_client));

	client->auth_attempts++;
	client->auth_aborted_by_client = FALSE;
	client->authenticating = TRUE;
	client->master_auth_id = 0;
	if (client->auth_first_started.tv_sec == 0)
		client->auth_first_started = ioloop_timeval;
	i_free(client->auth_mech_name);
	client->auth_mech_name = str_ucase(i_strdup(mech_name));
	client->auth_anonymous = FALSE;
	client->auth_success_data = NULL;
	client->auth_flags = flags;
	client->sasl_callback = callback;

	mech = sasl_server_find_available_mech(client, mech_name);
	if (mech == NULL ||
	    ((mech->flags & MECH_SEC_PRIVATE) != 0 && !private)) {
		sasl_server_auth_failed(client,
			"Unsupported authentication mechanism.",
			AUTH_CLIENT_FAIL_CODE_MECH_INVALID);
		return;
	}

	i_assert(!private || (mech->flags & MECH_SEC_PRIVATE) != 0);

	if (!client->connection_secured && !client->set->auth_allow_cleartext &&
	    (mech->flags & MECH_SEC_PLAINTEXT) != 0) {
		client_notify_status(client, TRUE,
			 "cleartext authentication not allowed "
			 "without SSL/TLS, but your client did it anyway. "
			 "If anyone was listening, the password was exposed.");
		sasl_server_auth_failed(client,
			 AUTH_CLEARTEXT_DISABLED_MSG,
			 AUTH_CLIENT_FAIL_CODE_MECH_SSL_REQUIRED);
		return;
	}

	if (sasl_server_auth_request_info_fill(client, &info, &client_error) < 0) {
		sasl_server_auth_failed(client, client_error,
					AUTH_CLIENT_FAIL_CODE_AUTHZFAILED);
		return;
	}
	info.mech = mech->name;
	info.initial_resp_base64 = initial_resp_base64;
	client->auth_request =
		auth_client_request_new(auth_client, &info,
					authenticate_callback, client);
	auth_client_request_enable_channel_binding(client->auth_request,
						   sasl_server_channel_binding,
						   client);
}

static void ATTR_NULL(2, 3)
sasl_server_auth_cancel(struct client *client, const char *reason,
			const char *code, enum sasl_server_reply reply)
{
	i_assert(client->authenticating);

	if (reason != NULL) {
		const char *auth_name =
			str_sanitize(client->auth_mech_name, MAX_MECH_NAME);
		e_info(client->event_auth, "Authenticate %s failed: %s",
		       auth_name, reason);
	}

	client->authenticating = FALSE;
	if (client->auth_request != NULL)
		auth_client_request_abort(&client->auth_request, reason);
	if (client->master_auth_id != 0)
		auth_client_send_cancel(auth_client, client->master_auth_id);

	if (code != NULL) {
		const char *args[2];

		args[0] = t_strconcat("code=", code, NULL);
		args[1] = NULL;
		call_client_callback(client, reply, reason, args);
		return;
	}

	call_client_callback(client, reply, reason, NULL);
}

void sasl_server_auth_continue(struct client *client, const char *response)
{
	auth_client_request_continue(client->auth_request, response);
}

void sasl_server_auth_failed(struct client *client, const char *reason,
			     const char *code)
{
	sasl_server_auth_cancel(client, reason, code,
				SASL_SERVER_REPLY_AUTH_FAILED);
}

void sasl_server_auth_abort(struct client *client)
{
	client->auth_aborted_by_client = TRUE;
	if (client->anvil_query != NULL) {
		anvil_client_query_abort(anvil, &client->anvil_query);
		i_free(client->anvil_request);
	}
	sasl_server_auth_cancel(client, "Aborted", NULL,
				SASL_SERVER_REPLY_AUTH_ABORTED);
}
