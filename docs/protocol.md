# fast-keymap protocol contract

This module's speed depends on the **web client reproducing the firmware's
fingerprints byte-for-byte**. That shared byte layout is a frozen contract,
versioned by `FK_DEFINITION_VERSION`. This file documents it and how to change
it safely.

- Firmware side: `src/fingerprint.c` (`crc_*` helpers), `proto/cormoran/fast-keymap/fast_keymap.proto`.
- Web side: `web/src/standardBehaviors.ts` (`crc32Update`, `metadataFp`),
  `web/src/fastKeymap.ts`.
- Rationale and RPC table: `DESIGN.md` (§3–§7).

## Fingerprints

CRC32 (IEEE 802.3 / zlib, reflected polynomial `0xEDB88320`, seed `0`,
`~crc` in and out). Computed with `crc32_ieee_update()` on-device and a matching
table implementation in the web app. Fingerprints are **internal hashing
inputs** — the bytes below never go on the wire; only the resulting `uint32`
does (inside `Snapshot`).

Serialization primitives (little-endian):

| token | bytes |
|-------|-------|
| `u8`  | 1 |
| `u16le` / `u32le` / `i32le` | 2 / 4 / 4, little-endian |
| `str16` | `u16le` length + raw UTF-8 bytes |
| `str8`  | `u8` length + raw UTF-8 bytes |

Canonical **binding** (12 bytes): `i32le behavior_local_id` (`-1` if no
behavior) + `u32le param1` + `u32le param2`.

Canonical **metadata** (input to each behavior's `metadata_fp`):

```
u8 num_sets
per set:  u8 num_param1_values ; u8 num_param2_values
  per value (all param1 values, then all param2 values):
    u8    value_type        ; 0=nil 1=constant 2=range 3=hid_usage 4=layer_id
    u32le a                 ; constant | range.min | hid keyboard_max ; else 0
    u32le b                 ; 0        | range.max | hid consumer_max
    str8  value_display_name
```

Groups reported in `Snapshot`:

| field | canonicalization |
|-------|------------------|
| `def_fp` (per behavior; the identity + validator) | metadata bytes + `str16 alias` + `str16 display_name`, using the **as-served** strings (alias = first DT node label, else device name; display_name = metadata display name, else device name). Keys the web's standard-behavior bundle and per-device cache. |
| `behaviors_fp` | per behavior, ascending `local_id`: `u16le local_id` + that behavior's `def_fp`-preimage (metadata bytes + `str16 alias` + `str16 display_name`, as-served) |
| `default_keymap_fp` | per layer (index order): `u8 layer_id` + each binding_idx's canonical binding (stock/default) |
| `keymap_fp` | `u8 active_layout_index`, then per layer: `u8 layer_id` + `str16 name` + each binding_idx's canonical binding (current) |
| `LayerFingerprint.fp` / `.default_fp` (per layer) | that layer's canonical bindings concatenated (current / default); no `layer_id` prefix — the message carries `id` separately |
| per-layout physical `fp` (in `PhysicalLayoutSummary`) | one layout: `str16 name` + `u16le key_count` + per key `i16le width,height,x,y,r,rx,ry` (rotation fields `0` when `CONFIG_ZMK_PHYSICAL_LAYOUT_KEY_ROTATION` is off). **Device-opaque** — the web caches+compares it, never recomputes it. |
| `physical_layout_fp` | `u8 active_layout_index`, then per layout that layout's per-layout-`fp`-preimage (name + keys, as above) |

> Note the two string encodings: alias/display_name (in `def_fp` /
> `behaviors_fp`) use `str16`, but value display-names **inside metadata** use
> `str8`. Keep both sides in sync.

## `FK_DEFINITION_VERSION`

A single integer defined in **both** `src/fingerprint.h` (C) and
`web/src/standardBehaviors.ts` (TS). It gates the compatibility of:

1. the fingerprint byte layouts above, and
2. the web app's bundled standard-behavior metadata (`STANDARD_BEHAVIORS`).

The web client only trusts a bundled definition (skipping a device fetch) when
its `FK_DEFINITION_VERSION` matches and the behavior's `def_fp` matches a
bundle entry (the web indexes `STANDARD_BEHAVIORS` by locally-computed
`def_fp`).

### When to bump it

Bump the constant (in both files, together) whenever you change **any** of:

- a fingerprint's canonical byte layout (order, widths, which fields hash);
- the CRC parameters;
- the meaning/shape of the bundled standard-behavior metadata.

You do **not** need to bump it to add a new RPC message or a new bundled
behavior that doesn't change existing canonicalization. When unsure, bump — a
mismatched version just makes clients fall back to fetching from the device
(slower, never incorrect).

## Behavior-dependent constants (web bundle caveat)

Some standard-behavior metadata depends on the consumer's ZMK Kconfig (HID
keyboard/consumer max usage under NKRO/HKRO and consumer basic/full; Bluetooth
profile count via `CONFIG_BT_MAX_PAIRED`). The web bundle can't know these, so
it uses documented defaults (see the web package's `standardBehaviors.ts`). On
a board with different Kconfig the affected behavior's `def_fp` won't match the
bundle, so it funnels into the `get_behaviors` batch fetch (`kp`/`kt`/`mt`/
`lt`/`sk`/`bt`) — one extra round-trip, never wrong data.
