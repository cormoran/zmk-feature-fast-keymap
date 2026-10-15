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

The four-character keymap identifier is separate from these fingerprints. It
is SHA-256 of a client-chosen UTF-8 source string, truncated to the first three
digest bytes and encoded as unpadded Base64url. It identifies the keymap type
and stays unchanged when bindings change. See the README for get/set semantics
and collision risk.

CRC32 (IEEE 802.3 / zlib, reflected polynomial `0xEDB88320`, seed `0`,
`~crc` in and out). Computed with `crc32_ieee_update()` on-device and a matching
table implementation in the web app. Fingerprints are **internal hashing
inputs** — the bytes below never go on the wire; only the resulting fp does
(inside `Snapshot` and the behavior/layout summaries).

**Truncation (v2).** fps are truncated before they are served, so the `uint32`
fp fields encode as short varints instead of ~5 bytes:

- **Opaque fps** (`behaviors_fp`, `default_keymap_fp`, `keymap_fp`, per-layer
  `fp`/`default_fp`, per-layout physical `fp`, `physical_layout_fp`) are
  truncated to their **low 16 bits** (`fk_fp16()` in `src/fingerprint.c`).
  These only ever gate a cache refetch, so a 16-bit collision is harmless (a
  rare unnecessary refetch, never wrong data).

- **`def_fp`** uses a **device-adaptive width** of 16, 24, or 32 bits — the
  smallest at which *every behavior on this device* has a distinct `def_fp`
  (`fk_def_fp_bits()`), reported in `Snapshot.def_fp_bits`. Because a `def_fp`
  match makes the web resolve a behavior's metadata from its bundle/cache
  *without* fetching, two distinct behaviors sharing a served `def_fp` would
  mis-resolve one of them (a visible-but-non-fatal wrong param label, never a
  crash). Choosing the width for on-device uniqueness makes **device-internal
  `def_fp` collisions impossible**, and almost every board is already unique at
  16 bits, so only the rare colliding board pays the extra byte(s). The web
  masks its bundle/cache `def_fp` keys to `def_fp_bits` (`maskDefFp()`), and
  refuses to resolve a masked key that two *different* bundle definitions
  share (forcing a fetch), so a bundle-internal collision can't mis-resolve
  either. The one residual (tiny) risk is a *custom* behavior colliding, at the
  served width, with a bundled standard behavior that isn't installed on the
  device — the device can't see the bundle to widen against it; at ≥16 bits
  that is ≈ `(#bundled)/2^bits` per such behavior (≈0.04 % at 16 bits).

crc32's low bits are as uniformly distributed as any hash of that width.
Two behaviors with byte-identical alias+display_name+metadata legitimately
share a `def_fp` (they resolve to the same, correct definition) and so never
force a wider width. Reverting to a fixed 32-bit `def_fp` is a
`FK_DEFINITION_VERSION` bump.

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

Only **`def_fp`** is a contract the web reproduces byte-for-byte (it
recomputes it from a `BehaviorDetails` response and compares against its
bundle/per-device cache). Every other fp below — `behaviors_fp`,
`default_keymap_fp`, `keymap_fp`, per-layer `fp`/`default_fp`, per-layout
physical `fp`, `physical_layout_fp` — is an **opaque, device-computed cache
key**: the web only ever compares a freshly reported value against the one it
cached from a previous load (`cached.fp === snapshot.xFp`); it never
reconstructs these from raw data. Their exact byte layout is therefore a
firmware-internal implementation detail and can change (e.g. for
memoization, see `src/fingerprint.c`) without an `FK_DEFINITION_VERSION`
bump, as long as each fp stays a pure, deterministic function of the data it
keys.

The "firmware memoizes it forever" / "memoized" language below refers to the
in-RAM caching in `src/fingerprint.c`, gated by
`CONFIG_ZMK_FAST_KEYMAP_FP_CACHE` (default `y`). With it disabled, every
fingerprint is recomputed on each call instead of cached (saving the cache
RAM) — the reported fp values are byte-identical either way.

