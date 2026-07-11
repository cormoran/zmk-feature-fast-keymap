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
#include <zmk/physical_layouts.h>

/* Compatibility contract version for the canonical fingerprint byte layouts
 * frozen in DESIGN.md SS4/SS5. Bump this (and the mirrored TS constant in
 * the web bundle) whenever that layout changes.
 *
 * v2: fingerprints are truncated so the wire `uint32` fp fields encode as
 * short varints -- opaque fps to 16 bits (fk_fp16()), def_fp to a device-
 * adaptive 16/24/32-bit width (fk_def_fp_bits(), reported in
 * Snapshot.def_fp_bits) chosen so this device's behaviors never collide.
 * The web masks its reproduced def_fp identically (fingerprint.ts), so the
 * bump keeps old caches/bundles from being trusted against the new values. */
#define FK_DEFINITION_VERSION 2

/*
 * Fingerprint (cache key) helpers -- see DESIGN.md SS4/SS5 for the frozen
 * canonical byte layouts these functions hash with crc32_ieee_update()
 * (zephyr/sys/crc.h, seed 0). The canonical form is an INTERNAL hashing
 * input, never the wire format, and must match the web bundle's TS
 * reimplementation bit-for-bit for behaviors_fp/def_fp (the groups the
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

/** behaviors_fp: crc32 over all behaviors in ascending local_id order; for
 * each, `u16le local_id` + `u32le def_fp` (fk_fp_behavior_def(), memoized --
 * see fingerprint.c). Opaque device-computed cache key: the web only ever
 * compares it, never reproduces it locally, so this composition (which
 * differs from the raw def_fp-preimage concatenation described in older
 * design notes) needs no FK_DEFINITION_VERSION bump. Memoized forever after
 * first call (immutable once local_ids settle post settings_load) when
 * CONFIG_ZMK_FAST_KEYMAP_FP_CACHE is on (the default); when off, always
 * recomputed -- the fp value is identical either way. */
uint32_t fk_fp_behaviors(void);

/** def_fp for a single behavior device (SS4/SS5): the canonical metadata
 * bytes (SS5) + `str16 alias` + `str16 display_name`, using the AS-SERVED
 * strings (fk_behavior_served_alias() / fk_behavior_display_name()) -- the
 * exact strings BehaviorDetails puts on the wire, so the client can
 * recompute this from a details response and it matches. Served truncated to
 * fk_def_fp_bits() (the device-adaptive width, below), NOT a fixed 16 bits.
 * Immutable (compile-time metadata/alias/display_name only) -- the full
 * 32-bit value is memoized forever, keyed by `dev` (fingerprint.c), when
 * CONFIG_ZMK_FAST_KEYMAP_FP_CACHE is on (the default); when off, always
 * recomputed with the identical value. */
uint32_t fk_fp_behavior_def(const struct device *dev);

/** The width (16, 24, or 32 bits) def_fp is served at on THIS device: the
 * smallest at which every behavior's def_fp stays distinct, so two distinct
 * behaviors can never collide on their served def_fp (which would mis-resolve
 * one from the web's bundle/cache). Reported in Snapshot.def_fp_bits; the web
 * masks its bundle/cache keys to the same width. Immutable once local_ids
 * settle -- memoized like behaviors_fp when CONFIG_ZMK_FAST_KEYMAP_FP_CACHE is
 * on. See fingerprint.c / DESIGN.md SS4. */
uint8_t fk_def_fp_bits(void);

/** default_keymap_fp: over every layer's DT-stock bindings (SS4). When
 * CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER is off there is no separate stock copy
 * (default == current), so this simply returns fk_fp_keymap() -- no second
 * walk. Under CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER it walks the stock bindings
 * and, when CONFIG_ZMK_FAST_KEYMAP_FP_CACHE is also on (the default), memoizes
 * the result, invalidated whenever the active physical layout changes
 * (fingerprint.c); with the cache off it is recomputed each call -- the fp
 * value is identical either way. */
uint32_t fk_fp_default_keymap(void);

