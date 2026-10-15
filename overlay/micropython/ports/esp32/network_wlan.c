/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * Development of the code in this file was sponsored by Microbric Pty Ltd
 * and Mnemote Pty Ltd
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016, 2017 Nick Moore @mnemote
 * Copyright (c) 2017 "Eric Poulsen" <eric@zyxod.com>
 *
 * Based on esp8266/modnetwork.c which is Copyright (c) 2015 Paul Sokolovsky
 * And the ESP IDF example code which is Public Domain / CC0
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <string.h>

#include "py/objlist.h"
#include "py/mperrno.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "extmod/modnetwork.h"
#include "modnetwork.h"

#include "esp_wifi.h"
#include "network_service.h"
#include "py/nlr.h"

#ifndef NO_QSTR
#include "mdns.h"
#include "network_discovery.h"
#include "esp_log.h"
#include "device_identity.h"
#endif

#if MICROPY_PY_NETWORK_WLAN

#if (WIFI_MODE_STA & WIFI_MODE_AP != WIFI_MODE_NULL || WIFI_MODE_STA | WIFI_MODE_AP != WIFI_MODE_APSTA)
#error WIFI_MODE_STA and WIFI_MODE_AP are supposed to be bitfields!
#endif

typedef base_if_obj_t wlan_if_obj_t;

static wlan_if_obj_t wlan_sta_obj;
static wlan_if_obj_t wlan_ap_obj;

static uint8_t conf_wifi_sta_reconnects = 0;

// The rules for this default are defined in the documentation of esp_wifi_set_protocol()
// rather than in code, so we have to recreate them here.
#if CONFIG_SOC_WIFI_HE_SUPPORT
// Note: No Explicit support for 5GHz here, yet
#define WIFI_PROTOCOL_DEFAULT (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX)
#else
#define WIFI_PROTOCOL_DEFAULT (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)
#endif

#if MICROPY_HW_ENABLE_MDNS_QUERIES || MICROPY_HW_ENABLE_MDNS_RESPONDER
static void network_wlan_ip_ready(const esp_vision_network_snapshot_t *snapshot, void *context) {
    (void)snapshot;
    (void)context;
    esp_err_t err = esp_vision_discovery_ensure();
    if (err != ESP_OK) {
        ESP_LOGW("network", "mDNS unavailable: %s", esp_err_to_name(err));
    }
}
#endif

static void require_if(mp_obj_t wlan_if, int if_no) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(wlan_if);
    if (self->if_id != if_no) {
        mp_raise_msg(&mp_type_OSError, if_no == ESP_IF_WIFI_STA ? MP_ERROR_TEXT("STA required") : MP_ERROR_TEXT("AP required"));
    }
}

void esp_initialise_wifi(void) {
    esp_exceptions(esp_vision_network_init());
    wlan_sta_obj.base.type = &esp_network_wlan_type;
    wlan_sta_obj.if_id = ESP_IF_WIFI_STA;
    wlan_sta_obj.netif = esp_vision_network_sta_netif();
    wlan_sta_obj.active = esp_vision_network_python_sta_active();
    wlan_ap_obj.base.type = &esp_network_wlan_type;
    wlan_ap_obj.if_id = ESP_IF_WIFI_AP;
    wlan_ap_obj.netif = esp_vision_network_ap_netif();
    wlan_ap_obj.active = esp_vision_network_python_ap_active();
#if MICROPY_HW_ENABLE_MDNS_QUERIES || MICROPY_HW_ENABLE_MDNS_RESPONDER
    esp_vision_network_set_ip_ready_callback(network_wlan_ip_ready, NULL);
#endif
}

static mp_obj_t network_wlan_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, false);

    esp_initialise_wifi();

    int idx = (n_args > 0) ? mp_obj_get_int(args[0]) : ESP_IF_WIFI_STA;
    if (idx == ESP_IF_WIFI_STA) {
        return MP_OBJ_FROM_PTR(&wlan_sta_obj);
    } else if (idx == ESP_IF_WIFI_AP) {
        return MP_OBJ_FROM_PTR(&wlan_ap_obj);
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid WLAN interface identifier"));
    }
}

