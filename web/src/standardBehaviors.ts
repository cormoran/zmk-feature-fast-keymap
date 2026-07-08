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

// ---------------------------------------------------------------------
// CRC32 (IEEE 802.3 / zlib, seed 0) -- bit-for-bit port of Zephyr's
// crc32_ieee_update() (dependencies/zephyr/lib/crc/crc32_sw.c): same
// reflected polynomial 0xEDB88320, same ~crc in/out. This is the standard
// byte-at-a-time CRC32 table algorithm, so it produces identical output to
// the firmware's nibble-table implementation for the same seed/bytes.
// ---------------------------------------------------------------------

let crc32Table: Uint32Array | null = null;

function getCrc32Table(): Uint32Array {
  if (crc32Table) return crc32Table;
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? (0xedb88320 ^ (c >>> 1)) >>> 0 : c >>> 1;
    }
    table[n] = c >>> 0;
  }
  crc32Table = table;
  return table;
}

/** crc32_ieee_update(seed, bytes) -- chainable, seed 0 for a fresh hash. */
export function crc32Update(seed: number, bytes: Uint8Array): number {
  const table = getCrc32Table();
  let crc = ~seed >>> 0;
  for (let i = 0; i < bytes.length; i++) {
    crc = (table[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8)) >>> 0;
  }
  return ~crc >>> 0;
}

// -- little-endian byte helpers mirroring src/fingerprint.c's crc_u8/crc_u16le/... --

function crcU8(seed: number, v: number): number {
  return crc32Update(seed, Uint8Array.of(v & 0xff));
}

function crcU32le(seed: number, v: number): number {
  const u = v >>> 0;
  return crc32Update(
    seed,
    Uint8Array.of(
      u & 0xff,
      (u >>> 8) & 0xff,
      (u >>> 16) & 0xff,
      (u >>> 24) & 0xff
    )
  );
}

const textEncoder = new TextEncoder();

/** DESIGN.md SS5's `u8 name_len ; name bytes` -- ONLY used inside
 * metadata_fp (unlike behaviors_fp's alias/display_name, which use a u16
 * length prefix -- fk_crc_str_u16len() in the firmware; metadataFp() below
 * never needs that variant). Names longer than 255 UTF-8 bytes are
 * truncated to match the firmware's u8 length prefix (an unrealistic case
 * for behavior parameter value display names). */
function crcStrU8len(seed: number, s: string): number {
  const bytes = textEncoder.encode(s);
  const len = Math.min(bytes.length, 0xff);
  seed = crcU8(seed, len);
  if (len) {
    seed = crc32Update(seed, bytes.subarray(0, len));
  }
  return seed;
}

/**
 * Canonical metadata_fp bytes (DESIGN.md SS5), reproduced bit-for-bit from
 * `crc_metadata()` in src/fingerprint.c:
 *
 * ```
 * u8  num_sets
 * repeat num_sets:
 *   u8 num_param1_values ; u8 num_param2_values
 *   repeat (all param1 values, then all param2 values):
 *     u8  value_type      ; 0=nil 1=constant 2=range 3=hid_usage 4=layer_id
 *     u32 a                ; constant | range.min | hid keyboard_max (0 if unused)
 *     u32 b                ; 0        | range.max | hid consumer_max
 *     u8  name_len ; name bytes
 * ```
 */
export function metadataFp(metadata: BehaviorBindingParametersSet[]): number {
  let seed = 0;
  seed = crcU8(seed, metadata.length & 0xff);

  for (const paramSet of metadata) {
    seed = crcU8(seed, paramSet.param1.length & 0xff);
    seed = crcU8(seed, paramSet.param2.length & 0xff);

    for (const values of [paramSet.param1, paramSet.param2]) {
      for (const val of values) {
        let valueType = 0;
        let a = 0;
        let b = 0;

        if (val.constant !== undefined) {
          valueType = 1;
          a = val.constant;
        } else if (val.range !== undefined) {
          valueType = 2;
          a = val.range.min;
          b = val.range.max;
        } else if (val.hidUsage !== undefined) {
          valueType = 3;
          a = val.hidUsage.keyboardMax;
          b = val.hidUsage.consumerMax;
        } else if (val.layerId !== undefined) {
          valueType = 4;
        } else {
          valueType = 0; // nil (also the default for an empty oneof)
        }

        seed = crcU8(seed, valueType);
        seed = crcU32le(seed, a);
        seed = crcU32le(seed, b);
        seed = crcStrU8len(seed, val.name);
      }
    }
  }

  return seed;
}

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
