/* Copyright (c) 2004-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "ioloop.h"
#include "hex-binary.h"
#include "str.h"
#include "time-util.h"
#include "settings.h"
#include "sql-api-private.h"
#include "llist.h"

#ifdef BUILD_PGSQL
#include <libpq-fe.h>

#define PGSQL_DNS_WARN_MSECS 500

/* <settings checks> */
#define PGSQL_SQLPOOL_SET_NAME "pgsql"
/* </settings checks> */

struct pgsql_settings {
	pool_t pool;

	ARRAY_TYPE(const_string) sqlpool_hosts;
	unsigned int connection_limit;

	const char *host;
	ARRAY_TYPE(const_string) parameters;
};

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type("pgsql_"#name, name, struct pgsql_settings)
static const struct setting_define pgsql_setting_defines[] = {
	{ .type = SET_FILTER_ARRAY, .key = PGSQL_SQLPOOL_SET_NAME,
	  .offset = offsetof(struct pgsql_settings, sqlpool_hosts),
	  .filter_array_field_name = "pgsql_host", },
	DEF(UINT, connection_limit),

	DEF(STR, host),
	DEF(STRLIST, parameters),

	SETTING_DEFINE_LIST_END
};

static const struct pgsql_settings pgsql_default_settings = {
	.sqlpool_hosts = ARRAY_INIT,
	.connection_limit = SQL_DEFAULT_CONNECTION_LIMIT,

	.host = "",
	.parameters = ARRAY_INIT,
};

const struct setting_parser_info pgsql_setting_parser_info = {
	.name = "pgsql",
#ifdef SQL_DRIVER_PLUGINS
	.plugin_dependency = "libdriver_pgsql",
#endif

	.defines = pgsql_setting_defines,
	.defaults = &pgsql_default_settings,

	.struct_size = sizeof(struct pgsql_settings),
	.pool_offset1 = 1 + offsetof(struct pgsql_settings, pool),
};

struct pgsql_db {
	struct sql_db api;

	const struct pgsql_settings *set;
	PGconn *pg;

	struct io *io;
	struct timeout *to_connect;
	enum io_condition io_dir;

	struct pgsql_result *pending_results;
	struct pgsql_result *cur_result;
	struct ioloop *ioloop, *orig_ioloop;
	struct sql_result *sync_result;

	bool (*next_callback)(void *);
	void *next_context;

	char *error;
	const char *connect_state;

	bool fatal_error:1;
};

struct pgsql_binary_value {
	unsigned char *value;
	size_t size;
};

struct pgsql_result {
	struct sql_result api;

	struct pgsql_result *prev, *next;

	PGresult *pgres;
	struct timeout *to;

	unsigned int rownum, rows;
	unsigned int fields_count;
	const char **fields;
	const char **values;
	char *query;

	ARRAY(struct pgsql_binary_value) binary_values;

	sql_query_callback_t *callback;
	void *context;

	bool timeout:1;
};

struct pgsql_transaction_context {
	struct sql_transaction_context ctx;
	int refcount;

	sql_commit_callback_t *callback;
	void *context;

	pool_t query_pool;
	const char *error;

	bool failed:1;
};

struct pgsql_db_cache {
	/* Contains the sqlpool connection */
	struct sql_db *db;

	const struct pgsql_settings *set;
};

extern const struct sql_db driver_pgsql_db;
extern const struct sql_result driver_pgsql_result;

static ARRAY(struct pgsql_db_cache) pgsql_db_cache;

static void result_finish(struct pgsql_result *result);
static void
transaction_update_callback(struct sql_result *result,
			    struct sql_transaction_query *query);

static struct event_category event_category_pgsql = {
	.parent = &event_category_sql,
	.name = "pgsql"
};

static void pgsql_notice_processor(void *arg, const char *message)
{
	struct pgsql_db *db = arg;
	e_info(db->api.event, "%s", message);
}

static void driver_pgsql_set_state(struct pgsql_db *db, enum sql_db_state state)
{
	i_assert(state == SQL_DB_STATE_BUSY || db->cur_result == NULL);

	/* switch back to original ioloop in case the caller wants to
	   add/remove timeouts */
	if (db->ioloop != NULL)
		io_loop_set_current(db->orig_ioloop);
	sql_db_set_state(&db->api, state);
	if (db->ioloop != NULL)
		io_loop_set_current(db->ioloop);
}

static bool driver_pgsql_next_callback(struct pgsql_db *db)
{
	bool (*next_callback)(void *) = db->next_callback;
	void *next_context = db->next_context;

	if (next_callback == NULL)
		return FALSE;

	db->next_callback = NULL;
	db->next_context = NULL;
	return next_callback(next_context);
}

static void driver_pgsql_stop_io(struct pgsql_db *db)
{
	if (db->io != NULL) {
		io_remove(&db->io);
		db->io_dir = 0;
	}
}

static void driver_pgsql_close(struct pgsql_db *db)
{
	db->io_dir = 0;
	db->fatal_error = FALSE;

	driver_pgsql_stop_io(db);

	PQfinish(db->pg);
	db->pg = NULL;

	timeout_remove(&db->to_connect);

	driver_pgsql_set_state(db, SQL_DB_STATE_DISCONNECTED);
	sql_connection_log_finished(&db->api);

	if (db->ioloop != NULL) {
		/* running a sync query, stop it */
		io_loop_stop(db->ioloop);
	}
	driver_pgsql_next_callback(db);
}

static const char *last_error(struct pgsql_db *db)
{
	const char *msg, *pos, *orig;
	size_t len;

	orig = msg = PQerrorMessage(db->pg);
	if (msg == NULL)
		return "(no error set)";

	len = strlen(msg);
	/* The error can contain multiple lines, but we only want the last */
	while ((pos = strchr(msg, '\n')) != NULL && (pos - orig) < (ptrdiff_t)len - 1)
		msg = pos + 1;

	len = strlen(msg);
	/* Error message should contain trailing \n, we don't want it */
	return len == 0 || msg[len-1] != '\n' ? msg :
		t_strndup(msg, len-1);
}

static void connect_callback(struct pgsql_db *db)
{
	enum io_condition io_dir = 0;
	int ret;

	driver_pgsql_stop_io(db);

	while ((ret = PQconnectPoll(db->pg)) == PGRES_POLLING_ACTIVE)
		;

	switch (ret) {
	case PGRES_POLLING_READING:
		db->connect_state = "wait for input";
		io_dir = IO_READ;
		break;
	case PGRES_POLLING_WRITING:
		db->connect_state = "wait for output";
		io_dir = IO_WRITE;
		break;
	case PGRES_POLLING_OK:
		break;
	case PGRES_POLLING_FAILED:
		e_error(db->api.event, "Connect failed to database %s: %s (state: %s)",
			PQdb(db->pg), last_error(db), db->connect_state);
		i_free(db->api.last_connect_error);
		db->api.last_connect_error = i_strdup(last_error(db));
		driver_pgsql_close(db);
		return;
	}

	if (io_dir != 0) {
		db->io = io_add(PQsocket(db->pg), io_dir, connect_callback, db);
		db->io_dir = io_dir;
	}

	if (io_dir == 0) {
		db->connect_state = "connected";
		timeout_remove(&db->to_connect);
		if (PQserverVersion(db->pg) >= 90500) {
			/* v9.5+ */
			db->api.flags |= SQL_DB_FLAG_ON_CONFLICT_DO;
		}
		driver_pgsql_set_state(db, SQL_DB_STATE_IDLE);
		if (db->ioloop != NULL) {
			/* driver_pgsql_sync_init() waiting for connection to
			   finish */
			io_loop_stop(db->ioloop);
		}
	}
}

static void driver_pgsql_connect_timeout(struct pgsql_db *db)
{
	unsigned int secs = ioloop_time - db->api.last_connect_try;
	i_free(db->api.last_connect_error);
	db->api.last_connect_error = i_strdup_printf("Timeout after %u seconds (state: %s)",
						     secs, db->connect_state);
	e_error(db->api.event, "Connect failed: %s", db->api.last_connect_error);
	driver_pgsql_close(db);
}

static int driver_pgsql_connect(struct sql_db *_db)
{
	struct pgsql_db *db = (struct pgsql_db *)_db;
	struct timeval tv_start;
	long long msecs;

	i_assert(db->api.state == SQL_DB_STATE_DISCONNECTED);

	io_loop_time_refresh();
	tv_start = ioloop_timeval;

	ARRAY_TYPE(const_string) keywords, values;
	t_array_init(&keywords, 16);
	t_array_init(&values, 16);

	const char *host_str = "host";
	array_push_back(&keywords, &host_str);
	array_push_back(&values, &db->set->host);

	unsigned int i, count;
	const char *const *strings =
		array_get(&db->set->parameters, &count);
	for (i = 0; i < count; i += 2) {
		array_push_back(&keywords, &strings[i]);
		array_push_back(&values, &strings[i + 1]);
	}

	array_append_zero(&keywords);
	array_append_zero(&values);
	db->pg = PQconnectStartParams(array_front(&keywords),
				      array_front(&values), 0);
	if (db->pg == NULL) {
		i_fatal_status(FATAL_OUTOFMEM, "pgsql: PQconnectStart() failed (out of memory)");
	}

	(void)PQsetNoticeProcessor(db->pg, pgsql_notice_processor, db);

	if (PQstatus(db->pg) == CONNECTION_BAD) {
		const char *name = PQdb(db->pg);
		if (name == NULL)
			name = db->set->host;
		e_error(_db->event, "Connect failed to database %s: %s",
			name, last_error(db));
		i_free(db->api.last_connect_error);
		db->api.last_connect_error = i_strdup(last_error(db));
		driver_pgsql_close(db);
		return -1;
	}
	/* PQconnectStart() blocks on host name resolving. Log a warning if
	   it takes too long. Also don't include time spent on that in the
	   connect timeout (by refreshing ioloop time). */
	io_loop_time_refresh();
	msecs = timeval_diff_msecs(&ioloop_timeval, &tv_start);
	if (msecs > PGSQL_DNS_WARN_MSECS) {
		e_warning(_db->event, "DNS lookup took %lld.%03lld s",
			  msecs/1000, msecs % 1000);
	}

	/* nonblocking connecting begins. */
	if (PQsetnonblocking(db->pg, 1) < 0)
		e_error(_db->event, "PQsetnonblocking() failed");
	i_assert(db->to_connect == NULL);
	db->to_connect = timeout_add(SQL_CONNECT_TIMEOUT_SECS * 1000,
				     driver_pgsql_connect_timeout, db);
	db->connect_state = "connecting";
	db->io = io_add(PQsocket(db->pg), IO_WRITE, connect_callback, db);
	db->io_dir = IO_WRITE;
	driver_pgsql_set_state(db, SQL_DB_STATE_CONNECTING);
	return 0;
}

static void driver_pgsql_disconnect(struct sql_db *_db)
{
	struct pgsql_db *db = (struct pgsql_db *)_db;

	if (db->cur_result != NULL && db->cur_result->to != NULL) {
		driver_pgsql_stop_io(db);
		result_finish(db->cur_result);
	}

	_db->no_reconnect = TRUE;
	driver_pgsql_close(db);
	_db->no_reconnect = FALSE;
}

static void driver_pgsql_free(struct pgsql_db **_db)
{
	struct pgsql_db *db = *_db;
	*_db = NULL;

	event_unref(&db->api.event);
	settings_free(db->set);
	i_free(db->error);
	array_free(&db->api.module_contexts);
	i_free(db);
}

static enum sql_db_flags driver_pgsql_get_flags(struct sql_db *db)
{
	switch (db->state) {
	case SQL_DB_STATE_DISCONNECTED:
		if (sql_connect(db) < 0)
			break;
		/* fall through */
	case SQL_DB_STATE_CONNECTING:
		/* Wait for connection to finish, so we can get the flags
		   reliably. */
		sql_wait(db);
		break;
	case SQL_DB_STATE_IDLE:
	case SQL_DB_STATE_BUSY:
		break;
	}
	return db->flags;
}

static struct pgsql_db_cache *
driver_pgsql_db_cache_find(const struct pgsql_settings *set)
{
	struct pgsql_db_cache *cache;

	array_foreach_modifiable(&pgsql_db_cache, cache) {
		if (settings_equal(&pgsql_setting_parser_info,
				   set, cache->set, NULL))
			return cache;
	}
	return NULL;
}

static struct pgsql_db *
driver_pgsql_init_from_set(struct event *event_parent,
			   const struct pgsql_settings *set)
{
	struct pgsql_db *db;

	db = i_new(struct pgsql_db, 1);
	db->api = driver_pgsql_db;
	db->api.event = event_create(event_parent);
	db->set = set;
	event_add_category(db->api.event, &event_category_pgsql);
	event_add_str(db->api.event, "sql_driver", "pgsql");
	event_set_append_log_prefix(db->api.event,
				    t_strdup_printf("pgsql(%s): ", set->host));
	return db;
}

static int
driver_pgsql_init_v(struct event *event, struct sql_db **db_r,
		    const char **error_r)
{
	const struct pgsql_settings *set;

	if (settings_get(event, &pgsql_setting_parser_info, 0,
			 &set, error_r) < 0)
		return -1;
	if (array_is_empty(&set->sqlpool_hosts)) {
		*error_r = "pgsql { .. } named list filter is missing";
		settings_free(set);
		return -1;
	}

	if (event_get_ptr(event, SQLPOOL_EVENT_PTR) == NULL) {
		/* See if there is already such a database */
		struct pgsql_db_cache *cache =
			driver_pgsql_db_cache_find(set);
		if (cache != NULL)
			settings_free(set);
		else {
			/* Use sqlpool for managing multiple connections.
			   Leave an extra reference to it, so it won't be freed
			   while it's still in the cache array. */
			struct sql_db *db =
				driver_sqlpool_init(&driver_pgsql_db, event,
						    PGSQL_SQLPOOL_SET_NAME,
						    &set->sqlpool_hosts,
						    set->connection_limit);
			cache = array_append_space(&pgsql_db_cache);
			cache->db = db;
			cache->set = set;
		}
		sql_ref(cache->db);
		*db_r = cache->db;
		return 0;
	}
	/* We're being initialized by sqlpool - create a real pgsql
	 connection. */

	struct pgsql_db *db = driver_pgsql_init_from_set(event, set);
	event_drop_parent_log_prefixes(db->api.event, 1);
	sql_init_common(&db->api);
	*db_r = &db->api;
	return 0;
}

static void driver_pgsql_deinit_v(struct sql_db *_db)
{
	struct pgsql_db *db = (struct pgsql_db *)_db;

	driver_pgsql_disconnect(_db);
	driver_pgsql_free(&db);
}

static void driver_pgsql_set_idle(struct pgsql_db *db)
{
	i_assert(db->api.state == SQL_DB_STATE_BUSY);

	if (db->fatal_error)
		driver_pgsql_close(db);
	else if (!driver_pgsql_next_callback(db))
		driver_pgsql_set_state(db, SQL_DB_STATE_IDLE);
}

static void consume_results(struct pgsql_db *db)
{
	PGresult *pgres;

	driver_pgsql_stop_io(db);

	while (PQconsumeInput(db->pg) != 0) {
		if (PQisBusy(db->pg) != 0) {
			db->io = io_add(PQsocket(db->pg), IO_READ,
					consume_results, db);
			db->io_dir = IO_READ;
			return;
		}

		pgres = PQgetResult(db->pg);
		if (pgres == NULL)
			break;
		PQclear(pgres);
	}

	if (PQstatus(db->pg) == CONNECTION_BAD)
		driver_pgsql_close(db);
	else
		driver_pgsql_set_idle(db);
}

static void driver_pgsql_result_free(struct sql_result *_result)
{
	struct pgsql_db *db = (struct pgsql_db *)_result->db;
        struct pgsql_result *result = (struct pgsql_result *)_result;
	bool success;

	i_assert(!result->api.callback);
	i_assert(db->cur_result == result);
	i_assert(result->callback == NULL);

	if (_result == db->sync_result)
		db->sync_result = NULL;
	db->cur_result = NULL;

	success = result->pgres != NULL && !db->fatal_error;
	if (result->pgres != NULL) {
		PQclear(result->pgres);
		result->pgres = NULL;
	}

	if (success) {
		/* we'll have to read the rest of the results as well */
		i_assert(db->io == NULL);
		consume_results(db);
	} else {
		driver_pgsql_set_idle(db);
	}

	if (array_is_created(&result->binary_values)) {
		struct pgsql_binary_value *value;

		array_foreach_modifiable(&result->binary_values, value)
			PQfreemem(value->value);
		array_free(&result->binary_values);
	}

	event_unref(&result->api.event);
	i_free(result->query);
	i_free(result->fields);
	i_free(result->values);
	i_free(result);
}

static void result_finish(struct pgsql_result *result)
{
	struct pgsql_db *db = (struct pgsql_db *)result->api.db;
	bool free_result = TRUE;
	int duration;

	i_assert(db->io == NULL);
	timeout_remove(&result->to);
	DLLIST_REMOVE(&db->pending_results, result);

	/* if connection to server was lost, we don't yet see that the
	   connection is bad. we only see the fatal error, so assume it also
	   means disconnection. */
	if (PQstatus(db->pg) == CONNECTION_BAD || result->pgres == NULL ||
	    PQresultStatus(result->pgres) == PGRES_FATAL_ERROR)
		db->fatal_error = TRUE;

	if (db->fatal_error) {
		result->api.failed = TRUE;
		result->api.failed_try_retry = TRUE;
	}

	/* emit event */
	if (result->api.failed) {
		const char *error = result->timeout ? "Timed out" : last_error(db);
		struct event_passthrough *e =
			sql_query_finished_event(&db->api, result->api.event,
						 result->query, TRUE, &duration);
		e->add_str("error", error);
		e_debug(e->event(), SQL_QUERY_FINISHED_FMT": %s", result->query,
			duration, error);
	} else {
		struct event_passthrough *e =
			sql_query_finished_event(&db->api, result->api.event,
						 result->query, FALSE, &duration);
		e_debug(e->event(), SQL_QUERY_FINISHED_FMT,
			result->query, duration);
	}
	result->api.callback = TRUE;
	T_BEGIN {
		if (result->callback != NULL)
			result->callback(&result->api, result->context);
	} T_END;
	result->api.callback = FALSE;

	free_result = db->sync_result != &result->api;
	if (db->ioloop != NULL)
		io_loop_stop(db->ioloop);

	i_assert(!free_result || result->api.refcount > 0);
	result->callback = NULL;
	if (free_result)
		sql_result_unref(&result->api);
}

static void get_result(struct pgsql_result *result)
{
        struct pgsql_db *db = (struct pgsql_db *)result->api.db;

	driver_pgsql_stop_io(db);

	if (PQconsumeInput(db->pg) == 0) {
		result_finish(result);
		return;
	}

	if (PQisBusy(db->pg) != 0) {
		db->io = io_add(PQsocket(db->pg), IO_READ,
				get_result, result);
		db->io_dir = IO_READ;
		return;
	}

	result->pgres = PQgetResult(db->pg);
	result_finish(result);
}

static void flush_callback(struct pgsql_result *result)
{
        struct pgsql_db *db = (struct pgsql_db *)result->api.db;
	int ret;

	driver_pgsql_stop_io(db);

	ret = PQflush(db->pg);
	if (ret > 0) {
		db->io = io_add(PQsocket(db->pg), IO_WRITE,
				flush_callback, result);
		db->io_dir = IO_WRITE;
		return;
	}

	if (ret < 0) {
		result_finish(result);
	} else {
		/* all flushed */
		get_result(result);
	}
}

static void query_timeout(struct pgsql_result *result)
{
        struct pgsql_db *db = (struct pgsql_db *)result->api.db;

	driver_pgsql_stop_io(db);

	result->timeout = TRUE;
	result_finish(result);
}

static void do_query(struct pgsql_result *result, const char *query)
{
        struct pgsql_db *db = (struct pgsql_db *)result->api.db;
	int ret;

	i_assert(SQL_DB_IS_READY(&db->api));
	i_assert(db->cur_result == NULL);
	i_assert(db->io == NULL);

	driver_pgsql_set_state(db, SQL_DB_STATE_BUSY);
	db->cur_result = result;
	DLLIST_PREPEND(&db->pending_results, result);
	result->to = timeout_add(SQL_QUERY_TIMEOUT_SECS * 1000,
				 query_timeout, result);
	result->query = i_strdup(query);

	if (PQsendQuery(db->pg, query) == 0 ||
	    (ret = PQflush(db->pg)) < 0) {
		/* failed to send query */
		result_finish(result);
		return;
	}

	if (ret > 0) {
		/* write blocks */
		db->io = io_add(PQsocket(db->pg), IO_WRITE,
				flush_callback, result);
		db->io_dir = IO_WRITE;
	} else {
		get_result(result);
	}
}

static const char *
driver_pgsql_escape_string(struct sql_db *_db, const char *string)
{
	struct pgsql_db *db = (struct pgsql_db *)_db;
	size_t len = strlen(string);
	char *to;

#ifdef HAVE_PQESCAPE_STRING_CONN
	if (db->api.state == SQL_DB_STATE_DISCONNECTED) {
		/* try connecting again */
		(void)sql_connect(&db->api);
	}
	if (db->api.state != SQL_DB_STATE_DISCONNECTED) {
		int error;

		to = t_buffer_get(len * 2 + 1);
		len = PQescapeStringConn(db->pg, to, string, len, &error);
	} else
#endif
	{
		to = t_buffer_get(len * 2 + 1);
		len = PQescapeString(to, string, len);
	}
	t_buffer_alloc(len + 1);
	return to;
}

static void exec_callback(struct sql_result *_result,
			  void *context ATTR_UNUSED)
{
	struct pgsql_result *result = (struct pgsql_result*)_result;
	result_finish(result);
}

static void driver_pgsql_exec(struct sql_db *db, const char *query)
{
	struct pgsql_result *result;

	result = i_new(struct pgsql_result, 1);
	result->api = driver_pgsql_result;
	result->api.db = db;
	result->api.refcount = 1;
	result->api.event = event_create(db->event);
	result->callback = exec_callback;
	do_query(result, query);
}

static void driver_pgsql_query(struct sql_db *db, const char *query,
			       sql_query_callback_t *callback, void *context)
{
	struct pgsql_result *result;

	result = i_new(struct pgsql_result, 1);
	result->api = driver_pgsql_result;
	result->api.db = db;
	result->api.refcount = 1;
	result->api.event = event_create(db->event);
	result->callback = callback;
	result->context = context;
	do_query(result, query);
}

static void pgsql_query_s_callback(struct sql_result *result, void *context)
{
        struct pgsql_db *db = context;

	db->sync_result = result;
}

static void driver_pgsql_sync_init(struct pgsql_db *db)
{
	bool add_to_connect;

	db->orig_ioloop = current_ioloop;
	if (db->io == NULL) {
		db->ioloop = io_loop_create();
		return;
	}

	i_assert(db->api.state == SQL_DB_STATE_CONNECTING);

	/* have to move our existing I/O and timeout handlers to new I/O loop */
	io_remove(&db->io);

	add_to_connect = (db->to_connect != NULL);
	timeout_remove(&db->to_connect);

	db->ioloop = io_loop_create();
	if (add_to_connect) {
		db->to_connect = timeout_add(SQL_CONNECT_TIMEOUT_SECS * 1000,
					     driver_pgsql_connect_timeout, db);
	}
	db->io = io_add(PQsocket(db->pg), db->io_dir, connect_callback, db);
	/* wait for connecting to finish */
	io_loop_run(db->ioloop);
}

static void driver_pgsql_sync_deinit(struct pgsql_db *db)
{
	io_loop_destroy(&db->ioloop);
}

static struct sql_result *
driver_pgsql_sync_query(struct pgsql_db *db, const char *query)
{
	struct sql_result *result;

	i_assert(db->sync_result == NULL);

	switch (db->api.state) {
	case SQL_DB_STATE_CONNECTING:
	case SQL_DB_STATE_BUSY:
		i_unreached();
	case SQL_DB_STATE_DISCONNECTED:
		sql_not_connected_result.refcount++;
		return &sql_not_connected_result;
	case SQL_DB_STATE_IDLE:
		break;
	}

	driver_pgsql_query(&db->api, query, pgsql_query_s_callback, db);
	if (db->sync_result == NULL)
		io_loop_run(db->ioloop);

	i_assert(db->io == NULL);

	result = db->sync_result;
	if (result == &sql_not_connected_result) {
		/* we don't end up in pgsql's free function, so sync_result
		   won't be set to NULL if we don't do it here. */
		db->sync_result = NULL;
	} else if (result == NULL) {
		result = &sql_not_connected_result;
		result->refcount++;
	}

	i_assert(db->io == NULL);
	return result;
}

static struct sql_result *
driver_pgsql_query_s(struct sql_db *_db, const char *query)
{
	struct pgsql_db *db = (struct pgsql_db *)_db;
	struct sql_result *result;

	driver_pgsql_sync_init(db);
	result = driver_pgsql_sync_query(db, query);
	driver_pgsql_sync_deinit(db);
	return result;
}

static int driver_pgsql_result_next_row(struct sql_result *_result)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;
	struct pgsql_db *db = (struct pgsql_db *)_result->db;

	if (result->rows != 0) {
		/* second time we're here */
		if (++result->rownum < result->rows)
			return 1;

		/* end of this packet. see if there's more. FIXME: this may
		   block, but the current API doesn't provide a non-blocking
		   way to do this.. */
		PQclear(result->pgres);
		result->pgres = PQgetResult(db->pg);
		if (result->pgres == NULL)
			return 0;
	}

	if (result->pgres == NULL) {
		_result->failed = TRUE;
		return -1;
	}

	switch (PQresultStatus(result->pgres)) {
	case PGRES_COMMAND_OK:
		/* no rows returned */
		return 0;
	case PGRES_TUPLES_OK:
		result->rows = PQntuples(result->pgres);
		return result->rows > 0 ? 1 : 0;
	case PGRES_EMPTY_QUERY:
	case PGRES_NONFATAL_ERROR:
		/* nonfatal error */
		_result->failed = TRUE;
		return -1;
	default:
		/* treat as fatal error */
		_result->failed = TRUE;
		db->fatal_error = TRUE;
		return -1;
	}
}

