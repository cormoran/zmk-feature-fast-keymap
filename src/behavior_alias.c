/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include "behavior_alias.h"

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

/*
 * Device -> alias table (DESIGN.md SS2.1), built at COMPILE time from a
 * devicetree walk over every child of /behaviors (ZMK built-ins AND
 * custom/user-defined behaviors, e.g. a user's `ht: hold_tap {...}`) and,
 * if present, /macros (the node keymap authors conventionally use for their
 * own `zmk,behavior-macro` definitions -- see e.g.
 * dependencies/zmk/app/boards/shields/tester_xiao/tester_xiao.keymap).
 *
 * For each child node this maps DEVICE_DT_NAME(node_id) -- the plain string
 * constant DEVICE_DT_DEFINE() bakes into `struct device.name`, i.e. the
 * exact string `dev->name` holds at runtime -- to the node's FIRST
 * devicetree *node label* (the `foo:` token used to reference it from
 * bindings, e.g. `kp` for `&kp`; NOT the unrelated `label` *property*).
 *
 * IMPORTANT (discovered the hard way, see git history): this table must be
 * built from STRING CONSTANTS ONLY (DEVICE_DT_NAME(), node-label strings)
 * and must NEVER take a device's address (DEVICE_DT_GET()) at compile
 * time. Every one of ZMK's built-in behavior nodes is wrapped in
 * `#if ZMK_BEHAVIOR_OMIT(NAME) /omit-if-no-ref/ #endif`
 * (dependencies/zmk/app/dts/behaviors/*.dtsi): a behavior nobody's keymap
 * references keeps its devicetree node/ordinal (so DT_FOREACH_* macros and
 * DEVICE_DT_NAME() still compile fine on it) but never gets an actual
 * `struct device` linked in -- DEVICE_DT_GET() on it compiles but fails at
 * LINK time with "undefined reference to __device_dts_ord_N". Plain string
 * constants never emit a device symbol reference, so this table is
 * link-safe regardless of which behaviors a consumer's keymap references.
 * The runtime lookup in fk_behavior_alias() below only ever sees *devices
 * that were actually passed in* (i.e. instantiated behaviors), so any
 * table entries for un-instantiated/omitted nodes are simply never
 * matched -- harmless.
 *
 * A node can have zero, one, or several node labels
 * (DT_NUM_NODELABELS(node_id)); FK_ALIAS_FIRST_LABEL_OR_NULL() below picks
 * the first one (in devicetree source order) without ever indexing into a
 * possibly-empty array -- see its comment.
 */

struct fk_alias_name_entry {
    const char *device_name;
    const char *alias; /* NULL if the node has no node label. */
};

/*
 * Expands to the node's first node label as a string literal, or to NULL if
 * it has none.
 *
 * This intentionally does NOT use `DT_NODELABEL_STRING_ARRAY(node_id)[0]`:
 * that macro expands to a brace-enclosed initializer list (`{ "a", "b", }`),
 * and (a) indexing it requires wrapping it in a compound literal, whose
 * element is then a *read* from a static array rather than a compile-time
 * constant expression suitable for use inside another static initializer,
 * and (b) for a label-less node the list is empty (`{ }`), which is not a
 * valid array initializer/subscript target at all. Instead, this pulls the
 * first label out with plain preprocessor token manipulation: expand
 * DT_FOREACH_NODELABEL() into a comma-separated list of string literals
 * (one per label, in source order) and take just the first argument with
 * GET_ARG_N(1, ...). That list is only ever expanded when
 * DT_NUM_NODELABELS(node_id) is nonzero (guarded by COND_CODE_0 below), so
 * GET_ARG_N(1, ...) always has at least one argument to select -- this
 * works uniformly whether the node has exactly one label or several (e.g.
 * dependencies/zmk/app/dts/behaviors/sensor_rotate_key_press.dtsi's
 * `inc_dec_cp: inc_dec_kp: enc_key_press` has two).
 */
#define FK_ALIAS_FIRST_LABEL_OR_NULL(node_id)                                                      \
    COND_CODE_0(                                                                                   \
        DT_NUM_NODELABELS(node_id), (NULL),                                                        \
        (GET_ARG_N(1, DT_FOREACH_NODELABEL(node_id, DT_NODELABEL_STRING_ARRAY_ENTRY_INTERNAL))))

#define FK_ALIAS_TABLE_ENTRY(node_id)                                                              \
    {DEVICE_DT_NAME(node_id), FK_ALIAS_FIRST_LABEL_OR_NULL(node_id)},

