/* Copyright (c) 2003-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "strescape.h"
#include "base64.h"
#include "ostream.h"
#include "auth-client-private.h"
#include "strfuncs.h"

static void
auth_client_request_fail_conn_lost(struct auth_client_request *request);
static void
auth_client_request_handle_input(struct auth_client_request **_request,
				 enum auth_request_status status,
				 const char *base64_data,
				 const char *const *args, bool final);

static void
auth_server_send_new_request(struct auth_client_connection *conn,
			     struct auth_client_request *request,
			     const struct auth_request_info *info)
{
	string_t *str;

	str = t_str_new(512);
	str_printfa(str, "AUTH\t%u\t", request->id);
	str_append_tabescaped(str, info->mech);
	str_append(str, "\tprotocol=");
	str_append_tabescaped(str, info->protocol);

	event_add_str(request->event, "mechanism", info->mech);
	event_add_str(request->event, "protocol", info->protocol);

	str_append(str, "\tfinal-resp-ok");
	if ((info->flags & AUTH_REQUEST_FLAG_CONN_SECURED) != 0) {
		str_append(str, "\tsecured");
		if ((info->flags & AUTH_REQUEST_FLAG_CONN_SECURED_TLS) != 0) {
			str_append(str, "=tls");
			event_add_str(request->event, "transport", "TLS");
		} else {
			event_add_str(request->event, "transport", "secured");
		}
	} else {
		i_assert((info->flags & AUTH_REQUEST_FLAG_CONN_SECURED_TLS) == 0);
		event_add_str(request->event, "transport", "insecure");
	}
	if ((info->flags & AUTH_REQUEST_FLAG_NO_PENALTY) != 0)
		str_append(str, "\tno-penalty");
	if ((info->flags & AUTH_REQUEST_FLAG_VALID_CLIENT_CERT) != 0)
		str_append(str, "\tvalid-client-cert");
	if ((info->flags & AUTH_REQUEST_FLAG_DEBUG) != 0)
		str_append(str, "\tdebug");

	if (info->session_id != NULL) {
		str_append(str, "\tsession=");
		str_append_tabescaped(str, info->session_id);
		event_add_str(request->event, "session", info->session_id);
	}
	if (info->cert_username != NULL) {
		str_append(str, "\tcert_username=");
		str_append_tabescaped(str, info->cert_username);
		event_add_str(request->event, "certificate_user",
			      info->cert_username);
	}
	if (info->ssl_client_cert_fp != NULL) {
		str_append(str, "\tssl_client_cert_fp=");
		str_append_tabescaped(str, info->ssl_client_cert_fp);
		event_add_str(request->event, "ssl_client_cert_fp",
			      info->ssl_client_cert_fp);
	}
	if (info->ssl_client_cert_pubkey_fp != NULL) {
		str_append(str, "\tssl_client_cert_pubkey_fp=");
		str_append_tabescaped(str, info->ssl_client_cert_pubkey_fp);
		event_add_str(request->event, "ssl_client_cert_pubkey_fp",
			      info->ssl_client_cert_pubkey_fp);
	}
	if (info->local_ip.family != 0) {
		str_printfa(str, "\tlip=%s", net_ip2addr(&info->local_ip));
		event_add_ip(request->event, "local_ip", &info->local_ip);
	}
	if (info->remote_ip.family != 0) {
		str_printfa(str, "\trip=%s", net_ip2addr(&info->remote_ip));
		event_add_ip(request->event, "remote_ip", &info->remote_ip);
	}
	if (info->local_port != 0) {
		str_printfa(str, "\tlport=%u", info->local_port);
		event_add_int(request->event, "local_port", info->local_port);
	}
	if (info->remote_port != 0) {
		str_printfa(str, "\trport=%u", info->remote_port);
		event_add_int(request->event, "remote_port", info->remote_port);
	}
	if (info->ssl_ja3_hash != NULL) {
		str_append(str, "\tssl_ja3_hash=");
		str_append_tabescaped(str,info->ssl_ja3_hash);
		event_add_str(request->event, "ssl_ja3_hash", info->ssl_ja3_hash);
	}
	if (info->real_local_ip.family != 0) {
		event_add_ip(request->event, "real_local_ip",
			     &info->real_local_ip);
	}
	if (info->real_remote_ip.family != 0) {
		event_add_ip(request->event, "real_remote_ip",
			     &info->real_remote_ip);
	}
	if (info->real_local_port != 0) {
		event_add_int(request->event, "real_local_port",
			      info->real_local_port);
	}
	if (info->real_remote_port != 0) {
		event_add_int(request->event, "real_remote_port",
			      info->real_remote_port);
	}
	/* send the real_* variants only when they differ from the unreal
	   ones */
	if (info->real_local_ip.family != 0 &&
	    !net_ip_compare(&info->real_local_ip, &info->local_ip)) {
		str_printfa(str, "\treal_lip=%s",
			    net_ip2addr(&info->real_local_ip));
	}
	if (info->real_remote_ip.family != 0 &&
	    !net_ip_compare(&info->real_remote_ip, &info->remote_ip)) {
		str_printfa(str, "\treal_rip=%s",
			    net_ip2addr(&info->real_remote_ip));
	}
	if (info->real_local_port != 0 &&
	    info->real_local_port != info->local_port)
		str_printfa(str, "\treal_lport=%u", info->real_local_port);
	if (info->real_remote_port != 0 &&
	    info->real_remote_port != info->remote_port)
		str_printfa(str, "\treal_rport=%u", info->real_remote_port);
	if (info->local_name != NULL &&
	    *info->local_name != '\0') {
		str_append(str, "\tlocal_name=");
		str_append_tabescaped(str, info->local_name);
		event_add_str(request->event, "local_name", info->local_name);
	}
	if (info->ssl_cipher_bits != 0 && info->ssl_cipher != NULL) {
		event_add_str(request->event, "tls_cipher", info->ssl_cipher);
		event_add_int(request->event, "tls_cipher_bits",
			      info->ssl_cipher_bits);
		if (info->ssl_pfs != NULL) {
			event_add_str(request->event, "tls_pfs", info->ssl_pfs);
		}
	}
	if (info->ssl_protocol != NULL) {
		event_add_str(request->event, "tls_protocol",
			      info->ssl_protocol);
	}
	if (info->client_id != NULL &&
	    *info->client_id != '\0') {
		str_append(str, "\tclient_id=");
		str_append_tabescaped(str, info->client_id);
		event_add_str(request->event, "client_id", info->client_id);
	}
	if (info->forward_fields != NULL && info->forward_fields[0] != NULL) {
		string_t *forward = t_str_new(64);
		str_append_tabescaped(forward, info->forward_fields[0]);
		for (unsigned int i = 1; info->forward_fields[i] != NULL; i++) {
			str_append_c(forward, '\t');
			str_append_tabescaped(forward, info->forward_fields[i]);
		}
		str_append(str, "\tforward_fields=");
		str_append_tabescaped(str, str_c(forward));
	}
	if (array_is_created(&info->extra_fields)) {
		const char *const *fieldp;
		array_foreach(&info->extra_fields, fieldp) {
			str_append_c(str, '\t');
			str_append_tabescaped(str, *fieldp);
		}
	}
	if (info->initial_resp_base64 != NULL) {
		str_append(str, "\tresp=");
		str_append_tabescaped(str, info->initial_resp_base64);
	}
	str_append_c(str, '\n');

	struct event_passthrough *e =
		event_create_passthrough(request->event)->
		set_name("auth_client_request_started");
	e_debug(e->event(), "Started request");

	if (!conn->connected) {
		e_error(request->event,
			"Error sending request to auth server: connection lost");
		/* try to reconnect */
		request->to_fail = timeout_add_short(
			0, auth_client_request_fail_conn_lost, request);
	} else if (o_stream_send(conn->conn.output,
				 str_data(str), str_len(str)) < 0) {
		e_error(request->event,
			"Error sending request to auth server: %m");
	}
}

