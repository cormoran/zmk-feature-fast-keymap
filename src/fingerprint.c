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

/* Every fingerprint this module serves is truncated to its low 16 bits before
 * it leaves fingerprint.c (wire, caching, and internal chaining all see the
 * same 16-bit value), so the `uint32` fp fields on the wire encode as <=3-byte
 * varints instead of ~5. crc32's low 16 bits are as uniformly distributed as
 * any 16-bit hash, so this is just a narrower cache key -- see DESIGN.md SS4
 * for the (tiny, cache-key-only) collision tradeoff this trades bytes for.
 * Mirrored on the web by masking def_fp the same way (fingerprint.ts). */
static uint32_t fk_fp16(uint32_t crc) { return crc & 0xFFFFu; }

/* `u16 length` prefix + raw bytes, used for behavior alias/display_name and
 * layer/physical-layout names (SS4). NOT used for behavior parameter value
 * display names inside the metadata bytes (def_fp's preimage) -- SS5 freezes
 * those with a `u8` length prefix instead; see crc_str_u8len() below. */
static uint32_t crc_str_u16len(uint32_t seed, const char *s) {
    size_t len = s ? strlen(s) : 0;
    seed = crc_u16le(seed, (uint16_t)len);
    if (len) {
        seed = crc32_ieee_update(seed, (const uint8_t *)s, len);
    }
    return seed;
}

/* SS5's `u8 name_len ; name bytes` -- only used inside the metadata bytes
 * (def_fp's preimage). */
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

/* Canonical metadata bytes (DESIGN.md SS5), shared by fk_fp_behavior_def()
 * (a standalone fp, via crc_behavior_def_preimage() below) and
 * fk_fp_behaviors() (embedded per-behavior). */
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

/* def_fp's preimage (DESIGN.md SS4/SS5): metadata bytes + str16 alias +
 * str16 display_name, both AS-SERVED (fk_behavior_served_alias() /
 * fk_behavior_display_name()) -- the exact strings BehaviorDetails puts on
 * the wire. Shared by fk_fp_behavior_def() (seed 0) and fk_fp_behaviors()
 * (chained after each behavior's u16 local_id) so the two can never
 * disagree (DESIGN.md SS4's behaviors_fp definition). */
static uint32_t crc_behavior_def_preimage(uint32_t seed, const struct device *dev) {
    struct behavior_parameter_metadata meta;
    /* A behavior without metadata support (or an error fetching it) hashes
     * as "0 sets" -- deterministic and distinct from any real result. */
    get_metadata(dev, &meta);
    seed = crc_metadata(seed, &meta);
    seed = crc_str_u16len(seed, fk_behavior_served_alias(dev));
    seed = crc_str_u16len(seed, fk_behavior_display_name(dev));
    return seed;
}

/* -- def_fp cache (memoization) --
 *
 * def_fp depends only on a behavior's metadata + alias + display_name, all
 * compile-time constant -- it does NOT depend on local_id or the active
 * physical layout. It is therefore immutable for the lifetime of the
 * process and safe to cache forever, keyed by the behavior's `const struct
 * device *` (stable for the process lifetime). Behavior counts are small
 * (tens), so a small fixed-cap array with linear lookup/insert is simpler
 * than a hash table; if the cap is somehow exceeded we just fall back to
 * computing without caching that entry (still correct, only slower). No
 * locking needed: Studio RPCs are handled serially on one thread.
 *
 * Gated by CONFIG_ZMK_FAST_KEYMAP_FP_CACHE (default y); when off, the cache
 * state below doesn't exist and fk_fp_behavior_def() always recomputes --
 * same fp value either way. */
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
#define FK_DEF_FP_CACHE_CAP 96

struct fk_def_fp_cache_entry {
    const struct device *dev;
    uint32_t fp;
};

static struct fk_def_fp_cache_entry fk_def_fp_cache[FK_DEF_FP_CACHE_CAP];
static size_t fk_def_fp_cache_len;

