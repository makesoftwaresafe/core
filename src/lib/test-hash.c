/* Copyright (c) 2014-2018 Dovecot authors, see the included COPYING file */

#include "test-lib.h"
#include "hash.h"


static void test_hash_random_pool(pool_t pool)
{
	const unsigned int keymax = ON_VALGRIND ? 10000 : 100000;
	HASH_TABLE(void *, void *) hash;
	unsigned int *keys;
	unsigned int i, key, keyidx, delidx;

	keys = i_new(unsigned int, keymax); keyidx = 0;
	hash_table_create_direct(&hash, pool, 0);
	for (i = 0; i < keymax; i++) {
		key = (i_rand_limit(keymax)) + 1;
		if (i_rand_limit(5) > 0) {
			if (hash_table_lookup(hash, POINTER_CAST(key)) == NULL) {
				hash_table_insert(hash, POINTER_CAST(key),
						  POINTER_CAST(1));
				keys[keyidx++] = key;
			}
		} else if (keyidx > 0) {
			delidx = i_rand_limit(keyidx);
			hash_table_remove(hash, POINTER_CAST(keys[delidx]));
			memmove(&keys[delidx], &keys[delidx+1],
				(keyidx-delidx-1) * sizeof(*keys));
			keyidx--;
		}
	}
	for (i = 0; i < keyidx; i++)
		hash_table_remove(hash, POINTER_CAST(keys[i]));
	hash_table_destroy(&hash);
	i_free(keys);
}

void test_hash(void)
{
	pool_t pool;

	test_begin("hash table (random)");
	test_hash_random_pool(default_pool);

	pool = pool_alloconly_create("test hash", 1024);
	test_hash_random_pool(pool);
	pool_unref(&pool);
	test_end();
}
