/* Copyright (c) 2002-2018 Dovecot authors, see the included COPYING file */

#include "imap-common.h"
#include "imap-search-args.h"
#include "imap-search.h"

bool cmd_search(struct client_command_context *cmd)
{
	struct imap_search_context *ctx;
	struct mail_search_args *sargs;
	const struct imap_arg *args;
	const char *charset;
	int ret;

	if (!client_read_args(cmd, 0, 0, &args))
		return FALSE;

	if (!client_verify_open_mailbox(cmd))
		return TRUE;

	ctx = p_new(cmd->pool, struct imap_search_context, 1);
	ctx->cmd = cmd;

	if ((ret = cmd_search_parse_return_if_found(ctx, &args)) <= 0) {
		/* error / waiting for unambiguity */
		return ret < 0;
	}

	if (imap_arg_atom_equals(args, "CHARSET")) {
		/* CHARSET specified */
		if ((client_enabled_mailbox_features(cmd->client) & MAILBOX_FEATURE_UTF8ACCEPT) != 0) {
			/* RFC 6855 Section 3 bans CHARSET after UTF8=ACCEPT */
			client_send_command_error(cmd,
				"Cannot set search charset when using UTF8=ACCEPT");
			return TRUE;
		}
		if (!imap_arg_get_astring(&args[1], &charset)) {
			client_send_command_error(cmd,
				"Invalid charset argument.");
			imap_search_context_free(ctx);
			return TRUE;
		}
		args += 2;
	} else {
		charset = "UTF-8";
	}

	ret = imap_search_args_build(cmd, args, charset, &sargs);
	if (ret <= 0) {
		imap_search_context_free(ctx);
		return ret < 0;
	}

	return imap_search_start(ctx, sargs, NULL);
}
