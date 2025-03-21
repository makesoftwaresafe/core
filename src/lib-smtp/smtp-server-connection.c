/* Copyright (c) 2013-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "llist.h"
#include "array.h"
#include "str.h"
#include "guid.h"
#include "base64.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "iostream.h"
#include "connection.h"
#include "iostream-rawlog.h"
#include "iostream-ssl.h"
#include "settings.h"
#include "master-service.h"
#include "master-service-ssl.h"

#include "smtp-syntax.h"
#include "smtp-reply-parser.h"
#include "smtp-command-parser.h"
#include "smtp-server-private.h"
#include "ssl-settings.h"

const char *const smtp_server_state_names[] = {
	"GREETING",
	"XCLIENT",
	"HELO",
	"STARTTLS",
	"AUTH",
	"READY",
	"MAIL FROM",
	"RCPT TO",
	"DATA"
};

/*
 * Connection
 */

static void smtp_server_connection_input(struct connection *_conn);
static int smtp_server_connection_output(struct smtp_server_connection *conn);
static void
smtp_server_connection_disconnect(struct smtp_server_connection *conn,
				  const char *reason) ATTR_NULL(2);

static void
smtp_server_connection_update_stats(struct smtp_server_connection *conn)
{
	if (conn->conn.input != NULL)
		conn->stats.input = conn->conn.input->v_offset;
	if (conn->conn.output != NULL)
		conn->stats.output = conn->conn.output->offset;
	connection_update_counters(&conn->conn);
}

const struct smtp_server_stats *
smtp_server_connection_get_stats(struct smtp_server_connection *conn)
{
	smtp_server_connection_update_stats(conn);
	return &conn->stats;
}

static bool
smtp_server_connection_check_pipeline(struct smtp_server_connection *conn)
{
	unsigned int pipeline = conn->command_queue_count;

	if (conn->command_queue_tail != NULL) {
		i_assert(pipeline > 0);
		if (conn->command_queue_tail->state ==
		    SMTP_SERVER_COMMAND_STATE_SUBMITTED_REPLY)
			pipeline--;
	}

	if (pipeline >= conn->set.max_pipelined_commands) {
		e_debug(conn->event, "Command pipeline is full "
			"(pipelined commands %u > limit %u)",
			pipeline, conn->set.max_pipelined_commands);
		return FALSE;
	}
	return TRUE;
}

void smtp_server_connection_input_halt(struct smtp_server_connection *conn)
{
	connection_input_halt(&conn->conn);
}

void smtp_server_connection_input_resume(struct smtp_server_connection *conn)
{
	struct smtp_server_command *cmd;
	bool cmd_locked = FALSE;

	if (conn->conn.io == NULL) {
		/* Only resume when we actually can */
		if (conn->input_locked || conn->input_broken ||
			conn->disconnected)
			return;
		if (!smtp_server_connection_check_pipeline(conn))
			return;

		/* Is queued command still blocking input? */
		cmd = conn->command_queue_head;
		while (cmd != NULL) {
			if (cmd->input_locked || cmd->pipeline_blocked) {
				cmd_locked = TRUE;
				break;
			}
			cmd = cmd->next;
		}
		if (cmd_locked)
			return;

		/* Restore input handler */
		connection_input_resume(&conn->conn);
	}

	if (conn->conn.io != NULL &&
		i_stream_have_bytes_left(conn->conn.input)) {
		io_set_pending(conn->conn.io);
	}
}

void smtp_server_connection_input_lock(struct smtp_server_connection *conn)
{
	conn->input_locked = TRUE;
	smtp_server_connection_input_halt(conn);
}

void smtp_server_connection_input_unlock(struct smtp_server_connection *conn)
{
	conn->input_locked = FALSE;
	smtp_server_connection_input_resume(conn);
}

#undef smtp_server_connection_input_capture
void smtp_server_connection_input_capture(
	struct smtp_server_connection *conn,
	smtp_server_input_callback_t *callback, void *context)
{
	i_assert(!conn->input_broken && !conn->disconnected);
	connection_input_halt(&conn->conn);
	i_stream_set_input_pending(conn->conn.input, TRUE);
	conn->conn.io = io_add_istream(conn->conn.input, *callback, context);
}

static void
smtp_server_connection_update_rawlog(struct smtp_server_connection *conn)
{
	struct stat st;

	if (conn->set.rawlog_dir == NULL)
		return;

	if (!conn->rawlog_checked) {
		conn->rawlog_checked = TRUE;
		if (stat(conn->set.rawlog_dir, &st) == 0)
			conn->rawlog_enabled = TRUE;
	}
	if (conn->rawlog_enabled) {
		iostream_rawlog_create(conn->set.rawlog_dir,
				       &conn->conn.input, &conn->conn.output);
	}
}

static void
smtp_server_connection_streams_changed(struct smtp_server_connection *conn)
{
	smtp_server_connection_update_rawlog(conn);
	smtp_command_parser_set_stream(conn->smtp_parser, conn->conn.input);

	o_stream_set_flush_callback(conn->conn.output,
		smtp_server_connection_output, conn);
	o_stream_set_flush_pending(conn->conn.output, TRUE);
}

void smtp_server_connection_set_streams(struct smtp_server_connection *conn,
					struct istream *input,
					struct ostream *output)
{
	struct istream *old_input = conn->conn.input;
	struct ostream *old_output = conn->conn.output;

	i_assert(conn->created_from_streams);

	conn->conn.input = input;
	i_stream_ref(conn->conn.input);

	conn->conn.output = output;
	o_stream_ref(conn->conn.output);
	o_stream_set_no_error_handling(conn->conn.output, TRUE);

	i_stream_unref(&old_input);
	o_stream_unref(&old_output);

	smtp_server_connection_streams_changed(conn);
}

void smtp_server_connection_set_ssl_streams(struct smtp_server_connection *conn,
					    struct istream *input,
					    struct ostream *output)
{
	conn->ssl_secured = TRUE;
	conn->set.capabilities &= ENUM_NEGATE(SMTP_CAPABILITY_STARTTLS);

	smtp_server_connection_set_streams(conn, input, output);
}

static void
smtp_server_connection_idle_timeout(struct smtp_server_connection *conn)
{
	smtp_server_connection_terminate(
		&conn, "4.4.2", "Disconnected for inactivity");
}

void smtp_server_connection_timeout_stop(struct smtp_server_connection *conn)
{
	if (conn->to_idle != NULL) {
		e_debug(conn->event, "Timeout stop");

		timeout_remove(&conn->to_idle);
	}
}

void smtp_server_connection_timeout_start(struct smtp_server_connection *conn)
{
	if (conn->disconnected)
		return;

	if (conn->to_idle == NULL &&
		conn->set.max_client_idle_time_msecs > 0) {
		e_debug(conn->event, "Timeout start");

		conn->to_idle = timeout_add(
			conn->set.max_client_idle_time_msecs,
			smtp_server_connection_idle_timeout, conn);
	}
}

void smtp_server_connection_timeout_reset(struct smtp_server_connection *conn)
{
	if (conn->to_idle != NULL)
		timeout_reset(conn->to_idle);
}

