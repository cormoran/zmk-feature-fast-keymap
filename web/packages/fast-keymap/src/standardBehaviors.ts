// Bundled definitions for ZMK's standard (built-in) behaviors, so the fast
// client (fastKeymap.ts) can resolve a behavior's alias/display-name/param
// metadata WITHOUT round-tripping `get_behavior_details` to the device --
// see DESIGN.md SS2 "Bundled standard definitions". Keyed by **alias**
// (e.g. "kp" for `&kp`), matching `BehaviorSummary.alias`.
//
// Values here were read directly off the ZMK source this module vendors at
// `dependencies/zmk/app/src/behaviors/*.c` (metadata) and
// `dependencies/zmk/app/dts/behaviors/*.dtsi` (alias/display-name), NOT
// guessed. Two families of exceptions -- kept anyway per DESIGN.md's
// "being incomplete is OK; a bundle mismatch just costs an extra fetch":
//
// 1. HID_USAGE keyboard/consumer max usage (kp/kt/mt/lt/sk) and the BT `sl`
//    profile-count range depend on the *firmware's* Kconfig (NKRO vs HKRO,
//    consumer-report basic/full, CONFIG_BT_MAX_PAIRED) -- values that vary
//    per board and are NOT knowable from the web bundle alone. The
//    constants below are ZMK's Kconfig-implicit defaults (first choice
//    entry when no `default` is set: HKRO / full consumer report) and a
//    common `CONFIG_BT_MAX_PAIRED=5`. Boards that configure differently
//    will simply metadata_fp-miss and fall back to `get_behavior_details`
//    -- correct, just not fast for that one behavior.
// 2. `sk`/`sl` (behavior_sticky_key.c) and `mt`/`lt` (behavior_hold_tap.c)
//    compute their metadata dynamically from their *bound* child
//    behavior(s) at runtime (`sk` binds `&kp`, `sl` binds `&mo`, `mt` binds
//    `&kp`+`&kp`, `lt` binds `&mo`+`&kp` -- see their .dtsi defaults). The
//    entries below hard-code that default wiring; a keymap that rebinds
//    e.g. `sk`'s child behavior would change its metadata_fp and miss the
//    bundle (falls back to fetch, still correct).

import type {
  BehaviorBindingParametersSet,
  BehaviorParameterValueDescription,
} from "./proto/cormoran/fast-keymap/fast_keymap";
import { metadataFp } from "./fingerprint";

/** Mirrors the firmware's `FK_DEFINITION_VERSION` (src/fingerprint.h). Bump
 * both together if the canonical fingerprint byte layout below ever
 * changes. */
export const FK_DEFINITION_VERSION = 1;

// -- best-effort, board-dependent constants (see file header, exception 1) --
const BUNDLED_HID_KEYBOARD_MAX_USAGE = 0xff; // ZMK_HID_REPORT_TYPE_HKRO default
const BUNDLED_HID_CONSUMER_MAX_USAGE = 0xfff; // ZMK_HID_CONSUMER_REPORT_USAGES_FULL default
const BUNDLED_BT_PROFILE_COUNT = 5; // common CONFIG_BT_MAX_PAIRED default

function hidUsage(name: string): BehaviorParameterValueDescription {
  return {
    name,
    hidUsage: {
      keyboardMax: BUNDLED_HID_KEYBOARD_MAX_USAGE,
      consumerMax: BUNDLED_HID_CONSUMER_MAX_USAGE,
    },
  };
}

function layerId(name: string): BehaviorParameterValueDescription {
  return { name, layerId: {} };
}

function constant(
  name: string,
  value: number
): BehaviorParameterValueDescription {
  return { name, constant: value };
}

function range(
  name: string,
  min: number,
  max: number
): BehaviorParameterValueDescription {
  return { name, range: { min, max } };
}

function set(
  param1: BehaviorParameterValueDescription[],
  param2: BehaviorParameterValueDescription[] = []
): BehaviorBindingParametersSet {
  return { param1, param2 };
}

export interface StandardBehaviorDef {
  /** e.g. "Key Press" for alias "kp" -- BehaviorSummary.display_name. */
  displayName: string;
  /** Same shape/semantics as GetBehaviorDetailsResponse.metadata. */
  metadata: BehaviorBindingParametersSet[];
}

