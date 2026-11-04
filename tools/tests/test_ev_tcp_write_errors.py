# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Execute production nonblocking-send error handling without a network stack."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class TcpWriteErrorsTest(unittest.TestCase):
    def test_transient_memory_pressure_yields_but_connection_errors_close(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('native compiler unavailable')
        production = (ROOT / 'platform/ev_tcp.c').read_text()
        result = function(production, 'static ev_io_result_t ev_tcp_result(')
        write = function(production, 'ev_io_result_t ev_tcp_link_writev(')
        classify = 'static ev_io_result_t classify(int written, int saved_errno) {\n'
        classify += write[write.index('    if (written > 0)'):]
        harness = '#include <assert.h>\n#include <errno.h>\n#include "ev_transport.h"\n'
        harness += result + '\n' + classify + r'''
int main(void) {
    int transient[] = {EAGAIN, EWOULDBLOCK, EINTR, ENOMEM, ENOBUFS};
    for (unsigned i = 0; i < sizeof(transient) / sizeof(transient[0]); ++i) {
        ev_io_result_t result = classify(-1, transient[i]);
        assert(result.status == EV_IO_WOULD_BLOCK && result.transferred == 0 && result.error == ESP_OK);
    }
    int closed[] = {ECONNRESET, ENOTCONN, EPIPE, ECONNABORTED};
    for (unsigned i = 0; i < sizeof(closed) / sizeof(closed[0]); ++i) {
        assert(classify(-1, closed[i]).status == EV_IO_CLOSED);
    }
    assert(classify(0, 0).status == EV_IO_CLOSED);
    assert(classify(-1, EBADF).status == EV_IO_ERROR);
    ev_io_result_t progress = classify(7, ENOMEM);
    assert(progress.status == EV_IO_PROGRESS && progress.transferred == 7);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='ev-tcp-write-') as temporary:
            source = pathlib.Path(temporary) / 'write.c'
            binary = pathlib.Path(temporary) / 'write'
            source.write_text(harness)
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Werror',
                            '-I' + str(ROOT / 'tools/tests/ev_mux_tx_stubs'),
                            '-I' + str(ROOT / 'platform'), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_sendmsg_uses_lwip_flags_bounded_vectors_and_short_progress(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('native compiler unavailable')
        production = (ROOT / 'platform/ev_tcp.c').read_text()
        result = function(production, 'static ev_io_result_t ev_tcp_result(')
        writev = function(production, 'ev_io_result_t ev_tcp_link_writev(')
        harness = r'''#include <assert.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include "ev_transport.h"
#define EV_TCP_MAX_WRITE_CHUNK 4096
#define pdTRUE 1
static void *s_socket_lock = (void *)1;
static struct { int fd; } s_links[EV_TRANSPORT_TCP_LINKS];
static bool locked, busy, current = true;
static unsigned calls;
static int xSemaphoreTake(void *lock, int wait) {
    (void)lock; (void)wait;
    if (busy) return 0;
    assert(!locked); locked = true; return pdTRUE;
}
static void xSemaphoreGive(void *lock) { (void)lock; assert(locked); locked = false; }
static bool ev_tcp_link_matches_locked(const ev_link_ref_t *link) {
    assert(locked && link->slot == 2); return current;
}
static int mock_sendmsg(int fd, const struct msghdr *msg, int flags) {
    assert(locked && fd == 17);
    assert(flags == MSG_DONTWAIT); // lwIP rejects MSG_NOSIGNAL on sendmsg.
    assert(msg->msg_iovlen == 3);
    assert(msg->msg_iov[0].iov_len == 10 && msg->msg_iov[1].iov_len == 20);
    assert(msg->msg_iov[2].iov_len == 4066);
    ++calls;
    return 7;
}
#define sendmsg mock_sendmsg
''' + result + '\n' + writev + r'''
int main(void) {
    static const char data[4096];
    ev_link_ref_t link = {.transport_id=EV_TRANSPORT_TCP, .slot=2, .generation=1};
    s_links[2].fd = 17;
    const ev_io_vector_t vectors[] = {{data,10}, {data,20}, {data,4096}, {data,1}};
    ev_io_result_t result = ev_tcp_link_writev(&link, vectors, 4);
    assert(result.status == EV_IO_PROGRESS && result.transferred == 7);
    assert(!locked && calls == 1);
    busy = true;
    assert(ev_tcp_link_writev(&link, vectors, 4).status == EV_IO_WOULD_BLOCK);
    busy = false; current = false;
    assert(ev_tcp_link_writev(&link, vectors, 4).status == EV_IO_CLOSED);
    assert(!locked && calls == 1);
    assert(ev_tcp_link_writev(&link, NULL, 0).status == EV_IO_ERROR);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='ev-tcp-vector-') as temporary:
            source = pathlib.Path(temporary) / 'vector.c'
            binary = pathlib.Path(temporary) / 'vector'
            source.write_text(harness)
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'tools/tests/ev_mux_tx_stubs'),
                            '-I' + str(ROOT / 'platform'), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_close_retires_exact_generation_without_holding_other_socket_writes(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('native compiler unavailable')
        production = (ROOT / 'platform/ev_tcp.c').read_text()
        close_function = function(production, 'static void ev_tcp_close_pending_links(')
        harness = r'''#include <assert.h>
#include <string.h>
#include "ev_transport.h"
#define portMAX_DELAY 1
#define pdTRUE 1
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static int s_close_mux, s_socket_lock;
static bool held;
static bool s_close_pending[EV_TRANSPORT_TCP_LINKS];
static uint32_t s_close_generation[EV_TRANSPORT_TCP_LINKS];
typedef struct { bool active; int fd; ev_link_ref_t ref; } ev_tcp_link_t;
static ev_tcp_link_t s_links[EV_TRANSPORT_TCP_LINKS];
static int closes;
static int xSemaphoreTake(int lock, int wait) {
    assert(!held); held = true; return pdTRUE;
}
static void xSemaphoreGive(int lock) { assert(held); held = false; }
static int close(int fd) {
    assert(!held); // A slow close must never exclude other socket writes.
    assert(fd == 17);
    assert(!s_links[0].active && s_links[0].fd == -1);
    assert(s_links[1].active && s_links[1].fd == 18);
    ++closes;
    return 0;
}
''' + close_function + r'''
int main(void) {
    s_links[0] = (ev_tcp_link_t){.active=true, .fd=17, .ref={.generation=9}};
    s_links[1] = (ev_tcp_link_t){.active=true, .fd=18, .ref={.generation=10}};
    s_close_pending[0] = s_close_pending[1] = true;
    s_close_generation[0] = s_close_generation[1] = 9;
    ev_tcp_close_pending_links();
    assert(closes == 1 && s_links[1].active);
    assert(!s_close_pending[0] && !s_close_pending[1]);
    ev_tcp_close_pending_links();
    assert(closes == 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='ev-tcp-close-') as temporary:
            source = pathlib.Path(temporary) / 'close.c'
            binary = pathlib.Path(temporary) / 'close'
            source.write_text(harness)
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Werror',
                            '-I' + str(ROOT / 'tools/tests/ev_mux_tx_stubs'),
                            '-I' + str(ROOT / 'platform'), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