struct auth_client_request *
auth_client_request_new(struct auth_client *client,
			const struct auth_request_info *request_info,
			auth_request_callback_t *callback, void *context)
{
	struct auth_client_request *request;
	pool_t pool;

	pool = pool_alloconly_create("auth client request", 512);
	request = p_new(pool, struct auth_client_request, 1);
	request->pool = pool;
	request->conn = client->conn;
	request->flags = request_info->flags;
	request->final_status = AUTH_REQUEST_STATUS_CONTINUE;

	request->callback = callback;
	request->context = context;

	request->id =
		auth_client_connection_add_request(request->conn, request);
	request->created = ioloop_time;

	request->event = event_create(client->event);
	event_add_int(request->event, "id", request->id);
	event_set_append_log_prefix(request->event,
				    t_strdup_printf("request [%u]: ",
						    request->id));

	T_BEGIN {
		auth_server_send_new_request(request->conn,
					     request, request_info);
	} T_END;
	return request;
}

void auth_client_request_enable_channel_binding(
	struct auth_client_request *request,
	auth_channel_binding_callback_t *callback, void *context)
{
	request->cbinding_callback = callback;
	request->cbinding_context = context;
}

static void
call_callback(struct auth_client_request *request,
	      enum auth_request_status status,
	      const char *data_base64,
	      const char *const *args)
{
	auth_request_callback_t *callback = request->callback;

	if (status != AUTH_REQUEST_STATUS_CONTINUE)
		request->callback = NULL;
	callback(request, status, data_base64, args, request->context);
}

