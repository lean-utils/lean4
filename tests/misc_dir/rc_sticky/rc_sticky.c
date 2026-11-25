/*
Regression tests for the sticky reference-count paths of the runtime (#14838, #15241).

A thread-shared adjustment first checks the count against the sticky thresholds and only then
updates it with an atomic fetch-and-add, so another thread can move the count in between. The tests
replay such races deterministically: they set up the count the other thread left behind and then
apply only the atomic update.
*/
#include <lean/lean.h>

// `leanc` ships no C library headers, but links the C library.
int printf(char const * fmt, ...);

void lean_initialize_runtime_module(void);

static int g_failures = 0;

#define CHECK(c) do { \
    if (!(c)) { printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #c); g_failures++; } \
} while (0)

static int rc(lean_object * o) { return lean_internal_get_rc(o); }

#define CHECK_RC(o, expected) do { \
    int actual_ = rc(o), expected_ = (expected); \
    if (actual_ != expected_) { \
        printf("%s:%d: count of %s is %#x, expected %#x\n", __FILE__, __LINE__, #o, \
                (unsigned)actual_, (unsigned)expected_); \
        g_failures++; \
    } \
} while (0)

static lean_object * leaf(int count) {
    lean_object * o = lean_alloc_ctor(0, 0, 0);
    lean_internal_set_rc(o, count);
    return o;
}

/* Frees `o` and what it owns through the ordinary deletion path, whatever its count, so that the leak
   checker reports only real leaks. */
static void release(lean_object * o) {
    lean_internal_set_rc(o, 1);
    lean_dec(o);
}

static lean_object * box1(lean_object * child) {
    lean_object * o = lean_alloc_ctor(0, 1, 0);
    lean_ctor_set(o, 0, child);
    return o;
}

/* The atomic update of a thread-shared `lean_inc_ref_n` whose sticky check passed before the count
   moved. */
static void stale_inc(lean_object * o, int n) {
    atomic_fetch_sub_explicit(lean_get_rc_mt_addr(o), n, memory_order_relaxed);
}

/* Every count no drop may free or adjust: persistent, the band where drops stop, and the frozen range
   down to `INT_MIN`, which a single-threaded count reaches by wrapping. */
static const int g_never_freed[] = {
    0, LEAN_RC_STICKY_DROP, LEAN_RC_STICKY_DROP - 1, LEAN_RC_STICKY, LEAN_RC_STICKY - 1, INT_MIN + 1, INT_MIN,
};
#define NUM_NEVER_FREED (sizeof(g_never_freed) / sizeof(g_never_freed[0]))

static void test_drop_leaves_never_freed_counts(void) {
    for (size_t i = 0; i < NUM_NEVER_FREED; i++) {
        lean_object * o = leaf(g_never_freed[i]);
        lean_dec(o);
        CHECK_RC(o, g_never_freed[i]);
        release(o);
    }
}

/* Freeing a parent drops its children through the deletion cascade rather than `lean_dec_ref` (#15241). */
static void test_cascade_leaves_never_freed_counts(void) {
    for (size_t i = 0; i < NUM_NEVER_FREED; i++) {
        lean_object * child = leaf(g_never_freed[i]);
        lean_dec(box1(child));
        CHECK_RC(child, g_never_freed[i]);
        release(child);
    }
    lean_object * shared = leaf(-2);
    lean_dec(box1(shared));
    CHECK_RC(shared, -1);
    lean_dec(shared);
}

/* Counts no increment may adjust: persistent, and frozen ones. */
static const int g_never_incremented[] = {0, LEAN_RC_STICKY, LEAN_RC_STICKY - 1, INT_MIN + 1, INT_MIN};

static void test_inc_leaves_frozen_counts(void) {
    for (size_t i = 0; i < sizeof(g_never_incremented) / sizeof(g_never_incremented[0]); i++) {
        lean_object * o = leaf(g_never_incremented[i]);
        lean_inc(o);
        lean_inc_n(o, LEAN_RC_INC_MAX);
        lean_inc_n(o, LEAN_RC_INC_MAX + 1);
        CHECK_RC(o, g_never_incremented[i]);
        release(o);
    }
}

