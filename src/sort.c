#include "sort.h"
#include <stddef.h>

_Static_assert(offsetof(SortRow, order) == 0xc, "native comparator layout");

bool sort_choice_valid(SortChoice choice)
{
    return choice.mode >= SORT_RESET && choice.mode <= 27;
}

void sort_rows(SortRow *rows, unsigned count, SortChoice choice, SortLess less)
{
    if (!sort_choice_valid(choice) || choice.mode == SORT_RESET || count > SORT_CAPACITY)
        return;
    for (unsigned i = 1; i < count; ++i) {
        SortRow row = rows[i];
        unsigned position = i;
        while (position && less(&row, &rows[position - 1], choice.mode, choice.arg, choice.extra)) {
            rows[position] = rows[position - 1];
            --position;
        }
        rows[position] = row;
    }
}

bool sort_page(const SortRow *rows, unsigned count, unsigned page,
               int32_t *quest_ids, int32_t *registered_indices)
{
    if (!count || count > SORT_CAPACITY || page > (count - 1) / SORT_PAGE_SIZE)
        return false;
    unsigned first = page * SORT_PAGE_SIZE;
    for (unsigned i = 0; i < SORT_PAGE_SIZE; ++i) {
        unsigned index = first + i;
        quest_ids[i] = index < count ? rows[index].quest_id : -1;
        registered_indices[i] = index < count ? rows[index].registered_index : -1;
    }
    return true;
}
