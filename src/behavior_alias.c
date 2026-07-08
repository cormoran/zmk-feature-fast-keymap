/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include "behavior_alias.h"

#include <string.h>

#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

/*
 * Best-effort device -> alias table (DESIGN.md SS2.1).
 *
 * DESIGN.md originally called for a compile-time devicetree walk (either
 * `DT_FOREACH_STATUS_OKAY(<compat>, FN)` over ZMK's built-in behavior
 * compatibles, or `DT_FOREACH_CHILD_STATUS_OKAY` over DT_PATH(behaviors) /
 * DT_PATH(macros)) that resolves each matched node straight to a
 * `DEVICE_DT_GET(node_id)` device pointer at compile time.
 *
 * That approach turned out to be unsafe: every one of ZMK's built-in
 * behavior nodes is declared with `#if ZMK_BEHAVIOR_OMIT(NAME) /omit-if-no-ref/
 * #endif` (dependencies/zmk/app/dts/behaviors/*.dtsi), and ZMK_BEHAVIOR_OMIT()
 * defaults to true for every behavior unless a keyboard's config explicitly
 * opts it back in (dependencies/zmk/app/include/dt-bindings/zmk/behaviors.h).
 * A behavior that no binding anywhere in the active devicetree references
 * (e.g. &bl, &rgb_ug, &soft_off, the mouse-move/scroll/click behaviors, on a
 * keymap that never uses them) keeps its devicetree node/ordinal (so
 * DT_FOREACH_STATUS_OKAY/DEVICE_DT_GET still compile) but never gets a
 * `struct device` linked in -- DEVICE_DT_GET() on it compiles fine and then
 * fails at LINK time with "undefined reference to __device_dts_ord_N".
 * Confirmed by trying it: tests/zmk-config's tester_xiao build failed to
 * link on exactly the six built-ins its keymap/config don't reference
 * (backlight, mouse move/scroll/click, rgb underglow, soft-off). Since which
 * behaviors a *consumer's* keymap references is exactly the kind of thing
 * this module cannot control or predict, any compile-time-resolved device
 * reference to a fixed behavior list is unsafe in general, not just for our
 * own tests.
 *
 * The fix: never take a device's address at compile time. Instead, key the
 * alias table by the behavior's DEVICE NAME STRING (DEVICE_DT_NAME(node_id),
 * i.e. the plain string constant that DEVICE_DT_DEFINE() bakes into
 * `struct device.name` -- see DEVICE_DT_NAME()'s definition in
 * zephyr/device.h: the devicetree node's name, since none of these nodes set
 * an explicit `label` *property* (colon-syntax node labels, e.g. `kp:`, are
 * a different thing and are NOT what ends up in `struct device.name`). Only
 * behaviors that are *actually instantiated* ever reach this table's lookup
 * (STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, ...) only iterates real
 * devices), so there is nothing to link against and this is safe regardless
 * of which behaviors a keymap does or doesn't use. The device NAME strings
 * below are frozen by dependencies/zmk/app/dts/behaviors/*.dtsi and
 * dependencies/zmk/app/dts/behaviors/macros.dtsi and don't depend on the
 * active devicetree at all.
 */

struct fk_alias_name_entry {
    const char *device_name;
    const char *alias;
};

static const struct fk_alias_name_entry fk_alias_table[] = {
    {"key_press", "kp"},
    {"momentary_layer", "mo"},
    {"transparent", "trans"},
    {"none", "none"},
    {"mod_tap", "mt"},
    {"layer_tap", "lt"},
    {"grave_escape", "gresc"},
    {"toggle_layer", "tog"},
    {"to_layer", "to"},
    {"sticky_key", "sk"},
    {"sticky_layer", "sl"},
    {"key_toggle", "kt"},
    {"caps_word", "caps_word"},
    {"rgb_ug", "rgb_ug"},
    {"outputs", "out"},
    {"z_so_off", "soft_off"},
    {"key_repeat", "key_repeat"},
    {"bcklight", "bl"},
    {"bluetooth", "bt"},
    {"extpower", "ext_power"},
    {"studio_unlock", "studio_unlock"},
    {"sysreset", "sys_reset"},
    {"bootload", "bootloader"},
    {"macro_tap", "macro_tap"},
    {"macro_press", "macro_press"},
    {"macro_release", "macro_release"},
    {"macro_tap_time", "macro_tap_time"},
    {"macro_wait_time", "macro_wait_time"},
    {"macro_pause_for_release", "macro_pause_for_release"},
    {"macro_param_1to1", "macro_param_1to1"},
    {"macro_param_1to2", "macro_param_1to2"},
    {"macro_param_2to1", "macro_param_2to1"},
    {"macro_param_2to2", "macro_param_2to2"},
    {"mouse_key_press", "mkp"},
    {"mouse_move", "mmv"},
    {"mouse_scroll", "msc"},
    {"enc_key_press", "inc_dec_kp"},
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

bool fk_behavior_is_standard(const struct device *dev) {
    return dev && dev->name && fk_behavior_alias(dev) != NULL;
}
