/* Copyright (c) 2005-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "lib-signals.h"
#include "ioloop.h"
#include "env-util.h"
#include "istream.h"
#include "istream-seekable.h"
#include "path-util.h"
#include "safe-mkstemp.h"
#include "eacces-error.h"
#include "ipwd.h"
#include "str.h"
#include "str-sanitize.h"
#include "strescape.h"
#include "unichar.h"
#include "rfc822-parser.h"
#include "message-address.h"
#include "smtp-address.h"
#include "settings.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "mail-storage-service.h"
#include "mail-namespace.h"
#include "raw-storage.h"
#include "mail-deliver.h"
#include "mail-send.h"
#include "mbox-from.h"
#include "smtp-submit-settings.h"
#include "lda-settings.h"

#include <stdio.h>
#include <sysexits.h>

const struct smtp_address default_envelope_sender = {
	.localpart = "MAILER-DAEMON",
};

/* After buffer grows larger than this, create a temporary file to /tmp
   where to read the mail. */
#define MAIL_MAX_MEMORY_BUFFER (1024*128)

struct event_category event_category_lda = {
	.name = "lda",
};

static const char *wanted_headers[] = {
	"From", "To", "Message-ID", "Subject", "Return-Path",
	NULL
};

static int seekable_fd_callback(const char **path_r, void *context)
{
	struct mail_deliver_input *dinput = context;
	string_t *path;
	int fd;

	path = t_str_new(128);
	mail_user_set_get_temp_prefix(path, dinput->rcpt_user->set);
	fd = safe_mkstemp(path, 0600, (uid_t)-1, (gid_t)-1);
	if (fd == -1) {
		i_error("safe_mkstemp(%s) failed: %m", str_c(path));
		return -1;
	}

	/* we just want the fd, unlink it */
	if (i_unlink(str_c(path)) < 0) {
		/* shouldn't happen.. */
		i_close_fd(&fd);
		return -1;
	}

	*path_r = str_c(path);
	return fd;
}

static struct istream *
create_raw_stream(struct mail_deliver_input *dinput,
		  int fd, time_t *mtime_r)
{
	struct istream *input, *input2, *input_list[2];
	const unsigned char *data;
	const char *error;
	char *sender = NULL;
	size_t i, size;
	int ret, tz;

	*mtime_r = (time_t)-1;
	fd_set_nonblock(fd, FALSE);

	input = i_stream_create_fd(fd, 4096);
	input->blocking = TRUE;
	/* If input begins with a From-line, drop it */
	ret = i_stream_read_bytes(input, &data, &size, 5);
	if (ret > 0 && memcmp(data, "From ", 5) == 0) {
		/* skip until the first LF */
		i_stream_skip(input, 5);
		while (i_stream_read_more(input, &data, &size) > 0) {
			for (i = 0; i < size; i++) {
				if (data[i] == '\n')
					break;
			}
			if (i != size) {
				(void)mbox_from_parse(data, i, mtime_r, &tz,
						      &sender);
				i_stream_skip(input, i + 1);
				break;
			}
			i_stream_skip(input, size);
		}
	}

	if (sender != NULL && dinput->mail_from == NULL) {
		struct smtp_address *mail_from = NULL;
		/* use the envelope sender from From_-line, but only if it
		   hasn't been specified with -f already. */
		if (smtp_address_parse_mailbox(pool_datastack_create(),
					       sender, 0, &mail_from,
					       &error) < 0) {
			e_warning(dinput->event_parent,
				  "Failed to parse address from `From_'-line: %s",
				  error);
		}
		dinput->mail_from = mail_from;
	}
	i_free(sender);

	if (input->v_offset == 0) {
		input2 = input;
		i_stream_ref(input2);
	} else {
		input2 = i_stream_create_limit(input, UOFF_T_MAX);
	}
	i_stream_unref(&input);

	input_list[0] = input2; input_list[1] = NULL;
	input = i_stream_create_seekable(input_list, MAIL_MAX_MEMORY_BUFFER,
					 seekable_fd_callback, dinput);
	i_stream_unref(&input2);
	return input;
}