static void
auth_client_request_fail_conn_lost(struct auth_client_request *request)
{
	struct auth_client_connection *conn = request->conn;

	timeout_remove(&request->to_fail);

	struct event_passthrough *e =
		event_create_passthrough(request->event)->
		set_name("auth_client_request_finished");
	e->add_str("error", "Lost connection to server");
	e_debug(e->event(), "Lost connection to server");

	call_callback(request, AUTH_REQUEST_STATUS_INTERNAL_FAIL, NULL, NULL);
	conn->to = timeout_add_short(0, auth_server_reconnect_timeout, conn);
}

static void auth_client_request_free(struct auth_client_request **_request)
{
	struct auth_client_request *request = *_request;

	*_request = NULL;

	auth_client_connection_remove_request(request->conn, request);

	timeout_remove(&request->to_fail);
	timeout_remove(&request->to_final);
	event_unref(&request->event);
	pool_unref(&request->pool);
}

void auth_client_request_abort(struct auth_client_request **_request,
			       const char *reason)
{
	struct auth_client_request *request = *_request;

	*_request = NULL;

	struct event_passthrough *e =
		event_create_passthrough(request->event)->
		set_name("auth_client_request_finished");
	e->add_str("error", reason);
	e_debug(e->event(), "Aborted: %s", reason);

	auth_client_send_cancel(request->conn->client, request->id);
	call_callback(request, AUTH_REQUEST_STATUS_ABORT, NULL, NULL);
	auth_client_request_free(&request);
}

static void
auth_client_request_fail(struct auth_client_request **_request,
			 enum auth_request_status status,
			 const char *reason) ATTR_NULL(3)
{
	struct auth_client_request *request = *_request;
	const char *args[2] = { NULL, NULL };

	*_request = NULL;

	i_assert(status == AUTH_REQUEST_STATUS_FAIL ||
		 status == AUTH_REQUEST_STATUS_INTERNAL_FAIL);

	struct event_passthrough *e =
		event_create_passthrough(request->event)->
		set_name("auth_client_request_finished");
	e->add_str("error", reason);
	e_debug(e->event(), "Failed: %s", reason);

	if (reason != NULL)
		args[0] = t_strconcat("reason=", reason, NULL);

	auth_client_send_cancel(request->conn->client, request->id);
	call_callback(request, status, NULL, args);
	auth_client_request_free(&request);
}

unsigned int auth_client_request_get_id(struct auth_client_request *request)
{
	return request->id;
}

unsigned int
auth_client_request_get_server_pid(struct auth_client_request *request)
{
	return request->conn->server_pid;
}

const char *auth_client_request_get_cookie(struct auth_client_request *request)
{
	return request->conn->cookie;
}

bool auth_client_request_is_aborted(struct auth_client_request *request)
{
	return request->callback == NULL;
}

time_t auth_client_request_get_create_time(struct auth_client_request *request)
{
	return request->created;
}