static mp_obj_t network_wlan_active(size_t n_args, const mp_obj_t *args) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    bool is_sta = self->if_id == ESP_IF_WIFI_STA;
    int bit = is_sta ? WIFI_MODE_STA : WIFI_MODE_AP;
    wifi_mode_t mode = WIFI_MODE_NULL;

    if (n_args > 1) {
        if (esp_vision_network_python_mutation_begin() != ESP_OK) {
            mp_raise_OSError(MP_EBUSY);
        }
        bool active = mp_obj_is_true(args[1]);
        esp_err_t err = esp_wifi_get_mode(&mode);
        if (err == ESP_ERR_WIFI_NOT_STARTED) {
            mode = WIFI_MODE_NULL;
            err = ESP_OK;
        }
        if (err != ESP_OK) {
            esp_vision_network_python_mutation_cancel();
            esp_exceptions(err);
        }
        mode = active ? (wifi_mode_t)(mode | bit) : (wifi_mode_t)(mode & ~bit);
        if (mode == WIFI_MODE_NULL) {
            if (esp_vision_network_python_sta_active() || esp_vision_network_python_ap_active()) {
                err = esp_wifi_stop();
            }
        } else {
            err = esp_wifi_set_mode(mode);
            if (err == ESP_OK) {
                err = esp_wifi_start();
                if (err == ESP_ERR_WIFI_NOT_STOPPED) {
                    err = ESP_OK;
                }
            }
        }
        if (err != ESP_OK) {
            esp_vision_network_python_mutation_end();
            esp_exceptions(err);
        }
        if (is_sta) {
            esp_vision_network_python_set_sta_active(active);
        } else {
            esp_vision_network_python_set_ap_active(active);
        }
        esp_vision_network_python_mutation_end();
        self->active = active;
    } else {
        esp_err_t err = esp_wifi_get_mode(&mode);
        if (err == ESP_ERR_WIFI_NOT_STARTED) {
            mode = WIFI_MODE_NULL;
        } else if (err != ESP_OK) {
            esp_exceptions(err);
        }
    }

    return (mode & bit) ? mp_const_true : mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_wlan_active_obj, 1, 2, network_wlan_active);

static mp_obj_t network_wlan_connect(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_ssid, ARG_key, ARG_bssid };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_, MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_, MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_bssid, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    wifi_config_t wifi_sta_config = {0};
    if (n_args > 1) {
        size_t len;
        const char *p;
        if (args[ARG_ssid].u_obj != mp_const_none) {
            p = mp_obj_str_get_data(args[ARG_ssid].u_obj, &len);
            memcpy(wifi_sta_config.sta.ssid, p, MIN(len, sizeof(wifi_sta_config.sta.ssid)));
        }
        if (args[ARG_key].u_obj != mp_const_none) {
            p = mp_obj_str_get_data(args[ARG_key].u_obj, &len);
            memcpy(wifi_sta_config.sta.password, p, MIN(len, sizeof(wifi_sta_config.sta.password)));
        }
        if (args[ARG_bssid].u_obj != mp_const_none) {
            p = mp_obj_str_get_data(args[ARG_bssid].u_obj, &len);
            if (len != sizeof(wifi_sta_config.sta.bssid)) {
                mp_raise_ValueError(NULL);
            }
            wifi_sta_config.sta.bssid_set = 1;
            memcpy(wifi_sta_config.sta.bssid, p, sizeof(wifi_sta_config.sta.bssid));
        }
    }

    if (esp_vision_network_python_mutation_begin() != ESP_OK) {
        mp_raise_OSError(MP_EBUSY);
    }
    esp_err_t err = ESP_OK;
    if (n_args > 1) {
        err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_sta_config);
    }
    memset(&wifi_sta_config, 0, sizeof(wifi_sta_config));
    if (err == ESP_OK) {
        err = esp_netif_set_hostname(wlan_sta_obj.netif, esp_vision_hostname());
    }
    if (err == ESP_OK) {
        MP_THREAD_GIL_EXIT();
        err = esp_wifi_connect();
        MP_THREAD_GIL_ENTER();
    }
    if (err != ESP_OK) {
        esp_vision_network_python_mutation_end();
        esp_exceptions(err);
    }
    esp_vision_network_python_set_connect_requested(true);
    esp_vision_network_python_runtime_changed();
    esp_vision_network_python_mutation_end();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(network_wlan_connect_obj, 1, network_wlan_connect);

