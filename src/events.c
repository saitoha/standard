#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "events.h"
#include "hash-table.h"


#define EVENT_HASH_BITS 8


static hash_table_t *event_handlers;


static unsigned eh_hash(const void *ehptr)
{
    const event_t *eh = ehptr;
    return eh->type + eh->code;
}

static bool eh_compare(const void *entry, const void *compare)
{
    const event_t *ee = entry, *ec = compare;
    return (ee->code == ec->code) && (ee->type == ec->type);
}


void init_events(void)
{
    event_handlers = create_hash_table(EVENT_HASH_BITS, &eh_hash, &eh_compare, &free);
}


bool trigger_event(event_t event)
{
    event_handler_t *handler = hash_table_lookup(event_handlers, &event);
    return handler && handler->handler(&event, handler->info);
}


void register_event_handler(event_t event, bool (*handler)(const event_t *event, void *info), void *info)
{
    event_handler_t *existing = hash_table_lookup(event_handlers, &event);

    if (existing)
    {
        existing->handler = handler;
        existing->info = info;
    }
    else
    {
        event_handler_t *neh = malloc(sizeof(*neh));
        memcpy(&neh->event, &event, sizeof(event));
        neh->handler = handler;
        neh->info = info;

        hash_table_insert(event_handlers, neh);
    }
}
