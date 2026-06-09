# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

add_doc_server_ssh_keys() {
    local key_string="$1"
    local server_url="$2"
    local server_user="$3"

    mkdir -p ~/.ssh
    chmod 700 ~/.ssh
    printf '%s' "$key_string" | base64 --decode --ignore-garbage > ~/.ssh/id_rsa
    chmod 600 ~/.ssh/id_rsa
    printf 'Host %s\n\tStrictHostKeyChecking no\n\tUser %s\n' \
        "$server_url" "$server_user" >> ~/.ssh/config
}