static void
smtp_server_connection_timeout_update(struct smtp_server_connection *conn)
{
	struct smtp_server_command *cmd = conn->command_queue_head;

	if (cmd == NULL) {
		smtp_server_connection_timeout_start(conn);
		return;
	}

	switch (cmd->state) {
	case SMTP_SERVER_COMMAND_STATE_NEW:
		smtp_server_connection_timeout_start(conn);
		break;
	case SMTP_SERVER_COMMAND_STATE_PROCESSING:
		if (cmd->input_captured) {
			/* Command updates timeout internally */
			return;
		}
		smtp_server_connection_timeout_stop(conn);
		break;
	case SMTP_SERVER_COMMAND_STATE_SUBMITTED_REPLY:
	case SMTP_SERVER_COMMAND_STATE_READY_TO_REPLY:
		smtp_server_connection_timeout_stop(conn);
		break;
	case SMTP_SERVER_COMMAND_STATE_FINISHED:
	case SMTP_SERVER_COMMAND_STATE_ABORTED:
		i_unreached();
	}
}

static void smtp_server_connection_ready(struct smtp_server_connection *conn)
{
	conn->raw_input = conn->conn.input;
	conn->raw_output = conn->conn.output;

	conn->connect_succeeded = TRUE;

	smtp_server_connection_update_rawlog(conn);

	conn->smtp_parser = smtp_command_parser_init(conn->conn.input,
						     &conn->set.command_limits);
	o_stream_set_flush_callback(conn->conn.output,
				    smtp_server_connection_output, conn);

	o_stream_cork(conn->conn.output);
	if (conn->set.no_greeting) {
		/* Don't send greeting or login reply. */
	} else if (conn->authenticated) {
		/* RFC 4954, Section 4:
		   Should the client successfully complete the exchange, the
		   SMTP server issues a 235 reply. */
		smtp_server_connection_send_line(
			conn, "235 2.7.0 Logged in.");
	} else {
		smtp_server_connection_send_line(
			conn, "220 %s %s", conn->set.hostname,
			conn->set.login_greeting);
	}
	if (!conn->corked)
		o_stream_uncork(conn->conn.output);
}

static void smtp_server_connection_destroy(struct connection *_conn)
{
	struct smtp_server_connection *conn =
		(struct smtp_server_connection *)_conn;

	smtp_server_connection_disconnect(conn, NULL);
	smtp_server_connection_unref(&conn);
}

static bool
smtp_server_connection_handle_command(struct smtp_server_connection *conn,
				      const char *cmd_name,
				      const char *cmd_params)
{
	struct smtp_server_connection *tmp_conn = conn;
	struct smtp_server_command *cmd;
	bool finished;

	cmd = smtp_server_command_new(tmp_conn, cmd_name);

	smtp_server_command_ref(cmd);

	smtp_server_connection_ref(tmp_conn);
	smtp_server_command_execute(cmd, cmd_params);
	if (!smtp_server_connection_unref(&tmp_conn)) {
		/* The command start callback managed to get this connection
		   destroyed */
		smtp_server_command_unref(&cmd);
		return FALSE;
	}

	if (conn->command_queue_head == cmd)
		(void)smtp_server_command_next_to_reply(&cmd);

	smtp_server_connection_timeout_update(conn);

	finished = !cmd->input_locked;
	return (!smtp_server_command_unref(&cmd) || finished);
}

static int
smtp_server_connection_sni_callback(const char *name, const char **error_r,
				    void *context)
{
	struct smtp_server_connection *conn = context;
	struct ssl_iostream_context *ssl_ctx;
	const struct ssl_settings *ssl_set;
	const struct ssl_server_settings *ssl_server_set;

	event_add_str(conn->event, "local_name", name);
	i_free(conn->local_name);
	conn->local_name = i_strdup(name);
	if (ssl_server_settings_get(conn->event, &ssl_set, &ssl_server_set,
				    error_r) < 0)
		return -1;
	if (conn->local_name != NULL && *conn->local_name != '\0')
		conn->set.hostname = conn->local_name;
	if (conn->callbacks->conn_tls_sni_callback != NULL &&
	    conn->callbacks->conn_tls_sni_callback(conn->context, name,
						   error_r) < 0) {
		settings_free(ssl_set);
		settings_free(ssl_server_set);
		return -1;
	}

	ssl_server_settings_to_iostream_set(ssl_set, ssl_server_set,
					    &conn->set.ssl);

	int ret;
	if ((ret = ssl_iostream_server_context_cache_get(conn->set.ssl, &ssl_ctx,
							 error_r)) < 0) {
		settings_free(ssl_set);
		settings_free(ssl_server_set);
		return -1;
	}
	settings_free(ssl_set);
	settings_free(ssl_server_set);
	if (ret == 1) {
		const char *application_protocol = smtp_protocol_name(conn->set.protocol);
		const char *const names[] = {
			application_protocol,
			NULL
		};
		ssl_iostream_context_set_application_protocols(ssl_ctx, names);
	}
	ssl_iostream_change_context(conn->ssl_iostream, ssl_ctx);
	ssl_iostream_context_unref(&ssl_ctx);
	return 0;
}

int smtp_server_connection_ssl_init(struct smtp_server_connection *conn)
{
	struct ssl_iostream_context *ssl_ctx;
	const char *error;
	int ret;

	e_debug(conn->event, "Starting SSL handshake");

	if (conn->raw_input != conn->conn.input) {
		/* Recreate rawlog after STARTTLS */
		i_stream_ref(conn->raw_input);
		o_stream_ref(conn->raw_output);
		i_stream_destroy(&conn->conn.input);
		o_stream_destroy(&conn->conn.output);
		conn->conn.input = conn->raw_input;
		conn->conn.output = conn->raw_output;
	}

	smtp_server_connection_input_halt(conn);
	if (conn->set.ssl == NULL) {
		const struct ssl_iostream_server_autocreate_parameters parameters = {
			.event_parent = conn->event,
		};
		ret = io_stream_autocreate_ssl_server(&parameters,
			&conn->conn.input, &conn->conn.output,
			&conn->ssl_iostream, &error);
	} else if (ssl_iostream_server_context_cache_get(conn->set.ssl,
							 &ssl_ctx, &error) < 0)
		ret = -1;
	else {
		ret = io_stream_create_ssl_server(ssl_ctx, conn->event,
			&conn->conn.input, &conn->conn.output,
			&conn->ssl_iostream, &error);
		ssl_iostream_context_unref(&ssl_ctx);
	}
	if (ret < 0) {
		e_error(conn->event,
			"Couldn't initialize SSL server for %s: %s",
			conn->conn.name, error);
		return -1;
	}
	ssl_iostream_set_sni_callback(
		conn->ssl_iostream, smtp_server_connection_sni_callback, conn);
	smtp_server_connection_input_resume(conn);

	if (ssl_iostream_handshake(conn->ssl_iostream) < 0) {
		e_error(conn->event, "SSL handshake failed: %s",
			ssl_iostream_get_last_error(conn->ssl_iostream));
		return -1;
	}

	conn->ssl_secured = TRUE;
	conn->set.capabilities &= ENUM_NEGATE(SMTP_CAPABILITY_STARTTLS);

	if (conn->connect_succeeded)
		smtp_server_connection_streams_changed(conn);
	else if (conn->ssl_start &&
		 ssl_iostream_is_handshaked(conn->ssl_iostream))
		smtp_server_connection_ready(conn);

	return 0;
}

