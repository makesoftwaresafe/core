/* Copyright (c) 2002-2018 Dovecot authors, see the included COPYING file */

#include "imap-common.h"
#include "ioloop.h"
#include "istream.h"
#include "istream-chain.h"
#include "ostream.h"
#include "str.h"
#include "strnum.h"
#include "time-util.h"
#include "imap-resp-code.h"
#include "istream-binary-converter.h"
#include "mail-storage-private.h"
#include "imap-parser.h"
#include "imap-date.h"
#include "imap-util.h"
#include "imap-commands.h"
#include "imap-msgpart-url.h"

#include <sys/time.h>

/* Don't allow internaldates to be too far in the future. At least with Maildir
   they can cause problems with incremental backups since internaldate is
   stored in file's mtime. But perhaps there are also some other reasons why
   it might not be wanted. */
#define INTERNALDATE_MAX_FUTURE_SECS (2*3600)

struct cmd_append_context {
	struct client *client;
        struct client_command_context *cmd;
	struct mailbox *box;
        struct mailbox_transaction_context *t;
	time_t started;

	struct mailbox_transaction_context *rep_trans;
	struct mail *rep_mail;

	struct istream_chain *catchain;
	uoff_t cat_msg_size;

	struct istream *input;
	struct istream *litinput;
	uoff_t literal_size;

	struct imap_parser *save_parser;
	struct mail_save_context *save_ctx;
	unsigned int count;

	bool replace:1;
	bool message_input:1;
	bool binary_input:1;
	bool utf8_input:1;
	bool catenate:1;
	bool cmd_args_set:1;
	bool failed:1;
	bool utf8_accept:1;
};

static void cmd_append_finish(struct cmd_append_context *ctx);
static bool cmd_append_continue_message(struct client_command_context *cmd);
static bool cmd_append_parse_new_msg(struct client_command_context *cmd);

static const char *
get_disconnect_reason(struct cmd_append_context *ctx, uoff_t lit_offset)
{
	string_t *str = t_str_new(128);
	unsigned int secs = ioloop_time - ctx->started;

	str_printfa(str, "%s (While running %s command: %u msgs, %u secs",
		    i_stream_get_disconnect_reason(ctx->input),
		    (ctx->replace ? "REPLACE" : "APPEND"), ctx->count, secs);
	if (ctx->literal_size > 0) {
		str_printfa(str, ", %"PRIuUOFF_T"/%"PRIuUOFF_T" bytes",
			    lit_offset, ctx->literal_size);
	}
	str_append_c(str, ')');
	return str_c(str);
}

static void client_input_append(struct client_command_context *cmd)
{
	struct cmd_append_context *ctx = cmd->context;
	struct client *client = cmd->client;
	const char *reason;
	bool finished;
	uoff_t lit_offset;

	i_assert(!client->destroyed);

	client->last_input = ioloop_time;
	timeout_reset(client->to_idle);

	switch (i_stream_read(client->input)) {
	case -1:
		/* disconnected */
		lit_offset = ctx->litinput == NULL ? 0 :
			ctx->litinput->v_offset;
		reason = get_disconnect_reason(ctx, lit_offset);
		cmd_append_finish(cmd->context);
		/* Reset command so that client_destroy() doesn't try to call
		   cmd_append_continue_message() anymore. */
		client_command_free(&cmd);
		client_destroy(client, reason);
		return;
	case -2:
		if (ctx->message_input) {
			/* message data, this is handled internally by
			   mailbox_save_continue() */
			break;
		}
		cmd_append_finish(cmd->context);

		/* parameter word is longer than max. input buffer size.
		   this is most likely an error, so skip the new data
		   until newline is found. */
		client->input_skip_line = TRUE;

		if (!ctx->failed)
			client_send_command_error(cmd, "Too long argument.");
		cmd->param_error = TRUE;
		client_command_free(&cmd);
		break;
	default:
		o_stream_cork(client->output);
		finished = command_exec(cmd);
		if (!finished)
			(void)client_handle_unfinished_cmd(cmd);
		else
			client_command_free(&cmd);
		break;
	}

	cmd_sync_delayed(client);
	o_stream_uncork(client->output);

	client_continue_pending_input(client);
}

static void cmd_append_finish(struct cmd_append_context *ctx)
{
	if (ctx->save_parser != NULL)
		imap_parser_unref(&ctx->save_parser);

	i_assert(ctx->client->input_lock == ctx->cmd);

	io_remove(&ctx->client->io);
	/* we must put back the original flush callback before beginning to
	   sync (the command is still unfinished at that point) */
	o_stream_set_flush_callback(ctx->client->output,
				    client_output, ctx->client);

	if (ctx->rep_trans != NULL) {
		mail_free(&ctx->rep_mail);
		mailbox_transaction_rollback(&ctx->rep_trans);
	}

	i_stream_unref(&ctx->litinput);
	i_stream_unref(&ctx->input);
	if (ctx->save_ctx != NULL)
		mailbox_save_cancel(&ctx->save_ctx);
	if (ctx->t != NULL)
		mailbox_transaction_rollback(&ctx->t);
	if (ctx->box != ctx->cmd->client->mailbox && ctx->box != NULL)
		mailbox_free(&ctx->box);
}