static mp_obj_t network_wlan_disconnect(mp_obj_t self_in) {
    (void)self_in;
    if (esp_vision_network_python_mutation_begin() != ESP_OK) {
        mp_raise_OSError(MP_EBUSY);
    }
    esp_vision_network_python_set_connect_requested(false);
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        esp_vision_network_python_mutation_end();
        esp_exceptions(err);
    }
    esp_vision_network_python_mutation_end();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_wlan_disconnect_obj, network_wlan_disconnect);

static mp_obj_t network_wlan_status(size_t n_args, const mp_obj_t *args) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (n_args == 1) {
        if (self->if_id == ESP_IF_WIFI_STA) {
            esp_vision_network_snapshot_t network;
            esp_vision_network_snapshot(&network);
            // Case of no arg is only for the STA interface
            if (network.sta_connected) {
                // Happy path, connected with IP
                return MP_OBJ_NEW_SMALL_INT(STAT_GOT_IP);
            } else if (network.sta_disconnect_reason == WIFI_REASON_NO_AP_FOUND) {
                return MP_OBJ_NEW_SMALL_INT(WIFI_REASON_NO_AP_FOUND);
            } else if (network.sta_disconnect_reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD) {
                return MP_OBJ_NEW_SMALL_INT(WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD);
            } else if (network.sta_disconnect_reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD) {
                return MP_OBJ_NEW_SMALL_INT(WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD);
            } else if (network.sta_disconnect_reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY) {
                return MP_OBJ_NEW_SMALL_INT(WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY);
            } else if ((network.sta_disconnect_reason == WIFI_REASON_AUTH_FAIL) || (network.sta_disconnect_reason == WIFI_REASON_CONNECTION_FAIL)) {
                // wrong password
                return MP_OBJ_NEW_SMALL_INT(WIFI_REASON_AUTH_FAIL);
            } else if (network.sta_disconnect_reason == WIFI_REASON_ASSOC_LEAVE) {
                // After wlan.disconnect()
                return MP_OBJ_NEW_SMALL_INT(STAT_IDLE);
            } else if (network.sta_connect_requested
                       && (conf_wifi_sta_reconnects == 0
                           || network.sta_reconnect_count < conf_wifi_sta_reconnects)) {
                // No connection or error, but is requested = Still connecting
                return MP_OBJ_NEW_SMALL_INT(STAT_CONNECTING);
            } else if (network.sta_disconnect_reason == 0) {
                // No activity, No error = Idle
                return MP_OBJ_NEW_SMALL_INT(STAT_IDLE);
            } else {
                // Simply pass the error through from ESP-identifier
                return MP_OBJ_NEW_SMALL_INT(network.sta_disconnect_reason);
            }
        }
        return mp_const_none;
    }

    // one argument: return status based on query parameter
    switch ((uintptr_t)args[1]) {
        case (uintptr_t)MP_OBJ_NEW_QSTR(MP_QSTR_stations): {
            // return list of connected stations, only if in soft-AP mode
            require_if(args[0], ESP_IF_WIFI_AP);
            wifi_sta_list_t station_list;
            esp_exceptions(esp_wifi_ap_get_sta_list(&station_list));
            wifi_sta_info_t *stations = (wifi_sta_info_t *)station_list.sta;
            mp_obj_t list = mp_obj_new_list(0, NULL);
            for (int i = 0; i < station_list.num; ++i) {
                mp_obj_tuple_t *t = mp_obj_new_tuple(1, NULL);
                t->items[0] = mp_obj_new_bytes(stations[i].mac, sizeof(stations[i].mac));
                mp_obj_list_append(list, t);
            }
            return list;
        }
        case (uintptr_t)MP_OBJ_NEW_QSTR(MP_QSTR_rssi): {
            // return signal of AP, only in STA mode
            require_if(args[0], ESP_IF_WIFI_STA);

            wifi_ap_record_t info;
            esp_exceptions(esp_wifi_sta_get_ap_info(&info));
            return MP_OBJ_NEW_SMALL_INT(info.rssi);
        }
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("unknown status param"));
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(network_wlan_status_obj, 1, 2, network_wlan_status);