static void driver_pgsql_result_fetch_fields(struct pgsql_result *result)
{
	unsigned int i;

	if (result->fields != NULL)
		return;

	/* @UNSAFE */
	result->fields_count = PQnfields(result->pgres);
	result->fields = i_new(const char *, result->fields_count);
	for (i = 0; i < result->fields_count; i++)
		result->fields[i] = PQfname(result->pgres, i);
}

static unsigned int
driver_pgsql_result_get_fields_count(struct sql_result *_result)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;

        driver_pgsql_result_fetch_fields(result);
	return result->fields_count;
}

static const char *
driver_pgsql_result_get_field_name(struct sql_result *_result, unsigned int idx)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;

	driver_pgsql_result_fetch_fields(result);
	i_assert(idx < result->fields_count);
	return result->fields[idx];
}

static int driver_pgsql_result_find_field(struct sql_result *_result,
					  const char *field_name)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;
	unsigned int i;

	driver_pgsql_result_fetch_fields(result);
	for (i = 0; i < result->fields_count; i++) {
		if (strcmp(result->fields[i], field_name) == 0)
			return i;
	}
	return -1;
}

static const char *
driver_pgsql_result_get_field_value(struct sql_result *_result,
				    unsigned int idx)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;

	if (PQgetisnull(result->pgres, result->rownum, idx) != 0)
		return NULL;

	return PQgetvalue(result->pgres, result->rownum, idx);
}