static bool cmd_append_send_literal_continue(struct cmd_append_context *ctx)
{
	if (ctx->failed) {
		/* tagline was already sent, we can abort here */
		return FALSE;
	}

	o_stream_nsend(ctx->client->output, "+ OK\r\n", 6);
	o_stream_uncork(ctx->client->output);
	o_stream_cork(ctx->client->output);
	return TRUE;
}

static int
cmd_append_catenate_mpurl(struct client_command_context *cmd,
			  const char *caturl, struct imap_msgpart_url *mpurl)
{
	struct cmd_append_context *ctx = cmd->context;
	struct imap_msgpart_open_result mpresult;
	uoff_t newsize;
	const char *client_error;
	int ret;

	/* catenate URL */
	ret = imap_msgpart_url_read_part(mpurl, &mpresult, &client_error);
	if (ret < 0) {
		client_send_box_error(cmd, ctx->box);
		return -1;
	}
	if (ret == 0) {
		/* invalid url, abort */
		client_send_tagline(cmd,
			t_strdup_printf("NO [BADURL %s] %s.",
					caturl, client_error));
		return -1;
	}
	if (mpresult.size == 0) {
		/* empty input */
		return 0;
	}

	newsize = ctx->cat_msg_size + mpresult.size;
	if (newsize < ctx->cat_msg_size) {
		client_send_tagline(cmd,
			"NO [TOOBIG] Composed message grows too big.");
		return -1;
	}

	ctx->cat_msg_size = newsize;
	/* add this input stream to chain */
	i_stream_chain_append(ctx->catchain, mpresult.input);
	/* save by reading the chain stream */
	do {
		ret = i_stream_read(mpresult.input);
		i_assert(ret != 0); /* we can handle only blocking input here */
	} while (mailbox_save_continue(ctx->save_ctx) == 0 && ret != -1);

	if (mpresult.input->stream_errno != 0) {
		mailbox_set_critical(ctx->box,
			"read(%s) failed: %s (for CATENATE URL %s)",
			i_stream_get_name(mpresult.input),
			i_stream_get_error(mpresult.input), caturl);
		client_send_box_error(cmd, ctx->box);
		ret = -1;
	} else if (!mpresult.input->eof) {
		/* save failed */
		client_send_box_error(cmd, ctx->box);
		ret = -1;
	} else {
		/* all the input must be consumed, so istream-chain's read()
		   unreferences the stream and we can free its parent mail */
		i_assert(!i_stream_have_bytes_left(mpresult.input));
		ret = 0;
	}
	return ret;
}

static int
cmd_append_catenate_url(struct client_command_context *cmd, const char *caturl)
{
	struct cmd_append_context *ctx = cmd->context;
	struct imap_msgpart_url *mpurl;
	const char *client_error;
	int ret;

	if (ctx->failed)
		return -1;

	if (imap_msgpart_url_parse(cmd->client->user, cmd->client->mailbox,
				   caturl, &mpurl, &client_error) < 0) {
		/* invalid url, abort */
		client_send_tagline(cmd,
			t_strdup_printf("NO [BADURL %s] %s.",
					caturl, client_error));
		return -1;
	}
	ret = cmd_append_catenate_mpurl(cmd, caturl, mpurl);
	imap_msgpart_url_free(&mpurl);
	return ret;
}

static void cmd_append_catenate_text(struct client_command_context *cmd)
{
	struct cmd_append_context *ctx = cmd->context;

	if (ctx->literal_size > UOFF_T_MAX - ctx->cat_msg_size &&
	    !ctx->failed) {
		client_send_tagline(cmd,
			"NO [TOOBIG] Composed message grows too big.");
		ctx->failed = TRUE;
	}

	/* save the mail */
	ctx->cat_msg_size += ctx->literal_size;
	if (ctx->literal_size == 0) {
		/* zero length literal. RFC doesn't explicitly specify
		   what should be done with this, so we'll simply
		   handle it by skipping the empty text part. */
		ctx->litinput = i_stream_create_from_data("", 0);
		ctx->litinput->eof = TRUE;
	} else {
		ctx->litinput = i_stream_create_limit(cmd->client->input,
						      ctx->literal_size);
		i_stream_chain_append(ctx->catchain, ctx->litinput);
	}
}

static void
cmd_append_catenate_arg_text(struct client_command_context *cmd,
			     const struct imap_arg *args, bool *nonsync_r)
{
	struct cmd_append_context *ctx = cmd->context;

	if (args->literal8 && !ctx->binary_input && !ctx->failed) {
		client_send_tagline(cmd,
			"NO ["IMAP_RESP_CODE_UNKNOWN_CTE"] "
			"Binary input allowed only when the first part is binary.");
		ctx->failed = TRUE;
	}
	*nonsync_r = args->type == IMAP_ARG_LITERAL_SIZE_NONSYNC;
	cmd_append_catenate_text(cmd);
}