static void
smtp_server_connection_handle_input(struct smtp_server_connection *conn)
{
	struct smtp_server_command *pending_command;
	enum smtp_command_parse_error error_code;
	const char *cmd_name, *cmd_params, *error;
	int ret;

	/* Check whether we are continuing a command */
	pending_command = NULL;
	if (conn->command_queue_tail != NULL) {
		pending_command =
			((conn->command_queue_tail->state ==
			  SMTP_SERVER_COMMAND_STATE_SUBMITTED_REPLY) ?
			 conn->command_queue_tail : NULL);
	}

	smtp_server_connection_timeout_reset(conn);

	/* Parse commands */
	ret = 1;
	while (!conn->closing && !conn->input_locked && ret != 0) {
		while ((ret = smtp_command_parse_next(
			conn->smtp_parser, &cmd_name, &cmd_params,
			&error_code, &error)) > 0) {

			if (pending_command != NULL) {
				/* Previous command is now fully read and ready
				   to reply */
				smtp_server_command_ready_to_reply(pending_command);
				pending_command = NULL;
			}

			e_debug(conn->event, "Received new command: %s %s",
				cmd_name, cmd_params);

			conn->stats.command_count++;

			/* Handle command (cmd may be destroyed after this) */
			if (!smtp_server_connection_handle_command(conn,
				cmd_name, cmd_params))
				return;

			if (conn->disconnected)
				return;
			/* Last command locked the input; stop trying to read
			   more. */
			if (conn->input_locked)
				break;
			/* Client indicated it will close after this command;
			   stop trying to read more. */
			if (conn->closing)
				break;

			if (!smtp_server_connection_check_pipeline(conn)) {
				smtp_server_connection_input_halt(conn);
				return;
			}

			if (conn->command_queue_tail != NULL) {
				pending_command =
					((conn->command_queue_tail->state ==
					  SMTP_SERVER_COMMAND_STATE_SUBMITTED_REPLY) ?
					 conn->command_queue_tail : NULL);
			}
		}

		if (ret < 0 && conn->conn.input->eof) {
			const char *error =
				i_stream_get_disconnect_reason(conn->conn.input);
			e_debug(conn->event, "Remote closed connection: %s",
				error);

			if (conn->command_queue_head == NULL ||
			    conn->command_queue_head->state <
			    SMTP_SERVER_COMMAND_STATE_SUBMITTED_REPLY) {
				/* No pending commands or unfinished
				   command; close */
				smtp_server_connection_close(&conn, error);
			} else {
				/* A command is still processing;
				   only drop input io for now */
				conn->input_broken = TRUE;
				smtp_server_connection_input_halt(conn);
			}
			return;
		}

		if (ret < 0) {
			struct smtp_server_command *cmd;

			e_debug(conn->event,
				"Client sent invalid command: %s", error);

			switch (error_code) {
			case SMTP_COMMAND_PARSE_ERROR_BROKEN_COMMAND:
				conn->input_broken = TRUE;
				/* fall through */
			case SMTP_COMMAND_PARSE_ERROR_BAD_COMMAND:
				cmd = smtp_server_command_new_invalid(conn);
				smtp_server_command_fail(
					cmd, 500, "5.5.2",
					"Invalid command syntax");
				break;
			case SMTP_COMMAND_PARSE_ERROR_LINE_TOO_LONG:
				cmd = smtp_server_command_new_invalid(conn);
				smtp_server_command_fail(
					cmd, 500, "5.5.2", "Line too long");
				break;
			case SMTP_COMMAND_PARSE_ERROR_DATA_TOO_LARGE:
				/* Command data size exceeds the absolute limit;
				   i.e. beyond which we don't even want to skip
				   data anymore. The command error is usually
				   already submitted by the application and sent
				   to the client. */
				smtp_server_connection_close(&conn,
					"Command data size exceeds absolute limit");
				return;
			case SMTP_COMMAND_PARSE_ERROR_BROKEN_STREAM:
				smtp_server_connection_close(&conn, error);
				return;
			default:
				i_unreached();
			}
		}

		if (conn->disconnected)
			return;
		if (conn->input_broken || conn->closing) {
			smtp_server_connection_input_halt(conn);
			return;
		}

		if (ret == 0 && pending_command != NULL &&
		    !smtp_command_parser_pending_data(conn->smtp_parser)) {
			/* Previous command is now fully read and ready to
			   reply */
			smtp_server_command_ready_to_reply(pending_command);
		}
	}
}

static void smtp_server_connection_input(struct connection *_conn)
{
	struct smtp_server_connection *conn =
		(struct smtp_server_connection *)_conn;
	int ret;

	i_assert(!conn->input_broken);

	if (conn->handling_input)
		return;

	smtp_server_connection_timeout_reset(conn);

	if (conn->ssl_start && conn->ssl_iostream == NULL) {
		if (smtp_server_connection_ssl_init(conn) < 0) {
			smtp_server_connection_close(&conn,
				"SSL Initialization failed");
			return;
		}
	}
	i_assert(!conn->halted);

	/* If connection is established over secure line, wait for TLS
	   handshake to finish. */
	if (conn->ssl_iostream != NULL &&
	    !ssl_iostream_is_handshaked(conn->ssl_iostream)) {
		/* Finish SSL negotiating by reading from input stream. */
		while ((ret = i_stream_read(conn->conn.input)) > 0 ||
		       ret == -2) {
			if (ssl_iostream_is_handshaked(conn->ssl_iostream))
				break;
		}
		if (ret == -1) {
			int stream_errno = conn->conn.input->stream_errno;

			/* Failed somehow. */
			i_assert(ret != -2);
			const char *error = t_strdup_printf(
				"SSL handshaking with %s failed: "
				"read(%s) failed: %s",
				_conn->name,
				i_stream_get_name(conn->conn.input),
				(stream_errno != 0 ?
				 i_stream_get_error(conn->conn.input) : "EOF"));
			e_error(conn->event, "%s", error);
			smtp_server_connection_close(&conn, error);
			return;
		}
		if (!ssl_iostream_is_handshaked(conn->ssl_iostream)) {
			/* Not finished. */
			i_assert(ret == 0);
			return;
		}
		if (conn->halted) {
			smtp_server_connection_input_lock(conn);
			return;
		}
	}

	if (!conn->connect_succeeded &&
	    (conn->ssl_iostream == NULL ||
	     ssl_iostream_is_handshaked(conn->ssl_iostream)))
		smtp_server_connection_ready(conn);

	if (!smtp_server_connection_check_pipeline(conn)) {
		smtp_server_connection_input_halt(conn);
		return;
	}

	smtp_server_connection_ref(conn);
	conn->handling_input = TRUE;
	if (conn->callbacks != NULL &&
	    conn->callbacks->conn_cmd_input_pre != NULL)
		conn->callbacks->conn_cmd_input_pre(conn->context);
	smtp_server_connection_handle_input(conn);
	if (conn->callbacks != NULL &&
	    conn->callbacks->conn_cmd_input_post != NULL)
		conn->callbacks->conn_cmd_input_post(conn->context);
	conn->handling_input = FALSE;

	/* Handle output errors from immediate replies sent to client
	   (normal replies are exclusively sent in output handler). */
	if (conn->conn.output != NULL && conn->conn.output->closed)
               	smtp_server_connection_handle_output_error(conn);
	smtp_server_connection_unref(&conn);
}