static mp_obj_t network_wlan_scan(mp_obj_t self_in) {
    // check that STA mode is active
    wifi_mode_t mode;
    esp_exceptions(esp_wifi_get_mode(&mode));
    if ((mode & WIFI_MODE_STA) == 0) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("STA must be active"));
    }

    mp_obj_t list = mp_obj_new_list(0, NULL);
    wifi_scan_config_t config = { 0 };
    config.show_hidden = true;
    MP_THREAD_GIL_EXIT();
    esp_err_t status = esp_wifi_scan_start(&config, 1);
    MP_THREAD_GIL_ENTER();
    if (status == 0) {
        uint16_t count = 0;
        esp_exceptions(esp_wifi_scan_get_ap_num(&count));
        if (count == 0) {
            // esp_wifi_scan_get_ap_records must be called to free internal buffers from the scan.
            // But it returns an error if wifi_ap_records==NULL.  So allocate at least 1 AP entry.
            // esp_wifi_scan_get_ap_records will then return the actual number of APs in count.
            count = 1;
        }
        wifi_ap_record_t *wifi_ap_records = calloc(count, sizeof(wifi_ap_record_t));
        esp_exceptions(esp_wifi_scan_get_ap_records(&count, wifi_ap_records));
        for (uint16_t i = 0; i < count; i++) {
            mp_obj_tuple_t *t = mp_obj_new_tuple(6, NULL);
            uint8_t *x = memchr(wifi_ap_records[i].ssid, 0, sizeof(wifi_ap_records[i].ssid));
            int ssid_len = x ? x - wifi_ap_records[i].ssid : sizeof(wifi_ap_records[i].ssid);
            t->items[0] = mp_obj_new_bytes(wifi_ap_records[i].ssid, ssid_len);
            t->items[1] = mp_obj_new_bytes(wifi_ap_records[i].bssid, sizeof(wifi_ap_records[i].bssid));
            t->items[2] = MP_OBJ_NEW_SMALL_INT(wifi_ap_records[i].primary);
            t->items[3] = MP_OBJ_NEW_SMALL_INT(wifi_ap_records[i].rssi);
            t->items[4] = MP_OBJ_NEW_SMALL_INT(wifi_ap_records[i].authmode);
            t->items[5] = mp_const_false; // XXX hidden?
            mp_obj_list_append(list, MP_OBJ_FROM_PTR(t));
        }
        free(wifi_ap_records);
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_wlan_scan_obj, network_wlan_scan);

static mp_obj_t network_wlan_isconnected(mp_obj_t self_in) {
    wlan_if_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->if_id == ESP_IF_WIFI_STA) {
        return mp_obj_new_bool(esp_vision_network_python_sta_connected());
    } else {
        wifi_sta_list_t sta;
        esp_wifi_ap_get_sta_list(&sta);
        return mp_obj_new_bool(sta.num != 0);
    }
}
static MP_DEFINE_CONST_FUN_OBJ_1(network_wlan_isconnected_obj, network_wlan_isconnected);

