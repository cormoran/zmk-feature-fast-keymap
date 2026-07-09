// CRC32 + canonicalization primitives shared by the fast-keymap fingerprint
// contract -- see DESIGN.md SS5 / docs/protocol.md in the parent repo. This
// file has no dependency on the bundled standard-behavior table
// (standardBehaviors.ts) so it can be reused standalone.

import type { BehaviorBindingParametersSet } from "./proto/cormoran/fast-keymap/fast_keymap";

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
