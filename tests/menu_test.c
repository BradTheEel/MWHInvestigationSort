#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static unsigned memory_queries;

static SIZE_T counted_query(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T size)
{
    ++memory_queries;
    return VirtualQuery(address, info, size);
}

#define VirtualQuery counted_query
#include "../src/investigation_sort.c"
#undef VirtualQuery

static Byte *gui, *owner, *button, *page, *filter, *popup, *save, *save_manager, *keyboard;
static QueueEntry queued;
static unsigned open_calls, page_refreshes, result_calls;
static uint32_t pad_pressed;

static Byte *allocate(size_t size)
{
    Byte *memory = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    assert(memory);
    return memory;
}

static void thunk(unsigned rva, uintptr_t target)
{
    Byte code[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    memcpy(code + 2, &target, sizeof(target));
    memcpy((void *)(image + rva), code, sizeof(code));
}

static Byte *fake_get_save(Byte *m) { assert(m == save_manager); return save; }
static Byte *fake_get_popup(Byte *m) { assert(m == gui); return popup; }
static int fake_rank(const void *record, int unused) { assert(!unused); return ((const Byte *)record)[0x41]; }
static int fake_difficulty(const void *record) { (void)record; return 1; }
static void no_op(Byte *p) { (void)p; }
static void fake_close(Byte *p, uint8_t animate) { assert(p == popup && animate == 1); U32(p, GUI_FLAGS) = 0; }

static void fake_result(const InlineCallable *callable, Byte **parent, Byte **result)
{
    assert(callable->argument == (uintptr_t)*parent && *result);
    ++result_calls;
}

static void manager_frame(void)
{
    Byte *selection = PTR(gui, GUI_SELECT);
    gui[0x146f6] = selection && (selection[0xc] & 0xe) &&
                   (U32(selection, GUI_FLAGS) & 0x80000011u);
    process_filter_frame(filter, popup + 0x28f0);
}

static bool fake_button_pressed(Byte *p, unsigned action, Byte *widget)
{
    assert(p == owner && !widget);
    if (!p[0x22c2] || (U32(p, GUI_INPUT_FLAGS) & 0x400u)) return false;
    if (action == 0x18) return (pad_pressed & 4u) != 0;
    assert(!"unexpected GUI action");
    return false;
}

static void input_frame(uint32_t pad, uint32_t keys)
{
    U32(owner, GUI_INPUT_FLAGS) &= ~0x40u; /* The stock input pass clears this bit each frame. */
    pad_pressed = pad;
    U32(keyboard, 0x178) = keys;
    manager_frame();
    pad_pressed = U32(keyboard, 0x178) = 0;
}

static void press_sort(void)
{
    input_frame(0, 1u << VK_TAB);
}

static void expect_idle_without_queries(void)
{
    memory_queries = 0;
    for (unsigned frame = 0; frame < 240; ++frame) input_frame(4u, 1u << VK_TAB);
    if (memory_queries) {
        fprintf(stderr, "Closed quest selection: %u memory queries in 240 frames\n", memory_queries);
        exit(1);
    }
    assert(!menu.owner && menu.phase == IDLE && !U64(gui, GUI_QUEUE_COUNT));
}

static bool fake_compare(const SortRow *a, const SortRow *b, int mode, int arg, int extra)
{
    assert((mode == 22 || mode == 23) && arg == -1 && extra == 0);
    if (mode == 23) return a->order < b->order;
    Byte first = ((const Byte *)a->record)[0x45];
    Byte second = ((const Byte *)b->record)[0x45];
    return first == second ? a->order < b->order : first > second;
}

static void fake_bind(QueueEntry *entry, Byte *parent, Byte *target)
{
    assert(parent == owner && target == popup && !entry->target);
    entry->owner = entry->owner_ref = parent;
    entry->popup_ref = target;
    entry->bound = 1;
}

static void fake_push(Byte *queue, QueueEntry *entry)
{
    assert(queue == gui + GUI_QUEUE && entry->bound && entry->target == &entry->callable);
    assert(!U64(gui, GUI_QUEUE_COUNT));
    ++U64(gui, GUI_QUEUE_COUNT);
    queued = *entry;
    typedef InlineCallable *(*Clone)(const InlineCallable *, InlineCallable *);
    Clone clone = (Clone)ADDRESS((Byte *)entry->callable.vtable, 0);
    assert(clone(entry->target, &queued.callable) == &queued.callable);
    queued.target = &queued.callable;
}

static void fake_open(const InlineCallable *callable, Byte **parent, Byte **target)
{
    assert(*parent == owner && *target == popup && I32(filter, FILTER_MODE) == 8);
    const PopupRows *input = (const PopupRows *)callable->argument;
    for (int i = 0; i < 13; ++i) assert(input->rows[i].slot == i * 2);
    for (int i = 13; i < RECORD_CAPACITY; ++i) assert(input->rows[i].slot == -1);
    U32(popup, GUI_FLAGS) = 0x10;
    ++open_calls;
}

static void *fake_weak(void) { return NULL; }
static void fake_release(void *weak, Byte **ref) { assert(!weak && *ref); *ref = NULL; }
static void fake_reset(Byte *p)
{
    assert(p == filter);
    I32(p, FILTER_MODE) = 11;
    I32(p, FILTER_CRITERION) = 29;
    p[FILTER_CHANGED] = 0;
}

static int fake_build(Byte *p, int category, int stars, unsigned number)
{
    assert(p == owner && category == 2 && stars == 1 && number <= 6);
    for (unsigned row = 0; row < SORT_PAGE_SIZE; ++row) {
        unsigned ordinal = number * SORT_PAGE_SIZE + row;
        I32(owner, SELECT_QUEST_IDS + 4 * row) = ordinal < 13 ? (int)(50000 + ordinal) : -1;
        I32(owner, SELECT_REGISTERED + 4 * row) = ordinal < 13 ? (int)ordinal : -1;
    }
    return 13;
}

static int fake_refresh(Byte *p, unsigned number, uint8_t details)
{
    assert(number == U32(page, 0x240));
    if (details) ++page_refreshes;
    return build_detour(p, 2, 1, number);
}

static void turn_to_page(unsigned number)
{
    U32(page, 0x240) = number;
    assert(fake_refresh(owner, number, 0) == 13); /* The stock page event passes zero for details. */
}

static DWORD WINAPI turn_to_second_page(void *unused)
{
    (void)unused;
    turn_to_page(1);
    return 0;
}

static void consume_queue(void)
{
    assert(U64(gui, GUI_QUEUE_COUNT) == 1 && queued.owner == owner);
    typedef void (*Invoke)(const InlineCallable *, Byte **, Byte **);
    Invoke invoke = (Invoke)ADDRESS((Byte *)queued.callable.vtable, 0x10);
    invoke(queued.target, &queued.owner, &queued.popup_ref);
    U64(gui, GUI_QUEUE_COUNT) = 0;
}

static void confirm(int mode)
{
    I32(filter, FILTER_CRITERION) = mode;
    I32(filter, 0xce40) = -1;
    filter[FILTER_CHANGED] = 1;
    U32(popup, GUI_FLAGS) = 0;
    manager_frame();
    filter[FILTER_CHANGED] = 0;
}

int main(void)
{
    image = (uintptr_t)allocate(IMAGE_SIZE);
    gui = allocate(0x16000);
    owner = allocate(0x126d0);
    button = allocate(0x400);
    page = allocate(0x400);
    popup = allocate(0x3d50);
    filter = allocate(0xeea0);
    save = allocate(0x26cc00);
    save_manager = allocate(0xb0);
    keyboard = allocate(0x2c30);
    InitializeCriticalSection((CRITICAL_SECTION *)(save + 8));

    PTR((Byte *)image, GUI_GLOBAL) = gui;
    PTR((Byte *)image, SAVE_GLOBAL) = save_manager;
    PTR((Byte *)image, 0x51c53b0) = keyboard;
    PTR(save_manager, 0xa8) = save;
    PTR(gui, GUI_SELECT) = owner;
    PTR(gui, GUI_POPUP) = popup;
    PTR(gui, GUI_SEARCH_FILTER) = filter;
    ADDRESS(owner, 0) = image + SELECT_VTABLE;
    owner[0xc] = 2;
    ADDRESS(popup, 0) = image + POPUP_VTABLE;
    popup[0xc] = 2;
    PTR(owner, SELECT_RESEARCH_LIST) = button;
    PTR(owner, SELECT_RESEARCH_PAGE) = page;
    I32(owner, 0x1d00) = I32(owner, 0x1d04) = 2;
    I32(owner, 0x2920) = 2;
    I32(owner, 0x290c) = 3;
    I32(owner, 0x28f4) = 2;
    owner[0x22c2] = 1;
    U32(owner, GUI_FLAGS) = 1;
    PTR(button, 0x80) = PTR(button, 0x120) = owner;
    PTR(button, 0x1e8) = allocate(SORT_PAGE_SIZE * sizeof(void *));
    U32(button, 0x1d8) = SORT_PAGE_SIZE;
    ADDRESS(button, 0) = image + 0x30cb560;
    ADDRESS((Byte *)image + 0x30cb560, 0x118) = image + 0xb76640;
    I32(button, 0xe4) = 0x1c;
    fake_reset(filter);
    for (int i = 0; i < 14; ++i) {
        Byte *record = save + RECORD_BASE + i * 2 * RECORD_SIZE;
        I32(record, 8) = 50000 + i;
        record[0xc] = 1;
        record[0x41] = i == 13 ? 12 : 9;
        record[0x45] = i;
    }

    thunk(0x1b8dbb0, (uintptr_t)fake_get_save);
    thunk(0x10834a0, (uintptr_t)fake_get_popup);
    thunk(0xf1a750, (uintptr_t)fake_rank);
    thunk(0xf19ea0, (uintptr_t)fake_difficulty);
    thunk(0x10dead0, (uintptr_t)fake_compare);
    thunk(0x1ae9150, (uintptr_t)fake_bind);
    thunk(0x509dc0, (uintptr_t)fake_push);
    thunk(0x10dee60, (uintptr_t)fake_open);
    thunk(0x2229230, (uintptr_t)fake_weak);
    thunk(0x22291d0, (uintptr_t)fake_release);
    thunk(0x1077360, (uintptr_t)no_op);
    thunk(0xb15760, (uintptr_t)fake_reset);
    thunk(0x1fb7cc0, (uintptr_t)fake_close);
    thunk(0x1fb6960, (uintptr_t)fake_button_pressed);
    thunk(0x1150500, (uintptr_t)fake_refresh);
    assert(FlushInstructionCache(GetCurrentProcess(), (void *)image, IMAGE_SIZE));
    original_close = no_op;
    original_result_single = original_result_multiple = fake_result;
    original_build = fake_build;
    popup_callable_vtable[0] = (uintptr_t)clone_callable;
    popup_callable_vtable[1] = (uintptr_t)clone_callable;
    popup_callable_vtable[2] = (uintptr_t)invoke_popup;

    owner[0xc] = 0;
    expect_idle_without_queries(); /* The manager retains a reference to the hidden widget. */
    PTR(gui, GUI_SELECT) = NULL;
    expect_idle_without_queries();
    PTR(gui, GUI_SELECT) = owner;
    owner[0xc] = 2;

    I32(owner, 0x2918) = 1;
    press_sort();
    assert(menu.phase == IDLE && U64(gui, GUI_QUEUE_COUNT) == 0);
    I32(owner, 0x2918) = 0;
    button[0x173] = 1;
    press_sort();
    assert(menu.phase == IDLE && U64(gui, GUI_QUEUE_COUNT) == 0);
    button[0x173] = 0;

    input_frame(0x80u, 0); /* D-pad left stays with page navigation. */
    assert(menu.phase == IDLE && !U64(gui, GUI_QUEUE_COUNT));
    input_frame(0, 1u << VK_RBUTTON);
    assert(menu.phase == IDLE && !U64(gui, GUI_QUEUE_COUNT));
    owner[0x22c2] = 0;
    input_frame(4u, 1u << VK_TAB);
    assert(menu.phase == IDLE && !U64(gui, GUI_QUEUE_COUNT));
    owner[0x22c2] = 1;
    U32(keyboard, 0x138) = 1u << VK_TAB; /* A held Tab without a new press. */
    input_frame(0, 0);
    assert(menu.phase == IDLE && !U64(gui, GUI_QUEUE_COUNT));
    U32(keyboard, 0x138) = 0;

    PTR(gui, GUI_SELECT) = NULL;
    manager_frame();
    assert(!menu.owner);
    PTR(gui, GUI_SELECT) = owner;
    press_sort();
    assert(menu.phase == QUEUED && U64(gui, GUI_QUEUE_COUNT) == 1);
    input_frame(4u, 1u << VK_TAB);
    assert(U64(gui, GUI_QUEUE_COUNT) == 1);
    assert(I32(button, 0xe4) == 0x1c && !ADDRESS(button, 0x138));
    consume_queue();
    assert(menu.phase == OPEN && open_calls == 1);
    Byte *origin = popup;
    Byte *result = allocate(0x29f0);
    InlineCallable result_call = {0, (uintptr_t)origin};
    result_single_detour(&result_call, &origin, &result);
    result_multiple_detour(&result_call, &origin, &result);
    assert(result_calls == 0);
    origin = owner;
    result_call.argument = (uintptr_t)origin;
    result_single_detour(&result_call, &origin, &result);
    result_multiple_detour(&result_call, &origin, &result);
    assert(result_calls == 2);
    U32(popup, GUI_FLAGS) = 0;
    manager_frame();
    assert(!page_refreshes && I32(filter, FILTER_MODE) == 11 && menu.phase == IDLE);

    PTR(gui, GUI_SELECT) = NULL;
    manager_frame();
    assert(!menu.owner);
    PTR(gui, GUI_SELECT) = owner;
    input_frame(4u, 0); /* Right stick click independently opens the window. */
    consume_queue();
    U32(page, 0x240) = 1;
    confirm(22);
    assert(page_refreshes == 1 && menu.phase == IDLE && I32(filter, FILTER_MODE) == 11);
    assert(I32(owner, SELECT_QUEST_IDS) == 50001 && I32(owner, SELECT_REGISTERED) == 1);
    assert(I32(owner, SELECT_QUEST_IDS + 4) == 50000 && I32(owner, SELECT_REGISTERED + 4) == 0);
    assert(I32(owner, SELECT_REGISTERED + 8) == -1);
    turn_to_page(0);
    assert(I32(owner, SELECT_QUEST_IDS) == 50012 && I32(owner, SELECT_REGISTERED) == 12);

    U32(owner, GUI_FLAGS) = 0x20;
    manager_frame();
    assert(menu.owner == owner && applied.owner == owner);
    U32(owner, GUI_FLAGS) = 1;
    manager_frame();
    HANDLE page_worker = CreateThread(NULL, 0, turn_to_second_page, NULL, 0, NULL);
    assert(page_worker && WaitForSingleObject(page_worker, 5000) == WAIT_OBJECT_0);
    assert(CloseHandle(page_worker));
    assert(I32(owner, SELECT_REGISTERED) == 1);

    save[RECORD_BASE + 0x45] = 14;
    turn_to_page(0);
    assert(I32(owner, SELECT_QUEST_IDS) == 50000 && I32(owner, SELECT_REGISTERED) == 0);
    assert(I32(owner, SELECT_QUEST_IDS + 4) == 50012 && I32(owner, SELECT_REGISTERED + 4) == 12);
    save[RECORD_BASE + 0x45] = 0;
    turn_to_page(1);
    press_sort();
    consume_queue();
    confirm(23);
    assert(page_refreshes == 2 && I32(owner, SELECT_REGISTERED) == 1);
    press_sort();
    consume_queue();
    confirm(20);
    assert(page_refreshes == 3 && !applied.snapshot.count);
    assert(I32(owner, SELECT_QUEST_IDS) == 50011 && I32(owner, SELECT_REGISTERED) == 11);

    press_sort();
    I32(filter, FILTER_MODE) = 8; /* Another menu acquired the filter before this queued request ran. */
    consume_queue();
    assert(open_calls == 4 && menu.phase == IDLE && I32(button, 0xe4) == 0x1c);
    assert(!ADDRESS(button, 0x138));
    assert(I32(filter, FILTER_MODE) == 8);

    fake_reset(filter);
    press_sort();
    consume_queue();
    assert(menu.phase == OPEN && open_calls == 5);
    confirm(22);
    assert(I32(owner, SELECT_REGISTERED) == 1);
    press_sort();
    consume_queue();
    assert(menu.phase == OPEN);
    close_detour(owner);
    PTR(gui, GUI_SELECT) = NULL;
    manager_frame();
    assert(!menu.owner && menu.phase == IDLE && I32(filter, FILTER_MODE) == 11);
    expect_idle_without_queries();
    turn_to_page(1);
    assert(I32(owner, SELECT_REGISTERED) == 11);
    puts("menu transaction fixture passed");
}