/** Keyed by alias (BehaviorSummary.alias / GetBehaviorDetailsResponse.alias). */
export const STANDARD_BEHAVIORS: Record<string, StandardBehaviorDef> = {
  kp: { displayName: "Key Press", metadata: [set([hidUsage("Key")])] },
  mo: { displayName: "Momentary Layer", metadata: [set([layerId("Layer")])] },
  trans: { displayName: "Transparent", metadata: [] },
  none: { displayName: "None", metadata: [] },
  // mod_tap default bindings = <&kp>, <&kp> (mod_tap.dtsi).
  mt: {
    displayName: "Mod-Tap",
    metadata: [set([hidUsage("Key")], [hidUsage("Key")])],
  },
  // layer_tap default bindings = <&mo>, <&kp> (layer_tap.dtsi).
  lt: {
    displayName: "Layer-Tap",
    metadata: [set([layerId("Layer")], [hidUsage("Key")])],
  },
  tog: { displayName: "Toggle Layer", metadata: [set([layerId("Layer")])] },
  to: { displayName: "To Layer", metadata: [set([layerId("Layer")])] },
  // sticky_key default bindings = <&kp> (sticky_key.dtsi); metadata is the
  // bound behavior's metadata verbatim (behavior_sticky_key.c).
  sk: { displayName: "Sticky Key", metadata: [set([hidUsage("Key")])] },
  // sticky_layer (same driver, bound to <&mo>).
  sl: { displayName: "Sticky Layer", metadata: [set([layerId("Layer")])] },
  kt: { displayName: "Key Toggle", metadata: [set([hidUsage("Key")])] },
  bt: {
    displayName: "Bluetooth",
    metadata: [
      set([
        constant("Next Profile", 1 /* BT_NXT_CMD */),
        constant("Previous Profile", 2 /* BT_PRV_CMD */),
        constant("Clear All Profiles", 4 /* BT_CLR_ALL_CMD */),
        constant("Clear Selected Profile", 0 /* BT_CLR_CMD */),
      ]),
      set(
        [
          constant("Select Profile", 3 /* BT_SEL_CMD */),
          constant("Disconnect Profile", 5 /* BT_DISC_CMD */),
        ],
        [range("Profile", 0, BUNDLED_BT_PROFILE_COUNT)]
      ),
    ],
  },
  out: {
    displayName: "Output Selection",
    metadata: [
      set([
        constant("Toggle Outputs", 0 /* OUT_TOG */),
        constant("USB Output", 1 /* OUT_USB */),
        constant("BLE Output", 2 /* OUT_BLE */),
        constant("No Output", 3 /* OUT_NONE */),
      ]),
    ],
  },
  rgb_ug: {
    displayName: "Underglow",
    metadata: [
      set([
        constant("Toggle On/Off", 0),
        constant("Turn On", 1),
        constant("Turn OFF", 2),
        constant("Hue Up", 3),
        constant("Hue Down", 4),
        constant("Saturation Up", 5),
        constant("Saturation Down", 6),
        constant("Brightness Up", 7),
        constant("Brightness Down", 8),
        constant("Speed Up", 9),
        constant("Speed Down", 10),
        constant("Next Effect", 11),
        constant("Previous Effect", 12),
      ]),
    ],
  },
  // No CONFIG_ZMK_BEHAVIOR_METADATA block in behavior_ext_power.c -> empty.
  ext_power: { displayName: "External Power", metadata: [] },
};

const bundledMetadataFpCache = new Map<string, number>();

/** The metadata_fp the firmware would compute for the bundled definition of
 * `alias`, or `undefined` if this alias isn't bundled. Compare against a
 * device's reported `BehaviorSummary.metadata_fp` / `metadata_fp` to decide
 * whether the bundled copy can be used as-is (fp match) or the behavior
 * must be fetched via `get_behavior_details` (fp miss or unbundled alias). */
export function bundledMetadataFp(alias: string): number | undefined {
  const def = STANDARD_BEHAVIORS[alias];
  if (!def) return undefined;

  let cached = bundledMetadataFpCache.get(alias);
  if (cached === undefined) {
    cached = metadataFp(def.metadata);
    bundledMetadataFpCache.set(alias, cached);
  }
  return cached;
}
