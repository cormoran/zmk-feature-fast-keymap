/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include "fingerprint.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/physical_layouts.h>

#include "behavior_alias.h"

/* -- little-endian byte helpers (DESIGN.md SS4/SS5 canonical primitives) -- */

static uint32_t crc_u8(uint32_t seed, uint8_t v) { return crc32_ieee_update(seed, &v, 1); }

static uint32_t crc_u16le(uint32_t seed, uint16_t v) {
    uint8_t buf[2] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF)};
    return crc32_ieee_update(seed, buf, sizeof(buf));
}

static uint32_t crc_u32le(uint32_t seed, uint32_t v) {
    uint8_t buf[4] = {
        (uint8_t)(v & 0xFF),
        (uint8_t)((v >> 8) & 0xFF),
        (uint8_t)((v >> 16) & 0xFF),
        (uint8_t)((v >> 24) & 0xFF),
    };
    return crc32_ieee_update(seed, buf, sizeof(buf));
}

static uint32_t crc_i32le(uint32_t seed, int32_t v) { return crc_u32le(seed, (uint32_t)v); }

static uint32_t crc_i16le(uint32_t seed, int16_t v) { return crc_u16le(seed, (uint16_t)v); }

/* `u16 length` prefix + raw bytes, used for behavior alias/display_name and
 * layer/physical-layout names (SS4). NOT used for behavior parameter value
 * display names inside metadata_fp -- SS5 freezes those with a `u8` length
 * prefix instead; see crc_str_u8len() below. */
static uint32_t crc_str_u16len(uint32_t seed, const char *s) {
    size_t len = s ? strlen(s) : 0;
    seed = crc_u16le(seed, (uint16_t)len);
    if (len) {
        seed = crc32_ieee_update(seed, (const uint8_t *)s, len);
    }
    return seed;
}

/* SS5's `u8 name_len ; name bytes` -- only used inside metadata_fp. */
static uint32_t crc_str_u8len(uint32_t seed, const char *s) {
    size_t len = s ? strlen(s) : 0;
    seed = crc_u8(seed, (uint8_t)len);
    if (len) {
        seed = crc32_ieee_update(seed, (const uint8_t *)s, len);
    }
    return seed;
}

uint32_t fk_crc_binding(uint32_t seed, const struct zmk_behavior_binding *binding) {
    int32_t local_id = -1;
    uint32_t param1 = 0;
    uint32_t param2 = 0;

    if (binding && binding->behavior_dev) {
        local_id = (int32_t)zmk_behavior_get_local_id(binding->behavior_dev);
        param1 = binding->param1;
        param2 = binding->param2;
    }

    seed = crc_i32le(seed, local_id);
    seed = crc_u32le(seed, param1);
    seed = crc_u32le(seed, param2);
    return seed;
}

uint16_t fk_binding_count(void) {
    uint16_t n = 0;
    while (n < ZMK_KEYMAP_LEN && zmk_keymap_get_layer_binding_at_idx(0, n) != NULL) {
        n++;
    }
    return n;
}

uint8_t fk_layer_count(void) {
    for (zmk_keymap_layer_index_t index = 0; index < ZMK_KEYMAP_LAYERS_LEN; index++) {
        if (zmk_keymap_layer_index_to_id(index) == ZMK_KEYMAP_LAYER_ID_INVAL) {
            return index;
        }
    }
    return ZMK_KEYMAP_LAYERS_LEN;
}

uint32_t fk_available_layers(void) {
    for (zmk_keymap_layer_index_t index = 0; index < ZMK_KEYMAP_LAYERS_LEN; index++) {
        if (zmk_keymap_layer_index_to_id(index) == ZMK_KEYMAP_LAYER_ID_INVAL) {
            return ZMK_KEYMAP_LAYERS_LEN - index;
        }
    }
    return 0;
}

uint32_t fk_behavior_count(void) {
    uint32_t count = 0;
    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, beh) {
        ARG_UNUSED(beh);
        count++;
    }
    return count;
}

/* Canonical metadata bytes (DESIGN.md SS5), shared by fk_fp_behavior_metadata()
 * (a standalone fp) and fk_fp_behaviors() (embedded per-behavior). */
