/*
Stress test of `lean_mark_persistent` on an object other threads hold, as `lean_obj_once_cold` marks
a closed term's value that running tasks already share. `test_mark_persistent_shared` in
`../rc_sticky` replays the race deterministically; this test lets it happen: in each round, holder
tasks take and drop references to a thread-shared object while the main thread marks it. An
adjustment whose check came before the marking applies its atomic update after it, so a marking that
stored a count of 0 left 1 or -1 there, and a later drop freed the object while it was still
referenced (seen as heap corruption). The marked object's count must stay one no drop frees.

The race is probabilistic: see the measurement next to `ROUNDS`.
*/
#include <lean/lean.h>

// `leanc` ships no C library headers, but links the C library.
int printf(char const * fmt, ...);

void lean_initialize_runtime_module(void);
// Exported by the runtime for `IO.Promise`, not declared in `lean.h`.
lean_obj_res lean_io_promise_new(void);
lean_obj_res lean_io_promise_resolve(lean_obj_arg value, b_lean_obj_arg promise);
lean_obj_res lean_io_promise_result_opt(b_lean_obj_arg promise);

#define HOLDERS 4
#define ADJUSTMENTS 20000
// On a runtime that stored 0, each of 30 runs failed (on an Apple M1); see the commit that added it.
#define ROUNDS 2000
// `Task.Priority.dedicated`: each holder gets a thread of its own, so all of them run at once.
#define DEDICATED 9

/* Takes and drops references to `o`, as a task holding a shared value does, once it has said that
   it started. Owns its reference to `o` and to `started`. */
static lean_object * holder(lean_object * o, lean_object * started, lean_object * unit) {
    (void)unit;
    lean_dec(lean_io_promise_resolve(lean_box(0), started));
    lean_dec(started);
    for (int i = 0; i < ADJUSTMENTS; i++) {
        lean_inc_ref(o);
        lean_dec_ref(o);
    }
    lean_dec(o);
    return lean_box(0);
}

static bool never_freed(int c) {
    return c == 0 || (c < 0 && (unsigned)c <= (unsigned)LEAN_RC_STICKY_DROP);
}

int main(void) {
    lean_initialize_runtime_module();
    lean_init_task_manager();
    int failures = 0;
    for (int r = 1; r <= ROUNDS; r++) {
        lean_object * o = lean_alloc_ctor(0, 0, sizeof(uint64_t));
        lean_ctor_set_uint64(o, 0, (uint64_t)r);
        lean_inc_ref_n(o, HOLDERS); // a reference per holder, and ours
        lean_object * started[HOLDERS], * tasks[HOLDERS];
        for (int i = 0; i < HOLDERS; i++) {
            lean_object * promise = lean_io_promise_new();
            started[i] = lean_io_promise_result_opt(promise);
            lean_object * c = lean_alloc_closure((void *)holder, 3, 2);
            lean_closure_set(c, 0, o);
            lean_closure_set(c, 1, promise);
            // Spawning shares the closure, and with it `o`.
            tasks[i] = lean_task_spawn_core(c, DEDICATED, false);
        }
        for (int i = 0; i < HOLDERS; i++) // every holder has started
            lean_task_get(started[i]);
        lean_mark_persistent(o);
        for (int i = 0; i < HOLDERS; i++) // every holder is done
            lean_task_get(tasks[i]);
        int c = lean_internal_get_rc(o);
        if (!never_freed(c)) {
            printf("round %d: count %d after lean_mark_persistent\n", r, c);
            failures++;
        }
        for (int i = 0; i < HOLDERS; i++) {
            lean_dec(started[i]);
            lean_dec(tasks[i]);
        }
        // `o` stays allocated: it is persistent, or a failed round may have freed it already.
    }
    // Waits for the holders' threads to exit.
    lean_finalize_task_manager();
    if (failures != 0) {
        printf("%d of %d rounds left a count a drop frees\n", failures, ROUNDS);
        return 1;
    }
    return 0;
}
