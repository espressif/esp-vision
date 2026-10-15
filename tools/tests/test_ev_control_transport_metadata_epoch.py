# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Exercise the production RPC metadata formatter at session epoch zero."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CONTROL_SOURCE = ROOT / "platform/ev_control_transport.c"


class MetadataEpochContractTests(unittest.TestCase):
    def test_bound_epoch_zero_nonzero_and_unbound_identity(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            self.skipTest("a native C compiler is unavailable")

        production = CONTROL_SOURCE.read_text(encoding="utf-8")
        start = production.index("static ev_mux_tx_class_t ev_control_tx_class(")
        end = production.index("\nstatic esp_err_t ev_control_send_rpc_with_domain(", start)
        formatter = production[start:end]

        prelude = r'''#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG -1
#define ESP_ERR_INVALID_SIZE -2
#define EV_CONTROL_METADATA_MAX 384
#define EV_SESSION_ID_HEX_LEN 32

typedef enum { EV_RPC_USER = 0, EV_RPC_DEBUG = 1 } ev_control_rpc_domain_t;
typedef struct { int transport_id; uint8_t slot; uint32_t generation; } ev_link_ref_t;
typedef enum { EV_MUX_TX_SESSION = 0, EV_MUX_TX_CONTROL = 1 } ev_mux_tx_class_t;
typedef uint32_t ev_mux_ticket_t;
typedef void (*ev_mux_completion_cb_t)(esp_err_t, ev_link_ref_t, const void *, size_t);
typedef struct {
    const ev_link_ref_t *link;
    const char *session_id;
    uint32_t vm_epoch;
    ev_mux_tx_class_t tx_class;
    ev_mux_completion_cb_t completion;
    const void *completion_context;
    size_t completion_context_len;
} ev_mux_submit_options_t;

static ev_mux_completion_cb_t s_response_completion;
static uint8_t s_response_completion_context[40];
static size_t s_response_completion_context_len;
static char s_metadata[EV_CONTROL_METADATA_MAX];
static char s_session_id[EV_SESSION_ID_HEX_LEN + 1];
static bool s_has_session_id;
static uint32_t s_tx_vm_epoch;
static ev_mux_tx_class_t s_tx_class;

static const char *ev_control_rpc_channel(ev_control_rpc_domain_t domain)
{
    return domain == EV_RPC_DEBUG ? "debug.rpc" : "user.rpc";
}

static uint32_t ev_control_now_ms(void)
{
    return 77;
}

static esp_err_t ev_mux_submit_link(const ev_mux_submit_options_t *options,
                                   const char *metadata,
                                   const void *payload,
                                   size_t payload_len,
                                   ev_mux_ticket_t *ticket_out)
{
    (void)ticket_out;
    assert(options != NULL && options->link != NULL);
    assert(payload != NULL && payload_len == strlen((const char *)payload));
    snprintf(s_metadata, sizeof(s_metadata), "%s", metadata);
    s_has_session_id = options->session_id != NULL;
    if (s_has_session_id) {
        snprintf(s_session_id, sizeof(s_session_id), "%s", options->session_id);
    } else {
        s_session_id[0] = '\0';
    }
    s_tx_vm_epoch = options->vm_epoch;
    s_tx_class = options->tx_class;
    return ESP_OK;
}

'''
        main = r'''
int main(void)
{
    const ev_link_ref_t link = { .transport_id = 3, .slot = 1, .generation = 9 };
    const char session_id[] = "0123456789abcdef0123456789abcdef";
    const char payload[] = "{}";

    assert(ev_control_submit_rpc_link(&link, EV_RPC_USER, "rsp", "hello", payload,
                                      1, session_id, 0, 0, NULL) == ESP_OK);
    assert(strstr(s_metadata, "\"sessionId\":\"0123456789abcdef0123456789abcdef\"") != NULL);
    assert(strstr(s_metadata, "\"vmEpoch\":0") != NULL);
    assert(s_has_session_id && strcmp(s_session_id, session_id) == 0);
    assert(s_tx_vm_epoch == 0 && s_tx_class == EV_MUX_TX_SESSION);

    assert(ev_control_submit_rpc_link(&link, EV_RPC_USER, "rsp", "script.run", payload,
                                      2, session_id, 17, 17, NULL) == ESP_OK);
    assert(strstr(s_metadata, "\"vmEpoch\":17") != NULL);
    assert(s_has_session_id && strcmp(s_session_id, session_id) == 0);
    assert(s_tx_vm_epoch == 17 && s_tx_class == EV_MUX_TX_CONTROL);

    assert(ev_control_submit_rpc_link(&link, EV_RPC_USER, "event", "hello", payload,
                                      3, NULL, 0, 0, NULL) == ESP_OK);
    assert(strstr(s_metadata, "\"sessionId\"") == NULL);
    assert(strstr(s_metadata, "\"vmEpoch\"") == NULL);
    assert(!s_has_session_id && s_session_id[0] == '\0');
    assert(s_tx_vm_epoch == 0 && s_tx_class == EV_MUX_TX_SESSION);
    return 0;
}
'''

        with tempfile.TemporaryDirectory(prefix="evmux-metadata-epoch-") as directory:
            source = Path(directory) / "metadata_epoch.c"
            binary = Path(directory) / "metadata_epoch"
            source.write_text(prelude + formatter + main, encoding="utf-8")
            subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)],
                check=True,
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(binary)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
