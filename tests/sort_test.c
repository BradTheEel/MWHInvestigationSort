#include <assert.h>
#include <stdio.h>
#include "../src/sort.h"

static unsigned compare_calls;

static bool native_less(const SortRow *left, const SortRow *right,
                        int mode, int arg, int extra)
{
    assert(mode == 25 && arg == 19 && extra == 7);
    ++compare_calls;
    int left_key = *(const int *)left->record;
    int right_key = *(const int *)right->record;
    if (left_key == right_key)
        return left->order < right->order;
    return left_key == arg;
}

int main(void)
{
    int keys[SORT_CAPACITY];
    SortRow rows[SORT_CAPACITY];
    for (int i = 0; i < SORT_CAPACITY; ++i) {
        keys[i] = i % 2 ? 19 : 0;
        rows[i] = (SortRow){&keys[i], i, i, 100 + i};
    }

    SortChoice choice = {25, 19, 7};
    assert(sort_choice_valid(choice));
    assert(!sort_choice_valid((SortChoice){19, 0, 0}));
    assert(sort_choice_valid((SortChoice){20, 0, 0}));
    sort_rows(rows, SORT_CAPACITY, choice, native_less);
    assert(compare_calls > 0);
    assert(rows[0].registered_index == 1);
    assert(rows[36].registered_index == 73);
    assert(rows[37].registered_index == 0);
    assert(rows[74].registered_index == 74);
    for (int i = 0; i < SORT_CAPACITY; ++i) {
        int identity = rows[i].registered_index;
        assert(rows[i].record == &keys[identity]);
    }

    int32_t quest_ids[SORT_PAGE_SIZE];
    int32_t registered[SORT_PAGE_SIZE];
    assert(sort_page(rows, SORT_CAPACITY, 6, quest_ids, registered));
    assert(quest_ids[0] == 158 && registered[0] == 58);
    assert(quest_ids[8] == 174 && registered[8] == 74);
    assert(quest_ids[9] == -1 && registered[10] == -1);
    assert(!sort_page(rows, SORT_CAPACITY, 7, quest_ids, registered));

    SortRow small[] = {{&keys[0], 4, 0, 55001}, {&keys[1], 12, 1, 55001}};
    assert(sort_page(small, 2, 0, quest_ids, registered));
    assert(quest_ids[0] == quest_ids[1] && registered[0] == 4 && registered[1] == 12);
    assert(quest_ids[2] == -1 && registered[2] == -1);

    unsigned previous_calls = compare_calls;
    sort_rows(small, 2, (SortChoice){20, 0, 0}, native_less);
    sort_rows(small, 0, choice, native_less);
    assert(compare_calls == previous_calls && small[0].registered_index == 4);
    puts("sort policy passed");
}
