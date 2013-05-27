#include <stdlib.h>

#include "hash-table.h"


hash_table_t *create_hash_table(int bits, unsigned (*hash)(const void *data), bool (*match)(const void *entry, const void *compare), void (*free)(void *data))
{
    hash_table_t *ht = malloc(sizeof(*ht));

    ht->bits = bits;
    ht->entries = calloc(1 << bits, sizeof(ht->entries[0]));

    ht->hash = hash;
    ht->match = match;
    ht->free = free;

    return ht;
}


void hash_table_destroy(hash_table_t *ht)
{
    for (int i = 0; i < 1 << ht->bits; i++)
    {
        hash_table_entry_t *next = ht->entries[i];

        while (next)
        {
            hash_table_entry_t *e = next;
            next = e->next;

            ht->free(e->data);
            free(e);
        }
    }

    free(ht->entries);
    free(ht);
}


void *hash_table_lookup(hash_table_t *ht, const void *compare)
{
    int i = ht->hash(compare) & ((1 << ht->bits) - 1);

    for (hash_table_entry_t *e = ht->entries[i]; e != NULL; e = e->next)
        if (ht->match(e->data, compare))
            return e->data;

    return NULL;
}


void hash_table_insert(hash_table_t *ht, void *data)
{
    int i = ht->hash(data) & ((1 << ht->bits) - 1);

    hash_table_entry_t *ne = malloc(sizeof(*ne));
    ne->next = NULL;
    ne->data = data;

    hash_table_entry_t **ep;
    for (ep = &ht->entries[i]; *ep != NULL; ep = &(*ep)->next);

    *ep = ne;
}