bool smtp_server_connection_pending_command_data(
	struct smtp_server_connection *conn)
{
	if (conn->smtp_parser == NULL)
		return FALSE;
	return smtp_command_parser_pending_data(conn->smtp_parser);
}

/*
 * Command reply handling
 */

void smtp_server_connection_handle_output_error(
	struct smtp_server_connection *conn)
{
	smtp_server_connection_close(&conn,
		o_stream_get_disconnect_reason(conn->conn.output));
}

static bool
smtp_server_connection_next_reply(struct smtp_server_connection *conn)
{
	struct smtp_server_command *cmd;

	cmd = conn->command_queue_head;
	if (cmd == NULL) {
		/* No commands pending */
		e_debug(conn->event, "No more commands pending");
		return FALSE;
	}

	return smtp_server_command_send_replies(cmd);
}

void smtp_server_connection_cork(struct smtp_server_connection *conn)
{
	conn->corked = TRUE;
	if (conn->conn.output != NULL)
		o_stream_cork(conn->conn.output);
}

void smtp_server_connection_uncork(struct smtp_server_connection *conn)
{
	conn->corked = FALSE;
	if (conn->conn.output != NULL) {
		if (o_stream_uncork_flush(conn->conn.output) < 0) {
			smtp_server_connection_handle_output_error(conn);
			return;
		}
		smtp_server_connection_trigger_output(conn);
	}
}

static void
smtp_server_connection_send_replies(struct smtp_server_connection *conn)
{
	/* Send more replies until no more replies remain, the output
	   blocks again, or the connection is closed */
	while (!conn->disconnected && smtp_server_connection_next_reply(conn));

	smtp_server_connection_timeout_update(conn);

	/* Accept more commands if possible */
	smtp_server_connection_input_resume(conn);
}

int smtp_server_connection_flush(struct smtp_server_connection *conn)
{
	struct ostream *output = conn->conn.output;
	int ret;

	if ((ret = o_stream_flush(output)) <= 0) {
		if (ret < 0)
			smtp_server_connection_handle_output_error(conn);
		return ret;
	}
	return 1;
}

static int smtp_server_connection_output(struct smtp_server_connection *conn)
{
	int ret;

	e_debug(conn->event, "Sending replies");

	smtp_server_connection_ref(conn);
	o_stream_cork(conn->conn.output);
	ret = smtp_server_connection_flush(conn);
	if (ret > 0) {
		smtp_server_connection_timeout_reset(conn);
		smtp_server_connection_send_replies(conn);
	}
	if (ret >= 0 && !conn->corked && conn->conn.output != NULL)
		ret = o_stream_uncork_flush(conn->conn.output);
	if (conn->conn.output != NULL && conn->conn.output->closed) {
		smtp_server_connection_handle_output_error(conn);
		ret = -1;
	}
	smtp_server_connection_unref(&conn);
	return ret;
}

void smtp_server_connection_trigger_output(struct smtp_server_connection *conn)
{
	if (conn->conn.output != NULL) {
		e_debug(conn->event, "Trigger output");
		o_stream_set_flush_pending(conn->conn.output, TRUE);
	}
}

/*
 *
 */

static struct connection_settings smtp_server_connection_set = {
	.input_max_size = SIZE_MAX,
	.output_max_size = SIZE_MAX,
	.client = FALSE,
	.log_connection_id = TRUE,
};

static const struct connection_vfuncs smtp_server_connection_vfuncs = {
	.destroy = smtp_server_connection_destroy,
	.input = smtp_server_connection_input,
};

struct connection_list *smtp_server_connection_list_init(void)
{
	return connection_list_init(&smtp_server_connection_set,
				    &smtp_server_connection_vfuncs);
}

static struct event *
smtp_server_connection_event_create(struct smtp_server *server,
				    const struct smtp_server_settings *set)
{
	struct event *conn_event;

	if (set != NULL && set->event_parent != NULL) {
		conn_event = event_create(set->event_parent);
		smtp_server_event_init(server, conn_event);
	} else
		conn_event = event_create(server->event);
	event_set_append_log_prefix(conn_event, t_strdup_printf(
		"%s-server: ", smtp_protocol_name(server->set.protocol)));
	event_set_forced_debug(conn_event, (set != NULL && set->debug));

	return conn_event;
}

static void
smtp_server_connection_update_event(struct smtp_server_connection *conn)
{
	event_add_str(conn->event, "connection_id", conn->session_id);
	event_add_str(conn->event, "session", conn->session_id);
}

static void
smtp_server_connection_init_session(struct smtp_server_connection *conn)
{
	guid_128_t guid;
	string_t *session_id;

	session_id = t_str_new(30);
	guid_128_generate(guid);
	base64_encode(guid, sizeof(guid), session_id);

	/* drop trailing "==" */
	i_assert(str_c(session_id)[str_len(session_id)-2] == '=');
	str_truncate(session_id, str_len(session_id)-2);

	conn->session_id = i_strdup(str_c(session_id));
}