/* A single-threaded count too large for the live thread-shared range freezes at `LEAN_RC_STICKY`
   when shared, not where negating it lands, which near `INT_MAX` is among the overflowed
   single-threaded counts that `lean_mark_mt` still visits. */
static void test_mark_mt_freezes_large_counts(void) {
    static const int large[] = {INT_MAX, -LEAN_RC_STUCK_ST, -LEAN_RC_STICKY_DROP};
    for (size_t i = 0; i < sizeof(large) / sizeof(large[0]); i++) {
        lean_object * child = leaf(1);
        lean_object * parent = box1(child);
        lean_internal_set_rc(parent, large[i]);
        for (int j = 0; j < 2; j++) {
            lean_mark_mt(parent);
            CHECK_RC(parent, LEAN_RC_STICKY);
            CHECK_RC(child, -1);
        }
        release(parent);
    }
    lean_object * o = leaf(-LEAN_RC_STICKY_DROP - 1);
    lean_mark_mt(o);
    CHECK_RC(o, LEAN_RC_STICKY_DROP + 1);
    release(o);
}

/* An object whose single-threaded count overflowed is still owned by one thread, so sharing it has
   to mark what it points to as well. */
static void check_mark_mt_after_overflow(lean_object * parent, lean_object * child) {
    CHECK(!lean_is_st(parent));
    for (int i = 0; i < 2; i++) {
        lean_mark_mt(parent);
        CHECK_RC(parent, LEAN_RC_STICKY);
        CHECK_RC(child, -1);
    }
    release(parent);
}

static void test_mark_mt_after_overflow(void) {
    lean_object * child = leaf(1);
    lean_object * parent = box1(child);
    lean_inc_n(parent, (size_t)INT_MAX);
    check_mark_mt_after_overflow(parent, child);

    static const size_t inline_steps[] = {1, LEAN_RC_INC_MAX};
    for (size_t i = 0; i < sizeof(inline_steps) / sizeof(inline_steps[0]); i++) {
        child = leaf(1);
        parent = box1(child);
        lean_internal_set_rc(parent, INT_MAX);
        lean_inc_n(parent, inline_steps[i]);
        check_mark_mt_after_overflow(parent, child);
    }
}

/* A thread-shared count that froze and then drifted under the most maximal increments in flight the
   frozen range admits is not mistaken for an overflowed single-threaded one. */
static void test_mark_mt_skips_drifted_shared_counts(void) {
    lean_object * o = leaf(LEAN_RC_STICKY);
    for (int i = 0; i < 4094; i++)
        stale_inc(o, LEAN_RC_INC_MAX);
    int drifted = rc(o);
    lean_mark_mt(o);
    CHECK_RC(o, drifted);
    release(o);
}

/* The atomic update of a thread-shared `lean_dec_ref` whose never-freed check passed before the count
   moved. */
static void stale_dec(lean_object * o) {
    atomic_fetch_add_explicit(lean_get_rc_mt_addr(o), 1, memory_order_acq_rel);
}

static bool never_freed(lean_object * o) {
    int c = rc(o);
    return c == 0 || (c < 0 && (unsigned)c <= (unsigned)LEAN_RC_STICKY_DROP);
}

/* Never called: the closure of an unforced thunk only has to hold its argument. */
static lean_object * unforced_fn(lean_object * a, lean_object * unit) {
    (void)unit;
    return a;
}

/* The kinds of object the walk of `lean_mark_persistent` enters, each holding `child`. */
static lean_object * wrap_ctor(lean_object * child) { return box1(child); }
static lean_object * wrap_forced_thunk(lean_object * child) { return lean_thunk_pure(child); }
static lean_object * wrap_unforced_thunk(lean_object * child) {
    lean_object * c = lean_alloc_closure((void *)unforced_fn, 2, 1);
    lean_closure_set(c, 0, child);
    return lean_mk_thunk(c);
}
static lean_object * wrap_ref(lean_object * child) { return lean_st_mk_ref(child); }
static lean_object * wrap_task(lean_object * child) { return lean_task_pure(child); }