static const unsigned char *
driver_pgsql_result_get_field_value_binary(struct sql_result *_result,
					   unsigned int idx, size_t *size_r)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;
	const char *value;
	struct pgsql_binary_value *binary_value;

	if (PQgetisnull(result->pgres, result->rownum, idx) != 0) {
		*size_r = 0;
		return NULL;
	}

	value = PQgetvalue(result->pgres, result->rownum, idx);

	if (!array_is_created(&result->binary_values))
		i_array_init(&result->binary_values, idx + 1);

	binary_value = array_idx_get_space(&result->binary_values, idx);
	if (binary_value->value == NULL) {
		binary_value->value =
			PQunescapeBytea((const unsigned char *)value,
					&binary_value->size);
	}

	*size_r = binary_value->size;
	return binary_value->value;
}

static const char *
driver_pgsql_result_find_field_value(struct sql_result *result,
				     const char *field_name)
{
	int idx;

	idx = driver_pgsql_result_find_field(result, field_name);
	if (idx < 0)
		return NULL;
	return driver_pgsql_result_get_field_value(result, idx);
}

static const char *const *
driver_pgsql_result_get_values(struct sql_result *_result)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;
	unsigned int i;

	if (result->values == NULL) {
		driver_pgsql_result_fetch_fields(result);
		result->values = i_new(const char *, result->fields_count);
	}

	/* @UNSAFE */
	for (i = 0; i < result->fields_count; i++) {
		result->values[i] =
                        driver_pgsql_result_get_field_value(_result, i);
	}

	return result->values;
}

