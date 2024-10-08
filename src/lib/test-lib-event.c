/* Copyright (c) 2021 Dovecot authors, see the included COPYING file */

#include "test-lib.h"
#include "array.h"

static void test_event_fields(void)
{
	test_begin("event fields");
	struct event *event = event_create(NULL);
	struct event_field *field;
	struct ip_addr ip = { .family = 0 };

	event_add_str(event, "key", NULL);
	test_assert(event_find_field_nonrecursive(event, "key") == NULL);
	event_add_ip(event, "key", &ip);
	test_assert(event_find_field_nonrecursive(event, "key") == NULL);

	event_add_str(event, "key", "value1");
	field = event_find_field_nonrecursive(event, "key");
	test_assert(field != NULL && field->value_type == EVENT_FIELD_VALUE_TYPE_STR &&
		    strcmp(field->value.str, "value1") == 0);

	event_add_str(event, "key", NULL);
	field = event_find_field_nonrecursive(event, "key");
	test_assert(field != NULL && field->value_type == EVENT_FIELD_VALUE_TYPE_STR &&
		    field->value.str[0] == '\0');

	event_add_int(event, "key", -1234);
	field = event_find_field_nonrecursive(event, "key");
	test_assert(field != NULL && field->value_type == EVENT_FIELD_VALUE_TYPE_INTMAX &&
		    field->value.intmax == -1234);
	test_assert_strcmp(event_find_field_recursive_str(event, "key"), "-1234");

	struct timeval tv = { .tv_sec = 123456789, .tv_usec = 654321 };
	event_add_timeval(event, "key", &tv);
	field = event_find_field_nonrecursive(event, "key");
	test_assert(field != NULL && field->value_type == EVENT_FIELD_VALUE_TYPE_TIMEVAL &&
		    field->value.timeval.tv_sec == tv.tv_sec &&
		    field->value.timeval.tv_usec == tv.tv_usec);

	if (net_addr2ip("1002::4301:6", &ip) < 0)
		i_unreached();
	event_add_ip(event, "key", &ip);
	field = event_find_field_nonrecursive(event, "key");
	test_assert(field != NULL && field->value_type == EVENT_FIELD_VALUE_TYPE_IP &&
		    net_ip_compare(&field->value.ip, &ip));

	ip.family = 0;
	event_add_ip(event, "key", &ip);
	field = event_find_field_nonrecursive(event, "key");
	test_assert(field != NULL && field->value_type == EVENT_FIELD_VALUE_TYPE_STR &&
		    field->value.str[0] == '\0');

	event_strlist_append(event, "key", "strlist1");
	field = event_find_field_nonrecursive(event, "key");
	test_assert(field != NULL && field->value_type == EVENT_FIELD_VALUE_TYPE_STRLIST &&
		    strcmp(array_idx_elem(&field->value.strlist, 0), "strlist1") == 0);

	event_unref(&event);
	test_end();
}

static void test_event_strlist(void)
{
	test_begin("event strlist");
	struct event *e1 = event_create(NULL);
	event_strlist_append(e1, "key", "s1");
	event_strlist_append(e1, "key", "s2");
	struct event *e2 = event_create(e1);
	event_strlist_append(e2, "key", "s3");
	event_strlist_append(e2, "key", "s2");

	test_assert_strcmp(event_find_field_recursive_str(e1, "key"), "s1,s2");
	test_assert_strcmp(event_find_field_recursive_str(e2, "key"), "s3,s2,s1");

	const char *new_strlist[] = { "new1", "new2", "new2", "s2" };
	event_strlist_replace(e2, "key", new_strlist, N_ELEMENTS(new_strlist));
	test_assert_strcmp(event_find_field_recursive_str(e2, "key"), "new1,new2,s2,s1");

	struct event *e3 = event_create(NULL);
	event_strlist_copy_recursive(e3, e2, "key");
	test_assert_strcmp(event_find_field_recursive_str(e3, "key"), "new1,new2,s2,s1");
	event_unref(&e3);

	event_unref(&e1);
	event_unref(&e2);
	test_end();
}

static void test_lib_event_reason_code(void)
{
	test_begin("event reason codes");
	test_assert_strcmp(event_reason_code("foo", "bar"), "foo:bar");
	test_assert_strcmp(event_reason_code("foo", "B A-r"), "foo:b_a_r");
	test_assert_strcmp(event_reason_code_prefix("foo", "x", "bar"), "foo:xbar");
	test_assert_strcmp(event_reason_code_prefix("foo", "", "bar"), "foo:bar");
	test_end();
}

void test_lib_event(void)
{
	test_event_fields();
	test_event_strlist();
	test_lib_event_reason_code();
}

enum fatal_test_state fatal_lib_event(unsigned int stage)
{
	switch (stage) {
	case 0:
		test_begin("event reason codes - asserts");
		/* module: uppercase */
		test_expect_fatal_string("Invalid module");
		(void)event_reason_code("FOO", "bar");
		return FATAL_TEST_FAILURE;
	case 1:
		/* module: space */
		test_expect_fatal_string("Invalid module");
		(void)event_reason_code("f oo", "bar");
		return FATAL_TEST_FAILURE;
	case 2:
		/* module: - */
		test_expect_fatal_string("Invalid module");
		(void)event_reason_code("f-oo", "bar");
		return FATAL_TEST_FAILURE;
	case 3:
		/* module: empty */
		test_expect_fatal_string("module[0] != '\\0'");
		(void)event_reason_code("", "bar");
		return FATAL_TEST_FAILURE;
	case 4:
		/* name_prefix: uppercase */
		test_expect_fatal_string("Invalid name_prefix");
		(void)event_reason_code_prefix("module", "FOO", "bar");
		return FATAL_TEST_FAILURE;
	case 5:
		/* name_prefix: space */
		test_expect_fatal_string("Invalid name_prefix");
		(void)event_reason_code_prefix("module", "f oo", "bar");
		return FATAL_TEST_FAILURE;
	case 6:
		/* name_prefix: - */
		test_expect_fatal_string("Invalid name_prefix");
		(void)event_reason_code_prefix("module", "f-oo", "bar");
		return FATAL_TEST_FAILURE;
	case 7:
		/* name: empty */
		test_expect_fatal_string("(name[0] != '\\0')");
		(void)event_reason_code("foo:", "");
		return FATAL_TEST_FAILURE;
	default:
		test_end();
		return FATAL_TEST_FINISHED;
	}
}
