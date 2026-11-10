#ifndef INVESTIGATION_SORT_H
#define INVESTIGATION_SORT_H

#include <stdbool.h>
#include <stdint.h>

enum { SORT_CAPACITY = 75, SORT_PAGE_SIZE = 11, SORT_RESET = 20 };

/* The stock comparator reads the record at +0 and tie order at +0xC. */
typedef struct {
    const void *record;
    int32_t registered_index;
    int32_t order;
    int32_t quest_id;
} SortRow;

typedef struct {
    int32_t mode;
    int32_t arg;
    int32_t extra;
} SortChoice;

typedef bool (*SortLess)(const SortRow *, const SortRow *, int, int, int);

bool sort_choice_valid(SortChoice choice);
void sort_rows(SortRow *rows, unsigned count, SortChoice choice, SortLess less);
bool sort_page(const SortRow *rows, unsigned count, unsigned page,
               int32_t *quest_ids, int32_t *registered_indices);

#endif