The three **mutable** opaque fps (`keymap_fp`, per-layer current `fp`,
`physical_layout_fp`) are additionally produced by a single shared binding
walk (`keymap_fp` + every per-layer current `fp` come from one pass, each
binding fetched once) and, under `CONFIG_ZMK_FAST_KEYMAP_FP_CACHE`, cached
until a `zmk_studio_rpc_notification` (any Studio keymap mutation) or an
active-physical-layout switch invalidates them. This is a pure
computation-sharing/caching change of opaque fps: the VALUES are identical
with the cache on or off, so it needs **no** `FK_DEFINITION_VERSION` bump (see
`DESIGN.md` §4 for the dedup + invalidation design and rationale).

Groups reported in `Snapshot`:

| field | canonicalization |
|-------|------------------|
| `def_fp` (per behavior; the identity + validator; **web-reproduced**) | metadata bytes + `str16 alias` + `str16 display_name`, using the **as-served** strings (alias = first DT node label, else device name; display_name = metadata display name, else device name). Keys the web's standard-behavior bundle and per-device cache. Immutable per behavior (compile-time metadata/alias/display_name) — firmware memoizes it forever, keyed by the behavior device pointer. |
| `behaviors_fp` (**opaque**) | per behavior, ascending `local_id`: `u16le local_id` + `u32le def_fp` (that behavior's already-computed `def_fp`, not its raw preimage bytes). Composing from `def_fp` lets the firmware compute each behavior's metadata preimage at most once and reuse it here; the aggregate itself is memoized forever once local_ids settle (after `settings_load`, which precedes any RPC). |
| `default_keymap_fp` (**opaque**) | per layer (index order): `u8 layer_id` + each binding_idx's canonical binding (stock/default). Depends on the active physical layout under `CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER` (applies its position map) — firmware memoizes it, invalidated on active-layout switch. |
| `keymap_fp` (**opaque**) | `u8 active_layout_index`, then per layer: `u8 layer_id` + `str16 name` + each binding_idx's canonical binding (current). Mutable (keymap edits, layout switches) — produced by the single shared binding walk and, under the FP cache, held until a Studio RPC notification / layout switch invalidates it (byte-identical value either way). |
| `LayerFingerprint.fp` / `.default_fp` (per layer; **opaque**) | that layer's canonical bindings concatenated (current / default); no `layer_id` prefix — the message carries `id` separately. `default_fp` is memoized the same way as `default_keymap_fp`; `fp` (current) is produced by the shared mutable-fp walk/cache alongside `keymap_fp`. |
| per-layout physical `fp` (in `PhysicalLayoutSummary`; **opaque**) | one layout: `str16 name` + `u16le key_count` + per key `i16le width,height,x,y,r,rx,ry` (rotation fields `0` when `CONFIG_ZMK_PHYSICAL_LAYOUT_KEY_ROTATION` is off). The web caches+compares it, never recomputes it. Compile-time constant per layout — memoized forever, keyed by the layout pointer. |
| `physical_layout_fp` (**opaque**) | `u8 active_layout_index`, then per layout that layout's per-layout-`fp`-preimage (name + keys, as above). Includes the mutable active-layout index, so it rides the shared mutable-fp cache alongside `keymap_fp` (cheap: few layouts). |

> Note the two string encodings: alias/display_name (in `def_fp` /
> `behaviors_fp`) use `str16`, but value display-names **inside metadata** use
> `str8`. Keep both sides in sync.

## `FK_DEFINITION_VERSION`

A single integer defined in **both** `src/fingerprint.h` (C) and
`web/src/standardBehaviors.ts` (TS). It gates the compatibility of:

1. `def_fp`'s canonical byte layout (the one fp above the web reproduces), and
2. the web app's bundled standard-behavior metadata (`STANDARD_BEHAVIORS`).

The web client only trusts a bundled definition (skipping a device fetch) when
its `FK_DEFINITION_VERSION` matches and the behavior's `def_fp` matches a
bundle entry (the web indexes `STANDARD_BEHAVIORS` by locally-computed
`def_fp`).

### When to bump it

Bump the constant (in both files, together) whenever you change **any** of:

- `def_fp`'s canonical byte layout (order, widths, which fields hash) or the
  metadata canonicalization it's built from;
- the CRC parameters;
- the meaning/shape of the bundled standard-behavior metadata.

Changing an **opaque** fp's internal composition (`behaviors_fp`,
`default_keymap_fp`, `keymap_fp`, per-layer `fp`/`default_fp`, per-layout
physical `fp`, `physical_layout_fp`) does **not** need a bump — the web never
reproduces those bytes, only compares the resulting `u32` against a
previously cached value.

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