/** keymap_fp: over the active physical-layout index, then all layers' current
 * bindings + layer names (SS4). Mutable. Produced together with every
 * per-layer current fp and physical_layout_fp from a single shared binding
 * walk (Part A dedup) and, under CONFIG_ZMK_FAST_KEYMAP_FP_CACHE, cached until
 * a Studio RPC notification or an active-layout switch invalidates it
 * (fingerprint.c). Byte-identical value whether the cache is on or off. */
uint32_t fk_fp_keymap(void);

/** physical_layout_fp: over the canonical physical layouts (SS4). Shares the
 * mutable dedup/cache with fk_fp_keymap() (fingerprint.c). */
uint32_t fk_fp_physical_layout(void);

/** Invalidate the mutable-keymap fp cache (keymap_fp / per-layer current fp /
 * physical_layout_fp) so the next read recomputes. Called from the Studio RPC
 * notification listener and, when CONFIG_ZMK_FAST_KEYMAP_FP_CACHE is off, by
 * the snapshot handler to force a single fresh compute per snapshot. */
void fk_fp_mutable_invalidate(void);

/** per-layout physical `fp` (SS4/SS6): one layout's `str16 name` + `u16
 * key_count` + per key `i16 width,height,x,y,r,rx,ry` (rotation fields 0
 * when CONFIG_ZMK_PHYSICAL_LAYOUT_KEY_ROTATION is off). Device-opaque -- the
 * web caches+compares it, never recomputes it. Shares its preimage with
 * fk_fp_physical_layout() (the whole-set fp), so a per-layout fp and the
 * corresponding entry inside the whole-set fp can never disagree. Immutable
 * (compile-time layout geometry) -- memoized forever, keyed by `layout`
 * (fingerprint.c), when CONFIG_ZMK_FAST_KEYMAP_FP_CACHE is on (the default);
 * when off, always recomputed with the identical value. */
uint32_t fk_fp_physical_layout_one(const struct zmk_physical_layout *layout);

/** fp for one layer's CURRENT bindings only (no layer_id/name prefix -- the
 * LayerFingerprint message already carries `id` separately). Mutable (changes
 * on keymap edits and active-layout switches). Produced by the shared
 * dedup/cache walk alongside fk_fp_keymap() (fingerprint.c); a layer the walk
 * didn't cover falls back to a direct recompute with the identical value. */
uint32_t fk_fp_layer_current(zmk_keymap_layer_id_t layer_id);

/** fp for one layer's DEFAULT (DT stock) bindings only. Equals
 * fk_fp_layer_current() when CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER is off
 * (always recomputed in that case). Under
 * CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER, memoized per layer_id and
 * invalidated whenever the active physical layout changes (fingerprint.c)
 * when CONFIG_ZMK_FAST_KEYMAP_FP_CACHE is also on (the default); with either
 * config off, always recomputed -- the fp value is identical in every case. */
uint32_t fk_fp_layer_default(zmk_keymap_layer_id_t layer_id);

/** Number of positions where the current binding differs from the default
 * (canonical-bytes comparison) on this layer. Always 0 when
 * CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER is off. */
uint16_t fk_layer_edited_count(zmk_keymap_layer_id_t layer_id);

/** True if binding_idx's current binding differs from its default one. Always
 * false when CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER is off. */
bool fk_binding_is_edited(zmk_keymap_layer_id_t layer_id, uint16_t binding_idx);

/** Number of currently configured layers (ZMK_KEYMAP_LAYERS_LEN minus unused
 * trailing slots), i.e. how many entries zmk_keymap_layer_index_to_id()
 * resolves before returning ZMK_KEYMAP_LAYER_ID_INVAL. */
uint8_t fk_layer_count(void);

/** How many layer slots remain unused (the official "available_layers"
 * meaning: free slots, not configured-layer count). */
uint32_t fk_available_layers(void);

/** Number of registered behaviors (STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, ...) count).
 */
uint32_t fk_behavior_count(void);