static mp_obj_t network_wlan_config_inner(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    if (n_args != 1 && kwargs->used != 0) {
        mp_raise_TypeError(MP_ERROR_TEXT("either pos or kw args are allowed"));
    }

    wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    bool is_wifi = self->if_id == ESP_IF_WIFI_AP || self->if_id == ESP_IF_WIFI_STA;

    wifi_config_t cfg;
    if (is_wifi) {
        esp_exceptions(esp_wifi_get_config(self->if_id, &cfg));
    }

    if (kwargs->used != 0) {
        if (!is_wifi) {
            goto unknown;
        }

        for (size_t i = 0; i < kwargs->alloc; i++) {
            if (mp_map_slot_is_filled(kwargs, i)) {
                int req_if = -1;

                switch (mp_obj_str_get_qstr(kwargs->table[i].key)) {
                    case MP_QSTR_mac: {
                        mp_buffer_info_t bufinfo;
                        mp_get_buffer_raise(kwargs->table[i].value, &bufinfo, MP_BUFFER_READ);
                        if (bufinfo.len != 6) {
                            mp_raise_ValueError(MP_ERROR_TEXT("invalid buffer length"));
                        }
                        esp_exceptions(esp_wifi_set_mac(self->if_id, bufinfo.buf));
                        break;
                    }
                    case MP_QSTR_ssid:
                    case MP_QSTR_essid: {
                        req_if = ESP_IF_WIFI_AP;
                        size_t len;
                        const char *s = mp_obj_str_get_data(kwargs->table[i].value, &len);
                        len = MIN(len, sizeof(cfg.ap.ssid));
                        memcpy(cfg.ap.ssid, s, len);
                        cfg.ap.ssid_len = len;
                        break;
                    }
                    case MP_QSTR_hidden: {
                        req_if = ESP_IF_WIFI_AP;
                        cfg.ap.ssid_hidden = mp_obj_is_true(kwargs->table[i].value);
                        break;
                    }
                    case MP_QSTR_security:
                    case MP_QSTR_authmode: {
                        req_if = ESP_IF_WIFI_AP;
                        cfg.ap.authmode = mp_obj_get_int(kwargs->table[i].value);
                        break;
                    }
                    case MP_QSTR_key:
                    case MP_QSTR_password: {
                        req_if = ESP_IF_WIFI_AP;
                        size_t len;
                        const char *s = mp_obj_str_get_data(kwargs->table[i].value, &len);
                        len = MIN(len, sizeof(cfg.ap.password) - 1);
                        memcpy(cfg.ap.password, s, len);
                        cfg.ap.password[len] = 0;
                        break;
                    }
                    case MP_QSTR_channel: {
                        uint8_t channel = mp_obj_get_int(kwargs->table[i].value);
                        if (self->if_id == ESP_IF_WIFI_AP) {
                            cfg.ap.channel = channel;
                        } else {
                            // This setting is only used to determine the
                            // starting channel for a scan, so it can result in
                            // slightly faster connection times.
                            cfg.sta.channel = channel;

                            // This additional code to directly set the channel
                            // on the STA interface is only relevant for ESP-NOW
                            // (when there is no STA connection attempt.)
                            uint8_t old_primary;
                            wifi_second_chan_t secondary;
                            // Get the current value of secondary
                            esp_exceptions(esp_wifi_get_channel(&old_primary, &secondary));
                            esp_err_t err = esp_wifi_set_channel(channel, secondary);
                            if (err == ESP_ERR_INVALID_ARG) {
                                // May need to swap secondary channel above to below or below to above
                                secondary = (
                                    (secondary == WIFI_SECOND_CHAN_ABOVE)
                                    ? WIFI_SECOND_CHAN_BELOW
                                    : (secondary == WIFI_SECOND_CHAN_BELOW)
                                    ? WIFI_SECOND_CHAN_ABOVE
                                    : WIFI_SECOND_CHAN_NONE);
                                err = esp_wifi_set_channel(channel, secondary);
                            }
                            esp_exceptions(err);
                            if (channel != old_primary) {
                                // Workaround the ESP-IDF Wi-Fi stack sometimes taking a moment to change channels
                                mp_hal_delay_ms(1);
                            }
                        }
                        break;
                    }
                    case MP_QSTR_hostname:
                    case MP_QSTR_dhcp_hostname: {
                        // TODO: Deprecated. Use network.hostname(name) instead.
                        mod_network_hostname(1, &kwargs->table[i].value);
                        break;
                    }
                    case MP_QSTR_max_clients: {
                        req_if = ESP_IF_WIFI_AP;
                        cfg.ap.max_connection = mp_obj_get_int(kwargs->table[i].value);
                        break;
                    }
                    case MP_QSTR_reconnects: {
                        int reconnects = mp_obj_get_int(kwargs->table[i].value);
                        req_if = ESP_IF_WIFI_STA;
                        // parameter reconnects == -1 means to retry forever.
                        // here means conf_wifi_sta_reconnects == 0 to retry forever.
                        conf_wifi_sta_reconnects = (reconnects == -1) ? 0 : reconnects + 1;
                        esp_vision_network_python_set_reconnect_limit(conf_wifi_sta_reconnects);
                        break;
                    }
                    case MP_QSTR_txpower: {
                        int8_t power = (mp_obj_get_float(kwargs->table[i].value) * 4);
                        esp_exceptions(esp_wifi_set_max_tx_power(power));
                        break;
                    }
                    case MP_QSTR_protocol: {
                        esp_exceptions(esp_wifi_set_protocol(self->if_id, mp_obj_get_int(kwargs->table[i].value)));
                        break;
                    }
                    case MP_QSTR_pm: {
                        esp_exceptions(esp_wifi_set_ps(mp_obj_get_int(kwargs->table[i].value)));
                        break;
                    }
                    default:
                        goto unknown;
                }

                // We post-check interface requirements to save on code size
                if (req_if >= 0) {
                    require_if(args[0], req_if);
                }
            }
        }

        esp_exceptions(esp_wifi_set_config(self->if_id, &cfg));

        return mp_const_none;
    }

    // Get config

    if (n_args != 2) {
        mp_raise_TypeError(MP_ERROR_TEXT("can query only one param"));
    }

    int req_if = -1;
    mp_obj_t val = mp_const_none;

    switch (mp_obj_str_get_qstr(args[1])) {
        case MP_QSTR_mac: {
            uint8_t mac[6];
            switch (self->if_id) {
                case ESP_IF_WIFI_AP: // fallthrough intentional
                case ESP_IF_WIFI_STA:
                    esp_exceptions(esp_wifi_get_mac(self->if_id, mac));
                    return mp_obj_new_bytes(mac, sizeof(mac));
                default:
                    goto unknown;
            }
        }
        case MP_QSTR_ssid:
        case MP_QSTR_essid:
            switch (self->if_id) {
                case ESP_IF_WIFI_STA:
                    val = mp_obj_new_str_from_cstr((char *)cfg.sta.ssid);
                    break;
                case ESP_IF_WIFI_AP:
                    val = mp_obj_new_str((char *)cfg.ap.ssid, cfg.ap.ssid_len);
                    break;
                default:
                    req_if = ESP_IF_WIFI_AP;
            }
            break;
        case MP_QSTR_hidden:
            req_if = ESP_IF_WIFI_AP;
            val = mp_obj_new_bool(cfg.ap.ssid_hidden);
            break;
        case MP_QSTR_security:
        case MP_QSTR_authmode:
            req_if = ESP_IF_WIFI_AP;
            val = MP_OBJ_NEW_SMALL_INT(cfg.ap.authmode);
            break;
        case MP_QSTR_channel: {
            uint8_t channel;
            wifi_second_chan_t second;
            esp_exceptions(esp_wifi_get_channel(&channel, &second));
            val = MP_OBJ_NEW_SMALL_INT(channel);
            break;
        }
        case MP_QSTR_ifname: {
            val = esp_ifname(self->netif);
            break;
        }
        case MP_QSTR_hostname:
        case MP_QSTR_dhcp_hostname: {
            // TODO: Deprecated. Use network.hostname() instead.
            req_if = ESP_IF_WIFI_STA;
            val = mod_network_hostname(0, NULL);
            break;
        }
        case MP_QSTR_max_clients: {
            val = MP_OBJ_NEW_SMALL_INT(cfg.ap.max_connection);
            break;
        }
        case MP_QSTR_reconnects:
            req_if = ESP_IF_WIFI_STA;
            int rec = conf_wifi_sta_reconnects - 1;
            val = MP_OBJ_NEW_SMALL_INT(rec);
            break;
        case MP_QSTR_txpower: {
            int8_t power;
            esp_exceptions(esp_wifi_get_max_tx_power(&power));
            val = mp_obj_new_float(power * 0.25);
            break;
        }
        case MP_QSTR_protocol: {
            uint8_t protocol_bitmap;
            esp_exceptions(esp_wifi_get_protocol(self->if_id, &protocol_bitmap));
            val = MP_OBJ_NEW_SMALL_INT(protocol_bitmap);
            break;
        }
        case MP_QSTR_pm: {
            wifi_ps_type_t ps_type;
            esp_exceptions(esp_wifi_get_ps(&ps_type));
            val = MP_OBJ_NEW_SMALL_INT(ps_type);
            break;
        }
        default:
            goto unknown;
    }

    // We post-check interface requirements to save on code size
    if (req_if >= 0) {
        require_if(args[0], req_if);
    }

    return val;

unknown:
    mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
}

