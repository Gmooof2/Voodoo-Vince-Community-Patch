/* Voodoo Vince Remastered, Steam x86 build 2049258.
 * Called by register-preserving wrappers in the appended PE section.
 * No CRT, imports, allocation, global state, or pointers retained across scenes.
 */
typedef unsigned int u32;
typedef unsigned char u8;
typedef void (__thiscall *Method)(void *);
typedef void (__thiscall *TimerMethod)(void *, u32);
typedef u8 (__thiscall *PlayingMethod)(void *);

int _fltused = 0;

#define U32(p, offset) (*(u32 *)((u8 *)(p) + (offset)))
#define PTR(p, offset) (*(void **)((u8 *)(p) + (offset)))
#define FLOAT(p, offset) (*(float *)((u8 *)(p) + (offset)))
#define ENGINE(base, rva, type) ((type)((base) + (rva)))
#define STATE 0x74       /* low two bits: 0 flythrough, 1 waiting, 2 finished */
#define ELAPSED 0x78     /* seconds of game time spent waiting */
#define REQUESTED 4u    /* flag: original two-second narration timer fired */
#define MODE(self) (U32(self, STATE) & 3u)
#define SET_MODE(self, mode) (U32(self, STATE) = (U32(self, STATE) & ~3u) | (mode))

static int same_name(void *a, void *b)
{
    u32 length = U32(a, 0x10), i;
    const u8 *left, *right;
    if (!length || length > 1024 || length != U32(b, 0x10)) return 0;
    left = U32(a, 0x14) >= 16 ? (const u8 *)PTR(a, 0) : (const u8 *)a;
    right = U32(b, 0x14) >= 16 ? (const u8 *)PTR(b, 0) : (const u8 *)b;
    if (!left || !right) return 0;
    for (i = 0; i < length; ++i) if (left[i] != right[i]) return 0;
    return 1;
}

static int narration_pending(void *self, u32 base)
{
    void *manager, *vince, *sound, *entry;
    u32 capacity, count, head, i;
    void **storage;
    /* A flythrough with no following cutscene must keep its original timing. */
    if (!U32(self, 0x50) || !U32(self, 0x68)) return 0;
    manager = PTR((void *)base, 0x7631ec);
    if (!manager) return 0;
    vince = PTR(manager, 0x1c);
    if (!vince) return 0;
    /* A very short camera can finish before the original narration timer. */
    if (!(U32(self, STATE) & REQUESTED)) return 1;
    sound = PTR(vince, 0x470);
    if (sound && same_name((u8 *)self + 0x58, (u8 *)sound + 8)
        && ENGINE(base, 0x49b60, PlayingMethod)(sound)) return 1;
    capacity = U32(vince, 0x480);
    count = U32(vince, 0x488);
    storage = (void **)PTR(vince, 0x47c);
    head = U32(vince, 0x484);
    /* Invalid queue geometry must not strand the transition. */
    if (!storage || !capacity || capacity > 1024 || (capacity & (capacity - 1))
        || count > capacity) return 0;
    for (i = 0; i < count; ++i) {
        entry = storage[(head + i) & (capacity - 1)];
        if (entry && same_name((u8 *)self + 0x58, entry)) return 1;
    }
    return 0;
}

void __cdecl nrf_activate(void *self, u32 base)
{
    U32(self, STATE) = 0;
    FLOAT(self, ELAPSED) = 0;
    ENGINE(base, 0xf2310, Method)(self);
}

void __cdecl nrf_finish(void *self, u32 base)
{
    /* Duplicate animation notifications cannot complete a scene twice. */
    if (MODE(self)) return;
    if (narration_pending(self, base)) {
        SET_MODE(self, 1);
        FLOAT(self, ELAPSED) = 0;
        return;
    }
    SET_MODE(self, 2);
    ENGINE(base, 0xf2390, Method)(self);
}

void __cdecl nrf_timer(void *self, u32 base, u32 timer_id)
{
    if (!timer_id) U32(self, STATE) |= REQUESTED;
    ENGINE(base, 0xf2580, TimerMethod)(self, timer_id);
}

void __cdecl nrf_tick(void *self, u32 base)
{
    void *engine, *clock;
    float dt = 0;
    ENGINE(base, 0xf2380, Method)(self);
    if (MODE(self) != 1) return;
    engine = PTR((void *)base, 0x7653f4);
    clock = engine ? PTR(engine, 8) : (void *)0;
    if (clock) dt = FLOAT(clock, 0);
    /* Game time stops while paused. Ignore negative/NaN/absurd frame deltas. */
    if (dt > 0 && dt <= 5) FLOAT(self, ELAPSED) += dt;
    if (FLOAT(self, ELAPSED) >= 60 || !narration_pending(self, base)) {
        SET_MODE(self, 2);
        /* Do not access self after original completion; it may destroy it. */
        ENGINE(base, 0xf2390, Method)(self);
    }
}