static int
cmd_append_catenate(struct client_command_context *cmd,
		    const struct imap_arg *args, bool *nonsync_r)
{
	struct cmd_append_context *ctx = cmd->context;
	const char *catpart;
	bool invalid_arg = FALSE;

	*nonsync_r = FALSE;

	/* Handle URLs until a TEXT literal is encountered */
	while (imap_arg_get_atom(args, &catpart)) {
		const char *caturl;

		if (strcasecmp(catpart, "URL") == 0 ) {
			/* URL <url> */
			args++;
			if (!imap_arg_get_astring(args, &caturl)) {
				invalid_arg = TRUE;
				break;
			}
			if (cmd_append_catenate_url(cmd, caturl) < 0) {
				/* delay failure until we can stop
				   parsing input */
				ctx->failed = TRUE;
			}
		} else if (strcasecmp(catpart, "TEXT") == 0) {
			/* TEXT <literal> */
			args++;
			if (!imap_arg_get_literal_size(args, &ctx->literal_size)) {
				invalid_arg = TRUE;
				break;
			}
			ctx->utf8_input = FALSE;
			cmd_append_catenate_arg_text(cmd, args, nonsync_r);
			return 1;
		} else if (ctx->utf8_accept && strcasecmp(catpart, "UTF8") == 0) {
			const struct imap_arg *list_args = NULL;

			args++;
			if (!imap_arg_get_list(args, &list_args) ||
			    !list_args[0].literal8 ||
			    !imap_arg_get_literal_size(list_args, &ctx->literal_size)) {
				invalid_arg = TRUE;
				break;
			}
			ctx->utf8_input = TRUE;
			cmd_append_catenate_arg_text(cmd, list_args, nonsync_r);
			return 1;
		} else {
			break;
		}
		args++;
	}

	if (!invalid_arg && IMAP_ARG_IS_EOL(args)) {
		/* ")" */
		return 0;
	}
	ctx->client->input_skip_line = TRUE;
	if (!ctx->failed)
		client_send_command_error(cmd, "Invalid arguments.");
	return -1;
}

static void cmd_append_finish_catenate(struct client_command_context *cmd)
{
	struct cmd_append_context *ctx = cmd->context;

	i_stream_chain_append_eof(ctx->catchain);
	i_stream_unref(&ctx->input);
	ctx->catenate = FALSE;
	ctx->catchain = NULL;

	if (ctx->failed) {
		/* APPEND has already failed */
		if (ctx->save_ctx != NULL)
			mailbox_save_cancel(&ctx->save_ctx);
	} else {
		if (mailbox_save_finish(&ctx->save_ctx) < 0) {
			client_send_box_error(cmd, ctx->box);
			ctx->failed = TRUE;
		}
	}
}

static bool catenate_args_can_stop(struct cmd_append_context *ctx,
				   const struct imap_arg *args)
{
	/* eat away literal_sizes from URLs */
	while (args->type != IMAP_ARG_EOL) {
		if (imap_arg_atom_equals(args, "TEXT"))
			return TRUE;
		if (ctx->utf8_accept && imap_arg_atom_equals(args, "UTF8")) {
			const struct imap_arg *list_args;

			if (!imap_arg_get_list(++args, &list_args))
				return FALSE;
			if (args->type == IMAP_ARG_LITERAL_SIZE ||
			    args->type == IMAP_ARG_LITERAL_SIZE_NONSYNC)
				return TRUE;
			args++;
			continue;
		}
		if (!imap_arg_atom_equals(args, "URL")) {
			/* error - handle it later */
			return TRUE;
		}
		args++;
		if (args->type == IMAP_ARG_EOL) {
			/* error - handle it later */
			return TRUE;
		}
		if (args->type == IMAP_ARG_LITERAL_SIZE ||
		    args->type == IMAP_ARG_LITERAL_SIZE_NONSYNC) {
			if (args->type == IMAP_ARG_LITERAL_SIZE) {
				if (!cmd_append_send_literal_continue(ctx))
					return TRUE;
			}
			imap_parser_read_last_literal(ctx->save_parser);
			return FALSE;
		}
		args++;
	}
	return TRUE;
}

static void cmd_append_handle_parse_error(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
	struct cmd_append_context *ctx = cmd->context;
	const char *client_error;
	enum imap_parser_error parse_error;

	client_error = imap_parser_get_error(ctx->save_parser, &parse_error);
	switch (parse_error) {
	case IMAP_PARSE_ERROR_NONE:
		i_unreached();
	case IMAP_PARSE_ERROR_LITERAL_TOO_BIG:
		client_send_line(client, t_strconcat("* BYE ",
			(client->set->imap_literal_minus ? "[TOOBIG] " : ""),
			client_error, NULL));
		client_disconnect(client, client_error);
		break;
	default:
		if (!ctx->failed)
			client_send_command_error(cmd, client_error);
	}
	client->input_skip_line = TRUE;
}

