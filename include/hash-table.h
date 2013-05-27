#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stdbool.h>


typedef struct hash_table_entry
{
    struct hash_table_entry *next;
    void *data;
} hash_table_entry_t;

typedef struct hash_table
{
    hash_table_entry_t **entries;
    int bits;

    unsigned (*hash)(const void *data);
    bool (*match)(const void *entry, const void *compare);
    void (*free)(void *data);
} hash_table_t;


hash_table_t *create_hash_table(int bits, unsigned (*hash)(const void *data), bool (*match)(const void *entry, const void *compare), void (*free)(void *data));
void hash_table_destroy(hash_table_t *ht);

void *hash_table_lookup(hash_table_t *ht, const void *compare);
void hash_table_insert(hash_table_t *ht, void *data);

#endif