static struct smtp_server_connection * ATTR_NULL(5, 6)
smtp_server_connection_alloc(struct smtp_server *server,
			     const struct smtp_server_settings *set,
			     int fd_in, int fd_out,
			     const struct smtp_server_callbacks *callbacks,
			     void *context)
{
	struct smtp_server_connection *conn;
	pool_t pool;

	pool = pool_alloconly_create("smtp server", 2048);
	conn = p_new(pool, struct smtp_server_connection, 1);
	conn->pool = pool;
	conn->refcount = 1;
	conn->server = server;
	conn->callbacks = callbacks;
	conn->context = context;

	/* Merge settings with global server settings */
	conn->set = server->set;
	if (set != NULL) {
		conn->set.protocol = server->set.protocol;
		if (set->rawlog_dir != NULL && *set->rawlog_dir != '\0')
			conn->set.rawlog_dir = p_strdup(pool, set->rawlog_dir);

		if (set->ssl != NULL) {
			conn->set.ssl = set->ssl;
			pool_ref(conn->set.ssl->pool);
		}

		if (set->hostname != NULL && *set->hostname != '\0')
			conn->set.hostname = p_strdup(pool, set->hostname);
		if (set->login_greeting != NULL &&
			*set->login_greeting != '\0') {
			conn->set.login_greeting =
				p_strdup(pool, set->login_greeting);
		}
		if (set->capabilities != 0)
			conn->set.capabilities = set->capabilities;
		conn->set.workarounds |= set->workarounds;

		if (set->max_client_idle_time_msecs > 0) {
			conn->set.max_client_idle_time_msecs =
				set->max_client_idle_time_msecs;
		}
		if (set->max_pipelined_commands > 0) {
			conn->set.max_pipelined_commands =
				set->max_pipelined_commands;
		}
		if (set->max_bad_commands > 0) {
			conn->set.max_bad_commands = set->max_bad_commands;
		}
		if (set->max_recipients > 0)
			conn->set.max_recipients = set->max_recipients;
		smtp_command_limits_merge(&conn->set.command_limits,
					  &set->command_limits);

		conn->set.max_message_size = set->max_message_size;
		if (set->max_message_size == 0 ||
		    set->max_message_size == UOFF_T_MAX) {
			conn->set.command_limits.max_data_size = UOFF_T_MAX;
		} else if (conn->set.command_limits.max_data_size != 0) {
			/* Explicit limit given */
		} else if (set->max_message_size >
			(UOFF_T_MAX - SMTP_SERVER_DEFAULT_MAX_SIZE_EXCESS_LIMIT)) {
			/* Very high limit */
			conn->set.command_limits.max_data_size = UOFF_T_MAX;
		} else {
			/* Absolute maximum before connection is closed in DATA
			   command */
			conn->set.command_limits.max_data_size =
				set->max_message_size +
					SMTP_SERVER_DEFAULT_MAX_SIZE_EXCESS_LIMIT;
		}

		if (set->mail_param_extensions != NULL) {
			conn->set.mail_param_extensions =
				p_strarray_dup(pool, set->mail_param_extensions);
		}
		if (set->rcpt_param_extensions != NULL) {
			conn->set.rcpt_param_extensions =
				p_strarray_dup(pool, set->rcpt_param_extensions);
		}
		if (set->xclient_extensions != NULL) {
			conn->set.xclient_extensions =
				p_strarray_dup(pool, set->xclient_extensions);
		}

		if (set->socket_send_buffer_size > 0) {
			conn->set.socket_send_buffer_size =
				set->socket_send_buffer_size;
		}
		if (set->socket_recv_buffer_size > 0) {
			conn->set.socket_recv_buffer_size =
				set->socket_recv_buffer_size;
		}

		conn->set.tls_required =
			conn->set.tls_required || set->tls_required;
		conn->set.auth_optional =
			conn->set.auth_optional || set->auth_optional;
		conn->set.mail_path_allow_broken =
			conn->set.mail_path_allow_broken ||
				set->mail_path_allow_broken;
		conn->set.rcpt_domain_optional =
			conn->set.rcpt_domain_optional ||
				set->rcpt_domain_optional;
		conn->set.no_greeting =
			conn->set.no_greeting || set->no_greeting;
		conn->set.debug = conn->set.debug || set->debug;
	}

	if (set != NULL && set->mail_param_extensions != NULL) {
		const char *const *extp;

		p_array_init(&conn->mail_param_extensions, pool,
			     str_array_length(set->mail_param_extensions) + 8);
		for (extp = set->mail_param_extensions; *extp != NULL; extp++) {
			const char *ext = p_strdup(pool, *extp);
			array_push_back(&conn->mail_param_extensions, &ext);
		}
		array_append_zero(&conn->mail_param_extensions);
	}
	if (set != NULL && set->rcpt_param_extensions != NULL) {
		const char *const *extp;

		p_array_init(&conn->rcpt_param_extensions, pool,
			     str_array_length(set->rcpt_param_extensions) + 8);
		for (extp = set->rcpt_param_extensions; *extp != NULL; extp++) {
			const char *ext = p_strdup(pool, *extp);
			array_push_back(&conn->rcpt_param_extensions, &ext);
		}
		array_append_zero(&conn->rcpt_param_extensions);
	}

	net_set_nonblock(fd_in, TRUE);
	if (fd_in != fd_out)
		net_set_nonblock(fd_out, TRUE);
	(void)net_set_tcp_nodelay(fd_out, TRUE);

	set = &conn->set;
	if (set->socket_send_buffer_size > 0 &&
	    net_set_send_buffer_size(fd_out,
			             set->socket_send_buffer_size) < 0) {
		e_error(conn->event,
			"net_set_send_buffer_size(%zu) failed: %m",
			set->socket_send_buffer_size);
	}
	if (set->socket_recv_buffer_size > 0 &&
	    net_set_recv_buffer_size(fd_in,
				     set->socket_recv_buffer_size) < 0) {
		e_error(conn->event,
			"net_set_recv_buffer_size(%zu) failed: %m",
			set->socket_recv_buffer_size);
	}

	smtp_server_connection_init_session(conn);

	return conn;
}

static void smtp_server_connection_created(struct smtp_server_connection *conn)
{
	conn->raw_input = conn->conn.input;
	conn->raw_output = conn->conn.output;

	/* Halt input until started */
	smtp_server_connection_halt(conn);

	e_debug(conn->event, "Connection created");
}

struct smtp_server_connection *
smtp_server_connection_create(
	struct smtp_server *server, int fd_in, int fd_out,
	const struct ip_addr *remote_ip, in_port_t remote_port,
	bool ssl_start, const struct smtp_server_settings *set,
	const struct smtp_server_callbacks *callbacks, void *context)
{
	struct smtp_server_connection *conn;
	struct event *conn_event;

	conn = smtp_server_connection_alloc(server, set, fd_in, fd_out,
					    callbacks, context);
	conn_event = smtp_server_connection_event_create(server, set);
	conn->conn.event_parent = conn_event;
	connection_init_server_ip(server->conn_list, &conn->conn, NULL,
				  fd_in, fd_out, remote_ip, remote_port);
	conn->event = conn->conn.event;
	smtp_server_connection_update_event(conn);
	event_unref(&conn_event);

	conn->ssl_start = ssl_start;
	if (ssl_start)
		conn->set.capabilities &= ENUM_NEGATE(SMTP_CAPABILITY_STARTTLS);

	smtp_server_connection_created(conn);

	return conn;
}

struct smtp_server_connection *
smtp_server_connection_create_from_streams(
	struct smtp_server *server,
	struct istream *input, struct ostream *output,
	const struct ip_addr *remote_ip, in_port_t remote_port,
	const struct smtp_server_settings *set,
	const struct smtp_server_callbacks *callbacks, void *context)
{
	struct smtp_server_connection *conn;
	struct event *conn_event;
	int fd_in, fd_out;

	fd_in = i_stream_get_fd(input);
	fd_out = o_stream_get_fd(output);
	i_assert(fd_in >= 0);
	i_assert(fd_out >= 0);

	conn = smtp_server_connection_alloc(server, set, fd_in, fd_out,
					    callbacks, context);
	if (remote_ip != NULL && remote_ip->family != 0)
		conn->conn.remote_ip = *remote_ip;
	if (remote_port != 0)
		conn->conn.remote_port = remote_port;
	conn_event = smtp_server_connection_event_create(server, set);
	conn->conn.event_parent = conn_event;
	connection_init_from_streams(server->conn_list,	&conn->conn, NULL,
				     input, output);
	conn->created_from_streams = TRUE;
	conn->event = conn->conn.event;
	smtp_server_connection_update_event(conn);
	event_unref(&conn_event);

	smtp_server_connection_created(conn);

	return conn;
}