static const char *driver_pgsql_result_get_error(struct sql_result *_result)
{
	struct pgsql_result *result = (struct pgsql_result *)_result;
	struct pgsql_db *db = (struct pgsql_db *)_result->db;
	const char *msg;
	size_t len;

	i_free_and_null(db->error);

	if (result->timeout) {
		db->error = i_strdup("Query timed out");
	} else if (result->pgres == NULL) {
		/* connection error */
		db->error = i_strdup(last_error(db));
	} else {
		msg = PQresultErrorMessage(result->pgres);
		if (msg == NULL)
			return "(no error set)";

		/* Error message should contain trailing \n, we don't want it */
		len = strlen(msg);
		db->error = len == 0 || msg[len-1] != '\n' ?
			i_strdup(msg) : i_strndup(msg, len-1);
	}
	return db->error;
}

static struct sql_transaction_context *
driver_pgsql_transaction_begin(struct sql_db *db)
{
	struct pgsql_transaction_context *ctx;

	ctx = i_new(struct pgsql_transaction_context, 1);
	ctx->ctx.db = db;
	ctx->ctx.event = event_create(db->event);
	/* we need to be able to handle multiple open transactions, so at least
	   for now just keep them in memory until commit time. */
	ctx->query_pool = pool_alloconly_create("pgsql transaction", 1024);
	return &ctx->ctx;
}