static void
args_parse_user(struct auth_client_request *request, const char *key,
		const char *value)
{
	if (strcmp(key, "user") == 0)
		event_add_str(request->event, "user", value);
	else if (strcmp(key, "original_user") == 0)
		event_add_str(request->event, "original_user", value);
	else if (strcmp(key, "auth_user") == 0)
		event_add_str(request->event, "auth_user", value);
}

static void auth_client_request_final(struct auth_client_request *request)
{
	timeout_remove(&request->to_final);
	i_assert(request->final_status != AUTH_REQUEST_STATUS_CONTINUE);
	auth_client_request_handle_input(&request, request->final_status, NULL,
					 request->final_args, FALSE);
}

void auth_client_request_continue(struct auth_client_request *request,
                                  const char *data_base64)
{
	struct const_iovec iov[4];
	const char *prefix, *cbinding = "";

	if (request->final_status != AUTH_REQUEST_STATUS_CONTINUE) {
		request->to_final = timeout_add_short(
			0, auth_client_request_final, request);
		return;
	}

	if (!request->conn->connected) {
		e_error(request->event,
			"Error sending continue request to auth server: "
			"connection lost");
		return;
	}

	prefix = t_strdup_printf("CONT\t%u\t", request->id);
	if (request->cbinding_data != NULL) {
		const buffer_t *cbdata = request->cbinding_data;
		string_t *cbdata_b64 =
			t_base64_encode(0, 0, cbdata->data, cbdata->used);

		cbinding = t_strconcat("\tchannel_binding=",
				       str_c(cbdata_b64), NULL);
	}

	iov[0].iov_base = prefix;
	iov[0].iov_len = strlen(prefix);
	if (data_base64 == NULL) {
		/* Send out-of-band response */
		iov[1].iov_base = "#";
		iov[1].iov_len = 1;
	} else {
		/* Send normal SASL response */
		iov[1].iov_base = data_base64;
		iov[1].iov_len = strlen(data_base64);
	}
	iov[2].iov_base = cbinding;
	iov[2].iov_len = strlen(cbinding);
	iov[3].iov_base = "\n";
	iov[3].iov_len = 1;

	struct event_passthrough *e =
		event_create_passthrough(request->event)->
		set_name("auth_client_request_continued");
	e_debug(e->event(), "Continue request");

	if (o_stream_sendv(request->conn->conn.output, iov, 4) < 0) {
		e_error(request->event,
			"Error sending continue request to auth server: %m");
	}

	request->cbinding_data = NULL;
}