void smtp_server_connection_ref(struct smtp_server_connection *conn)
{
	conn->refcount++;
}

static const char *
smtp_server_connection_get_disconnect_reason(
	struct smtp_server_connection *conn)
{
	const char *err;

	if (conn->ssl_iostream != NULL &&
	    !ssl_iostream_is_handshaked(conn->ssl_iostream)) {
		err = ssl_iostream_get_last_error(conn->ssl_iostream);
		if (err != NULL) {
			return t_strdup_printf(
				"TLS handshaking failed: %s", err);
		}
	}

	return io_stream_get_disconnect_reason(conn->conn.input,
					       conn->conn.output);
}

static void
smtp_server_connection_disconnect(struct smtp_server_connection *conn,
				  const char *reason)
{
	struct smtp_server_command *cmd, *cmd_next;

	if (conn->disconnected)
		return;
	conn->disconnected = TRUE;

	if (reason == NULL)
		reason = smtp_server_connection_get_disconnect_reason(conn);
	else
		reason = t_str_oneline(reason);

	cmd = conn->command_queue_head;
	if (cmd != NULL && cmd->reg != NULL) {
		/* Unfinished command - include it in the reason string */
		reason = t_strdup_printf("%s (unfinished %s command)",
			reason, cmd->reg->name);
	}
	if (!conn->set.no_state_in_reason) {
		reason = t_strdup_printf("%s (state=%s)", reason,
			smtp_server_state_names[conn->state.state]);
	}

	e_debug(conn->event, "Disconnected: %s", reason);

	/* Preserve statistics */
	smtp_server_connection_update_stats(conn);

	/* Drop transaction */
	smtp_server_connection_reset_state(conn);

	/* Clear command queue */
	cmd = conn->command_queue_head;
	while (cmd != NULL) {
		cmd_next = cmd->next;
		smtp_server_command_abort(&cmd);
		cmd = cmd_next;
	}

	smtp_server_connection_timeout_stop(conn);
	if (conn->conn.output != NULL)
		o_stream_uncork(conn->conn.output);
	if (conn->smtp_parser != NULL)
		smtp_command_parser_deinit(&conn->smtp_parser);
	ssl_iostream_destroy(&conn->ssl_iostream);
	settings_free(conn->set.ssl);

	if (conn->callbacks != NULL &&
	    conn->callbacks->conn_disconnect != NULL) {
		/* The callback may close the fd, so remove IO before that */
		io_remove(&conn->conn.io);
		conn->callbacks->conn_disconnect(conn->context, reason);
	}

	if (!conn->created_from_streams)
		connection_disconnect(&conn->conn);
	else {
		conn->conn.fd_in = conn->conn.fd_out = -1;
		io_remove(&conn->conn.io);
		i_stream_unref(&conn->conn.input);
		o_stream_unref(&conn->conn.output);
	}
}

bool smtp_server_connection_unref(struct smtp_server_connection **_conn)
{
	struct smtp_server_connection *conn = *_conn;

	*_conn = NULL;

	i_assert(conn->refcount > 0);
	if (--conn->refcount > 0)
		return TRUE;

	smtp_server_connection_disconnect(conn, NULL);

	e_debug(conn->event, "Connection destroy");

	connection_deinit(&conn->conn);

	if (conn->callbacks != NULL && conn->callbacks->conn_free != NULL)
		conn->callbacks->conn_free(conn->context);

	i_free(conn->proxy_helo);
	i_free(conn->helo_domain);
	i_free(conn->username);
	i_free(conn->session_id);
	i_free(conn->client_transport);
	i_free(conn->local_name);
	event_unref(&conn->next_trans_event);
	pool_unref(&conn->pool);
	return FALSE;
}

void smtp_server_connection_send_line(struct smtp_server_connection *conn,
				      const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);

	T_BEGIN {
		string_t *str;

		str = t_str_new(256);
		str_vprintfa(str, fmt, args);

		e_debug(conn->event, "Sent: %s", str_c(str));

		str_append(str, "\r\n");
		o_stream_nsend(conn->conn.output, str_data(str), str_len(str));
	} T_END;
	va_end(args);
}

void smtp_server_connection_reply_lines(struct smtp_server_connection *conn,
				        unsigned int status,
					const char *enh_code,
					const char *const *text_lines)
{
	struct smtp_reply reply;

	i_zero(&reply);
	reply.status = status;
	reply.text_lines = text_lines;

	if (!smtp_reply_parse_enhanced_code(
		enh_code, &reply.enhanced_code, NULL))
		reply.enhanced_code = SMTP_REPLY_ENH_CODE(status / 100, 0, 0);

	T_BEGIN {
		string_t *str;

		e_debug(conn->event, "Sent: %s", smtp_reply_log(&reply));

		str = t_str_new(256);
		smtp_reply_write(str, &reply);
		o_stream_nsend(conn->conn.output, str_data(str), str_len(str));
	} T_END;
}

void smtp_server_connection_reply_immediate(
	struct smtp_server_connection *conn,
	unsigned int status, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	T_BEGIN {
		string_t *str;

		str = t_str_new(256);
		str_printfa(str, "%03u ", status);
		str_vprintfa(str, fmt, args);

		e_debug(conn->event, "Sent: %s", str_c(str));

		str_append(str, "\r\n");
		o_stream_nsend(conn->conn.output, str_data(str), str_len(str));
	} T_END;
	va_end(args);

	/* Send immediately */
	if (o_stream_is_corked(conn->conn.output)) {
		o_stream_uncork(conn->conn.output);
		o_stream_cork(conn->conn.output);
	}
}

void smtp_server_connection_login(struct smtp_server_connection *conn,
				  const char *username, const char *helo,
				  const unsigned char *pdata,
				  unsigned int pdata_len, bool ssl_secured)
{
	i_assert(!conn->started);

	conn->set.capabilities &= ENUM_NEGATE(SMTP_CAPABILITY_STARTTLS);
	i_free(conn->username);
	conn->username = i_strdup(username);
	if (helo != NULL && *helo != '\0') {
		i_free(conn->helo_domain);
		conn->helo_domain = i_strdup(helo);
		conn->helo.domain = conn->helo_domain;
		conn->helo.domain_valid = TRUE;
	}
	conn->authenticated = TRUE;
	conn->ssl_secured = ssl_secured;

	if (pdata_len > 0) {
		if (!i_stream_add_data(conn->conn.input, pdata, pdata_len))
			i_panic("Couldn't add client input to stream");
	}
}

void smtp_server_connection_start_pending(struct smtp_server_connection *conn)
{
	i_assert(!conn->started);
	conn->started = TRUE;

	e_debug(conn->event, "Connection started");

	if (!conn->ssl_start)
		smtp_server_connection_ready(conn);
	else if (conn->ssl_iostream == NULL)
		smtp_server_connection_input_unlock(conn);
}

void smtp_server_connection_start(struct smtp_server_connection *conn)
{
	smtp_server_connection_start_pending(conn);
	smtp_server_connection_resume(conn);
}