static void
driver_pgsql_transaction_free(struct pgsql_transaction_context *ctx)
{
	pool_unref(&ctx->query_pool);
	event_unref(&ctx->ctx.event);
	i_free(ctx);
}

static void
transaction_commit_callback(struct sql_result *result,
			    struct pgsql_transaction_context *ctx)
{
	struct sql_commit_result commit_result;

	i_zero(&commit_result);
	if (sql_result_next_row(result) < 0) {
		commit_result.error = sql_result_get_error(result);
		commit_result.error_type = sql_result_get_error_type(result);
	}
	ctx->callback(&commit_result, ctx->context);
	driver_pgsql_transaction_free(ctx);
}

static bool transaction_send_next(void *context)
{
	struct pgsql_transaction_context *ctx = context;

	i_assert(!ctx->failed);

	if (ctx->ctx.db->state == SQL_DB_STATE_BUSY) {
		/* kludgy.. */
		ctx->ctx.db->state = SQL_DB_STATE_IDLE;
	} else if (!SQL_DB_IS_READY(ctx->ctx.db)) {
		struct sql_commit_result commit_result = {
			.error = "Not connected"
		};
		ctx->callback(&commit_result, ctx->context);
		return FALSE;
	}

	if (ctx->ctx.head != NULL) {
		struct sql_transaction_query *query = ctx->ctx.head;

		ctx->ctx.head = ctx->ctx.head->next;
		sql_query(ctx->ctx.db, query->query,
			  transaction_update_callback, query);
	} else {
		sql_query(ctx->ctx.db, "COMMIT",
			  transaction_commit_callback, ctx);
	}
	return TRUE;
}