static uint32_t crc_metadata(uint32_t seed, const struct behavior_parameter_metadata *meta) {
    size_t sets_len = meta ? meta->sets_len : 0;
    seed = crc_u8(seed, (uint8_t)sets_len);

    for (size_t i = 0; i < sets_len; i++) {
        const struct behavior_parameter_metadata_set *set = &meta->sets[i];
        seed = crc_u8(seed, (uint8_t)set->param1_values_len);
        seed = crc_u8(seed, (uint8_t)set->param2_values_len);

        for (int pass = 0; pass < 2; pass++) {
            size_t values_len = pass == 0 ? set->param1_values_len : set->param2_values_len;
            const struct behavior_parameter_value_metadata *values =
                pass == 0 ? set->param1_values : set->param2_values;

            for (size_t v = 0; v < values_len; v++) {
                const struct behavior_parameter_value_metadata *val = &values[v];
                uint8_t value_type = 0;
                uint32_t a = 0, b = 0;

                switch (val->type) {
                case BEHAVIOR_PARAMETER_VALUE_TYPE_NIL:
                    value_type = 0;
                    break;
                case BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE:
                    value_type = 1;
                    a = val->value;
                    break;
                case BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE:
                    value_type = 2;
                    a = (uint32_t)val->range.min;
                    b = (uint32_t)val->range.max;
                    break;
                case BEHAVIOR_PARAMETER_VALUE_TYPE_HID_USAGE:
                    value_type = 3;
                    a = ZMK_HID_KEYBOARD_MAX_USAGE;
                    b = ZMK_HID_CONSUMER_MAX_USAGE;
                    break;
                case BEHAVIOR_PARAMETER_VALUE_TYPE_LAYER_ID:
                    value_type = 4;
                    break;
                default:
                    break;
                }

                seed = crc_u8(seed, value_type);
                seed = crc_u32le(seed, a);
                seed = crc_u32le(seed, b);
                seed = crc_str_u8len(seed, val->display_name);
            }
        }
    }

    return seed;
}

static int get_metadata(const struct device *dev, struct behavior_parameter_metadata *out) {
    *out = (struct behavior_parameter_metadata){0};
    if (!dev) {
        return -ENODEV;
    }
    return behavior_get_parameter_metadata(dev, out);
}

uint32_t fk_fp_behavior_metadata(const struct device *dev) {
    struct behavior_parameter_metadata meta;
    /* A behavior without metadata support (or an error fetching it) hashes
     * as "0 sets" -- deterministic and distinct from any real result. */
    get_metadata(dev, &meta);
    return crc_metadata(0, &meta);
}

uint32_t fk_fp_behaviors(void) {
    uint32_t seed = 0;

    /* zmk_behavior_local_id_map entries are populated in devicetree order,
     * not ascending local_id order. Behavior counts are small (tens), so a
     * simple O(n^2) repeated-selection walk in ascending local_id order
     * avoids needing a variable-length/dynamic scratch array. */
    int32_t last_id = -1;
    for (;;) {
        const struct device *best_dev = NULL;
        int32_t best_id = INT32_MAX;

        STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, beh) {
            int32_t id = beh->local_id;
            if (id > last_id && id < best_id) {
                best_id = id;
                best_dev = beh->device;
            }
        }

        if (!best_dev) {
            break;
        }

        seed = crc_u16le(seed, (uint16_t)best_id);

        struct behavior_parameter_metadata meta;
        get_metadata(best_dev, &meta);
        seed = crc_metadata(seed, &meta);

        seed = crc_str_u16len(seed, fk_behavior_alias(best_dev));
        seed = crc_str_u16len(seed, fk_behavior_display_name(best_dev));

        last_id = best_id;
    }

    return seed;
}

uint32_t fk_fp_default_keymap(void) {
    uint32_t seed = 0;
    uint16_t binding_count = fk_binding_count();
    uint8_t layer_count = fk_layer_count();

    for (uint8_t idx = 0; idx < layer_count; idx++) {
        zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(idx);
        seed = crc_u8(seed, layer_id);

        for (uint16_t b = 0; b < binding_count; b++) {
#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)
            const struct zmk_behavior_binding *binding =
                zmk_stock_keymap_get_layer_binding_at_idx(layer_id, b);
#else
            const struct zmk_behavior_binding *binding =
                zmk_keymap_get_layer_binding_at_idx(layer_id, b);
#endif
            seed = fk_crc_binding(seed, binding);
        }
    }

    return seed;
}

