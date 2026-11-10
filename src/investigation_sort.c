#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <string.h>
#include "MinHook.h"
#include "sort.h"

enum {
    IMAGE_SIZE = 0x55d3000,
    SELECT_VTABLE = 0x3225808,
    POPUP_VTABLE = 0x320f720,
    SELECT_CLOSE = 0x1147300,
    BUILD_INVESTIGATIONS = 0x114d3b0,
    GUI_ACTION_PRESSED = 0x1fb6960,
    PUBLISH_FILTER = 0xb165a0,
    PUBLISH_RETURN = 0x1aeb173,
    RESULT_SINGLE = 0x1074b40,
    RESULT_MULTIPLE = 0x1074bf0,
    RESULT_SINGLE_CALLABLE = 0x320fb10,
    RESULT_MULTIPLE_CALLABLE = 0x320fb40,
    STOCK_OPEN_CALLABLE = 0x321cbb8,
    GUI_GLOBAL = 0x51c4640,
    SAVE_GLOBAL = 0x5013950,
    KEYBOARD_GLOBAL = 0x51c53b0,
    GUI_SELECT = 0x14408,
    GUI_POPUP = 0x141b0,
    GUI_POPUP_FOCUS = 0x146f2,
    GUI_SEARCH_FILTER = 0x15540,
    GUI_QUEUE = 0x4f60,
    GUI_QUEUE_COUNT = 0x8f60,
    FILTER_MODE = 0xce68,
    FILTER_CRITERION = 0xce64,
    FILTER_CHANGED = 0xee99,
    SELECT_RESEARCH_LIST = 0x3328,
    SELECT_RESEARCH_PAGE = 0x3358,
    SELECT_QUEST_IDS = 0x33c0,
    SELECT_REGISTERED = 0x33ec,
    GUI_FLAGS = 0x2298,
    GUI_INPUT_FLAGS = 0x229c,
    KEYBOARD_TRIGGER = 0x178,
    RECORD_BASE = 0x105028,
    RECORD_SIZE = 0x48,
    RECORD_CAPACITY = 400,
    RIGHT_STICK_CLICK = 0x18
};

typedef unsigned char Byte;
typedef void (*UiMethod)(Byte *);
typedef int (*BuildList)(Byte *, int, int, unsigned);
typedef Byte *(*GameObjectGetter)(Byte *);
typedef int (*RankKey)(const void *, int);
typedef int (*DifficultyKey)(const void *);
typedef void (*PublishFilter)(Byte *, const Byte *, unsigned, uint8_t);

typedef struct {
    _Alignas(8) Byte record[RECORD_SIZE];
    int32_t slot;
    int32_t registered_index;
} Investigation;

typedef struct {
    Byte *save;
    unsigned count;
    Investigation entries[SORT_CAPACITY];
} Snapshot;

typedef struct {
    Byte *owner;
    int rank;
    uint64_t revision;
    Snapshot snapshot;
    unsigned order[SORT_CAPACITY];
    SortChoice choice;
} AppliedOrder;

typedef struct {
    Byte prefix[0x2d98];
    struct { const void *record; int32_t slot; int32_t order; } rows[RECORD_CAPACITY];
} PopupRows;

typedef struct {
    uintptr_t vtable;
    uintptr_t argument;
} InlineCallable;

typedef void (*QueueCallback)(const InlineCallable *, Byte **, Byte **);

typedef struct {
    InlineCallable callable;
    Byte padding0[0x28];
    InlineCallable *target;
    Byte *owner_ref;
    Byte padding1[0x10];
    Byte *popup_ref;
    Byte padding2[0x10];
    Byte *owner;
    uint8_t bound;
    Byte padding3[7];
} QueueEntry;

_Static_assert(offsetof(PopupRows, rows[0].slot) == 0x2da0, "stock popup input");
_Static_assert(sizeof(QueueEntry) == 0x80 && offsetof(QueueEntry, target) == 0x38 &&
               offsetof(QueueEntry, popup_ref) == 0x58, "stock GUI queue entry");

static uintptr_t image;
static UiMethod original_close;
static BuildList original_build;
static PublishFilter original_publish;
static QueueCallback original_result_single, original_result_multiple;
static uintptr_t popup_callable_vtable[6];
static PopupRows popup_rows;