static void
transaction_commit_error_callback(struct pgsql_transaction_context *ctx,
				  struct sql_result *result)
{
	struct sql_commit_result commit_result;

	i_zero(&commit_result);
	commit_result.error = sql_result_get_error(result);
	commit_result.error_type = sql_result_get_error_type(result);
	e_debug(sql_transaction_finished_event(&ctx->ctx)->
		add_str("error", commit_result.error)->event(),
		"Transaction failed: %s", commit_result.error);
	ctx->callback(&commit_result, ctx->context);
}

static void
transaction_begin_callback(struct sql_result *result,
			   struct pgsql_transaction_context *ctx)
{
	struct pgsql_db *db = (struct pgsql_db *)result->db;

	i_assert(result->db == ctx->ctx.db);

	if (sql_result_next_row(result) < 0) {
		transaction_commit_error_callback(ctx, result);
		driver_pgsql_transaction_free(ctx);
		return;
	}
	i_assert(db->next_callback == NULL);
	db->next_callback = transaction_send_next;
	db->next_context = ctx;
}

static void
transaction_update_callback(struct sql_result *result,
			    struct sql_transaction_query *query)
{
	struct pgsql_transaction_context *ctx =
		(struct pgsql_transaction_context *)query->trans;
	struct pgsql_db *db = (struct pgsql_db *)result->db;