static lean_object * (* const g_wraps[])(lean_object *) = {
    wrap_ctor, wrap_forced_thunk, wrap_unforced_thunk, wrap_ref, wrap_task,
};
static char const * const g_wrap_names[] = {"constructor", "forced thunk", "unforced thunk", "Ref", "task"};
#define NUM_WRAPS (sizeof(g_wraps) / sizeof(g_wraps[0]))

#define CHECK_KIND(c, w) do { \
    if (!(c)) { printf("%s:%d: check failed for a %s: %s\n", __FILE__, __LINE__, g_wrap_names[w], #c); \
                g_failures++; } \
} while (0)

static const uint8_t g_wrap_tags[] = {0, LeanThunk, LeanThunk, LeanRef, LeanTask};

/* An object of kind `w` that other threads hold (count -2), around a thread-shared leaf it owns. */
static lean_object * shared_node(size_t w, lean_object ** inner) {
    *inner = leaf(-1);
    lean_object * o = g_wraps[w](*inner);
    CHECK_KIND(lean_ptr_tag(o) == g_wrap_tags[w], w);
    lean_internal_set_rc(o, -2);
    return o;
}

/* `lean_mark_persistent` on objects other threads hold, as `lean_obj_once_cold` does when a closed
   term's value was already shared before the term was first read: a thread-shared root, and a
   thread-shared child of a single-threaded parent, of every kind the walk enters (a spawned task is
   always thread-shared). An adjustment whose check passed before the marking applies its
   atomic update after it; a plain store of 0 turned that into a count of 1 or -1, which the next drop
   freed while the object was still referenced. The walk stops at such an object: what it points to
   stays alive through it, and is not read while other threads may update it (a thunk being forced, a
   `Ref` being set) or waited for (a running task). */
static void test_mark_persistent_shared(void) {
    for (size_t w = 0; w < NUM_WRAPS; w++) {
        for (int late_drop = 0; late_drop < 2; late_drop++) {
            lean_object * root_inner, * child_inner;
            lean_object * root = shared_node(w, &root_inner);
            lean_object * child = shared_node(w, &child_inner);
            lean_object * parent = box1(child);
            lean_mark_persistent(root);
            lean_mark_persistent(parent);
            CHECK_KIND(rc(parent) == 0, w);
            lean_object * shared[] = {root, child};
            lean_object * inner[] = {root_inner, child_inner};
            for (size_t i = 0; i < 2; i++) {
                lean_object * o = shared[i];
                CHECK_KIND(rc(o) == LEAN_RC_STICKY, w);
                CHECK_KIND(rc(inner[i]) == -1, w);
                if (late_drop) stale_dec(o); else stale_inc(o, LEAN_RC_INC_MAX);
                CHECK_KIND(never_freed(o), w);
                if (!never_freed(o)) continue;
                lean_dec(o);
                lean_inc(o);
                lean_dec(o);
                CHECK_KIND(never_freed(o), w);
            }
            release(parent);
            release(child);
            release(root);
        }
    }
    // A thread-shared count that froze, and one that late increments moved further into the band,
    // are left where they are; one that late drops moved back above the band's top is lowered to it.
    static const int frozen[][2] = {
        {LEAN_RC_STICKY, LEAN_RC_STICKY}, {LEAN_RC_STICKY - 8, LEAN_RC_STICKY - 8},
        {LEAN_RC_STICKY + 8, LEAN_RC_STICKY},
    };
    for (size_t i = 0; i < sizeof(frozen) / sizeof(frozen[0]); i++) {
        lean_object * o = leaf(frozen[i][0]);
        lean_mark_persistent(o);
        CHECK_RC(o, frozen[i][1]);
        release(o);
    }
    // A single-threaded graph is still marked persistent throughout.
    lean_object * child = leaf(1);
    lean_object * parent = box1(child);
    lean_mark_persistent(parent);
    CHECK_RC(parent, 0);
    CHECK_RC(child, 0);
    release(parent);
}

int main(void) {
    lean_initialize_runtime_module();
    test_mark_persistent_shared();
    test_drop_leaves_never_freed_counts();
    test_cascade_leaves_never_freed_counts();
    test_inc_leaves_frozen_counts();
    test_mark_mt_freezes_large_counts();
    test_mark_mt_after_overflow();
    test_mark_mt_skips_drifted_shared_counts();
    if (g_failures != 0) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