static const struct fk_alias_name_entry fk_alias_table[] = {
    DT_FOREACH_CHILD_STATUS_OKAY(DT_PATH(behaviors), FK_ALIAS_TABLE_ENTRY)
#if DT_NODE_EXISTS(DT_PATH(macros))
        DT_FOREACH_CHILD_STATUS_OKAY(DT_PATH(macros), FK_ALIAS_TABLE_ENTRY)
#endif
};

const char *fk_behavior_alias(const struct device *dev) {
    if (!dev || !dev->name) {
        return NULL;
    }

    for (size_t i = 0; i < ARRAY_SIZE(fk_alias_table); i++) {
        if (strcmp(fk_alias_table[i].device_name, dev->name) == 0) {
            return fk_alias_table[i].alias;
        }
    }

    return NULL;
}

const char *fk_behavior_served_alias(const struct device *dev) {
    if (!dev) {
        return NULL;
    }

    const char *alias = fk_behavior_alias(dev);
    if (alias) {
        return alias;
    }

    return dev->name;
}

const char *fk_behavior_display_name(const struct device *dev) {
    if (!dev) {
        return "";
    }

    STRUCT_SECTION_FOREACH(zmk_behavior_ref, item) {
        if (item->device == dev) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
            if (item->metadata.display_name) {
                return item->metadata.display_name;
            }
#endif
            break;
        }
    }

    return dev->name;
}

/*
 * ZMK's built-in behaviors' devicetree node NAMES (not aliases -- some
 * built-ins have a different node label than node name, e.g. `bl: bcklight`),
 * frozen by dependencies/zmk/app/dts/behaviors/*.dtsi and
 * dependencies/zmk/app/dts/behaviors/macros.dtsi. This is intentionally
 * separate from fk_alias_table above: that table now also contains
 * custom/user-defined behaviors and macros, so "is this device_name in the
 * alias table" is no longer a valid definition of "standard".
 *
 * To save flash we store only the crc32_ieee() fingerprint of each name
 * rather than the name strings themselves (~35 pointers + string bytes ->
 * one uint32 each). The comment on each line is the source name; the values
 * are `crc32_ieee((uint8_t *)name, strlen(name))` == zlib.crc32(name) and were
 * verified collision-free for this set. Collision risk with a non-builtin
 * device name is negligible and would only mis-set the advisory `standard`
 * hint (never wrong keymap data); regenerate with tools if the list changes.
 */
static const uint32_t fk_builtin_name_fps[] = {
    0x9FFA0172U, /* key_press */
    0xB438CCE4U, /* momentary_layer */
    0x0875E90CU, /* transparent */
    0x7F9000CFU, /* none */
    0x0E686AFBU, /* mod_tap */
    0x0EB18A27U, /* layer_tap */
    0xE5715B67U, /* grave_escape */
    0x156757EDU, /* toggle_layer */
    0xEFE7102AU, /* to_layer */
    0x560F39B4U, /* sticky_key */
    0x05B663F1U, /* sticky_layer */
    0x2ED34CD7U, /* key_toggle */
    0xA68E54B9U, /* caps_word */
    0xAB6B8979U, /* rgb_ug */
    0x0C75AF5CU, /* outputs */
    0x5C68CF39U, /* z_so_off */
    0xA36E529CU, /* key_repeat */
    0x82B97AA4U, /* bcklight */
    0xD11711C2U, /* bluetooth */
    0xDFB55690U, /* extpower */
    0x8FCFBB02U, /* studio_unlock */
    0x2504EBDDU, /* sysreset */
    0x7BA08422U, /* bootload */
    0x5CC73CF4U, /* macro_tap */
    0x7EB9239FU, /* macro_press */
    0xC2210A25U, /* macro_release */
    0xB716CB47U, /* macro_tap_time */
    0x7AC71B0CU, /* macro_wait_time */
    0x2817F6E5U, /* macro_pause_for_release */
    0x64F90E61U, /* macro_param_1to1 */
    0xFDF05FDBU, /* macro_param_1to2 */
    0x764CA18FU, /* macro_param_2to1 */
    0xEF45F035U, /* macro_param_2to2 */
    0x57CA6127U, /* mouse_key_press */
    0x5179D216U, /* mouse_move */
    0xC609D01AU, /* mouse_scroll */
    0x2511A8F9U, /* enc_key_press */
};

bool fk_behavior_is_standard(const struct device *dev) {
    if (!dev || !dev->name) {
        return false;
    }

    uint32_t fp = crc32_ieee((const uint8_t *)dev->name, strlen(dev->name));

    for (size_t i = 0; i < ARRAY_SIZE(fk_builtin_name_fps); i++) {
        if (fk_builtin_name_fps[i] == fp) {
            return true;
        }
    }

    return false;
}