	if (sql_result_next_row(result) < 0) {
		transaction_commit_error_callback(ctx, result);
		driver_pgsql_transaction_free(ctx);
		return;
	}

	if (query->affected_rows != NULL) {
		struct pgsql_result *pg_result = (struct pgsql_result *)result;

		if (str_to_uint(PQcmdTuples(pg_result->pgres),
				query->affected_rows) < 0)
			i_unreached();
	}
	i_assert(db->next_callback == NULL);
	db->next_callback = transaction_send_next;
	db->next_context = ctx;
}

static void
transaction_trans_query_callback(struct sql_result *result,
				 struct sql_transaction_query *query)
{
	struct pgsql_transaction_context *ctx =
		(struct pgsql_transaction_context *)query->trans;
	struct sql_commit_result commit_result;

	if (sql_result_next_row(result) < 0) {
		transaction_commit_error_callback(ctx, result);
		driver_pgsql_transaction_free(ctx);
		return;
	}

	if (query->affected_rows != NULL) {
		struct pgsql_result *pg_result = (struct pgsql_result *)result;

		if (str_to_uint(PQcmdTuples(pg_result->pgres),
				query->affected_rows) < 0)
			i_unreached();
	}
	e_debug(sql_transaction_finished_event(&ctx->ctx)->event(),
		"Transaction committed");
	i_zero(&commit_result);
	ctx->callback(&commit_result, ctx->context);
	driver_pgsql_transaction_free(ctx);
}

static void
driver_pgsql_transaction_commit(struct sql_transaction_context *_ctx,
				sql_commit_callback_t *callback, void *context)
{
	struct pgsql_transaction_context *ctx =
		(struct pgsql_transaction_context *)_ctx;
	struct sql_commit_result result;

	i_zero(&result);
	ctx->callback = callback;
	ctx->context = context;

	if (ctx->failed || _ctx->head == NULL) {
		if (ctx->failed) {
			result.error = ctx->error;
			e_debug(sql_transaction_finished_event(_ctx)->
				add_str("error", ctx->error)->event(),
				"Transaction failed: %s", ctx->error);
		} else {
			e_debug(sql_transaction_finished_event(_ctx)->event(),
				"Transaction committed");
		}
		callback(&result, context);
		driver_pgsql_transaction_free(ctx);
	} else if (_ctx->head->next == NULL) {
		/* just a single query, send it */
		sql_query(_ctx->db, _ctx->head->query,
			  transaction_trans_query_callback, _ctx->head);
	} else {
		/* multiple queries, use a transaction */
		i_assert(_ctx->db->v.query == driver_pgsql_query);
		sql_query(_ctx->db, "BEGIN", transaction_begin_callback, ctx);
	}
}

static void
commit_multi_fail(struct pgsql_transaction_context *ctx,
		  struct sql_result *result, const char *query)
{
	ctx->failed = TRUE;
	ctx->error = t_strdup_printf("%s (query: %s)",
				     sql_result_get_error(result), query);
	sql_result_unref(result);
}

static struct sql_result *
driver_pgsql_transaction_commit_multi(struct pgsql_transaction_context *ctx)
{
	struct pgsql_db *db = (struct pgsql_db *)ctx->ctx.db;
	struct sql_result *result;
	struct sql_transaction_query *query;

	result = driver_pgsql_sync_query(db, "BEGIN");
	if (sql_result_next_row(result) < 0) {
		commit_multi_fail(ctx, result, "BEGIN");
		return NULL;
	}
	sql_result_unref(result);

	/* send queries */
	for (query = ctx->ctx.head; query != NULL; query = query->next) {
		result = driver_pgsql_sync_query(db, query->query);
		if (sql_result_next_row(result) < 0) {
			commit_multi_fail(ctx, result, query->query);
			break;
		}
		if (query->affected_rows != NULL) {
			struct pgsql_result *pg_result =
				(struct pgsql_result *)result;

			if (str_to_uint(PQcmdTuples(pg_result->pgres),
					query->affected_rows) < 0)
				i_unreached();
		}
		sql_result_unref(result);
	}

	return driver_pgsql_sync_query(db, ctx->failed ?
				       "ROLLBACK" : "COMMIT");
}

static void
driver_pgsql_try_commit_s(struct pgsql_transaction_context *ctx,
			  const char **error_r)
{
	struct sql_transaction_context *_ctx = &ctx->ctx;
	struct pgsql_db *db = (struct pgsql_db *)_ctx->db;
	struct sql_transaction_query *single_query = NULL;
	struct sql_result *result;

	if (_ctx->head->next == NULL) {
		/* just a single query, send it */
		single_query = _ctx->head;
		result = sql_query_s(_ctx->db, single_query->query);
	} else {
		/* multiple queries, use a transaction */
		driver_pgsql_sync_init(db);
		result = driver_pgsql_transaction_commit_multi(ctx);
		driver_pgsql_sync_deinit(db);
	}

	if (ctx->failed) {
		i_assert(ctx->error != NULL);
		e_debug(sql_transaction_finished_event(_ctx)->
			add_str("error", ctx->error)->event(),
			"Transaction failed: %s", ctx->error);
		*error_r = ctx->error;
	} else if (result != NULL) {
		if (sql_result_next_row(result) < 0)
			*error_r = sql_result_get_error(result);
		else if (single_query != NULL &&
			 single_query->affected_rows != NULL) {
			struct pgsql_result *pg_result =
				(struct pgsql_result *)result;

			if (str_to_uint(PQcmdTuples(pg_result->pgres),
					single_query->affected_rows) < 0)
				i_unreached();
		}
	}

	if (!ctx->failed) {
		e_debug(sql_transaction_finished_event(_ctx)->event(),
			"Transaction committed");
	}

	if (result != NULL)
		sql_result_unref(result);
}