uint32_t fk_fp_keymap(void) {
    uint32_t seed = 0;
    uint16_t binding_count = fk_binding_count();
    uint8_t layer_count = fk_layer_count();

    seed = crc_u8(seed, (uint8_t)zmk_physical_layouts_get_selected());

    for (uint8_t idx = 0; idx < layer_count; idx++) {
        zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(idx);
        seed = crc_u8(seed, layer_id);
        seed = crc_str_u16len(seed, zmk_keymap_layer_name(layer_id));

        for (uint16_t b = 0; b < binding_count; b++) {
            const struct zmk_behavior_binding *binding =
                zmk_keymap_get_layer_binding_at_idx(layer_id, b);
            seed = fk_crc_binding(seed, binding);
        }
    }

    return seed;
}

uint32_t fk_fp_physical_layout(void) {
    uint32_t seed = 0;
    seed = crc_u8(seed, (uint8_t)zmk_physical_layouts_get_selected());

    struct zmk_physical_layout const *const *layouts;
    size_t layout_count = zmk_physical_layouts_get_list(&layouts);

    for (size_t i = 0; i < layout_count; i++) {
        const struct zmk_physical_layout *l = layouts[i];
        seed = crc_str_u16len(seed, l->display_name);
        seed = crc_u16le(seed, (uint16_t)l->keys_len);

        for (size_t k = 0; k < l->keys_len; k++) {
            const struct zmk_key_physical_attrs *attrs = &l->keys[k];
            seed = crc_i16le(seed, attrs->width);
            seed = crc_i16le(seed, attrs->height);
            seed = crc_i16le(seed, attrs->x);
            seed = crc_i16le(seed, attrs->y);
#if IS_ENABLED(CONFIG_ZMK_PHYSICAL_LAYOUT_KEY_ROTATION)
            seed = crc_i16le(seed, attrs->r);
            seed = crc_i16le(seed, attrs->rx);
            seed = crc_i16le(seed, attrs->ry);
#else
            seed = crc_i16le(seed, 0);
            seed = crc_i16le(seed, 0);
            seed = crc_i16le(seed, 0);
#endif
        }
    }

    return seed;
}

uint32_t fk_fp_layer_current(zmk_keymap_layer_id_t layer_id) {
    uint32_t seed = 0;
    uint16_t binding_count = fk_binding_count();

    for (uint16_t b = 0; b < binding_count; b++) {
        const struct zmk_behavior_binding *binding =
            zmk_keymap_get_layer_binding_at_idx(layer_id, b);
        seed = fk_crc_binding(seed, binding);
    }

    return seed;
}

uint32_t fk_fp_layer_default(zmk_keymap_layer_id_t layer_id) {
#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)
    uint32_t seed = 0;
    uint16_t binding_count = fk_binding_count();

    for (uint16_t b = 0; b < binding_count; b++) {
        const struct zmk_behavior_binding *binding =
            zmk_stock_keymap_get_layer_binding_at_idx(layer_id, b);
        seed = fk_crc_binding(seed, binding);
    }

    return seed;
#else
    return fk_fp_layer_current(layer_id);
#endif
}

bool fk_binding_is_edited(zmk_keymap_layer_id_t layer_id, uint16_t binding_idx) {
#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)
    const struct zmk_behavior_binding *current =
        zmk_keymap_get_layer_binding_at_idx(layer_id, binding_idx);
    const struct zmk_behavior_binding *def =
        zmk_stock_keymap_get_layer_binding_at_idx(layer_id, binding_idx);

    uint32_t current_crc = fk_crc_binding(0, current);
    uint32_t default_crc = fk_crc_binding(0, def);
    return current_crc != default_crc;
#else
    ARG_UNUSED(layer_id);
    ARG_UNUSED(binding_idx);
    return false;
#endif
}

uint16_t fk_layer_edited_count(zmk_keymap_layer_id_t layer_id) {
#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)
    uint16_t binding_count = fk_binding_count();
    uint16_t edited = 0;
    for (uint16_t b = 0; b < binding_count; b++) {
        if (fk_binding_is_edited(layer_id, b)) {
            edited++;
        }
    }
    return edited;
#else
    ARG_UNUSED(layer_id);
    return 0;
#endif
}