static mp_obj_t network_wlan_config(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    if (kwargs->used == 0) {
        return network_wlan_config_inner(n_args, args, kwargs);
    }
    if (esp_vision_network_python_mutation_begin() != ESP_OK) {
        mp_raise_OSError(MP_EBUSY);
    }
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t result = network_wlan_config_inner(n_args, args, kwargs);
        nlr_pop();
        wlan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);
        if (self->if_id == ESP_IF_WIFI_STA) {
            esp_vision_network_python_runtime_changed();
            esp_vision_network_python_set_reconnect_limit(conf_wifi_sta_reconnects);
        }
        esp_vision_network_python_mutation_end();
        return result;
    }
    mp_obj_t exception = nlr.ret_val;
    nlr_pop();
    esp_vision_network_python_mutation_end();
    nlr_raise(exception);
}
MP_DEFINE_CONST_FUN_OBJ_KW(network_wlan_config_obj, 1, network_wlan_config);

static const mp_rom_map_elem_t wlan_if_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&network_wlan_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&network_wlan_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&network_wlan_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&network_wlan_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_scan), MP_ROM_PTR(&network_wlan_scan_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&network_wlan_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_config), MP_ROM_PTR(&network_wlan_config_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig), MP_ROM_PTR(&esp_network_ifconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipconfig), MP_ROM_PTR(&esp_nic_ipconfig_obj) },

    // Constants
    { MP_ROM_QSTR(MP_QSTR_IF_STA), MP_ROM_INT(WIFI_IF_STA)},
    { MP_ROM_QSTR(MP_QSTR_IF_AP), MP_ROM_INT(WIFI_IF_AP)},

    { MP_ROM_QSTR(MP_QSTR_SEC_OPEN), MP_ROM_INT(WIFI_AUTH_OPEN) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WEP), MP_ROM_INT(WIFI_AUTH_WEP) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA), MP_ROM_INT(WIFI_AUTH_WPA_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA2), MP_ROM_INT(WIFI_AUTH_WPA2_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA_WPA2), MP_ROM_INT(WIFI_AUTH_WPA_WPA2_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA2_ENT), MP_ROM_INT(WIFI_AUTH_WPA2_ENTERPRISE) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA3), MP_ROM_INT(WIFI_AUTH_WPA3_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA2_WPA3), MP_ROM_INT(WIFI_AUTH_WPA2_WPA3_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WAPI), MP_ROM_INT(WIFI_AUTH_WAPI_PSK) },
    { MP_ROM_QSTR(MP_QSTR_SEC_OWE), MP_ROM_INT(WIFI_AUTH_OWE) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA3_ENT_192), MP_ROM_INT(WIFI_AUTH_WPA3_ENT_192) },
    { MP_ROM_QSTR(MP_QSTR_SEC_DPP), MP_ROM_INT(WIFI_AUTH_DPP) },
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA3_ENT), MP_ROM_INT(WIFI_AUTH_WPA3_ENTERPRISE) },
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA2_WPA3_ENT), MP_ROM_INT(WIFI_AUTH_WPA2_WPA3_ENTERPRISE) },
    #endif
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
    { MP_ROM_QSTR(MP_QSTR_SEC_WPA_ENT), MP_ROM_INT(WIFI_AUTH_WPA_ENTERPRISE) },
    #endif
    #if ESP_VISION_WIFI_AUTH_UNKNOWN_SUPPORTED
    { MP_ROM_QSTR(MP_QSTR_SEC_UNKNOWN), MP_ROM_INT(WIFI_AUTH_UNKNOWN) },
    #endif

    { MP_ROM_QSTR(MP_QSTR_PROTOCOL_DEFAULT), MP_ROM_INT(WIFI_PROTOCOL_DEFAULT) },
    #if !CONFIG_IDF_TARGET_ESP32C2
    { MP_ROM_QSTR(MP_QSTR_PROTOCOL_LR), MP_ROM_INT(WIFI_PROTOCOL_LR) },
    #endif

    { MP_ROM_QSTR(MP_QSTR_PM_NONE), MP_ROM_INT(WIFI_PS_NONE) },
    { MP_ROM_QSTR(MP_QSTR_PM_PERFORMANCE), MP_ROM_INT(WIFI_PS_MIN_MODEM) },
    { MP_ROM_QSTR(MP_QSTR_PM_POWERSAVE), MP_ROM_INT(WIFI_PS_MAX_MODEM) },
};
static MP_DEFINE_CONST_DICT(wlan_if_locals_dict, wlan_if_locals_dict_table);