static int
cmd_append_finish_list(struct client_command_context *cmd)
{
	struct cmd_append_context *ctx = cmd->context;
	const struct imap_arg *args;
	int ret;

	ret = imap_parser_read_args(ctx->save_parser, 0,
				    IMAP_PARSE_FLAG_LITERAL_SIZE |
				    IMAP_PARSE_FLAG_LITERAL8 |
				    IMAP_PARSE_FLAG_INSIDE_LIST, &args);
	if (ret == -1) {
		cmd_append_handle_parse_error(cmd);
		return -1;
	}
	if (ret < 0) {
		/* need more data */
		return 0;
	}

	if (!IMAP_ARG_IS_EOL(&args[0])) {
		client_send_command_error(cmd, "Invalid arguments.");
		return -1;
	}
	return 1;
}

static bool cmd_append_continue_catenate(struct client_command_context *cmd)
{
	struct cmd_append_context *ctx = cmd->context;
	const struct imap_arg *args;
	bool nonsync = FALSE;
	int ret;

	if (cmd->cancel) {
		/* cancel the command immediately (disconnection) */
		cmd_append_finish(ctx);
		return TRUE;
	}

	if (ctx->utf8_input) {
		ret = cmd_append_finish_list(cmd);
		if (ret == 0)
			return FALSE;
		if (ret < 0) {
			cmd_append_finish(ctx);
			return TRUE;
		}
		ctx->utf8_input = FALSE;
	}

	/* we're parsing inside CATENATE (..) list after handling a TEXT part.
	   it's fine that this would need to fully fit into input buffer
	   (although clients attempting to DoS could simply insert an extra
	   {1+} between the URLs) */
	do {
		ret = imap_parser_read_args(ctx->save_parser, 0,
					    IMAP_PARSE_FLAG_LITERAL_SIZE |
					    IMAP_PARSE_FLAG_LITERAL8 |
					    IMAP_PARSE_FLAG_INSIDE_LIST, &args);
	} while (ret > 0 && !catenate_args_can_stop(ctx, args));
	if (ret == -1) {
		cmd_append_handle_parse_error(cmd);
		cmd_append_finish(ctx);
		return TRUE;
	}
	if (ret < 0) {
		/* need more data */
		return FALSE;
	}

	if ((ret = cmd_append_catenate(cmd, args, &nonsync)) < 0) {
		/* invalid parameters, abort immediately */
		cmd_append_finish(ctx);
		return TRUE;
	}

	if (ret == 0) {
		/* ")" */
		cmd_append_finish_catenate(cmd);

		/* last catenate part */
		imap_parser_reset(ctx->save_parser);
		cmd->func = cmd_append_parse_new_msg;
		return cmd_append_parse_new_msg(cmd);
	}

	/* TEXT <literal> */

	if (!nonsync) {
		if (!cmd_append_send_literal_continue(ctx)) {
			cmd_append_finish(ctx);
			return TRUE;
		}
	}

	i_assert(ctx->litinput != NULL);
	ctx->message_input = TRUE;
	cmd->func = cmd_append_continue_message;
	return cmd_append_continue_message(cmd);
}

static int
cmd_append_start_catenate(struct cmd_append_context *ctx,
			  const struct imap_arg **args,
			  const struct imap_arg **cat_list_r)
{
	const struct imap_arg *list_args;

	if (!imap_arg_atom_equals(*args, "CATENATE"))
		return 0;
	if (!imap_arg_get_list(++(*args), &list_args))
		return -1;

	ctx->catenate = TRUE;

	/* We'll do BINARY conversion only if the CATENATE's first
	   part is a literal8. If it doesn't and a literal8 is seen
	   later we'll abort the append with UNKNOWN-CTE. */
	if (ctx->utf8_accept && imap_arg_atom_equals(&list_args[0], "UTF8")) {
		const struct imap_arg *tmp_args;
		if (!imap_arg_get_list(&list_args[1], &tmp_args))
			return -1;
		if (!tmp_args[0].literal8)
			return -1;
		ctx->utf8_input = TRUE;
		ctx->binary_input = TRUE;
	} else {
		ctx->utf8_input = FALSE;
		ctx->binary_input =
			imap_arg_atom_equals(&list_args[0], "TEXT") &&
			list_args[1].literal8;
	}
	*cat_list_r = list_args;
	return 1;
}

static int
cmd_append_start_utf8(struct cmd_append_context *ctx,
		      const struct imap_arg **args,
		      bool *nonsync_r)
{
	struct client *client = ctx->client;
	const struct imap_arg *list_args;

	if (!ctx->utf8_accept)
		return 0;

	if (!imap_arg_atom_equals(*args, "UTF8"))
		return 0;
	if (!imap_arg_get_list(++(*args), &list_args))
		return -1;
	if (!imap_arg_get_literal_size(list_args, &ctx->literal_size))
		return -1;
	if (!list_args[0].literal8)
		return -1;
	*nonsync_r = list_args->type == IMAP_ARG_LITERAL_SIZE_NONSYNC;
	ctx->litinput = i_stream_create_limit(client->input, ctx->literal_size);
	ctx->utf8_input = TRUE;
	ctx->binary_input = TRUE;
	return 1;
}

