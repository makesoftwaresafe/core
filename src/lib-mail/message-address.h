#ifndef MESSAGE_ADDRESS_H
#define MESSAGE_ADDRESS_H

struct smtp_address;

enum message_address_parse_flags {
	/* If enabled, missing mailbox and domain are set to MISSING_MAILBOX
	   and MISSING_DOMAIN strings. Otherwise they're set to "". */
	MESSAGE_ADDRESS_PARSE_FLAG_FILL_MISSING = BIT(0),
	/* Require local-part to strictly adhere to RFC5322 when parsing dots.
	   For example ".user", "us..ser" and "user." will be invalid. This
	   isn't enabled by default, because these kind of invalid addresses
	   are commonly used in Japan. */
	MESSAGE_ADDRESS_PARSE_FLAG_STRICT_DOTS = BIT(1),
};

/* group: ... ; will be stored like:
   {name = NULL, NULL, "group", NULL}, ..., {NULL, NULL, NULL, NULL}
*/
struct message_address {
	struct message_address *prev, *next;

	/* display-name */
	const char *name;
	/* route string contains the @ prefix */
	const char *route;
	/* local-part */
	const char *mailbox;
	const char *domain;
	/* there were errors when parsing this address */
	bool invalid_syntax;
};

struct message_address_list {
	struct message_address *head, *tail;
};

/* Parse message addresses from given data. Note that giving an empty string
   will return NULL since there are no addresses. */
struct message_address *
message_address_parse(pool_t pool, const unsigned char *data, size_t size,
		      unsigned int max_addresses,
		      enum message_address_parse_flags flags);
/* Same as message_address_parse(), but return message_address_list containing
   both the first and the last address in the linked list. */
void message_address_parse_full(pool_t pool, const unsigned char *data,
				size_t size, unsigned int max_addresses,
				enum message_address_parse_flags flags,
				struct message_address_list *list_r);

/* Parse RFC 5322 "path" (Return-Path header) from given data. Returns -1 if
   the path is invalid and 0 otherwise.
 */
int message_address_parse_path(pool_t pool, const unsigned char *data,
			       size_t size, struct message_address **addr_r);

void message_address_init(struct message_address *addr,
	const char *name, const char *mailbox, const char *domain)
	ATTR_NULL(1);
void message_address_init_from_smtp(struct message_address *addr,
	const char *name, const struct smtp_address *smtp_addr)
	ATTR_NULL(1);

void message_address_write(string_t *str, const struct message_address *addr);
const char *message_address_to_string(const struct message_address *addr);
const char *message_address_first_to_string(const struct message_address *addr);

/* Returns TRUE if header is known to be an address */
bool message_header_is_address(const char *hdr_name);

#endif