#if ESP_VISION_WIFI_AUTH_UNKNOWN_SUPPORTED
_Static_assert(WIFI_AUTH_MAX == 18, "Synchronize WIFI_AUTH_XXX constants with the ESP-IDF Wi-Fi API");
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
_Static_assert(WIFI_AUTH_MAX == 17, "Synchronize WIFI_AUTH_XXX constants with the ESP-IDF Wi-Fi API");
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
// ESP-IDF release/v5.5 and the stable v5.5.x tags carry different snapshots
// of the generic Wi-Fi enum: the former includes WIFI_AUTH_UNKNOWN while the
// latter does not. Accept both layouts without changing the managed component.
_Static_assert((WIFI_AUTH_MAX == 17) || (WIFI_AUTH_MAX == 18), "Synchronize WIFI_AUTH_XXX constants with the ESP-IDF Wi-Fi API");
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
_Static_assert(WIFI_AUTH_MAX == 16, "Synchronize WIFI_AUTH_XXX constants with the ESP-IDF. Look at esp-idf/components/esp_wifi/include/esp_wifi_types_generic.h");
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
_Static_assert(WIFI_AUTH_MAX == 14, "Synchronize WIFI_AUTH_XXX constants with the ESP-IDF. Look at esp-idf/components/esp_wifi/include/esp_wifi_types_generic.h");
#else
#error "Error in macro logic, all supported versions should be covered."
#endif

MP_DEFINE_CONST_OBJ_TYPE(
    esp_network_wlan_type,
    MP_QSTR_WLAN,
    MP_TYPE_FLAG_NONE,
    make_new, network_wlan_make_new,
    locals_dict, &wlan_if_locals_dict
    );

#endif // MICROPY_PY_NETWORK_WLAN
