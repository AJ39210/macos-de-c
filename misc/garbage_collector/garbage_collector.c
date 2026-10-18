#define _DEFAULT_SOURCE
#include "garbage_collector.h"

#include <malloc.h>

static gboolean collect_released_heap(gpointer user_data) {
    (void)user_data;
    if (malloc_trim(0)) {
        g_debug("Garbage collector returned unused heap pages to the system.");
    }
    return G_SOURCE_CONTINUE;
}

void garbage_collector_start(void) {
    g_timeout_add_seconds(30, collect_released_heap, NULL);
}
