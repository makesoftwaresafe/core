#ifndef QUOTA_H
#define QUOTA_H

struct mail;
struct mailbox;
struct mail_user;

/* Message storage size kilobytes. */
#define QUOTA_NAME_STORAGE_KILOBYTES "STORAGE"
/* Message storage size bytes. This is used only internally. */
#define QUOTA_NAME_STORAGE_BYTES "STORAGE_BYTES"
/* Number of messages. */
#define QUOTA_NAME_MESSAGES "MESSAGE"

struct quota;
struct quota_root;
struct quota_root_iter;
struct quota_transaction_context;

struct quota_param_parser {
	char *param_name;
	void (* param_handler)(struct quota_root *_root, const char *param_value);
};

extern struct quota_param_parser quota_param_hidden;
extern struct quota_param_parser quota_param_noenforcing;
extern struct quota_param_parser quota_param_ns;

enum quota_recalculate {
	QUOTA_RECALCULATE_DONT = 0,
	/* We may want to recalculate quota because we weren't able to call
	   quota_free*() correctly for all mails. Quota needs to be
	   recalculated unless the backend does the quota tracking
	   internally. */
	QUOTA_RECALCULATE_MISSING_FREES,
	/* doveadm quota recalc called - make sure the quota is correct */
	QUOTA_RECALCULATE_FORCED
};

enum quota_alloc_result {
	QUOTA_ALLOC_RESULT_OK,
	QUOTA_ALLOC_RESULT_TEMPFAIL,
	QUOTA_ALLOC_RESULT_OVER_MAXSIZE,
	QUOTA_ALLOC_RESULT_OVER_QUOTA,
	/* Mail size is larger than even the maximum allowed quota. */
	QUOTA_ALLOC_RESULT_OVER_QUOTA_LIMIT,
	/* Maximum number of messages per mailbox was reached */
	QUOTA_ALLOC_RESULT_OVER_QUOTA_MAILBOX_LIMIT,
	/* Blocked by ongoing background quota calculation. */
	QUOTA_ALLOC_RESULT_BACKGROUND_CALC,
};

/* Anything <= QUOTA_GET_RESULT_INTERNAL_ERROR is an error. */
enum quota_get_result {
	/* Ongoing background quota calculation */
	QUOTA_GET_RESULT_BACKGROUND_CALC,
	/* Quota resource name doesn't exist */
	QUOTA_GET_RESULT_UNKNOWN_RESOURCE,
	/* Internal error */
	QUOTA_GET_RESULT_INTERNAL_ERROR,

	/* Quota limit exists and was returned successfully */
	QUOTA_GET_RESULT_LIMITED,
	/* Quota is unlimited, but its value was returned */
	QUOTA_GET_RESULT_UNLIMITED,
};

struct quota_overrun {
	struct quota_root *root;

	struct {
		uoff_t count;
		uoff_t bytes;
	} resource;
};

const char *quota_alloc_result_errstr(enum quota_alloc_result res,
		struct quota_transaction_context *qt);

/* Initialize quota for the given user. Returns 0 and quota_r on success,
   -1 and error_r on failure. */
int quota_init(struct mail_user *user, struct quota **quota_r,
	       const char **error_r);
void quota_deinit(struct quota **quota);

/* List all visible quota roots. They don't need to be freed. */
struct quota_root_iter *quota_root_iter_init_user(struct mail_user *user);
struct quota_root_iter *quota_root_iter_init(struct mailbox *box);
struct quota_root *quota_root_iter_next(struct quota_root_iter *iter);
void quota_root_iter_deinit(struct quota_root_iter **iter);

/* Return quota root or NULL. */
struct quota_root *quota_root_lookup(struct mail_user *user, const char *name);

/* Returns name of the quota root. */
const char *quota_root_get_name(struct quota_root *root);
/* Return a list of all resources set for the quota root. */
const char *const *quota_root_get_resources(struct quota_root *root);
/* Returns TRUE if quota root is marked as hidden (so it shouldn't be visible
   to users via IMAP GETQUOTAROOT command). */
bool quota_root_is_hidden(struct quota_root *root);

/* Returns 1 if values were successfully returned, 0 if resource name doesn't
   exist or isn't enabled, -1 if error. */
enum quota_get_result
quota_get_resource(struct quota_root *root, struct mailbox *box,
		   const char *name, uint64_t *value_r, uint64_t *limit_r,
		   const char **error_r);

/* Start a new quota transaction. */
struct quota_transaction_context *quota_transaction_begin(struct mailbox *box);
/* Commit quota transaction. Returns 0 if ok, -1 if failed. */
int quota_transaction_commit(struct quota_transaction_context **ctx);
/* Rollback quota transaction changes. */
void quota_transaction_rollback(struct quota_transaction_context **ctx);

/* Allocate from quota if there's space. error_r is set when result is not
   QUOTA_ALLOC_RESULT_OK. overruns_r (if not NULL) is set when result is
   QUOTA_ALLOC_RESULT_OVER_QUOTA. This is a NULL-terminated array of struct
   quota_overrun which indicates which roots have overruns and how much is used.
 */
enum quota_alloc_result
quota_try_alloc(struct quota_transaction_context *ctx,
		struct mail *mail, struct mail *expunged_mail,
		const struct quota_overrun **overruns_r, const char **error_r);
/* Like quota_try_alloc(), but don't actually allocate anything. */
enum quota_alloc_result
quota_test_alloc(struct quota_transaction_context *ctx, uoff_t size,
		 struct mailbox *expunged_box, uoff_t expunged_size,
		 const struct quota_overrun **overruns_r, const char **error_r)
	ATTR_NULL(3);
/* Update quota by allocating/freeing space used by mail. */
void quota_alloc(struct quota_transaction_context *ctx, struct mail *mail);
void quota_free_bytes(struct quota_transaction_context *ctx,
		      uoff_t physical_size);
/* Mark the quota to be recalculated */
void quota_recalculate(struct quota_transaction_context *ctx,
		       enum quota_recalculate recalculate);

/* Execute quota_over_scripts if needed. */
void quota_over_status_check_startup(struct quota *quota);

#endif