static int
cmd_append_handle_args(struct client_command_context *cmd,
		       const struct imap_arg *args, bool *nonsync_r)
{
	struct client *client = cmd->client;
	struct cmd_append_context *ctx = cmd->context;
	const struct imap_arg *flags_list;
	const struct imap_arg *cat_list = NULL;
	enum mail_flags flags;
	const char *const *keywords_list;
	struct mail_keywords *keywords;
	struct istream *input;
	const char *internal_date_str;
	time_t internal_date;
	int ret, timezone_offset;
	bool valid;

	if (!ctx->cmd_args_set) {
		ctx->cmd_args_set = TRUE;
		client_args_finished(cmd, args);
	}

	/* [<flags>] */
	if (!imap_arg_get_list(args, &flags_list))
		flags_list = NULL;
	else
		args++;

	/* [<internal date>] */
	if (args->type != IMAP_ARG_STRING)
		internal_date_str = NULL;
	else {
		internal_date_str = imap_arg_as_astring(args);
		args++;
	}

	/* <message literal> | CATENATE (..) | UTF8 (..) */
	*nonsync_r = FALSE;
	ctx->catenate = FALSE;
	if (imap_arg_get_literal_size(args, &ctx->literal_size)) {
		*nonsync_r = args->type == IMAP_ARG_LITERAL_SIZE_NONSYNC;
		ctx->binary_input = args->literal8;
		ctx->litinput = i_stream_create_limit(client->input, ctx->literal_size);
		ctx->utf8_input = FALSE;
		valid = TRUE;
	} else {
		ret = cmd_append_start_catenate(ctx, &args, &cat_list);
		if (ret == 0)
			ret = cmd_append_start_utf8(ctx, &args, nonsync_r);
		valid = (ret > 0);
	}
	if (!IMAP_ARG_IS_EOL(&args[1]))
		valid = FALSE;
	if (!valid) {
		client->input_skip_line = TRUE;
		if (!ctx->failed)
			client_send_command_error(cmd, "Invalid arguments.");
		return -1;
	}

	if (flags_list == NULL || ctx->failed) {
		flags = 0;
		keywords = NULL;
	} else {
		if (!client_parse_mail_flags(cmd, flags_list,
					     &flags, &keywords_list))
			return -1;
		if (keywords_list == NULL)
			keywords = NULL;
		else if (mailbox_keywords_create(ctx->box, keywords_list,
						 &keywords) < 0) {
			/* invalid keywords - delay failure */
			client_send_box_error(cmd, ctx->box);
			ctx->failed = TRUE;
			keywords = NULL;
		}
	}

	if (internal_date_str == NULL || ctx->failed) {
		/* no time given, default to now. */
		internal_date = (time_t)-1;
		timezone_offset = 0;
	} else if (!imap_parse_datetime(internal_date_str,
					&internal_date, &timezone_offset)) {
		client_send_command_error(cmd, "Invalid internal date.");
		if (keywords != NULL)
			mailbox_keywords_unref(&keywords);
		return -1;
	}

	if (internal_date != (time_t)-1 &&
	    internal_date > ioloop_time + INTERNALDATE_MAX_FUTURE_SECS) {
		/* the client specified a time in the future, set it to now. */
		internal_date = (time_t)-1;
		timezone_offset = 0;
	}

	if (ctx->catenate) {
		ctx->cat_msg_size = 0;
		ctx->input = i_stream_create_chain(&ctx->catchain,
						   IO_BLOCK_SIZE);
	} else {
		if (ctx->literal_size == 0) {
			/* no message data, abort */
			if (!ctx->failed) {
				client_send_tagline(cmd,
					"NO Can't save a zero byte message.");
				ctx->failed = TRUE;
			}
			if (!*nonsync_r) {
				if (keywords != NULL)
					mailbox_keywords_unref(&keywords);
				return -1;
			}
			/* {0+} used. although there isn't any point in using
			   MULTIAPPEND here and adding more messages, it is
			   technically valid so we'll continue parsing.. */
		}
		i_assert(ctx->litinput != NULL);
		ctx->input = ctx->litinput;
		i_stream_ref(ctx->input);
	}
	if (ctx->binary_input) {
		input = i_stream_create_binary_converter(ctx->input);
		i_stream_unref(&ctx->input);
		ctx->input = input;
	}

	if (!ctx->failed) {
		/* save the mail */
		ctx->save_ctx = mailbox_save_alloc(ctx->t);
		mailbox_save_set_flags(ctx->save_ctx, flags, keywords);
		mailbox_save_set_received_date(ctx->save_ctx,
					       internal_date, timezone_offset);
		if (!ctx->replace)
			ret = mailbox_save_begin(&ctx->save_ctx, ctx->input);
		else {
			ret = mailbox_save_begin_replace(&ctx->save_ctx,
							 ctx->input,
							 ctx->rep_mail);
		}
		if (ret < 0) {
			/* save initialization failed */
			client_send_box_error(cmd, ctx->box);
			ctx->failed = TRUE;
		}
	}
	if (keywords != NULL)
		mailbox_keywords_unref(&keywords);
	ctx->count++;

	if (!ctx->catenate) {
		/* normal APPEND */
		return 1;
	} else if (cat_list->type == IMAP_ARG_EOL) {
		/* zero parts */
		if (!ctx->failed)
			client_send_command_error(cmd, "Empty CATENATE list.");
		client->input_skip_line = TRUE;
		return -1;
	} else if ((ret = cmd_append_catenate(cmd, cat_list, nonsync_r)) < 0) {
		/* invalid parameters, abort immediately */
		return -1;
	} else if (ret == 0) {
		/* CATENATE consisted only of URLs */
		return 0;
	} else {
		/* TEXT part found from CATENATE */
		return 1;
	}
}

