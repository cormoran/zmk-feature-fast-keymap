/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

#include <drivers/behavior.h>
#include <zmk/keymap.h>

/* Compatibility contract version for the canonical fingerprint byte layouts
 * frozen in DESIGN.md SS4/SS5. Bump this (and the mirrored TS constant in
 * the web bundle) whenever that layout changes. */
#define FK_DEFINITION_VERSION 1

/*
 * Fingerprint (cache key) helpers -- see DESIGN.md SS4/SS5 for the frozen
 * canonical byte layouts these functions hash with crc32_ieee_update()
 * (zephyr/sys/crc.h, seed 0). The canonical form is an INTERNAL hashing
 * input, never the wire format, and must match the web bundle's TS
 * reimplementation bit-for-bit for behaviors_fp/metadata_fp (the groups the
 * web reproduces locally). Bump FK_DEFINITION_VERSION if this layout ever
 * changes.
 *
 * Canonical binding bytes (12 bytes), used by every keymap-related fp:
 *   i32 behavior_local_id (LE; -1 when behavior_dev == NULL)
 *   u32 param1 (LE)
 *   u32 param2 (LE)
 */

/** Number of key positions in the currently selected physical layout, i.e.
 * the valid range for `binding_idx` passed to zmk_keymap_get_layer_binding_at_idx() /
 * zmk_stock_keymap_get_layer_binding_at_idx(). Shared by the handler (binding_count,
 * iteration bounds) and every fingerprint helper below. */
uint16_t fk_binding_count(void);

/** crc32 over a single canonical binding (12 bytes), chained from `seed`. */
uint32_t fk_crc_binding(uint32_t seed, const struct zmk_behavior_binding *binding);

/** behaviors_fp: over all behaviors in ascending local_id order (SS4). */
uint32_t fk_fp_behaviors(void);

/** metadata_fp for a single behavior device (SS5's canonical byte layout). */
uint32_t fk_fp_behavior_metadata(const struct device *dev);

/** default_keymap_fp: over every layer's DT-stock bindings (SS4). Equals
 * fk_fp_keymap_bindings_only() when CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE is off
 * (no separate stock copy exists; default == current). */
uint32_t fk_fp_default_keymap(void);

/** keymap_fp: over all layers' current bindings + layer names + the active
 * physical-layout index (SS4). */
uint32_t fk_fp_keymap(void);

/** physical_layout_fp: over the canonical physical layouts (SS4). */
uint32_t fk_fp_physical_layout(void);

/** fp for one layer's CURRENT bindings only (no layer_id/name prefix -- the
 * LayerFingerprint message already carries `id` separately). */
uint32_t fk_fp_layer_current(zmk_keymap_layer_id_t layer_id);

/** fp for one layer's DEFAULT (DT stock) bindings only. Equals
 * fk_fp_layer_current() when CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE is off. */
uint32_t fk_fp_layer_default(zmk_keymap_layer_id_t layer_id);

/** Number of positions where the current binding differs from the default
 * (canonical-bytes comparison) on this layer. Always 0 when
 * CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE is off. */
uint16_t fk_layer_edited_count(zmk_keymap_layer_id_t layer_id);

/** True if binding_idx's current binding differs from its default one. Always
 * false when CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE is off. */
bool fk_binding_is_edited(zmk_keymap_layer_id_t layer_id, uint16_t binding_idx);

/** Number of currently configured layers (ZMK_KEYMAP_LAYERS_LEN minus unused
 * trailing slots), i.e. how many entries zmk_keymap_layer_index_to_id()
 * resolves before returning ZMK_KEYMAP_LAYER_ID_INVAL. */
uint8_t fk_layer_count(void);

/** How many layer slots remain unused (the official "available_layers"
 * meaning: free slots, not configured-layer count). */
uint32_t fk_available_layers(void);

/** Number of registered behaviors (STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, ...) count). */
uint32_t fk_behavior_count(void);