static int
driver_pgsql_transaction_commit_s(struct sql_transaction_context *_ctx,
				  const char **error_r)
{
	struct pgsql_transaction_context *ctx =
		(struct pgsql_transaction_context *)_ctx;
	struct pgsql_db *db = (struct pgsql_db *)_ctx->db;

	*error_r = NULL;

	if (_ctx->head != NULL) {
		driver_pgsql_try_commit_s(ctx, error_r);
		if (_ctx->db->state == SQL_DB_STATE_DISCONNECTED) {
			*error_r = t_strdup(*error_r);
			e_info(db->api.event, "Disconnected from database, "
			       "retrying commit");
			if (sql_connect(_ctx->db) >= 0) {
				ctx->failed = FALSE;
				*error_r = NULL;
				driver_pgsql_try_commit_s(ctx, error_r);
			}
		}
	}

	driver_pgsql_transaction_free(ctx);
	return *error_r == NULL ? 0 : -1;
}

static void
driver_pgsql_transaction_rollback(struct sql_transaction_context *_ctx)
{
	struct pgsql_transaction_context *ctx =
		(struct pgsql_transaction_context *)_ctx;
	e_debug(sql_transaction_finished_event(_ctx)->
			add_str("error", "Rolled back")->event(),
		"Transaction rolled back");

	driver_pgsql_transaction_free(ctx);
}

static void
driver_pgsql_update(struct sql_transaction_context *_ctx, const char *query,
		    unsigned int *affected_rows)
{
	struct pgsql_transaction_context *ctx =
		(struct pgsql_transaction_context *)_ctx;

	sql_transaction_add_query(_ctx, ctx->query_pool, query, affected_rows);
}

static const char *
driver_pgsql_escape_blob(struct sql_db *_db ATTR_UNUSED,
			 const unsigned char *data, size_t size)
{
	string_t *str = t_str_new(128);

	str_append(str, "E'\\\\x");
	binary_to_hex_append(str, data, size);
	str_append_c(str, '\'');
	return str_c(str);
}

static bool driver_pgsql_have_work(struct pgsql_db *db)
{
	return db->next_callback != NULL || db->pending_results != NULL ||
		db->api.state == SQL_DB_STATE_CONNECTING;
}

static void driver_pgsql_wait(struct sql_db *_db)
{
	struct pgsql_db *db = (struct pgsql_db *)_db;

	if (!driver_pgsql_have_work(db))
		return;

	db->orig_ioloop = current_ioloop;
	db->ioloop = io_loop_create();
	db->io = io_loop_move_io(&db->io);
	while (driver_pgsql_have_work(db))
		io_loop_run(db->ioloop);

	io_loop_set_current(db->orig_ioloop);
	db->io = io_loop_move_io(&db->io);
	io_loop_set_current(db->ioloop);
	io_loop_destroy(&db->ioloop);
}

const struct sql_db driver_pgsql_db = {
	.name = "pgsql",
	.flags = SQL_DB_FLAG_POOLED,

	.v = {
		.get_flags = driver_pgsql_get_flags,
		.init = driver_pgsql_init_v,
		.deinit = driver_pgsql_deinit_v,
		.connect = driver_pgsql_connect,
		.disconnect = driver_pgsql_disconnect,
		.escape_string = driver_pgsql_escape_string,
		.exec = driver_pgsql_exec,
		.query = driver_pgsql_query,
		.query_s = driver_pgsql_query_s,
		.wait = driver_pgsql_wait,

		.transaction_begin = driver_pgsql_transaction_begin,
		.transaction_commit = driver_pgsql_transaction_commit,
		.transaction_commit_s = driver_pgsql_transaction_commit_s,
		.transaction_rollback = driver_pgsql_transaction_rollback,

		.update = driver_pgsql_update,

		.escape_blob = driver_pgsql_escape_blob,
	}
};

const struct sql_result driver_pgsql_result = {
	.v = {
		.free = driver_pgsql_result_free,
		.next_row = driver_pgsql_result_next_row,
		.get_fields_count = driver_pgsql_result_get_fields_count,
		.get_field_name = driver_pgsql_result_get_field_name,
		.find_field = driver_pgsql_result_find_field,
		.get_field_value = driver_pgsql_result_get_field_value,
		.get_field_value_binary = driver_pgsql_result_get_field_value_binary,
		.find_field_value = driver_pgsql_result_find_field_value,
		.get_values = driver_pgsql_result_get_values,
		.get_error = driver_pgsql_result_get_error,
	}
};

const char *driver_pgsql_version = DOVECOT_ABI_VERSION;

void driver_pgsql_init(void)
{
	i_array_init(&pgsql_db_cache, 4);
	sql_driver_register(&driver_pgsql_db);
}

void driver_pgsql_deinit(void)
{
	struct pgsql_db_cache *cache;

	array_foreach_modifiable(&pgsql_db_cache, cache) {
		settings_free(cache->set);
		sql_unref(&cache->db);
	}
	array_free(&pgsql_db_cache);
	sql_driver_unregister(&driver_pgsql_db);
}

#endif