static struct mail *
lda_raw_mail_open(struct mail_deliver_input *dinput, const char *path)
{
	struct mail_user *raw_mail_user;
	struct mailbox *box;
	struct mailbox_transaction_context *t;
	struct mail *mail;
	struct mailbox_header_lookup_ctx *headers_ctx;
	const struct smtp_address *mail_from;
	struct istream *input;
	time_t mtime;
	int ret;

	struct mail_storage_service_ctx *storage_service =
		mail_storage_service_user_get_service_ctx(dinput->rcpt_user->service_user);
	struct settings_instance *set_instance =
		mail_storage_service_user_get_settings_instance(dinput->rcpt_user->service_user);
	raw_mail_user = raw_storage_create_from_set(storage_service, set_instance);

	mail_from = (dinput->mail_from != NULL ?
		     dinput->mail_from : &default_envelope_sender);
	if (path == NULL) {
		input = create_raw_stream(dinput, 0, &mtime);
		i_stream_set_name(input, "stdin");
		ret = raw_mailbox_alloc_stream(raw_mail_user, input, mtime,
					       smtp_address_encode(mail_from), &box);
		i_stream_unref(&input);
	} else {
		ret = raw_mailbox_alloc_path(raw_mail_user, path, (time_t)-1,
					     smtp_address_encode(mail_from), &box);
	}
	if (ret < 0) {
		i_fatal("Can't open delivery mail as raw: %s",
			mailbox_get_last_internal_error(box, NULL));
	}
	mail_user_unref(&raw_mail_user);

	t = mailbox_transaction_begin(box, 0, __func__);
	headers_ctx = mailbox_header_lookup_init(box, wanted_headers);
	mail = mail_alloc(t, 0, headers_ctx);
	mailbox_header_lookup_unref(&headers_ctx);
	mail_set_seq(mail, 1);
	return mail;
}

static void
lda_set_rcpt_to(struct mail_deliver_input *dinput,
		const struct smtp_address *rcpt_to, const char *user,
		const char *rcpt_to_source)
{
	const char *error;

	if (rcpt_to == NULL &&
	    *dinput->set->lda_original_recipient_header != '\0') {
		rcpt_to = mail_deliver_get_address(
			dinput->src_mail,
			dinput->set->lda_original_recipient_header);
		rcpt_to_source = t_strconcat(
			dinput->set->lda_original_recipient_header,
			" header", NULL);
	}
	if (rcpt_to == NULL) {
		struct smtp_address *user_addr;

		if (smtp_address_parse_username(pool_datastack_create(), user,
						&user_addr, &error) < 0) {
			i_fatal_status(EX_USAGE,
				"Cannot obtain SMTP address from username `%s': %s",
				user, error);
		}
		if (user_addr->domain == NULL)
			user_addr->domain = dinput->set->hostname;
		rcpt_to = user_addr;
		rcpt_to_source = "user@hostname";
	}

	dinput->rcpt_params.orcpt.addr = rcpt_to;
	if (dinput->rcpt_to == NULL)
		dinput->rcpt_to = rcpt_to;

	e_debug(dinput->rcpt_user->event,
		"Destination address: %s (source: %s)",
		smtp_address_encode_path(rcpt_to), rcpt_to_source);
}

static int
lda_do_deliver(struct mail_deliver_context *ctx, bool stderr_rejection)
{
	enum mail_deliver_error error_code;
	const char *error;
	int ret;

	if (mail_deliver(ctx, &error_code, &error) >= 0)
		return EX_OK;

	if (error_code == MAIL_DELIVER_ERROR_INTERNAL) {
		/* This shouldn't happen */
		return EX_TEMPFAIL;
	}

	if (stderr_rejection) {
		/* write to stderr also for tempfails so that MTA
		   can log the reason if it wants to. */
		fprintf(stderr, "%s\n", error);
	}

	switch (error_code) {
	case MAIL_DELIVER_ERROR_NONE:
		i_unreached();
	case MAIL_DELIVER_ERROR_TEMPORARY:
		return EX_TEMPFAIL;
	case MAIL_DELIVER_ERROR_REJECTED:
		break;
	case MAIL_DELIVER_ERROR_NOQUOTA:
		if (ctx->set->quota_full_tempfail)
			return EX_TEMPFAIL;
		ctx->mailbox_full = TRUE;
		break;
	case MAIL_DELIVER_ERROR_INTERNAL:
		i_unreached();
	}

	/* Rejected */

	ctx->dsn = TRUE;

	/* we'll have to reply with permanent failure */
	mail_deliver_log(ctx, "rejected: %s",
			 str_sanitize(error, 512));

	if (stderr_rejection)
		return EX_NOPERM;
	ret = mail_send_rejection(ctx, ctx->rcpt_to, error);
	if (ret != 0)
		return ret < 0 ? EX_TEMPFAIL : ret;
	/* ok, rejection sent */

	return EX_OK;
}

static int
lda_deliver(struct mail_deliver_input *dinput,
	    const char *user, const char *path,
	    struct smtp_address *rcpt_to, const char *rcpt_to_source,
	    bool stderr_rejection)
{
	struct mail_deliver_context ctx;
	const char *error;
	int ret;

	if (settings_get(dinput->rcpt_user->event, &lda_setting_parser_info, 0,
			 &dinput->set, &error) < 0 ||
	    settings_get(dinput->rcpt_user->event,
			 &smtp_submit_setting_parser_info, 0,
			 &dinput->smtp_set, &error) < 0)
		i_fatal("%s", error);

	dinput->src_mail = lda_raw_mail_open(dinput, path);
	lda_set_rcpt_to(dinput, rcpt_to, user, rcpt_to_source);

	mail_deliver_init(&ctx, dinput);
	ret = lda_do_deliver(&ctx, stderr_rejection);
	mail_deliver_deinit(&ctx);

	return ret;
}

