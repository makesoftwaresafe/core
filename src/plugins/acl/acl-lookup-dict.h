#ifndef ACL_LOOKUP_DICT_H
#define ACL_LOOKUP_DICT_H

struct acl_lookup_dict;

int acl_lookup_dict_init(struct mail_user *user, struct acl_lookup_dict **dict_r,
			 const char **error_r);
void acl_lookup_dict_deinit(struct acl_lookup_dict **dict);

bool acl_lookup_dict_is_enabled(struct acl_lookup_dict *dict);

int acl_lookup_dict_rebuild(struct acl_lookup_dict *dict);

struct acl_lookup_dict_iter *
acl_lookup_dict_iterate_visible_init(struct acl_lookup_dict *dict,
				     const ARRAY_TYPE(const_string) *groups);
const char *
acl_lookup_dict_iterate_visible_next(struct acl_lookup_dict_iter *iter);
int acl_lookup_dict_iterate_visible_deinit(struct acl_lookup_dict_iter **iter);

#endif
