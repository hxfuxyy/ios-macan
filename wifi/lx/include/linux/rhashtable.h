#ifndef _LX_LINUX_RHASHTABLE_H
#define _LX_LINUX_RHASHTABLE_H
#include <linux/types.h>
struct rhash_head {
 struct rhash_head *next;
};
struct rhashtable_params {
 uint16_t head_offset;
 uint16_t key_len;
 uint16_t key_offset;
 uint16_t nelem_hint;
 bool automatic_shrinking;
};
struct rhashtable {
 struct rhash_head *chains[64];
};
int rhashtable_init(struct rhashtable *table, const struct rhashtable_params *params);
void rhashtable_destroy(struct rhashtable *table);
void *rhashtable_lookup_fast(struct rhashtable *table, const void *key, const struct rhashtable_params params);
void *rhashtable_lookup_get_insert_fast(struct rhashtable *table, struct rhash_head *head,
     const struct rhashtable_params params);
int rhashtable_remove_fast(struct rhashtable *table, struct rhash_head *head, const struct rhashtable_params params);
#endif