static bool cmd_append_finish_parsing(struct client_command_context *cmd)
{
	struct cmd_append_context *ctx = cmd->context;
	struct client *client = cmd->client;
	enum mailbox_sync_flags sync_flags;
	enum imap_sync_flags imap_flags;
	struct mail_transaction_commit_changes changes;
	const char *msg_suffix = "";
	unsigned int save_count;
	string_t *msg;
	int ret;

	/* eat away the trailing CRLF */
	cmd->client->input_skip_line = TRUE;

	if (ctx->failed) {
		/* we failed earlier, error message is sent */
		cmd_append_finish(ctx);
		return TRUE;
	}
	if (ctx->count == 0) {
		client_send_command_error(cmd, "Missing message size.");
		cmd_append_finish(ctx);
		return TRUE;
	}

	ret = mailbox_transaction_commit_get_changes(&ctx->t, &changes);
	if (ret < 0) {
		client_send_box_error(cmd, ctx->box);
		cmd_append_finish(ctx);
		return TRUE;
	}

	msg = t_str_new(256);
	save_count = seq_range_count(&changes.saved_uids);

	if (ctx->replace) {
		/* Send APPENDUID response code if possible */
		if (save_count > 0 && !changes.no_read_perm) {
			i_assert(ctx->count == save_count);
			str_printfa(msg, "* OK [APPENDUID %u ",
				    changes.uid_validity);
			imap_write_seq_range(msg, &changes.saved_uids);
			str_append(msg, "] Replacement message saved.");
			client_send_line(client, str_c(msg));
			str_truncate(msg, 0);
		}

		/* Commit removal of the replaced message */
		i_assert(ctx->rep_trans != NULL);
		mail_free(&ctx->rep_mail);
		if (mailbox_transaction_commit(&ctx->rep_trans) < 0) {
			client_send_line(client, t_strflocaltime(
				"* NO "MAIL_ERRSTR_CRITICAL_MSG_STAMP,
				ioloop_time));
			e_error(client->event, "REPLACE: "
				"Failed to expunge the old message: %s",
				mailbox_get_last_error(client->mailbox, NULL));
			msg_suffix = ", but failed to expunge old message";
		}
	}

	if (ctx->replace || save_count == 0 || changes.no_read_perm) {
		/* This is a REPLACE command or APPENDUID is not supported by
		   backend (virtual) */
		if (ctx->replace)
			str_append(msg, "OK Replace completed");
		else
			str_append(msg, "OK Append completed");
	} else {
		i_assert(ctx->count == save_count);
		str_printfa(msg, "OK [APPENDUID %u ",
			    changes.uid_validity);
		imap_write_seq_range(msg, &changes.saved_uids);
		str_append(msg, "] Append completed");
	}
	str_append(msg, msg_suffix);
	str_append_c(msg, '.');
	ctx->client->logout_stats.append_count += save_count;
	pool_unref(&changes.pool);

	if (ctx->box == cmd->client->mailbox) {
		sync_flags = 0;
		imap_flags = IMAP_SYNC_FLAG_SAFE;
	} else {
		sync_flags = MAILBOX_SYNC_FLAG_FAST;
		imap_flags = 0;
	}

	cmd_append_finish(ctx);
	return cmd_sync(cmd, sync_flags, imap_flags, str_c(msg));
}

static bool cmd_append_args_can_stop(struct cmd_append_context *ctx,
				     const struct imap_arg *args,
				     bool *last_literal_r)
{
	const struct imap_arg *list;

	*last_literal_r = FALSE;
	if (args->type == IMAP_ARG_EOL)
		return TRUE;

	/* [(flags)] ["internal date"]
	     <message literal> | CATENATE (..) | UTF8 (..) */
	if (args->type == IMAP_ARG_LIST)
		args++;
	if (args->type == IMAP_ARG_STRING)
		args++;

	if (args->type == IMAP_ARG_LITERAL_SIZE ||
	    args->type == IMAP_ARG_LITERAL_SIZE_NONSYNC)
		return TRUE;
	if (imap_arg_atom_equals(args, "CATENATE") &&
	    imap_arg_get_list(&args[1], &list)) {
		if (catenate_args_can_stop(ctx, list))
			return TRUE;
		*last_literal_r = TRUE;
	}
	if (ctx->utf8_accept &&
	    imap_arg_atom_equals(args, "UTF8") &&
	    imap_arg_get_list(&args[1], &list)) {
		if (list->type == IMAP_ARG_LITERAL_SIZE ||
		    list->type == IMAP_ARG_LITERAL_SIZE_NONSYNC)
			return TRUE;
		*last_literal_r = TRUE;
	}
	return FALSE;
}

