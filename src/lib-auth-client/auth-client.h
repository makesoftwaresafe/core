#ifndef AUTH_CLIENT_H
#define AUTH_CLIENT_H

#include "net.h"
#include "auth-client-interface.h"

struct auth_client;
struct auth_client_request;

enum auth_request_flags {
	/* Connection from the previous hop (client, proxy, haproxy) is
	   considered secured. Either because TLS is used, or because the
	   connection is otherwise considered not to need TLS. Note that this
	   doesn't necessarily mean that the client connection behind the
	   previous hop is secured. */
	AUTH_REQUEST_FLAG_CONN_SECURED		= 0x01,
	AUTH_REQUEST_FLAG_VALID_CLIENT_CERT	= 0x02,
	/* Skip penalty checks for this request */
	AUTH_REQUEST_FLAG_NO_PENALTY		= 0x04,
	/* Support final SASL response (handled locally) */
	AUTH_REQUEST_FLAG_SUPPORT_FINAL_RESP	= 0x08,
	/* Enable auth_debug=yes logging for this request */
	AUTH_REQUEST_FLAG_DEBUG			= 0x10,
	/* Connection from the previous hop is secured by TLS. */
	AUTH_REQUEST_FLAG_CONN_SECURED_TLS	= 0x20,
};

enum auth_request_status {
	AUTH_REQUEST_STATUS_ABORT = -3,
	AUTH_REQUEST_STATUS_INTERNAL_FAIL = -2,
	AUTH_REQUEST_STATUS_FAIL = -1,
	AUTH_REQUEST_STATUS_CONTINUE,
	AUTH_REQUEST_STATUS_OK
};

struct auth_mech_desc {
	char *name;
        enum mech_security_flags flags;
};

struct auth_connect_id {
	unsigned int server_pid;
	unsigned int connect_uid;
};

struct auth_request_info {
	const char *mech;
	const char *protocol;
	const char *session_id;
	const char *cert_username;
	const char *local_name;
	const char *client_id;
	const char *const *forward_fields;
	ARRAY_TYPE(const_string) extra_fields;

	unsigned int ssl_cipher_bits;
	const char *ssl_cipher;
	const char *ssl_pfs;
	const char *ssl_protocol;
	const char *ssl_ja3_hash;
	const char *ssl_client_cert_fp;
	const char *ssl_client_cert_pubkey_fp;

	enum auth_request_flags flags;

	struct ip_addr local_ip, remote_ip, real_local_ip, real_remote_ip;
	in_port_t local_port, remote_port, real_local_port, real_remote_port;

	const char *initial_resp_base64;
};

typedef void auth_request_callback_t(struct auth_client_request *request,
				     enum auth_request_status status,
				     const char *data_base64,
				     const char *const *args, void *context);

typedef int auth_channel_binding_callback_t(const char *type, void *context,
					    const buffer_t **data_r,
					    const char **error_r);

typedef void auth_connect_notify_callback_t(struct auth_client *client,
					    bool connected, void *context);

/* Create new authentication client. */
struct auth_client *
auth_client_init(const char *auth_socket_path, unsigned int client_pid,
		 bool debug);
void auth_client_deinit(struct auth_client **client);

void auth_client_connect(struct auth_client *client);
void auth_client_disconnect(struct auth_client *client, const char *reason);
bool auth_client_is_connected(struct auth_client *client);
bool auth_client_is_disconnected(struct auth_client *client);

void auth_client_set_connect_timeout(struct auth_client *client,
				     unsigned int msecs);
void auth_client_set_connect_notify(struct auth_client *client,
				    auth_connect_notify_callback_t *callback,
				    void *context);
const struct auth_mech_desc *
auth_client_get_available_mechs(struct auth_client *client,
				unsigned int *mech_count);
const struct auth_mech_desc *
auth_client_find_mech(struct auth_client *client, const char *name);

/* Return current connection's identifiers. */
void auth_client_get_connect_id(struct auth_client *client,
				unsigned int *server_pid_r,
				unsigned int *connect_uid_r);

/* Create a new authentication request. callback is called whenever something
   happens for the request. */
struct auth_client_request *
auth_client_request_new(struct auth_client *client,
			const struct auth_request_info *request_info,
			auth_request_callback_t *callback, void *context);
/* Enable channel binding support for this request. */
void auth_client_request_enable_channel_binding(
	struct auth_client_request *request,
	auth_channel_binding_callback_t *callback, void *context);
/* Continue authentication. Call when
   reply->result == AUTH_CLIENT_REQUEST_CONTINUE */
void auth_client_request_continue(struct auth_client_request *request,
				  const char *data_base64);
/* Abort ongoing authentication request. */
void auth_client_request_abort(struct auth_client_request **request,
			       const char *reason);
/* Return ID of this request. */
unsigned int auth_client_request_get_id(struct auth_client_request *request);
/* Return the PID of the server that handled this request. */
unsigned int
auth_client_request_get_server_pid(struct auth_client_request *request);
/* Return cookie of the server that handled this request. */
const char *auth_client_request_get_cookie(struct auth_client_request *request);

/* Tell auth process to drop specified request from memory */
void auth_client_send_cancel(struct auth_client *client, unsigned int id);

#endif
