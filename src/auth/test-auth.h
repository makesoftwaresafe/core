/* Copyright (c) 2017-2018 Dovecot authors, see the included COPYING file */

#ifndef TEST_AUTH_H
#define TEST_AUTH_H 1

#define AUTH_REQUEST_FIELDS_CONST

#include "lib.h"
#include "test-common.h"

struct auth_passdb;

void test_auth_request_var_expand(void);
void test_auth_request_fields(void);
void test_db_dict_parse_cache_key(void);
void test_username_filter(void);
void test_db_lua(void);
struct auth_passdb *passdb_mock(void);
void passdb_mock_mod_init(void);
void passdb_mock_mod_deinit(void);

void test_auth_init(void);
void test_auth_deinit(void);

#endif