static bool cmd_append_parse_new_msg(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
	struct cmd_append_context *ctx = cmd->context;
	const struct imap_arg *args;
	unsigned int arg_min_count;
	bool nonsync, last_literal;
	int ret;

	if (ctx->utf8_input) {
		ret = cmd_append_finish_list(cmd);
		if (ret == 0)
			return FALSE;
		if (ret < 0) {
			cmd_append_finish(ctx);
			return TRUE;
		}
		ctx->utf8_input = FALSE;
	}

	/* this function gets called 1) after parsing APPEND <mailbox> and
	   2) with MULTIAPPEND extension after already saving one or more
	   mails. */
	if (cmd->cancel) {
		/* cancel the command immediately (disconnection) */
		cmd_append_finish(ctx);
		return TRUE;
	}

	/* if error occurs, the CRLF is already read. */
	client->input_skip_line = FALSE;

	/* parse the entire line up to the first message literal, or in case
	   the input buffer is full of MULTIAPPEND CATENATE URLs, parse at
	   least until the beginning of the next message */
	arg_min_count = 0; last_literal = FALSE;
	do {
		if (!last_literal)
			arg_min_count++;
		else {
			/* we only read the literal size. now we read the
			   literal itself. */
		}
		ret = imap_parser_read_args(ctx->save_parser, arg_min_count,
					    IMAP_PARSE_FLAG_LITERAL_SIZE |
					    IMAP_PARSE_FLAG_LITERAL8, &args);
	} while (ret >= (int)arg_min_count &&
		 !cmd_append_args_can_stop(ctx, args, &last_literal));
	if (ret == -1) {
		cmd_append_handle_parse_error(cmd);
		cmd_append_finish(ctx);
		return TRUE;
	}
	if (ret < 0) {
		/* need more data */
		return FALSE;
	}

	if (ctx->replace && ctx->count > 0 && !IMAP_ARG_IS_EOL(args)) {
		/* Only one message allowed for REPLACE */
		if (!ctx->failed)
			client_send_command_error(cmd, "Invalid arguments.");
		cmd_append_finish(ctx);
		return TRUE;
	}

	if (IMAP_ARG_IS_EOL(args)) {
		/* last message */
		return cmd_append_finish_parsing(cmd);
	}

	ret = cmd_append_handle_args(cmd, args, &nonsync);
	if (ret < 0) {
		/* invalid parameters, abort immediately */
		cmd_append_finish(ctx);
		return TRUE;
	}
	if (ret == 0) {
		/* CATENATE contained only URLs. Finish it and see if there
		   are more messages. */
		cmd_append_finish_catenate(cmd);
		imap_parser_reset(ctx->save_parser);
		return cmd_append_parse_new_msg(cmd);
	}

	if (!nonsync) {
		if (!cmd_append_send_literal_continue(ctx)) {
			cmd_append_finish(ctx);
			return TRUE;
		}
	}

	i_assert(ctx->litinput != NULL);
	ctx->message_input = TRUE;
	cmd->func = cmd_append_continue_message;
	return cmd_append_continue_message(cmd);
}

static bool cmd_append_continue_message(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
	struct cmd_append_context *ctx = cmd->context;
	int ret = 0;

	if (cmd->cancel) {
		/* cancel the command immediately (disconnection) */
		cmd_append_finish(ctx);
		return TRUE;
	}

	if (ctx->save_ctx != NULL) {
		while (ctx->litinput->v_offset != ctx->literal_size) {
			ret = i_stream_read(ctx->litinput);
			if (mailbox_save_continue(ctx->save_ctx) < 0) {
				/* we still have to finish reading the message
				   from client */
				mailbox_save_cancel(&ctx->save_ctx);
				break;
			}
			if (ret == -1 || ret == 0)
				break;
		}
	}

	if (ctx->save_ctx == NULL) {
		/* saving has already failed, we're just eating away the
		   literal */
		(void)i_stream_read(ctx->litinput);
		i_stream_skip(ctx->litinput,
			      i_stream_get_data_size(ctx->litinput));
	}

	if (ctx->litinput->eof || client->input->closed) {
		uoff_t lit_offset = ctx->litinput->v_offset;

		/* finished - do one more read, to make sure istream-chain
		   unreferences its stream, which is needed for litinput's
		   unreferencing to seek the client->input to correct
		   position. the seek is needed to avoid trying to seek
		   backwards in the ctx->input's parent stream. */
		i_stream_seek(ctx->input, ctx->input->v_offset);
		(void)i_stream_read(ctx->input);
		i_stream_unref(&ctx->litinput);

		if (ctx->failed) {
			if (ctx->save_ctx != NULL)
				mailbox_save_cancel(&ctx->save_ctx);
		} else if (ctx->save_ctx == NULL) {
			/* failed above */
			client_send_box_error(cmd, ctx->box);
			ctx->failed = TRUE;
		} else if (lit_offset != ctx->literal_size) {
			/* client disconnected before it finished sending the
			   whole message. */
			ctx->failed = TRUE;
			mailbox_save_cancel(&ctx->save_ctx);
			client_disconnect(client,
				get_disconnect_reason(ctx, lit_offset));
		} else if (ctx->catenate) {
			/* CATENATE isn't finished yet */
		} else if (mailbox_save_finish(&ctx->save_ctx) < 0) {
			client_send_box_error(cmd, ctx->box);
			ctx->failed = TRUE;
		}

		if (client->input->closed) {
			cmd_append_finish(ctx);
			return TRUE;
		}

		/* prepare for the next message (or its part with catenate) */
		ctx->message_input = FALSE;
		imap_parser_reset(ctx->save_parser);

		if (ctx->catenate) {
			cmd->func = cmd_append_continue_catenate;
			return cmd_append_continue_catenate(cmd);
		}

		i_stream_unref(&ctx->input);
		cmd->func = cmd_append_parse_new_msg;
		return cmd_append_parse_new_msg(cmd);
	}
	return FALSE;
}