/* The manager publishes an order and the native page builder can run on a worker. */
static SRWLOCK order_lock = SRWLOCK_INIT;
static AppliedOrder applied;

/* The manager consumes its queue and publishes filter results in one serial pass. */
static struct {
    Byte *owner;
    Byte *popup;
    Byte *filter;
    enum { IDLE, QUEUED, OPEN } phase;
    uintptr_t generation;
    ULONGLONG queued_at;
    int rank;
    Snapshot pending;
    SortChoice confirmed;
    bool has_confirmation;
} menu;

#define GAME(type, rva) ((type)(image + (rva)))
#define PTR(object, offset) (*(Byte **)((object) + (offset)))
#define U32(object, offset) (*(uint32_t *)((object) + (offset)))
#define I32(object, offset) (*(int32_t *)((object) + (offset)))
#define U64(object, offset) (*(uint64_t *)((object) + (offset)))
#define ADDRESS(object, offset) (*(uintptr_t *)((object) + (offset)))

static bool readable(const void *pointer, size_t size)
{
    MEMORY_BASIC_INFORMATION info;
    if (!pointer || !VirtualQuery(pointer, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return size <= info.RegionSize - ((uintptr_t)pointer - (uintptr_t)info.BaseAddress);
}

static Byte *manager(void)
{
    return PTR((Byte *)image, GUI_GLOBAL);
}

static bool posting(Byte *owner)
{
    return readable(owner, 0x34b0) && ADDRESS(owner, 0) == image + SELECT_VTABLE &&
           I32(owner, 0x2918) == 0 && I32(owner, 0x2920) == 2 &&
           (uint32_t)I32(owner, 0x2924) <= 1 &&
           I32(owner, 0x290c) != 1 && I32(owner, 0x290c) != 2;
}

static bool current_menu(Byte *owner, Byte *gui)
{
    return posting(owner) && readable(gui, GUI_SEARCH_FILTER + 8) &&
           PTR(gui, GUI_SELECT) == owner && (owner[0xc] & 0xe);
}

static bool active(Byte *owner)
{
    uint32_t flags = U32(owner, GUI_FLAGS);
    return (flags & 0x80000011u) && !(flags & 0x22u) &&
           I32(owner, 0x1d00) == 2 && I32(owner, 0x1d04) == 2 &&
           I32(owner, 0x28f4) == 2;
}

static bool collect(int rank, Snapshot *out)
{
    Byte *save_manager = PTR((Byte *)image, SAVE_GLOBAL);
    if (!readable(save_manager, 0xb0) || U32(save_manager, 0xa0) >= 3 ||
        !PTR(save_manager, 0xa8)) return false;
    Byte *save = GAME(GameObjectGetter, 0x1b8dbb0)(save_manager);
    if (!readable(save, RECORD_BASE + RECORD_CAPACITY * RECORD_SIZE)) return false;

    out->save = save;
    out->count = 0;
    EnterCriticalSection((CRITICAL_SECTION *)(save + 8));
    unsigned registered = 0;
    for (int slot = 0; slot < RECORD_CAPACITY; ++slot) {
        const Byte *record = save + RECORD_BASE + slot * RECORD_SIZE;
        if (record[0xc] != 1) continue;
        if (registered == SORT_CAPACITY) break;
        Investigation *entry = &out->entries[registered];
        memcpy(entry->record, record, RECORD_SIZE);
        entry->slot = slot;
        entry->registered_index = registered++;
    }
    LeaveCriticalSection((CRITICAL_SECTION *)(save + 8));

    for (unsigned i = 0; i < registered; ++i) {
        Investigation *entry = &out->entries[i];
        if (I32(entry->record, 8) < 0) return false;
        if ((GAME(RankKey, 0xf1a750)(entry->record, 0) >= 11) == rank)
            out->entries[out->count++] = *entry;
    }
    return true;
}

static bool same_snapshot(const Snapshot *a, const Snapshot *b)
{
    return a->save == b->save && a->count == b->count &&
           !memcmp(a->entries, b->entries, a->count * sizeof(a->entries[0]));
}

static void prepare_order(AppliedOrder *sorted, const Snapshot *snapshot, SortChoice choice)
{
    SortRow rows[SORT_CAPACITY];
    const unsigned count = snapshot->count;
    bool previous = same_snapshot(snapshot, &sorted->snapshot);
    for (unsigned i = 0; i < count; ++i) {
        unsigned index = previous ? sorted->order[i] : i;
        const Investigation *entry = &snapshot->entries[index];
        rows[i] = (SortRow){entry->record, entry->registered_index, (int)i, I32(entry->record, 8)};
    }
    if (!previous) {
        /* Stock posting first swaps on difficulty, then swaps on quest rank. */
        for (unsigned pass = 0; pass < 2; ++pass) {
            int keys[SORT_CAPACITY];
            for (unsigned i = 0; i < count; ++i)
                keys[i] = pass ? GAME(RankKey, 0xf1a750)(rows[i].record, 0)
                               : GAME(DifficultyKey, 0xf19ea0)(rows[i].record);
            for (unsigned i = 0; i < count; ++i) {
                for (unsigned j = i + 1; j < count; ++j) {
                    if (pass ? (unsigned)keys[i] >= (unsigned)keys[j] : keys[i] >= keys[j])
                        continue;
                    int key = keys[i];
                    keys[i] = keys[j];
                    keys[j] = key;
                    SortRow row = rows[i];
                    rows[i] = rows[j];
                    rows[j] = row;
                }
            }
        }
        for (unsigned i = 0; i < count; ++i) rows[i].order = i;
    }
    sort_rows(rows, count, choice, GAME(SortLess, 0x10dead0));
    for (unsigned i = 0; i < count; ++i)
        sorted->order[i] = ((const Investigation *)rows[i].record) - snapshot->entries;
    sorted->snapshot = *snapshot;
    sorted->choice = choice;
}

static void clear_order(Byte *owner)
{
    AcquireSRWLockExclusive(&order_lock);
    if (applied.owner == owner) {
        applied.owner = NULL;
        applied.snapshot.count = 0;
        ++applied.revision;
    }
    ReleaseSRWLockExclusive(&order_lock);
}

static bool copy_order(Byte *owner, int rank, AppliedOrder *out)
{
    AcquireSRWLockShared(&order_lock);
    bool present = applied.owner == owner && applied.rank == rank && applied.snapshot.count;
    if (present) *out = applied;
    else out->revision = applied.revision;
    ReleaseSRWLockShared(&order_lock);
    return present;
}

static void apply_choice(Byte *owner, int rank, const Snapshot *snapshot, SortChoice choice)
{
    if (choice.mode == SORT_RESET) {
        clear_order(owner);
        return;
    }
    AppliedOrder sorted = {0};
    copy_order(owner, rank, &sorted);
    prepare_order(&sorted, snapshot, choice);
    sorted.owner = owner;
    sorted.rank = rank;
    AcquireSRWLockExclusive(&order_lock);
    if (applied.revision == sorted.revision) {
        ++sorted.revision;
        applied = sorted;
    }
    ReleaseSRWLockExclusive(&order_lock);
}

static int build_detour(Byte *owner, int category, int stars, unsigned page)
{
    int count = original_build(owner, category, stars, page);
    if (category != 2 || count <= 0 || !current_menu(owner, manager())) return count;

    int rank = I32(owner, 0x2924);
    AppliedOrder sorted;
    if (!copy_order(owner, rank, &sorted)) return count;
    Snapshot live = {0};
    if (!collect(rank, &live) || live.count != (unsigned)count ||
        live.save != sorted.snapshot.save) return count;
    bool changed = !same_snapshot(&sorted.snapshot, &live);
    if (changed) prepare_order(&sorted, &live, sorted.choice);
    SortRow rows[SORT_CAPACITY];
    for (unsigned i = 0; i < live.count; ++i) {
        Investigation *entry = &live.entries[sorted.order[i]];
        rows[i] = (SortRow){entry->record, entry->registered_index, (int)i, I32(entry->record, 8)};
    }
    AcquireSRWLockExclusive(&order_lock);
    if (applied.revision == sorted.revision) {
        if (changed) applied = sorted;
        sort_page(rows, live.count, page, (int32_t *)(owner + SELECT_QUEST_IDS),
                  (int32_t *)(owner + SELECT_REGISTERED));
    }
    ReleaseSRWLockExclusive(&order_lock);
    return count;
}

static InlineCallable *clone_callable(const InlineCallable *source, InlineCallable *destination)
{
    *destination = *source;
    return destination;
}

static bool popup_busy(Byte *gui, Byte *popup)
{
    return gui[GUI_POPUP_FOCUS] ||
           (U32(popup, GUI_FLAGS) & 0x80000011u) || I32(popup, 0x1d00) != 0 ||
           I32(popup, 0x1d04) != 0;
}

static void reset_filter(Byte *gui);

static void invoke_popup(const InlineCallable *callable, Byte **owner, Byte **popup)
{
    if (menu.phase != QUEUED || callable->argument != menu.generation) return;
    Byte *gui = manager();
    if (*owner != menu.owner || *popup != menu.popup ||
        !current_menu(menu.owner, gui) || !active(menu.owner) ||
        PTR(gui, GUI_SEARCH_FILTER) != menu.filter ||
        PTR(gui, GUI_POPUP) != menu.popup || !readable(menu.popup, 0x3d50) ||
        !readable(menu.filter, 0xeea0) || !(menu.popup[0xc] & 0xe) ||
        ADDRESS(menu.popup, 0) != image + POPUP_VTABLE || popup_busy(gui, menu.popup) ||
        I32(menu.owner, 0x2924) != menu.rank ||
        I32(menu.filter, FILTER_MODE) != 11 || I32(menu.filter, FILTER_CRITERION) != 29 ||
        !collect(menu.rank, &menu.pending) || !menu.pending.count) {
        reset_filter(NULL);
        return;
    }
    for (unsigned i = 0; i < RECORD_CAPACITY; ++i) popup_rows.rows[i].slot = -1;
    for (unsigned i = 0; i < menu.pending.count; ++i)
        popup_rows.rows[i].slot = menu.pending.entries[i].slot;

    I32(menu.filter, FILTER_MODE) = 8;
    GAME(UiMethod, 0x1077360)(menu.popup);
    InlineCallable stock = {image + STOCK_OPEN_CALLABLE, (uintptr_t)&popup_rows};
    GAME(QueueCallback, 0x10dee60)(&stock, owner, popup);
    menu.phase = OPEN;
    U32(menu.owner, GUI_INPUT_FLAGS) |= 0x40u;
}

static void queue_popup(Byte *owner, Byte *gui)
{
    bool lock = gui[0x30] || *(Byte *)(image + 0x51c4108);
    if (lock) EnterCriticalSection((CRITICAL_SECTION *)(gui + 8));
    Byte *popup = GAME(GameObjectGetter, 0x10834a0)(gui);
    Byte *filter = PTR(gui, GUI_SEARCH_FILTER);
    if (U64(gui, GUI_QUEUE_COUNT) < 128 && popup == PTR(gui, GUI_POPUP) &&
        readable(popup, 0x3d50) && ADDRESS(popup, 0) == image + POPUP_VTABLE &&
        (popup[0xc] & 0xe) && !popup_busy(gui, popup) &&
        readable(filter, 0xeea0) && I32(filter, FILTER_MODE) == 11 &&
        I32(filter, FILTER_CRITERION) == 29 && !filter[FILTER_CHANGED]) {
        menu.popup = popup;
        menu.filter = filter;
        menu.phase = QUEUED;
        menu.has_confirmation = false;
        menu.queued_at = GetTickCount64();

        QueueEntry entry = {0};
        GAME(void (*)(QueueEntry *, Byte *, Byte *), 0x1ae9150)(&entry, owner, popup);
        entry.callable = (InlineCallable){(uintptr_t)popup_callable_vtable, ++menu.generation};
        entry.target = &entry.callable;
        GAME(void (*)(Byte *, QueueEntry *), 0x509dc0)(gui + GUI_QUEUE, &entry);
        void *weak = GAME(void *(*)(void), 0x2229230)();
        GAME(void (*)(void *, Byte **), 0x22291d0)(weak, &entry.popup_ref);
        GAME(void (*)(void *, Byte **), 0x22291d0)(weak, &entry.owner_ref);
        U32(owner, GUI_INPUT_FLAGS) |= 0x40u;
    }
    if (lock) LeaveCriticalSection((CRITICAL_SECTION *)(gui + 8));
}

static void reset_filter(Byte *gui)
{
    if (menu.phase == OPEN && readable(gui, GUI_SEARCH_FILTER + 8) &&
        PTR(gui, GUI_SEARCH_FILTER) == menu.filter &&
        readable(menu.filter, 0xeea0) && I32(menu.filter, FILTER_MODE) == 8) {
        if (PTR(gui, GUI_POPUP) == menu.popup && readable(menu.popup, 0x3d50) &&
            ADDRESS(menu.popup, 0) == image + POPUP_VTABLE && (menu.popup[0xc] & 0xe))
            GAME(UiMethod, 0x1077360)(menu.popup);
        GAME(UiMethod, 0xb15760)(menu.filter);
    }
    menu.filter = NULL;
    menu.popup = NULL;
    menu.phase = IDLE;
}

static SortChoice confirmed_choice(Byte *filter)
{
    SortChoice choice = {I32(filter, FILTER_CRITERION), I32(filter, 0xce40), 0};
    if (choice.mode == 24) choice.arg = I32(filter, 0xce6c);
    if (choice.mode == 25) choice.arg = I32(filter, 0xce70);
    if (choice.mode == 27) {
        choice.arg = I32(filter, 0xce74);
        choice.extra = I32(filter, 0xce78);
    }
    return choice;
}

static void capture_confirmation(Byte *gui, Byte *filter, const Byte *payload)
{
    /* The shared result is overwritten every manager tick, including on close. */
    if (menu.phase != OPEN || filter != menu.filter ||
        payload != menu.popup + 0x28f0 || I32(filter, FILTER_MODE) != 8 ||
        !filter[FILTER_CHANGED]) return;
    if (!current_menu(menu.owner, gui) || PTR(gui, GUI_POPUP) != menu.popup ||
        I32(menu.owner, 0x2924) != menu.rank) return;
    SortChoice choice = confirmed_choice(filter);
    if (sort_choice_valid(choice)) {
        menu.confirmed = choice;
        menu.has_confirmation = true;
    }
}

static bool own_result(const InlineCallable *callable, Byte **owner)
{
    return menu.phase == OPEN && *owner == menu.popup &&
           callable->argument == (uintptr_t)menu.popup;
}

static void result_single_detour(const InlineCallable *callable, Byte **owner, Byte **result)
{
    if (!own_result(callable, owner)) original_result_single(callable, owner, result);
}

static void result_multiple_detour(const InlineCallable *callable, Byte **owner, Byte **result)
{
    if (!own_result(callable, owner)) original_result_multiple(callable, owner, result);
}

static void finish_popup(Byte *owner, Byte *gui)
{
    if (menu.phase == QUEUED) {
        if (GetTickCount64() - menu.queued_at > 2000) reset_filter(NULL);
        else U32(owner, GUI_INPUT_FLAGS) |= 0x40u;
        return;
    }
    if (menu.phase != OPEN) return;
    if (PTR(gui, GUI_POPUP) != menu.popup || PTR(gui, GUI_SEARCH_FILTER) != menu.filter ||
        !readable(menu.popup, 0x3d50) || !readable(menu.filter, 0xeea0)) {
        reset_filter(gui);
        return;
    }
    if (popup_busy(gui, menu.popup)) {
        U32(owner, GUI_INPUT_FLAGS) |= 0x40u;
        return;
    }

    bool changed = menu.has_confirmation;
    if (changed && !active(owner)) return;
    SortChoice choice = menu.confirmed;
    reset_filter(gui);
    if (!changed || !sort_choice_valid(choice)) return;
    Byte *page = PTR(owner, SELECT_RESEARCH_PAGE);
    if (!readable(page, 0x244) || U32(page, 0x240) > 6) return;
    Snapshot live = {0};
    if (!collect(menu.rank, &live) || !same_snapshot(&menu.pending, &live)) return;
    apply_choice(owner, menu.rank, &live, choice);
    bool refresh_details = (uint32_t)I32(owner, 0x2900) < SORT_PAGE_SIZE;
    GAME(int (*)(Byte *, unsigned, uint8_t), 0x1150500)(owner, U32(page, 0x240), refresh_details);
    U32(owner, GUI_INPUT_FLAGS) |= 0x40u;
}

static void leave_menu(Byte *gui)
{
    if (menu.phase == OPEN && readable(gui, GUI_SEARCH_FILTER + 8) &&
        PTR(gui, GUI_SEARCH_FILTER) == menu.filter && readable(menu.filter, 0xeea0) &&
        I32(menu.filter, FILTER_MODE) == 8 && PTR(gui, GUI_POPUP) == menu.popup &&
        readable(menu.popup, 0x3d50) && ADDRESS(menu.popup, 0) == image + POPUP_VTABLE &&
        (menu.popup[0xc] & 0xe) && popup_busy(gui, menu.popup))
        GAME(void (*)(Byte *, uint8_t), 0x1fb7cc0)(menu.popup, 1);
    reset_filter(gui);
    clear_order(menu.owner);
    menu.owner = NULL;
}

static void close_detour(Byte *owner)
{
    clear_order(owner);
    original_close(owner);
}

static bool sort_pressed(Byte *owner)
{
    if ((U32(owner, GUI_INPUT_FLAGS) & 0x400u) || (!owner[0x22c1] && !owner[0x22c2]))
        return false;
    if (GAME(bool (*)(Byte *, unsigned, Byte *), GUI_ACTION_PRESSED)(owner, RIGHT_STICK_CLICK, NULL))
        return true;
    /* The game's keyboard frame stores press edges indexed by virtual key. */
    Byte *keyboard = PTR((Byte *)image, KEYBOARD_GLOBAL);
    return readable(keyboard, KEYBOARD_TRIGGER + sizeof(uint32_t)) &&
           (U32(keyboard, KEYBOARD_TRIGGER) & (1u << VK_TAB));
}

static void process_filter_frame(Byte *filter, const Byte *payload)
{
    Byte *gui = manager();
    if (!readable(gui, GUI_SEARCH_FILTER + 8) || PTR(gui, GUI_SEARCH_FILTER) != filter)
        return;
    Byte *owner = PTR(gui, GUI_SELECT);
    bool in_scope = current_menu(owner, gui);
    if (menu.owner && (menu.owner != owner || !in_scope ||
        menu.rank != I32(owner, 0x2924) || I32(owner, 0x1d04) == 0))
        leave_menu(gui);
    if (!in_scope) return;
    if (!menu.owner) {
        if (!active(owner)) return;
        menu.owner = owner;
        menu.rank = I32(owner, 0x2924);
    }
    capture_confirmation(gui, filter, payload);
    finish_popup(owner, gui);
    if (menu.phase != IDLE || !active(owner) || owner[0x22c0] ||
        (U32(owner, GUI_INPUT_FLAGS) & 0x40u)) return;
    Byte *button = PTR(owner, SELECT_RESEARCH_LIST);
    if (!readable(button, 0x174) || button[0x173]) return;
    if (sort_pressed(owner)) queue_popup(owner, gui);
}

static void publish_detour(Byte *filter, const Byte *payload, unsigned target, uint8_t mode)
{
    const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    original_publish(filter, payload, target, mode);
    if (caller == image + PUBLISH_RETURN) process_filter_frame(filter, payload);
}

static bool stock_image(void)
{
    Byte *base = (Byte *)image;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (!readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return false;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (!readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.TimeDateStamp != 0x66d13cb6 || nt->OptionalHeader.SizeOfImage != IMAGE_SIZE)
        return false;
    static const struct { uint32_t rva, size; uint64_t hash; } signatures[] = {
        {SELECT_CLOSE, 0x20e, UINT64_C(0x62200baf62944753)},
        {BUILD_INVESTIGATIONS, 0x5eb, UINT64_C(0xfbb0df2b4f6cedd1)},
        {GUI_ACTION_PRESSED, 0xc1, UINT64_C(0xafb4ee753da5d5d5)},
        {0x1b16860, 0x5a, UINT64_C(0x59a73d0af7e6b8c8)},
        {0x22e74f0, 0x12c, UINT64_C(0x1bd48a0d4e120c6d)},
        {0x10dee60, 0x164, UINT64_C(0xeadf0ba9a075595e)},
        {0x10dead0, 0x208, UINT64_C(0xfc1d7bd4511b7ef8)},
        {0x10834a0, 0x71, UINT64_C(0xb3e5888df79f4062)},
        {0x1ae9150, 0x6e, UINT64_C(0x16eab2cd6b057234)},
        {0x509dc0, 0xe0, UINT64_C(0x0d4e0beeabba4dd9)},
        {0xb15760, 0xb8, UINT64_C(0x333ac3870644b9f3)},
        {0x1150500, 0x78a, UINT64_C(0xbea5720779a040a4)},
        {0xf19ea0, 0x314, UINT64_C(0x29a9ef2b2353b275)},
        {0xf1a750, 0x1d8, UINT64_C(0xcda31e23130f9e7c)},
        {0x1b8dbb0, 0x16, UINT64_C(0x94439d673d7206df)},
        {0x1fb7cc0, 0x25, UINT64_C(0x75fe309995471dad)},
        {0x22291d0, 0x60, UINT64_C(0x0177b42b32333db2)},
        {PUBLISH_FILTER, 0x250, UINT64_C(0x07519c85a0e6943c)},
        {0x1aeaf30, 0x2b, UINT64_C(0x5eaa6d6a9519a167)},
        {0x1aeb130, 0x43, UINT64_C(0x1f9890a7279694d5)},
        {RESULT_SINGLE, 0xa1, UINT64_C(0x317c05b217c22f92)},
        {RESULT_MULTIPLE, 0x110, UINT64_C(0x0251b3a874271e3e)},
        {0x1077360, 0xb0, UINT64_C(0x481f93872ade56b2)}
    };
    for (unsigned i = 0; i < sizeof(signatures) / sizeof(signatures[0]); ++i) {
        const Byte *code = base + signatures[i].rva;
        if (!readable(code, signatures[i].size)) return false;
        uint64_t hash = UINT64_C(14695981039346656037);
        for (unsigned j = 0; j < signatures[i].size; ++j)
            hash = (hash ^ code[j]) * UINT64_C(1099511628211);
        if (hash != signatures[i].hash) return false;
    }
    return ADDRESS(base + SELECT_VTABLE, 0x2d8) == image + SELECT_CLOSE &&
           ADDRESS(base + STOCK_OPEN_CALLABLE, 0x10) == image + 0x10dee60 &&
           ADDRESS(base + RESULT_SINGLE_CALLABLE, 0x10) == image + RESULT_SINGLE &&
           ADDRESS(base + RESULT_MULTIPLE_CALLABLE, 0x10) == image + RESULT_MULTIPLE;
}

static DWORD WINAPI install(void *unused)
{
    (void)unused;
    image = (uintptr_t)GetModuleHandleW(NULL);
    HMODULE self;
    if (!stock_image() ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCWSTR)(uintptr_t)install, &self) ||
        MH_Initialize() != MH_OK) return 0;
    memcpy(popup_callable_vtable, (const void *)(image + STOCK_OPEN_CALLABLE),
           sizeof(popup_callable_vtable));
    popup_callable_vtable[0] = (uintptr_t)clone_callable;
    popup_callable_vtable[1] = (uintptr_t)clone_callable;
    popup_callable_vtable[2] = (uintptr_t)invoke_popup;
    struct { uint32_t rva; void *detour; void **original; } hooks[] = {
        {SELECT_CLOSE, (void *)close_detour, (void **)&original_close},
        {BUILD_INVESTIGATIONS, (void *)build_detour, (void **)&original_build},
        {PUBLISH_FILTER, (void *)publish_detour, (void **)&original_publish},
        {RESULT_SINGLE, (void *)result_single_detour, (void **)&original_result_single},
        {RESULT_MULTIPLE, (void *)result_multiple_detour, (void **)&original_result_multiple}
    };
    const unsigned count = sizeof(hooks) / sizeof(hooks[0]);
    unsigned created = 0;
    for (; created < count; ++created) {
        void *target = (void *)(image + hooks[created].rva);
        if (MH_CreateHook(target, hooks[created].detour, hooks[created].original) != MH_OK) break;
    }
    if (created == count) {
        unsigned queued = 0;
        for (; queued < count; ++queued)
            if (MH_QueueEnableHook((void *)(image + hooks[queued].rva)) != MH_OK) break;
        if (queued == count && MH_ApplyQueued() == MH_OK) return 0;
    }
    while (created) MH_RemoveHook((void *)(image + hooks[--created].rva));
    MH_Uninitialize();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE worker = CreateThread(NULL, 0, install, NULL, 0, NULL);
        if (worker) CloseHandle(worker);
    }
    return TRUE;
}
