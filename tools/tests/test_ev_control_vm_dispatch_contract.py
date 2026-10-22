# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Keep VM RPC safe-point dispatch bounded and session-scoped."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CONTROL = (ROOT / "platform/ev_control_transport.c").read_text(encoding="utf-8")
PORT_CONFIG = (ROOT / "overlay/micropython/ports/esp32/mpconfigport.h").read_text(
    encoding="utf-8"
)
HAL = (ROOT / "overlay/micropython/ports/esp32/mphalport.c").read_text(
    encoding="utf-8"
)


def function_body(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


def function_text(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class VmDispatchContractTests(unittest.TestCase):
    def test_dispatch_runs_at_tight_loop_event_and_sleep_safe_points(self):
        self.assertIn("#define MICROPY_VM_HOOK_LOOP", PORT_CONFIG)
        vm_hook = function_body(PORT_CONFIG, "#define MICROPY_VM_HOOK_LOOP")
        self.assertIn("ev_control_transport_vm_poll();", vm_hook)

        event_start = PORT_CONFIG.index("#define MICROPY_EVENT_POLL_HOOK")
        event_end = PORT_CONFIG.index("// Functions that should go in IRAM", event_start)
        event_hooks = PORT_CONFIG[event_start:event_end]
        self.assertEqual(event_hooks.count("ev_control_transport_vm_poll();"), 2)

        delay = function_body(HAL, "void mp_hal_delay_ms(mp_uint_t ms)")
        self.assertIn("ev_control_transport_vm_poll();", delay)
        self.assertLess(
            delay.index("ev_control_transport_vm_poll();"),
            delay.index("MP_THREAD_GIL_EXIT();"),
        )

    def test_dispatch_is_main_thread_only_reentrancy_guarded_and_one_frame_bounded(self):
        poll = function_body(CONTROL, "void ev_control_transport_vm_poll(void)")
        self.assertLess(
            poll.index("__atomic_load_n(&s_vm_queue_pending, __ATOMIC_ACQUIRE)"),
            poll.index("!mp_thread_is_main_thread() || s_vm_poll_active"),
        )
        self.assertIn("!mp_thread_is_main_thread() || s_vm_poll_active", poll)
        self.assertLess(
            poll.index("s_vm_poll_active = true;"),
            poll.index("ev_control_vm_queue_lock();"),
        )
        self.assertEqual(
            poll.count("s_vm_queue_head = (uint8_t)((s_vm_queue_head + 1) % EV_CONTROL_VM_QUEUE_LEN);"),
            1,
        )
        self.assertEqual(
            poll.count("memcpy(&frame, &s_vm_queue[s_vm_queue_head], sizeof(frame));"),
            1,
        )
        self.assertIn("s_vm_poll_active = false;", poll)

    def test_pending_fast_path_tracks_all_queue_mutations_under_lock(self):
        for signature in (
            "static void ev_control_vm_queue_clear(void)",
            "static void ev_control_vm_queue_clear_stale(uint32_t input_generation)",
            "static void ev_control_vm_queue_expire(void)",
            "static bool ev_control_vm_queue_push(",
        ):
            body = function_body(CONTROL, signature)
            self.assertIn("ev_control_vm_queue_lock();", body)
            self.assertIn("__atomic_store_n(&s_vm_queue_pending", body)
        self.assertIn("__atomic_load_n(&s_vm_queue_pending, __ATOMIC_ACQUIRE)", CONTROL)
        poll = function_body(CONTROL, "void ev_control_transport_vm_poll(void)")
        self.assertIn("__ATOMIC_RELEASE", poll)

    def test_rpc_dispatch_keeps_epoch_session_output_and_nlr_scopes(self):
        poll = function_body(CONTROL, "void ev_control_transport_vm_poll(void)")
        for required in (
            "ev_session_input_generation() != frame.input_generation",
            "ev_session_authorize(&frame.link, frame.session_id, true, true,",
            "frame.vm_epoch",
            "session.transition_id != frame.transition_id",
            "ev_vm_output_origin_from_session(&session, &origin)",
            "ev_vm_output_scope_push(&origin, &output_scope)",
            "nlr_push(&nlr)",
            "ev_vm_output_scope_pop(&output_scope)",
            "ev_control_response_context_clear()",
        ):
            self.assertIn(required, poll)

    def test_production_queue_and_dispatch_functions_with_native_stubs(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        clear = function_text(CONTROL, "static void ev_control_vm_queue_clear(void)")
        push = function_text(CONTROL, "static bool ev_control_vm_queue_push(")
        poll = function_text(CONTROL, "void ev_control_transport_vm_poll(void)")
        prelude = r'''#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EV_CONTROL_VM_QUEUE_LEN 2
#define EV_CONTROL_JSON_METHOD_MAX 48
#define EV_SESSION_ID_HEX_LEN 32

typedef enum { EV_RPC_USER = 0, EV_RPC_DEBUG = 1 } ev_control_rpc_domain_t;
typedef enum { EV_SESSION_RESULT_OK = 0, EV_SESSION_RESULT_REJECTED = 1 } ev_session_result_t;
typedef struct { int transport_id; uint8_t slot; uint32_t generation; } ev_link_ref_t;
typedef struct {
    uint16_t payload_len;
    uint8_t domain;
    ev_link_ref_t link;
    char session_id[EV_SESSION_ID_HEX_LEN + 1];
    uint32_t vm_epoch;
    uint32_t input_generation;
    uint32_t transition_id;
    uint32_t queued_ms;
    uint32_t seq;
    char method[EV_CONTROL_JSON_METHOD_MAX + 1];
    uint8_t payload[64];
} ev_control_vm_frame_t;
typedef struct { uint32_t input_generation; uint32_t transition_id; } ev_session_snapshot_t;
typedef struct { uint32_t stale_generation_drops; } stats_t;
typedef struct { int unused; } ev_vm_output_origin_t;
typedef struct { bool active; } ev_vm_output_scope_t;
typedef struct { int unused; } nlr_buf_t;

static ev_control_vm_frame_t s_vm_queue[EV_CONTROL_VM_QUEUE_LEN];
static volatile uint8_t s_vm_queue_head;
static volatile uint8_t s_vm_queue_tail;
static uint32_t s_vm_queue_pending;
static bool s_vm_poll_active;
static stats_t s_stats;
static bool s_queue_locked;
static bool s_main_thread = true;
static bool s_authorized = true;
static bool s_reenter_from_handler;
static bool s_clear_before_next_lock;
static bool s_enqueue_after_unlock;
static bool s_fail_next_nlr;
static uint32_t s_input_generation = 7;
static uint32_t s_transition_id = 9;
static uint32_t s_now_ms = 100;
static uint32_t s_auth_calls;
static uint32_t s_last_auth_epoch;
static uint32_t s_lock_calls;
static uint32_t s_main_thread_checks;
static uint32_t s_handler_calls;
static uint32_t s_context_clears;
static uint32_t s_scope_pushes;
static uint32_t s_scope_pops;
static uint32_t s_error_calls;
static ev_control_rpc_domain_t s_response_rpc_domain;
static ev_link_ref_t s_response_link;
static bool s_has_response_link;
static char s_response_session_id[EV_SESSION_ID_HEX_LEN + 1];
static uint32_t s_response_vm_epoch;
static uint32_t s_response_tx_vm_epoch;
static uint32_t s_response_input_generation;
static uint32_t s_response_transition_id;
static const ev_link_ref_t s_link = { .transport_id = 3, .slot = 1, .generation = 5 };
static const char s_session_id[EV_SESSION_ID_HEX_LEN + 1] = "0123456789abcdef0123456789abcdef";

static bool ev_control_vm_queue_push(ev_control_rpc_domain_t domain,
                                     const ev_link_ref_t *link,
                                     const char *session_id,
                                     const char *method,
                                     uint32_t seq,
                                     uint32_t vm_epoch,
                                     uint32_t input_generation,
                                     uint32_t transition_id,
                                     const uint8_t *payload,
                                     size_t payload_len);
void ev_control_transport_vm_poll(void);

static bool mp_thread_is_main_thread(void) { ++s_main_thread_checks; return s_main_thread; }
static void ev_control_vm_queue_lock(void)
{
    // Model a queue clear immediately before this task acquires the mutex.
    if (s_clear_before_next_lock) {
        memset(s_vm_queue, 0, sizeof(s_vm_queue));
        s_vm_queue_head = s_vm_queue_tail;
        __atomic_store_n(&s_vm_queue_pending, 0, __ATOMIC_RELEASE);
        s_clear_before_next_lock = false;
    }
    assert(!s_queue_locked);
    s_queue_locked = true;
    ++s_lock_calls;
}
static void ev_control_vm_queue_unlock(void)
{
    assert(s_queue_locked);
    s_queue_locked = false;
    // A producer blocked behind the queue lock publishes after the consumer
    // has observed empty and cleared pending.
    if (s_enqueue_after_unlock) {
        static const uint8_t raced_payload[] = "{}";
        s_enqueue_after_unlock = false;
        assert(ev_control_vm_queue_push(EV_RPC_USER, &s_link, s_session_id,
                                        "raced", 55, 55,
                                        s_input_generation, s_transition_id,
                                        raced_payload, sizeof(raced_payload) - 1));
    }
}
static uint32_t ev_control_now_ms(void) { return s_now_ms; }
static uint32_t ev_session_input_generation(void) { return s_input_generation; }
static ev_session_result_t ev_session_authorize(const ev_link_ref_t *link,
                                                 const char *session_id,
                                                 bool require_owner,
                                                 bool require_vm,
                                                 uint32_t vm_epoch,
                                                 uint32_t now_ms,
                                                 ev_session_snapshot_t *snapshot)
{
    (void)require_owner;
    (void)require_vm;
    (void)now_ms;
    ++s_auth_calls;
    s_last_auth_epoch = vm_epoch;
    if (!s_authorized || link == NULL || session_id == NULL || snapshot == NULL) {
        return EV_SESSION_RESULT_REJECTED;
    }
    snapshot->input_generation = s_input_generation;
    snapshot->transition_id = s_transition_id;
    return EV_SESSION_RESULT_OK;
}
static void ev_control_vm_queue_expire(void) { }
static void ev_control_response_context_clear(void)
{
    ++s_context_clears;
    s_response_rpc_domain = EV_RPC_USER;
    memset(&s_response_link, 0, sizeof(s_response_link));
    s_has_response_link = false;
    memset(s_response_session_id, 0, sizeof(s_response_session_id));
    s_response_vm_epoch = 0;
    s_response_tx_vm_epoch = 0;
    s_response_input_generation = 0;
    s_response_transition_id = 0;
}
static bool ev_vm_output_origin_from_session(const ev_session_snapshot_t *session,
                                             ev_vm_output_origin_t *origin)
{
    (void)session;
    (void)origin;
    return true;
}
static bool ev_vm_output_scope_push(const ev_vm_output_origin_t *origin,
                                    ev_vm_output_scope_t *scope)
{
    (void)origin;
    scope->active = true;
    ++s_scope_pushes;
    return true;
}
static void ev_vm_output_scope_pop(ev_vm_output_scope_t *scope)
{
    assert(scope->active);
    scope->active = false;
    ++s_scope_pops;
}
static int nlr_push(nlr_buf_t *nlr)
{
    (void)nlr;
    if (s_fail_next_nlr) {
        s_fail_next_nlr = false;
        return 1;
    }
    return 0;
}
static void nlr_pop(void) { }
static int ev_control_send_error(const char *method, uint32_t seq,
                                 const char *code, const char *message)
{
    (void)method;
    (void)seq;
    (void)code;
    (void)message;
    ++s_error_calls;
    return 0;
}
static void ev_control_handle_user_rpc(uint32_t seq, const char *method, const char *payload);
static void ev_control_handle_debug_rpc(uint32_t seq, const char *method, const char *payload)
{
    (void)seq;
    (void)method;
    (void)payload;
    ++s_handler_calls;
}
static void ev_control_handle_user_rpc(uint32_t seq, const char *method, const char *payload)
{
    (void)seq;
    (void)method;
    (void)payload;
    ++s_handler_calls;
    if (s_reenter_from_handler) {
        static const uint8_t next_payload[] = "{}";
        s_reenter_from_handler = false;
        assert(s_response_vm_epoch == 11);
        assert(ev_control_vm_queue_push(EV_RPC_USER, &s_link, s_session_id,
                                        "second", 22, 22,
                                        s_input_generation, s_transition_id,
                                        next_payload, sizeof(next_payload) - 1));
        ev_control_transport_vm_poll();
        assert(s_handler_calls == 1);
        assert(s_response_vm_epoch == 11);
        assert(strcmp(s_response_session_id, s_session_id) == 0);
    }
}

'''
        main = r'''
int main(void)
{
    static const uint8_t payload[] = "{}";
    uint32_t lock_calls;
    uint32_t main_thread_checks;

    lock_calls = s_lock_calls;
    main_thread_checks = s_main_thread_checks;
    ev_control_transport_vm_poll();
    assert(s_lock_calls == lock_calls); // empty fast path avoids the mutex
    assert(s_main_thread_checks == main_thread_checks); // safe before VM-thread init

    assert(ev_control_vm_queue_push(EV_RPC_USER, &s_link, s_session_id, "first", 11, 11,
                                    s_input_generation, s_transition_id,
                                    payload, sizeof(payload) - 1));
    assert(s_vm_queue_pending == 1);
    s_main_thread = false;
    lock_calls = s_lock_calls;
    ev_control_transport_vm_poll();
    assert(s_lock_calls == lock_calls); // worker thread must not inspect or pop the queue
    assert(s_handler_calls == 0 && s_vm_queue_pending == 1);

    s_main_thread = true;
    s_reenter_from_handler = true;
    ev_control_transport_vm_poll();
    assert(s_handler_calls == 1); // nested poll could not dispatch the second frame
    assert(s_vm_queue_pending == 1); // enqueue during handling remains visible
    assert(!s_vm_poll_active);
    assert(s_scope_pushes == 1 && s_scope_pops == 1);

    ev_control_transport_vm_poll();
    assert(s_handler_calls == 2);
    assert(s_vm_queue_pending == 0);
    assert(s_last_auth_epoch == 22);
    assert(s_scope_pushes == 2 && s_scope_pops == 2);

    assert(ev_control_vm_queue_push(EV_RPC_USER, &s_link, s_session_id, "stale", 33, 33,
                                    s_input_generation, s_transition_id,
                                    payload, sizeof(payload) - 1));
    s_authorized = false;
    ev_control_transport_vm_poll();
    assert(s_handler_calls == 2 && s_stats.stale_generation_drops == 1);
    assert(s_vm_queue_pending == 0 && !s_vm_poll_active);
    s_authorized = true;

    assert(ev_control_vm_queue_push(EV_RPC_USER, &s_link, s_session_id, "race-old", 44, 44,
                                    s_input_generation, s_transition_id,
                                    payload, sizeof(payload) - 1));
    s_clear_before_next_lock = true;
    s_enqueue_after_unlock = true;
    ev_control_transport_vm_poll();
    assert(s_handler_calls == 2 && s_vm_queue_pending == 1);
    ev_control_transport_vm_poll();
    assert(s_handler_calls == 3 && s_last_auth_epoch == 55);

    assert(ev_control_vm_queue_push(EV_RPC_USER, &s_link, s_session_id, "raise", 66, 66,
                                    s_input_generation, s_transition_id,
                                    payload, sizeof(payload) - 1));
    s_fail_next_nlr = true;
    ev_control_transport_vm_poll();
    assert(s_handler_calls == 3 && s_error_calls == 1);
    assert(s_vm_queue_pending == 0 && !s_vm_poll_active);
    assert(s_scope_pushes == 4 && s_scope_pops == 4);

    assert(ev_control_vm_queue_push(EV_RPC_USER, &s_link, s_session_id, "clear", 44, 44,
                                    s_input_generation, s_transition_id,
                                    payload, sizeof(payload) - 1));
    ev_control_vm_queue_clear();
    assert(s_vm_queue_pending == 0 && s_vm_queue_head == s_vm_queue_tail);
    ev_control_transport_vm_poll();
    assert(s_handler_calls == 3 && s_error_calls == 1);
    assert(s_context_clears == 8);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="evmux-vm-dispatch-") as directory:
            source = Path(directory) / "vm_dispatch.c"
            binary = Path(directory) / "vm_dispatch"
            source.write_text(
                prelude + clear + "\n" + push + "\n" + poll + "\n" + main,
                encoding="utf-8",
            )
            subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)],
                check=True,
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            result = subprocess.run(
                [str(binary)], check=False, cwd=ROOT, capture_output=True, text=True
            )
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