static bool cmd_append_full(struct client_command_context *cmd, bool replace)
{
	struct client *client = cmd->client;
	const struct imap_arg *args;
        struct cmd_append_context *ctx;
	const char *mailbox;
	uint32_t seqnum = 0;

	if (client->syncing) {
		/* if transaction is created while its view is synced,
		   appends aren't allowed for it. */
		cmd->state = CLIENT_COMMAND_STATE_WAIT_UNAMBIGUITY;
		return FALSE;
	}

	if (!client_read_args(cmd, (replace ? 2 : 1), 0, &args))
		return FALSE;

	if (replace) {
		if (!client_verify_open_mailbox(cmd))
			return TRUE;

		const char *seq;

		/* <seq-number> */
		if (!imap_arg_get_atom(args, &seq) ||
		     str_to_uint32(seq, &seqnum) < 0) {
			client_send_command_error(cmd, "Invalid arguments.");
			return TRUE;
		}

		args++;
	}

	/* <mailbox> */
	if (!imap_arg_get_astring(args, &mailbox)) {
		client_send_command_error(cmd, "Invalid arguments.");
		return TRUE;
	}

	if (replace) {
		if (!cmd->uid && (seqnum > client->messages_count)) {
			client_send_command_error(
				cmd, "Invalid message sequence.");
			return TRUE;
		}
	}

	/* we keep the input locked all the time */
	client->input_lock = cmd;

	ctx = p_new(cmd->pool, struct cmd_append_context, 1);
	ctx->cmd = cmd;
	ctx->client = client;
	ctx->replace = replace;
	ctx->started = ioloop_time;
	ctx->utf8_accept = (client_enabled_mailbox_features(cmd->client) &
			    MAILBOX_FEATURE_UTF8ACCEPT) != 0;
	if (client_open_save_dest_box(cmd, mailbox, &ctx->box) < 0)
		ctx->failed = TRUE;
	else {
		event_add_str(cmd->global_event, "mailbox",
			      mailbox_get_vname(ctx->box));
		ctx->t = mailbox_transaction_begin(ctx->box,
					MAILBOX_TRANSACTION_FLAG_EXTERNAL |
					MAILBOX_TRANSACTION_FLAG_ASSIGN_UIDS,
					imap_client_command_get_reason(cmd));
	}

	if (replace) {
		ctx->rep_trans = mailbox_transaction_begin(
			client->mailbox, 0,
			imap_client_command_get_reason(cmd));
		ctx->rep_mail = mail_alloc(ctx->rep_trans,
			MAIL_FETCH_PHYSICAL_SIZE |
			MAIL_FETCH_VIRTUAL_SIZE, NULL);
		if (!cmd->uid) {
			mail_set_seq(ctx->rep_mail, seqnum);
		} else if (!mail_set_uid(ctx->rep_mail, seqnum)) {
			client_send_tagline(cmd,
				"NO Invalid UID for replaced message.");
			ctx->failed = TRUE;
		}
	}

	io_remove(&client->io);
	client->io = io_add_istream(client->input, client_input_append, cmd);
	/* append is special because we're only waiting on client input, not
	   client output, so disable the standard output handler until we're
	   finished */
	o_stream_unset_flush_callback(client->output);

	ctx->save_parser = imap_parser_create(client->input, client->output,
					      client->set->imap_max_line_length);
	if (client->set->imap_literal_minus)
		imap_parser_enable_literal_minus(ctx->save_parser);

	cmd->func = cmd_append_parse_new_msg;
	cmd->context = ctx;
	return cmd_append_parse_new_msg(cmd);
}

bool cmd_append(struct client_command_context *cmd)
{
	return cmd_append_full(cmd, FALSE);
}

bool cmd_replace(struct client_command_context *cmd)
{
	return cmd_append_full(cmd, TRUE);
}
