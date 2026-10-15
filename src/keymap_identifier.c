/*
 * Copyright (c) 2026 cormoran
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include "keymap_identifier.h"

#define FK_KEYMAP_IDENTIFIER_SETTING "fast_keymap/identifier"

static char identifier[FK_KEYMAP_IDENTIFIER_LENGTH + 1];

static bool valid_identifier(const char *value, size_t len) {
    if (len == 0) {
        return true;
    }
    if (len != FK_KEYMAP_IDENTIFIER_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'A' && value[i] <= 'Z') ||
              (value[i] >= 'a' && value[i] <= 'z') || value[i] == '-' || value[i] == '_')) {
            return false;
        }
    }
    return true;
}

const char *fk_keymap_identifier_get(void) { return identifier; }

int fk_keymap_identifier_set(const char *value) {
    size_t len = 0;
    while (len <= FK_KEYMAP_IDENTIFIER_LENGTH && value[len] != '\0') {
        len++;
    }
    if (!valid_identifier(value, len)) {
        return -EINVAL;
    }

#if IS_ENABLED(CONFIG_SETTINGS) && !IS_ENABLED(CONFIG_SETTINGS_NONE)
    int err = len == 0 ? settings_delete(FK_KEYMAP_IDENTIFIER_SETTING)
                       : settings_save_one(FK_KEYMAP_IDENTIFIER_SETTING, value, len);
    if (err != 0) {
        return err;
    }
    memcpy(identifier, value, len);
    identifier[len] = '\0';
    return 0;
#else
    return -ENOTSUP;
#endif
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int fk_keymap_identifier_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                             void *cb_arg) {
    const char *next;
    if (!settings_name_steq(name, "identifier", &next) || next) {
        return 0;
    }
    if (len != 0 && len != FK_KEYMAP_IDENTIFIER_LENGTH) {
        return -EINVAL;
    }
    char loaded[FK_KEYMAP_IDENTIFIER_LENGTH + 1] = {0};
    if (len > 0) {
        int read = read_cb(cb_arg, loaded, len);
        if (read < 0) {
            return read;
        }
        if (read != len || !valid_identifier(loaded, len)) {
            return -EINVAL;
        }
    }
    memcpy(identifier, loaded, sizeof(identifier));
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(fast_keymap_identifier, "fast_keymap", NULL,
                               fk_keymap_identifier_settings_set, NULL, NULL);
#endif