static bool fk_def_fp_cache_lookup(const struct device *dev, uint32_t *out) {
    for (size_t i = 0; i < fk_def_fp_cache_len; i++) {
        if (fk_def_fp_cache[i].dev == dev) {
            *out = fk_def_fp_cache[i].fp;
            return true;
        }
    }
    return false;
}

static void fk_def_fp_cache_insert(const struct device *dev, uint32_t fp) {
    if (fk_def_fp_cache_len < FK_DEF_FP_CACHE_CAP) {
        fk_def_fp_cache[fk_def_fp_cache_len++] =
            (struct fk_def_fp_cache_entry){.dev = dev, .fp = fp};
    }
    /* Cap exceeded (shouldn't happen on any real keyboard): silently skip
     * caching -- the caller already has the freshly computed value. */
}
#endif /* CONFIG_ZMK_FAST_KEYMAP_FP_CACHE */

uint32_t fk_fp_behavior_def(const struct device *dev) {
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
    uint32_t cached;
    if (fk_def_fp_cache_lookup(dev, &cached)) {
        return cached;
    }
#endif

    uint32_t fp = fk_fp16(crc_behavior_def_preimage(0, dev));

#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
    fk_def_fp_cache_insert(dev, fp);
#endif

    return fp;
}

/* -- behaviors_fp cache --
 *
 * REDEFINED vs the raw-preimage-concatenation described historically: now
 * crc32 over, per behavior in ascending local_id order, `u16le local_id` +
 * `u32le def_fp` (the CACHED fk_fp_behavior_def() value), instead of
 * `u16le local_id` + the full def_fp preimage bytes. This composes from the
 * already-cached per-behavior def_fp, so crc_behavior_def_preimage() runs at
 * most once per behavior across the whole initial load (shared with
 * list_behaviors' per-behavior def_fp).
 *
 * This redefinition is safe: behaviors_fp is an opaque, device-computed
 * cache key that the web only ever *compares* (`cached.fp ===
 * snapshot.behaviorsFp`), never reproduces locally -- unlike def_fp, whose
 * exact canonicalization the web DOES reimplement (see
 * crc_behavior_def_preimage() above, unchanged). So behaviors_fp's bytes are
 * firmware-internal and this change needs no FK_DEFINITION_VERSION bump
 * (that version gates def_fp/metadata canonicalization only). See
 * DESIGN.md SS4 and docs/protocol.md for the updated definition.
 *
 * local_ids are fixed once "behavior" settings are loaded/committed, which
 * always precedes any RPC (see fast_keymap_rpc_test.c's
 * fast_keymap_rpc_test_init() for the same ordering requirement on the test
 * side) -- so once computed, the aggregate is immutable and cached forever,
 * same as the per-behavior def_fps it's built from.
 *
 * Gated by CONFIG_ZMK_FAST_KEYMAP_FP_CACHE (default y); when off, the
 * aggregate is always recomputed by the ascending-local_id loop below (which
 * still calls fk_fp_behavior_def(), itself uncached in that case) -- same fp
 * value either way. */
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
static bool fk_behaviors_fp_valid;
static uint32_t fk_behaviors_fp_cache;
#endif

uint32_t fk_fp_behaviors(void) {
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
    if (fk_behaviors_fp_valid) {
        return fk_behaviors_fp_cache;
    }
#endif

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
        seed = crc_u32le(seed, fk_fp_behavior_def(best_dev));

        last_id = best_id;
    }

    seed = fk_fp16(seed);

#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
    fk_behaviors_fp_cache = seed;
    fk_behaviors_fp_valid = true;
#endif
    return seed;
}