static void
auth_client_request_handle_input(struct auth_client_request **_request,
				 enum auth_request_status status,
				 const char *base64_data,
				 const char *const *args, bool final)
{
	struct auth_client_request *request = *_request;
	const char *const *tmp;
	const char *cbinding_type = NULL;
	struct event_passthrough *e;

	if (auth_client_request_is_aborted(request)) {
		/* aborted already */
		auth_client_request_free(_request);
		return;
	}

	switch (status) {
	case AUTH_REQUEST_STATUS_CONTINUE:
		e = event_create_passthrough(request->event)->
			set_name("auth_client_request_challenged");

		for (tmp = args; tmp != NULL && *tmp != NULL; tmp++) {
			if (str_begins(*tmp, "channel_binding=",
				       &cbinding_type))
				break;
		}
		args = NULL;
		break;
	default:
		e = event_create_passthrough(request->event)->
			set_name("auth_client_request_finished");

		for (tmp = args; tmp != NULL && *tmp != NULL; tmp++) {
			const char *key;
			const char *value;
			t_split_key_value_eq(*tmp, &key, &value);
			if (str_begins(key, "event_", &key))
				event_add_str(request->event, key, value);
			else
				args_parse_user(request, key, value);
		}
		break;
	}

	if (cbinding_type != NULL) {
		const buffer_t *data;
		const char *error;

		if (request->cbinding_callback == NULL) {
			auth_client_request_fail(
				&request, AUTH_REQUEST_STATUS_INTERNAL_FAIL,
				NULL);
			return;
		}
		if (request->cbinding_callback(cbinding_type,
					       request->cbinding_context,
					       &data, &error) < 0) {
			auth_client_request_fail(
				&request, AUTH_REQUEST_STATUS_FAIL,
				t_strdup_printf("Channel binding failed: %s",
						error));
			return;
		}

		request->cbinding_data = buffer_create_dynamic(request->pool,
							       data->used);
		buffer_append_buf(request->cbinding_data, data, 0, SIZE_MAX);
	}

	switch (status) {
	case AUTH_REQUEST_STATUS_OK:
		e_debug(e->event(), "Finished");
		break;
	case AUTH_REQUEST_STATUS_CONTINUE:
		if (base64_data == NULL) {
			/* Received a challenge outside the normal SASL
			   interaction. This is used to obtain out-of-band data
			   such as the channel binding when the auth service
			   messages exchanged as part of the normal SASL
			   interaction don't provide a means to piggy-back the
			   data, such as when channel binding data is needed for
			   composing the first server SASL challenge. Therefore,
			   we reply to the challenge immediately without doing
			   the SASL callback. The SASL response is empty, but
			   any requested fields are included.
			 */
			e_debug(e->event(), "Got out-of-band challenge");
			i_assert(!final);
			auth_client_request_continue(request, NULL);
			return;
		}
		if (!final)
			e_debug(e->event(), "Got challenge");
		else
			e_debug(e->event(), "Created final challenge");
		break;
	case AUTH_REQUEST_STATUS_FAIL:
		e->add_str("error", "Authentication failed");
		e_debug(e->event(), "Finished");
		break;
	case AUTH_REQUEST_STATUS_INTERNAL_FAIL:
		e->add_str("error", "Internal failure");
		e_debug(e->event(), "Finished");
		break;
	case AUTH_REQUEST_STATUS_ABORT:
		i_unreached();
	}

	call_callback(request, status, base64_data, args);
	if (status != AUTH_REQUEST_STATUS_CONTINUE)
		auth_client_request_free(_request);
}

void auth_client_request_server_input(struct auth_client_request **_request,
				      enum auth_request_status status,
				      const char *const *args)
{
	struct auth_client_request *request = *_request;
	const char *const *tmp, *base64_data = NULL;
	bool final = FALSE;

	if (auth_client_request_is_aborted(request)) {
		/* aborted already */
		auth_client_request_free(_request);
		return;
	}

	switch (status) {
	case AUTH_REQUEST_STATUS_FAIL:
	case AUTH_REQUEST_STATUS_OK:
	case AUTH_REQUEST_STATUS_INTERNAL_FAIL:
		for (tmp = args; *tmp != NULL; tmp++) {
			const char *key;
			const char *value;
			t_split_key_value_eq(*tmp, &key, &value);
			if (strcmp(key, "resp") == 0)
				base64_data = value;
		}
		if (base64_data == NULL ||
		    (status == AUTH_REQUEST_STATUS_OK &&
		     HAS_ALL_BITS(request->flags,
				  AUTH_REQUEST_FLAG_SUPPORT_FINAL_RESP)))
			break;
		request->final_status = status;
		request->final_args = p_strarray_dup(request->pool, args);
		status = AUTH_REQUEST_STATUS_CONTINUE;
		args = NULL;
		final = TRUE;
		break;
	case AUTH_REQUEST_STATUS_CONTINUE:
		base64_data = args[0];
		if (strcmp(base64_data, "#") == 0) {
			/* Out-of-band challenge */
			base64_data = NULL;
		}
		args++;
		break;
	case AUTH_REQUEST_STATUS_ABORT:
		i_unreached();
	}

	auth_client_request_handle_input(_request, status,
					 base64_data, args, final);
}

void auth_client_send_cancel(struct auth_client *client, unsigned int id)
{
	if (!client->conn->connected) {
		e_error(client->conn->conn.event,
			"Error sending cancel request to auth server: "
			"connection lost");
		return;
	}
	const char *str = t_strdup_printf("CANCEL\t%u\n", id);

	if (o_stream_send_str(client->conn->conn.output, str) < 0) {
		e_error(client->conn->conn.event,
			"Error sending cancel request to auth server: %m");
	}
}