static void failure_exit_callback(int *status)
{
	/* we want all our exit codes to be sysexits.h compatible.
	   if we failed because of a logging related error, we most likely
	   aren't writing to stderr, so try writing there to give some kind of
	   a clue what's wrong. FATAL_LOGOPEN failure already wrote to
	   stderr, so don't duplicate it. */
	switch (*status) {
	case FATAL_LOGWRITE:
		fputs("Failed to write to log file", stderr);
		break;
	case FATAL_LOGERROR:
		fputs("Internal logging error", stderr);
		break;
	case FATAL_LOGOPEN:
	case FATAL_OUTOFMEM:
	case FATAL_EXEC:
	case FATAL_DEFAULT:
		break;
	default:
		return;
	}
	*status = EX_TEMPFAIL;
}

static void print_help(void)
{
	printf(
"Usage: dovecot-lda [-c <config file>] [-d <username>] [-p <path>]\n"
"                   [-m <mailbox>] [-e] [-k] [-f <envelope sender>]\n"
"                   [-a <original envelope recipient>]\n"
"                   [-r <final envelope recipient>] \n");
}

int main(int argc, char *argv[])
{
	struct mail_deliver_input dinput;
	enum mail_storage_service_flags service_flags = 0;
	const char *user, *errstr, *path, *error;
	struct smtp_address *rcpt_to, *final_rcpt_to, *mail_from;
	struct mail_storage_service_ctx *storage_service;
	struct mail_storage_service_input service_input;
	struct event *event;
	const char *user_source = "", *rcpt_to_source = "", *mail_from_error;
	uid_t process_euid;
	bool stderr_rejection = FALSE;
	int ret, c;

	i_set_failure_exit_callback(failure_exit_callback);

	master_service = master_service_init("lda",
		MASTER_SERVICE_FLAG_STANDALONE |
		MASTER_SERVICE_FLAG_DONT_LOG_TO_STDERR |
		MASTER_SERVICE_FLAG_NO_INIT_DATASTACK_FRAME,
		&argc, &argv, "a:d:ef:m:p:r:");

	event = event_create(NULL);
	event_add_category(event, &event_category_lda);

	i_zero(&dinput);
	dinput.session = mail_deliver_session_init();
	dinput.rcpt_default_mailbox = "INBOX";
	path = NULL;

	user = getenv("USER");
	mail_from = final_rcpt_to = rcpt_to = NULL;
	mail_from_error = NULL;
	while ((c = master_getopt(master_service)) > 0) {
		switch (c) {
		case 'a':
			/* original recipient address */
			if (smtp_address_parse_path(
				pool_datastack_create(), optarg,
				SMTP_ADDRESS_PARSE_FLAG_ALLOW_LOCALPART |
				SMTP_ADDRESS_PARSE_FLAG_BRACKETS_OPTIONAL,
				&rcpt_to, &errstr) < 0) {
				i_fatal_status(EX_USAGE,
					"Invalid -a parameter: %s", errstr);
			}
			rcpt_to_source = "-a parameter";
			break;
		case 'd':
			/* destination user */
			user = optarg;
			service_flags |= MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP;
			break;
		case 'e':
			stderr_rejection = TRUE;
			break;
		case 'f':
			/* envelope sender address */
			ret = smtp_address_parse_path(
				pool_datastack_create(), optarg,
				SMTP_ADDRESS_PARSE_FLAG_BRACKETS_OPTIONAL |
				SMTP_ADDRESS_PARSE_FLAG_ALLOW_LOCALPART |
				SMTP_ADDRESS_PARSE_FLAG_ALLOW_EMPTY |
				SMTP_ADDRESS_PARSE_FLAG_IGNORE_BROKEN |
				SMTP_ADDRESS_PARSE_FLAG_PRESERVE_RAW,
				&mail_from, &errstr);
			if (ret < 0 && !smtp_address_is_broken(mail_from)) {
				i_fatal_status(EX_USAGE,
					"Invalid -f parameter: %s", errstr);
			}
			if (ret < 0)
				mail_from_error = errstr;
			break;
		case 'm':
			/* destination mailbox.
			   Ignore -m "". This allows doing -m ${extension}
			   in Postfix to handle user+mailbox */
			if (*optarg != '\0') T_BEGIN {
				if (!uni_utf8_str_is_valid(optarg)) {
					i_fatal("Mailbox name not UTF-8: %s",
						optarg);
				}
				dinput.rcpt_default_mailbox = optarg;
			} T_END;
			break;
		case 'p':
			/* input path */
			if (t_abspath(optarg, &path, &errstr) < 0) {
				i_fatal("t_abspath(%s) failed: %s",
					optarg, errstr);
			}
			break;
		case 'r':
			/* final recipient address */
			if (smtp_address_parse_path(
				pool_datastack_create(), optarg,
				SMTP_ADDRESS_PARSE_FLAG_ALLOW_LOCALPART |
				SMTP_ADDRESS_PARSE_FLAG_BRACKETS_OPTIONAL,
				&final_rcpt_to, &errstr) < 0) {
				i_fatal_status(EX_USAGE,
					"Invalid -r parameter: %s", errstr);
			}
			break;
		default:
			print_help();
			return EX_USAGE;
		}
	}
	if (optind != argc) {
		print_help();
		i_fatal_status(EX_USAGE, "Unknown argument: %s", argv[optind]);
	}

	process_euid = geteuid();
	if ((service_flags & MAIL_STORAGE_SERVICE_FLAG_USERDB_LOOKUP) != 0)
		;
	else if (process_euid != 0) {
		/* we're non-root. get our username and possibly our home. */
		struct passwd pw;
		const char *home;

		home = getenv("HOME");
		if (user != NULL && home != NULL) {
			/* no need for a pw lookup */
			user_source = "USER environment";
		} else if ((ret = i_getpwuid(process_euid, &pw)) > 0) {
			user = t_strdup(pw.pw_name);
			if (home == NULL)
				env_put("HOME", pw.pw_dir);
			user_source = "passwd lookup for process euid";
		} else if (ret < 0) {
			/* temporary failure */
			i_fatal("getpwuid() failed: %m");
		} else if (user == NULL) {
			i_fatal_status(EX_USAGE,
				       "Couldn't lookup our username (uid=%s)",
				       dec2str(process_euid));
		}
		struct settings_root *set_root =
			master_service_get_settings_root(master_service);
		settings_root_override(set_root, "mail_home", home,
				       SETTINGS_OVERRIDE_TYPE_DEFAULT);
	} else {
		i_fatal_status(EX_USAGE,
			"destination user parameter (-d user) not given");
	}
	struct master_service_settings_input set_input = {
		.preserve_user = TRUE,
		.preserve_home = TRUE,
	};
	struct master_service_settings_output set_output;
	if (master_service_settings_read(master_service, &set_input,
					 &set_output, &error) < 0)
		i_fatal("%s", error);
	master_service_init_finish(master_service);

	dinput.mail_from = mail_from;
	dinput.rcpt_to = final_rcpt_to;

	event_add_str(event, "protocol", "lda");
	event_add_str(event, "user", user);
	if (mail_from != NULL) {
		event_add_str(event, "mail_from",
			      smtp_address_encode(mail_from));
	}
	if (final_rcpt_to != NULL) {
		event_add_str(event, "rcpt_to",
			      smtp_address_encode(final_rcpt_to));
	}

	i_zero(&service_input);
	service_input.service = "lda";
	service_input.username = user;
	service_input.event_parent = event;

	storage_service = mail_storage_service_init(master_service,
						    service_flags);
	mail_deliver_hooks_init();
	/* set before looking up the user (or ideally we'd do this between
	   _lookup() and _next(), but don't bother) */
	dinput.delivery_time_started = ioloop_timeval;
	ret = mail_storage_service_lookup_next(storage_service,
					       &service_input,
					       &dinput.rcpt_user,
					       &errstr);
	if (ret <= 0) {
		if (ret < 0)
			i_fatal("%s", errstr);
		ret = EX_NOUSER;
	} else {
#ifdef SIGXFSZ
		lib_signals_ignore(SIGXFSZ, TRUE);
#endif
		dinput.event_parent = dinput.rcpt_user->event;
		if (*user_source != '\0') {
			e_debug(dinput.rcpt_user->event,
				"userdb lookup skipped, username taken from %s",
				user_source);
		}
		if (mail_from_error != NULL) {
			e_debug(event, "Broken -f parameter: %s "
				"(proceeding with <> as sender)",
				mail_from_error);
		}

		ret = lda_deliver(&dinput, user, path,
				  rcpt_to, rcpt_to_source, stderr_rejection);

		struct mailbox_transaction_context *t =
			dinput.src_mail->transaction;
		struct mailbox *box = dinput.src_mail->box;

		mail_free(&dinput.src_mail);
		mailbox_transaction_rollback(&t);
		mailbox_free(&box);

		mail_user_deinit(&dinput.rcpt_user);
	}

	mail_deliver_session_deinit(&dinput.session);
	settings_free(dinput.set);
	settings_free(dinput.smtp_set);
	mail_storage_service_deinit(&storage_service);

	event_unref(&event);
	master_service_deinit(&master_service);
        return ret;
}
