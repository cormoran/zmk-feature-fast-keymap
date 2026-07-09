/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <zephyr/device.h>

/*
 * Best-effort resolution of a behavior device to its keymap alias (the DT
 * node label used to reference it in bindings, e.g. "kp" for &kp) -- see
 * DESIGN.md SS2.1. The browser bundle is the authoritative source of
 * canonical aliases for standard behaviors; this firmware-side table is a
 * convenience/override, mainly useful for custom (user-defined) behaviors
 * the bundle cannot know about ahead of time.
 */

/** The device's primary node label, or NULL if none could be resolved
 * (caller should fall back to zmk_behavior_find_behavior_name_from_local_id()
 * / dev->name). */
const char *fk_behavior_alias(const struct device *dev);

/** The AS-SERVED alias (DESIGN.md SS4/SS5): fk_behavior_alias(dev), falling
 * back to dev->name when the node has no resolvable label. This is the exact
 * string the RPC wire encoder sends for BehaviorDetails.alias, so it MUST be
 * the same helper used by the def_fp/behaviors_fp fingerprint computation
 * (fingerprint.c) -- sharing it is what guarantees the two can never
 * disagree. Returns NULL only if dev itself is NULL. */
const char *fk_behavior_served_alias(const struct device *dev);

/** dev's human-readable display name (struct zmk_behavior_ref's
 * metadata.display_name), or dev->name if no zmk_behavior_ref entry matches. */
const char *fk_behavior_display_name(const struct device *dev);

/** True if dev is one of ZMK's built-in behaviors (shipped in
 * dependencies/zmk/app/dts/behaviors, *.dtsi files), false for custom/user-defined
 * behaviors and macros. Best-effort: matches the device name against a fixed
 * list of the built-in behaviors' devicetree node names. */
bool fk_behavior_is_standard(const struct device *dev);