#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER) && IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
/* -- default-keymap fp cache (default_keymap_fp + per-layer default_fp) --
 *
 * Both fk_fp_default_keymap() and fk_fp_layer_default() read
 * zmk_stock_keymap_get_layer_binding_at_idx(), which applies the SELECTED
 * physical layout's position map -- so unlike def_fp/behaviors_fp above,
 * these are only immutable as long as the active physical layout doesn't
 * change. Guard the caches on zmk_physical_layouts_get_selected(): if it
 * differs from the index that was active when the caches were last filled,
 * invalidate (clear) them and let the next call recompute. Layout switches
 * are rare, so this only recomputes then.
 *
 * Gated additionally by CONFIG_ZMK_FAST_KEYMAP_FP_CACHE (default y); when
 * off, this cache machinery doesn't exist and both functions always
 * recompute from zmk_stock_keymap_get_layer_binding_at_idx() -- same fp
 * value either way. The compute path itself stays gated on
 * CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER alone (see below). */
static bool fk_default_cache_layout_known;
static int fk_default_cache_layout_index;

static bool fk_default_keymap_fp_valid;
static uint32_t fk_default_keymap_fp_cache;

static bool fk_layer_default_fp_valid[ZMK_KEYMAP_LAYERS_LEN];
static uint32_t fk_layer_default_fp_cache[ZMK_KEYMAP_LAYERS_LEN];

/* Call at the start of every default-keymap-cache-reading function, before
 * touching any of the caches above. */
static void fk_default_cache_sync_layout(void) {
    int current = zmk_physical_layouts_get_selected();

    if (fk_default_cache_layout_known && fk_default_cache_layout_index == current) {
        return;
    }

    fk_default_keymap_fp_valid = false;
    memset(fk_layer_default_fp_valid, 0, sizeof(fk_layer_default_fp_valid));

    fk_default_cache_layout_index = current;
    fk_default_cache_layout_known = true;
}
#endif

uint32_t fk_fp_default_keymap(void) {
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER) && IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
    fk_default_cache_sync_layout();
    if (fk_default_keymap_fp_valid) {
        return fk_default_keymap_fp_cache;
    }
#endif

    uint32_t seed = 0;
    uint16_t binding_count = fk_binding_count();
    uint8_t layer_count = fk_layer_count();

    for (uint8_t idx = 0; idx < layer_count; idx++) {
        zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(idx);
        seed = crc_u8(seed, layer_id);

        for (uint16_t b = 0; b < binding_count; b++) {
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER)
            const struct zmk_behavior_binding *binding =
                zmk_stock_keymap_get_layer_binding_at_idx(layer_id, b);
#else
            const struct zmk_behavior_binding *binding =
                zmk_keymap_get_layer_binding_at_idx(layer_id, b);
#endif
            seed = fk_crc_binding(seed, binding);
        }
    }

    seed = fk_fp16(seed);

#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER) && IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
    fk_default_keymap_fp_cache = seed;
    fk_default_keymap_fp_valid = true;
#endif

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

    return fk_fp16(seed);
}

/* Per-layout physical fp preimage (DESIGN.md SS4/SS6): one layout's `str16
 * name` + `u16 key_count` + per key `i16 width,height,x,y,r,rx,ry` (rotation
 * fields 0 when CONFIG_ZMK_PHYSICAL_LAYOUT_KEY_ROTATION is off). Shared by
 * fk_fp_physical_layout_one() (seed 0) and fk_fp_physical_layout() (chained
 * per layout after the active-index prefix) so the two can never disagree,
 * same pattern as crc_behavior_def_preimage()/fk_fp_behaviors() above. */
static uint32_t crc_physical_layout_preimage(uint32_t seed, const struct zmk_physical_layout *l) {
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

    return seed;
}

/* -- per-layout physical fp cache --
 *
 * A layout's name + key geometry is compile-time constant, and this fp is
 * per-layout (not the active one) -- immutable, cache forever, keyed by the
 * layout's `struct zmk_physical_layout *` pointer (stable). Same small
 * fixed-cap linear-lookup pattern as the def_fp cache above; keyboards have
 * far fewer physical layouts than this cap, and an exceeded cap just falls
 * back to computing without caching (still correct).
 *
 * Gated by CONFIG_ZMK_FAST_KEYMAP_FP_CACHE (default y); when off, the cache
 * state below doesn't exist and fk_fp_physical_layout_one() always
 * recomputes -- same fp value either way. */
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
#define FK_LAYOUT_FP_CACHE_CAP 16