void smtp_server_connection_abort(struct smtp_server_connection **_conn,
				  unsigned int status, const char *enh_code,
				  const char *reason)
{
	struct smtp_server_connection *conn = *_conn;
	const char **reason_lines;

	if (conn == NULL)
		return;
	*_conn = NULL;

	i_assert(!conn->started);
	conn->started = TRUE;

	if (conn->authenticated) {
		reason_lines = t_strsplit_spaces(reason, "\r\n");
		smtp_server_connection_reply_lines(
			conn, status, enh_code, reason_lines);
		smtp_server_connection_terminate(
			&conn, "4.3.2", "Shutting down due to fatal error");
	} else {
		smtp_server_connection_terminate(&conn, enh_code, reason);
	}
}

void smtp_server_connection_halt(struct smtp_server_connection *conn)
{
	conn->halted = TRUE;
	smtp_server_connection_timeout_stop(conn);
	if (conn->ssl_start &&
	    (conn->ssl_iostream == NULL ||
	     !ssl_iostream_is_handshaked(conn->ssl_iostream)))
		return;

	smtp_server_connection_input_lock(conn);
}

void smtp_server_connection_resume(struct smtp_server_connection *conn)
{
	smtp_server_connection_input_unlock(conn);
	smtp_server_connection_timeout_update(conn);
	conn->halted = FALSE;
}

void smtp_server_connection_close(struct smtp_server_connection **_conn,
				  const char *reason)
{
	struct smtp_server_connection *conn = *_conn;

	*_conn = NULL;

	if (conn->closed)
		return;
	conn->closed = TRUE;

	smtp_server_connection_disconnect(conn, reason);
	smtp_server_connection_unref(&conn);
}

void smtp_server_connection_terminate(struct smtp_server_connection **_conn,
				      const char *enh_code, const char *reason)
{
	smtp_server_connection_terminate_full(_conn, enh_code, reason, reason);
}

void smtp_server_connection_terminate_full(
	struct smtp_server_connection **_conn, const char *enh_code,
	const char *reply_reason, const char *log_reason)
{
	struct smtp_server_connection *conn = *_conn;
	const char **reason_lines;

	*_conn = NULL;

	if (conn->closed)
		return;

	i_assert(enh_code[0] == '4' && enh_code[1] == '.');

	T_BEGIN {
		/* Add hostname prefix */
		reason_lines = t_strsplit_spaces(reply_reason, "\r\n");
		reason_lines[0] = t_strconcat(conn->set.hostname, " ",
					      reason_lines[0], NULL);

		smtp_server_connection_reply_lines(conn, 421, enh_code,
						   reason_lines);

		smtp_server_connection_close(&conn, log_reason);
	} T_END;
}

struct smtp_server_helo_data *
smtp_server_connection_get_helo_data(struct smtp_server_connection *conn)
{
	return &conn->helo;
}

enum smtp_server_state
smtp_server_connection_get_state(struct smtp_server_connection *conn,
				 const char **args_r)
{
	if (args_r != NULL)
		*args_r = conn->state.args;
	return conn->state.state;
}

void smtp_server_connection_set_state(struct smtp_server_connection *conn,
				      enum smtp_server_state state,
				      const char *args)
{
	bool changed = FALSE;

	if (conn->state.state != state) {
		conn->state.state = state;
		changed = TRUE;
	}
	if (null_strcmp(args, conn->state.args) != 0) {
		i_free(conn->state.args);
		conn->state.args = i_strdup(args);
		changed = TRUE;
	}

	if (changed && conn->callbacks != NULL &&
	    conn->callbacks->conn_state_changed != NULL)
		conn->callbacks->conn_state_changed(conn->context, state, args);
}

const char *
smtp_server_connection_get_security_string(struct smtp_server_connection *conn)
{
	if (conn->ssl_iostream == NULL)
		return NULL;
	return ssl_iostream_get_security_string(conn->ssl_iostream);
}

void smtp_server_connection_reset_state(struct smtp_server_connection *conn)
{
	e_debug(conn->event, "Connection state reset");

	i_free(conn->state.args);

	if (conn->state.trans != NULL)
		smtp_server_transaction_free(&conn->state.trans);

	/* RFC 3030, Section 2:
	   The RSET command, when issued after the first BDAT and before the
	   BDAT LAST, clears all segments sent during that transaction and
	   resets the session.
	 */
	i_stream_destroy(&conn->state.data_input);
	i_stream_destroy(&conn->state.data_chain_input);
	conn->state.data_chain = NULL;

	/* Reset state */
	i_zero(&conn->state);
	smtp_server_connection_set_state(conn, SMTP_SERVER_STATE_READY, NULL);
}

void smtp_server_connection_clear(struct smtp_server_connection *conn)
{
	e_debug(conn->event, "Connection clear");

	i_free(conn->helo_domain);
	i_zero(&conn->helo);
	smtp_server_connection_reset_state(conn);
}

void smtp_server_connection_set_capabilities(
	struct smtp_server_connection *conn, enum smtp_capability capabilities)
{
	conn->set.capabilities = capabilities;
}

void smtp_server_connection_set_greeting(struct smtp_server_connection *conn,
					 const char *greeting)
{
	conn->set.login_greeting = p_strdup(conn->pool, greeting);
}

void smtp_server_connection_add_extra_capability(
	struct smtp_server_connection *conn,
	const struct smtp_capability_extra *cap)
{
	const struct smtp_capability_extra *cap_idx;
	struct smtp_capability_extra cap_new;
	unsigned int insert_idx;
	pool_t pool = conn->pool;

	/* Avoid committing protocol errors */
	i_assert(smtp_ehlo_keyword_is_valid(cap->name));
	i_assert(smtp_ehlo_params_are_valid(cap->params));

	/* Cannot override standard capabilities */
	i_assert(smtp_capability_find_by_name(cap->name)
		 == SMTP_CAPABILITY_NONE);

	if (!array_is_created(&conn->extra_capabilities))
		p_array_init(&conn->extra_capabilities, pool, 4);

	/* Keep array sorted */
	insert_idx = array_count(&conn->extra_capabilities);
	array_foreach(&conn->extra_capabilities, cap_idx) {
		int cmp = strcasecmp(cap_idx->name, cap->name);

		/* Prohibit duplicates */
		i_assert(cmp != 0);

		if (cmp > 0) {
			insert_idx = array_foreach_idx(
				&conn->extra_capabilities, cap_idx);
			break;
		}
	}

	i_zero(&cap_new);
	cap_new.name = p_strdup(pool, cap->name);
	if (cap->params != NULL)
		cap_new.params = p_strarray_dup(pool, cap->params);

	array_insert(&conn->extra_capabilities, insert_idx, &cap_new, 1);
}

void *smtp_server_connection_get_context(struct smtp_server_connection *conn)
{
	return conn->context;
}

bool smtp_server_connection_is_ssl_secured(struct smtp_server_connection *conn)
{
	return conn->ssl_secured;
}

bool smtp_server_connection_is_trusted(struct smtp_server_connection *conn)
{
	if (conn->callbacks == NULL || conn->callbacks->conn_is_trusted == NULL)
		return FALSE;
	return conn->callbacks->conn_is_trusted(conn->context);
}