struct fk_layout_fp_cache_entry {
    const struct zmk_physical_layout *layout;
    uint32_t fp;
};

static struct fk_layout_fp_cache_entry fk_layout_fp_cache[FK_LAYOUT_FP_CACHE_CAP];
static size_t fk_layout_fp_cache_len;

static bool fk_layout_fp_cache_lookup(const struct zmk_physical_layout *layout, uint32_t *out) {
    for (size_t i = 0; i < fk_layout_fp_cache_len; i++) {
        if (fk_layout_fp_cache[i].layout == layout) {
            *out = fk_layout_fp_cache[i].fp;
            return true;
        }
    }
    return false;
}

static void fk_layout_fp_cache_insert(const struct zmk_physical_layout *layout, uint32_t fp) {
    if (fk_layout_fp_cache_len < FK_LAYOUT_FP_CACHE_CAP) {
        fk_layout_fp_cache[fk_layout_fp_cache_len++] =
            (struct fk_layout_fp_cache_entry){.layout = layout, .fp = fp};
    }
    /* Cap exceeded (shouldn't happen on any real keyboard): silently skip
     * caching -- the caller already has the freshly computed value. */
}
#endif /* CONFIG_ZMK_FAST_KEYMAP_FP_CACHE */

uint32_t fk_fp_physical_layout_one(const struct zmk_physical_layout *layout) {
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
    uint32_t cached;
    if (fk_layout_fp_cache_lookup(layout, &cached)) {
        return cached;
    }
#endif

    uint32_t fp = fk_fp16(crc_physical_layout_preimage(0, layout));

#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
    fk_layout_fp_cache_insert(layout, fp);
#endif

    return fp;
}

uint32_t fk_fp_physical_layout(void) {
    uint32_t seed = 0;
    seed = crc_u8(seed, (uint8_t)zmk_physical_layouts_get_selected());

    struct zmk_physical_layout const *const *layouts;
    size_t layout_count = zmk_physical_layouts_get_list(&layouts);

    for (size_t i = 0; i < layout_count; i++) {
        seed = crc_physical_layout_preimage(seed, layouts[i]);
    }

    return fk_fp16(seed);
}

uint32_t fk_fp_layer_current(zmk_keymap_layer_id_t layer_id) {
    uint32_t seed = 0;
    uint16_t binding_count = fk_binding_count();

    for (uint16_t b = 0; b < binding_count; b++) {
        const struct zmk_behavior_binding *binding =
            zmk_keymap_get_layer_binding_at_idx(layer_id, b);
        seed = fk_crc_binding(seed, binding);
    }

    return fk_fp16(seed);
}

uint32_t fk_fp_layer_default(zmk_keymap_layer_id_t layer_id) {
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER)
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
    fk_default_cache_sync_layout();

    bool cacheable = layer_id < ZMK_KEYMAP_LAYERS_LEN;
    if (cacheable && fk_layer_default_fp_valid[layer_id]) {
        return fk_layer_default_fp_cache[layer_id];
    }
#endif

    uint32_t seed = 0;
    uint16_t binding_count = fk_binding_count();

    for (uint16_t b = 0; b < binding_count; b++) {
        const struct zmk_behavior_binding *binding =
            zmk_stock_keymap_get_layer_binding_at_idx(layer_id, b);
        seed = fk_crc_binding(seed, binding);
    }

    seed = fk_fp16(seed);

#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
    if (cacheable) {
        fk_layer_default_fp_cache[layer_id] = seed;
        fk_layer_default_fp_valid[layer_id] = true;
    }
#endif

    return seed;
#else
    return fk_fp_layer_current(layer_id);
#endif
}

bool fk_binding_is_edited(zmk_keymap_layer_id_t layer_id, uint16_t binding_idx) {
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER)
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
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER)
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