bool smtp_server_connection_is_started(struct smtp_server_connection *conn)
{
	return conn->started;
}

enum smtp_protocol
smtp_server_connection_get_protocol(struct smtp_server_connection *conn)
{
	return conn->set.protocol;
}

const char *
smtp_server_connection_get_protocol_name(struct smtp_server_connection *conn)
{
	string_t *pname = t_str_new(16);

	switch (conn->set.protocol) {
	case SMTP_PROTOCOL_SMTP:
		if (conn->helo.old_smtp)
			str_append(pname, "SMTP");
		else
			str_append(pname, "ESMTP");
		break;
	case SMTP_PROTOCOL_LMTP:
		str_append(pname, "LMTP");
		break;
	default:
		i_unreached();
	}
	if (conn->ssl_secured)
		str_append_c(pname, 'S');
	if (conn->authenticated)
		str_append_c(pname, 'A');
	return str_c(pname);
}

struct smtp_server_transaction *
smtp_server_connection_get_transaction(struct smtp_server_connection *conn)
{
	return conn->state.trans;
}

const char *
smtp_server_connection_get_transaction_id(struct smtp_server_connection *conn)
{
	if (conn->state.trans == NULL)
		return NULL;
	return conn->state.trans->id;
}

void smtp_server_connection_get_proxy_data(struct smtp_server_connection *conn,
					   struct smtp_proxy_data *proxy_data)
{
	i_zero(proxy_data);
	proxy_data->source_ip = conn->conn.remote_ip;
	proxy_data->source_port = conn->conn.remote_port;
	if (conn->proxy_helo != NULL)
		proxy_data->helo = conn->proxy_helo;
	else if (conn->helo.domain_valid)
		proxy_data->helo = conn->helo.domain;
	proxy_data->login = conn->username;
	proxy_data->session = conn->session_id;
	proxy_data->client_transport = conn->client_transport;
	proxy_data->local_name = conn->local_name;

	if (conn->proxy_proto != SMTP_PROXY_PROTOCOL_UNKNOWN)
		proxy_data->proto = conn->proxy_proto;
	else if (conn->set.protocol == SMTP_PROTOCOL_LMTP)
		proxy_data->proto = SMTP_PROXY_PROTOCOL_LMTP;
	else if (conn->helo.old_smtp)
		proxy_data->proto = SMTP_PROXY_PROTOCOL_SMTP;
	else
		proxy_data->proto = SMTP_PROXY_PROTOCOL_ESMTP;

	proxy_data->ttl_plus_1 = conn->proxy_ttl_plus_1;
	proxy_data->timeout_secs = conn->proxy_timeout_secs;
}

void smtp_server_connection_set_proxy_data(
	struct smtp_server_connection *conn,
	const struct smtp_proxy_data *proxy_data)
{
	if (proxy_data->source_ip.family != 0)
		conn->conn.remote_ip = proxy_data->source_ip;
	if (proxy_data->source_port != 0)
		conn->conn.remote_port = proxy_data->source_port;
	if (proxy_data->helo != NULL) {
		i_free(conn->helo_domain);
		conn->helo_domain = i_strdup(proxy_data->helo);
		conn->helo.domain = conn->helo_domain;
		conn->helo.domain_valid = TRUE;
		if (conn->helo.domain_valid) {
			i_free(conn->proxy_helo);
			conn->proxy_helo = i_strdup(proxy_data->helo);
		}
	}
	if (proxy_data->login != NULL) {
		i_free(conn->username);
		conn->username = i_strdup(proxy_data->login);
	}
	if (proxy_data->proto != SMTP_PROXY_PROTOCOL_UNKNOWN)
		conn->proxy_proto = proxy_data->proto;
	if (proxy_data->session != NULL &&
	    strcmp(proxy_data->session, conn->session_id) != 0) {
		e_debug(conn->event, "Updated session ID from %s to %s",
		        conn->session_id, proxy_data->session);
		i_free(conn->session_id);
		conn->session_id = i_strdup(proxy_data->session);
	}
	if (proxy_data->client_transport != NULL) {
		i_free(conn->client_transport);
		conn->client_transport = i_strdup(proxy_data->client_transport);
	}
	if (proxy_data->local_name != NULL) {
		i_free(conn->local_name);
		conn->local_name = i_strdup(proxy_data->local_name);
	}
	if (proxy_data->ttl_plus_1 > 0)
		conn->proxy_ttl_plus_1 = proxy_data->ttl_plus_1;
	if (conn->proxy_timeout_secs > 0)
		conn->proxy_timeout_secs = proxy_data->timeout_secs;

	connection_update_properties(&conn->conn);
	smtp_server_connection_update_event(conn);

	if (conn->callbacks != NULL &&
	    conn->callbacks->conn_proxy_data_updated != NULL) {
		struct smtp_proxy_data full_data;

		smtp_server_connection_get_proxy_data(conn, &full_data);

		conn->callbacks->
			conn_proxy_data_updated(conn->context, &full_data);
	}
}

void smtp_server_connection_register_mail_param(
	struct smtp_server_connection *conn, const char *param)
{
	param = p_strdup(conn->pool, param);

	if (!array_is_created(&conn->mail_param_extensions)) {
		p_array_init(&conn->mail_param_extensions, conn->pool, 8);
		array_push_back(&conn->mail_param_extensions, &param);
	} else {
		unsigned int count = array_count(&conn->mail_param_extensions);

		i_assert(count > 0);
		array_idx_set(&conn->mail_param_extensions,
			      count - 1, &param);
	}
	array_append_zero(&conn->mail_param_extensions);
}

void smtp_server_connection_register_rcpt_param(
	struct smtp_server_connection *conn, const char *param)
{
	param = p_strdup(conn->pool, param);

	if (!array_is_created(&conn->rcpt_param_extensions)) {
		p_array_init(&conn->rcpt_param_extensions, conn->pool, 8);
		array_push_back(&conn->rcpt_param_extensions, &param);
	} else {
		unsigned int count = array_count(&conn->rcpt_param_extensions);

		i_assert(count > 0);
		array_idx_set(&conn->rcpt_param_extensions,
			      count - 1, &param);
	}
	array_append_zero(&conn->rcpt_param_extensions);
}

void smtp_server_connection_switch_ioloop(struct smtp_server_connection *conn)
{
	if (conn->to_idle != NULL)
		conn->to_idle = io_loop_move_timeout(&conn->to_idle);
	connection_switch_ioloop(&conn->conn);
}

struct event_reason *
smtp_server_connection_reason_begin(struct smtp_server_connection *conn,
				    const char *name)
{
	if (conn->set.reason_code_module == NULL)
		return NULL;
	const char *reason_code =
		event_reason_code(conn->set.reason_code_module, name);
	return event_reason_begin(reason_code);
}

const char *
smtp_server_connection_get_server_name(struct smtp_server_connection *conn)
{
	if (conn->ssl_iostream == NULL)
		return NULL;
	return ssl_iostream_get_server_name(conn->ssl_iostream);
}
